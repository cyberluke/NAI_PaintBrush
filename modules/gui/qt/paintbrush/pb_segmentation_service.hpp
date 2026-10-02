/*****************************************************************************
 * pb_segmentation_service.hpp : NAI Paintbrush — SAM object segmentation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_SEGMENTATION_SERVICE_HPP
#define PB_SEGMENTATION_SERVICE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QImage>
#include <QVector>
#include <QPointF>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <QAtomicInt>

#include <thread>
#include <deque>
#include <memory>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

#ifdef HAVE_SAM3
/* The sam3.h header defines the complete deleters required by the
 * unique_ptr-based state handles; the real header is tiny (declarations
 * only) so including it here is cheap and keeps the member types valid. */
# include <sam3.h>
#endif

namespace paintbrush {

/* Async segmentation service around the vendored SAM3 (sam3.cpp) library.
 *
 * Model loading, image encoding and inference all run on a dedicated worker
 * thread so the Qt UI thread is never blocked. One heavy job runs at a time
 * (requests beyond the single pending slot are dropped with a status
 * message). Availability is explicit: when no model path is configured or
 * the SAM3 backend is not compiled in, the service reports exactly why. */
class SegmentationService : public QObject
{
    Q_OBJECT

public:
    enum State {
        STATE_UNAVAILABLE = 0,
        STATE_IDLE,
        STATE_INITIALIZING,
        STATE_READY,
        STATE_BUSY,
        STATE_FAILED
    };
    Q_ENUM(State)

    explicit SegmentationService(QObject *parent = nullptr);
    ~SegmentationService() override;

    void configure(vlc_object_t *obj);
    bool isAvailable() const { return m_state == STATE_READY || m_state == STATE_BUSY; }
    bool isConfigured() const { return !m_modelPath.isEmpty(); }
    QString unavailableReason() const { return m_unavailableReason; }
    QString modelPath() const { return m_modelPath; }
    QString device() const { return m_device; }
    int state() const { return m_state; }
    bool busy() const { return m_state == STATE_BUSY || m_state == STATE_INITIALIZING; }
    QString lastError() const { return m_lastError; }

    /* Start the worker and load the model (async). */
    void initialize();
    /* Stop the worker and release model resources. */
    void shutdown();
    /* Clear the frame-embedding cache (keeps the loaded model). */
    void clearCache();

    /* Segment the frame with positive/negative points and/or a text prompt.
     * The frame is encoded once; repeated refinement clicks on the same
     * frame reuse the cached embedding. */
    void segment(const QImage &frame, const QVector<QPointF> &posPoints,
                 const QVector<QPointF> &negPoints, const QString &textPrompt);
    void cancel();

signals:
    void stateChanged(int state);
    void availabilityChanged(bool available, const QString &reason);
    void maskReady(const QImage &mask, double score, const QString &method);
    void errorOccurred(const QString &message);
    void progressChanged(const QString &status);
    void busyChanged(bool busy);
    void initialized(bool ok, const QString &error);

private:
    struct Job
    {
        QImage frame;
        QVector<QPointF> posPoints;
        QVector<QPointF> negPoints;
        QString textPrompt;
    };

    void workerLoop();
    void runJob(Job job);
    void setState(State state);
    void setAvailability(bool available, const QString &reason);

    vlc_object_t *m_obj = nullptr;
    QString m_modelPath;
    QString m_device;
    QString m_unavailableReason;
    QString m_lastError;
    State m_state = STATE_UNAVAILABLE;

    std::thread m_worker;
    QMutex m_mutex;
    QWaitCondition m_cond;
    std::deque<Job> m_queue;
    bool m_running = false;
    bool m_cancelPending = false;

#ifdef HAVE_SAM3
    std::shared_ptr<sam3_model> m_model;
    sam3_state_ptr m_stateSam;
    QString m_loadedModelPath;
#endif

    /* cache of the encoded frame (hash of image bytes) */
    quint64 m_encodedHash = 0;
};

} // namespace paintbrush

#endif // PB_SEGMENTATION_SERVICE_HPP