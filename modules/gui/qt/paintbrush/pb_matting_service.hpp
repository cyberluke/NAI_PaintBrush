/*****************************************************************************
 * pb_matting_service.hpp : NAI Paintbrush — asynchronous matte refinement
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_MATTING_SERVICE_HPP
#define PB_MATTING_SERVICE_HPP

#include "pb_common.hpp"
#include "pb_matte.hpp"

#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>
#include <deque>
#include <memory>
#include <thread>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

namespace paintbrush {

/* Asynchronous refinement around pb_matte + the optional neural runtime.
 *
 * Contract:
 *   - the deterministic stages ALWAYS run, with or without a model;
 *   - Neural mode additionally loads the shared ONNX Runtime session and
 *     replaces the alpha with the model's continuous matte;
 *   - a model failure NEVER masquerades as a successful neural result: the
 *     result carries the refined backend and the precise failure reason;
 *   - only the newest interactive request is executed (superseded jobs are
 *     dropped), and a stale completion is not emitted;
 *   - the GUI thread never blocks on inference.
 */
class MattingService : public QObject
{
    Q_OBJECT

public:
    explicit MattingService(QObject *parent = nullptr);
    ~MattingService() override;

    void configure(vlc_object_t *obj);

    bool neuralAvailable() const;
    QString unavailableReason() const;
    QString backendText() const;

    matte::Mode mode() const { return m_mode; }
    void setMode(matte::Mode mode);

    matte::Quality quality() const { return m_quality; }
    void setQuality(matte::Quality quality);

    matte::Parameters parameters() const;
    void setParameters(const matte::Parameters &parameters);

    QString modelDir() const { return m_modelDir; }
    void setModelDir(const QString &dir);

    /* 0 auto, 1 gpu required, 2 cpu */
    int deviceTier() const { return m_deviceTier; }
    void setDeviceTier(int tier);

    bool isBusy() const;
    QString statusText() const;

    /* Queue a refinement. Returns the request id (0 = invalid input). */
    quint64 request(const matte::Inputs &inputs,
                    const matte::FrameIdentity &identity,
                    bool diagnostics);
    void cancel(quint64 requestId);
    void cancelAll();
    void shutdown();

    /* Cached result for an identity + current parameters, if still valid. */
    bool cached(const matte::FrameIdentity &identity,
                matte::Result *result) const;

    static QRect subjectRoi(const QImage &mask, int expandPercent,
                            int extraPixels);

signals:
    void matteReady(const paintbrush::matte::Result &result,
                    const paintbrush::matte::FrameIdentity &identity,
                    quint64 requestId);
    void refineFailed(quint64 requestId, const QString &error);
    void availabilityChanged();
    void busyChanged(bool busy);
    void statusChanged(const QString &status);

private:
    struct Job
    {
        matte::Inputs inputs;
        matte::FrameIdentity identity;
        matte::Mode mode = matte::Mode::Refined;
        matte::Quality quality = matte::Quality::Balanced;
        matte::Parameters parameters;
        bool diagnostics = false;
        quint64 id = 0;
    };

    void workerLoop();
    void runJob(const Job &job);
    void setStatus(const QString &status);
    /* Status whose text follows the live interface language: the msgid is
     * stored and translated on read, arguments are applied afterwards. */
    void setStatusMsgid(const char *msgid, const QStringList &args = {});
    void setBusy(bool busy);
    bool ensureRuntime(QString *error);
    int presetSizeFor(matte::Quality quality) const;
    QString modelPathFor(int preset) const;
    void logInfo(const char *format, ...) const;

    vlc_object_t *m_obj = nullptr;
    QString m_modelDir;
    int m_deviceTier = 0;
    matte::Mode m_mode = matte::Mode::Neural;
    matte::Quality m_quality = matte::Quality::Balanced;
    matte::Parameters m_parameters;

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

    bool m_neuralProbed = false;
    bool m_neuralAvailable = false;
    QString m_unavailableReason;
    QString m_backendText;

    QHash<QString, matte::Result> m_cache;
    mutable QMutex m_cacheMutex;
};

} // namespace paintbrush

#endif // PB_MATTING_SERVICE_HPP
