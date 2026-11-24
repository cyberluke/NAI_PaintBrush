/*****************************************************************************
 * pb_lama_service.cpp : NAI Paintbrush — native LaMa inpainting service
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_lama_service.hpp"
#include "gpu/pb_lama.h"

#include <vlc_configuration.h>
#include <vlc_plugin.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QThread>

#include <cstdarg>
#include <cstdio>

namespace paintbrush {

namespace {

/* The shipped Carve/LaMa-ONNX lama_fp32.onnx (Apache-2.0) is pinned by hash.
 * scripts/pb-package-lama-cuda.ps1 copies this exact graph and records the
 * same value in DEPENDENCIES.json. */
constexpr const char *kPinnedLamaSha256 =
    "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6";
constexpr const char *kModelFileName = "lama_fp32.onnx";

/* Config keys are read defensively: qt.cpp owns their registration, and a
 * build that has not registered them yet must not assert inside VLC's config
 * layer. Environment overrides keep the portable/test path usable. */
QString registeredString(vlc_object_t *obj, const char *name)
{
    if (!obj)
        return {};
    const int type = config_GetType(name);
    if (type == 0 || CONFIG_CLASS(type) != CONFIG_ITEM_STRING)
        return {};
    return pbConfigString(obj, name).trimmed();
}

QString appLamaRoot()
{
    const QString app = QCoreApplication::applicationDirPath();
    if (!app.isEmpty())
        return QDir(app).filePath(QStringLiteral("lama"));
    return QStringLiteral("lama");
}

QString normalizeModelPath(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return {};
    QFileInfo info(trimmed);
    if (!info.isDir())
        return QDir::cleanPath(trimmed);
    const QStringList candidates = {
        QDir(trimmed).filePath(QStringLiteral("models/") + QString::fromLatin1(kModelFileName)),
        QDir(trimmed).filePath(QString::fromLatin1(kModelFileName)),
    };
    for (const QString &candidate : candidates)
    {
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QDir::cleanPath(candidates.first());
}

QString defaultModelPath()
{
    const QByteArray env = qgetenv("PB_LAMA_DIR");
    if (!env.isEmpty())
    {
        const QString resolved = normalizeModelPath(QString::fromUtf8(env));
        if (!resolved.isEmpty())
            return resolved;
    }
    return QDir(appLamaRoot()).filePath(
        QStringLiteral("models/") + QString::fromLatin1(kModelFileName));
}

QString readPinnedShaFromDependencies(const QString &modelPath)
{
    /* <lama>/models/lama_fp32.onnx -> <lama>/DEPENDENCIES.json */
    const QDir modelDir = QFileInfo(modelPath).absoluteDir();
    const QString manifest = modelDir.filePath(QStringLiteral("../DEPENDENCIES.json"));
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return {};
    const QJsonObject model = document.object().value(QStringLiteral("model")).toObject();
    QString sha = model.value(QStringLiteral("sha256")).toString().trimmed();
    if (sha.isEmpty())
        sha = model.value(QStringLiteral("pinnedSha256")).toString().trimmed();
    return sha.toLower();
}

} // namespace

LamaService::LamaService(QObject *parent)
    : QObject(parent)
{
    m_modelPath = defaultModelPath();
    setStatusMsgid("Local LaMa inpainting is idle.");
}

LamaService::~LamaService()
{
    shutdown();
}

void LamaService::configure(vlc_object_t *obj)
{
    m_obj = obj;

    const QString configured = registeredString(obj, "paintbrush-lama-model");
    const QString resolved = configured.isEmpty()
                                 ? defaultModelPath()
                                 : normalizeModelPath(configured);

    /* PB_LAMA_DEVICE is a test/portable override; otherwise the registered
     * option (default "auto") selects the tier. */
    QString device = QString::fromUtf8(qgetenv("PB_LAMA_DEVICE")).trimmed().toLower();
    if (device.isEmpty())
        device = registeredString(obj, "paintbrush-lama-device").toLower();
    const int tier = device == QLatin1String("gpu") ? 1
                   : device == QLatin1String("cpu") ? 2
                                                    : 0;

    bool changed;
    {
        QMutexLocker lock(&m_mutex);
        changed = (resolved != m_modelPath) || (tier != m_deviceTier);
        m_modelPath = resolved;
        m_deviceTier = tier;
        /* An explicit re-configure is also a retry after a load failure. */
        m_loadFailure.clear();
    }
    if (changed)
        lama::sharedRuntime().unload();

    {
        QMutexLocker lock(&m_mutex);
        if (!m_running)
        {
            m_running = true;
            m_worker = std::thread(&LamaService::workerLoop, this);
        }
    }

    /* Probe only files/providers; no 208 MB model load at startup. */
    refreshAvailability(true);
}

