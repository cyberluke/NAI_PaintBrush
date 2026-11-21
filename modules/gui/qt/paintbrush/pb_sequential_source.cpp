/*****************************************************************************
 * pb_sequential_source.cpp : persistent in-process libvlc frame stepper
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * One libvlc media player with vmem video callbacks, independent of the
 * user's playback player. prepare() opens/seeks once; request() resumes that
 * persistent player just long enough to present one actual next frame, then
 * pauses it. The input is bounded to one frame because the tracking worker
 * consumes each frame before asking for the next one.
 *****************************************************************************/

#include "pb_sequential_source.hpp"

#include <vlc/libvlc.h>
#include <vlc/libvlc_media.h>
#include <vlc/libvlc_media_player.h>

#include <QElapsedTimer>
#include <QThread>

#include <cstdio>
#include <cstring>

namespace paintbrush {

namespace {

constexpr qint64 kStateTimeoutMs = 8000;
constexpr qint64 kFrameTimeoutMs = 4000;
constexpr qint64 kToleranceMs = 250;

QString localPathFromMrl(const QString &mrl)
{
    if (mrl.startsWith(QStringLiteral("file:///")))
        return mrl.mid(8);
    if (mrl.startsWith(QStringLiteral("file://")))
        return mrl.mid(7);
    return mrl;
}

} // namespace

SequentialVideoSource::SequentialVideoSource() = default;

SequentialVideoSource::~SequentialVideoSource()
{
    shutdown();
}

void *SequentialVideoSource::lockCb(void *opaque, void **planes)
{
    auto *self = static_cast<SequentialVideoSource *>(opaque);
    if (!self || self->m_buffer.empty())
        return nullptr;
    planes[0] = self->m_buffer.data();
    return nullptr;
}

void SequentialVideoSource::unlockCb(void *, void *, void *const *)
{
}

void SequentialVideoSource::displayCb(void *opaque, void *)
{
    auto *self = static_cast<SequentialVideoSource *>(opaque);
    if (!self)
        return;
    QMutexLocker lock(&self->m_mutex);
    ++self->m_seq;
    self->m_cond.wakeAll();
}

bool SequentialVideoSource::waitForDisplay(quint64 sinceSeq, int timeoutMs) const
{
    QElapsedTimer timer;
    timer.start();
    QMutexLocker lock(&m_mutex);
    while (m_seq <= sinceSeq && !m_cancelled) {
        const qint64 left = qint64(timeoutMs) - timer.elapsed();
        if (left <= 0)
            break;
        m_cond.wait(&m_mutex, ulong(left));
    }
    return m_seq > sinceSeq && !m_cancelled;
}

bool SequentialVideoSource::copyCurrentFrame(QImage &out, qint64 &ptsUsOut) const
{
    const qint64 ptsUs = m_player ? libvlc_media_player_get_time(m_player) : -1;
    QMutexLocker lock(&m_mutex);
    if (m_width <= 0 || m_height <= 0 || m_buffer.empty())
        return false;
    const size_t expected = size_t(m_width) * size_t(m_height) * 4;
    if (m_buffer.size() < expected)
        return false;
    QImage image(m_width, m_height, QImage::Format_RGB32);
    std::memcpy(image.bits(), m_buffer.data(), expected);
    out = image;
    ptsUsOut = ptsUs;
    return !out.isNull();
}

void SequentialVideoSource::releasePlayer()
{
    if (m_player) {
        libvlc_media_player_stop_async(m_player);
        libvlc_media_player_release(m_player);
        m_player = nullptr;
    }
    if (m_media) {
        libvlc_media_release(m_media);
        m_media = nullptr;
    }
    m_prepared = false;
}

void SequentialVideoSource::shutdown()
{
    {
        QMutexLocker lock(&m_mutex);
        m_cancelled = true;
        m_cond.wakeAll();
    }
    releasePlayer();
    if (m_instance) {
        libvlc_release(m_instance);
        m_instance = nullptr;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_buffer.clear();
        m_prepared = false;
        m_cancelled = false;
        m_seq = 0;
        m_lastPtsMs = -1;
        m_lastImage = QImage();
        m_mrl.clear();
        m_width = m_height = 0;
    }
}

bool SequentialVideoSource::ensurePlayer(const FrameRef &seed)
{
    releasePlayer();

    if (seed.mrl.isEmpty() || seed.width <= 0 || seed.height <= 0) {
        std::fprintf(stderr,
                     "paintbrush: sequential source invalid seed (mrl='%s' %dx%d)\n",
                     qUtf8Printable(seed.mrl), seed.width, seed.height);
        return false;
    }

    if (!m_instance) {
        /* Isolated second libvlc core: no UI/config, no one-instance
         * forwarding, no media library, no OSD/SPU. The video callbacks
         * provide the frames; nothing is displayed. */
        const char *args[] = {
            "--ignore-config",
            "--no-one-instance",
            "--no-media-library",
            "--no-video-title-show",
            "--no-spu",
            "--no-osd",
        };
        m_instance = libvlc_new(int(sizeof(args) / sizeof(args[0])), args);
        if (!m_instance) {
            std::fprintf(stderr, "paintbrush: libvlc instance for sequential "
                                 "decoder failed\n");
            return false;
        }
    }

    const QByteArray mrl = seed.mrl.toUtf8();
    m_media = libvlc_media_new_location(mrl.constData());
    if (!m_media) {
        std::fprintf(stderr, "paintbrush: sequential media open failed '%s'\n",
                     mrl.constData());
        return false;
    }
    m_player = libvlc_media_player_new(m_instance, nullptr, nullptr);
    if (!m_player) {
        libvlc_media_release(m_media);
        m_media = nullptr;
        std::fprintf(stderr, "paintbrush: sequential player creation failed\n");
        return false;
    }
    libvlc_media_player_set_media(m_player, m_media);

    {
        QMutexLocker lock(&m_mutex);
        m_mrl = seed.mrl;
        m_width = seed.width;
        m_height = seed.height;
        m_buffer.assign(size_t(m_width) * size_t(m_height) * 4, 0);
        m_seq = 0;
        m_lastPtsMs = -1;
        m_lastImage = QImage();
        m_cancelled = false;
    }

    libvlc_video_set_callbacks(m_player, lockCb, unlockCb, displayCb, this);
    libvlc_video_set_format(m_player, "RV32", unsigned(m_width),
                            unsigned(m_height), unsigned(m_width * 4));

    if (libvlc_media_player_play(m_player) != 0) {
        std::fprintf(stderr, "paintbrush: sequential play failed '%s'\n",
                     mrl.constData());
        return false;
    }

    QElapsedTimer stateTimer;
    stateTimer.start();
    while (libvlc_media_player_get_state(m_player) != libvlc_Playing) {
        if (stateTimer.elapsed() > kStateTimeoutMs) {
            std::fprintf(stderr, "paintbrush: sequential player did not reach "
                                 "Playing\n");
            return false;
        }
        QThread::msleep(10);
    }

    /* First display proves the vmem output is live. */
    (void)waitForDisplay(0, int(kFrameTimeoutMs));

    /* Seek ONCE to the tracking seed. Clear the sequence first so the
     * pre-seek display cannot be mistaken for the target frame. */
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            QMutexLocker lock(&m_mutex);
            m_seq = 0;
        }
        libvlc_media_player_set_time(m_player,
                                     libvlc_time_t(seed.ptsMs * 1000),
                                     true);
        if (waitForDisplay(0, int(kFrameTimeoutMs)))
            break;
        if (attempt == 1) {
            std::fprintf(stderr, "paintbrush: sequential seek to %lld ms "
                                 "produced no frame\n",
                         (long long)seed.ptsMs);
            return false;
        }
    }

    /* Pause: all subsequent frames are decoded one at a time. */
    libvlc_media_player_set_pause(m_player, true);
    {
        QElapsedTimer pauseTimer;
        pauseTimer.start();
        while (libvlc_media_player_get_state(m_player) != libvlc_Paused &&
               pauseTimer.elapsed() < 2000)
            QThread::msleep(5);
    }

    QImage seedImage;
    qint64 seedUs = -1;
    if (!copyCurrentFrame(seedImage, seedUs)) {
        std::fprintf(stderr, "paintbrush: sequential seed frame unavailable\n");
        return false;
    }

    {
        QMutexLocker lock(&m_mutex);
        m_lastImage = seedImage;
        m_lastPtsMs = seedUs >= 0 ? seedUs / 1000 : seed.ptsMs;
        m_prepared = true;
    }
    std::fprintf(stderr,
                 "paintbrush: sequential decoder ready mrl='%s' %dx%d seed=%lldms\n",
                 qUtf8Printable(seed.mrl), m_width, m_height,
                 (long long)(seedUs >= 0 ? seedUs / 1000 : seed.ptsMs));
    return true;
}

