/*****************************************************************************
 * pb_tracking_service.cpp : NAI Paintbrush — video object tracking
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_tracking_service.hpp"
#include "pb_frame_capture.hpp"

#include <QFileInfo>
#include <QDebug>
#include <cerrno>

#include <vlc_player.h>
#include <vlc_vout.h>

#ifdef HAVE_SAM3
# include <sam3.h>
#endif

namespace paintbrush {

TrackingService::TrackingService(QObject *parent)
    : QObject(parent)
{
    connect(&m_stepTimer, &QTimer::timeout, this, &TrackingService::onStepTimeout);
    /* worker-thread signals are delivered queued to this GUI-thread object */
    connect(this, &TrackingService::workerJobDone,
            this, &TrackingService::onWorkerJobDone);
    connect(this, &TrackingService::workerInstanceReady,
            this, &TrackingService::onWorkerInstanceReady);
    connect(this, &TrackingService::workerError,
            this, &TrackingService::onWorkerError);
}

TrackingService::~TrackingService()
{
    shutdown();
}

void TrackingService::configure(vlc_object_t *obj)
{
    m_obj = obj;
    m_modelPath = pbConfigString(obj, "paintbrush-sam-model").trimmed();
    m_device = pbConfigString(obj, "paintbrush-sam-device").trimmed();
    if (m_device.isEmpty())
        m_device = QStringLiteral("auto");

    if (m_modelPath.isEmpty())
    {
        setAvailable(false, QStringLiteral("Tracking is unavailable: SAM model is not configured."));
        return;
    }
    if (!QFileInfo::exists(m_modelPath))
    {
        setAvailable(false,
                     QStringLiteral("Tracking is unavailable: SAM model file not found: %1")
                         .arg(m_modelPath));
        return;
    }
#ifndef HAVE_SAM3
    setAvailable(false,
                 QStringLiteral("Tracking is unavailable: this VLC build does not include "
                                "the SAM3 backend."));
#else
    setAvailable(true, {});
#endif
}

void TrackingService::setAvailable(bool available, const QString &reason)
{
    m_unavailableReason = reason;
    m_available = available;
    emit availabilityChanged(available, reason);
}

void TrackingService::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged(status);
}

/* ------------------------------------------------------------- lifecycle */

void TrackingService::shutdown()
{
    cancel();
    resetPlayerInteraction();
    if (!m_running)
        return;
    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_queue.clear();
        m_cond.wakeAll();
    }
    if (m_worker.joinable())
        m_worker.join();
#ifdef HAVE_SAM3
    m_tracker.reset();
    m_stateSam.reset();
    m_model.reset();
    m_loadedModelPath.clear();
#endif
}

void TrackingService::startTrack(vlc_player_t *player, const QImage &startFrame,
                                 const QImage &startMask,
                                 const QVector<QPointF> &posPoints,
                                 int direction, int maxFrames)
{
    if (m_tracking)
        return;
    if (!m_available || !isConfigured())
    {
        emit errorOccurred(m_unavailableReason);
        return;
    }
    if (!player || startFrame.isNull() || startMask.isNull())
    {
        emit errorOccurred(QStringLiteral("Tracking requires a selected object on a video frame."));
        return;
    }

    m_player = player;
    m_startFrame = startFrame;
    m_startMask = startMask;
    m_startPoints = posPoints;
    m_direction = direction;
    m_total = qBound(1, maxFrames, 200);
    m_done = 0;
    m_bothPhase = 0;
    m_stepSign = 1;
    m_instanceId = -1;
    m_lastMask = startMask;
    m_cancelPending = false;

    /* ensure the worker is running */
    if (!m_running)
    {
        m_running = true;
        m_worker = std::thread([this]() { workerLoop(); });
    }

    /* the player must be paused for frame stepping */
    {
        PlayerLock lock(m_player);
        if (vlc_player_CanPause(m_player))
            vlc_player_Pause(m_player);
    }

    beginTracking();
}

void TrackingService::beginTracking()
{
    m_tracking = true;
    emit trackingStarted();
    setStatus(QStringLiteral("Seeding tracker…"));

    /* register for frame-step status callbacks */
    if (!m_listener && m_player)
    {
        vlc_player_cbs cbs;
        memset(&cbs, 0, sizeof(cbs));
        cbs.on_next_frame_status = [](vlc_player_t *, int status, void *data) {
            QMetaObject::invokeMethod(static_cast<TrackingService *>(data),
                                      "onFrameStepStatus", Qt::QueuedConnection,
                                      Q_ARG(int, status));
        };
        PlayerLock lock(m_player);
        m_listener = vlc_player_AddListener(m_player, &cbs, this);
    }

    /* step 0: add the tracked instance from the click prompts */
    {
        QMutexLocker lock(&m_mutex);
        m_queue.push_back({WorkerJob::Type::AddInstance, m_startFrame,
                           m_startPoints, {}, 0});
        m_cond.wakeAll();
    }
}

