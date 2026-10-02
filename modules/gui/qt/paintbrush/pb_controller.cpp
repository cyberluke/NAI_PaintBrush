/*****************************************************************************
 * pb_controller.cpp : NAI Paintbrush — workspace controller
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_controller.hpp"
#include "pb_canvas_item.hpp"
#include "pb_frame_capture.hpp"
#include "pb_export.hpp"
#include "pb_common.hpp"

#include <QQuickItem>
#include <QImage>
#include <QFileInfo>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QStandardPaths>
#include <QDir>
#include <QPainter>
#include <QDateTime>
#include <QVariantMap>

#include "qt.hpp"
#include "player/player_controller.hpp"
#include "playlist/playlist_controller.hpp"
#include <vlc_player.h>

namespace paintbrush {

PaintbrushController::PaintbrushController(qt_intf_t *intf, QObject *parent)
    : QObject(parent)
    , m_intf(intf)
{
    configureServices();

    connect(&m_segmentation, &SegmentationService::maskReady,
            this, &PaintbrushController::onSegmentationMaskReady);
    connect(&m_segmentation, &SegmentationService::errorOccurred,
            this, &PaintbrushController::onSegmentationError);
    connect(&m_segmentation, &SegmentationService::availabilityChanged,
            this, &PaintbrushController::onSegmentationAvailability);

    connect(&m_vision, &VisionService::responseReady,
            this, &PaintbrushController::onVisionResponse);
    connect(&m_vision, &VisionService::errorOccurred,
            this, &PaintbrushController::onVisionError);
    connect(&m_vision, &VisionService::busyChanged,
            this, &PaintbrushController::visionStateChanged);

    connect(&m_generation, &GenerationService::resultReady,
            this, &PaintbrushController::onGenerationResult);
    connect(&m_generation, &GenerationService::errorOccurred,
            this, &PaintbrushController::onGenerationError);
    connect(&m_generation, &GenerationService::busyChanged,
            this, &PaintbrushController::generationStateChanged);

    connect(&m_tracking, &TrackingService::keyframeReady,
            this, &PaintbrushController::onTrackKeyframe);
    connect(&m_tracking, &TrackingService::trackFinished,
            this, &PaintbrushController::onTrackFinished);
    connect(&m_tracking, &TrackingService::progressChanged,
            this, &PaintbrushController::trackingStateChanged);
    connect(&m_tracking, &TrackingService::statusChanged,
            this, &PaintbrushController::trackingStateChanged);
    connect(&m_tracking, &TrackingService::errorOccurred,
            this, &PaintbrushController::setStatus);

    connect(&m_companion, &CompanionService::messageReady,
            this, &PaintbrushController::onCompanionMessage);
    connect(&m_companion, &CompanionService::thinkingChanged,
            this, &PaintbrushController::companionStateChanged);
    connect(&m_companion, &CompanionService::errorOccurred,
            this, &PaintbrushController::setStatus);

    connect(&m_assistant, &AssistantService::availabilityChanged,
            this, [this](bool, const QString &) { emit serviceAvailabilityChanged(); });

    m_companion.setAssistantService(&m_assistant);

    connect(&m_undoStack, &UndoStack::canUndoChanged,
            this, &PaintbrushController::canUndoChanged);
    connect(&m_undoStack, &UndoStack::canRedoChanged,
            this, &PaintbrushController::canRedoChanged);

    /* a new blank canvas, like opening Paintbrush */
    m_document.resetDocument(QSize(1280, 720));
    emit documentChanged();

    m_mediaTimer.setInterval(250);
    connect(&m_mediaTimer, &QTimer::timeout, this, &PaintbrushController::onMediaTimer);
    m_mediaTimer.start();

    startAutosave();
}

PaintbrushController::~PaintbrushController()
{
    m_mediaTimer.stop();
    m_autosaveTimer.stop();
    m_segmentation.shutdown();
    m_tracking.shutdown();
    m_vision.cancelAll();
    m_generation.cancelAll();
    m_assistant.cancelAll();
}

void PaintbrushController::configureServices()
{
    m_segmentation.configure(VLC_OBJECT(m_intf));
    m_tracking.configure(VLC_OBJECT(m_intf));
    m_vision.configure(VLC_OBJECT(m_intf));
    m_generation.configure(VLC_OBJECT(m_intf));
    m_assistant.configure(VLC_OBJECT(m_intf));
    m_companion.configure(VLC_OBJECT(m_intf));

    if (m_companion.mode() == CompanionService::MODE_OFF)
        m_companionModeOffReason = QStringLiteral("The companion is off.");

    m_segmentation.initialize();
}