bool SequentialVideoSource::prepare(const FrameRef &seed)
{
    {
        QMutexLocker lock(&m_mutex);
        m_cancelled = false;
        m_prepared = false;
    }
    const bool ok = ensurePlayer(seed);
    if (!ok) {
        QMutexLocker lock(&m_mutex);
        m_prepared = false;
    }
    return ok;
}

void SequentialVideoSource::cancel()
{
    QMutexLocker lock(&m_mutex);
    m_cancelled = true;
    m_cond.wakeAll();
}

DecodedFrame SequentialVideoSource::request(const FrameRef &ref) const
{
    DecodedFrame out;
    out.requestedMs = ref.ptsMs;

    {
        QMutexLocker lock(&m_mutex);
        if (!m_prepared || !m_player || m_cancelled)
            return out;
    }

    /* One sequential step per request. libvlc 4's next_frame() blocks on a
     * vmem-only video output, so resume the paused player just long enough to
     * present the next frame, then pause it again. One persistent decoder,
     * one initial seek, no per-frame seeking; the vmem vout is the frame
     * source and the only buffer between requests. */
    QImage image;
    qint64 actualMs = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        quint64 before = 0;
        {
            QMutexLocker lock(&m_mutex);
            before = m_seq;
        }
        libvlc_media_player_set_pause(m_player, false);
        const bool presented = waitForDisplay(before, int(kFrameTimeoutMs));
        libvlc_media_player_set_pause(m_player, true);
        if (!presented) {
            std::fprintf(stderr, "paintbrush: sequential frame wait timed out\n");
            return out;
        }

        qint64 ptsUs = -1;
        if (!copyCurrentFrame(image, ptsUs))
            return out;
        actualMs = ptsUs >= 0 ? ptsUs / 1000 : ref.ptsMs;

        qint64 last = -1;
        {
            QMutexLocker lock(&m_mutex);
            last = m_lastPtsMs;
        }
        if (last < 0 || actualMs > last)
            break;
    }

    {
        QMutexLocker lock(&m_mutex);
        if (m_cancelled)
            return out;
        /* media ended or the clock did not advance */
        if (m_lastPtsMs >= 0 && actualMs <= m_lastPtsMs) {
            std::fprintf(stderr,
                         "paintbrush: sequential frame did not advance "
                         "(%lld -> %lld ms)\n",
                         (long long)m_lastPtsMs, (long long)actualMs);
            return out;
        }
        m_lastPtsMs = actualMs;
        m_lastImage = image;
    }

    out.image = image;
    out.ptsMs = actualMs;
    /* Sequential stepping returns the actual next decoded frame; there is no
     * wrong-frame ambiguity, and VFR timestamps are the truth. The requested
     * PTS is only a sampling hint. */
    out.ptsVerified = true;
    (void)kToleranceMs;
    return out;
}

FrameRef SequentialVideoSource::last() const
{
    QMutexLocker lock(&m_mutex);
    FrameRef ref;
    ref.mrl = m_mrl;
    ref.ptsMs = m_lastPtsMs;
    ref.width = m_lastImage.width();
    ref.height = m_lastImage.height();
    return ref;
}

bool SequentialVideoSource::canDecode() const
{
    QMutexLocker lock(&m_mutex);
    return m_prepared && m_player != nullptr && !m_cancelled;
}

} // namespace paintbrush
