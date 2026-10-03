/*****************************************************************************
 * pb_controller.hpp : NAI Paintbrush — workspace controller
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CONTROLLER_HPP
#define PB_CONTROLLER_HPP

#include "pb_common.hpp"
#include "pb_document.hpp"
#include "pb_history.hpp"
#include "pb_segmentation_service.hpp"
#include "pb_tracking_service.hpp"
#include "pb_vision_service.hpp"
#include "pb_generation_service.hpp"
#include "pb_assistant_service.hpp"
#include "pb_companion_service.hpp"
#include "pb_project_store.hpp"
#include "pb_thumbnail_service.hpp"
#include "pb_lookup_service.hpp"

#include <QObject>
#include <QImage>
#include <QPointF>
#include <QUrl>
#include <QTimer>
#include <QVariantList>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

struct qt_intf_t;

class QQuickItem;

namespace paintbrush {

class PaintbrushCanvasItem;

/* Owns the Paintbrush document, services and the player bridge. Exposed to
 * QML as the `PaintbrushCtx` singleton in the VLC.Paintbrush module. */
class PaintbrushController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(PaintbrushDocument *document READ document CONSTANT FINAL)
    Q_PROPERTY(bool paintbrushEnabled READ paintbrushEnabled CONSTANT FINAL)
    Q_PROPERTY(bool welcomeVisible READ welcomeVisible NOTIFY welcomeChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged FINAL)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged FINAL)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged FINAL)
    Q_PROPERTY(bool hasOpenDocument READ hasOpenDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(bool segmentationAvailable READ segmentationAvailable NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString segmentationUnavailableReason READ segmentationUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool visionConfigured READ visionConfigured NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString visionUnavailableReason READ visionUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool generationConfigured READ generationConfigured NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString generationUnavailableReason READ generationUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool trackingAvailable READ trackingAvailable NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString trackingUnavailableReason READ trackingUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool companionAvailable READ companionAvailable NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString companionUnavailableReason READ companionUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool assistantConfigured READ assistantConfigured NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString assistantUnavailableReason READ assistantUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool thumbnailsGenerating READ thumbnailsGenerating NOTIFY thumbnailsChanged FINAL)
    Q_PROPERTY(int thumbnailCount READ thumbnailCount NOTIFY thumbnailsChanged FINAL)

    Q_PROPERTY(bool objectSelected READ objectSelected NOTIFY objectSelectionChanged FINAL)
    Q_PROPERTY(QString objectMaskBoundsText READ objectMaskBoundsText NOTIFY objectSelectionChanged FINAL)
    Q_PROPERTY(bool objectSelectBusy READ objectSelectBusy NOTIFY objectSelectionChanged FINAL)

    Q_PROPERTY(bool tracking READ isTracking NOTIFY trackingStateChanged FINAL)
    Q_PROPERTY(int trackingDone READ trackingDone NOTIFY trackingStateChanged FINAL)
    Q_PROPERTY(int trackingTotal READ trackingTotal NOTIFY trackingStateChanged FINAL)
    Q_PROPERTY(QString trackingStatus READ trackingStatus NOTIFY trackingStateChanged FINAL)
    Q_PROPERTY(QVariantList trackList READ trackList NOTIFY trackingStateChanged FINAL)
    Q_PROPERTY(bool trackingPaused READ trackingPaused NOTIFY trackingStateChanged FINAL)

    Q_PROPERTY(bool visionBusy READ visionBusy NOTIFY visionStateChanged FINAL)
    Q_PROPERTY(QString visionAnswer READ visionAnswer NOTIFY visionStateChanged FINAL)
    Q_PROPERTY(QString visionEvidence READ visionEvidence NOTIFY visionStateChanged FINAL)
    Q_PROPERTY(QString visionUncertainty READ visionUncertainty NOTIFY visionStateChanged FINAL)
    Q_PROPERTY(bool visionHasResult READ visionHasResult NOTIFY visionStateChanged FINAL)
    Q_PROPERTY(QString objectThumbnailDataUrl READ objectThumbnailDataUrl NOTIFY objectSelectionChanged FINAL)

    Q_PROPERTY(bool generationBusy READ generationBusy NOTIFY generationStateChanged FINAL)

    Q_PROPERTY(int companionMode READ companionMode NOTIFY companionStateChanged FINAL)
    Q_PROPERTY(bool companionThinking READ companionThinking NOTIFY companionStateChanged FINAL)
    Q_PROPERTY(QVariantList assistantMessages READ assistantMessages NOTIFY companionStateChanged FINAL)
    Q_PROPERTY(int companionTalkLevel READ companionTalkLevel WRITE setCompanionTalkLevel NOTIFY companionStateChanged FINAL)

    Q_PROPERTY(QString currentMediaTitle READ currentMediaTitle NOTIFY mediaStateChanged FINAL)
    Q_PROPERTY(QString currentMediaMRL READ currentMediaMRL NOTIFY mediaStateChanged FINAL)
    Q_PROPERTY(qint64 playerTimeMs READ playerTimeMs NOTIFY mediaStateChanged FINAL)
    Q_PROPERTY(qint64 playerLengthMs READ playerLengthMs NOTIFY mediaStateChanged FINAL)
    Q_PROPERTY(bool playerHasVideo READ playerHasVideo NOTIFY mediaStateChanged FINAL)
    Q_PROPERTY(QString diagnosticsText READ diagnosticsText NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(bool lookupAvailable READ lookupAvailable NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString lookupUnavailableReason READ lookupUnavailableReason NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QVariantList lookupMatches READ lookupMatches NOTIFY lookupChanged FINAL)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged FINAL)
    Q_PROPERTY(QVariantList activeJobs READ activeJobs NOTIFY activeJobsChanged FINAL)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryStateChanged FINAL)
    Q_PROPERTY(QString recoveryDescription READ recoveryDescription NOTIFY recoveryStateChanged FINAL)

