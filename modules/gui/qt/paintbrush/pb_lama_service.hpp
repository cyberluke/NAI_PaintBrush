/*****************************************************************************
 * pb_lama_service.hpp : NAI Paintbrush — native LaMa inpainting service
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_LAMA_SERVICE_HPP
#define PB_LAMA_SERVICE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QWaitCondition>
#include <QString>

#include <atomic>
#include <deque>
#include <thread>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

namespace paintbrush {

/* Asynchronous native LaMa inpainting (pb_lama_cuda.dll + ONNX Runtime).
 *
 * Contract:
 *   - the model is probed (DLL + closure + pinned graph + device) WITHOUT
 *     loading the 208 MB graph; load happens on the first request;
 *   - one persistent session stays resident; the GUI thread never blocks on
 *     inference;
 *   - only the newest interactive request is executed (superseded jobs are
 *     dropped), and a stale completion is not emitted;
 *   - a failure is reported truthfully — never as a successful pixel result
 *     and never by silently sending the job to a cloud provider;
 *   - the pinned model SHA-256 is verified before the graph is loaded.
 *
 * Fixed 512 export: this is context-ROI inpainting, not native-resolution
 * synthesis.
 */
class LamaService : public QObject
{
    Q_OBJECT

public:
    explicit LamaService(QObject *parent = nullptr);
    ~LamaService() override;

    void configure(vlc_object_t *obj);

    /* Probe-only usability (no model load). */
    bool available() const;
    QString unavailableReason() const;
    QString backendText() const;

    QString modelPath() const;
    void setModelPath(const QString &path);

    /* 0 auto, 1 gpu required, 2 cpu */
    int deviceTier() const { return m_deviceTier; }
    void setDeviceTier(int tier);

    bool isBusy() const;
    QString statusText() const;

    /* Queue an inpaint. Returns the request id (0 = invalid input). */
    quint64 request(const QImage &rgb, const QImage &mask, const QString &tag);
    void cancel(quint64 requestId);
    void cancelAll();
    void shutdown();

signals:
    void inpaintReady(const QString &tag, const QImage &result);
    void inpaintFailed(const QString &tag, const QString &error);
    void availabilityChanged();
    void busyChanged(bool busy);
    void statusChanged(const QString &status);

private:
    struct Job
    {
        QImage rgb;
        QImage mask;
        QString tag;
        quint64 id = 0;
    };

    void workerLoop();
    void runJob(const Job &job);
    void refreshAvailability(bool emitSignal);
    bool ensureRuntime(QString *error);
    QString expectedSha256() const;
    void setStatus(const QString &status);
    void setStatusMsgid(const char *msgid, const QStringList &args = {});
    void setBusy(bool busy);
    void logInfo(const char *format, ...) const;

    vlc_object_t *m_obj = nullptr;
    QString m_modelPath;
    QString m_expectedSha256;
    int m_deviceTier = 0;
    int m_inputSize = 512;

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

    bool m_probed = false;
    bool m_available = false;
    QString m_unavailableReason;
    QString m_backendText;
    QString m_loadFailure;
};

} // namespace paintbrush

#endif // PB_LAMA_SERVICE_HPP