bool PaintbrushController::paintbrushEnabled() const
{
    return var_InheritBool(m_intf, "paintbrush-enabled");
}

void PaintbrushController::setCanvasItem(QQuickItem *item)
{
    auto *canvas = qobject_cast<PaintbrushCanvasItem *>(item);
    if (m_canvas == canvas)
        return;
    if (m_canvas)
    {
        disconnect(m_canvas, nullptr, this, nullptr);
    }
    m_canvas = canvas;
    if (!m_canvas)
        return;

    m_canvas->setController(this);
    m_canvas->setDocument(&m_document);
    m_canvas->setUndoStack(&m_undoStack);

    connect(m_canvas, &PaintbrushCanvasItem::objectSelectClick,
            this, &PaintbrushController::onCanvasObjectClick);
    connect(m_canvas, &PaintbrushCanvasItem::strokeCommitted,
            this, &PaintbrushController::onCanvasStrokeCommitted);
    connect(m_canvas, &PaintbrushCanvasItem::canvasModified,
            this, &PaintbrushController::onCanvasModified);
    connect(m_canvas, &PaintbrushCanvasItem::imageDropped,
            this, &PaintbrushController::onLayerDrop);
}

void PaintbrushController::setStatus(const QString &message)
{
    if (m_statusMessage == message)
        return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

/* -------------------------------------------------------------- document */

void PaintbrushController::newBlankCanvas(int width, int height)
{
    width = qBound(16, width, 8192);
    height = qBound(16, height, 8192);
    m_document.resetDocument(QSize(width, height));
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    m_projectName.clear();
    m_undoStack.clear();
    m_objectMask = QImage();
    m_objectSelected = false;
    m_posPoints.clear();
    m_negPoints.clear();
    if (m_canvas)
        m_canvas->clearObjectMask();
    if (m_welcomeVisible)
    {
        m_welcomeVisible = false;
        emit welcomeChanged();
    }
    emit projectChanged();
    emit documentChanged();
    setStatus(QStringLiteral("New blank canvas"));
}

void PaintbrushController::openImageFile(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
    {
        setStatus(QStringLiteral("Cannot open that file."));
        return;
    }
    QImage image(path);
    if (image.isNull())
    {
        setStatus(QStringLiteral("Could not open image: %1").arg(path));
        return;
    }
    openImageData(image, QFileInfo(path).completeBaseName());
}

void PaintbrushController::openImageData(const QImage &image, const QString &name)
{
    if (image.isNull())
        return;

    if (m_welcomeVisible)
    {
        m_welcomeVisible = false;
        emit welcomeChanged();
    }

    m_objectMask = QImage();
    m_objectSelected = false;
    m_posPoints.clear();
    m_negPoints.clear();
    if (m_canvas)
        m_canvas->clearObjectMask();

    /* first image on a blank canvas: adopt its size */
    const bool blank = (m_document.layerCount() <= 1 &&
                        m_document.currentLayer() &&
                        m_document.currentLayer()->isEmpty());
    if (blank)
    {
        m_document.resetDocument(image.size());
    }

    addImageLayer(image, QPointF(0, 0), name, LAYER_IMAGE);
    m_document.setSourceMedia({}, name, 0);
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    emit mediaStateChanged();
    setStatus(QStringLiteral("Opened %1").arg(name));
    triggerCompanionEvent(QStringLiteral("image-inserted"));
}

void PaintbrushController::captureVideoFrame()
{
    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    if (!player || !playerHasVideo())
    {
        setStatus(QStringLiteral("No video frame is available to capture."));
        return;
    }

    const QImage frame = FrameCaptureService::captureCurrentFrame(player);
    if (frame.isNull())
    {
        setStatus(QStringLiteral("Could not capture the current video frame."));
        return;
    }

    if (m_welcomeVisible)
    {
        m_welcomeVisible = false;
        emit welcomeChanged();
    }

    const MediaFrameRef ref = FrameCaptureService::currentMediaRef(player);

    /* adopt the frame size on a blank canvas */
    const bool blank = (m_document.layerCount() <= 1 &&
                        m_document.currentLayer() &&
                        m_document.currentLayer()->isEmpty());
    if (blank)
        m_document.resetDocument(frame.size());

    addImageLayer(frame, QPointF(0, 0),
                  ref.title.isEmpty() ? QStringLiteral("Video frame") : ref.title,
                  LAYER_REFERENCE);
    m_document.setSourceMedia(ref.mrl, ref.title, ref.timeMs);

    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    emit mediaStateChanged();
    setStatus(QStringLiteral("Captured video frame at %1 ms").arg(ref.timeMs));
    triggerCompanionEvent(QStringLiteral("media-frame-captured"));
}

void PaintbrushController::pasteImageFromClipboard()
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    const QMimeData *mime = clipboard->mimeData();
    if (!mime || !mime->hasImage())
    {
        setStatus(QStringLiteral("The clipboard does not contain an image."));
        return;
    }
    openImageData(qvariant_cast<QImage>(mime->imageData()),
                  QStringLiteral("Clipboard"));
}

