/*****************************************************************************
 * pb_matting_service.cpp : NAI Paintbrush — asynchronous matte refinement
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_matting_service.hpp"
#include "gpu/pb_matting.h"

#include <vlc_configuration.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QThread>

#include <cstdarg>
#include <cstdio>

namespace paintbrush {

namespace {

constexpr int kMaxCacheEntries = 6;

QString defaultModelDir()
{
    const QByteArray env = qgetenv("PB_MATTING_DIR");
    if (!env.isEmpty())
        return QDir(QString::fromUtf8(env)).filePath(QStringLiteral("models"));
    const QString app = QCoreApplication::applicationDirPath();
    if (!app.isEmpty())
        return QDir(app).filePath(QStringLiteral("matting/models"));
    return QStringLiteral("matting/models");
}

} // namespace

MattingService::MattingService(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<paintbrush::matte::Result>("paintbrush::matte::Result");
    qRegisterMetaType<paintbrush::matte::FrameIdentity>("paintbrush::matte::FrameIdentity");
    m_modelDir = defaultModelDir();
    setStatus(QStringLiteral("Neural matting is idle."));
}

MattingService::~MattingService()
{
    shutdown();
}

void MattingService::configure(vlc_object_t *obj)
{
    m_obj = obj;
    const QString configured = obj
        ? pbConfigString(obj, "paintbrush-matting-model-dir").trimmed()
        : QString();
    if (!configured.isEmpty())
        m_modelDir = configured;
    else
        m_modelDir = defaultModelDir();

    const QString device = obj
        ? pbConfigString(obj, "paintbrush-matting-device").trimmed().toLower()
        : QString();
    m_deviceTier = device == QLatin1String("gpu") ? 1
                 : device == QLatin1String("cpu") ? 2
                                                  : 0;

    const int preset = obj ? pbConfigInt(obj, "paintbrush-matting-preset") : 1;
    m_quality = preset <= 0 ? matte::Quality::Preview
              : preset >= 2 ? matte::Quality::Maximum
                            : matte::Quality::Balanced;

    /* Advanced controls persist as 0..100 integers. */
    if (obj)
    {
        const int boundary = pbConfigInt(obj, "paintbrush-matting-boundary");
        const int detail = pbConfigInt(obj, "paintbrush-matting-detail");
        const int artifacts = pbConfigInt(obj, "paintbrush-matting-artifacts");
        const int holes = pbConfigInt(obj, "paintbrush-matting-holes");
        const int decontam = pbConfigInt(obj, "paintbrush-matting-decontamination");
        const int temporal = pbConfigInt(obj, "paintbrush-matting-temporal");
        if (boundary > 0)
        {
            m_parameters.innerBandPx = qBound(1, boundary, 24);
            m_parameters.outerBandPx = qBound(m_parameters.innerBandPx + 1,
                                              boundary * 2, 48);
        }
        if (detail > 0)
            m_parameters.detailRecovery = qBound(0.0, detail / 100.0, 1.0);
        if (artifacts > 0)
            m_parameters.artifactSuppression = qBound(0.0, artifacts / 100.0, 1.0);
        if (holes > 0)
            m_parameters.holeRecovery = qBound(0.0, holes / 100.0, 1.0);
        if (decontam >= 0)
            m_parameters.decontamination = qBound(0.0, decontam / 100.0, 1.0);
        if (temporal >= 0)
            m_parameters.temporalStability = qBound(0.0, temporal / 100.0, 1.0);
    }

    m_mutex.lock();
    if (!m_running)
    {
        m_running = true;
        m_worker = std::thread(&MattingService::workerLoop, this);
    }
    m_mutex.unlock();

    /* Probe only files/providers; no model load at startup. */
    const matting::Capabilities caps = matting::Runtime::probe();
    m_neuralAvailable = caps.runtimePresent && (caps.cudaAvailable || caps.cpuAvailable);
    m_neuralProbed = true;
    m_backendText = caps.backend;
    m_unavailableReason = m_neuralAvailable ? QString() : caps.detail;
    if (!m_neuralAvailable && m_unavailableReason.isEmpty())
        m_unavailableReason = QStringLiteral("neural matting runtime is not installed");
    emit availabilityChanged();
}

bool MattingService::neuralAvailable() const
{
    return m_neuralAvailable;
}

QString MattingService::unavailableReason() const
{
    return m_unavailableReason;
}

QString MattingService::backendText() const
{
    if (!m_backendText.isEmpty())
        return m_backendText;
    return neuralAvailable() ? QStringLiteral("neural runtime ready")
                             : unavailableReason();
}

void MattingService::setMode(matte::Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    emit statusChanged(m_status);
}

