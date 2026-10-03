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
#include <QBuffer>
#include <QElapsedTimer>
#include <QThread>
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

    connect(&m_lookup, &LookupService::lookupResult,
            this, [this](int, const QVariantList &matches) {
                m_lookupMatches = matches;
                setStatus(QStringLiteral("Product identification ready"));
                emit lookupChanged();
                emit serviceAvailabilityChanged();
            });
    connect(&m_lookup, &LookupService::errorOccurred,
            this, [this](const QString &message) { setStatus(message); });
    connect(&m_lookup, &LookupService::availabilityChanged,
            this, [this](bool, const QString &) { emit serviceAvailabilityChanged(); });

    connect(&m_thumbnails, &ThumbnailService::thumbnailReady,
            this, [this](int, const QImage &) { emit thumbnailsChanged(); });
    connect(&m_thumbnails, &ThumbnailService::generationFinished,
            this, [this]() { emit thumbnailsChanged(); });
    connect(&m_thumbnails, &ThumbnailService::generationFailed,
            this, [this](const QString &reason) { setStatus(reason); emit thumbnailsChanged(); });

    m_companion.setAssistantService(&m_assistant);

    connect(&m_undoStack, &UndoStack::canUndoChanged,
            this, &PaintbrushController::canUndoChanged);
    connect(&m_undoStack, &UndoStack::canRedoChanged,
            this, &PaintbrushController::canRedoChanged);
    connect(&m_undoStack, &UndoStack::maskRestored,
            this, &PaintbrushController::onMaskRestored);

    /* a new blank canvas, like opening Paintbrush */
    m_document.resetDocument(QSize(1280, 720));
    emit documentChanged();

    m_mediaTimer.setInterval(250);
    connect(&m_mediaTimer, &QTimer::timeout, this, &PaintbrushController::onMediaTimer);
    m_mediaTimer.start();

    startAutosave();
    checkForRecovery();

    /* welcome screen on a fresh start: show it whenever nothing is loaded
     * and no crash recovery is pending, so the entry points (draw / open
     * picture / play with a video) stay discoverable */
    if (!m_recoveryAvailable && m_document.layerCount() == 0)
    {
        m_welcomeVisible = true;
        emit welcomeChanged();
    }
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
    m_thumbnails.cancel();
}

void PaintbrushController::configureServices()
{
    m_segmentation.configure(VLC_OBJECT(m_intf));
    m_tracking.configure(VLC_OBJECT(m_intf));
    m_vision.configure(VLC_OBJECT(m_intf));
    m_generation.configure(VLC_OBJECT(m_intf));
    m_assistant.configure(VLC_OBJECT(m_intf));
    m_companion.configure(VLC_OBJECT(m_intf));
    m_lookup.configure(VLC_OBJECT(m_intf));

    /* demo mode: preload a sample project if present; otherwise a blank
     * canvas is ready anyway */
    if (var_InheritBool(m_intf, "paintbrush-demo-mode"))
    {
        QString sample;
        const QStringList candidates = {
            QStringLiteral("sample.npb"),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                QStringLiteral("/paintbrush/sample.npb"),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                QStringLiteral("/paintbrush/demo.npb"),
        };
        for (const QString &c : candidates)
            if (QFileInfo::exists(c))
            {
                sample = c;
                break;
            }
        if (!sample.isEmpty())
        {
            const QString err = m_projectStore.load(&m_document, sample);
            if (err.isEmpty())
            {
                m_welcomeVisible = false;
                if (m_canvas)
                {
                    m_canvas->markCanvasDirty();
                    m_canvas->fitToView();
                }
            }
        }
        /* highlight the strongest tool for the demo */
        if (m_canvas)
            m_canvas->setTool(TOOL_OBJECT_SELECT);
    }

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
    connect(m_canvas, &PaintbrushCanvasItem::maskRefineStarted,
            this, &PaintbrushController::onMaskRefineStarted);
    connect(m_canvas, &PaintbrushCanvasItem::objectMaskEdited,
            this, &PaintbrushController::onObjectMaskEdited);
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

void PaintbrushController::openVideoFile(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
    {
        setStatus(QStringLiteral("Cannot open that file."));
        return;
    }

    /* A Paintbrush video session is the main VLC player: append the file to
     * the playlist and start playing it, then the media drawer appears and
     * frames can be captured/tracked. */
    vlc::playlist::Media media(QUrl::fromLocalFile(path).toString(),
                               QFileInfo(path).fileName());
    m_intf->p_mainPlaylistController->append(
        QVector<vlc::playlist::Media>{media}, true);
    emit mediaStateChanged();
    setStatus(QStringLiteral("Playing %1").arg(QFileInfo(path).fileName()));

    if (m_welcomeVisible)
    {
        m_welcomeVisible = false;
        emit welcomeChanged();
    }
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
    pushRecentProject(path);
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
    pushRecentProject(path);
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    emit projectChanged();
    emit documentChanged();
    setStatus(QStringLiteral("Opened project %1").arg(m_projectName));
}

void PaintbrushController::pushRecentProject(const QString &path)
{
    /* keep the 6 most recent projects in the config (persisted) */
    QStringList recent;
    const QString current = var_InheritString(m_intf, "paintbrush-recent-projects");
    if (!current.isEmpty())
        recent = current.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 6)
        recent.removeLast();
    var_SetString(m_intf, "paintbrush-recent-projects",
                  recent.join(QLatin1Char(';')).toUtf8().constData());
    emit recentProjectsChanged();
}

QVariantList PaintbrushController::recentProjects() const
{
    QVariantList list;
    const QString current = var_InheritString(m_intf, "paintbrush-recent-projects");
    if (current.isEmpty())
        return list;
    const QStringList recent = current.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &path : recent)
    {
        QVariantMap item;
        item[QStringLiteral("path")] = path;
        item[QStringLiteral("name")] = QFileInfo(path).completeBaseName();
        list.append(item);
    }
    return list;
}

void PaintbrushController::openRecentProject(int index)
{
    const QVariantList recent = recentProjects();
    if (index < 0 || index >= recent.size())
        return;
    const QString path = recent[index].toMap()[QStringLiteral("path")].toString();
    if (!QFileInfo::exists(path))
    {
        setStatus(QStringLiteral("Recent project is missing: %1").arg(path));
        return;
    }
    openProject(QUrl::fromLocalFile(path));
}

/* ----------------------------------------------------------- AI scheduler */

QVariantList PaintbrushController::activeJobs() const
{
    QVariantList jobs;
    auto add = [&jobs](const QString &id, const QString &name,
                       const QString &resource, bool busy) {
        if (!busy)
            return;
        QVariantMap job;
        job[QStringLiteral("id")] = id;
        job[QStringLiteral("name")] = name;
        job[QStringLiteral("resource")] = resource;
        jobs.append(job);
    };
    add(QStringLiteral("segmentation"), QStringLiteral("Object Select"),
        QStringLiteral("GPU"), m_segmentation.busy());
    add(QStringLiteral("tracking"), QStringLiteral("Tracking"),
        QStringLiteral("GPU"), m_tracking.isTracking());
    add(QStringLiteral("vision"), QStringLiteral("Ask about this"),
        QStringLiteral("remote"), m_vision.isBusy());
    add(QStringLiteral("generation"), QStringLiteral("Generation"),
        QStringLiteral("remote"), m_generation.isBusy());
    add(QStringLiteral("assistant"), QStringLiteral("AI assistant"),
        QStringLiteral("remote"), m_assistant.isBusy());
    add(QStringLiteral("thumbnails"), QStringLiteral("Thumbnails"),
        QStringLiteral("cpu"), m_thumbnails.isGenerating());
    return jobs;
}

