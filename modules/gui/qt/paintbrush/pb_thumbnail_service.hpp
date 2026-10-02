/*****************************************************************************
 * pb_thumbnail_service.hpp : NAI Paintbrush — media thumbnail timeline
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_THUMBNAIL_SERVICE_HPP
#define PB_THUMBNAIL_SERVICE_HPP

#include "pb_frame_capture.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QHash>
#include <QList>

struct vlc_player_t;

namespace paintbrush {

/* Bounded, cancellable media thumbnail generator for the media drawer.
 *
 * Thumbnails are produced from the real vout snapshot pipeline (the same
 * FrameCaptureService path), one frame at a time on a timer so the UI never
 * blocks. Generation only runs while the player is paused — while it plays,
 * the generator idles and the strip shows placeholders for pending slots.
 *
 * Behaviour:
 *  - adaptive sampling: at most `maxThumbnails` frames across the duration,
 *    denser near the requested anchor time (the current viewport);
 *  - two-phase per thumbnail: seek to the target time, then snapshot the
 *    freshly displayed frame on the next tick (so the vout has decoded it);
 *  - the player is restored to its original time when the batch finishes or
 *    is cancelled;
 *  - results are cached by media identity + timestamp, so reseeking the same
 *    file does not re-decode frames;
 *  - cancellation clears the pending queue and restores the player.
 *
 * "No UI-thread frame decoding" is honoured: the vout itself decodes on its
 * own pipeline; this service only requests an encoded snapshot (the same
 * operation the single-frame capture uses) one at a time. */
class ThumbnailService : public QObject
{
    Q_OBJECT

public:
    struct Thumbnail {
        qint64 timeMs = 0;
        QImage image;
        bool pending = true;
        bool failed = false;
    };

    explicit ThumbnailService(QObject *parent = nullptr);

    void startBatch(vlc_player_t *player, const QString &mediaKey,
                    qint64 durationMs, qint64 anchorMs, int maxThumbnails);
    void cancel();
    bool isGenerating() const { return m_timer.isActive(); }
    int count() const { return m_thumbnails.size(); }
    QList<Thumbnail> thumbnails() const { return m_thumbnails; }
    QImage cachedImage(const QString &mediaKey, qint64 timeMs) const;

signals:
    void thumbnailReady(int index, const QImage &image);
    void generationFinished();
    void generationFailed(const QString &reason);

private slots:
    void onTick();

private:
    void seekTo(qint64 timeMs);
    QImage snapshotNow();

    vlc_player_t *m_player = nullptr;
    QString m_mediaKey;
    qint64 m_originalTimeMs = 0;
    qint64 m_durationMs = 0;
    QTimer m_timer;
    QList<qint64> m_pendingTimes;      /* queue of timestamps to generate */
    QList<Thumbnail> m_thumbnails;     /* slots, in display order */
    int m_cursor = -1;                 /* current slot being generated */
    bool m_seekPhase = false;          /* false = seek, true = snapshot */
    qint64 m_lastSeekedTime = -1;      /* the timestamp we just seeked to */
    int m_retryCount = 0;              /* snapshot retries for the current slot */
    QHash<QString, QImage> m_cache;    /* mediaKey|timeMs -> thumbnail */
};

} // namespace paintbrush

#endif // PB_THUMBNAIL_SERVICE_HPP