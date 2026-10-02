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

#include <QObject>
#include <QImage>
#include <QVector>
#include <QPointF>
#include <QString>
#include <QTimer>
#include <QMutex>
#include <QWaitCondition>

#include <thread>
#include <deque>
#include <memory>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

struct vlc_player_t;
struct vlc_player_listener_id;

#ifdef HAVE_SAM3
/* The sam3.h header defines the complete deleters required by the
 * unique_ptr-based state/tracker handles. */
# include <sam3.h>
#endif

namespace paintbrush {

/* Real temporal object tracking. Uses the SAM3 visual tracker
 * (sam3_tracker_add_instance + sam3_propagate_frame) — a genuine
 * propagation backend, never per-frame independent segmentation.
 *
 * The player is stepped one frame at a time on the GUI thread (via
 * vlc_player_NextVideoFrame/PreviousVideoFrame with the
 * on_next_frame_status callback), the frame is captured from the vout,
 * and propagation runs on a worker thread so the UI stays responsive. */
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
    int progressDone() const { return m_done; }
    int progressTotal() const { return m_total; }
    QString statusText() const { return m_status; }

    /* Start tracking an object. startFrame is the frame where the object
     * was selected, startMask its mask, posPoints the click prompts used
     * to seed the tracker instance. */
    void startTrack(vlc_player_t *player, const QImage &startFrame,
                    const QImage &startMask, const QVector<QPointF> &posPoints,
                    int direction, int maxFrames);
    void cancel();
    void shutdown();

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
    void workerJobDone(const QImage &mask, float confidence, int stepIndex);
    void workerInstanceReady(int instanceId);
    void workerError(const QString &error);

private slots:
    void stepNext();
    void onWorkerJobDone(const QImage &mask, float confidence, int stepIndex);
    void onWorkerInstanceReady(int instanceId);
    void onWorkerError(const QString &error);
    void onFrameStepStatus(int status);
    void onStepTimeout();

private:
    enum class StepKind { Forward, Backward };

    void beginTracking();
    void requestFrameStep(StepKind kind);
    void captureAndPropagate();
    void finishTrack(bool complete, bool cancelled, const QString &error);
    void resetPlayerInteraction();
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
    };
    void runWorkerJob(const WorkerJob &job);

    vlc_object_t *m_obj = nullptr;
    QString m_modelPath;
    QString m_device;
    QString m_unavailableReason;
    bool m_available = false;

    vlc_player_t *m_player = nullptr;
    vlc_player_listener_id *m_listener = nullptr;
    bool m_tracking = false;
    int m_done = 0;
    int m_total = 0;
    int m_direction = TRACK_FORWARD;
    int m_stepSign = 1;          /* +1 forward, -1 backward */
    int m_bothPhase = 0;         /* 0 = first direction, 1 = reverse */
    QString m_status;
    QTimer m_stepTimer;

    QImage m_startFrame;
    QImage m_startMask;
    QVector<QPointF> m_startPoints;
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