void PaintbrushController::cancelActiveJob(const QString &jobId)
{
    if (jobId == QStringLiteral("segmentation"))
        m_segmentation.cancel();
    else if (jobId == QStringLiteral("tracking"))
        m_tracking.cancel();
    else if (jobId == QStringLiteral("vision"))
        m_vision.cancelAll();
    else if (jobId == QStringLiteral("generation"))
        m_generation.cancelAll();
    else if (jobId == QStringLiteral("assistant"))
        m_assistant.cancelAll();
    else if (jobId == QStringLiteral("thumbnails"))
        m_thumbnails.cancel();
    emit activeJobsChanged();
    setStatus(QStringLiteral("Cancelled %1").arg(jobId));
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

void PaintbrushController::exportImagePreset(const QUrl &url, int preset)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    QString format;
    double scale = 1.0;
    bool crop = false;
    QColor background = QColor(Qt::transparent);
    switch (preset)
    {
    case 1: /* Transparent Sticker: trim + alpha */
        crop = true;
        format = QStringLiteral("png");
        break;
    case 2: /* Social Post: 1080 square */
        scale = 1080.0 / qMax(1, m_document.canvasWidth());
        format = QStringLiteral("png");
        break;
    case 3: /* Wallpaper: 1920x1080-ish, fill white behind JPEG */
        scale = 1.0;
        format = QStringLiteral("jpg");
        background = Qt::white;
        break;
    case 4: /* Tracked Clip is handled by exportTrackedSticker */
    default:
        break; /* Canvas PNG: default */
    }
    const QString err = ExportService::exportImageOptions(
        &m_document, path, format, scale, crop, background);
    setStatus(err.isEmpty() ? QStringLiteral("Exported (%1)").arg(preset)
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

void PaintbrushController::relinkMedia(const QUrl &url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    /* keep the old timestamp; only the MRL/title change */
    m_document.setSourceMedia(path, QFileInfo(path).fileName(),
                              m_document.sourceTimeMs());
    /* update the tracked objects that referenced the old MRL */
    QVector<ObjectTrack> tracks = m_document.tracks();
    for (ObjectTrack &track : tracks)
        track.sourceMRL = path;
    m_document.setTracks(tracks);
    m_document.setDirty(true);
    emit documentChanged();
    emit mediaStateChanged();
    setStatus(QStringLiteral("Media relinked to %1").arg(QFileInfo(path).fileName()));
}

QString PaintbrushController::projectInfoText() const
{
    QStringList lines;
    lines << QStringLiteral("Canvas: %1 x %2 px")
                 .arg(m_document.canvasWidth()).arg(m_document.canvasHeight());
    lines << QStringLiteral("Layers: %1 (current %2)")
                 .arg(m_document.layerCount()).arg(m_document.currentLayerIndex() + 1);
    if (!m_document.sourceMediaMRL().isEmpty())
        lines << QStringLiteral("Linked media: %1").arg(m_document.sourceMediaTitle());
    lines << QStringLiteral("Tracked objects: %1").arg(m_document.trackCount());
    lines << QStringLiteral("AI history: %1 message(s)")
                 .arg(m_assistantMessages.size());
    lines << QStringLiteral("Autosave: %1")
                 .arg(var_InheritBool(m_intf, "paintbrush-autosave")
                          ? QStringLiteral("on (%1 s)").arg(
                                var_InheritInteger(m_intf, "paintbrush-autosave-interval"))
                          : QStringLiteral("off"));
    if (!m_document.projectPath().isEmpty())
        lines << QStringLiteral("Project: %1").arg(m_document.projectPath());
    return lines.join(QStringLiteral("\n"));
}

void PaintbrushController::autosaveNow()
{
    if (!var_InheritBool(m_intf, "paintbrush-autosave"))
        return;

    /* nothing to recover on a pristine blank canvas: do not create a
     * recovery file that would hide the welcome screen on next launch */
    if (m_document.layerCount() == 0 && m_document.projectPath().isEmpty())
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

/* ----------------------------------------------------------- crash recovery */

void PaintbrushController::checkForRecovery()
{
    /* look for the recovery autosave for an unsaved document, or the sidecar
     * autosave of the last project */
    const QString fallback = QStandardPaths::writableLocation(
                                 QStandardPaths::AppDataLocation) +
                             QStringLiteral("/paintbrush/recovery.npb");
    const QString projectAutosave = m_document.projectPath() + QStringLiteral(".autosave");

    QString path;
    if (QFileInfo::exists(fallback))
        path = fallback;
    else if (!m_document.projectPath().isEmpty() && QFileInfo::exists(projectAutosave))
        path = projectAutosave;

    if (path.isEmpty())
    {
        m_recoveryAvailable = false;
        m_recoveryDescription.clear();
        m_recoveryPath.clear();
        emit recoveryStateChanged();
        return;
    }

    m_recoveryPath = path;
    m_recoveryDescription =
        QStringLiteral("Paintbrush found an unsaved project from an earlier session.\n%1")
            .arg(QFileInfo(path).fileName());
    m_recoveryAvailable = true;
    emit recoveryStateChanged();
}

void PaintbrushController::restoreRecovery()
{
    if (m_recoveryPath.isEmpty())
        return;
    const QString err = m_projectStore.load(&m_document, m_recoveryPath);
    if (!err.isEmpty())
    {
        setStatus(QStringLiteral("Recovery failed: %1").arg(err));
        return;
    }
    /* the recovery copy is clearly identified so it can never be confused
     * with the explicit saved project */
    m_document.setProjectPath({});
    m_document.setDirty(true);
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->setTool(m_canvas->tool());
    }
    m_undoStack.clear();
    /* discard recovery data only after a successful restore */
    QFile::remove(m_recoveryPath);
    m_recoveryAvailable = false;
    m_recoveryPath.clear();
    m_recoveryDescription.clear();
    emit documentChanged();
    emit recoveryStateChanged();
    setStatus(QStringLiteral("Recovered unsaved project (copy)"));
}

void PaintbrushController::discardRecovery()
{
    if (!m_recoveryPath.isEmpty())
        QFile::remove(m_recoveryPath);
    m_recoveryAvailable = false;
    m_recoveryPath.clear();
    m_recoveryDescription.clear();
    emit recoveryStateChanged();
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

void PaintbrushController::transformLayer(int index, double scale, double rotation)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size())
        return;
    CanvasLayer &layer = layers[index];
    if (layer.type != LAYER_IMAGE && layer.type != LAYER_TEXT)
        return;
    scale = qBound(0.01, scale, 64.0);
    rotation = qBound(-360.0, rotation, 360.0);
    if (qFuzzyCompare(float(scale), layer.scale) &&
        qFuzzyCompare(float(rotation), layer.rotation))
        return;

    const QByteArray before = m_document.layersSnapshot();
    layer.scale = float(scale);
    layer.rotation = float(rotation);
    m_document.setLayers(layers);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Transform layer")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::moveImageLayer(int index, const QPointF &position)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size())
        return;
    CanvasLayer &layer = layers[index];
    if (layer.type != LAYER_IMAGE && layer.type != LAYER_TEXT)
        return;
    if (layer.position == position)
        return;

    const QByteArray before = m_document.layersSnapshot();
    layer.position = position;
    m_document.setLayers(layers);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Move image layer")});
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

void PaintbrushController::onMaskRefineStarted(const QImage &mask)
{
    m_maskUndoBefore = mask;
}

void PaintbrushController::onObjectMaskEdited(const QImage &mask)
{
    const QImage before = std::move(m_maskUndoBefore);
    m_maskUndoBefore = QImage();
    m_objectMask = mask;
    m_objectSelected = !mask.isNull();
    updateObjectSelection();
    emit objectSelectionChanged();
    /* one mask-refine stroke == one undoable operation */
    if (!before.isNull() && before != mask)
        m_undoStack.pushMask({before, mask, QStringLiteral("Refine selection mask")});
    setStatus(QStringLiteral("Selection mask refined with brush."));
}

void PaintbrushController::onMaskRestored(const QImage &mask)
{
    m_objectMask = mask;
    m_objectSelected = !mask.isNull();
    if (m_canvas)
    {
        m_canvas->setObjectMask(mask, 1.0);
        m_canvas->markCanvasDirty();
    }
    updateObjectSelection();
    emit objectSelectionChanged();
    emit documentChanged();
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

void PaintbrushController::setObjectMaskEdgeHard()
{
    if (m_objectMask.isNull())
        return;
    QImage mask = m_objectMask.convertToFormat(QImage::Format_Grayscale8);
    const int w = mask.width(), h = mask.height();
    for (int y = 0; y < h; ++y)
    {
        uchar *line = mask.scanLine(y);
        for (int x = 0; x < w; ++x)
            line[x] = line[x] >= 128 ? 255 : 0;
    }
    m_objectMask = mask;
    m_objectMaskSoftEdge = false;
    if (m_canvas)
    {
        m_canvas->setObjectMask(mask, 1.0);
        m_canvas->markCanvasDirty();
    }
    emit objectMaskEdgeChanged();
    emit objectSelectionChanged();
    setStatus(QStringLiteral("Object mask edge set to hard."));
}

void PaintbrushController::setObjectMaskEdgeSoft()
{
    if (m_objectMask.isNull())
        return;
    /* Binarize, then distance-feather the boundary band into a smooth
     * alpha edge: interior stays 255, exterior stays 0, the shell becomes
     * a graded transition. This is genuine alpha-edge generation (a blur
     * of the mask boundary), NOT semantic hair matting. */
    QImage mask = m_objectMask.convertToFormat(QImage::Format_Grayscale8);
    const int w = mask.width(), h = mask.height();
    QImage bin(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y)
    {
        const uchar *src = mask.constScanLine(y);
        uchar *dst = bin.scanLine(y);
        for (int x = 0; x < w; ++x)
            dst[x] = src[x] >= 128 ? 255 : 0;
    }
    /* box-blur passes give a soft edge; 2 passes ~= small feather */
    QImage blurred = bin;
    constexpr int passes = 3;
    for (int p = 0; p < passes; ++p)
    {
        QImage tmp = blurred;
        const int r = 2; /* blur radius 2 -> ~4px transition band */
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                int sum = 0, n = 0;
                for (int dy = -r; dy <= r; ++dy)
                {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    const uchar *sline = tmp.constScanLine(yy);
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        const int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        sum += sline[xx];
                        ++n;
                    }
                }
                blurred.scanLine(y)[x] = static_cast<uchar>(sum / n);
            }
        }
    }
    /* clamp interior/exterior so the core stays opaque and the hole stays clear */
    for (int y = 0; y < h; ++y)
    {
        const uchar *bline = bin.constScanLine(y);
        uchar *oline = blurred.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            if (bline[x] >= 128)
                oline[x] = qMax<uchar>(oline[x], 200);
            else
                oline[x] = qMin<uchar>(oline[x], 60);
        }
    }
    m_objectMask = blurred;
    m_objectMaskSoftEdge = true;
    if (m_canvas)
    {
        m_canvas->setObjectMask(blurred, 1.0);
        m_canvas->markCanvasDirty();
    }
    emit objectMaskEdgeChanged();
    emit objectSelectionChanged();
    setStatus(QStringLiteral("Object mask edge set to soft."));
}

