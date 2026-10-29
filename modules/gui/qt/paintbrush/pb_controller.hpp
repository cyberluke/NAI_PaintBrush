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
#include "pb_layer_model.hpp"
#include "pb_history.hpp"
#include "pb_sticker.hpp"
#include "pb_segmentation_service.hpp"
#include "pb_tracking_service.hpp"
#include "pb_vision_service.hpp"
#include "pb_generation_service.hpp"
#include "pb_assistant_service.hpp"
#include "pb_companion_service.hpp"
#include "pb_project_store.hpp"
#include "pb_thumbnail_service.hpp"
#include "pb_lookup_service.hpp"
#include "pb_frame_capture.hpp"
#include "pb_harvest.hpp"
#include "pb_boost.hpp"

#include <QObject>
#include <QImage>
#include <QPointF>
#include <QTransform>
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

/* Ephemeral context for a LIVE harvest frame: the exact decoded frame at a
 * given player PTS plus the transforms that map between the on-canvas video
 * viewport and the decoded pixels. Unlike the document's mutable
 * `sourceFrame` (a captured reference/composition provenance), this is the
 * player cursor — re-captured fresh on every SAM seed click so a later PTS
 * can never reuse a stale earlier frame.
 *
 * Mapping is standard letterbox aspect-fit (the same geometry the VideoSurface
 * displays): the full viewport is cleared to transparent and the frame is
 * drawn scaled into its content rect leaving letterbox margins. */
struct LiveFrameContext
{
    FrameRef ref;            /* current MRL + verified PTS               */
    QImage frame;            /* decoded video pixels (native size)       */
    QSize viewportSize;      /* the QML canvas-area size the video fills  */
    QRectF contentRect;      /* aspect-fit rect inside the viewport      */
    QTransform itemToFrame;  /* viewport QML point -> frame pixel         */
    QTransform frameToItem;  /* frame pixel -> viewport QML point         */

    bool valid() const { return !frame.isNull() && !contentRect.isEmpty(); }

    /* Remap a viewport-space point into frame-pixel space (returns NaN on
     * failure so callers can reject outside-the-video clicks). */
    QPointF itemToFramePoint(const QPointF &p) const
    {
        if (!valid())
            return { qQNaN(), qQNaN() };
        return itemToFrame.map(p);
    }
    /* Remap a frame-pixel point into viewport space. */
    QPointF frameToItemPoint(const QPointF &p) const
    {
        if (!valid())
            return { qQNaN(), qQNaN() };
        return frameToItem.map(p);
    }
};

/* Owns the Paintbrush document, services and the player bridge. Exposed to
 * QML as the `PaintbrushCtx` singleton in the VLC.Paintbrush module. */
class PaintbrushController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(PaintbrushDocument *document READ document CONSTANT FINAL)
    Q_PROPERTY(PaintbrushLayerModel *layerModel READ layerModel CONSTANT FINAL)
    /* Capability-honest status strings (S5/A3/A4): derived from the
     * services' CapabilityReport / configured state — never a hardcoded
     * "ready". Used by the diagnostics surface in the UI. */
    Q_PROPERTY(QString generationStatusText READ generationStatusText NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString visionStatusText READ visionStatusText NOTIFY serviceAvailabilityChanged FINAL)
    Q_PROPERTY(QString segmentationStatusText READ segmentationStatusText NOTIFY serviceAvailabilityChanged FINAL)
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
    /* The tracked object currently selected for sticker editing (A15). The
     * media drawer's sticker editor binds to this instead of a hardcoded 0.
     * -1 = no track selected (no tracks exist). */
    Q_PROPERTY(int selectedTrackId READ selectedTrackId WRITE setSelectedTrackId NOTIFY selectedTrackChanged FINAL)

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

    /* -- Paintbrush 2.0: Harvest -> Compose -> Boost ------------------------ */
    Q_PROPERTY(paintbrush::HarvestTrayModel *harvestTray READ harvestTray CONSTANT FINAL)
    Q_PROPERTY(int harvestCount READ harvestCount NOTIFY harvestChanged FINAL)
    Q_PROPERTY(bool canKeep READ canKeep NOTIFY objectSelectionChanged FINAL)
    Q_PROPERTY(bool boostBusy READ boostBusy NOTIFY boostStateChanged FINAL)
    Q_PROPERTY(QString boostSummary READ boostSummary NOTIFY boostStateChanged FINAL)
    Q_PROPERTY(bool canBoost READ canBoost NOTIFY documentChanged FINAL)
    Q_PROPERTY(QVariantList moodList READ moodList CONSTANT FINAL)
    Q_PROPERTY(QVariantList paperList READ paperList CONSTANT FINAL)
    Q_PROPERTY(QVariantList typographyList READ typographyList CONSTANT FINAL)
    Q_PROPERTY(QStringList badgeList READ badgeList CONSTANT FINAL)
    Q_PROPERTY(QString currentMoodId READ currentMoodId NOTIFY artStateChanged FINAL)
    Q_PROPERTY(QString currentPaperId READ currentPaperId NOTIFY artStateChanged FINAL)
    Q_PROPERTY(QVariantList moodPalette READ moodPalette NOTIFY artStateChanged FINAL)
    Q_PROPERTY(QString undoText READ undoText NOTIFY canUndoChanged FINAL)