void PaintbrushController::saveProject(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;

    const QString err = m_projectStore.save(&m_document, path);
    if (!err.isEmpty())
    {
        setStatus(err);
        return;
    }
    m_document.setProjectPath(path);
    m_projectName = QFileInfo(path).completeBaseName();
    m_document.setDirty(false);
    emit projectChanged();
    setStatus(QStringLiteral("Saved project %1").arg(m_projectName));
}

void PaintbrushController::openProject(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;

    const QString err = m_projectStore.load(&m_document, path);
    if (!err.isEmpty())
    {
        setStatus(err);
        return;
    }
    if (m_welcomeVisible)
    {
        m_welcomeVisible = false;
        emit welcomeChanged();
    }
    m_document.setProjectPath(path);
    m_projectName = QFileInfo(path).completeBaseName();
    m_undoStack.clear();
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    emit projectChanged();
    emit documentChanged();
    setStatus(QStringLiteral("Opened project %1").arg(m_projectName));
}

void PaintbrushController::exportImage(const QUrl &url, const QString &format)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    const QString err = ExportService::exportImage(&m_document, path, format);
    setStatus(err.isEmpty() ? QStringLiteral("Exported image")
                            : err);
}

void PaintbrushController::exportObjectPng(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    const QImage cutout = objectCrop();
    const QString err = ExportService::exportCutout(cutout, path);
    setStatus(err.isEmpty() ? QStringLiteral("Exported object PNG")
                            : err);
}

void PaintbrushController::exportMaskPng(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    const QString err = ExportService::exportMask(m_objectMask, path);
    setStatus(err.isEmpty() ? QStringLiteral("Exported mask PNG")
                            : err);
}

void PaintbrushController::autosaveNow()
{
    if (!var_InheritBool(m_intf, "paintbrush-autosave"))
        return;

    QString path = m_document.projectPath();
    if (path.isEmpty())
    {
        const QString dir = QStandardPaths::writableLocation(
                                QStandardPaths::AppDataLocation) +
                            QStringLiteral("/paintbrush");
        QDir().mkpath(dir);
        path = dir + QStringLiteral("/recovery.npb");
    }
    else
    {
        path += QStringLiteral(".autosave");
    }
    const QString err = m_projectStore.save(&m_document, path);
    if (!err.isEmpty())
        setStatus(err);
}

void PaintbrushController::startAutosave()
{
    const int interval = var_InheritInteger(m_intf, "paintbrush-autosave-interval");
    m_autosaveTimer.setInterval(qMax(10, interval) * 1000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &PaintbrushController::autosaveNow);
    m_autosaveTimer.start();
}

void PaintbrushController::undo()
{
    if (m_undoStack.undo(m_document) && m_canvas)
    {
        m_canvas->markCanvasDirty();
        emit documentChanged();
    }
}

void PaintbrushController::redo()
{
    if (m_undoStack.redo(m_document) && m_canvas)
    {
        m_canvas->markCanvasDirty();
        emit documentChanged();
    }
}

/* ---------------------------------------------------------------- layers */

