/*****************************************************************************
 * pb_human_parser_service.cpp : NAI Paintbrush - asynchronous FASHN parsing
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_human_parser_service.hpp"

#include <vlc_configuration.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <cstdarg>
#include <cstdio>

namespace paintbrush {

namespace {

QString defaultModelDir()
{
    const QByteArray env = qgetenv("PB_HUMAN_PARSER_MODEL_DIR");
    if (!env.isEmpty())
        return QString::fromUtf8(env);
    const QByteArray runtimeDir = qgetenv("PB_HUMAN_PARSER_DIR");
    if (!runtimeDir.isEmpty())
        return QDir(QString::fromUtf8(runtimeDir))
            .filePath(QStringLiteral("models"));
    const QString app = QCoreApplication::applicationDirPath();
    if (!app.isEmpty())
        return QDir(app).filePath(QStringLiteral("human-parser/models"));
    return QStringLiteral("human-parser/models");
}

QString modelFileFor(const QString &dir)
{
    return QDir(dir).filePath(QStringLiteral("fashn-human-parser.onnx"));
}

QString manifestFileFor(const QString &dir)
{
    return QDir(dir).filePath(QStringLiteral("manifest.json"));
}

} // namespace

QImage composeSemanticCoverage(const QImage &subjectAlpha,
                               const QImage &semanticProbability,
                               creative::CoverageMode mode)
{
    if (subjectAlpha.isNull() || semanticProbability.isNull()
        || subjectAlpha.size() != semanticProbability.size())
        return {};

    const QImage alpha =
        subjectAlpha.convertToFormat(QImage::Format_Grayscale8);
    const QImage semantic =
        semanticProbability.convertToFormat(QImage::Format_Grayscale8);
    if (alpha.isNull() || semantic.isNull())
        return {};

    QImage out(alpha.size(), QImage::Format_Grayscale8);
    if (out.isNull())
        return {};

    for (int y = 0; y < out.height(); ++y)
    {
        const uchar *a = alpha.constScanLine(y);
        const uchar *b = semantic.constScanLine(y);
        uchar *o = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x)
            o[x] = uchar(creative::composeCoverage(a[x], b[x], mode));
    }
    return out;
}

HumanParserService::HumanParserService(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<paintbrush::HumanParseResult>(
        "paintbrush::HumanParseResult");
    m_modelDir = defaultModelDir();
    setStatusMsgid("Human parser is idle.");
}

HumanParserService::~HumanParserService()
{
    shutdown();
}

void HumanParserService::configure(vlc_object_t *obj)
{
    m_obj = obj;

    const QString appDir = QCoreApplication::applicationDirPath();
    m_installDir = QDir(QStandardPaths::writableLocation(
                            QStandardPaths::GenericDataLocation))
                       .filePath(QStringLiteral("NAI Paintbrush/human-parser"));
    m_manifestUrl = obj
        ? pbConfigString(obj, "paintbrush-human-parser-manifest-url").trimmed()
        : QString();

    const QString configured = obj
        ? pbConfigString(obj, "paintbrush-human-parser-model-dir").trimmed()
        : QString();
    const QString userModels = QDir(m_installDir).filePath(QStringLiteral("models"));
    const QString appModels = QDir(appDir).filePath(QStringLiteral("human-parser/models"));
    if (!configured.isEmpty())
        m_modelDir = configured;
    else if (QFileInfo::exists(modelFileFor(userModels)))
        m_modelDir = userModels;
    else
        m_modelDir = appModels;

    /* The shim resolves the runtime leaf from this environment variable. Prefer
     * the user-installed copy, then the packaged app copy. */
    const QString userRuntime =
        QDir(m_installDir).filePath(QStringLiteral("pb_human_parser_cuda.dll"));
    const QString chosenRuntime = QFileInfo::exists(userRuntime)
                                      ? m_installDir
                                      : QDir(appDir).filePath(QStringLiteral("human-parser"));
    qputenv("PB_HUMAN_PARSER_DIR", chosenRuntime.toUtf8());

    const QString device = obj
        ? pbConfigString(obj, "paintbrush-human-parser-device").trimmed().toLower()
        : QString();
    m_deviceTier = device == QLatin1String("gpu") ? 1
                 : device == QLatin1String("cpu") ? 2
                                                  : 0;

    if (obj)
    {
        const int probabilities =
            pbConfigInt(obj, "paintbrush-human-parser-class-probabilities");
        m_exportClassProbabilities = probabilities != 0;
    }

    human_parser::sharedRuntime().unload();
    m_loadedModelDir.clear();
    m_loadedDeviceTier = -1;

    {
        QMutexLocker lock(&m_mutex);
        if (!m_running)
        {
            m_running = true;
            m_worker = std::thread(&HumanParserService::workerLoop, this);
        }
    }

    probeAvailability();
}