void LamaService::refreshAvailability(bool emitSignal)
{
    const lama::Capabilities caps = lama::Runtime::probe();
    const bool modelPresent = !m_modelPath.isEmpty()
                              && QFileInfo::exists(m_modelPath);

    bool deviceOk = false;
    QString reason;
    if (!caps.runtimePresent)
        reason = caps.detail.isEmpty()
                     ? pbTr("pb_lama_cuda.dll is not installed")
                     : caps.detail;
    else if (!modelPresent)
        reason = pbTr("LaMa model not found: %1").arg(m_modelPath);
    else if (m_deviceTier == 1 && !caps.cudaAvailable)
        reason = pbTr("GPU LaMa was requested but CUDA is not available");
    else if (m_deviceTier == 2 && !caps.cpuAvailable)
        reason = pbTr("CPU LaMa was requested but the CPU provider is unavailable");
    else if (m_deviceTier != 2 && !caps.cudaAvailable && !caps.cpuAvailable)
        reason = pbTr("no ONNX Runtime execution provider is available");
    else
        deviceOk = true;

    const bool available = caps.runtimePresent && modelPresent && deviceOk;
    QString backend = caps.backend;
    if (available && m_deviceTier == 2)
        backend = QStringLiteral("CPUExecutionProvider");
    if (available && backend.isEmpty())
        backend = m_deviceTier == 1 ? QStringLiteral("CUDAExecutionProvider")
                                    : QStringLiteral("ONNX Runtime");

    bool changed = false;
    {
        QMutexLocker lock(&m_mutex);
        changed = (available != m_available)
                  || (backend != m_backendText)
                  || (reason != m_unavailableReason);
        m_probed = true;
        m_available = available;
        m_backendText = backend;
        m_unavailableReason = reason;
    }
    if (emitSignal && changed)
        emit availabilityChanged();
}

bool LamaService::available() const
{
    QMutexLocker lock(&m_mutex);
    return m_available;
}

QString LamaService::unavailableReason() const
{
    QMutexLocker lock(&m_mutex);
    return m_unavailableReason;
}

QString LamaService::backendText() const
{
    QMutexLocker lock(&m_mutex);
    if (!m_backendText.isEmpty())
        return m_backendText;
    return m_available ? pbTr("LaMa 512 (local ONNX)")
                       : m_unavailableReason;
}

QString LamaService::modelPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_modelPath;
}

void LamaService::setModelPath(const QString &path)
{
    const QString resolved = path.trimmed().isEmpty() ? defaultModelPath()
                                                      : normalizeModelPath(path);
    {
        QMutexLocker lock(&m_mutex);
        if (resolved == m_modelPath)
            return;
        m_modelPath = resolved;
        m_loadFailure.clear();
    }
    lama::sharedRuntime().unload();
    if (m_obj && config_GetType("paintbrush-lama-model") != 0)
        config_PutPsz("paintbrush-lama-model", resolved.toUtf8().constData());
    refreshAvailability(true);
}

void LamaService::setDeviceTier(int tier)
{
    tier = qBound(0, tier, 2);
    {
        QMutexLocker lock(&m_mutex);
        if (tier == m_deviceTier)
            return;
        m_deviceTier = tier;
        m_loadFailure.clear();
    }
    lama::sharedRuntime().unload();
    if (m_obj && config_GetType("paintbrush-lama-device") != 0)
    {
        const char *name = tier == 1 ? "gpu" : (tier == 2 ? "cpu" : "auto");
        config_PutPsz("paintbrush-lama-device", name);
    }
    refreshAvailability(true);
}

QString LamaService::expectedSha256() const
{
    const QByteArray env = qgetenv("PB_LAMA_SHA256");
    if (!env.isEmpty())
        return QString::fromUtf8(env).trimmed().toLower();

    const QString fromManifest = readPinnedShaFromDependencies(m_modelPath);
    if (!fromManifest.isEmpty())
        return fromManifest;

    /* Only the shipped filename is assumed to match the pinned graph; a
     * custom model path is not silently hashed against someone else's pin. */
    if (QFileInfo(m_modelPath).fileName().compare(
            QString::fromLatin1(kModelFileName), Qt::CaseInsensitive) == 0)
        return QString::fromLatin1(kPinnedLamaSha256);
    return {};
}