public:
    explicit PaintbrushController(qt_intf_t *intf, QObject *parent = nullptr);
    ~PaintbrushController() override;

    qt_intf_t *intf() const { return m_intf; }
    PaintbrushDocument *document() { return &m_document; }
    bool paintbrushEnabled() const;
    bool welcomeVisible() const { return m_welcomeVisible; }
    QString projectName() const { return m_projectName; }

    /* -- QML canvas wiring ---------------------------------------------- */
    Q_INVOKABLE void setCanvasItem(QQuickItem *item);
    PaintbrushCanvasItem *canvas() const { return m_canvas; }

    /* -- document --------------------------------------------------------- */
    Q_INVOKABLE void newBlankCanvas(int width, int height);
    Q_INVOKABLE void openImageFile(const QUrl &url);
    Q_INVOKABLE void openImageData(const QImage &image, const QString &name);
    Q_INVOKABLE void openVideoFile(const QUrl &url);
    Q_INVOKABLE void captureVideoFrame();
    Q_INVOKABLE void pasteImageFromClipboard();
    Q_INVOKABLE void saveProject(const QUrl &url);
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void exportImage(const QUrl &url, const QString &format);
    Q_INVOKABLE void exportImagePreset(const QUrl &url, int preset);
    Q_INVOKABLE void exportObjectPng(const QUrl &url);
    Q_INVOKABLE void exportMaskPng(const QUrl &url);
    Q_INVOKABLE void relinkMedia(const QUrl &url);
    Q_PROPERTY(QString projectInfoText READ projectInfoText NOTIFY documentChanged FINAL)
    Q_INVOKABLE void autosaveNow();

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void openRecentProject(int index);
    QVariantList recentProjects() const;
    QVariantList activeJobs() const;
    Q_INVOKABLE void cancelActiveJob(const QString &jobId);

    /* -- layers ----------------------------------------------------------- */
    Q_INVOKABLE void newLayer();
    Q_INVOKABLE void deleteLayer(int index);
    Q_INVOKABLE void duplicateLayer(int index);
    Q_INVOKABLE void moveLayer(int index, int delta);
    Q_INVOKABLE void setLayerVisible(int index, bool visible);
    Q_INVOKABLE void setCurrentLayer(int index);
    /* Image-layer transforms (scale/rotation/pivot at content center),
     * one undoable operation each, persisted with the project. */
    Q_INVOKABLE void transformLayer(int index, double scale, double rotation);
    Q_INVOKABLE void moveImageLayer(int index, const QPointF &position);

    /* -- object selection ------------------------------------------------ */
    Q_INVOKABLE void refineObjectByText(const QString &textPrompt);
    Q_INVOKABLE void cutOutObject();
    Q_INVOKABLE void addObjectToCanvas();
    Q_INVOKABLE void extractObject();
    /* Selection edge modes for the object mask (false-positive-resistant).
     * Hard re-binarizes at the 128 threshold; Soft feathers the boundary
     * band into a genuine alpha edge. Hair/fine-edge is NOT provided because
     * no real semantic hair-matting algorithm exists in-tree (per Phase 3). */
    Q_INVOKABLE void setObjectMaskEdgeHard();
    Q_INVOKABLE void setObjectMaskEdgeSoft();
    Q_PROPERTY(bool objectMaskSoftEdge READ objectMaskSoftEdge NOTIFY objectMaskEdgeChanged FINAL)
    bool objectMaskSoftEdge() const { return m_objectMaskSoftEdge; }
    Q_INVOKABLE void askAboutObject(const QString &question);
    Q_INVOKABLE void askAboutSelection(const QString &question);
    Q_INVOKABLE void askAboutObjectFollowUp(const QString &question);
    Q_INVOKABLE void findProduct();
    Q_INVOKABLE void copyVisionAnswer();
    Q_INVOKABLE void addVisionAnswerAsText();
    Q_INVOKABLE void clearVisionResult();
    Q_INVOKABLE void trackObject(int direction, int frames);
    Q_INVOKABLE void cancelTracking();
    Q_INVOKABLE void pauseResumeTracking();
    Q_INVOKABLE void deleteTrack(int trackId);
    Q_INVOKABLE void seekToTrackKeyframe(int trackId, int keyframeIndex);
    bool trackingPaused() const;
    QVariantList trackList() const;
    /* Export a tracked object as a transparent animated sticker: seeks the
     * player to each keyframe, applies the keyframe mask, trims to content
     * and writes a transparent PNG sequence into `dir`. Returns a status
     * string (may be a path prefix on success). */
    Q_INVOKABLE QString exportTrackedSticker(int trackId, const QUrl &dir);
    Q_INVOKABLE void makeSticker();

    /* Animated sticker editor: preview keyframes of a track, configure
     * trim/outline/shadow/scale/speed, place a cutout on the canvas, or
     * export the trimmed sequence. */
    Q_INVOKABLE int stickerEditorTrackCount() const { return m_document.trackCount(); }
    Q_INVOKABLE int stickerEditorKeyframeCount(int trackId) const;
    Q_INVOKABLE QString stickerEditorFrameDataUrl(int trackId, int frameIndex) const;
    Q_INVOKABLE QString stickerEditorPreviewDataUrl(int trackId, int frameIndex,
                                                    int outline, const QString &outlineColor,
                                                    bool shadow, double scale) const;
    Q_INVOKABLE QString exportTrackedStickerOptions(int trackId, const QUrl &dir,
                                                    int startFrame, int endFrame,
                                                    int outline, const QString &outlineColor,
                                                    bool shadow, double scale, int speed);
    Q_INVOKABLE void placeStickerFrame(int trackId, int frameIndex, int outline,
                                       const QString &outlineColor, bool shadow,
                                       double scale);
    Q_INVOKABLE void removeObject(const QString &prompt);
    Q_INVOKABLE void generativeFill(const QString &prompt);
    Q_INVOKABLE void replaceBackground(const QString &prompt);
    Q_INVOKABLE void cancelGeneration();
    /* Auto Collage: arrange two or more selected raster/cutout layers into
     * a layout preset without flattening. One undoable operation. */
    Q_INVOKABLE void autoCollage(int preset); /* 0 free, 1 grid, 2 editorial,
                                                 3 scrapbook, 4 poster, 5 banner */

    /* -- companion --------------------------------------------------------- */
    Q_INVOKABLE void companionNotifyEvent(const QString &event);
    Q_INVOKABLE void companionAsk(const QString &question);
    Q_INVOKABLE void setCompanionMode(int mode);
    Q_INVOKABLE void companionReset();
    Q_INVOKABLE void companionClearConversation();
    int companionTalkLevel() const;
    void setCompanionTalkLevel(int level);

    /* -- media drawer ------------------------------------------------------ */
    Q_INVOKABLE void playerFrameNext();
    Q_INVOKABLE void playerFramePrev();
    Q_INVOKABLE void playerPlayPause();
    Q_INVOKABLE void playerSeek(qint64 timeMs);
    Q_INVOKABLE void playerSeekBegin();
    Q_INVOKABLE void playerSeekEnd();
    Q_INVOKABLE void playerStop();

    /* -- helpers ----------------------------------------------------------- */
    Q_INVOKABLE QImage flattenedPreview() const;
    Q_INVOKABLE QString fileDialogFilter() const;

    bool canUndo() const { return m_undoStack.canUndo(); }
    bool canRedo() const { return m_undoStack.canRedo(); }
    bool hasOpenDocument() const { return m_document.layerCount() > 0; }
    QString statusMessage() const { return m_statusMessage; }

    bool segmentationAvailable() const { return m_segmentation.isConfigured(); }
    QString segmentationUnavailableReason() const { return m_segmentation.unavailableReason(); }
    bool visionConfigured() const { return m_vision.isConfigured(); }
    QString visionUnavailableReason() const { return m_vision.unavailableReason(); }
    bool generationConfigured() const { return m_generation.isConfigured(); }
    QString generationUnavailableReason() const { return m_generation.unavailableReason(); }
    bool trackingAvailable() const { return m_tracking.isConfigured(); }
    QString trackingUnavailableReason() const { return m_tracking.unavailableReason(); }
    bool companionAvailable() const { return m_companion.mode() != CompanionService::MODE_OFF; }
    QString companionUnavailableReason() const { return m_companionModeOffReason; }
    bool assistantConfigured() const { return m_assistant.isConfigured(); }
    QString assistantUnavailableReason() const { return m_assistant.unavailableReason(); }
    bool lookupAvailable() const { return m_lookup.isConfigured(); }
    QString lookupUnavailableReason() const { return m_lookup.unavailableReason(); }
    QVariantList lookupMatches() const { return m_lookupMatches; }
    QString objectThumbnailDataUrl() const;
    bool thumbnailsGenerating() const { return m_thumbnails.isGenerating(); }
    int thumbnailCount() const { return m_thumbnails.count(); }
    /* QML: list of {timeMs, hasImage, failed} for the current batch */
    QVariantList thumbnailList() const;
    Q_INVOKABLE QString thumbnailDataUrl(int index) const;
    Q_INVOKABLE qint64 thumbnailTimeMs(int index) const;
    Q_INVOKABLE void generateThumbnails();
    Q_INVOKABLE void cancelThumbnails();
    /* Diagnostics support actions */
    Q_INVOKABLE QString copyDiagnostics();
    Q_INVOKABLE void clearAiCache();
    Q_INVOKABLE void unloadAiModels();
    Q_INVOKABLE QString autosavePath() const;
    QString projectInfoText() const;
    /* Crash recovery */
    Q_INVOKABLE void checkForRecovery();
    Q_INVOKABLE void restoreRecovery();
    Q_INVOKABLE void discardRecovery();
    bool recoveryAvailable() const { return m_recoveryAvailable; }
    QString recoveryDescription() const { return m_recoveryDescription; }

    bool objectSelected() const { return m_objectSelected; }
    QString objectMaskBoundsText() const { return m_objectMaskBoundsText; }
    bool objectSelectBusy() const { return m_objectSelectBusy; }

    bool isTracking() const { return m_tracking.isTracking(); }
    int trackingDone() const { return m_tracking.progressDone(); }
    int trackingTotal() const { return m_tracking.progressTotal(); }
    QString trackingStatus() const { return m_tracking.statusText(); }

    bool visionBusy() const { return m_vision.isBusy(); }
    QString visionAnswer() const { return m_visionAnswer; }
    QString visionEvidence() const { return m_visionEvidence; }
    QString visionUncertainty() const { return m_visionUncertainty; }
    bool visionHasResult() const { return m_visionHasResult; }

    bool generationBusy() const { return m_generation.isBusy(); }

    int companionMode() const { return m_companion.mode(); }
    bool companionThinking() const { return m_companion.isThinking(); }
    QVariantList assistantMessages() const { return m_assistantMessages; }

    QString currentMediaTitle() const;
    QString currentMediaMRL() const;
    qint64 playerTimeMs() const;
    qint64 playerLengthMs() const;
    bool playerHasVideo() const;
    QString diagnosticsText() const;