public:
    QString undoText() const { return m_undoStack.undoText(); }
    /* -- harvest (spec §5) ---------------------------------------------- */
    HarvestTrayModel *harvestTray() { return m_document.harvest(); }
    int harvestCount() const { return m_document.harvest()->count(); }
    bool canKeep() const { return m_objectSelected && !m_objectMask.isNull(); }
    /* Keep: the current object selection becomes an immutable HarvestItem in
     * the tray (exact frame + mask). Never clutters the canvas. */
    Q_INVOKABLE void keepSelection();
    /* Place a tray item on the canvas as a draggable sticker layer. `canvasPos`
     * is the drop point (content center); a null point centers it. */
    Q_INVOKABLE void placeHarvest(int harvestId, const QPointF &canvasPos);
    Q_INVOKABLE void removeHarvest(int harvestId);
    Q_INVOKABLE void toggleHarvestFavorite(int harvestId);
    /* Seek the player to the moment this harvest came from. */
    Q_INVOKABLE void seekToHarvest(int harvestId);

    /* -- living harvest (stage 3) ---------------------------------------- */
    /* Rig a tray item into a PuppetAsset (rig once, auto source-motion
     * window). Non-destructive: the HarvestItem itself is untouched and the
     * asset joins the undo stack. Returns the asset id (0 = not riggable). */
    Q_INVOKABLE int rigHarvest(int harvestId);
    /* Attach a rigged asset to a layer with an amplitude in 0..1
     * (photo regime: 0.35 subtle, 0.65 normal, 1.0 strong but photo-safe).
     * One undo restores the static figure. */
    Q_INVOKABLE void setLayerPuppet(int index, int assetId, double amplitude);
    Q_INVOKABLE void removeLayerPuppet(int index);
    Q_INVOKABLE bool layerHasPuppet(int index) const;
    Q_INVOKABLE double layerPuppetAmplitude(int index) const;
    Q_INVOKABLE int layerPuppetAssetId(int index) const;
    /* Number of rigged assets in the store (UI enables the Motion group). */
    Q_INVOKABLE int riggedAssetCount() const;
    /* First rigged asset id (UI falls back to it when the layer has none). */
    Q_INVOKABLE int firstRiggedAssetId() const;
    /* 3-second living poster: PNG frame sequence (canonical time grid,
     * deterministic for a fixed document). */
    Q_INVOKABLE QString exportLivingPoster(const QUrl &dir, int durationMs,
                                           int fps, bool paper);
    /* Living Showroom: mux the canonical frames into a REAL video file.
     * format: "mp4" (H.264, paper) or "webm" (VP9, transparent alpha).
     * Frames land next to the output file and stay (canonical
     * intermediate). Returns an error message or empty. */
    Q_INVOKABLE QString exportLivingVideo(const QUrl &dir, int durationMs, int fps,
                                          const QString &format, const QUrl &outFile);
    /* True only when the mux pipeline really works on this machine
     * (ffmpeg + encoder present) — the UI never offers a fake export. */
    Q_INVOKABLE bool canExportLivingVideo(const QString &format) const;

    /* -- boost (spec §24-§30) -------------------------------------------- */
    bool boostBusy() const { return m_boostBusy; }
    QString boostSummary() const { return m_boostSummary; }
    bool canBoost() const;
    /* Teen Boost: one grouped, reversible art-direction pass. Deterministic
     * structure first; a real bridge pose is decoded + segmented when the
     * analysis finds a sparse gap and SAM is available. */
    Q_INVOKABLE void boost();
    Q_INVOKABLE void cancelBoost();

    /* -- art direction (spec §8, §21-§23) --------------------------------- */
    QVariantList moodList() const;
    QVariantList paperList() const;
    QVariantList typographyList() const;
    QStringList badgeList() const;
    QString currentMoodId() const { return m_document.moodId(); }
    QString currentPaperId() const { return m_document.paperStockId(); }
    QVariantList moodPalette() const;
    Q_INVOKABLE void applyMood(const QString &moodId);
    Q_INVOKABLE void setPaper(const QString &paperId);
    /* Layer finish + role (non-destructive, one typed command each). */
    Q_INVOKABLE void setLayerRole(int index, const QString &role);
    Q_INVOKABLE QString layerRole(int index) const;
    Q_INVOKABLE void addLayerShadow(int index, double blur, double opacity);
    Q_INVOKABLE void addLayerGlow(int index, const QString &color, double radius);
    Q_INVOKABLE void addLayerOutline(int index, const QString &color, double width);
    Q_INVOKABLE void clearLayerEffects(int index);
    Q_INVOKABLE int layerEffectCount(int index) const;
    /* Typography + badges as new layers (text stays editable in metadata). */
    Q_INVOKABLE void addStyledText(const QString &text, const QString &styleId,
                                   double sizePx, const QString &color);
    /* Editable typography (spec §57): replace the text, role, size, tracking
     * and color of an existing text layer. The raster is re-rendered from the
     * treatment (never a bitmap stretch) and the change is one typed undo
     * step. An empty text deletes nothing — it renders empty glyphs; callers
     * use deleteLayer to remove a text layer. `tracking` is the em-fraction
     * override; pass -99 to use the style role's tracking. */
    Q_INVOKABLE void editTextLayer(int index, const QString &text,
                                   const QString &styleId, double sizePx,
                                   const QString &color, double tracking);
    /* Current treatment of a text layer for the inline editor: the QVariantMap
     * of TextTreatment, or empty for a non-text layer. */
    Q_INVOKABLE QVariantMap textTreatmentFor(int index) const;
    Q_INVOKABLE void addBadge(const QString &kind, const QString &color, double size);
    Q_INVOKABLE QString badgePreviewDataUrl(const QString &kind, const QString &color) const;
    Q_INVOKABLE QString paperPreviewDataUrl(const QString &paperId) const;