bool HumanParserService::available() const
{
    QMutexLocker lock(&m_mutex);
    return m_available;
}

QString HumanParserService::unavailableReason() const
{
    QMutexLocker lock(&m_mutex);
    return m_unavailableReason;
}

QString HumanParserService::backendText() const
{
    QMutexLocker lock(&m_mutex);
    if (!m_backendText.isEmpty())
        return m_backendText;
    return m_available ? pbTr("human parser runtime ready")
                       : m_unavailableReason;
}

bool HumanParserService::canInstall() const
{
    QMutexLocker lock(&m_mutex);
    return m_canInstall;
}

bool HumanParserService::installing() const
{
    QMutexLocker lock(&m_mutex);
    return m_installing;
}

int HumanParserService::installProgress() const
{
    QMutexLocker lock(&m_mutex);
    return m_installProgress;
}

QString HumanParserService::installStatus() const
{
    QMutexLocker lock(&m_mutex);
    return m_installStatus;
}

QString HumanParserService::installDir() const
{
    QMutexLocker lock(&m_mutex);
    return m_installDir;
}

QString HumanParserService::manifestUrl() const
{
    QMutexLocker lock(&m_mutex);
    return m_manifestUrl;
}

QString HumanParserService::installerScript() const
{
    const QString app = QCoreApplication::applicationDirPath();
    if (app.isEmpty())
        return {};
    const QStringList candidates = {
        QDir(app).filePath(QStringLiteral("scripts/pb-install-human-parser.ps1")),
        QDir(app).filePath(QStringLiteral("../scripts/pb-install-human-parser.ps1")),
        QDir(app).filePath(QStringLiteral("../../scripts/pb-install-human-parser.ps1")),
        QDir(app).filePath(QStringLiteral("../../../scripts/pb-install-human-parser.ps1"))
    };
    for (const QString &candidate : candidates)
    {
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).canonicalFilePath();
    }
    return {};
}

void HumanParserService::setInstallStatus(const QString &status, int percent)
{
    percent = qBound(0, percent, 100);
    {
        QMutexLocker lock(&m_mutex);
        m_installStatus = status;
        m_installProgress = percent;
    }
    emit installProgressChanged(percent, status);
}