void TrackingService::stepNext()
{
    if (!m_tracking || m_paused)
        return;

    if (m_done >= m_total)
    {
        /* both directions: reverse and continue */
        if (m_direction == TRACK_BOTH && m_bothPhase == 0)
        {
            m_bothPhase = 1;
            m_done = 0;
            m_stepSign = -1;
        }
        else
        {
            finishTrack(true, false, {});
            return;
        }
    }

    emit progressChanged(m_done, m_total);
    setStatus(m_stepSign > 0 ? QStringLiteral("Tracking forward…")
                             : QStringLiteral("Tracking backward…"));

    requestFrameStep(m_stepSign > 0 ? StepKind::Forward : StepKind::Backward);
}

void TrackingService::requestFrameStep(StepKind kind)
{
    if (!m_player)
    {
        finishTrack(false, false, QStringLiteral("Player is gone."));
        return;
    }
    m_stepTimer.start(3000); /* safety timeout per frame step */
    {
        PlayerLock lock(m_player);
        if (kind == StepKind::Forward)
            vlc_player_NextVideoFrame(m_player);
        else
            vlc_player_PreviousVideoFrame(m_player);
    }
}

void TrackingService::onFrameStepStatus(int status)
{
    if (!m_tracking)
        return;
    if (status != 0)
    {
        if (status == -EAGAIN)
        {
            /* no more frames in this direction */
            if (m_direction == TRACK_BOTH && m_bothPhase == 0)
            {
                m_bothPhase = 1;
                m_done = 0;
                m_stepSign = -1;
                stepNext();
                return;
            }
            finishTrack(true, false, {});
            return;
        }
        finishTrack(false, m_cancelPending,
                    QStringLiteral("Frame step failed (status %1).").arg(status));
        return;
    }

    m_stepTimer.stop();
    captureAndPropagate();
}

void TrackingService::captureAndPropagate()
{
    if (!m_tracking || !m_player)
        return;

    /* small delay so the vout has the new frame before snapshotting */
    QTimer::singleShot(40, this, [this]() {
        if (!m_tracking)
            return;
        const QImage frame = FrameCaptureService::captureCurrentFrame(m_player);
        if (frame.isNull())
        {
            finishTrack(false, false,
                        QStringLiteral("Could not capture the next video frame."));
            return;
        }

        QMutexLocker lock(&m_mutex);
        if (m_cancelPending)
            return;
        m_queue.push_back({WorkerJob::Type::Propagate, frame, {}, {},
                           m_done + 1});
        m_cond.wakeAll();
    });
}

void TrackingService::onWorkerInstanceReady(int instanceId)
{
    if (!m_tracking)
        return;
    m_instanceId = instanceId;
    if (m_instanceId < 0)
    {
        finishTrack(false, false,
                    QStringLiteral("Failed to seed the tracker with the selected object."));
        return;
    }
    /* emit the initial keyframe */
    emit keyframeReady(0, FrameCaptureService::currentTimeMs(m_player), m_lastMask, 1.0f);
    stepNext();
}

void TrackingService::onWorkerJobDone(const QImage &mask, float confidence,
                                      int stepIndex)
{
    if (!m_tracking)
        return;
    m_lastMask = mask;
    m_done = stepIndex;
    emit keyframeReady(m_done, FrameCaptureService::currentTimeMs(m_player),
                       mask, confidence);
    stepNext();
}

void TrackingService::onWorkerError(const QString &error)
{
    if (!m_tracking)
        return;
    emit errorOccurred(error);
    finishTrack(false, false, error);
}

void TrackingService::onStepTimeout()
{
    finishTrack(false, false, QStringLiteral("Timed out waiting for the next video frame."));
}

void TrackingService::refineCurrentFrame(const QVector<QPointF> &posPoints,
                                         const QVector<QPointF> &negPoints)
{
    if (!m_tracking || m_instanceId < 0)
        return;
    QMutexLocker lock(&m_mutex);
    m_queue.push_back({WorkerJob::Type::Refine, {}, posPoints, negPoints, m_done});
    m_cond.wakeAll();
}

