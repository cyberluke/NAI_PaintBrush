/*****************************************************************************
 * pb_creative.hpp : NAI Paintbrush — Creative Play Pad / patch synthesis
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CREATIVE_HPP
#define PB_CREATIVE_HPP

#include "pb_common.hpp"
#include "pb_human_parser_service.hpp"
#include "pb_layer.hpp"
#include "pb_creative_types.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <memory>

namespace paintbrush {

class PaintbrushController;
class PaintbrushCreative;

/* One captured gesture frame. Milliseconds are relative to the recording
 * start so a loop is independent of when it is replayed. */
struct CreativeSample
{
    float x = 0.5f;
    float y = 0.5f;
    float energy = 0.0f;
    bool hold = false;
    qint64 tMs = 0;
};

/* Cooperative cancellation flag shared by the GUI-side commit handle and the
 * worker. std::atomic makes the cross-thread observation well-defined; the
 * job never touches this object's owner, the pad or the document. */
class CreativeCancelToken
{
public:
    void cancel() noexcept
    {
        m_cancelled.store(true, std::memory_order_relaxed);
    }
    bool isCancelled() const noexcept
    {
        return m_cancelled.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> m_cancelled{ false };
};

/* Result of one asynchronous commit build. Layers are already trimmed and
 * placed in canvas space, so publication is a single GUI-thread transaction. */
struct CreativeBuildResult
{
    QList<CanvasLayer> layers;
    QImage preview; /* bounded preview of the exact committed look */
    QString error;
    bool cancelled = false;
};

/* One event streamed from the commit job to the GUI thread. QFutureWatcher
 * delivers these in order on the thread that owns the watcher. */
struct CreativeCommitEvent
{
    enum Kind { Progress, Finished };
    Kind kind = Progress;
    quint64 revision = 0;
    int percent = 0;
    QImage preview;
    CreativeBuildResult result;
};

/* Image provider for the live stage. QML requests
 * image://pbcreative/live/<revision>; the engine keeps the latest frame. */
class CreativePreviewProvider : public QQuickImageProvider
{
public:
    explicit CreativePreviewProvider(PaintbrushCreative *engine);
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    PaintbrushCreative *m_engine = nullptr;
};

class PaintbrushCreative : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList patchList READ patchList CONSTANT FINAL)
    Q_PROPERTY(QVariantMap activePatch READ activePatch NOTIFY patchChanged)
    Q_PROPERTY(QString activePatchId READ activePatchId NOTIFY patchChanged)

    Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(bool chooserOpen READ chooserOpen NOTIFY patchChanged)
    Q_PROPERTY(bool hasSubject READ hasSubject NOTIFY subjectChanged)
    Q_PROPERTY(QString subjectLabel READ subjectLabel NOTIFY subjectChanged)

    Q_PROPERTY(double padX READ padX NOTIFY padChanged)
    Q_PROPERTY(double padY READ padY NOTIFY padChanged)
    Q_PROPERTY(double padEnergy READ padEnergy NOTIFY padChanged)
    Q_PROPERTY(bool holdActive READ holdActive NOTIFY padChanged)

    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool replaying READ replaying NOTIFY stateChanged)
    Q_PROPERTY(bool hasRecording READ hasRecording NOTIFY stateChanged)
    Q_PROPERTY(double recordSeconds READ recordSeconds NOTIFY stateChanged)
    Q_PROPERTY(double replayProgress READ replayProgress NOTIFY stateChanged)

    Q_PROPERTY(int previewRevision READ previewRevision NOTIFY previewChanged)
    Q_PROPERTY(int previewWidth READ previewWidth NOTIFY previewChanged)
    Q_PROPERTY(int previewHeight READ previewHeight NOTIFY previewChanged)

    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(int commitProgress READ commitProgress NOTIFY commitProgressChanged)
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY stateChanged)
    Q_PROPERTY(bool committed READ committed NOTIFY stateChanged)

    Q_PROPERTY(bool suggestionVisible READ suggestionVisible NOTIFY suggestionChanged)
    Q_PROPERTY(QString suggestionText READ suggestionText NOTIFY suggestionChanged)

    /* Fashion-aware material: when a FASHN parse is available, the material
     * is applied to the garment while skin, hair and accessories keep their
     * original pixels. Turning it off styles the whole subject. */
    Q_PROPERTY(bool fashionProtection READ fashionProtection
               WRITE setFashionProtection NOTIFY fashionProtectionChanged)
    Q_PROPERTY(QString humanParseStatus READ humanParseStatus
               NOTIFY humanParseStatusChanged)
    Q_PROPERTY(bool fashionParserInstalled READ fashionParserInstalled
               NOTIFY fashionParserInstallChanged)
    Q_PROPERTY(bool fashionParserCanInstall READ fashionParserCanInstall
               NOTIFY fashionParserInstallChanged)
    Q_PROPERTY(bool fashionParserInstalling READ fashionParserInstalling
               NOTIFY fashionParserInstallChanged)
    Q_PROPERTY(int fashionParserInstallProgress READ fashionParserInstallProgress
               NOTIFY fashionParserInstallChanged)
    Q_PROPERTY(QString fashionParserInstallStatus READ fashionParserInstallStatus
               NOTIFY fashionParserInstallChanged)