void HumanParserService::installHumanParser()
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_installing)
            return;
        m_installing = true;
        m_installProgress = 0;
        m_installStatus = pbTr("Preparing the fashion parser download...");
    }
    emit installProgressChanged(0, installStatus());

    QString manifestUrl;
    QString installDir;
    {
        QMutexLocker lock(&m_mutex);
        manifestUrl = m_manifestUrl;
        installDir = m_installDir;
    }
    if (manifestUrl.isEmpty())
    {
        {
            QMutexLocker lock(&m_mutex);
            m_installing = false;
            m_installStatus = pbTr(
                "No parser manifest URL is configured; set "
                "paintbrush-human-parser-manifest-url or use the post-installer.");
        }
        emit installProgressChanged(0, installStatus());
        emit installFinished(false, installStatus());
        return;
    }
    const QString script = installerScript();
    if (script.isEmpty())
    {
        {
            QMutexLocker lock(&m_mutex);
            m_installing = false;
            m_installStatus = pbTr(
                "The bundled post-installer script was not found next to the app.");
        }
        emit installProgressChanged(0, installStatus());
        emit installFinished(false, installStatus());
        return;
    }

    m_installProcess = new QProcess(this);
    m_installProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_installProcess, &QProcess::readyReadStandardOutput, this,
            [this]() {
                const QString output = QString::fromUtf8(
                    m_installProcess->readAllStandardOutput());
                const QStringList lines =
                    output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString &raw : lines)
                {
                    const QString line = raw.trimmed();
                    if (line.startsWith(QLatin1String("PBHP-PROGRESS ")))
                    {
                        const QString rest = line.mid(14);
                        const int space = rest.indexOf(QLatin1Char(' '));
                        bool ok = false;
                        const int percent =
                            (space > 0 ? rest.left(space) : rest).toInt(&ok);
                        if (ok)
                        {
                            setInstallStatus(
                                space > 0 ? rest.mid(space + 1) : QString(),
                                percent);
                        }
                    }
                    else if (line.startsWith(QLatin1String("PBHP-ERROR ")))
                    {
                        setInstallStatus(line.mid(11), installProgress());
                    }
                }
            });
    connect(m_installProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                {
                    QMutexLocker lock(&m_mutex);
                    m_installing = false;
                    m_installStatus =
                        pbTr("Could not start the fashion parser installer.");
                }
                emit installProgressChanged(0, installStatus());
                emit installFinished(false, installStatus());
                m_installProcess->deleteLater();
                m_installProcess = nullptr;
            });
    connect(m_installProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                const bool ok = status == QProcess::NormalExit && exitCode == 0;
                if (ok)
                {
                    human_parser::sharedRuntime().unload();
                    m_loadedModelDir.clear();
                    m_loadedDeviceTier = -1;
                    probeAvailability();
                    setInstallStatus(
                        pbTr("Fashion parser installed. It will be used for "
                             "the next parse."),
                        100);
                }
                else
                {
                    QString message;
                    {
                        QMutexLocker lock(&m_mutex);
                        m_installing = false;
                        message = m_installStatus.isEmpty()
                                      ? pbTr("The fashion parser installer failed.")
                                      : m_installStatus;
                    }
                    emit installFinished(false, message);
                }
                {
                    QMutexLocker lock(&m_mutex);
                    m_installing = false;
                }
                if (ok)
                    emit installFinished(true, QString());
                m_installProcess->deleteLater();
                m_installProcess = nullptr;
            });

    m_installProcess->start(
        QStringLiteral("pwsh"),
        { QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
          QStringLiteral("Bypass"), QStringLiteral("-File"), script,
          QStringLiteral("-ManifestUrl"), manifestUrl,
          QStringLiteral("-InstallDir"), installDir,
          QStringLiteral("-AppDir"),
          QCoreApplication::applicationDirPath() });
}

void HumanParserService::setModelDir(const QString &dir)
{
    const QString trimmed = dir.trimmed();
    const QString resolved = trimmed.isEmpty() ? defaultModelDir() : trimmed;
    {
        QMutexLocker lock(&m_mutex);
        if (resolved == m_modelDir)
            return;
        m_modelDir = resolved;
    }
    if (m_obj)
        config_PutPsz("paintbrush-human-parser-model-dir",
                      resolved.toUtf8().constData());
    probeAvailability();
}

void HumanParserService::setDeviceTier(int tier)
{
    tier = qBound(0, tier, 2);
    {
        QMutexLocker lock(&m_mutex);
        if (tier == m_deviceTier)
            return;
        m_deviceTier = tier;
    }
    if (m_obj)
    {
        const char *name = tier == 1 ? "gpu" : (tier == 2 ? "cpu" : "auto");
        config_PutPsz("paintbrush-human-parser-device", name);
    }
    probeAvailability();
}

void HumanParserService::setExportClassProbabilities(bool enabled)
{
    {
        QMutexLocker lock(&m_mutex);
        if (enabled == m_exportClassProbabilities)
            return;
        m_exportClassProbabilities = enabled;
    }
    if (m_obj)
        config_PutInt("paintbrush-human-parser-class-probabilities",
                      enabled ? 1 : 0);
}