void MattingService::setQuality(matte::Quality quality)
{
    if (m_quality == quality)
        return;
    m_quality = quality;
    if (m_obj)
        config_PutInt("paintbrush-matting-preset", int(quality));
}

matte::Parameters MattingService::parameters() const
{
    return m_parameters;
}

void MattingService::setParameters(const matte::Parameters &parameters)
{
    m_parameters = parameters;
}

void MattingService::setModelDir(const QString &dir)
{
    const QString trimmed = dir.trimmed();
    const QString resolved = trimmed.isEmpty() ? defaultModelDir() : trimmed;
    if (resolved == m_modelDir)
        return;
    m_modelDir = resolved;
    m_neuralProbed = false;
    matting::sharedRuntime().unload();
    if (m_obj)
        config_PutPsz("paintbrush-matting-model-dir",
                      m_modelDir.toUtf8().constData());
    const matting::Capabilities caps = matting::Runtime::probe();
    m_neuralAvailable = caps.runtimePresent && (caps.cudaAvailable || caps.cpuAvailable);
    m_backendText = caps.backend;
    m_unavailableReason = m_neuralAvailable ? QString() : caps.detail;
    emit availabilityChanged();
}

void MattingService::setDeviceTier(int tier)
{
    tier = qBound(0, tier, 2);
    if (tier == m_deviceTier)
        return;
    m_deviceTier = tier;
    matting::sharedRuntime().unload();
    if (m_obj)
    {
        const char *name = tier == 1 ? "gpu" : (tier == 2 ? "cpu" : "auto");
        config_PutPsz("paintbrush-matting-device", name);
    }
    emit availabilityChanged();
}

int MattingService::presetSizeFor(matte::Quality quality) const
{
    /* Fixed-shape graphs exported by scripts/pb-export-matting-model.py.
     * Maximum uses the 1024 graph on a tighter subject ROI (native-resolution
     * pixels are mapped back losslessly); a larger export can be added to the
     * manifest without changing this code. */
    return quality == matte::Quality::Preview ? 512 : 1024;
}

QString MattingService::modelPathFor(int preset) const
{
    return QDir(m_modelDir).filePath(
        QStringLiteral("vitmatte-s-%1.onnx").arg(preset));
}

bool MattingService::ensureRuntime(QString *error)
{
    const int preset = presetSizeFor(m_quality);
    matting::Runtime &runtime = matting::sharedRuntime();
    if (runtime.isLoaded())
    {
        const matting::RunInfo info = runtime.backendInfo();
        if (info.inputSize == preset)
            return true;
        runtime.unload();
    }

    const matting::Capabilities caps = matting::Runtime::probe();
    if (!caps.runtimePresent)
    {
        if (error)
            *error = caps.detail.isEmpty()
                         ? QStringLiteral("neural runtime is not installed")
                         : caps.detail;
        return false;
    }
    if (m_deviceTier == 1 && !caps.cudaAvailable)
    {
        if (error)
            *error = QStringLiteral("GPU matting was requested but CUDA is not available");
        return false;
    }
    if (m_deviceTier != 2 && !caps.cudaAvailable && !caps.cpuAvailable)
    {
        if (error)
            *error = QStringLiteral("no ONNX Runtime execution provider is available");
        return false;
    }

    matting::SessionConfig config;
    config.modelPath = modelPathFor(preset);
    config.manifestPath = QDir(m_modelDir).filePath(QStringLiteral("manifest.json"));
    config.presetSize = preset;
    if (m_deviceTier == 1)
    {
        config.device = 1;               /* GPU required, never silent CPU   */
        config.requireGpuOperators = true;
    }
    else if (m_deviceTier == 2)
    {
        config.device = 2;               /* CPU explicitly selected          */
        config.requireGpuOperators = false;
    }
    else
    {
        /* AUTO: prefer CUDA but allow the DLL's reported CPU fallback. */
        config.device = 0;
        config.requireGpuOperators = caps.cudaAvailable;
    }

    if (qEnvironmentVariableIsSet("PB_MATTING_PROFILE"))
    {
        const QString profileDir = QDir(m_modelDir).filePath(QStringLiteral("profiles"));
        QDir().mkpath(profileDir);
        config.profileDir = profileDir;
    }

    QString loadError;
    if (!runtime.load(config, &loadError))
    {
        if (error)
            *error = loadError;
        m_unavailableReason = loadError;
        return false;
    }
    m_backendText = runtime.backend();
    m_neuralAvailable = true;
    m_unavailableReason.clear();
    return true;
}