void TrackingService::cancel()
{
    m_cancelPending = true;
    resetPlayerInteraction();
    if (m_tracking)
    {
        m_tracking = false;
        m_paused = false;
        emit trackFinished(false, true, {});
    }
    QMutexLocker lock(&m_mutex);
    m_queue.clear();
}

void TrackingService::pause()
{
    if (!m_tracking || m_paused)
        return;
    m_paused = true;
    m_stepTimer.stop();
    setStatus(QStringLiteral("Tracking paused"));
}

void TrackingService::resume()
{
    if (!m_tracking || !m_paused)
        return;
    m_paused = false;
    setStatus(QStringLiteral("Tracking resumed"));
    QTimer::singleShot(0, this, [this]() { stepNext(); });
}

void TrackingService::finishTrack(bool complete, bool cancelled,
                                  const QString &error)
{
    m_tracking = false;
    m_stepTimer.stop();
    resetPlayerInteraction();
    if (!m_cancelPending)
        m_cancelPending = cancelled;
    emit trackFinished(complete, m_cancelPending, error);
}

void TrackingService::resetPlayerInteraction()
{
    if (m_listener && m_player)
    {
        PlayerLock lock(m_player);
        vlc_player_RemoveListener(m_player, m_listener);
        m_listener = nullptr;
    }
    m_player = nullptr;
}

/* --------------------------------------------------------------- worker */

void TrackingService::workerLoop()
{
    for (;;)
    {
        WorkerJob job;
        {
            QMutexLocker lock(&m_mutex);
            while (m_queue.empty() && m_running)
                m_cond.wait(&m_mutex);
            if (!m_running)
                break;
            job = m_queue.front();
            m_queue.pop_front();
        }
        if (m_cancelPending)
            continue;
        runWorkerJob(job);
    }
}

#ifdef HAVE_SAM3