void PaintbrushController::newLayer()
{
    CanvasLayer layer = CanvasLayer::createRaster(m_document.canvasSize(),
                                                  QStringLiteral("Layer %1")
                                                      .arg(m_document.layerCount() + 1));
    const QByteArray before = m_document.layersSnapshot();
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(layers.size() - 1);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Add layer")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::deleteLayer(int index)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size() || layers.size() <= 1)
        return;

    const QByteArray before = m_document.layersSnapshot();
    layers.removeAt(index);
    m_document.setLayers(layers);
    if (m_document.currentLayerIndex() >= layers.size())
        m_document.setCurrentLayerIndex(layers.size() - 1);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Delete layer")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::duplicateLayer(int index)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size())
        return;

    const QByteArray before = m_document.layersSnapshot();
    CanvasLayer copy = layers[index];
    copy.name = copy.name + QStringLiteral(" copy");
    layers.insert(index + 1, std::move(copy));
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(index + 1);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Duplicate layer")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::setCurrentLayer(int index)
{
    m_document.setCurrentLayerIndex(index);
    emit documentChanged();
}

void PaintbrushController::setLayerVisible(int index, bool visible)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size() || layers[index].visible == visible)
        return;

    const QByteArray before = m_document.layersSnapshot();
    layers[index].visible = visible;
    m_document.setLayers(layers);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Toggle layer visibility")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::moveLayer(int index, int delta)
{
    QList<CanvasLayer> layers = m_document.layers();
    const int target = index + delta;
    if (index < 0 || index >= layers.size() || target < 0 || target >= layers.size())
        return;

    const QByteArray before = m_document.layersSnapshot();
    layers.move(index, target);
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(target);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Reorder layer")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

/* ------------------------------------------------------ object selection */

void PaintbrushController::onCanvasObjectClick(QPointF canvasPos, bool positive,
                                              QPointF itemPos)
{
    Q_UNUSED(itemPos);
    if (!m_segmentation.isConfigured())
    {
        setStatus(m_segmentation.unavailableReason());
        return;
    }

    /* maintain per-session click prompts; shift-click adds more points */
    if (positive)
        m_posPoints.append(canvasPos);
    else
        m_negPoints.append(canvasPos);

    m_objectSelectBusy = true;
    emit objectSelectionChanged();
    if (m_canvas)
        m_canvas->setObjectSelectBusy(true);

    m_segmentation.segment(currentAiSource(), m_posPoints, m_negPoints, {});
}

void PaintbrushController::onSegmentationMaskReady(const QImage &mask, double score,
                                                   const QString &method)
{
    Q_UNUSED(method);
    m_objectMask = mask;
    m_objectSelectBusy = false;
    m_objectSelected = !mask.isNull();
    if (m_canvas)
    {
        m_canvas->setObjectMask(mask, score);
        m_canvas->setObjectSelectBusy(false);
    }
    updateObjectSelection();
    emit objectSelectionChanged();
    setStatus(QStringLiteral("Object selected (confidence %.0f%%)").arg(score * 100.0));
}

void PaintbrushController::onSegmentationError(const QString &message)
{
    m_objectSelectBusy = false;
    if (m_canvas)
        m_canvas->setObjectSelectBusy(false);
    emit objectSelectionChanged();
    setStatus(message);
}

void PaintbrushController::onSegmentationAvailability(bool available,
                                                     const QString &reason)
{
    Q_UNUSED(available);
    Q_UNUSED(reason);
    emit serviceAvailabilityChanged();
}

void PaintbrushController::updateObjectSelection()
{
    if (m_objectMask.isNull())
    {
        m_objectMaskBoundsText.clear();
        return;
    }
    const QRectF bounds = m_canvas ? m_canvas->objectMaskBounds()
                                   : QRectF();
    m_objectMaskBoundsText = bounds.isValid() && !bounds.isEmpty()
        ? QStringLiteral("%1 x %2").arg(int(bounds.width())).arg(int(bounds.height()))
        : QString();
}

QImage PaintbrushController::currentAiSource() const
{
    /* the media frame reference layer is the true AI source when present */
    if (m_canvas)
        return m_canvas->grabCanvasRegion(QRectF(QPointF(0, 0),
                                                 m_document.canvasSize()));
    return m_document.aiSourceImage();
}

QImage PaintbrushController::objectCrop() const
{
    if (m_objectMask.isNull() || !m_canvas)
        return {};
    const QRectF bounds = m_canvas->objectMaskBounds();
    if (bounds.isEmpty())
        return {};
    QImage region = m_canvas->grabCanvasRegion(bounds);
    /* apply the mask as alpha for a clean object crop */
    const QImage mask = m_objectMask.copy(bounds.toAlignedRect());
    return applyMaskAsAlpha(region, mask);
}

void PaintbrushController::refineObjectByText(const QString &textPrompt)
{
    if (textPrompt.trimmed().isEmpty())
        return;
    if (!m_segmentation.isConfigured())
    {
        setStatus(m_segmentation.unavailableReason());
        return;
    }
    m_objectSelectBusy = true;
    emit objectSelectionChanged();
    m_segmentation.segment(currentAiSource(), {}, {}, textPrompt.trimmed());
}

void PaintbrushController::cutOutObject()
{
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select an object first (Object Select tool)."));
        return;
    }
    addObjectToCanvas();
}

void PaintbrushController::addObjectToCanvas()
{
    const QImage cutout = objectCrop();
    if (cutout.isNull())
    {
        setStatus(QStringLiteral("Nothing selected to add."));
        return;
    }
    const QRectF bounds = m_canvas->objectMaskBounds();
    addImageLayer(cutout, bounds.topLeft(), QStringLiteral("Cutout"),
                  LAYER_IMAGE);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Cutout added to the canvas"));
    triggerCompanionEvent(QStringLiteral("cutout-added"));
}