quint64 HumanParserService::request(const QImage &rgb,
                                    const QImage &subjectAlpha,
                                    quint64 revision)
{
    if (rgb.isNull() || subjectAlpha.isNull()
        || rgb.size() != subjectAlpha.size())
        return 0;

    Job job;
    /* Snapshot on the caller's thread: a later canvas edit must not race the
     * worker, and convertToFormat() may return a shallow copy when the format
     * already matches. */
    job.rgb = rgb.convertToFormat(QImage::Format_RGB888).copy();
    if (job.rgb.isNull())
        return 0;

    if (subjectAlpha.hasAlphaChannel())
    {
        const QImage rgba =
            subjectAlpha.convertToFormat(QImage::Format_RGBA8888);
        if (rgba.isNull())
            return 0;
        job.subjectAlpha =
            QImage(rgba.width(), rgba.height(), QImage::Format_Grayscale8);
        if (job.subjectAlpha.isNull())
            return 0;
        for (int y = 0; y < rgba.height(); ++y)
        {
            const uchar *src = rgba.constScanLine(y);
            uchar *dst = job.subjectAlpha.scanLine(y);
            for (int x = 0; x < rgba.width(); ++x)
                dst[x] = src[x * 4 + 3];
        }
    }
    else
    {
        job.subjectAlpha =
            subjectAlpha.convertToFormat(QImage::Format_Grayscale8).copy();
        if (job.subjectAlpha.isNull())
            return 0;
    }

    job.revision = revision;

    bool notifyBusy = false;
    quint64 requestId = 0;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_running)
            return 0;

        job.id = m_nextId++;
        requestId = job.id;
        m_latestId = job.id;
        m_queue.clear(); /* coalesce: only the newest interactive job */
        m_queue.push_back(job);
        m_cond.wakeAll();

        if (!m_busy)
        {
            m_busy = true;
            notifyBusy = true;
        }
    }
    if (notifyBusy)
        emit busyChanged(true);
    return requestId;
}

void HumanParserService::cancel(quint64 requestId)
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
    human_parser::sharedRuntime().cancel();
}

void HumanParserService::cancelAll()
{
    cancel(0);
}

void HumanParserService::shutdown()
{
    /* Stop a long inference/postprocess before waiting for the worker. */
    m_cancelRequested.store(true);
    human_parser::sharedRuntime().cancel();
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
    human_parser::sharedRuntime().unload();
}

void HumanParserService::workerLoop()
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

void HumanParserService::runJob(const Job &job)
{
    const auto superseded = [&]() {
        QMutexLocker lock(&m_mutex);
        return m_latestId != job.id;
    };

    if (superseded() || m_cancelRequested.load())
        return;

    setStatusMsgid("Reading the look...");

    QString error;
    if (!ensureRuntime(&error))
    {
        if (superseded())
            return;
        setStatusMsgid("Human parse failed.");
        setBusy(false);
        emit parseFailed(job.id, error);
        logInfo("human parser runtime failed: %s", error.toUtf8().constData());
        return;
    }

    human_parser::Masks masks;
    human_parser::RunInfo info;
    if (!human_parser::sharedRuntime().parse(job.rgb, &masks, &info, &error))
    {
        if (superseded())
            return;
        setStatusMsgid("Human parse failed.");
        setBusy(false);
        emit parseFailed(job.id, error);
        logInfo("human parse failed: %s", error.toUtf8().constData());
        return;
    }

    if (m_cancelRequested.load())
    {
        if (superseded())
            return;
        setBusy(false);
        emit parseFailed(job.id, QStringLiteral("cancelled"));
        return;
    }

    if (superseded())
        return; /* a newer request superseded this result */

    HumanParseResult result;
    result.masks = std::move(masks);
    result.info = info;
    result.subjectAlpha = job.subjectAlpha;
    result.revision = job.revision;

    setStatusMsgid("Look parsed - %1 (%2 ms)",
                   {info.backend,
                    QString::number(info.inferenceMs, 'f', 1)});
    logInfo("parsed %dx%d in %.1f ms (%s); garmentPx=%llu skinPx=%llu "
            "hairPx=%llu accessoryPx=%llu",
            info.sourceWidth, info.sourceHeight, info.inferenceMs,
            info.backend.toUtf8().constData(),
            static_cast<unsigned long long>(
                result.masks.groupPixels.value(1)),
            static_cast<unsigned long long>(
                result.masks.groupPixels.value(3)),
            static_cast<unsigned long long>(
                result.masks.groupPixels.value(2)),
            static_cast<unsigned long long>(
                result.masks.groupPixels.value(4)));

    setBusy(false);
    emit parseReady(result, job.revision, job.id);
}