signals:
    void harvestChanged();
    void boostStateChanged();
    void artStateChanged();

private:
    void finishBoost(const QString &summary);
    void boostInsertBridge(const QImage &mask, double score);
    void applyMoodToCanvasTools();

    bool m_boostBusy = false;
    QString m_boostSummary;
    /* in-flight boost transaction (bridge pose waits for segmentation) */
    std::unique_ptr<BoostTransaction> m_boostTx;
    BoostAnalysis m_boostAnalysis;
    BoostOptions m_boostOptions;
    QVector<BridgeCandidate> m_boostCandidates;
    int m_boostBestCandidate = -1;
    bool m_boostAwaitingMask = false;
    QImage m_boostSavedMask;        /* user's object mask, restored after boost */
    QTimer m_boostTimeout;

public:
    explicit PaintbrushController(qt_intf_t *intf, QObject *parent = nullptr);
    ~PaintbrushController() override;

    qt_intf_t *intf() const { return m_intf; }
    PaintbrushDocument *document() { return &m_document; }
    PaintbrushLayerModel *layerModel() const { return m_layerModel; }
    bool paintbrushEnabled() const;
    bool welcomeVisible() const { return m_welcomeVisible; }
    QString projectName() const { return m_projectName; }

    /* -- QML canvas wiring ---------------------------------------------- */
    Q_INVOKABLE void setCanvasItem(QQuickItem *item);
    PaintbrushCanvasItem *canvas() const { return m_canvas; }

    /* -- Living Showroom: live canvas preview -------------------------- */
    /* The AnimationClock ticks 30 Hz (10 Hz while a stroke is active) but
     * ONLY while a visible living layer exists and the canvas is on screen;
     * every tick advances the canonical time and recomposites the canvas
     * through DocumentRenderer::composite(doc, t). Static layers are never
     * re-rendered differently — they are simply part of the same composite.
     * When the clock stops, the canvas rests at t=0 == the static poster. */
    Q_INVOKABLE void setShowroomPreviewActive(bool active);
    Q_INVOKABLE bool showroomPreviewActive() const { return m_showroomPreview; }
    /* True while the clock is actually running (QML status / diagnostics). */
    Q_INVOKABLE bool livingPreviewActive() const { return m_livingClock.isActive(); }
    /* Before/After moment: freeze the preview at the static poster (t=0).
     * The clock stops; un-pausing resumes the living loop. The canvas rests
     * at exactly the exported frame 0, so the comparison is honest. */
    Q_INVOKABLE void setLivingPreviewPaused(bool paused);
    Q_INVOKABLE bool livingPreviewPaused() const { return m_previewPaused; }

    /* -- window fitting --------------------------------------------------- */
    /* Clamp the Paintbrush interface window to the screen's available
     * geometry so the bottom chrome (palette + media drawer) never falls
     * behind the taskbar when the window was restored larger than the
     * current desktop (e.g. saved 4K geometry shown on a smaller screen). */
    Q_INVOKABLE void fitInterfaceToDesktop();

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
    /* Layer opacity (0..1), one undoable typed command per change. */
    Q_INVOKABLE void setLayerOpacity(int index, double opacity);
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
    /* Real track selection for the sticker editor. setSelectedTrackId
     * clamps to an existing track id (-1 when no tracks exist); the drawer
     * binds stickerEditor.trackId to this property. */
    int selectedTrackId() const { return m_selectedTrackId; }
    Q_INVOKABLE void setSelectedTrackId(int trackId);
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
    /* Track effects: blur/dim/spotlight/outline/attach. Applied at render
     * time (sticker preview/export/placement). Persisted in the project. */
    Q_INVOKABLE int trackEffectType(int trackId) const;
    Q_INVOKABLE int trackEffectIntensity(int trackId) const;
    Q_INVOKABLE QString trackEffectColor(int trackId) const;
    Q_INVOKABLE void setTrackEffect(int trackId, int type, int intensity,
                                    const QString &color);
    Q_INVOKABLE QString trackEffectPreviewDataUrl(int trackId, int frameIndex,
                                                  int outline,
                                                  const QString &outlineColor,
                                                  bool shadow, double scale) const;
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
    /* Close the attached video source: cancel thumbnail + exact-frame decode
     * work, stop the playback session, clear the current source-frame/media
     * state and hide the media drawer. The canvas and Harvest Tray (with its
     * item provenance: MRL/PTS/thumbnails) are NOT touched. */
    Q_INVOKABLE void closeVideo();

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
    /* Capability-honest status text: generation reports local-heal /
     * local-blur / remote-generation(model) / unavailable(reason) from its
     * CapabilityReport; vision reports Ready(model) / Unconfigured; SAM
     * reports Ready / the service's own reason (MissingModel, BuildNoSam3,
     * LoadError, ...). */
    QString generationStatusText() const;
    QString visionStatusText() const;
    QString segmentationStatusText() const;
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
    Q_INVOKABLE void openRecoveryCopy();
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
    void selectedTrackChanged();