void PaintbrushController::askAboutObject(const QString &question)
{
    if (!m_vision.isConfigured())
    {
        setStatus(m_vision.unavailableReason());
        return;
    }
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select an object first."));
        return;
    }

    m_visionConversationId = QStringLiteral("obj-%1")
                                 .arg(QDateTime::currentMSecsSinceEpoch());
    m_visionHasResult = false;
    emit visionStateChanged();
    m_vision.ask(objectCrop(), currentAiSource(), question, m_visionConversationId);
    setStatus(QStringLiteral("Asking about the selected object…"));
}

void PaintbrushController::askAboutSelection(const QString &question)
{
    if (!m_vision.isConfigured())
    {
        setStatus(m_vision.unavailableReason());
        return;
    }
    if (!m_canvas || !m_canvas->hasSelection())
    {
        setStatus(QStringLiteral("Select a region first."));
        return;
    }
    m_visionConversationId = QStringLiteral("sel-%1")
                                 .arg(QDateTime::currentMSecsSinceEpoch());
    m_visionHasResult = false;
    emit visionStateChanged();
    m_vision.ask(m_canvas->grabSelectionImage(), currentAiSource(), question,
                 m_visionConversationId);
}

void PaintbrushController::onVisionResponse(const QString &conversationId,
                                            const QString &answer,
                                            const QString &evidence,
                                            const QString &uncertainty,
                                            const QString &raw)
{
    Q_UNUSED(raw);
    m_visionAnswer = answer;
    m_visionEvidence = evidence;
    m_visionUncertainty = uncertainty;
    m_visionHasResult = true;
    emit visionStateChanged();
    appendAssistantMessage(QStringLiteral("user"),
                           QStringLiteral("Ask about this"));
    appendAssistantMessage(QStringLiteral("assistant"), answer);
}

void PaintbrushController::onVisionError(const QString &conversationId,
                                         const QString &message)
{
    Q_UNUSED(conversationId);
    setStatus(message);
}

void PaintbrushController::trackObject(int direction, int frames)
{
    if (m_objectMask.isNull() || !m_canvas)
    {
        setStatus(QStringLiteral("Select an object first."));
        return;
    }
    if (!m_tracking.isConfigured())
    {
        setStatus(m_tracking.unavailableReason());
        return;
    }
    if (!playerHasVideo())
    {
        setStatus(QStringLiteral("Tracking needs a video loaded in the player."));
        return;
    }

    /* reuse the click points used to seed the segmentation session */
    m_pendingTrack.clear();
    ObjectTrack track;
    track.id = m_trackIdCounter++;
    track.sourceMRL = m_document.sourceMediaMRL();
    track.startMs = m_document.sourceTimeMs();
    track.complete = false;
    m_pendingTrack.append(track);

    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    m_tracking.startTrack(player, currentAiSource(), m_objectMask,
                          positivePointsForTracking(), direction, frames);
    emit trackingStateChanged();
}

QVector<QPointF> PaintbrushController::positivePointsForTracking() const
{
    /* the controller stores the click session in the segmentation service
     * indirectly; for tracking we re-derive the positive point as the
     * center of the object mask */
    const QRectF bounds = m_canvas ? m_canvas->objectMaskBounds() : QRectF();
    if (bounds.isEmpty())
        return {};
    return { bounds.center() };
}

void PaintbrushController::cancelTracking()
{
    m_tracking.cancel();
}

void PaintbrushController::onTrackKeyframe(int frameIndex, qint64 timeMs,
                                           const QImage &mask, float confidence)
{
    if (m_pendingTrack.isEmpty())
        return;
    TrackKeyframe kf;
    kf.frameIndex = frameIndex;
    kf.timeMs = timeMs;
    kf.mask = mask;
    kf.confidence = confidence;
    m_pendingTrack[0].keyframes.append(kf);
    m_pendingTrack[0].endMs = timeMs;
    emit trackingStateChanged();
}

void PaintbrushController::onTrackFinished(bool complete, bool cancelled,
                                           const QString &error)
{
    Q_UNUSED(cancelled);
    if (!m_pendingTrack.isEmpty())
    {
        m_pendingTrack[0].complete = complete;
        if (!error.isEmpty())
            m_pendingTrack[0].error = error;

        QVector<ObjectTrack> tracks = m_document.tracks();
        tracks.append(m_pendingTrack[0]);
        m_document.setTracks(tracks);
        m_pendingTrack.clear();
        triggerCompanionEvent(QStringLiteral("object-tracked"));
    }
    emit trackingStateChanged();
    if (!error.isEmpty())
        setStatus(QStringLiteral("Tracking finished: %1").arg(error));
    else if (complete)
        setStatus(QStringLiteral("Tracking complete"));
}