namespace {

sam3_image toSamImage(const QImage &image)
{
    sam3_image out;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    out.width = rgb.width();
    out.height = rgb.height();
    out.channels = 3;
    out.data.assign(rgb.constBits(), rgb.constBits() + rgb.sizeInBytes());
    return out;
}

QImage maskFromSam(const sam3_mask &mask)
{
    if (mask.width <= 0 || mask.height <= 0 || mask.data.empty())
        return {};
    QImage out(mask.width, mask.height, QImage::Format_Grayscale8);
    memcpy(out.bits(), mask.data.data(),
           size_t(mask.width) * size_t(mask.height));
    return out;
}

quint64 fnvImageHash(const QImage &image)
{
    if (image.isNull())
        return 0;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    quint64 hash = 1469598103934665603ULL;
    for (int y = 0; y < rgb.height(); ++y)
    {
        const uchar *line = rgb.constScanLine(y);
        for (int i = 0; i < rgb.bytesPerLine(); ++i)
        {
            hash ^= line[i];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

float maskIoU(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull())
        return 0.f;
    qint64 inter = 0, uni = 0;
    const int h = a.height();
    for (int y = 0; y < h; ++y)
    {
        const uchar *la = a.constScanLine(y);
        const uchar *lb = b.constScanLine(y);
        const int w = a.width();
        for (int x = 0; x < w; ++x)
        {
            const bool fa = la[x] >= 128;
            const bool fb = lb[x] >= 128;
            if (fa || fb)
            {
                uni++;
                if (fa && fb)
                    inter++;
            }
        }
    }
    return uni == 0 ? 0.f : float(inter) / float(uni);
}

} // namespace

void TrackingService::runWorkerJob(const WorkerJob &job)
{
    try
    {
        /* lazily load the model + visual tracker */
        if (!m_model || m_loadedModelPath != m_modelPath)
        {
            sam3_params params;
            params.model_path = m_modelPath.toStdString();
            params.n_threads = static_cast<int>(std::thread::hardware_concurrency());
            params.use_gpu = (m_device != QStringLiteral("cpu"));
            params.encode_img_size = 0;

            auto model = sam3_load_model(params);
            if (!model)
            {
                emit workerError(QStringLiteral("Tracking: failed to load the SAM model."));
                return;
            }
            auto state = sam3_create_state(*model, params);
            if (!state)
            {
                emit workerError(QStringLiteral("Tracking: failed to create SAM state."));
                return;
            }

            sam3_visual_track_params vparams;
            vparams.assoc_iou_threshold = 0.1f;
            vparams.max_keep_alive = 30;
            vparams.recondition_every = 16;
            vparams.fill_hole_area = 16;

            auto tracker = sam3_create_visual_tracker(*model, vparams);
            if (!tracker)
            {
                emit workerError(QStringLiteral("Tracking: failed to create the visual tracker."));
                return;
            }

            m_model = model;
            m_stateSam = std::move(state);
            m_tracker = std::move(tracker);
            m_loadedModelPath = m_modelPath;
            m_encodedHash = 0;
        }

        switch (job.type)
        {
        case WorkerJob::Type::AddInstance:
        {
            /* encode the start frame and add the clicked instance */
            const quint64 hash = fnvImageHash(job.frame);
            if (hash != m_encodedHash)
            {
                const sam3_image img = toSamImage(job.frame);
                if (!sam3_encode_image(*m_stateSam, *m_model, img))
                {
                    emit workerError(QStringLiteral("Tracking: frame encoding failed."));
                    return;
                }
                m_encodedHash = hash;
            }

            sam3_pvs_params pvs;
            pvs.multimask = false;
            for (const QPointF &pt : job.posPoints)
                pvs.pos_points.push_back({float(pt.x()), float(pt.y())});
            for (const QPointF &pt : job.negPoints)
                pvs.neg_points.push_back({float(pt.x()), float(pt.y())});

            const int id = sam3_tracker_add_instance(*m_tracker, *m_stateSam,
                                                     *m_model, pvs);
            emit workerInstanceReady(id);
            break;
        }

        case WorkerJob::Type::Propagate:
        {
            /* NOTE: sam3_propagate_frame() encodes the frame internally
             * ("The image is encoded, then each tracked instance is
             * propagated"). Encoding here first would double the per-frame
             * cost (~50 s extra on CPU), so we let the tracker do it and
             * only keep the hash cache in sync afterwards. */
            const quint64 hash = fnvImageHash(job.frame);
            const sam3_image img = toSamImage(job.frame);
            const sam3_result result = sam3_propagate_frame(*m_tracker, *m_stateSam,
                                                            *m_model, img);
            /* the state now holds this frame's embedding; remember it so a
             * later refine/segment on the same frame skips re-encoding */
            m_encodedHash = hash;

            QImage bestMask;
            float bestScore = 0.f;
            /* Prefer the detection for our instance, but only if it actually
             * overlaps the previous mask. After a scene cut the tracker can
             * emit a same-id detection for a completely different object
             * (mask teleports), which must not win. */
            for (const sam3_detection &det : result.detections)
            {
                if (det.instance_id != m_instanceId)
                    continue;
                const QImage m = maskFromSam(det.mask);
                if (m.isNull())
                    continue;
                const float iou = maskIoU(m, m_lastMask);
                if (iou >= 0.15f)
                {
                    bestMask = m;
                    bestScore = det.iou_score;
                    break;
                }
            }
            if (bestMask.isNull())
            {
                /* fall back to the highest-IoU detection with our mask */
                for (const sam3_detection &det : result.detections)
                {
                    const QImage m = maskFromSam(det.mask);
                    if (m.isNull())
                        continue;
                    const float iou = maskIoU(m, m_lastMask);
                    if (iou > bestScore)
                    {
                        bestScore = iou;
                        bestMask = m;
                    }
                }
            }
            if (bestMask.isNull())
            {
                emit workerError(QStringLiteral("Tracking: object was lost on this frame."));
                return;
            }
            if (bestMask.size() != job.frame.size())
                bestMask = bestMask.scaled(job.frame.size(), Qt::IgnoreAspectRatio,
                                           Qt::FastTransformation);

            emit workerJobDone(bestMask, bestScore, job.stepIndex);
            break;
        }

        case WorkerJob::Type::Refine:
        {
            if (m_instanceId < 0)
                return;
            std::vector<sam3_point> pos, neg;
            for (const QPointF &pt : job.posPoints)
                pos.push_back({float(pt.x()), float(pt.y())});
            for (const QPointF &pt : job.negPoints)
                neg.push_back({float(pt.x()), float(pt.y())});

            if (sam3_refine_instance(*m_tracker, *m_stateSam, *m_model,
                                     m_instanceId, pos, neg))
            {
                /* after refinement, re-run propagation on the current frame
                 * to get the corrected mask */
                /* (the tracker keeps the refined instance; the next frame
                 * propagation will use it) */
            }
            break;
        }

        default:
            break;
        }
    }
    catch (const std::exception &ex)
    {
        emit workerError(QStringLiteral("Tracking failed: %1").arg(
            QString::fromUtf8(ex.what())));
    }
    catch (...)
    {
        emit workerError(QStringLiteral("Tracking failed with an unknown error."));
    }
}

#else /* !HAVE_SAM3 */

void TrackingService::runWorkerJob(const WorkerJob &)
{
    /* unreachable when the backend is not compiled in */
}

#endif

} // namespace paintbrush