public:
    explicit PaintbrushCreative(PaintbrushController *controller,
                                QObject *parent = nullptr);
    ~PaintbrushCreative() override;

    QVariantList patchList() const;
    QVariantMap activePatch() const;
    QString activePatchId() const;

    bool isOpen() const { return m_open; }
    void setOpen(bool open);
    bool chooserOpen() const { return m_chooserOpen; }
    bool hasSubject() const { return !m_subject.isNull(); }
    QString subjectLabel() const { return m_subjectLabel; }

    double padX() const { return m_frame.x; }
    double padY() const { return m_frame.y; }
    double padEnergy() const { return m_frame.energy; }
    bool holdActive() const { return m_frame.hold; }

    bool recording() const { return m_recording; }
    bool replaying() const { return m_replayTimer.isActive(); }
    bool hasRecording() const { return !m_samples.isEmpty(); }
    double recordSeconds() const;
    double replayProgress() const;

    int previewRevision() const { return m_previewRevision; }
    int previewWidth() const { return m_preview.width(); }
    int previewHeight() const { return m_preview.height(); }

    QString status() const { return m_status; }
    bool busy() const { return m_busy; }
    int commitProgress() const { return m_commitPercent; }
    bool canCommit() const;
    bool committed() const { return m_committed; }

    bool suggestionVisible() const { return m_suggestionVisible; }
    QString suggestionText() const { return m_suggestionText; }

    bool fashionProtection() const { return m_fashionProtection; }
    void setFashionProtection(bool enabled);
    QString humanParseStatus() const { return m_humanParseStatus; }
    bool fashionParserInstalled() const;
    bool fashionParserCanInstall() const;
    bool fashionParserInstalling() const;
    int fashionParserInstallProgress() const;
    QString fashionParserInstallStatus() const;
    /* On-demand download: the controller's parser service runs the bundled
     * post-installer and reloads the runtime when it finishes. */
    Q_INVOKABLE void installFashionParser();

    /* Frame for the image provider. Safe from any thread. */
    QImage previewImage() const;

    Q_INVOKABLE void selectPatch(const QString &id);
    Q_INVOKABLE void openFromHarvest(int harvestId);
    Q_INVOKABLE void openForSelection();
    Q_INVOKABLE void close();

    Q_INVOKABLE void beginPad();
    Q_INVOKABLE void movePad(double x, double y, double energy, bool hold);
    Q_INVOKABLE void endPad();
    Q_INVOKABLE void setHold(bool hold);

    Q_INVOKABLE void toggleRecord();
    Q_INVOKABLE void toggleReplay();
    Q_INVOKABLE void reseed();
    Q_INVOKABLE void surprise();

    /* Start (or replace) a cancellable full-resolution render. Returns an
     * empty string when the job was accepted; the result arrives through
     * commitFinished() on the GUI thread and is published as one undo step.
     * A non-empty return means the request was rejected before any work. */
    Q_INVOKABLE QString commit();
    /* Cancel the in-flight render and discard its revision. Nothing is
     * published; the pad returns to the live preview. */
    Q_INVOKABLE void cancelCommit();
    /* Automation/self-test helper: run a local event loop until the in-flight
     * commit publishes. Returns "" on success, otherwise the exact error or a
     * timeout message. Never used by the product QML. */
    QString waitForCommit(int timeoutMs = 120000);
    /* Automation/self-test helper: wait for the current FASHN parse (if one
     * is in flight). Returns the parse status text; never used by product QML
     * and never blocks when the parser is unavailable or idle. */
    QString waitForHumanParse(int timeoutMs = 120000);
    Q_INVOKABLE void acceptSuggestion();
    Q_INVOKABLE void dismissSuggestion();
    /* Art QA aid: save each preview piece and its stage rect. Used by the
     * creative acceptance self-test; not part of the product surface. */
    Q_INVOKABLE bool dumpStagePieces(const QString &directory) const;