/* ------------------------------------------------------- creative actions */

void PaintbrushController::makeSticker()
{
    const QImage cutout = objectCrop();
    if (cutout.isNull())
    {
        setStatus(QStringLiteral("Select an object first."));
        return;
    }

    /* trim transparent bounds */
    int minX = cutout.width(), minY = cutout.height();
    int maxX = -1, maxY = -1;
    for (int y = 0; y < cutout.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(cutout.constScanLine(y));
        for (int x = 0; x < cutout.width(); ++x)
            if (qAlpha(line[x]) != 0)
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
    }
    if (maxX < 0)
    {
        setStatus(QStringLiteral("The selected object is empty."));
        return;
    }

    QImage trimmed = cutout.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));

    /* white outline: dilate the alpha channel and fill behind */
    const QImage alpha = trimmed.convertToFormat(QImage::Format_Alpha8);
    const int outline = 3;
    QImage dilated(trimmed.size(), QImage::Format_ARGB32);
    dilated.fill(Qt::transparent);
    {
        QPainter p(&dilated);
        p.setPen(QPen(Qt::white, outline * 2 + 1, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::white);
        /* approximate the silhouette with the alpha mask path */
        QPainterPath path;
        QImage grayMask = alpha;
        for (int y = 0; y < grayMask.height(); ++y)
        {
            uchar *line = grayMask.scanLine(y);
            for (int x = 0; x < grayMask.width(); ++x)
                line[x] = qAlpha(trimmed.pixel(x, y));
        }
        path = maskToPath(grayMask);
        p.drawPath(path);
        p.end();
    }
    /* composite: white outline behind the cutout */
    QImage sticker(trimmed.size(), QImage::Format_ARGB32);
    sticker.fill(Qt::transparent);
    QPainter sp(&sticker);
    sp.drawImage(0, 0, dilated);
    sp.drawImage(0, 0, trimmed);
    sp.end();

    const QRectF pos = m_canvas->objectMaskBounds();
    addImageLayer(sticker, pos.topLeft(), QStringLiteral("Sticker"), LAYER_IMAGE);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Sticker created"));
    triggerCompanionEvent(QStringLiteral("sticker-made"));
}

void PaintbrushController::removeObject(const QString &prompt)
{
    if (!m_generation.isConfigured())
    {
        setStatus(m_generation.unavailableReason());
        return;
    }
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select an object first."));
        return;
    }
    m_generationRequestId = QStringLiteral("gen-%1")
                                .arg(QDateTime::currentMSecsSinceEpoch());
    m_generation.generate(GenerationService::ACTION_REMOVE_OBJECT,
                          currentAiSource(), m_objectMask, prompt,
                          m_generationRequestId);
    setStatus(QStringLiteral("Removing the selected object…"));
}

void PaintbrushController::generativeFill(const QString &prompt)
{
    if (!m_generation.isConfigured())
    {
        setStatus(m_generation.unavailableReason());
        return;
    }
    if (!m_canvas || !m_canvas->hasSelection())
    {
        setStatus(QStringLiteral("Select a region first."));
        return;
    }
    m_generationRequestId = QStringLiteral("gen-%1")
                                .arg(QDateTime::currentMSecsSinceEpoch());
    m_generation.generate(GenerationService::ACTION_GENERATIVE_FILL,
                          currentAiSource(), m_canvas->selectionMaskImage(),
                          prompt, m_generationRequestId);
    setStatus(QStringLiteral("Generating fill…"));
}

void PaintbrushController::replaceBackground(const QString &prompt)
{
    if (!m_generation.isConfigured())
    {
        setStatus(m_generation.unavailableReason());
        return;
    }
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select the subject first."));
        return;
    }
    /* background mask = inverse of the object mask */
    QImage bgMask = m_objectMask.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < bgMask.height(); ++y)
    {
        uchar *line = bgMask.scanLine(y);
        for (int x = 0; x < bgMask.width(); ++x)
            line[x] = line[x] >= 128 ? 0 : 255;
    }
    m_generationRequestId = QStringLiteral("gen-%1")
                                .arg(QDateTime::currentMSecsSinceEpoch());
    m_generation.generate(GenerationService::ACTION_REPLACE_BACKGROUND,
                          currentAiSource(), bgMask, prompt,
                          m_generationRequestId);
    setStatus(QStringLiteral("Replacing the background…"));
}