private slots:
    void onCanvasObjectClick(QPointF canvasPos, bool positive, QPointF itemPos);
    void onCanvasAskClick(QPointF canvasPos, QPointF itemPos);
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
    /* Insert a LAYER_STICKER asset (cutout pixels + FrameRef provenance +
     * seed mask in metadata) as a new layer into the EXISTING document via
     * InsertLayerCommand — undoable, transformable, persisted. `raster` is
     * the already-trimmed sticker pixels; `position` is its canvas
     * top-left. */
    void insertStickerLayer(const QImage &raster, const QPointF &position,
                            const QString &name, const QString &sourceMRL,
                            const QString &sourceTitle, qint64 sourceTimeMs,
                            const QImage &seedMask);
    /* Real source pixels for a track keyframe (A16): bounded LRU cache
     * first, then ONE verified seek+capture through the existing
     * FrameCaptureService bridge. No UI-thread sleep-polling (A17); the
     * frame is accepted only when its PTS matches the keyframe time. A full
     * offline decode pipeline (EditDecodeSource / FrameProvider) lands with
     * Stream 3. */
    QImage stickerFrameForKeyframe(int trackId, int frameIndex,
                                   bool *playerTouched = nullptr) const;
    void cacheStickerFrame(int trackId, int frameIndex, qint64 timeMs,
                           const QImage &frame) const;
    void sanitizeSelectedTrack();
    void updateObjectSelection();
    void appendAssistantMessage(const QString &role, const QString &text);
    void pushRecentProject(const QString &path);
    void buildTrackObject();
    void triggerCompanionEvent(const QString &event);
    QImage objectCrop() const;
    QImage selectionCrop() const;
    QImage currentAiSource() const;
    QImage pristineSourceFrame() const;
    /* Non-const: decode the CURRENT player PTS into an ephemeral
     * LiveFrameContext (fresh every SAM seed click) complete with the
     * viewport<->frame letterbox transforms. `viewportSize` is the on-canvas
     * video area the click came from. Never mutates the document's
     * sourceFrame — that is composition provenance, not the player cursor. */
    LiveFrameContext captureLiveFrameContext(const QSize &viewportSize);
    /* Map the document/source frame used for a compose-mode SAM/Keep onto
     * the given viewport size (1:1 when the canvas matches the frame). */
    LiveFrameContext composeFrameContext(const QSize &viewportSize) const;
    QVector<QPointF> positivePointsForTracking() const;
    /* Exact-frame decode helpers (EditDecodeSource): all edit-pipeline
     * frame acquisition goes through these, never the playback player. */
    DecodedFrame requestFrame(const QString &mrl, qint64 ptsMs) const;
    /* Decode the frame a track keyframe was propagated on (source MRL
     * from the track, PTS from the keyframe) with PTS verification. */
    QImage decodeKeyframeFrame(const ObjectTrack &track,
                               const TrackKeyframe &kf) const;
    void startAutosave();
    /* Total bytes held by tracked-object masks and a bounded-eviction pass
     * used while a track grows (memory hardening). Returns bytes after
     * eviction. */
    qint64 trackedMaskBytes() const;
    qint64 enforceTrackMaskMemoryBudget();

    qt_intf_t *m_intf = nullptr;
    PaintbrushDocument m_document;
    PaintbrushLayerModel *m_layerModel = nullptr;
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
    /* Dedicated exact-frame decode source (EditDecodeSource). The main
     * playback player is the PlaybackSession (viewing only); every exact-
     * frame consumer decodes through this FrameProvider. */
    EditFrameSource m_editSource;

    QString m_statusMessage;
    QString m_projectName;
    bool m_objectSelected = false;
    QString m_objectMaskBoundsText;
    bool m_objectSelectBusy = false;
    QImage m_objectMask;
    double m_objectMaskScore = 0.0; /* confidence of the current object mask */
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
    /* real track selection for the sticker editor (A15); -1 = none */
    int m_selectedTrackId = -1;
    /* real source pixels for sticker previews/exports (A16), LRU-bounded */
    mutable StickerFrameCache m_stickerFrames;

    /* object selection click session */
    QVector<QPointF> m_posPoints;
    QVector<QPointF> m_negPoints;
    /* Live harvest frame for the CURRENT SAM prompt session (points are held
     * in FRAME space; reused only while MRL+PTS are unchanged). A change of
     * MRL or PTS starts a fresh session so a 9s click is never combined with
     * a 15s click. */
    LiveFrameContext m_liveCtx;
    FrameRef m_liveSessionRef;   /* the FrameRef the m_posPoints belong to */

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
    /* Living Showroom clock: canonical time + canvas recomposite gate */
    QTimer m_livingClock;
    qint64 m_animTimeMs = 0;
    bool m_showroomPreview = true;
    bool m_previewPaused = false;    /* Before/After compare: rest at t=0 */
    void onLivingClockTick();
    void refreshLivingClock();
    /* MRL for which the thumbnail timeline was auto-started, so the timer
     * does not restart generation on every tick for the same media */
    QString m_thumbnailsAutoMedia;
};

} // namespace paintbrush

#endif // PB_CONTROLLER_HPP
