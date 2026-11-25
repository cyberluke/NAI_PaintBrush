/*****************************************************************************
 * pb_human_parser_service.hpp : NAI Paintbrush - asynchronous FASHN parsing
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_HUMAN_PARSER_SERVICE_HPP
#define PB_HUMAN_PARSER_SERVICE_HPP

#include "pb_common.hpp"
#include "pb_creative_art.hpp"
#include "gpu/pb_human_parser.h"

#include <QImage>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

class QProcess;

#include <atomic>
#include <deque>
#include <memory>
#include <thread>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

namespace paintbrush {

/* One completed parse. The subject alpha is the immutable SAM3-owned input
 * snapshot; the parser masks are independent semantic probabilities. */
struct HumanParseResult
{
    human_parser::Masks masks;
    human_parser::RunInfo info;
    QImage subjectAlpha; /* Grayscale8, same size as masks */
    quint64 revision = 0;

    bool isValid() const
    {
        return !masks.classMap.isNull() && !subjectAlpha.isNull()
               && masks.classMap.size() == subjectAlpha.size();
    }
};

/* Combine a finished subject alpha with an independent semantic probability
 * through the canonical CoverageMode contract from pb_creative_art.cpp.
 * InheritedCoverage/UserConstraint use min(); IndependentProbability uses the
 * rounded product. This is the only place the two coverages meet. */
QImage composeSemanticCoverage(
    const QImage &subjectAlpha, const QImage &semanticProbability,
    creative::CoverageMode mode = creative::CoverageMode::InheritedCoverage);

/* Asynchronous parsing around pb_human_parser_cuda.dll.
 *
 * Contract:
 *   - the parser never changes the subject identity; it consumes the SAM3
 *     alpha supplied by the caller and returns independent semantic planes;
 *   - a model/runtime failure is reported as a failure, never as a fabricated
 *     mask;
 *   - only the newest interactive request is executed (superseded jobs are
 *     dropped) and a stale completion is not emitted;
 *   - the GUI thread never blocks on inference; inputs are snapshotted before
 *     the job is queued;
 *   - the model is loaded once and kept resident.
 */
class HumanParserService : public QObject
{
    Q_OBJECT

public:
    explicit HumanParserService(QObject *parent = nullptr);
    ~HumanParserService() override;

    void configure(vlc_object_t *obj);

    bool available() const;
    QString unavailableReason() const;
    QString backendText() const;

    /* Dynamic install / post-install support. The app checks the user-writable
     * install directory (and the packaged app directory) on every launch; if
     * the parser is missing and a manifest URL is configured, the Play Pad can
     * run the bundled installer on demand. */
    bool canInstall() const;
    bool installing() const;
    int installProgress() const;
    QString installStatus() const;
    QString installDir() const;
    QString manifestUrl() const;
    void installHumanParser();

    QString modelDir() const
    {
        QMutexLocker lock(&m_mutex);
        return m_modelDir;
    }
    void setModelDir(const QString &dir);

    /* 0 auto, 1 gpu required, 2 cpu */
    int deviceTier() const
    {
        QMutexLocker lock(&m_mutex);
        return m_deviceTier;
    }
    void setDeviceTier(int tier);

    /* Full 18-plane probabilities are opt-in: at 4K they cost ~216 MB per
     * request. The grouped planes are always produced. */
    bool exportClassProbabilities() const
    {
        QMutexLocker lock(&m_mutex);
        return m_exportClassProbabilities;
    }
    void setExportClassProbabilities(bool enabled);

    bool isBusy() const;
    QString statusText() const;

    /* Queue a parse. `rgb` and `subjectAlpha` must be the same size. The images
     * are converted/copied on the calling thread so a later edit cannot race
     * the worker. Returns the request id (0 = invalid input). */
    quint64 request(const QImage &rgb, const QImage &subjectAlpha,
                    quint64 revision);
    void cancel(quint64 requestId);
    void cancelAll();
    void shutdown();

signals:
    void parseReady(const paintbrush::HumanParseResult &result,
                    quint64 revision, quint64 requestId);
    void parseFailed(quint64 requestId, const QString &error);
    void availabilityChanged();
    void busyChanged(bool busy);
    void statusChanged(const QString &status);
    void installProgressChanged(int percent, const QString &status);
    void installFinished(bool ok, const QString &error);

private:
    struct Job
    {
        QImage rgb;          /* RGB888 snapshot */
        QImage subjectAlpha; /* Grayscale8 snapshot */
        quint64 revision = 0;
        quint64 id = 0;
    };

    void workerLoop();
    void runJob(const Job &job);
    void setStatus(const QString &status);
    void setStatusMsgid(const char *msgid, const QStringList &args = {});
    void setBusy(bool busy);
    bool ensureRuntime(QString *error);
    void probeAvailability();
    void ensureProvisioned();
    QString installerScript() const;
    void setInstallStatus(const QString &status, int percent);
    void logInfo(const char *format, ...) const;

    vlc_object_t *m_obj = nullptr;
    QString m_modelDir;
    int m_deviceTier = 0;
    bool m_exportClassProbabilities = false;

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    std::thread m_worker;
    std::deque<Job> m_queue;
    bool m_running = false;
    bool m_busy = false;
    quint64 m_nextId = 1;
    quint64 m_latestId = 0;
    quint64 m_activeId = 0;
    std::atomic<bool> m_cancelRequested{false};
    mutable QString m_status;
    const char *m_statusMsgid = nullptr;
    QStringList m_statusArgs;

    bool m_available = false;
    QString m_unavailableReason;
    QString m_backendText;

    /* Installer state. The process and its path are GUI-thread objects; the
     * worker never touches them. */
    QString m_installDir;
    QString m_manifestUrl;
    QString m_manifestSha256;
    bool m_canInstall = false;
    bool m_installing = false;
    int m_installProgress = 0;
    QString m_installStatus;
    QProcess *m_installProcess = nullptr;

    /* Worker-owned: configuration of the loaded session. Changing the model
     * directory or device marks the runtime stale; the worker unloads and
     * reloads at the start of the next job instead of destroying a session
     * while a parse is in flight. */
    QString m_loadedModelDir;
    int m_loadedDeviceTier = -1;
};

} // namespace paintbrush

Q_DECLARE_METATYPE(paintbrush::HumanParseResult)

#endif // PB_HUMAN_PARSER_SERVICE_HPP