void PaintbrushController::cancelGeneration()
{
    m_generation.cancelAll();
}

void PaintbrushController::onGenerationResult(const QString &requestId,
                                              const QImage &result)
{
    if (requestId != m_generationRequestId)
        return;
    addResultLayer(result, QStringLiteral("AI result"));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit generationStateChanged();
    setStatus(QStringLiteral("AI result added as a new layer"));
    triggerCompanionEvent(QStringLiteral("ai-result"));
}

void PaintbrushController::onGenerationError(const QString &requestId,
                                             const QString &message)
{
    Q_UNUSED(requestId);
    setStatus(message);
    emit generationStateChanged();
}

/* ------------------------------------------------------------ companion */

void PaintbrushController::triggerCompanionEvent(const QString &event)
{
    if (m_companion.mode() == CompanionService::MODE_OFF)
        return;
    m_companion.notifyEvent(event, flattenedPreview());
}

void PaintbrushController::companionNotifyEvent(const QString &event)
{
    triggerCompanionEvent(event);
}

void PaintbrushController::companionAsk(const QString &question)
{
    if (m_companion.mode() == CompanionService::MODE_OFF)
    {
        setStatus(QStringLiteral("Turn the companion on to chat about your drawing."));
        return;
    }
    appendAssistantMessage(QStringLiteral("user"), question);
    m_companion.ask(question, flattenedPreview());
}

void PaintbrushController::companionReset()
{
    m_companion.resetConversation();
    emit companionStateChanged();
}

void PaintbrushController::companionClearConversation()
{
    m_assistantMessages.clear();
    emit companionStateChanged();
}

void PaintbrushController::setCompanionMode(int mode)
{
    m_companion.setMode(mode);
    emit companionStateChanged();
    emit serviceAvailabilityChanged();
}

void PaintbrushController::onCompanionMessage(const QString &text, const QString &role)
{
    appendAssistantMessage(role, text);
}

void PaintbrushController::appendAssistantMessage(const QString &role,
                                                  const QString &text)
{
    m_assistantMessages.append(QVariantMap{
        {QStringLiteral("role"), role},
        {QStringLiteral("text"), text},
    });
    /* keep the dock bounded */
    while (m_assistantMessages.size() > 200)
        m_assistantMessages.removeFirst();
    emit assistantMessageAppended(role, text);
    emit companionStateChanged();
}

/* --------------------------------------------------------------- layers */

void PaintbrushController::addImageLayer(const QImage &image, const QPointF &position,
                                         const QString &name, int type)
{
    CanvasLayer layer = CanvasLayer::createImage(image, position, name);
    layer.type = type;

    const QByteArray before = m_document.layersSnapshot();
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(layers.size() - 1);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Add %1").arg(name)});
}

void PaintbrushController::addResultLayer(const QImage &image, const QString &name)
{
    addImageLayer(image, QPointF(0, 0), name, LAYER_GENERATED);
}

QImage PaintbrushController::flattenedPreview() const
{
    return m_document.flattenedImage();
}

QString PaintbrushController::fileDialogFilter() const
{
    QStringList formats;
    for (const QString &f : supportedImageFormats())
        formats << QStringLiteral("*.%1").arg(f);
    return QStringLiteral("Paintbrush project (*.npb);;Images (%1);;All files (*)")
        .arg(formats.join(QLatin1Char(' ')));
}

/* ------------------------------------------------------------ media drawer */

bool PaintbrushController::playerHasVideo() const
{
    return m_intf->p_mainPlayerController->hasVideoOutput() &&
           m_intf->p_mainPlayerController->isStarted();
}

QString PaintbrushController::currentMediaTitle() const
{
    return m_intf->p_mainPlayerController->getName();
}

QString PaintbrushController::currentMediaMRL() const
{
    return FrameCaptureService::currentMRL(m_intf->p_mainPlayerController->getPlayer());
}

qint64 PaintbrushController::playerTimeMs() const
{
    return FrameCaptureService::currentTimeMs(
        m_intf->p_mainPlayerController->getPlayer());
}

qint64 PaintbrushController::playerLengthMs() const
{
    const int len = m_intf->p_mainPlayerController->getLength().toMilliseconds();
    return len > 0 ? len : 0;
}

void PaintbrushController::playerFrameNext()
{
    m_intf->p_mainPlayerController->frameNext();
    emit mediaStateChanged();
}

