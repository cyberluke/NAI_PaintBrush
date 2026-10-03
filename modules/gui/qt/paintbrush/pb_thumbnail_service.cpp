/*****************************************************************************
 * pb_thumbnail_service.cpp : NAI Paintbrush — media thumbnail timeline
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_thumbnail_service.hpp"

#include <vlc_player.h>

#include <QtMath>

namespace paintbrush {

namespace {

constexpr int TICK_MS = 80;          /* one seek or one snapshot per tick */
constexpr int SNAPSHOT_TIMEOUT_MS = 4000;
constexpr int THUMB_WIDTH = 160;     /* thumbnail target width */

/* Scale a decoded frame down to the strip height while keeping the aspect
 * ratio. Decoding happens inside the vout; here we only downscale. */
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
{
    m_timer.setInterval(TICK_MS);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ThumbnailService::onTick);
}

void ThumbnailService::startBatch(vlc_player_t *player, const QString &mediaKey,
                                  qint64 durationMs, qint64 anchorMs,
                                  int maxThumbnails)
{
    cancel();

    if (!player || mediaKey.isEmpty() || durationMs <= 0)
    {
        emit generationFailed(QStringLiteral("No playable media for thumbnails."));
        return;
    }

    m_player = player;
    m_mediaKey = mediaKey;
    m_durationMs = durationMs;
    m_originalTimeMs = FrameCaptureService::currentTimeMs(player);
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

    /* reorder: anchor first, then alternate outward */
    QList<qint64> ordered;
    {
        qint64 best = times.first();
        qint64 bestDist = INT64_MAX;
        for (qint64 t : times)
        {
            const qint64 d = qAbs(t - anchorMs);
            if (d < bestDist)
            {
                bestDist = d;
                best = t;
            }
        }
        ordered.append(best);
        QList<qint64> left, right;
        for (qint64 t : times)
        {
            if (t == best)
                continue;
            if (t < best)
                left.append(t);
            else
                right.append(t);
        }
        /* farthest first on each side so the viewport fills first */
        std::sort(left.begin(), left.end(), std::greater<qint64>());
        std::sort(right.begin(), right.end());
        int li = 0, ri = 0;
        bool takeLeft = true;
        while (li < left.size() || ri < right.size())
        {
            if (takeLeft && li < left.size())
                ordered.append(left[li++]);
            else if (ri < right.size())
                ordered.append(right[ri++]);
            else if (li < left.size())
                ordered.append(left[li++]);
            takeLeft = !takeLeft;
        }
    }

    /* slots in display order */
    m_thumbnails.clear();
    m_pendingTimes.clear();
    for (qint64 t : times)
    {
        Thumbnail th;
        th.timeMs = t;
        const QString key = m_mediaKey + QStringLiteral("|") + QString::number(t);
        if (m_cache.contains(key))
        {
            th.image = m_cache.value(key);
            th.pending = false;
        }
        m_thumbnails.append(th);
    }

    /* queue only slots that are not already cached */
    for (int i = 0; i < m_thumbnails.size(); ++i)
    {
        const QString key = m_mediaKey + QStringLiteral("|") +
                            QString::number(m_thumbnails[i].timeMs);
        if (!m_cache.contains(key))
            m_pendingTimes.append(m_thumbnails[i].timeMs);
    }

    m_cursor = -1;
    m_suspended = false;
    m_userSought = false;
    if (m_pendingTimes.isEmpty())
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
    {
        m_timer.stop();
        /* restore the player to where it was before the batch started,
         * unless the user has moved the playhead themselves in the meantime */
        if (m_player && m_originalTimeMs > 0 && !m_userSought)
        {
            PlayerLock lock(m_player);
            vlc_player_SetTime(m_player, m_originalTimeMs * 1000);
        }
    }
    m_player = nullptr;
    m_mediaKey.clear();
    m_pendingTimes.clear();
    m_cursor = -1;
    m_seekPhase = false;
    m_suspended = false;
    m_userSought = false;
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
        if (m_player && !m_pendingTimes.isEmpty() && !m_timer.isActive())
        {
            m_timer.start();
            onTick();
        }
    }
}

void ThumbnailService::seekTo(qint64 timeMs)
{
    if (!m_player)
        return;
    PlayerLock lock(m_player);
    vlc_player_SetTime(m_player, timeMs * 1000);
}

QImage ThumbnailService::snapshotNow()
{
    if (!m_player)
        return {};
    return FrameCaptureService::captureCurrentFrame(m_player);
}

void ThumbnailService::onTick()
{
    if (m_pendingTimes.isEmpty())
    {
        m_timer.stop();
        /* restore the original position, unless the user moved the playhead
         * themselves while the batch was running */
        if (m_player && m_originalTimeMs > 0 && !m_userSought)
        {
            PlayerLock lock(m_player);
            vlc_player_SetTime(m_player, m_originalTimeMs * 1000);
        }
        m_player = nullptr;
        emit generationFinished();
        return;
    }

    if (m_suspended)
        return; /* user is scrubbing; wait until resume() restarts the timer */

    if (!m_seekPhase)
    {
        /* phase 1: seek to the next pending time. Seeking while the player
         * is playing is fine: the vout shows the requested frame on the next
         * tick, we snapshot it, and the original position is restored when
         * the batch finishes. */
        m_lastSeekedTime = m_pendingTimes.first();
        m_pendingTimes.removeFirst();
        seekTo(m_lastSeekedTime);
        m_seekPhase = true;
        return; /* wait one tick for the vout to display the frame */
    }

    /* phase 2: snapshot the frame we just seeked to */
    m_seekPhase = false;
    const qint64 lastSeeked = m_lastSeekedTime;

    QImage image = snapshotNow();
    if (image.isNull())
    {
        /* no frame yet — retry the same slot, then mark failed */
        if (m_retryCount++ < 3)
        {
            m_pendingTimes.prepend(lastSeeked);
            m_seekPhase = true;
            return;
        }
        m_retryCount = 0;
        for (Thumbnail &th : m_thumbnails)
        {
            if (th.timeMs == lastSeeked)
                th.failed = true;
        }
        emit generationFailed(QStringLiteral("A thumbnail frame could not be decoded."));
        return;
    }

    m_retryCount = 0;
    const QImage thumb = downscale(image, THUMB_WIDTH);
    const QString key = m_mediaKey + QStringLiteral("|") + QString::number(lastSeeked);
    m_cache.insert(key, thumb);

    for (int i = 0; i < m_thumbnails.size(); ++i)
    {
        Thumbnail &th = m_thumbnails[i];
        if (th.timeMs == lastSeeked)
        {
            th.image = thumb;
            th.pending = false;
            emit thumbnailReady(i, thumb);
            break;
        }
    }
}

QImage ThumbnailService::cachedImage(const QString &mediaKey, qint64 timeMs) const
{
    return m_cache.value(mediaKey + QStringLiteral("|") + QString::number(timeMs));
}

} // namespace paintbrush