bool LamaService::ensureRuntime(QString *error)
{
    if (m_loadFailure.size() > 0)
    {
        if (error)
            *error = m_loadFailure;
        return false;
    }

    lama::Runtime &runtime = lama::sharedRuntime();
    if (runtime.isLoaded())
    {
        const lama::RunInfo info = runtime.backendInfo();
        if (info.inputSize == m_inputSize && runtime.modelPath() == m_modelPath)
            return true;
        runtime.unload();
    }

    const lama::Capabilities caps = lama::Runtime::probe();
    if (!caps.runtimePresent)
    {
        const QString detail = caps.detail.isEmpty()
                                   ? pbTr("pb_lama_cuda.dll is not installed")
                                   : caps.detail;
        if (error)
            *error = detail;
        return false;
    }
    if (m_deviceTier == 1 && !caps.cudaAvailable)
    {
        if (error)
            *error = pbTr("GPU LaMa was requested but CUDA is not available");
        return false;
    }
    if (m_deviceTier != 2 && !caps.cudaAvailable && !caps.cpuAvailable)
    {
        if (error)
            *error = QStringLiteral("no ONNX Runtime execution provider is available");
        return false;
    }

    lama::SessionConfig config;
    config.modelPath = m_modelPath;
    config.expectedSha256 = expectedSha256();
    config.inputSize = m_inputSize;
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
        /* AUTO: use CUDA when the probe confirmed a complete GPU closure,
         * otherwise select CPU explicitly instead of attempting a provider
         * load that cannot succeed. */
        if (caps.cudaAvailable)
        {
            config.device = 0;
            config.requireGpuOperators = true;
        }
        else
        {
            config.device = 2;
            config.requireGpuOperators = false;
        }
    }

    if (qEnvironmentVariableIsSet("PB_LAMA_PROFILE"))
    {
        const QString profileDir = QFileInfo(m_modelPath).absoluteDir().filePath(
            QStringLiteral("profiles"));
        QDir().mkpath(profileDir);
        config.profileDir = profileDir;
    }

    QString loadError;
    if (!runtime.load(config, &loadError))
    {
        if (error)
            *error = loadError;
        {
            QMutexLocker lock(&m_mutex);
            m_loadFailure = loadError;
            m_available = false;
            m_unavailableReason = loadError;
        }
        emit availabilityChanged();
        return false;
    }

    const lama::RunInfo info = runtime.backendInfo();
    {
        QMutexLocker lock(&m_mutex);
        m_loadFailure.clear();
        m_available = true;
        m_unavailableReason.clear();
        m_backendText = info.backend.isEmpty() ? runtime.backend() : info.backend;
    }
    emit availabilityChanged();
    return true;
}

quint64 LamaService::request(const QImage &rgb, const QImage &mask, const QString &tag)
{
    if (rgb.isNull() || mask.isNull() || rgb.size() != mask.size())
        return 0;

    QImage imageCopy = rgb;
    QImage maskCopy = mask;
    imageCopy.detach();
    maskCopy.detach();

    QMutexLocker lock(&m_mutex);
    if (!m_running)
        return 0;

    Job job;
    job.rgb = imageCopy;
    job.mask = maskCopy;
    job.tag = tag;
    job.id = m_nextId++;
    m_latestId = job.id;
    m_queue.clear();          /* coalesce: only the newest interactive job   */
    m_queue.push_back(job);
    m_cond.wakeAll();

    if (!m_busy)
    {
        m_busy = true;
        lock.unlock();
        emit busyChanged(true);
    }
    return job.id;
}

void LamaService::cancel(quint64 requestId)
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
    lama::sharedRuntime().cancel();
}

void LamaService::cancelAll()
{
    cancel(0);
}

void LamaService::shutdown()
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_running)
            return;
        m_running = false;
        m_queue.clear();
        m_cond.wakeAll();
    }
    m_cancelRequested.store(true);
    lama::sharedRuntime().cancel();
    if (m_worker.joinable())
        m_worker.join();
    lama::sharedRuntime().unload();
}

bool LamaService::isBusy() const
{
    QMutexLocker lock(&m_mutex);
    return m_busy;
}

QString LamaService::statusText() const
{
    QMutexLocker lock(&m_mutex);
    if (m_statusMsgid != nullptr)
    {
        QString text = pbTr(m_statusMsgid);
        for (const QString &arg : m_statusArgs)
            text = text.arg(arg);
        return text;
    }
    return m_status;
}

