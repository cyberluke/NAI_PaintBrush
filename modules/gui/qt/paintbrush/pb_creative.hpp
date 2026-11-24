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
#include "pb_layer.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

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

/* How one synthesized piece reacts to the pad. Offsets are normalized to the
 * stage size; opacity/scale/rotation are additive around the base value. */
struct CreativeMotion
{
    float opacityX = 0.0f;
    float opacityY = 0.0f;
    float opacityEnergy = 0.0f;
    float opacityHold = 0.0f;
    float dxX = 0.0f;
    float dyY = 0.0f;
    float dxEnergy = 0.0f;
    float dyEnergy = 0.0f;
    float scaleY = 0.0f;
    float scaleEnergy = 0.0f;
    float scaleHold = 0.0f;
    float rotationEnergy = 0.0f;
    float rotationHold = 0.0f;
};

/* One editable output piece. The same vector drives both the live preview
 * and the committed document layers. */
struct CreativePiece
{
    QString name;
    QString role; /* backdrop / echo / subject / rim / accent / finish */
    QImage image;
    QRectF rect; /* stage coordinates */
    QPointF pivot; /* motion pivot; null = rect center */
    int blendMode = BLEND_NORMAL;
    float opacity = 1.0f;
    CreativeMotion motion;
    QVariantMap metadata;
};

/* Live pad state. */
struct CreativeFrame
{
    float x = 0.5f;
    float y = 0.5f;
    float energy = 0.0f;
    bool hold = false;
    float time = 0.0f;
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
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY stateChanged)
    Q_PROPERTY(bool committed READ committed NOTIFY stateChanged)

    Q_PROPERTY(bool suggestionVisible READ suggestionVisible NOTIFY suggestionChanged)
    Q_PROPERTY(QString suggestionText READ suggestionText NOTIFY suggestionChanged)
    Q_PROPERTY(QString suggestionKind READ suggestionKind NOTIFY suggestionChanged)

    /* Live performance telemetry (real measurements from recompose/commit). */
    Q_PROPERTY(double previewP95Ms READ previewP95Ms NOTIFY metricsChanged)
    Q_PROPERTY(double lastCommitMs READ lastCommitMs NOTIFY metricsChanged)
    Q_PROPERTY(bool compareMode READ compareMode WRITE setCompare
                   NOTIFY compareChanged)

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
    bool canCommit() const;
    bool committed() const { return m_committed; }

    bool suggestionVisible() const { return m_suggestionVisible; }
    QString suggestionText() const { return m_suggestionText; }
    QString suggestionKind() const { return m_suggestionKind; }

    double previewP95Ms() const { return m_previewP95Ms; }
    double lastCommitMs() const { return m_lastCommitMs; }
    bool compareMode() const { return m_compare; }
    void setCompare(bool compare);

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

    Q_INVOKABLE QString commit();
    Q_INVOKABLE void acceptSuggestion();
    Q_INVOKABLE void dismissSuggestion();
    /* Resume a committed look from its layer metadata: re-adopts the original
     * cutout, restores patch/seed/pad and, on the next commit, replaces the
     * old group in the same one-undo transaction. */
    Q_INVOKABLE bool restoreFromCommittedLayer(int layerIndex);
    /* Apply the current patch/seed/pad to the currently selected cutout. */
    Q_INVOKABLE QString applyLookToSelection();
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
    void metricsChanged();
    void compareChanged();

private slots:
    void onIdleTick();
    void onReplayTick();

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
    void rebuildPreview();
    void recompose();
    void startIdle();
    void stopIdle();

    QVector<CreativePiece> buildPatch(const QString &patchId,
                                      const QImage &subject,
                                      const QRectF &subjectRect,
                                      const QSize &stage, quint32 seed) const;
    QImage composeStage(const QVector<CreativePiece> &pieces,
                        const QSize &stage, const QColor &background) const;
    QVector<CreativePiece> bakeFrame(QVector<CreativePiece> pieces,
                                    const QSize &stage) const;

    PaintbrushController *m_controller = nullptr;

    mutable QMutex m_previewMutex;
    QImage m_preview;
    QSize m_previewSize;
    QRectF m_previewSubjectRect;
    int m_previewRevision = 0;

    QImage m_subject;       /* cutout at preview resolution */
    QImage m_subjectNative; /* cutout at source resolution */
    QRectF m_subjectCanvasRect;
    QString m_subjectLabel;
    int m_sourceLayerId = -1;
    int m_sourceHarvestId = -1;

    QString m_patchId;
    quint32 m_seed = 1;
    bool m_open = false;
    bool m_chooserOpen = false;
    bool m_busy = false;
    bool m_committed = false;
    bool m_compare = false;
    QString m_resumeGroup;

    QVector<double> m_previewTimes;
    double m_previewP95Ms = 0.0;
    double m_lastCommitMs = 0.0;

    CreativeFrame m_frame;
    QVector<CreativePiece> m_pieces;

    QTimer m_idleTimer;
    QTimer m_replayTimer;
    QTimer m_recordUiTimer;
    QElapsedTimer m_replayClock;
    QElapsedTimer m_recordClock;

    bool m_recording = false;
    QVector<CreativeSample> m_samples;

    QString m_status;
    QString m_suggestionText;
    QString m_suggestionKind;
    bool m_suggestionVisible = false;
};

} /* namespace paintbrush */

#endif /* PB_CREATIVE_HPP */