quint64 MattingService::request(const matte::Inputs &inputs,
                                const matte::FrameIdentity &identity,
                                bool diagnostics)
{
    if (inputs.rgb.isNull() || inputs.mask.isNull())
        return 0;

    Job job;
    job.inputs = inputs;
    job.identity = identity;
    job.mode = m_mode;
    job.quality = m_quality;
    job.parameters = m_parameters;
    job.diagnostics = diagnostics;

    const QString key = identity.cacheKey(job.parameters,
                                          QStringLiteral("preset-%1-%2")
                                              .arg(presetSizeFor(job.quality))
                                              .arg(int(job.mode)));
    {
        QMutexLocker lock(&m_cacheMutex);
        if (!diagnostics && m_cache.contains(key))
            return 0; /* caller already has it through cached() */
    }

    QMutexLocker lock(&m_mutex);
    if (!m_running)
        return 0;

    job.id = m_nextId++;
    m_latestId = job.id;
    m_queue.clear();          /* coalesce: only the newest interactive job   */
    m_queue.push_back(job);
    m_cond.wakeAll();

    if (!m_busy)
    {
        m_busy = true;
        emit busyChanged(true);
    }
    return job.id;
}

void MattingService::cancel(quint64 requestId)
{
    m_cancelRequested.store(true);
    {
        QMutexLocker lock(&m_mutex);
        if (m_activeId == requestId || requestId == 0)
        {
            m_latestId = 0;
            m_queue.clear();
        }
    }
    matting::sharedRuntime().cancel();
}

void MattingService::cancelAll()
{
    cancel(0);
}

void MattingService::shutdown()
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_running)
            return;
        m_running = false;
        m_queue.clear();
        m_cond.wakeAll();
    }
    if (m_worker.joinable())
        m_worker.join();
    matting::sharedRuntime().unload();
}

bool MattingService::cached(const matte::FrameIdentity &identity,
                            matte::Result *result) const
{
    if (!result || !identity.isValid())
        return false;
    const QString key = identity.cacheKey(m_parameters,
                                          QStringLiteral("preset-%1-%2")
                                              .arg(presetSizeFor(m_quality))
                                              .arg(int(m_mode)));
    QMutexLocker lock(&m_cacheMutex);
    const auto it = m_cache.constFind(key);
    if (it == m_cache.constEnd())
        return false;
    *result = it.value();
    return true;
}

void MattingService::workerLoop()
{
    for (;;)
    {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (m_running && m_queue.empty())
                m_cond.wait(&m_mutex);
            if (!m_running)
                break;
            job = m_queue.front();
            m_queue.pop_front();
            m_activeId = job.id;
            m_cancelRequested.store(false);
        }
        runJob(job);
        {
            QMutexLocker lock(&m_mutex);
            m_activeId = 0;
        }
    }
}