LamaTimings LamaService::lastTimings() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastTimings;
}

void LamaService::workerLoop()
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

void LamaService::runJob(const Job &job)
{
    const bool superseded = [&]() {
        QMutexLocker lock(&m_mutex);
        return m_latestId != job.id;
    }();
    if (superseded)
        return; /* a newer job is queued/running and keeps the busy state */
    if (m_cancelRequested.load())
    {
        setBusy(false);
        return;
    }

    setStatusMsgid("Inpainting with the local LaMa model…");

    QString error;
    if (!ensureRuntime(&error))
    {
        emit inpaintFailed(job.tag, error);
        setStatusMsgid("LaMa inpaint unavailable: %1", { error });
        setBusy(false);
        return;
    }

    lama::RunInfo info;
    QImage result;
    QString inpaintError;
    const bool ok = lama::sharedRuntime().inpaint(job.rgb, job.mask, &result,
                                                  &info, &inpaintError);
    if (!ok)
    {
        if (m_cancelRequested.load())
        {
            emit inpaintFailed(job.tag, QStringLiteral("cancelled"));
            setBusy(false);
            return;
        }
        emit inpaintFailed(job.tag, inpaintError);
        setStatusMsgid("LaMa inpaint failed: %1", { inpaintError });
        logInfo("LaMa inpaint failed: %s", inpaintError.toUtf8().constData());
        setBusy(false);
        return;
    }

    if (m_cancelRequested.load())
    {
        emit inpaintFailed(job.tag, QStringLiteral("cancelled"));
        setBusy(false);
        return;
    }

    {
        QMutexLocker lock(&m_mutex);
        if (m_latestId != job.id)
            return; /* a newer request superseded this result */
    }

    const QString backend = info.backend.isEmpty() ? backendText() : info.backend;
    setStatusMsgid("LaMa inpaint ready — %1 (%2 ms)",
                   { backend, QString::number(info.inferenceMs, 'f', 1) });

    /* Publication: hand the finished image to the consumer. The elapsed time
     * covers the queued signal delivery setup; the shim already measured the
     * RGBA -> QImage materialisation and exposed it as info.publishMs. */
    QElapsedTimer publishTimer;
    publishTimer.start();
    emit inpaintReady(job.tag, result);
    const double publishMs = (info.publishMs > 0.0 ? info.publishMs : 0.0)
                             + double(publishTimer.nsecsElapsed()) / 1e6;

    LamaTimings timings;
    timings.valid = true;
    timings.loadMs = info.loadMs;
    timings.maskAnalysisMs = info.maskAnalysisMs;
    timings.preprocessMs = info.preprocessMs;
    timings.inferenceMs = info.inferenceMs;
    timings.compositeMs = info.compositeMs;
    timings.publishMs = publishMs;
    timings.totalMs = info.totalMs;
    timings.usedGpu = info.usedGpu;
    timings.components = info.components;
    timings.backend = backend;
    {
        QMutexLocker lock(&m_mutex);
        m_lastTimings = timings;
    }

    logInfo("LaMa inpaint ready: backend=%s gpu=%d roi=%d,%d %dx%d components=%d "
            "load=%.1fms mask=%.1fms preprocess=%.1fms infer=%.1fms "
            "composite=%.1fms publish=%.1fms total=%.1fms",
            backend.toUtf8().constData(), info.usedGpu ? 1 : 0,
            info.roiX, info.roiY, info.roiW, info.roiH, info.components,
            info.loadMs, info.maskAnalysisMs, info.preprocessMs, info.inferenceMs,
            info.compositeMs, publishMs, info.totalMs);

    setBusy(false);
}

void LamaService::setStatus(const QString &status)
{
    {
        QMutexLocker lock(&m_mutex);
        m_status = status;
        m_statusMsgid = nullptr;
        m_statusArgs.clear();
    }
    emit statusChanged(status);
}

void LamaService::setStatusMsgid(const char *msgid, const QStringList &args)
{
    {
        QMutexLocker lock(&m_mutex);
        m_statusMsgid = msgid;
        m_statusArgs = args;
        m_status.clear();
    }
    emit statusChanged(statusText());
}

void LamaService::setBusy(bool busy)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_busy == busy)
            return;
        m_busy = busy;
    }
    emit busyChanged(busy);
}

void LamaService::logInfo(const char *format, ...) const
{
    if (!m_obj)
        return;
    char buffer[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1] = '\0';
    pbLogInfo(m_obj, "lama", "%s", buffer);
}

} // namespace paintbrush
