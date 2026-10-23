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

#include "pb_media_logic.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QList>

namespace paintbrush {

class FrameProvider;

/* Bounded, cancellable media thumbnail generator for the media drawer.
 *
 * Exact-frame pipeline (A10/A11/A12): thumbnails are decoded by the
 * EditDecodeSource (FrameProvider) at their exact timestamps — never by
 * seeking/snapshotting the main playback player. The two-phase
 * seek->tick->snapshot race is gone: each slot is decoded once with a
 * concrete PTS, and the result is published only when the returned PTS is
 * within the configured tolerance of the requested timestamp.
 *
 * Real priority (A10): candidate timestamps are sorted by distance from
 * the anchor (the current viewport) into a priority queue and popped
 * highest-priority first, so the frame the user is looking at fills in
 * before the rest. The previously computed-but-discarded `ordered` list is
 * now actually fed into the queue.
 *
 * Bounded cache (A12): decoded thumbs live in an LRU byte-budget store;
 * entries are evicted least-recently-used by cumulative bytes beyond the
 * cap, so the cache cannot grow without bound.
 *
 * "No UI-thread frame decoding" is honoured: decoding happens on the edit
 * source's own decode thread; the GUI thread only waits for the result,
 * one slot per timer tick so the UI stays responsive between slots. */
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

    /* Start a batch over [0, durationMs) on `provider`, denser near
     * `anchorMs`. `mediaKey` is the media MRL used for cache identity and
     * frame requests. No player interaction of any kind. */
    void startBatch(FrameProvider *provider, const QString &mediaKey,
                    qint64 durationMs, qint64 anchorMs, int maxThumbnails);
    void cancel();
    bool isGenerating() const { return m_timer.isActive(); }
    int count() const { return m_thumbnails.size(); }
    QList<Thumbnail> thumbnails() const { return m_thumbnails; }
    QImage cachedImage(const QString &mediaKey, qint64 timeMs) const;

    /* PTS verification tolerance (ms). Must be set by the controller from
     * the paintbrush-edit-pts-tolerance-ms config before batches run. */
    void setPtsToleranceMs(qint64 toleranceMs) { m_ptsToleranceMs = toleranceMs; }
    qint64 ptsToleranceMs() const { return m_ptsToleranceMs; }

    /* Legacy scrub-interlock API kept for QML/controller compatibility.
     * The service no longer touches the player at all, so these are
     * no-ops apart from pausing generation while the user drags the
     * scrubber (so decodes do not queue behind a busy UI). */
    void suspend();
    void resume();
    bool isSuspended() const { return m_suspended; }
    void noteUserSeek() { /* the playhead is the user's; nothing to do */ }

signals:
    void thumbnailReady(int index, const QImage &image);
    void generationFinished();
    void generationFailed(const QString &reason);

private slots:
    void onTick();

private:
    FrameProvider *m_provider = nullptr;
    QString m_mediaKey;
    qint64 m_ptsToleranceMs = 250;
    QTimer m_timer;
    ThumbnailPriorityQueue m_pending;  /* highest priority (anchor-nearest) first */
    QList<Thumbnail> m_thumbnails;     /* slots, in display order */
    bool m_suspended = false;          /* user scrubbing: timer stopped */
    mutable LruCache<QImage> m_cache;  /* mediaKey|timeMs -> thumbnail, LRU bytes */
};

} // namespace paintbrush

#endif // PB_THUMBNAIL_SERVICE_HPP