signals:
    void patchChanged();
    void openChanged();
    void subjectChanged();
    void padChanged();
    void previewChanged();
    void stateChanged();
    void suggestionChanged();
    /* The async commit reached a terminal state on the GUI thread. `error` is
     * empty when the layers were published, otherwise it says why nothing
     * changed. A cancellation is reported as an empty error; `committed()`
     * stays false and the status text says it was cancelled. */
    void commitFinished(const QString &error);
    void commitProgressChanged();
    void fashionProtectionChanged();
    void humanParseStatusChanged();
    void fashionParserInstallChanged();
    /* Emitted once per accepted parse when it finishes (ready or failed).
     * Used by the self-test wait helper; not needed by the pad. */
    void humanParseFinished(bool ok);

private slots:
    void onIdleTick();
    void onReplayTick();
    void onCommitResultReady(int index);
    void onCommitProgress(int percent);
    void onCommitFinished();
    void onHumanParseReady(const paintbrush::HumanParseResult &result,
                           quint64 revision, quint64 requestId);
    void onHumanParseFailed(quint64 requestId, const QString &error);

private:
    struct PatchDef
    {
        QString id;
        QString name;
        QString tagline;
        QString family;
        QString hint;
        QString suggestion;
        QColor accent;
        QColor secondary;
        QColor ink;
    };

    static const QVector<PatchDef> &catalog();
    const PatchDef *patchDef(const QString &id) const;
    QString defaultPatchId() const;
    void setStatus(const QString &text);

    bool adoptSubject(const QImage &cutout, const QRectF &canvasRect,
                      int sourceLayerId, const QString &label,
                      const QString &missingMessage);
    void clearSubject();
    /* FASHN semantic parse of the current subject. The parser is optional:
     * an unavailable runtime is reported in humanParseStatus and the pad
     * continues to style the whole subject. */
    void requestHumanParse();
    void clearHumanMasks();
    void setHumanParseStatus(const QString &text);
    bool usesFashionProtection() const;
    void rebuildPreview();
    void recompose();
    void startIdle();
    void stopIdle();

    /* One commit job's GUI-side context. The worker only receives the
     * immutable CreativeBuildInput copy plus the cancel token. */
    struct CommitContext
    {
        quint64 revision = 0;
        QString suggestion;
        QString desc;
        int sourceLayerId = -1;
        QSize canvasSize; /* guards against a document resize mid-render */
        QSize previewSize;
        std::shared_ptr<CreativeCancelToken> token;
    };

    bool beginCommit(QString *error);
    void cancelCommitInternal(bool notify);
    void publishCommitPreview(const QImage &preview);
    void publishCommitResult(const CreativeBuildResult &result);
    void applyCommitEvent(const CreativeCommitEvent &event);
    void consumeCommitResults(int uptoInclusive);
    void finishCommitJob();

    PaintbrushController *m_controller = nullptr;

    mutable QMutex m_previewMutex;
    QImage m_preview;
    QSize m_previewSize;
    int m_previewRevision = 0;

    QImage m_subject;       /* cutout at preview resolution */
    QImage m_subjectNative; /* cutout at source resolution */
    QRectF m_subjectCanvasRect;
    QString m_subjectLabel;
    int m_sourceLayerId = -1;

    /* Fashion semantics for the current subject. m_semanticProtection is
     * source-sized Grayscale8 where 255 keeps the original pixel. It belongs
     * to m_subjectRevision: a stale parse can never touch a newer subject. */
    quint64 m_subjectRevision = 0;
    QImage m_semanticProtection;
    QString m_humanParseStatus;
    bool m_fashionProtection = true;
    bool m_humanParsePending = false;
    quint64 m_humanParseRequest = 0;

    QString m_patchId;
    quint32 m_seed = 1;
    bool m_open = false;
    bool m_chooserOpen = false;
    bool m_busy = false;
    bool m_committed = false;

    /* Async commit job state. Everything below is GUI-thread only. The worker
     * receives a CreativeBuildInput copy and the cancel token; it never sees
     * these members, the pad or the document. */
    QThreadPool m_buildPool;
    QFutureWatcher<CreativeCommitEvent> *m_commitWatcher = nullptr;
    quint64 m_commitRevision = 0; /* monotonic; stale events can never publish */
    int m_commitPercent = 0;
    int m_consumedResults = 0;
    CommitContext m_commit;
    QString m_lastCommitError;

    CreativeFrame m_frame;
    QVector<CreativePiece> m_pieces;

    QTimer m_idleTimer;
    QTimer m_replayTimer;
    QElapsedTimer m_replayClock;
    QElapsedTimer m_recordClock;

    bool m_recording = false;
    QVector<CreativeSample> m_samples;

    QString m_status;
    QString m_suggestionText;
    bool m_suggestionVisible = false;
};

} /* namespace paintbrush */

#endif /* PB_CREATIVE_HPP */