void PaintbrushController::playerFramePrev()
{
    m_intf->p_mainPlayerController->framePrev();
    emit mediaStateChanged();
}

void PaintbrushController::playerPlayPause()
{
    m_intf->p_mainPlaylistController->togglePlayPause();
    emit mediaStateChanged();
}

void PaintbrushController::playerSeek(qint64 timeMs)
{
    m_intf->p_mainPlayerController->setTime(VLCTime(timeMs * 1000));
    emit mediaStateChanged();
}

void PaintbrushController::playerStop()
{
    m_intf->p_mainPlaylistController->stop();
    emit mediaStateChanged();
}

void PaintbrushController::onMediaTimer()
{
    emit mediaStateChanged();
}

/* --------------------------------------------------------------- signals */

void PaintbrushController::onCanvasStrokeCommitted(const QRect &region)
{
    Q_UNUSED(region);
    emit documentChanged();
}

void PaintbrushController::onCanvasModified()
{
    emit documentChanged();
    m_document.setDirty(true);
}

void PaintbrushController::onLayerDrop(const QUrl &url, const QPointF &pos)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    QImage image(path);
    if (image.isNull())
    {
        /* a video dropped onto the canvas: capture its current frame */
        setStatus(QStringLiteral("Dropped item is not an image."));
        return;
    }
    const bool blank = (m_document.layerCount() <= 1 &&
                        m_document.currentLayer() &&
                        m_document.currentLayer()->isEmpty());
    if (blank)
        m_document.resetDocument(image.size());
    addImageLayer(image, pos, QFileInfo(path).completeBaseName(), LAYER_IMAGE);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Image added to the canvas"));
}

QString PaintbrushController::diagnosticsText() const
{
    QStringList lines;
    lines << QStringLiteral("NAI Paintbrush diagnostics");

    lines << QStringLiteral("Segmentation backend: %1")
                 .arg(m_segmentation.isConfigured()
                          ? QStringLiteral("SAM3 (%1, %2)")
                                .arg(m_segmentation.modelPath(),
                                     m_segmentation.device())
                          : QStringLiteral("unavailable"));
    if (!m_segmentation.unavailableReason().isEmpty())
        lines << QStringLiteral("  reason: %1").arg(m_segmentation.unavailableReason());

    lines << QStringLiteral("VLM endpoint: %1 (%2)")
                 .arg(m_vision.isConfigured() ? m_vision.baseUrl()
                                              : QStringLiteral("unavailable"),
                      m_vision.model().isEmpty() ? QStringLiteral("default model")
                                                 : m_vision.model());
    if (!m_vision.unavailableReason().isEmpty())
        lines << QStringLiteral("  reason: %1").arg(m_vision.unavailableReason());

    lines << QStringLiteral("Image provider: %1 (%2)")
                 .arg(m_generation.isConfigured() ? m_generation.baseUrl()
                                                  : QStringLiteral("unavailable"),
                      m_generation.model().isEmpty() ? QStringLiteral("default model")
                                                     : m_generation.model());
    if (!m_generation.unavailableReason().isEmpty())
        lines << QStringLiteral("  reason: %1").arg(m_generation.unavailableReason());

    lines << QStringLiteral("AI assistant: %1 (%2, %3)")
                 .arg(m_assistant.isConfigured() ? m_assistant.baseUrl()
                                                 : QStringLiteral("unavailable"),
                      m_assistant.model().isEmpty() ? QStringLiteral("default model")
                                                    : m_assistant.model(),
                      m_assistant.reasoning() ? QStringLiteral("reasoning")
                                              : QStringLiteral("no reasoning"));
    if (!m_assistant.unavailableReason().isEmpty())
        lines << QStringLiteral("  reason: %1").arg(m_assistant.unavailableReason());
    lines << QStringLiteral("  limits: context %1, input %2, output %3, history %4 messages")
                 .arg(m_assistant.contextLimit())
                 .arg(m_assistant.inputLimit())
                 .arg(m_assistant.outputLimit())
                 .arg(m_assistant.conversationMessageCount());

    lines << QStringLiteral("Tracking: %1").arg(m_tracking.isConfigured()
                                                    ? QStringLiteral("available")
                                                    : QStringLiteral("unavailable"));
    lines << QStringLiteral("Companion: mode %1, %2")
                 .arg(m_companion.mode())
                 .arg(m_companion.isThinking() ? QStringLiteral("thinking")
                                               : QStringLiteral("idle"));

    if (!m_statusMessage.isEmpty())
        lines << QStringLiteral("Last status: %1").arg(m_statusMessage);
    return lines.join(QLatin1Char('\n'));
}

} // namespace paintbrush