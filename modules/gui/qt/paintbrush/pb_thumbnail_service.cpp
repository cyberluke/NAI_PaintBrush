/*****************************************************************************
 * pb_thumbnail_service.cpp : NAI Paintbrush — media thumbnail timeline
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_thumbnail_service.hpp"

#include <QtMath>

namespace paintbrush {

namespace {

constexpr int TICK_MS = 80;      /* one decode request per tick */
constexpr int THUMB_WIDTH = 160; /* thumbnail target width */
constexpr qint64 THUMB_CACHE_BUDGET_BYTES = 32LL * 1024 * 1024;

/* Scale a decoded frame down to the strip height while keeping the aspect
 * ratio. Decoding happens inside the edit frame source; here we only
 * downscale. */
QImage downscale(const QImage &src, int targetWidth)
{
    if (src.isNull() || src.width() <= targetWidth)
        return src;
    const qreal scale = qreal(targetWidth) / qreal(src.width());
    return src.scaled(targetWidth,
                      qMax(1, int(src.height() * scale)),
                      Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

} // namespace

ThumbnailService::ThumbnailService(QObject *parent)
    : QObject(parent)
    , m_cache(THUMB_CACHE_BUDGET_BYTES)
{
    m_timer.setInterval(TICK_MS);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ThumbnailService::onTick);
}

void ThumbnailService::startBatch(FrameProvider *provider,
                                  const QString &mediaKey,
                                  qint64 durationMs, qint64 anchorMs,
                                  int maxThumbnails)
{
    cancel();

    if (!provider || mediaKey.isEmpty() || durationMs <= 0)
    {
        emit generationFailed(QStringLiteral("No playable media for thumbnails."));
        return;
    }

    m_provider = provider;
    m_mediaKey = mediaKey;
    maxThumbnails = qBound(4, maxThumbnails, 48);

    /* adaptive sampling: spread slots across the duration, but concentrate
     * them around the anchor (current viewport) so what the user is looking
     * at fills in first */
    QList<qint64> times;
    const qint64 stride = qMax<qint64>(500, durationMs / maxThumbnails);
    for (qint64 t = 0; t < durationMs; t += stride)
        times.append(qMin(t, durationMs - 1));
    if (times.isEmpty())
        times.append(0);

    /* Real priority (A10): the anchor-first order (previously computed and
     * discarded) is fed into the priority queue, and each tick pops the
     * highest-priority remaining slot. */
    const QList<qint64> ordered = orderedThumbnailTimes(times, anchorMs);
    m_pending = ThumbnailPriorityQueue(ordered, anchorMs);

    /* slots in display order */
    m_thumbnails.clear();
    for (qint64 t : times)
    {
        Thumbnail th;
        th.timeMs = t;
        const QString key = m_mediaKey + QStringLiteral("|") + QString::number(t);
        if (m_cache.contains(key))
        {
            th.image = m_cache.get(key);
            th.pending = false;
        }
        m_thumbnails.append(th);
    }

    /* queue only slots that are not already cached, in priority order */
    for (qint64 t : ordered)
    {
        const QString key = m_mediaKey + QStringLiteral("|") +
                            QString::number(t);
        if (!m_cache.contains(key))
            m_pending.push(t);
    }

    m_suspended = false;
    if (m_pending.isEmpty())
    {
        emit generationFinished();
        return;
    }
    m_timer.start();
    onTick();
}

void ThumbnailService::cancel()
{
    if (m_timer.isActive())
        m_timer.stop();
    m_provider = nullptr;
    m_mediaKey.clear();
    m_pending.clear();
    m_suspended = false;
    /* The slots list is kept so the strip does not flash empty; the next
     * startBatch() replaces it. The playback player is never touched. */
}

void ThumbnailService::suspend()
{
    if (!m_suspended)
    {
        m_suspended = true;
        if (m_timer.isActive())
            m_timer.stop();
    }
}

void ThumbnailService::resume()
{
    if (m_suspended)
    {
        m_suspended = false;
        if (m_provider && !m_pending.isEmpty() && !m_timer.isActive())
        {
            m_timer.start();
            onTick();
        }
    }
}

void ThumbnailService::onTick()
{
    if (m_pending.isEmpty())
    {
        m_timer.stop();
        emit generationFinished();
        return;
    }

    if (m_suspended)
        return; /* user is scrubbing; wait until resume() restarts the timer */

    /* pop the highest-priority (anchor-nearest) slot and decode it via the
     * edit frame source: exact PTS, no player seeks, no vout snapshots */
    const qint64 targetMs = m_pending.popHighest();

    DecodedFrame frame = m_provider
                             ? m_provider->request(FrameRef{m_mediaKey, {},
                                                            targetMs})
                             : DecodedFrame();
    if (frame.image.isNull() || !frame.ptsVerified)
    {
        /* correctness gate (A11): never publish a frame whose actual PTS
         * is not within tolerance of the requested timestamp */
        for (Thumbnail &th : m_thumbnails)
        {
            if (th.timeMs == targetMs)
                th.failed = true;
        }
        return; /* continue with the next slot on the next tick */
    }

    const QImage thumb = downscale(frame.image, THUMB_WIDTH);
    const QString key = m_mediaKey + QStringLiteral("|") +
                        QString::number(targetMs);
    m_cache.insert(key, thumb); /* LRU byte-budget eviction inside */

    for (int i = 0; i < m_thumbnails.size(); ++i)
    {
        Thumbnail &th = m_thumbnails[i];
        if (th.timeMs == targetMs)
        {
            th.image = thumb;
            th.pending = false;
            emit thumbnailReady(i, thumb);
            break;
        }
    }
}

QImage ThumbnailService::cachedImage(const QString &mediaKey,
                                     qint64 timeMs) const
{
    const QString key = mediaKey + QStringLiteral("|") +
                        QString::number(timeMs);
    return m_cache.contains(key) ? m_cache.get(key) : QImage();
}

} // namespace paintbrush