bool HumanParserService::ensureRuntime(QString *error)
{
    QString modelDir;
    int deviceTier = 0;
    bool exportProbabilities = false;
    {
        QMutexLocker lock(&m_mutex);
        modelDir = m_modelDir;
        deviceTier = m_deviceTier;
        exportProbabilities = m_exportClassProbabilities;
    }

    human_parser::Runtime &runtime = human_parser::sharedRuntime();
    if (runtime.isLoaded())
    {
        if (m_loadedModelDir == modelDir && m_loadedDeviceTier == deviceTier)
        {
            runtime.setExportClassProbabilities(exportProbabilities);
            return true;
        }
        runtime.unload();
        m_loadedModelDir.clear();
        m_loadedDeviceTier = -1;
    }

    const human_parser::Capabilities caps = human_parser::Runtime::probe();
    if (!caps.runtimePresent)
    {
        if (error)
            *error = caps.detail.isEmpty()
                         ? QStringLiteral("human parser runtime is not installed")
                         : caps.detail;
        return false;
    }
    if (deviceTier == 1 && !caps.cudaAvailable)
    {
        if (error)
            *error = pbTr("GPU parsing was requested but CUDA is not available");
        return false;
    }
    if (deviceTier != 2 && !caps.cudaAvailable && !caps.cpuAvailable)
    {
        if (error)
            *error = QStringLiteral(
                "no ONNX Runtime execution provider is available");
        return false;
    }

    const QString modelPath = modelFileFor(modelDir);
    if (!QFileInfo::exists(modelPath))
    {
        if (error)
            *error = QStringLiteral("human parser model not found: %1")
                         .arg(modelPath);
        return false;
    }

    const QString manifestPath = manifestFileFor(modelDir);
    if (!QFileInfo::exists(manifestPath))
    {
        if (error)
            *error = QStringLiteral("human parser manifest not found: %1")
                         .arg(manifestPath);
        return false;
    }

    human_parser::SessionConfig config;
    config.modelPath = modelPath;
    config.manifestPath = manifestPath;
    config.device = deviceTier == 1 ? 1 : (deviceTier == 2 ? 2 : 0);
    /* AUTO may fall back to CPU and says so; GPU-required fails loudly. */
    config.requireGpuOperators = deviceTier == 1;
    config.verifyModelHash = true;
    config.exportClassProbabilities = exportProbabilities;

    if (qEnvironmentVariableIsSet("PB_HUMAN_PARSER_PROFILE"))
    {
        const QString profileDir =
            QDir(modelDir).filePath(QStringLiteral("profiles"));
        QDir().mkpath(profileDir);
        config.profileDir = profileDir;
    }

    QString loadError;
    if (!runtime.load(config, &loadError))
    {
        m_loadedModelDir.clear();
        m_loadedDeviceTier = -1;
        if (error)
            *error = loadError;
        QMutexLocker lock(&m_mutex);
        m_unavailableReason = loadError;
        return false;
    }

    m_loadedModelDir = modelDir;
    m_loadedDeviceTier = deviceTier;
    const QString backend = runtime.backend();
    {
        QMutexLocker lock(&m_mutex);
        m_backendText = backend;
        m_available = true;
        m_unavailableReason.clear();
    }
    emit availabilityChanged();
    return true;
}