void MattingService::runJob(const Job &job)
{
    const bool superseded = [&]() {
        QMutexLocker lock(&m_mutex);
        return m_latestId != job.id;
    }();
    if (superseded || m_cancelRequested.load())
        return;

    setStatus(job.mode == matte::Mode::Neural
                  ? QStringLiteral("Refining matte (neural)…")
                  : QStringLiteral("Refining matte…"));

    matte::Result result = matte::refineDeterministic(job.inputs, job.parameters);
    if (!result.isValid())
    {
        emit refineFailed(job.id, QStringLiteral("The selected mask produced no matte."));
        setBusy(false);
        setStatus(QStringLiteral("Matte refinement failed: empty mask."));
        return;
    }

    bool neuralFailed = false;
    QString neuralError;

    if (job.mode == matte::Mode::Neural)
    {
        if (m_cancelRequested.load())
        {
            emit refineFailed(job.id, QStringLiteral("cancelled"));
            setBusy(false);
            return;
        }
        QString error;
        if (!ensureRuntime(&error))
        {
            neuralFailed = true;
            neuralError = error;
        }
        else
        {
            const QRect roi = subjectRoi(job.inputs.mask, 22,
                                         job.parameters.maxBandFor(
                                             std::min(job.inputs.rgb.width(),
                                                      job.inputs.rgb.height())) * 2);
            matte::AlphaPlane neuralAlpha;
            matting::RunInfo info;
            QString refineError;
            if (!matting::sharedRuntime().refine(job.inputs.rgb, result.trimap,
                                                 roi, &neuralAlpha, &info,
                                                 &refineError))
            {
                neuralFailed = true;
                neuralError = refineError;
            }
            else
            {
                const QString backend = QStringLiteral("%1 / %2 px%3")
                    .arg(info.backend.isEmpty() ? QStringLiteral("neural") : info.backend)
                    .arg(info.inputSize)
                    .arg(info.usedGpu ? QStringLiteral(" / GPU") : QStringLiteral(" / CPU"));
                matte::Inputs full = job.inputs;
                if (neuralAlpha.size() != result.alpha.size())
                {
                    /* ROI-sized alpha: paste it into the frame-sized plane. */
                    matte::AlphaPlane frame(result.alpha.width(), result.alpha.height());
                    for (int y = 0; y < frame.height(); ++y)
                        for (int x = 0; x < frame.width(); ++x)
                            frame.setAt(x, y, result.alpha.at(x, y));
                    for (int y = 0; y < neuralAlpha.height(); ++y)
                        for (int x = 0; x < neuralAlpha.width(); ++x)
                            frame.setAt(roi.x() + x, roi.y() + y,
                                        neuralAlpha.at(x, y));
                    neuralAlpha = frame;
                }
                result = matte::applyNeuralAlpha(full, result, neuralAlpha,
                                                 job.parameters, backend,
                                                 QStringLiteral("vitmatte-s"),
                                                 info.inferenceMs);
                result.inferenceMs = info.inferenceMs;
            }
        }
    }

    if (neuralFailed)
    {
        /* Honest fallback: the result is the deterministic refined matte and
         * the backend says exactly why it is not neural. */
        result.backend = QStringLiteral("refined (neural unavailable: %1)").arg(neuralError);
        emit refineFailed(job.id, neuralError);
        logInfo("neural refinement failed: %s", neuralError.toUtf8().constData());
    }

    if (m_cancelRequested.load())
    {
        emit refineFailed(job.id, QStringLiteral("cancelled"));
        setBusy(false);
        return;
    }

    {
        QMutexLocker lock(&m_mutex);
        if (m_latestId != job.id)
            return; /* a newer request superseded this result */
    }

    if (!job.diagnostics)
    {
        const QString key = job.identity.cacheKey(
            job.parameters,
            QStringLiteral("preset-%1-%2").arg(presetSizeFor(job.quality)).arg(int(job.mode)));
        QMutexLocker lock(&m_cacheMutex);
        m_cache.insert(key, result);
        while (m_cache.size() > kMaxCacheEntries)
        {
            /* QHash has no order; drop an arbitrary old entry. Interactive
             * requests are few per second and the identity key prevents a
             * wrong hit. */
            auto it = m_cache.begin();
            if (it.key() == key && m_cache.size() > 1)
                ++it;
            m_cache.erase(it);
        }
    }

    const QString status = result.backend.startsWith(QStringLiteral("CUDA"))
        ? QStringLiteral("Neural matte ready — %1 (%2 ms)")
              .arg(result.backend)
              .arg(result.inferenceMs, 0, 'f', 1)
        : QStringLiteral("Matte ready — %1").arg(result.backend);
    setStatus(status);
    logInfo("matte ready: %s unknown=%.3f holesFilled=%d artifactsRemoved=%d",
            result.backend.toUtf8().constData(),
            result.trimapUnknownFraction,
            result.topology.holesFilled,
            result.topology.detachedRemoved);

    emit matteReady(result, job.identity, job.id);
    setBusy(false);
}

void MattingService::setStatus(const QString &status)
{
    {
        QMutexLocker lock(&m_mutex);
        m_status = status;
    }
    emit statusChanged(status);
}

bool MattingService::isBusy() const
{
    QMutexLocker lock(&m_mutex);
    return m_busy;
}

QString MattingService::statusText() const
{
    QMutexLocker lock(&m_mutex);
    return m_status;
}

void MattingService::setBusy(bool busy)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_busy == busy)
            return;
        m_busy = busy;
    }
    emit busyChanged(busy);
}

QRect MattingService::subjectRoi(const QImage &mask, int expandPercent,
                                 int extraPixels)
{
    if (mask.isNull())
        return {};
    QImage levels = mask.format() == QImage::Format_Grayscale8
                        ? mask
                        : mask.convertToFormat(QImage::Format_Grayscale8);
    int minX = levels.width(), minY = levels.height();
    int maxX = -1, maxY = -1;
    for (int y = 0; y < levels.height(); ++y)
    {
        const uchar *line = levels.constScanLine(y);
        for (int x = 0; x < levels.width(); ++x)
        {
            if (line[x] >= 128)
            {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY)
        return {};
    QRect box(minX, minY, maxX - minX + 1, maxY - minY + 1);
    const int expandX = qMax(4, box.width() * qBound(0, expandPercent, 60) / 100);
    const int expandY = qMax(4, box.height() * qBound(0, expandPercent, 60) / 100);
    box.adjust(-expandX - extraPixels, -expandY - extraPixels,
               expandX + extraPixels, expandY + extraPixels);
    return box.intersected(levels.rect());
}

void MattingService::logInfo(const char *format, ...) const
{
    if (!m_obj)
        return;
    char buffer[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1] = '\0';
    pbLogInfo(m_obj, "matting", "%s", buffer);
}

} // namespace paintbrush
