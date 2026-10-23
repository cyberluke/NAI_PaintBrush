/*****************************************************************************
 * pb_tracking_service.hpp : NAI Paintbrush — video object tracking
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_TRACKING_SERVICE_HPP
#define PB_TRACKING_SERVICE_HPP

#include "pb_common.hpp"
#include "pb_media_logic.hpp"

#include <QObject>
#include <QImage>
#include <QVector>
#include <QPointF>
#include <QString>
#include <QMutex>
#include <QWaitCondition>

#include <thread>
#include <deque>
#include <memory>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

#ifdef HAVE_SAM3
/* The sam3.h header defines the complete deleters required by the
 * unique_ptr-based state/tracker handles. */
# include <sam3.h>
#endif

namespace paintbrush {

class FrameProvider;

/* Real temporal object tracking. Uses the SAM3 visual tracker
 * (sam3_tracker_add_instance + sam3_propagate_frame) — a genuine
 * propagation backend, never per-frame independent segmentation.
 *
 * Exact-frame pipeline (A14): every tracked frame is decoded by the
 * EditDecodeSource (FrameProvider) at a concrete PTS — no
 * vlc_player frame stepping, no singleShot(40) vout snapshot guess, no
 * "whatever the vout was showing". Masks are keyed by the ACTUAL decoded
 * PTS of the frame they were propagated on.
 *
 * Two-pass tracking (A13): the run keeps a SeedState {seedFrame,
 * seedPoints, seedMask, pts}. When a TRACK_BOTH run finishes its first
 * pass, it performs a GENUINE second pass: re-AddInstance with the same
 * seed mask/points (the worker's encode hash skips re-encoding the seed
 * frame), the pass restarts at seedPts as frame 0 (walking backward),
 * m_lastMask is reset to the SEED mask, and propagation continues from
 * there — never from "wherever playback stopped". */
class TrackingService : public QObject
{
    Q_OBJECT

public:
    enum Direction {
        TRACK_FORWARD = 0,
        TRACK_BACKWARD = 1,
        TRACK_BOTH = 2
    };
    Q_ENUM(Direction)

    explicit TrackingService(QObject *parent = nullptr);
    ~TrackingService() override;

    void configure(vlc_object_t *obj);
    bool isConfigured() const { return !m_modelPath.isEmpty(); }
    bool isAvailable() const { return m_available; }
    QString unavailableReason() const { return m_unavailableReason; }
    bool isTracking() const { return m_tracking; }
    QString device() const { return m_device; }
    int progressDone() const { return m_pass.done(); }
    int progressTotal() const { return m_pass.total(); }
    QString statusText() const { return m_status; }

    /* Start tracking an object. `provider` is the edit frame source used
     * for every step; `seedPtsMs` is the PTS of `startFrame` (pass-2 frame
     * 0 is re-seeded at this same PTS). startFrame/startMask/posPoints are
     * the seed state the SAM instance is added from — and re-added from on
     * the reverse pass. */
    void startTrack(FrameProvider *provider, const QString &mrl,
                    const QImage &startFrame, const QImage &startMask,
                    const QVector<QPointF> &posPoints,
                    int direction, int maxFrames, qint64 seedPtsMs);
    void cancel();
    void shutdown();
    void pause();
    void resume();
    bool paused() const { return m_paused; }

    /* Refine the tracked object on the current frame with extra points,
     * then continue propagation. */
    void refineCurrentFrame(const QVector<QPointF> &posPoints,
                            const QVector<QPointF> &negPoints);

signals:
    void trackingStarted();
    void progressChanged(int done, int total);
    void statusChanged(const QString &status);
    void keyframeReady(int frameIndex, qint64 timeMs, const QImage &mask,
                       float confidence);
    void trackFinished(bool complete, bool cancelled, const QString &error);
    void errorOccurred(const QString &message);
    void availabilityChanged(bool available, const QString &reason);

    /* emitted from the worker thread; connected to the GUI-thread slots */
    void workerJobDone(const QImage &mask, float confidence, int stepIndex,
                       qint64 ptsMs);
    void workerInstanceReady(int instanceId);
    void workerError(const QString &error);

private slots:
    void stepNext();
    void onWorkerJobDone(const QImage &mask, float confidence, int stepIndex,
                         qint64 ptsMs);
    void onWorkerInstanceReady(int instanceId);
    void onWorkerError(const QString &error);

private:
    /* The immutable seed of a tracking run: the frame, the click prompts,
     * the object mask, and the PTS of the seed frame. The reverse pass
     * re-adds the SAM instance from exactly this state and restarts its
     * walk at seedPtsMs. */
    struct SeedState
    {
        QImage frame;
        QVector<QPointF> points;
        QImage mask;
        qint64 ptsMs = 0;
        QString mrl;
    };

    void beginTracking();
    /* Switch to the genuine second pass: re-AddInstance with the seed
     * mask/points, reset m_lastMask to the SEED mask, restart the walk at
     * seedPts as pass-2 frame 0. */
    void beginReversePass();
    void finishTrack(bool complete, bool cancelled, const QString &error);
    void setAvailable(bool available, const QString &reason);
    void setStatus(const QString &status);

    /* worker thread functions */
    void workerLoop();
    struct WorkerJob
    {
        enum class Type { AddInstance, Propagate, Refine, Reset };
        Type type = Type::Reset;
        QImage frame;
        QVector<QPointF> posPoints;
        QVector<QPointF> negPoints;
        int stepIndex = 0;
        qint64 ptsMs = 0; /* actual PTS of the frame (masks keyed by PTS) */
    };
    void runWorkerJob(const WorkerJob &job);

    vlc_object_t *m_obj = nullptr;
    QString m_modelPath;
    QString m_device;
    QString m_unavailableReason;
    bool m_available = false;

    FrameProvider *m_provider = nullptr;
    bool m_tracking = false;
    bool m_paused = false;
    TrackingPassState m_pass;      /* pure two-pass walk state machine */
    qint64 m_lastPtsMs = 0;        /* actual PTS of the last emitted keyframe */
    int m_nextFrameIndex = 0;      /* global keyframe index (unique across passes) */
    QString m_status;
    qint64 m_stepMs = 40;          /* nominal frame interval (config) */

    SeedState m_seed;
    QImage m_lastMask;
    int m_instanceId = -1;

    /* worker */
    std::thread m_worker;
    QMutex m_mutex;
    QWaitCondition m_cond;
    std::deque<WorkerJob> m_queue;
    bool m_running = false;
    bool m_cancelPending = false;
    bool m_workerIdle = true;

#ifdef HAVE_SAM3
    sam3_tracker_ptr m_tracker;
    sam3_state_ptr m_stateSam;
    std::shared_ptr<sam3_model> m_model;
    QString m_loadedModelPath;
    quint64 m_encodedHash = 0;
#endif
};

} // namespace paintbrush

#endif // PB_TRACKING_SERVICE_HPP