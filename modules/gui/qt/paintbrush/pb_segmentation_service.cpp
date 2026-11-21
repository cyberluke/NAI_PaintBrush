/*****************************************************************************
 * pb_segmentation_service.cpp : NAI Paintbrush — SAM object segmentation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_segmentation_service.hpp"

#include <QFileInfo>
#include <QDebug>

#include <chrono>

#ifdef HAVE_SAM3
# include <sam3.h>
#endif

namespace paintbrush {

/* Cheap FNV-1a hash over the image bytes, used to cache the SAM image
 * embedding so refinement clicks on the same frame do not re-encode. */
static quint64 fnvImageHash(const QImage &image)
{
    if (image.isNull())
        return 0;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    quint64 hash = 1469598103934665603ULL;
    for (int y = 0; y < rgb.height(); ++y)
    {
        const uchar *line = rgb.constScanLine(y);
        const int bytes = rgb.bytesPerLine();
        for (int i = 0; i < bytes; ++i)
        {
            hash ^= line[i];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

SegmentationService::SegmentationService(QObject *parent)
    : QObject(parent)
{
}

SegmentationService::~SegmentationService()
{
    shutdown();
}

void SegmentationService::configure(vlc_object_t *obj)
{
    m_obj = obj;
    m_modelPath = pbConfigString(obj, "paintbrush-sam-model").trimmed();
    m_device = pbConfigString(obj, "paintbrush-sam-device").trimmed();
    pbLogInfo(obj, "segmentation",
              "configured model=%s device=%s",
              qUtf8Printable(m_modelPath), qUtf8Printable(m_device));
    if (m_device.isEmpty())
        m_device = QStringLiteral("auto");

    if (m_modelPath.isEmpty())
    {
        setAvailability(false,
                        QStringLiteral("Object Select is unavailable: SAM model is not configured."));
        return;
    }
    if (!QFileInfo::exists(m_modelPath))
    {
        setAvailability(false,
                        QStringLiteral("Object Select is unavailable: SAM model file not found: %1")
                            .arg(m_modelPath));
        return;
    }

#ifndef HAVE_SAM3
    setAvailability(false,
                    QStringLiteral("Object Select is unavailable: this VLC build does not "
                                   "include the SAM3 backend."));
#else
    setAvailability(true, {});
#endif
}

void SegmentationService::setAvailability(bool available, const QString &reason)
{
    m_unavailableReason = reason;
    if (!available)
        setState(STATE_UNAVAILABLE);
    emit availabilityChanged(available, reason);
}

void SegmentationService::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(int(state));
    if (state == STATE_BUSY || state == STATE_INITIALIZING)
        emit busyChanged(true);
    else
        emit busyChanged(false);
}

void SegmentationService::initialize()
{
    if (!isConfigured() || m_running)
        return;

    m_running = true;
    m_worker = std::thread([this]() { workerLoop(); });
    pbLogInfo(m_obj, "segmentation", "worker started");
}

void SegmentationService::clearCache()
{
    QMutexLocker lock(&m_mutex);
    m_encodedHash = 0;
    /* drop any queued jobs that would re-use the stale embedding */
    m_queue.clear();
}

void SegmentationService::shutdown()
{
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
    m_stateSam.reset();
    m_model.reset();
    m_loadedModelPath.clear();
#endif
}

void SegmentationService::segment(const QImage &frame,
                                  const QVector<QPointF> &posPoints,
                                  const QVector<QPointF> &negPoints,
                                  const QString &textPrompt)
{
    if (frame.isNull())
        return;

    QMutexLocker lock(&m_mutex);
    if (!m_running)
        return;

    /* one heavy local inference job at a time: keep at most one pending */
    if (m_queue.size() >= 1)
        m_queue.pop_front();
    m_queue.push_back({frame, posPoints, negPoints, textPrompt});
    pbLogInfo(m_obj, "segmentation",
              "segment: queued %dx%d pos=%d neg=%d (pending=%d)",
              frame.width(), frame.height(), int(posPoints.size()),
              int(negPoints.size()), int(m_queue.size()));
    m_cond.wakeAll();
}

void SegmentationService::cancel()
{
    QMutexLocker lock(&m_mutex);
    m_cancelPending = true;
}

void SegmentationService::workerLoop()
{
    /* Eager, off-GUI-thread preload: the model is ready before the first
     * click. Failure is non-fatal (each job retries) and already reported. */
    ensureModelLoaded();
    for (;;)
    {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (m_queue.empty() && m_running)
                m_cond.wait(&m_mutex);
            if (!m_running)
                break;
            job = m_queue.front();
            m_queue.pop_front();
        }
        runJob(job);
    }
}

#ifdef HAVE_SAM3

namespace {

/* Convert a QImage to a sam3_image (RGB). */
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

/* Convert a sam3_mask to a grayscale QImage. */
QImage maskFromSam(const sam3_mask &mask)
{
    if (mask.width <= 0 || mask.height <= 0 || mask.data.empty())
        return {};
    QImage out(mask.width, mask.height, QImage::Format_Grayscale8);
    /* mask.data is 0/255 binary, packed row-major with NO padding. QImage rows
     * ARE 4-byte aligned (bytesPerLine >= width), so a single flat memcpy
     * shears every row once the width is not a multiple of 4 — which is what
     * produced diagonal stripes after the encode width became 338/340. Copy
     * row by row into the image's own stride instead. */
    for (int y = 0; y < mask.height; ++y)
    {
        memcpy(out.scanLine(y),
               mask.data.data() + size_t(y) * size_t(mask.width),
               size_t(mask.width));
    }
    return out;
}

} // namespace

/* Load the SAM model/state if not already loaded for the current path.
 * Idempotent and cheap when cached. Runs on the WORKER thread only (called
 * from workerLoop's prologue and from runJob), never on the GUI thread.
 * Returns false when the model or its inference state could not be built;
 * availability and error signals are published here so both callers behave
 * identically. */
bool SegmentationService::ensureModelLoaded()
{
    if (m_model && m_loadedModelPath == m_modelPath)
        return true;

    setState(STATE_INITIALIZING);
    emit progressChanged(QStringLiteral("Loading SAM model…"));

    sam3_params params;
    params.model_path = m_modelPath.toStdString();
    params.n_threads = static_cast<int>(std::thread::hardware_concurrency());
    params.use_gpu = (m_device != QStringLiteral("cpu"));
    params.encode_img_size = 0;

    /* The load is a single opaque C call with no progress callback, and it is
     * the longest phase of the first job (706 MB). Without these markers a
     * stalled load is indistinguishable from a crash. */
    pbLogInfo(m_obj, "segmentation",
              "model: load START '%s' (%lld bytes, device=%s, threads=%d)",
              m_modelPath.toUtf8().constData(),
              (long long)QFileInfo(m_modelPath).size(),
              m_device.toUtf8().constData(),
              params.n_threads);

    auto model = sam3_load_model(params);
    pbLogInfo(m_obj, "segmentation",
              "model: load %s", model ? "DONE" : "FAILED");
    if (!model)
    {
        m_lastError = QStringLiteral("model load failed");
        setAvailability(false,
                        QStringLiteral("Object Select is unavailable: failed to load "
                                       "the SAM model from %1.").arg(m_modelPath));
        setState(STATE_FAILED);
        emit errorOccurred(QStringLiteral("SAM model load failed"));
        emit initialized(false, m_lastError);
        return false;
    }

    auto state = sam3_create_state(*model, params);
    if (!state)
    {
        m_lastError = QStringLiteral("state creation failed");
        setAvailability(false,
                        QStringLiteral("Object Select is unavailable: failed to create "
                                       "SAM inference state."));
        setState(STATE_FAILED);
        emit errorOccurred(QStringLiteral("SAM state creation failed"));
        emit initialized(false, m_lastError);
        return false;
    }

    m_model = model;
    m_stateSam = std::move(state);
    m_loadedModelPath = m_modelPath;
    emit initialized(true, {});
    return true;
}

void SegmentationService::runJob(Job job)
{
    if (m_cancelPending)
    {
        m_cancelPending = false;
        return;
    }

    setState(STATE_BUSY);
    emit progressChanged(QStringLiteral("Segmenting…"));

    try
    {
        /* Model loading is idempotent and is also called eagerly from the
         * worker prologue, so the first click never pays the 706 MB load. */
        if (!ensureModelLoaded())
            return;

        /* Back to BUSY: ensureModelLoaded() may have moved the state to
         * INITIALIZING (fresh load) or left it untouched (cached). */
        setState(STATE_BUSY);
        emit progressChanged(QStringLiteral("Segmenting…"));

        /* Encode at a BOUNDED resolution. SAM's visual encoder cost scales
         * with pixel count, and a CPU ViT encode of a 570x1080 frame takes
         * minutes — the request looked like a silent hang. The mask is
         * upscaled back to the frame size below, and prompt points are
         * mapped into the encoded space, so accuracy is retained. */
        constexpr int kMaxEncodeSide = 640;
        QImage encodeFrame = job.frame;
        qreal encodeScale = 1.0;
        if (qMax(job.frame.width(), job.frame.height()) > kMaxEncodeSide)
        {
            encodeFrame = job.frame.scaled(kMaxEncodeSide, kMaxEncodeSide,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
            encodeScale = qreal(encodeFrame.width()) / qreal(job.frame.width());
        }

        /* encode the frame once; reuse the cached embedding on refinement */
        const quint64 hash = fnvImageHash(encodeFrame);
        if (hash != m_encodedHash)
        {
            emit progressChanged(QStringLiteral("Encoding frame…"));
            pbLogInfo(m_obj, "segmentation",
                      "segment: encode START %dx%d (frame %dx%d, scale %.3f)",
                      encodeFrame.width(), encodeFrame.height(),
                      job.frame.width(), job.frame.height(),
                      double(encodeScale));
            const sam3_image img = toSamImage(encodeFrame);
            if (!sam3_encode_image(*m_stateSam, *m_model, img))
            {
                pbLogWarn(m_obj, "segmentation", "segment: encode FAILED");
                emit errorOccurred(QStringLiteral("SAM frame encoding failed"));
                setState(STATE_READY);
                return;
            }
            pbLogInfo(m_obj, "segmentation", "segment: encode DONE");
            m_encodedHash = hash;
        }
        else
        {
            pbLogInfo(m_obj, "segmentation",
                      "segment: encode reused (cached embedding)");
        }

        sam3_result result;
        QString method;

        /* P0.12: measure the actual inference latency (model load and frame
         * encoding are tracked separately by their own progress states). */
        const auto inferStart = std::chrono::steady_clock::now();

        if (!job.textPrompt.isEmpty())
        {
            /* text-driven segmentation (PCS path) */
            sam3_pcs_params params;
            params.text_prompt = job.textPrompt.toStdString();
            params.score_threshold = 0.5f;
            result = sam3_segment_pcs(*m_stateSam, *m_model, params);
            method = QStringLiteral("text");
        }
        else
        {
            /* point-driven segmentation (PVS path) */
            sam3_pvs_params params;
            params.multimask = true;
            for (const QPointF &pt : job.posPoints)
                params.pos_points.push_back({float(pt.x() * encodeScale),
                                             float(pt.y() * encodeScale)});
            for (const QPointF &pt : job.negPoints)
                params.neg_points.push_back({float(pt.x() * encodeScale),
                                             float(pt.y() * encodeScale)});
            result = sam3_segment_pvs(*m_stateSam, *m_model, params);
            method = QStringLiteral("points");
        }

        m_lastInferenceLatencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - inferStart)
                                       .count();

        if (result.detections.empty())
        {
            emit errorOccurred(QStringLiteral("No object detected at the selected location."));
            setState(STATE_READY);
            return;
        }

        /* pick the detection with the best score */
        const sam3_detection *best = &result.detections.front();
        for (const sam3_detection &det : result.detections)
        {
            if (det.score > best->score)
                best = &det;
        }

        if (best->mask.data.empty())
        {
            emit errorOccurred(QStringLiteral("Segmentation produced an empty mask."));
            setState(STATE_READY);
            return;
        }

        QImage mask = maskFromSam(best->mask);
        if (!mask.isNull() && mask.size() != job.frame.size())
            mask = mask.scaled(job.frame.size(), Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);

        setState(STATE_READY);
        emit maskReady(mask, best->score, method);
    }
    catch (const std::exception &ex)
    {
        m_lastError = QString::fromUtf8(ex.what());
        emit errorOccurred(QStringLiteral("SAM segmentation failed: %1").arg(m_lastError));
        setState(STATE_FAILED);
    }
    catch (...)
    {
        m_lastError = QStringLiteral("unknown error");
        emit errorOccurred(QStringLiteral("SAM segmentation failed with an unknown error."));
        setState(STATE_FAILED);
    }
}

#else /* !HAVE_SAM3 */

void SegmentationService::runJob(Job)
{
    /* unreachable when the backend is not compiled in */
}

#endif

} // namespace paintbrush