QImage PaintbrushController::pristineSourceFrame() const
{
    /* The reference layer holds the true captured video frame. The canvas
     * composite mixes user strokes and generated layers over it, so AI
     * operations must read the reference raster directly — otherwise the
     * VLM/segmenter is shown a synthetic reconstruction, not the frame. */
    const auto &layers = m_document.layers();
    for (const CanvasLayer &layer : layers)
    {
        if (layer.type == LAYER_REFERENCE && !layer.raster.isNull())
            return layer.raster;
    }
    return {};
}

QImage PaintbrushController::currentAiSource() const
{
    /* the media frame reference layer is the true AI source when present */
    const QImage frame = pristineSourceFrame();
    if (!frame.isNull())
    {
        if (frame.size() != m_document.canvasSize())
            return frame.scaled(m_document.canvasSize(), Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
        return frame;
    }
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

    /* Crop the true video pixels, not the canvas composite: the mask
     * coordinates live in canvas space, so scale the pristine frame to the
     * canvas size first (1:1 when the canvas was sized to the frame). */
    QImage region;
    QImage frame = pristineSourceFrame();
    if (!frame.isNull())
    {
        if (frame.size() != m_document.canvasSize())
            frame = frame.scaled(m_document.canvasSize(), Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        region = frame.copy(bounds.toAlignedRect().intersected(frame.rect()));
    }
    else
    {
        region = m_canvas->grabCanvasRegion(bounds);
    }
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

void PaintbrushController::extractObject()
{
    /* the collage main feature: pull the selected object out of the video
     * frame onto its own transparent canvas, ready to be placed anywhere */
    const QImage cutout = objectCrop();
    if (cutout.isNull())
    {
        setStatus(QStringLiteral("Select an object first (Object Select tool)."));
        return;
    }
    const int w = qBound(16, cutout.width() + 128, 8192);
    const int h = qBound(16, cutout.height() + 128, 8192);
    m_document.resetDocument(QSize(w, h));
    addImageLayer(cutout, QPointF(64, 64), QStringLiteral("Extracted object"),
                  LAYER_IMAGE);
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    setStatus(QStringLiteral("Object extracted to a new canvas"));
    triggerCompanionEvent(QStringLiteral("object-extracted"));
}

QImage PaintbrushController::selectionCrop() const
{
    if (!m_canvas || !m_canvas->hasSelection())
        return {};
    const QImage mask = m_canvas->selectionMaskImage();
    if (mask.isNull())
        return {};

    const QRect b = m_document.selectionPath().boundingRect().toAlignedRect()
                        .intersected(mask.rect());
    if (b.isEmpty())
        return {};

    /* Prefer the pristine media frame over the canvas composite, exactly
     * like objectCrop(): the user's freehand selection over a video frame
     * should feed the VLM the true pixels, not strokes composited on top. */
    QImage source;
    QImage frame = pristineSourceFrame();
    if (!frame.isNull())
    {
        if (frame.size() != m_document.canvasSize())
            frame = frame.scaled(m_document.canvasSize(), Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        source = frame;
    }
    else
    {
        source = m_canvas->grabSelectionImage();
        /* grabSelectionImage already applies the selection mask */
        return source;
    }

    QImage result(b.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(0, 0, source, b.x(), b.y(), b.width(), b.height());
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, mask, b.x(), b.y(), b.width(), b.height());
    p.end();
    return result;
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
    m_vision.ask(selectionCrop(), currentAiSource(), question,
                 m_visionConversationId);
}

void PaintbrushController::askAboutObjectFollowUp(const QString &question)
{
    /* retains the object crop context from the previous turn */
    if (!m_vision.isConfigured())
    {
        setStatus(m_vision.unavailableReason());
        return;
    }
    if (m_visionConversationId.isEmpty())
    {
        askAboutObject(question);
        return;
    }
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select an object first."));
        return;
    }
    m_visionHasResult = false;
    emit visionStateChanged();
    m_vision.ask(objectCrop(), currentAiSource(), question, m_visionConversationId);
    setStatus(QStringLiteral("Asking a follow-up…"));
}

void PaintbrushController::findProduct()
{
    if (!m_lookup.isConfigured())
    {
        setStatus(m_lookup.unavailableReason());
        return;
    }
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select an object first."));
        return;
    }
    m_lookupMatches.clear();
    emit lookupChanged();
    setStatus(QStringLiteral("Identifying the selected object…"));
    /* OCR text is not produced by the integrated build yet; pass an empty
     * string. The vision question that produced the current answer, if any,
     * is not reused here — the crop itself is the evidence. */
    m_lookup.lookup(objectCrop(), {}, {}, {});
}

void PaintbrushController::copyVisionAnswer()
{
    if (m_visionAnswer.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(m_visionAnswer);
    setStatus(QStringLiteral("Answer copied"));
}

void PaintbrushController::addVisionAnswerAsText()
{
    if (m_visionAnswer.isEmpty() || !m_canvas)
        return;
    QFont font;
    font.setPixelSize(18);
    /* place near the object if there is one, else center-ish */
    const QPointF pos = m_canvas->objectMaskBounds().bottomLeft() +
                        QPointF(0, 12);
    m_canvas->applyText(m_visionAnswer, pos, font, QColor(Qt::black));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Answer added as text"));
}

void PaintbrushController::clearVisionResult()
{
    m_visionAnswer.clear();
    m_visionEvidence.clear();
    m_visionUncertainty.clear();
    m_visionHasResult = false;
    m_visionConversationId.clear();
    emit visionStateChanged();
}

QString PaintbrushController::objectThumbnailDataUrl() const
{
    const QImage crop = objectCrop();
    if (crop.isNull())
        return {};
    QImage thumb = crop.scaled(88, 88, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    thumb.save(&buffer, "PNG");
    return QStringLiteral("data:image/png;base64,") +
           QString::fromLatin1(bytes.toBase64());
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

void PaintbrushController::pauseResumeTracking()
{
    if (m_tracking.paused())
        m_tracking.resume();
    else
        m_tracking.pause();
    emit trackingStateChanged();
}

bool PaintbrushController::trackingPaused() const
{
    return m_tracking.paused();
}

QVariantList PaintbrushController::trackList() const
{
    QVariantList list;
    const QVector<ObjectTrack> tracks = m_document.tracks();
    for (const ObjectTrack &track : tracks)
    {
        QVariantMap item;
        item[QStringLiteral("id")] = track.id;
        item[QStringLiteral("label")] =
            track.label.isEmpty() ? QStringLiteral("Object %1").arg(track.id + 1)
                                  : track.label;
        item[QStringLiteral("startMs")] = track.startMs;
        item[QStringLiteral("endMs")] = track.endMs;
        item[QStringLiteral("keyframes")] = track.keyframes.size();
        item[QStringLiteral("complete")] = track.complete;
        item[QStringLiteral("cancelled")] = track.cancelled;
        /* correction markers */
        int corrections = 0;
        for (const TrackKeyframe &kf : track.keyframes)
            if (kf.corrected)
                ++corrections;
        item[QStringLiteral("corrections")] = corrections;
        list.append(item);
    }
    return list;
}

void PaintbrushController::seekToTrackKeyframe(int trackId, int keyframeIndex)
{
    const QVector<ObjectTrack> tracks = m_document.tracks();
    for (const ObjectTrack &track : tracks)
    {
        if (track.id != trackId)
            continue;
        if (keyframeIndex < 0 || keyframeIndex >= track.keyframes.size())
            return;
        playerSeek(track.keyframes[keyframeIndex].timeMs);
        return;
    }
}

void PaintbrushController::deleteTrack(int trackId)
{
    QVector<ObjectTrack> tracks = m_document.tracks();
    for (int i = 0; i < tracks.size(); ++i)
    {
        if (tracks[i].id == trackId)
        {
            tracks.removeAt(i);
            m_document.setTracks(tracks);
            emit trackingStateChanged();
            emit documentChanged();
            return;
        }
    }
}

QString PaintbrushController::exportTrackedSticker(int trackId, const QUrl &dirUrl)
{
    const QString dir = dirUrl.toLocalFile();
    if (dir.isEmpty())
        return QStringLiteral("Choose a folder first.");

    const QVector<ObjectTrack> tracks = m_document.tracks();
    int trackIndex = -1;
    for (int i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == trackId)
        {
            trackIndex = i;
            break;
        }
    if (trackIndex < 0)
        return QStringLiteral("Track not found.");
    const ObjectTrack &track = tracks[trackIndex];
    if (track.keyframes.isEmpty())
        return QStringLiteral("This track has no keyframes yet.");

    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    if (!player)
        return QStringLiteral("The source media is no longer loaded.");

    const qint64 originalTime = FrameCaptureService::currentTimeMs(player);
    QDir().mkpath(dir);
    int written = 0;
    QSize lastSize;

    for (const TrackKeyframe &kf : track.keyframes)
    {
        /* seek the player to the keyframe time and grab the real frame */
        {
            PlayerLock lock(player);
            vlc_player_SetTime(player, kf.timeMs * 1000);
        }
        /* allow the vout to display the frame */
        QElapsedTimer wait;
        wait.start();
        QImage frame;
        while (wait.elapsed() < 3000)
        {
            frame = FrameCaptureService::captureCurrentFrame(player);
            if (!frame.isNull())
                break;
            QThread::msleep(60);
        }
        if (frame.isNull())
            continue;

        /* apply the keyframe mask -> transparent cutout */
        QImage mask = kf.mask;
        if (mask.size() != frame.size())
            mask = mask.scaled(frame.size(), Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
        mask = mask.convertToFormat(QImage::Format_Grayscale8);

        QImage cutout(frame.size(), QImage::Format_ARGB32);
        cutout.fill(Qt::transparent);
        {
            QPainter p(&cutout);
            p.drawImage(0, 0, frame);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, mask);
            p.end();
        }

        /* trim to the mask bounds */
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
            continue;
        QImage sticker = cutout.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
        lastSize = sticker.size();

        const QString path = dir + QStringLiteral("/sticker_%1.png")
                                     .arg(kf.frameIndex, 4, 10, QLatin1Char('0'));
        if (!sticker.save(path, "PNG"))
            return QStringLiteral("Failed to write %1").arg(path);
        ++written;
    }

    /* restore the original position */
    if (written > 0 && originalTime > 0)
    {
        PlayerLock lock(player);
        vlc_player_SetTime(player, originalTime * 1000);
    }

    if (written == 0)
        return QStringLiteral("No frames could be exported.");
    setStatus(QStringLiteral("Sticker sequence: %1 frames (%2x%3) in %4")
                  .arg(written).arg(lastSize.width()).arg(lastSize.height()).arg(dir));
    return dir;
}

/* --------------------------------------------------- animated sticker editor */

static const ObjectTrack *findTrack(const QVector<ObjectTrack> &tracks, int trackId)
{
    for (const ObjectTrack &track : tracks)
        if (track.id == trackId)
            return &track;
    return nullptr;
}

/* Render a keyframe cutout with optional outline/shadow/scale. Shared by the
 * preview data URLs and the optioned export so what you see is what exports. */
static QImage renderStickerFrame(const QImage &frame, const QImage &mask,
                                 int outline, const QColor &outlineColor,
                                 bool shadow, double scale)
{
    QImage maskScaled = mask.isNull() ? QImage() : mask;
    if (!maskScaled.isNull() && maskScaled.size() != frame.size())
        maskScaled = maskScaled.scaled(frame.size(), Qt::IgnoreAspectRatio,
                                       Qt::FastTransformation);
    if (!maskScaled.isNull())
        maskScaled = maskScaled.convertToFormat(QImage::Format_Grayscale8);

    QImage cutout(frame.size(), QImage::Format_ARGB32);
    cutout.fill(Qt::transparent);
    {
        QPainter p(&cutout);
        p.drawImage(0, 0, frame);
        if (!maskScaled.isNull())
        {
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, maskScaled);
        }
        p.end();
    }

    /* trim to non-empty bounds */
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
        return {};
    QImage trimmed = cutout.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));

    if (scale != 1.0)
    {
        const QSize scaledSize(qMax(1, int(trimmed.width() * scale)),
                               qMax(1, int(trimmed.height() * scale)));
        trimmed = trimmed.scaled(scaledSize, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
    }

    if (outline <= 0 && !shadow)
        return trimmed;

    /* shadow: dark silhouette offset below-right */
    QImage result(trimmed.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    {
        QPainter p(&result);
        if (shadow)
        {
            QImage shadowMask(trimmed.size(), QImage::Format_ARGB32);
            shadowMask.fill(Qt::transparent);
            QImage alpha = trimmed.convertToFormat(QImage::Format_Alpha8);
            QPainterPath path;
            QImage gray = alpha.convertToFormat(QImage::Format_Grayscale8);
            path = maskToPath(gray);
            QPainter sp(&shadowMask);
            sp.setPen(QPen(QColor(0, 0, 0, 160), 6, Qt::SolidLine,
                           Qt::RoundCap, Qt::RoundJoin));
            sp.setBrush(QColor(0, 0, 0, 160));
            sp.drawPath(path);
            sp.end();
            p.drawImage(QPoint(4, 4), shadowMask);
        }
        if (outline > 0)
        {
            QPainterPath path;
            QImage alpha = trimmed.convertToFormat(QImage::Format_Alpha8);
            QImage gray = alpha.convertToFormat(QImage::Format_Grayscale8);
            path = maskToPath(gray);
            QPainter op(&result);
            op.setPen(QPen(outlineColor, outline * 2 + 1, Qt::SolidLine,
                           Qt::RoundCap, Qt::RoundJoin));
            op.setBrush(outlineColor);
            op.drawPath(path);
            op.end();
        }
        p.drawImage(0, 0, trimmed);
        p.end();
    }
    return result;
}

int PaintbrushController::stickerEditorKeyframeCount(int trackId) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    return track ? track->keyframes.size() : 0;
}

QString PaintbrushController::stickerEditorFrameDataUrl(int trackId, int frameIndex) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    if (!track || frameIndex < 0 || frameIndex >= track->keyframes.size())
        return {};
    const TrackKeyframe &kf = track->keyframes[frameIndex];
    if (kf.mask.isNull())
        return {};
    QImage frame = kf.mask.convertToFormat(QImage::Format_Grayscale8);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!frame.save(&buffer, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,%1")
        .arg(QString::fromLatin1(png.toBase64()));
}

QString PaintbrushController::stickerEditorPreviewDataUrl(int trackId, int frameIndex,
                                                          int outline,
                                                          const QString &outlineColor,
                                                          bool shadow, double scale) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    if (!track || frameIndex < 0 || frameIndex >= track->keyframes.size())
        return {};
    const TrackKeyframe &kf = track->keyframes[frameIndex];
    if (kf.mask.isNull())
        return {};
    /* the true frame pixels are re-captured from the player on export; for
     * the preview we render the mask shape with a neutral fill so the trim/
     * outline/shadow settings are visible without re-seeking the player. */
    QImage frame(kf.mask.size(), QImage::Format_ARGB32);
    frame.fill(QColor(200, 200, 210));
    QImage rendered = renderStickerFrame(frame, kf.mask, outline,
                                         QColor(outlineColor), shadow, scale);
    if (rendered.isNull())
        return {};
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!rendered.save(&buffer, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,%1")
        .arg(QString::fromLatin1(png.toBase64()));
}

QString PaintbrushController::exportTrackedStickerOptions(int trackId, const QUrl &dirUrl,
                                                          int startFrame, int endFrame,
                                                          int outline,
                                                          const QString &outlineColor,
                                                          bool shadow, double scale,
                                                          int speed)
{
    const QString dir = dirUrl.toLocalFile();
    if (dir.isEmpty())
        return QStringLiteral("Choose a folder first.");

    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    if (!track)
        return QStringLiteral("Track not found.");
    if (track->keyframes.isEmpty())
        return QStringLiteral("This track has no keyframes yet.");

    startFrame = qBound(0, startFrame, track->keyframes.size() - 1);
    endFrame = qBound(startFrame, endFrame, track->keyframes.size() - 1);
    speed = qBound(1, speed, 8);

    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    if (!player)
        return QStringLiteral("The source media is no longer loaded.");

    const qint64 originalTime = FrameCaptureService::currentTimeMs(player);
    QDir().mkpath(dir);
    int written = 0;
    int exported = 0;
    QSize lastSize;
    const QColor oc(outlineColor);

    for (int i = startFrame; i <= endFrame; ++i)
    {
        const TrackKeyframe &kf = track->keyframes[i];
        /* seek the player to the keyframe time and grab the real frame */
        {
            PlayerLock lock(player);
            vlc_player_SetTime(player, kf.timeMs * 1000);
        }
        QElapsedTimer wait;
        wait.start();
        QImage frame;
        while (wait.elapsed() < 3000)
        {
            frame = FrameCaptureService::captureCurrentFrame(player);
            if (!frame.isNull())
                break;
            QThread::msleep(60);
        }
        if (frame.isNull())
            continue;

        QImage sticker = renderStickerFrame(frame, kf.mask, outline, oc,
                                            shadow, scale);
        if (sticker.isNull())
            continue;
        lastSize = sticker.size();

        /* speed > 1 keeps every speed-th frame (playback speed control) */
        if (speed > 1 && (exported % speed) != 0)
        {
            ++exported;
            continue;
        }
        ++exported;

        const QString path = dir + QStringLiteral("/sticker_%1.png")
                                     .arg(kf.frameIndex, 4, 10, QLatin1Char('0'));
        if (!sticker.save(path, "PNG"))
            return QStringLiteral("Failed to write %1").arg(path);
        ++written;
    }

    if (written > 0 && originalTime > 0)
    {
        PlayerLock lock(player);
        vlc_player_SetTime(player, originalTime * 1000);
    }

    if (written == 0)
        return QStringLiteral("No frames could be exported.");
    setStatus(QStringLiteral("Sticker sequence: %1 frames (%2x%3) in %4")
                  .arg(written).arg(lastSize.width()).arg(lastSize.height()).arg(dir));
    return dir;
}

void PaintbrushController::placeStickerFrame(int trackId, int frameIndex, int outline,
                                             const QString &outlineColor, bool shadow,
                                             double scale)
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    if (!track || frameIndex < 0 || frameIndex >= track->keyframes.size())
    {
        setStatus(QStringLiteral("Choose a track frame first."));
        return;
    }
    const TrackKeyframe &kf = track->keyframes[frameIndex];
    if (kf.mask.isNull())
    {
        setStatus(QStringLiteral("This frame has no mask (evicted to save memory). Re-track or use a corrected frame."));
        return;
    }
    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    QImage frame;
    if (player)
    {
        const qint64 originalTime = FrameCaptureService::currentTimeMs(player);
        {
            PlayerLock lock(player);
            vlc_player_SetTime(player, kf.timeMs * 1000);
        }
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 3000)
        {
            frame = FrameCaptureService::captureCurrentFrame(player);
            if (!frame.isNull())
                break;
            QThread::msleep(60);
        }
        if (originalTime > 0)
        {
            PlayerLock lock(player);
            vlc_player_SetTime(player, originalTime * 1000);
        }
    }
    if (frame.isNull())
    {
        setStatus(QStringLiteral("Could not capture the frame; place it from a still instead."));
        return;
    }
    QImage sticker = renderStickerFrame(frame, kf.mask, outline,
                                        QColor(outlineColor), shadow, scale);
    if (sticker.isNull())
    {
        setStatus(QStringLiteral("The selected frame produced an empty cutout."));
        return;
    }
    const QPointF pos = kf.mask.isNull()
        ? QPointF(0, 0)
        : QPointF((m_document.canvasSize().width() - sticker.width()) / 2.0,
                  (m_document.canvasSize().height() - sticker.height()) / 2.0);
    addImageLayer(sticker, pos, QStringLiteral("Animated sticker frame"),
                  LAYER_IMAGE);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Sticker frame placed on the canvas."));
    triggerCompanionEvent(QStringLiteral("sticker-made"));
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
    /* keep tracked-mask memory bounded as the track grows */
    enforceTrackMaskMemoryBudget();
    emit trackingStateChanged();
}

qint64 PaintbrushController::trackedMaskBytes() const
{
    qint64 bytes = 0;
    const auto countTrack = [&](const ObjectTrack &track) {
        for (const TrackKeyframe &kf : track.keyframes)
            if (!kf.mask.isNull())
                bytes += qint64(kf.mask.sizeInBytes());
    };
    for (const ObjectTrack &track : m_document.tracks())
        countTrack(track);
    for (const ObjectTrack &track : m_pendingTrack)
        countTrack(track);
    return bytes;
}

qint64 PaintbrushController::enforceTrackMaskMemoryBudget()
{
    /* Bounded caching: if tracked masks exceed the budget, evict the oldest
     * non-user-corrected mask content (keep frame metadata + corrected
     * keyframes) so a long 4K track cannot grow memory without limit. The
     * default budget is generous (512 MiB) and covers many dozens of frames;
     * exports read from the remaining masks. */
    const qint64 budget = qint64(512) * 1024 * 1024;
    const qint64 before = trackedMaskBytes();
    if (before <= budget)
        return before;

    /* gather candidate frames oldest-first across all tracks + pending */
    QList<TrackKeyframe *> candidates;
    auto collect = [&candidates](QVector<ObjectTrack> &tracks) {
        for (ObjectTrack &track : tracks)
            for (TrackKeyframe &kf : track.keyframes)
                if (!kf.corrected && !kf.mask.isNull())
                    candidates.append(&kf);
    };
    QVector<ObjectTrack> all = m_document.tracks();
    // pending track lives separately; operate through references below
    for (ObjectTrack &track : m_pendingTrack)
        for (TrackKeyframe &kf : track.keyframes)
            if (!kf.corrected && !kf.mask.isNull())
                candidates.append(&kf);
    std::sort(candidates.begin(), candidates.end(),
              [](const TrackKeyframe *a, const TrackKeyframe *b) {
                  return a->frameIndex < b->frameIndex;
              });

    qint64 cur = before;
    for (TrackKeyframe *kf : candidates)
    {
        if (cur <= budget)
            break;
        if (!kf->mask.isNull())
        {
            cur -= qint64(kf->mask.sizeInBytes());
            kf->mask = QImage();   /* evict content, keep metadata */
        }
    }
    return cur;
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

void PaintbrushController::autoCollage(int preset)
{
    if (!m_canvas)
        return;

    /* collect eligible layers: raster, image, sticker and generated layers
     * that have actual content (not text/reference layers) */
    QList<CanvasLayer> all = m_document.layers();
    QList<int> indices;
    for (int i = 0; i < all.size(); ++i)
    {
        const CanvasLayer &l = all[i];
        if (l.type == LAYER_RASTER || l.type == LAYER_IMAGE ||
            l.type == LAYER_GENERATED)
        {
            if (!l.raster.isNull() && !l.isEmpty())
                indices.append(i);
        }
    }
    if (indices.size() < 2)
    {
        setStatus(QStringLiteral("Auto collage needs at least two layers with content."));
        return;
    }

    const QSize canvas = m_document.canvasSize();
    if (canvas.isEmpty())
        return;

    /* compute each piece's non-transparent bounds (document coords) */
    struct Piece { int layerIndex; QRectF bounds; };
    QList<Piece> pieces;
    for (int idx : indices)
    {
        const CanvasLayer &l = all[idx];
        const QRect cb = l.contentBounds();
        QRectF b;
        if (!cb.isEmpty())
            b = l.position.isNull()
                    ? QRectF(cb)
                    : QRectF(l.position + cb.topLeft(), cb.size());
        else
            b = l.position.isNull() ? QRectF(QPointF(0, 0), l.raster.size())
                                    : QRectF(l.position, l.raster.size());
        pieces.append({idx, b});
    }

    /* deterministic pseudo-random for scatter/scrapbook jitter */
    auto rnd = [](int seed, int salt) {
        return double((seed * 2654435761u + salt * 40503u) % 100000) / 100000.0;
    };

    const int n = pieces.size();
    const qreal margin = qMax<qreal>(8, canvas.width() * 0.01);
    const qreal areaW = canvas.width() - margin * 2;
    const qreal areaH = canvas.height() - margin * 2;

    QList<QPointF> newPositions;
    newPositions.reserve(n);
    QList<qreal> newScales;      /* per-piece scale factor */
    QList<qreal> newRotations;   /* per-piece rotation (scrapbook only) */
    newScales.fill(1.0, n);
    newRotations.fill(0.0, n);

    switch (preset)
    {
    case 1: /* balanced grid */
    {
        const int cols = qCeil(qSqrt(qreal(n)));
        const int rows = qCeil(qreal(n) / cols);
        const qreal cellW = areaW / cols;
        const qreal cellH = areaH / rows;
        for (int i = 0; i < n; ++i)
        {
            const int c = i % cols;
            const int r = i / cols;
            const QRectF b = pieces[i].bounds;
            const qreal scale = qMin(cellW / qMax(1.0, b.width()),
                                     cellH / qMax(1.0, b.height()));
            const qreal w = b.width() * scale, h = b.height() * scale;
            newPositions.append(QPointF(margin + c * cellW + (cellW - w) / 2,
                                        margin + r * cellH + (cellH - h) / 2));
            newScales[i] = scale;
        }
        break;
    }
    case 2: /* editorial: one hero + column of the rest */
    {
        const QRectF hero = pieces[0].bounds;
        const qreal heroScale = qMin(areaW * 0.55 / qMax(1.0, hero.width()),
                                     areaH / qMax(1.0, hero.height()));
        const qreal heroW = hero.width() * heroScale;
        const qreal heroH = hero.height() * heroScale;
        newPositions.append(QPointF(margin, margin + (areaH - heroH) / 2));
        const qreal colX = margin + heroW + margin;
        const qreal colW = areaW - heroW - margin;
        const qreal slotH = qMax<qreal>(30, (areaH - margin * (n - 1)) / (n - 1));
        for (int i = 1; i < n; ++i)
        {
            const QRectF b = pieces[i].bounds;
            const qreal scale = qMin(colW / qMax(1.0, b.width()),
                                     slotH / qMax(1.0, b.height()));
            const qreal w = b.width() * scale, h = b.height() * scale;
            newPositions.append(QPointF(colX + (colW - w) / 2,
                                            margin + (i - 1) * (slotH + margin) +
                                                (slotH - h) / 2));
            newScales[i] = scale;
        }
        break;
    }
    case 3: /* scrapbook: scattered with rotation jitter */
    {
        const int seed = int(QDateTime::currentMSecsSinceEpoch()) & 0x7fffffff;
        for (int i = 0; i < n; ++i)
        {
            const QRectF b = pieces[i].bounds;
            const qreal maxW = areaW * (n > 6 ? 0.3 : 0.45);
            const qreal maxH = areaH * 0.45;
            const qreal scale = qMin(maxW / qMax(1.0, b.width()),
                                     maxH / qMax(1.0, b.height()));
            const qreal w = b.width() * scale, h = b.height() * scale;
            const qreal x = margin + rnd(seed, i * 3 + 1) * (areaW - w);
            const qreal y = margin + rnd(seed, i * 3 + 2) * (areaH - h);
            newPositions.append(QPointF(x, y));
            newScales[i] = scale;
            /* scrapbook jitter: ±10 degrees keeps it playful but readable */
            newRotations[i] = (rnd(seed, i * 3 + 3) - 0.5) * 20.0;
        }
        break;
    }
    case 4: /* vertical poster: stacked from top */
    {
        const qreal slotH = qMax<qreal>(24, (areaH - margin * (n - 1)) / n);
        for (int i = 0; i < n; ++i)
        {
            const QRectF b = pieces[i].bounds;
            const qreal scale = qMin(areaW / qMax(1.0, b.width()),
                                     slotH / qMax(1.0, b.height()));
            const qreal w = b.width() * scale, h = b.height() * scale;
            newPositions.append(QPointF(margin + (areaW - w) / 2,
                                        margin + i * (slotH + margin) +
                                            (slotH - h) / 2));
            newScales[i] = scale;
        }
        break;
    }
    case 5: /* horizontal banner: one row */
    {
        const qreal slotW = qMax<qreal>(24, (areaW - margin * (n - 1)) / n);
        for (int i = 0; i < n; ++i)
        {
            const QRectF b = pieces[i].bounds;
            const qreal scale = qMin(slotW / qMax(1.0, b.width()),
                                     areaH / qMax(1.0, b.height()));
            const qreal w = b.width() * scale, h = b.height() * scale;
            newPositions.append(QPointF(margin + i * (slotW + margin) +
                                            (slotW - w) / 2,
                                        margin + (areaH - h) / 2));
            newScales[i] = scale;
        }
        break;
    }
    case 0: /* free scatter (no jitter, keep sizes, avoid overlap) */
    default:
    {
        const int seed = int(QDateTime::currentMSecsSinceEpoch()) & 0x7fffffff;
        for (int i = 0; i < n; ++i)
        {
            const QRectF b = pieces[i].bounds;
            const qreal scale = qMin(areaW * 0.5 / qMax(1.0, b.width()),
                                     areaH * 0.5 / qMax(1.0, b.height()));
            const qreal w = b.width() * scale, h = b.height() * scale;
            const qreal x = margin + rnd(seed, i * 7 + 1) * (areaW - w);
            const qreal y = margin + rnd(seed, i * 7 + 2) * (areaH - h);
            newPositions.append(QPointF(x, y));
            newScales[i] = scale;
        }
        break;
    }
    }

    /* apply as one undoable structure change: raster layers are converted to
     * positioned image layers (cut to content); image/generated layers keep
     * their raster and are scaled/rotated via the layer transform — every
     * piece stays an independent editable layer with real transforms. */
    const QByteArray before = m_document.layersSnapshot();
    QList<CanvasLayer> layers = m_document.layers();
    for (int i = 0; i < pieces.size(); ++i)
    {
        const int idx = pieces[i].layerIndex;
        const QRectF b = pieces[i].bounds;
        CanvasLayer &l = layers[idx];
        const QPointF newPos = newPositions[i];

        if (l.type == LAYER_RASTER)
        {
            /* cut the raster layer's content into a positioned image layer */
            QImage content = l.raster.copy(b.toAlignedRect());
            if (content.isNull())
                continue;
            l.raster = content;
            l.position = newPos;
            l.type = LAYER_IMAGE;
        }
        else
        {
            /* image/generated layer: keep raster, apply transform */
            l.position = newPos;
        }
        l.scale = float(newScales[i]);
        l.rotation = float(newRotations[i]);
    }
    m_document.setLayers(layers);
    const QByteArray after = m_document.layersSnapshot();
    m_undoStack.pushStructure({before, after, QStringLiteral("Auto collage")});
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Auto collage applied"));
    triggerCompanionEvent(QStringLiteral("collage"));
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
    m_companion.resetConversation();
    emit companionStateChanged();
}

int PaintbrushController::companionTalkLevel() const
{
    return m_companion.talkLevel();
}

void PaintbrushController::setCompanionTalkLevel(int level)
{
    m_companion.setTalkLevel(level);
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
    QImage flat = m_document.flattenedImage();
    if (flat.isNull())
        return {};
    /* The AI sees an opaque image: composite the (possibly transparent)
     * canvas onto white so sparse black strokes are visible. A fully
     * transparent flatten would be seen as a blank/black canvas. */
    QImage preview(flat.size(), QImage::Format_ARGB32);
    preview.fill(Qt::white);
    QPainter p(&preview);
    p.drawImage(0, 0, flat);
    p.end();
    return preview;
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
    m_thumbnails.noteUserSeek();
    m_intf->p_mainPlayerController->frameNext();
    emit mediaStateChanged();
}

void PaintbrushController::playerFramePrev()
{
    m_thumbnails.noteUserSeek();
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
    /* the user took over the playhead: the thumbnail batch must not restore
     * its original position when it finishes. (During a slider drag the
     * batch is also suspended via playerSeekBegin/playerSeekEnd so it cannot
     * seek the player underneath the drag.) */
    m_thumbnails.noteUserSeek();

    m_intf->p_mainPlayerController->setTime(VLCTime(timeMs * 1000));
    emit mediaStateChanged();
}

void PaintbrushController::playerSeekBegin()
{
    /* user pressed the seek slider: stop the thumbnail batch from seeking
     * the player underneath the drag */
    m_thumbnails.suspend();
}

void PaintbrushController::playerSeekEnd()
{
    /* user released the seek slider. The suspended thumbnail batch is NOT
     * resumed here: resuming would seek the player immediately and race the
     * final precise seek issued on release. onMediaTimer() resumes the batch
     * a moment later, only while the video is actually playing — when paused,
     * generation stays suspended so it cannot disturb the exact frame the
     * user is inspecting. */
}

/* ---------------------------------------------------------- thumbnails */

QVariantList PaintbrushController::thumbnailList() const
{
    QVariantList list;
    const auto thumbs = m_thumbnails.thumbnails();
    for (const auto &th : thumbs)
    {
        QVariantMap item;
        item[QStringLiteral("timeMs")] = th.timeMs;
        item[QStringLiteral("hasImage")] = !th.pending && !th.image.isNull();
        item[QStringLiteral("failed")] = th.failed;
        list.append(item);
    }
    return list;
}

QString PaintbrushController::thumbnailDataUrl(int index) const
{
    const auto thumbs = m_thumbnails.thumbnails();
    if (index < 0 || index >= thumbs.size())
        return {};
    const auto &th = thumbs[index];
    if (th.image.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    th.image.save(&buffer, "PNG");
    return QStringLiteral("data:image/png;base64,") +
           QString::fromLatin1(bytes.toBase64());
}

qint64 PaintbrushController::thumbnailTimeMs(int index) const
{
    const auto thumbs = m_thumbnails.thumbnails();
    if (index < 0 || index >= thumbs.size())
        return 0;
    return thumbs[index].timeMs;
}

void PaintbrushController::generateThumbnails()
{
    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    if (!player)
    {
        setStatus(QStringLiteral("No media is loaded for thumbnails."));
        return;
    }
    const QString mediaKey = FrameCaptureService::currentMRL(player);
    if (mediaKey.isEmpty())
        return;
    const qint64 duration = m_intf->p_mainPlayerController->getLength().toMilliseconds();
    const qint64 anchor = m_intf->p_mainPlayerController->getTime().toMilliseconds();
    m_thumbnails.startBatch(player, mediaKey, qMax<qint64>(1, duration),
                            anchor, 24);
    emit thumbnailsChanged();
}

void PaintbrushController::cancelThumbnails()
{
    m_thumbnails.cancel();
    emit thumbnailsChanged();
}

/* ----------------------------------------------------------- diagnostics */

QString PaintbrushController::copyDiagnostics()
{
    const QString report = diagnosticsText();
    QGuiApplication::clipboard()->setText(report);
    return report;
}

void PaintbrushController::clearAiCache()
{
    m_segmentation.clearCache();
    m_thumbnails.cancel();
    setStatus(QStringLiteral("AI caches cleared"));
    emit serviceAvailabilityChanged();
}

void PaintbrushController::unloadAiModels()
{
    m_segmentation.shutdown();
    m_tracking.shutdown();
    m_vision.cancelAll();
    m_generation.cancelAll();
    m_assistant.cancelAll();
    setStatus(QStringLiteral("AI models unloaded"));
    emit serviceAvailabilityChanged();
}

QString PaintbrushController::autosavePath() const
{
    const QString current = m_document.projectPath();
    if (!current.isEmpty())
        return current + QStringLiteral(".autosave");
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/paintbrush/recovery.npb");
}

void PaintbrushController::playerStop()
{
    m_intf->p_mainPlaylistController->stop();
    emit mediaStateChanged();
}

void PaintbrushController::onMediaTimer()
{
    emit mediaStateChanged();

    /* A batch suspended by a paused scrub resumes as soon as playback
     * starts: generation then no longer disturbs the exact frame a paused
     * user was inspecting. */
    if (m_thumbnails.isSuspended()
        && m_intf->p_mainPlayerController->getPlayingState()
               == PlayerController::PLAYING_STATE_PLAYING)
        m_thumbnails.resume();

    /* Auto-fill the thumbnail timeline: as soon as a video with a known
     * length is loaded, start generating thumbnails once (the service seeks
     * the player to each slot, snapshots it, then restores the original
     * position). This makes the timeline populated instead of empty even if
     * the user never touches the "Generate thumbnails" button. */
    if (playerHasVideo())
    {
        const QString mrl = currentMediaMRL();
        const qint64 duration = playerLengthMs();
        if (!mrl.isEmpty() && duration > 0
            && m_thumbnailsAutoMedia != mrl
            && !m_thumbnails.isGenerating()
            && !m_thumbnails.isSuspended()
            && m_thumbnails.count() == 0)
        {
            m_thumbnailsAutoMedia = mrl;
            generateThumbnails();
        }
    }
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
                 .arg(m_generation.hasProvider() ? m_generation.baseUrl()
                                                 : QStringLiteral("local inpainting fallback"),
                      m_generation.model().isEmpty() ? QStringLiteral("no external model")
                                                     : m_generation.model());
    if (!m_generation.unavailableReason().isEmpty())
        lines << QStringLiteral("  note: %1").arg(m_generation.unavailableReason());

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

    lines << QStringLiteral("Lookup: %1 (%2)")
                 .arg(m_lookup.isConfigured() ? QStringLiteral("available")
                                              : QStringLiteral("unavailable"),
                      m_lookup.providerName());

    lines << QStringLiteral("Tracking: %1").arg(m_tracking.isConfigured()
                                                    ? QStringLiteral("available")
                                                    : QStringLiteral("unavailable"));
    lines << QStringLiteral("  backend: %1 (%2)")
                 .arg(m_tracking.isConfigured() ? QStringLiteral("SAM3")
                                                : QStringLiteral("none"),
                      m_tracking.isConfigured() && !m_tracking.device().isEmpty()
                          ? m_tracking.device()
                          : QStringLiteral("cpu"));
    {
        int totalFrames = 0;
        for (const ObjectTrack &track : m_document.tracks())
            totalFrames += track.keyframes.size();
        const qint64 cacheBytes = qMax<qint64>(0, trackedMaskBytes());
        lines << QStringLiteral("  tracked frames: %1, mask cache: %2 MiB")
                     .arg(totalFrames)
                     .arg(cacheBytes / (1024 * 1024));
    }
    lines << QStringLiteral("Companion: mode %1, talk %2, %3")
                 .arg(m_companion.mode())
                 .arg(m_companion.talkLevel())
                 .arg(m_companion.isThinking() ? QStringLiteral("thinking")
                                               : QStringLiteral("idle"));

    /* active AI jobs */
    const QVariantList jobs = activeJobs();
    lines << QStringLiteral("Active jobs: %1").arg(jobs.size());
    for (const QVariant &v : jobs)
    {
        const QVariantMap job = v.toMap();
        lines << QStringLiteral("  - %1 (%2)").arg(job[QStringLiteral("name")].toString(),
                                                   job[QStringLiteral("resource")].toString());
    }
    lines << QStringLiteral("Autosave: %1").arg(autosavePath());

    if (!m_statusMessage.isEmpty())
        lines << QStringLiteral("Last status: %1").arg(m_statusMessage);
    return lines.join(QLatin1Char('\n'));
}

} // namespace paintbrush