void HumanParserService::probeAvailability()
{
    /* This runs while the controller is being constructed, so it must stay
     * cheap: checking the packaged files does not load ONNX Runtime, the CUDA
     * provider, cuDNN or cublasLt on the GUI thread. The worker performs the
     * real runtime probe and model load on the first parse; a broken runtime
     * then surfaces as an honest parseFailed instead of a frozen startup. */
    QString modelDir;
    QString installDir;
    QString manifestUrl;
    {
        QMutexLocker lock(&m_mutex);
        modelDir = m_modelDir;
        installDir = m_installDir;
        manifestUrl = m_manifestUrl;
    }
    /* Do NOT call Runtime::dllPath() here: it calls ensureApi(), which loads
     * pb_human_parser_cuda.dll and its ONNX Runtime/CUDA closure. That is a
     * worker task on first use, not a GUI-thread startup task. */
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList runtimeDirs = {
        installDir,
        QDir(appDir).filePath(QStringLiteral("human-parser"))
    };
    QString runtimeDir;
    bool dllPresent = false;
    for (const QString &candidate : runtimeDirs)
    {
        const QFileInfo runtime(
            QDir(candidate).filePath(QStringLiteral("pb_human_parser_cuda.dll")));
        if (runtime.exists())
        {
            runtimeDir = candidate;
            dllPresent = true;
            break;
        }
    }

    const QStringList modelDirs = {
        modelDir,
        QDir(installDir).filePath(QStringLiteral("models")),
        QDir(appDir).filePath(QStringLiteral("human-parser/models"))
    };
    QString chosenModelDir;
    QFileInfo model;
    QFileInfo manifest;
    for (const QString &candidate : modelDirs)
    {
        const QFileInfo candidateModel(modelFileFor(candidate));
        const QFileInfo candidateManifest(manifestFileFor(candidate));
        if (candidateModel.exists() && candidateManifest.exists())
        {
            chosenModelDir = candidate;
            model = candidateModel;
            manifest = candidateManifest;
            break;
        }
    }
    const bool available = dllPresent && !chosenModelDir.isEmpty();

    QString reason;
    if (!dllPresent)
        reason = pbTr("human parser runtime is not installed");
    else if (chosenModelDir.isEmpty())
    {
        reason = QStringLiteral("human parser model not found: %1")
                     .arg(modelFileFor(modelDir));
    }

    const bool canInstall = !available && !manifestUrl.isEmpty()
                            && !installerScript().isEmpty();
    {
        QMutexLocker lock(&m_mutex);
        m_available = available;
        if (available)
            m_modelDir = chosenModelDir;
        m_canInstall = canInstall;
        m_backendText.clear();
        m_unavailableReason = available ? QString() : reason;
        if (!available && canInstall && !m_installing)
        {
            m_installStatus = pbTr(
                "Fashion parser is not installed. Download it to keep "
                "materials on the outfit.");
        }
        else if (!available && m_installStatus.isEmpty())
        {
            m_installStatus = m_unavailableReason;
        }
    }
    if (available)
        qputenv("PB_HUMAN_PARSER_DIR", runtimeDir.toUtf8());
    emit availabilityChanged();
}

void HumanParserService::setStatus(const QString &status)
{
    {
        QMutexLocker lock(&m_mutex);
        m_status = status;
        m_statusMsgid = nullptr;
        m_statusArgs.clear();
    }
    emit statusChanged(status);
}

void HumanParserService::setStatusMsgid(const char *msgid,
                                        const QStringList &args)
{
    {
        QMutexLocker lock(&m_mutex);
        m_statusMsgid = msgid;
        m_statusArgs = args;
        m_status.clear();
    }
    emit statusChanged(statusText());
}

bool HumanParserService::isBusy() const
{
    QMutexLocker lock(&m_mutex);
    return m_busy;
}

QString HumanParserService::statusText() const
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

void HumanParserService::setBusy(bool busy)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_busy == busy)
            return;
        m_busy = busy;
    }
    emit busyChanged(busy);
}

void HumanParserService::logInfo(const char *format, ...) const
{
    if (!m_obj)
        return;
    char buffer[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1] = '\0';
    pbLogInfo(m_obj, "human-parser", "%s", buffer);
}

} // namespace paintbrush