signals:
    void canUndoChanged();
    void canRedoChanged();
    void statusMessageChanged();
    void projectChanged();
    void documentChanged();
    void serviceAvailabilityChanged();
    void objectSelectionChanged();
    void objectMaskEdgeChanged();
    void trackingStateChanged();
    void visionStateChanged();
    void generationStateChanged();
    void companionStateChanged();
    void mediaStateChanged();
    void thumbnailsChanged();
    void recoveryStateChanged();
    void recentProjectsChanged();
    void activeJobsChanged();
    void lookupChanged();
    void assistantMessageAppended(const QString &role, const QString &text);
    void welcomeChanged();

private slots:
    void onCanvasObjectClick(QPointF canvasPos, bool positive, QPointF itemPos);
    void onMaskRefineStarted(const QImage &mask);
    void onObjectMaskEdited(const QImage &mask);
    void onMaskRestored(const QImage &mask);
    void onSegmentationMaskReady(const QImage &mask, double score, const QString &method);
    void onSegmentationError(const QString &message);
    void onSegmentationAvailability(bool available, const QString &reason);
    void onVisionResponse(const QString &conversationId, const QString &answer,
                          const QString &evidence, const QString &uncertainty,
                          const QString &raw);
    void onVisionError(const QString &conversationId, const QString &message);
    void onGenerationResult(const QString &requestId, const QImage &result);
    void onGenerationError(const QString &requestId, const QString &message);
    void onTrackKeyframe(int frameIndex, qint64 timeMs, const QImage &mask,
                         float confidence);
    void onTrackFinished(bool complete, bool cancelled, const QString &error);
    void onCompanionMessage(const QString &text, const QString &role);
    void onCanvasStrokeCommitted(const QRect &region);
    void onCanvasModified();
    void onLayerDrop(const QUrl &url, const QPointF &pos);
    void onMediaTimer();

private:
    void configureServices();
    void setStatus(const QString &message);
    void addImageLayer(const QImage &image, const QPointF &position,
                       const QString &name, int type);
    void addResultLayer(const QImage &image, const QString &name);
    void updateObjectSelection();
    void appendAssistantMessage(const QString &role, const QString &text);
    void pushRecentProject(const QString &path);
    void buildTrackObject();
    void triggerCompanionEvent(const QString &event);
    QImage objectCrop() const;
    QImage selectionCrop() const;
    QImage currentAiSource() const;
    QImage pristineSourceFrame() const;
    QVector<QPointF> positivePointsForTracking() const;
    void startAutosave();
    /* Total bytes held by tracked-object masks and a bounded-eviction pass
     * used while a track grows (memory hardening). Returns bytes after
     * eviction. */
    qint64 trackedMaskBytes() const;
    qint64 enforceTrackMaskMemoryBudget();

    qt_intf_t *m_intf = nullptr;
    PaintbrushDocument m_document;
    UndoStack m_undoStack;
    PaintbrushCanvasItem *m_canvas = nullptr;

    SegmentationService m_segmentation;
    TrackingService m_tracking;
    VisionService m_vision;
    GenerationService m_generation;
    AssistantService m_assistant;
    CompanionService m_companion;
    ProjectStore m_projectStore;
    ThumbnailService m_thumbnails;
    LookupService m_lookup;

    QString m_statusMessage;
    QString m_projectName;
    bool m_objectSelected = false;
    QString m_objectMaskBoundsText;
    bool m_objectSelectBusy = false;
    QImage m_objectMask;
    QImage m_maskUndoBefore;   /* mask captured when a refine stroke arms */
    bool m_objectMaskSoftEdge = false;

    QString m_visionConversationId;
    QString m_visionAnswer;
    QString m_visionEvidence;
    QString m_visionUncertainty;
    bool m_visionHasResult = false;
    QString m_generationRequestId;

    QVector<ObjectTrack> m_pendingTrack;
    int m_trackIdCounter = 1;

    /* object selection click session */
    QVector<QPointF> m_posPoints;
    QVector<QPointF> m_negPoints;

    QVariantList m_assistantMessages;
    QVariantList m_lookupMatches;
    QString m_companionModeOffReason;
    /* recovery autosave state */
    bool m_recoveryAvailable = false;
    QString m_recoveryDescription;
    QString m_recoveryPath;
    /* The workspace starts ready to draw: a blank canvas is created in the
     * constructor and the first-run overlay is hidden, so the tool rail and
     * canvas accept input immediately. */
    bool m_welcomeVisible = false;

    QTimer m_mediaTimer;
    QTimer m_autosaveTimer;
    /* MRL for which the thumbnail timeline was auto-started, so the timer
     * does not restart generation on every tick for the same media */
    QString m_thumbnailsAutoMedia;
};

} // namespace paintbrush

#endif // PB_CONTROLLER_HPP