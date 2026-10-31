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
#include <QtMath>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
/* SetCurrentProcessExplicitAppUserModelID lives in shell32 (resolved via
 * the VLC core's imports); MinGW's shellapi.h gates it behind
 * NTDDI_VERSION, so declare it explicitly. */
extern "C" __declspec(dllimport) HRESULT WINAPI
    SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);
#endif
#include <QFileInfo>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QStandardPaths>
#include <QDir>
#include <QPainter>
#include <QDateTime>
#include <QUuid>
#include <QVariantMap>
#include <QWindow>
#include <QScreen>

#include "qt.hpp"
#include "maininterface/mainctx.hpp"
#include "player/player_controller.hpp"
#include "playlist/playlist_controller.hpp"
#include <vlc_player.h>
#include <vlc_configuration.h>
#include <vlc_variables.h>

namespace paintbrush {

PaintbrushController::PaintbrushController(qt_intf_t *intf, QObject *parent)
    : QObject(parent)
    , m_intf(intf)
{
    /* layer list model for the inspector: mirrors the document and reloads
     * on structural changes (document signals) and in-place mutations
     * (controller documentChanged) */
    m_layerModel = new PaintbrushLayerModel(&m_document, this, this);

    /* Living Showroom clock: runs only while a visible living layer exists
     * and the canvas is on screen; every tick advances the canonical time
     * and recomposites the canvas through the document renderer. */
    m_livingClock.setTimerType(Qt::PreciseTimer);
    m_livingClock.setInterval(33);                     /* ~30 Hz */
    connect(&m_livingClock, &QTimer::timeout,
            this, &PaintbrushController::onLivingClockTick);
    connect(&m_document, &PaintbrushDocument::layersChanged,
            this, &PaintbrushController::refreshLivingClock);

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

    /* keep the sticker-editor track selection valid when tracks change
     * (track finished, deleted, document reset/loaded) */
    connect(&m_document, &PaintbrushDocument::tracksChanged,
            this, &PaintbrushController::sanitizeSelectedTrack);

    m_companion.setAssistantService(&m_assistant);

    connect(&m_undoStack, &UndoStack::canUndoChanged,
            this, &PaintbrushController::canUndoChanged);
    connect(&m_undoStack, &UndoStack::canRedoChanged,
            this, &PaintbrushController::canRedoChanged);
    connect(&m_undoStack, &UndoStack::maskRestored,
            this, &PaintbrushController::onMaskRestored);

    /* Paintbrush 2.0: tray + art-direction state are document-scoped */
    connect(m_document.harvest(), &HarvestTrayModel::countChanged,
            this, &PaintbrushController::harvestChanged);
    connect(&m_document, &PaintbrushDocument::artStateChanged,
            this, &PaintbrushController::artStateChanged);
    connect(&m_document, &PaintbrushDocument::artStateChanged,
            this, &PaintbrushController::applyMoodToCanvasTools);
    connect(&m_document, &PaintbrushDocument::layersChanged,
            this, &PaintbrushController::documentChanged);
    m_boostTimeout.setSingleShot(true);
    m_boostTimeout.setInterval(20000);
    connect(&m_boostTimeout, &QTimer::timeout, this, [this]() {
        if (m_boostBusy && m_boostAwaitingMask)
        {
            m_boostAwaitingMask = false;
            finishBoost(QStringLiteral("(The bridge pose timed out, so the structure pass stands on its own.)"));
        }
    });

    /* a new blank canvas, like opening Paintbrush */
    m_document.resetDocument(QSize(1280, 720));
    emit documentChanged();

#ifdef Q_OS_WIN
    /* product identity: this process is NAI PaintBrush — its own taskbar
     * group and name, never grouped with a media player */
    SetCurrentProcessExplicitAppUserModelID(L"NAI.PaintBrush");
#endif

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
    m_thumbnails.cancel();
    m_segmentation.shutdown();
    m_tracking.shutdown();
    m_editSource.shutdown();
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
    m_lookup.configure(VLC_OBJECT(m_intf));

    /* the edit decode source: dedicated exact-frame decoder for the
     * edit/AI pipeline (thumbnails, tracking, sticker export, crops) */
    m_editSource.configure(VLC_OBJECT(m_intf));
    /* async exact-frame delivery (P0#4): the interactive Object Select seed
     * decodes asynchronously and never blocks the GUI. */
    connect(&m_editSource, &EditFrameSource::frameDecoded,
            this, &PaintbrushController::onAsyncObjectFrameDecoded);
    connect(&m_editSource, &EditFrameSource::frameDecodeFailed,
            this, &PaintbrushController::onAsyncObjectFrameDecodeFailed);
    m_thumbnails.setPtsToleranceMs(qMax<qint64>(
        20, pbConfigInt(VLC_OBJECT(m_intf), "paintbrush-edit-pts-tolerance-ms")));

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

    /* detect whether any secret (API key) is configured — the AI Settings
     * surface then shows a "configured" placeholder and never the value */
    vlc_object_t *cfgObj = VLC_OBJECT(m_intf);
    const bool hasSecret =
        !pbApiKey(cfgObj, "PAINTBRUSH_VLM_API_KEY", "paintbrush-vlm-api-key").isEmpty()
        || !pbApiKey(cfgObj, "PAINTBRUSH_IMAGE_API_KEY", "paintbrush-image-api-key").isEmpty()
        || !pbApiKey(cfgObj, "PAINTBRUSH_ASSISTANT_API_KEY", "paintbrush-assistant-api-key").isEmpty();
    m_settingsSecretsPresent = hasSecret;

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
    connect(m_canvas, &PaintbrushCanvasItem::askClick,
            this, &PaintbrushController::onCanvasAskClick);
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
    refreshLivingClock();

    /* First attach: fit the initial document into the view so the paper
     * fills the window and the user paints where they look (on a large
     * display a zoom=1 1280x720 doc is only a corner patch, so strokes
     * outside it silently clipped to nothing). The QML item usually has no
     * size yet at this point, so retry on the first real size. */
    auto fitted = std::make_shared<bool>(false);
    auto tryFit = [canvas, fitted]() {
        if (*fitted)
            return;
        if (canvas->width() > 0 && canvas->height() > 0)
        {
            *fitted = true;
            canvas->fitToView();
        }
    };
    tryFit();
    connect(canvas, &QQuickItem::widthChanged, canvas, tryFit);
    connect(canvas, &QQuickItem::heightChanged, canvas, tryFit);
    /* Keep the live Object Select frame<->viewport transforms in step with the
     * actual canvas viewport (P0#2/#3): recompute on resize so a seed click or
     * refine stroke maps to the correct native pixel even after a relayout. */
    connect(canvas, &QQuickItem::widthChanged,
            this, &PaintbrushController::onCanvasResized);
    connect(canvas, &QQuickItem::heightChanged,
            this, &PaintbrushController::onCanvasResized);
}

/* ------------------------------------------------- Living Showroom clock */

void PaintbrushController::setShowroomPreviewActive(bool active)
{
    if (m_showroomPreview == active)
        return;
    m_showroomPreview = active;
    refreshLivingClock();
}

void PaintbrushController::setLivingPreviewPaused(bool paused)
{
    if (m_previewPaused == paused)
        return;
    m_previewPaused = paused;
    refreshLivingClock();
}

void PaintbrushController::refreshLivingClock()
{
    const bool want = m_showroomPreview && !m_previewPaused && m_canvas
        && m_canvas->isVisible() && m_document.hasVisibleLivingLayer();
    if (want == m_livingClock.isActive())
        return;
    if (want)
        m_livingClock.start();
    else
    {
        m_livingClock.stop();
        /* rest at the static poster (t=0): recomposite once */
        m_animTimeMs = 0;
        if (m_canvas)
            m_canvas->setAnimTime(0);
    }
}

void PaintbrushController::onLivingClockTick()
{
    if (!m_canvas)
        return;
    /* gate re-check per tick: layer hidden/removed, canvas minimized or
     * workspace hidden -> the clock stops on its own */
    if (!m_showroomPreview || m_previewPaused || !m_canvas->isVisible()
        || !m_document.hasVisibleLivingLayer())
    {
        refreshLivingClock();
        return;
    }
    /* editing gestures (stroke/interaction) drop the refresh rate so the
     * preview never fights the user's input */
    const int interval = m_canvas->strokeActive() ? 100 : 33; /* 10 vs 30 Hz */
    if (m_livingClock.interval() != interval)
        m_livingClock.setInterval(interval);
    m_animTimeMs += interval;
    m_canvas->setAnimTime(m_animTimeMs);
}

void PaintbrushController::fitInterfaceToDesktop()
{
    /* Clamp the Paintbrush interface window to the current screen so the
     * bottom chrome (palette bar + media drawer) is always visible. When the
     * window was restored larger than the desktop (e.g. saved geometry from a
     * bigger/4K display, or a DPI/virtual-screen mismatch), the bottom UX
     * would otherwise fall behind the taskbar and look "cut off". */
    if (!m_intf || !m_intf->p_mi)
        return;
    QWindow *window = m_intf->p_mi->intfMainWindow();
    if (!window || !window->screen())
        return;

    const QScreen *screen = window->screen();
    const QRect avail = screen->availableGeometry();
    const QRect win = window->geometry();
    if (win.isEmpty())
        return;

    /* device pixel ratio: geometry() is in native pixels; availableGeometry()
     * is in device-independent pixels. Scale the available frame to native
     * units so both sides are compared in the same space. */
    const qreal dpr = window->devicePixelRatio();
    const QRect availNative = QRect(avail.topLeft() * dpr, avail.size() * dpr);

    /* Only shrink if the window is meaningfully larger than the screen;
     * never make it smaller than the screen, never grow it. */
    if (win.size().width() <= availNative.width()
        && win.size().height() <= availNative.height())
        return;

    QSize fit = win.size();
    fit = fit.boundedTo(availNative.size());
    /* keep at least a usable canvas area */
    fit = fit.expandedTo(QSize(availNative.width() / 2, availNative.height() / 2));
    window->resize(fit);
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

    /* opening a video starts a fresh live session (any previous selection is
     * reset — no stale prompts/mask survive) and switches the central area to
     * Harvest presentation (live video as the visual source). */
    resetLiveSelectionSession();
    enterHarvestPresentation();

    /* A Paintbrush video session is the main VLC player (PlaybackSession):
     * append the file to the playlist and start playing it, then the media
     * drawer appears and frames can be captured/tracked. Playback keeps the
     * DEFAULT decode policy (Auto) — it is never forced to software decode
     * anymore (A18). Exact-frame work (thumbnails, tracking, sticker
     * export, AI crops) never seeks/snapshots this session: it decodes
     * through EditFrameSource, which applies the generic avcodec-hwdec
     * tri-state to its own dedicated decoder only. */
    vlc::playlist::Media media(QUrl::fromLocalFile(path).toString(),
                               QFileInfo(path).fileName());
    m_intf->p_mainPlaylistController->append(
        QVector<vlc::playlist::Media>{media}, true);
    emit mediaStateChanged();
    setStatus(QStringLiteral("Playing %1").arg(QFileInfo(path).fileName()));
    pbLogInfo(VLC_OBJECT(m_intf), "media", "opened %s (playback decode policy: auto)",
              qUtf8Printable(path));

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
    if (!playerHasVideo())
    {
        setStatus(QStringLiteral("No video frame is available to capture."));
        return;
    }

    const QString mrl = currentMediaMRL();
    const qint64 pts = playerTimeMs();

    /* Exact-frame decode first: the edit source returns the DECODED frame
     * with a verified PTS (no PNG round-trip). The viewer snapshot path is
     * only a fallback when the edit source cannot decode (e.g. a stream
     * the thumbnailing input cannot open). */
    QImage frame;
    qint64 actualPts = pts;
    bool ptsVerified = false;
    if (!mrl.isEmpty())
    {
        const DecodedFrame f = requestFrame(mrl, pts);
        if (f.ok())
        {
            frame = f.image;
            actualPts = f.ptsMs;
            ptsVerified = true;
        }
    }
    if (frame.isNull())
    {
        /* generic viewer screenshot utility (kept out of the edit path) */
        vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
        frame = FrameCaptureService::captureCurrentFrame(player);
        const FrameRef ref = FrameCaptureService::currentMediaRef(player);
        if (!mrl.isEmpty())
            actualPts = ref.ptsMs;
    }
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

    /* store the exact frame identity (A19): AI/crop paths re-decode THIS
     * frame through the FrameProvider instead of scanning layers */
    const QString title = currentMediaTitle();
    m_document.setSourceFrame(FrameRef{mrl, title, actualPts,
                                       frame.width(), frame.height()});

    /* adopt the frame size on a blank canvas */
    const bool blank = (m_document.layerCount() <= 1 &&
                        m_document.currentLayer() &&
                        m_document.currentLayer()->isEmpty());
    if (blank)
        m_document.resetDocument(frame.size());

addImageLayer(frame, QPointF(0, 0),
                  title.isEmpty() ? QStringLiteral("Video frame") : title,
                  LAYER_REFERENCE);

    /* For an EXISTING composition (P1#9): the source raster stays at its
     * native decoded size — the fit/center is done through the layer
     * transform (scale + centered pivot), never baked into the raster.
     * Aspect ratio is preserved. On a BLANK canvas the document adopts the
     * frame's display size and the frame is inserted 1:1. */
    if (!blank && !frame.size().isEmpty())
    {
        const QSize canvas = m_document.canvasSize();
        if (!canvas.isEmpty())
        {
            const qreal scale = qMin(canvas.width() / qreal(frame.width()),
                                     canvas.height() / qreal(frame.height()));
            const int idx = m_document.layerCount() - 1;
            if (idx >= 0)
            {
                moveImageLayer(idx, QPointF(
                    (canvas.width() - frame.width() * scale) / 2.0,
                    (canvas.height() - frame.height() * scale) / 2.0));
                transformLayer(idx, qBound(0.01, scale, 100.0), 0.0);
            }
        }
    }

    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    emit mediaStateChanged();
    setStatus(QStringLiteral("Captured video frame at %1 ms%2")
                  .arg(actualPts)
                  .arg(ptsVerified ? QString() : QStringLiteral(" (unverified)")));
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
    /* media replacement invalidates any live selection made on the old MRL */
    resetLiveSelectionSession();
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

void PaintbrushController::openRecoveryCopy()
{
    /* Open the recovered autosave as a *copy*: load its content into an
     * unsaved, non-project document but leave the recovery file untouched on
     * disk, so the original autosave is never consumed by merely viewing a
     * copy of it. The banner is dismissed for this session because the content
     * is now visible on the canvas. */
    if (m_recoveryPath.isEmpty())
        return;
    const QString err = m_projectStore.load(&m_document, m_recoveryPath);
    if (!err.isEmpty())
    {
        setStatus(QStringLiteral("Open copy failed: %1").arg(err));
        return;
    }
    m_document.setProjectPath({});
    m_document.setDirty(true);
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->setTool(m_canvas->tool());
    }
    m_undoStack.clear();
    /* Do NOT remove m_recoveryPath: this is a copy, the original stays on disk. */
    m_recoveryAvailable = false;
    m_recoveryPath.clear();
    m_recoveryDescription.clear();
    emit documentChanged();
    emit recoveryStateChanged();
    setStatus(QStringLiteral("Opened a copy of the recovered project (original kept)"));
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
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Add layer");
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = m_document.layerAt(cmd.index) ? *m_document.layerAt(cmd.index)
                                              : CanvasLayer();
    m_undoStack.pushInsertLayer(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::deleteLayer(int index)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size() || layers.size() <= 1)
        return;

    DeleteLayerCommand cmd;
    cmd.desc = QStringLiteral("Delete layer");
    cmd.index = index;
    cmd.layer = layers[index]; /* immutable copy: the undo payload */
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    layers.removeAt(index);
    m_document.setLayers(layers);
    if (m_document.currentLayerIndex() >= layers.size())
        m_document.setCurrentLayerIndex(layers.size() - 1);
    m_undoStack.pushDeleteLayer(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::duplicateLayer(int index)
{
    QList<CanvasLayer> layers = m_document.layers();
    if (index < 0 || index >= layers.size())
        return;

    CanvasLayer copy = layers[index];
    copy.name = copy.name + QStringLiteral(" copy");
    copy.id = 0; /* fresh stable id for the copy */
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Duplicate layer");
    layers.insert(index + 1, std::move(copy));
    cmd.index = index + 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = m_document.layerAt(cmd.index) ? *m_document.layerAt(cmd.index)
                                              : CanvasLayer();
    m_undoStack.pushInsertLayer(cmd);
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
    CanvasLayer *layer = m_document.layerAt(index);
    if (!layer || layer->visible == visible)
        return;

    SetVisibilityCommand cmd;
    cmd.desc = QStringLiteral("Toggle layer visibility");
    cmd.layerIndex = index;
    cmd.before = layer->visible;
    cmd.after = visible;
    layer->visible = visible;
    m_undoStack.pushSetVisibility(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::setLayerOpacity(int index, double opacity)
{
    CanvasLayer *layer = m_document.layerAt(index);
    if (!layer)
        return;
    opacity = qBound(0.0, opacity, 1.0);
    if (qFuzzyCompare(float(opacity), layer->opacity))
        return;

    SetOpacityCommand cmd;
    cmd.desc = QStringLiteral("Set layer opacity");
    cmd.layerIndex = index;
    cmd.before = layer->opacity;
    layer->opacity = float(opacity);
    cmd.after = layer->opacity;
    m_undoStack.pushSetOpacity(cmd);
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

    ReorderCommand cmd;
    cmd.desc = QStringLiteral("Reorder layer");
    cmd.index = index;
    cmd.target = target;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    layers.move(index, target);
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(target);
    m_undoStack.pushReorder(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::transformLayer(int index, double scale, double rotation)
{
    CanvasLayer *layer = m_document.layerAt(index);
    if (!layer)
        return;
    if (layer->type != LAYER_IMAGE && layer->type != LAYER_TEXT &&
        layer->type != LAYER_STICKER)
        return;
    scale = qBound(0.01, scale, 64.0);
    rotation = qBound(-360.0, rotation, 360.0);
    if (qFuzzyCompare(float(scale), layer->scale) &&
        qFuzzyCompare(float(rotation), layer->rotation))
        return;

    SetTransformCommand cmd;
    cmd.desc = QStringLiteral("Transform layer");
    cmd.layerId = layer->id;
    cmd.layerIndex = index;
    cmd.before = layer->transform();
    layer->scale = float(scale);
    layer->rotation = float(rotation);
    cmd.after = layer->transform();
    m_undoStack.pushSetTransform(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::moveImageLayer(int index, const QPointF &position)
{
    CanvasLayer *layer = m_document.layerAt(index);
    if (!layer)
        return;
    if (layer->type != LAYER_IMAGE && layer->type != LAYER_TEXT &&
        layer->type != LAYER_STICKER)
        return;
    if (layer->position == position)
        return;

    SetTransformCommand cmd;
    cmd.desc = QStringLiteral("Move image layer");
    cmd.layerId = layer->id;
    cmd.layerIndex = index;
    cmd.before = layer->transform();
    layer->position = position;
    cmd.after = layer->transform();
    m_undoStack.pushSetTransform(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

/* ------------------------------------------- capability-honest status (S5) */

QString PaintbrushController::generationStatusText() const
{
    /* Truth comes from the generation service's CapabilityReport: the local
     * heal/blur fallbacks are named as what they are (pixel operations, not
     * generative), remote-generation names the configured model, and the
     * unavailable branch carries the service's own reason. */
    const CapabilityReport report =
        m_generation.capability(GenerationService::ACTION_GENERATIVE_FILL);
    switch (report.mode)
    {
    case CapabilityReport::RemoteGeneration:
        return QStringLiteral("Remote generation (%1)")
            .arg(report.modelName.isEmpty() ? QStringLiteral("default model")
                                            : report.modelName);
    case CapabilityReport::LocalHeal:
        return QStringLiteral("Local heal — pixel propagation, not generative");
    case CapabilityReport::LocalBlur:
        return QStringLiteral("Local blur — pixel filter, not generative");
    case CapabilityReport::Unavailable:
        break;
    }
    return m_generation.unavailableReason().isEmpty()
               ? QStringLiteral("Unavailable")
               : m_generation.unavailableReason();
}

QString PaintbrushController::visionStatusText() const
{
    if (m_vision.isConfigured())
        return QStringLiteral("Ready (%1)")
            .arg(m_vision.model().isEmpty() ? QStringLiteral("default model")
                                            : m_vision.model());
    return m_vision.unavailableReason().isEmpty()
               ? QStringLiteral("Unconfigured")
               : QStringLiteral("Unconfigured — %1")
                     .arg(m_vision.unavailableReason());
}

QString PaintbrushController::segmentationStatusText() const
{
    if (m_segmentation.isConfigured())
        return QStringLiteral("Ready");
    return m_segmentation.unavailableReason().isEmpty()
               ? QStringLiteral("Unavailable")
               : m_segmentation.unavailableReason();
}

/* ------------------------------------------------------ object selection */

void PaintbrushController::onCanvasObjectClick(QPointF canvasPos, bool positive,
                                              QPointF itemPos)
{
    Q_UNUSED(canvasPos);
    if (!m_segmentation.isConfigured())
    {
        setStatus(m_segmentation.unavailableReason());
        return;
    }

    /* The viewport is the actual displayed video area (the canvas item's
     * size). P0#2: the pointer position MUST be mapped from the QML/video
     * viewport coordinate system (`itemPos`), NOT from document/content
     * coordinates (`canvasPos`) — `captureLiveFrameContext` builds the
     * transforms against the viewport, and `itemPos` already is a real
     * viewport point. */
    const QSize viewport = m_canvas
                               ? m_canvas->size().toSize()
                               : m_document.canvasSize();
    if (viewport.isEmpty())
        return;

    if (playerHasVideo())
    {
        /* ---- LIVE video seed ---------------------------------------- */
        /* The viewport the seed click was made on. `adoptAsyncFrameIntoSelection`
         * builds the frame<->viewport letterbox transform against THIS size so
         * a real viewport click maps to the correct native pixel — the empty-
         * viewport default in buildSelectionTransforms must never be used for
         * a live seed (P0#2). */
        m_asyncViewportSize = viewport;

        const QString mrl = currentMediaMRL();
        if (mrl.isEmpty())
        {
            setStatus(QStringLiteral("No video frame to select from (open a video "
                                     "or capture a frame to canvas first)."));
            return;
        }
        const qint64 pts = playerTimeMs();
        const FrameRef shot{mrl, currentMediaTitle(), pts};

        /* Session scoping: a change of source frame (MRL or PTS) starts a
         * fresh prompt session — a 9s click must never join a 15s click's
         * inference. Same MRL+PTS keeps the session (repeated refinement
         * benefits from the SAM embedding cache). */
        if (m_liveSessionRef.mrl != shot.mrl ||
            m_liveSessionRef.ptsMs != shot.ptsMs)
        {
            resetLiveSelectionSession();
            m_liveSessionRef = shot;
        }

        /* Freeze the visual frame: a SAM selection belongs to one exact
         * FrameRef. Pause playback so the live video cannot move underneath
         * the pending/result mask (P0#5). */
        if (!m_selectionFrozen)
        {
            m_selectionFrozen = true;
            m_frozenPtsMs = pts;
            if (playerIsPlaying())
                m_intf->p_mainPlaylistController->togglePlayPause();
        }

        /* Queue the click as a viewport point; it is mapped into frame space
         * once the exact frame arrives (the mapping depends on the decoded
         * frame size, and decode is async so the GUI never blocks — P0#4). */
        if (positive)
            m_posItemPoints.append(itemPos);
        else
            m_negItemPoints.append(itemPos);

        m_objectSelectBusy = true;
        emit objectSelectionChanged();
        if (m_canvas)
            m_canvas->setObjectSelectBusy(true);

        if (!m_selection.validFrame())
        {
            /* No live frame context yet: kick off the async exact decode.
             * If one is already in flight for this session, it is reused. */
            if (!m_asyncInFlight)
                requestAsyncObjectFrame(shot, m_objectSessionId);
        }
        else if (m_selection.matchesFrame(shot))
        {
            /* We already hold the frame for this exact PTS: drain the pending
             * viewport click into frame space and run SAM now. */
            drainPendingItemPoints();
            runSegmentationForPendingPoints(shot);
        }
        else
        {
            /* The held frame belongs to a different PTS — must re-decode.
             * resetLiveSelectionSession() cleared the pending item list, so
             * re-queue the click before issuing the async decode. */
            resetLiveSelectionSession();
            m_liveSessionRef = shot;
            /* Re-freeze at the NEW PTS: resetLiveSelectionSession cleared the
             * freeze state (P0#5 invariance). The media is already paused
             * from the first freeze, so only re-arm the guard so resuming
             * playback (or seeking) correctly ends this new session. */
            m_selectionFrozen = true;
            m_frozenPtsMs = shot.ptsMs;
            if (positive)
                m_posItemPoints.append(itemPos);
            else
                m_negItemPoints.append(itemPos);
            m_objectSelectBusy = true;
            emit objectSelectionChanged();
            if (m_canvas)
                m_canvas->setObjectSelectBusy(true);
            requestAsyncObjectFrame(shot, m_objectSessionId);
        }
        return;
    }

    /* ---- still / compose seed -------------------------------------- */
    ObjectSelectionContext ctx = composeFrameContext(viewport);
    if (!ctx.validFrame())
    {
        setStatus(QStringLiteral("No video frame to select from (open a video "
                                 "or capture a frame to canvas first)."));
        return;
    }
    const QPointF framePt = ctx.itemToFramePoint(itemPos);
    if (qIsNaN(framePt.x()) || qIsNaN(framePt.y()) ||
        framePt.x() < 0 || framePt.y() < 0 ||
        framePt.x() >= ctx.frame.width() || framePt.y() >= ctx.frame.height())
    {
        setStatus(QStringLiteral("Object Select needs a point inside the video."));
        return;
    }
    /* Fresh session for a compose seed. */
    m_selection = ctx;
    m_selection.sessionId = m_objectSessionId;
    QVector<QPointF> pos = m_posPoints, neg = m_negPoints;
    pos.detach(); neg.detach();
    if (positive)
        pos.append(framePt);
    else
        neg.append(framePt);
    m_posPoints = pos;
    m_negPoints = neg;
    m_objectSelectBusy = true;
    emit objectSelectionChanged();
    if (m_canvas)
        m_canvas->setObjectSelectBusy(true);
    m_segmentation.segment(ctx.frame, m_posPoints, m_negPoints, {});
}

void PaintbrushController::onCanvasAskClick(QPointF canvasPos, QPointF itemPos)
{
    Q_UNUSED(canvasPos);
    Q_UNUSED(itemPos);

    if (!m_vision.isConfigured())
    {
        setStatus(m_vision.unavailableReason());
        return;
    }

    /* Ask-anything (P1#11): establishing visual context only — the user then
     * types their question in the compact Ask input. We NEVER auto-send a
     * fixed prompt. The target (object / selection / whole frame) is stored so
     * Submit routes the typed question to the correct context. */
    if (m_objectSelected && !m_objectMask.isNull())
    {
        m_askTarget = 2;
        m_askInputPlaceholder = tr("Ask about the selected object…");
    }
    else if (m_canvas && m_canvas->hasSelection())
    {
        m_askTarget = 1;
        m_askInputPlaceholder = tr("Ask about this selection…");
    }
    else
    {
        m_askTarget = 0;
        m_askInputPlaceholder = tr("Ask about this frame…");
    }
    m_askInputVisible = true;
    emit askInputChanged();
}

void PaintbrushController::beginAsk()
{
    if (!m_vision.isConfigured())
    {
        setStatus(m_vision.unavailableReason());
        return;
    }
    if (m_objectSelected && !m_objectMask.isNull())
    {
        m_askTarget = 2;
        m_askInputPlaceholder = tr("Ask about the selected object…");
    }
    else if (m_canvas && m_canvas->hasSelection())
    {
        m_askTarget = 1;
        m_askInputPlaceholder = tr("Ask about this selection…");
    }
    else
    {
        m_askTarget = 0;
        m_askInputPlaceholder = tr("Ask about this frame…");
    }
    m_askInputVisible = true;
    emit askInputChanged();
}

void PaintbrushController::submitAsk(const QString &question)
{
    const QString q = question.trimmed();
    if (q.isEmpty())
        return;
    if (!m_vision.isConfigured())
    {
        setStatus(m_vision.unavailableReason());
        return;
    }
    m_askInputVisible = false;
    emit askInputChanged();
    if (m_askTarget == 2)
        askAboutObject(q);
    else if (m_askTarget == 1)
        askAboutSelection(q);
    else
    {
        /* whole frame: use the exact selection/frame context — currentAiSource
         * already prefers the live ObjectSelectionContext's native frame. */
        m_visionConversationId = QStringLiteral("frame-%1")
                                     .arg(QDateTime::currentMSecsSinceEpoch());
        m_visionHasResult = false;
        emit visionStateChanged();
        m_vision.ask(QImage(), currentAiSource(), q,
                     m_visionConversationId);
        setStatus(QStringLiteral("Asking about this frame…"));
    }
}

void PaintbrushController::cancelAsk()
{
    if (m_askInputVisible)
    {
        m_askInputVisible = false;
        emit askInputChanged();
    }
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
    /* keep the authoritative native mask in the selection context too */
    if (m_objectSelected)
        m_selection.mask = mask;
    updateObjectSelection();
    emit objectSelectionChanged();
    /* one mask-refine stroke == one undoable operation */
    if (!before.isNull() && before != mask)
    {
        MaskCommand cmd;
        cmd.data.before = before;
        cmd.data.after = mask;
        cmd.data.description = QStringLiteral("Refine selection mask");
        m_undoStack.pushMask(cmd);
    }
    /* live mode: rebuild the viewport display overlay from the refined
     * native mask (the canvas's paint path already did, but ensure the
     * selection context and VLM crop see the same authoritative mask) */
    if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
        && m_canvas && m_selection.validFrame()
        && mask.size() == m_selection.frame.size())
        m_canvas->setLiveHarvestOverlay(mask, m_selection.contentRect,
                                        m_selection.frameToItem);
    setStatus(QStringLiteral("Selection mask refined with brush."));
}

void PaintbrushController::onMaskRestored(const QImage &mask)
{
    m_objectMask = mask;
    m_objectSelected = !mask.isNull();
    if (m_objectSelected)
        m_selection.mask = mask;
    if (m_canvas)
    {
        if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
            && m_selection.validFrame()
            && mask.size() == m_selection.frame.size())
            m_canvas->setLiveHarvestOverlay(mask, m_selection.contentRect,
                                            m_selection.frameToItem);
        else
        {
            m_canvas->setObjectMask(mask, 1.0);
        }
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
    if (m_boostAwaitingMask)
    {
        /* this mask was requested by Boost for the bridge candidate frame */
        m_boostAwaitingMask = false;
        boostInsertBridge(mask, score);
        return;
    }

    /* Discard a stale SAM result: if a newer selection session started (the
     * media/PTS moved on) this mask no longer belongs to the current frame.
     * `m_segmentTargetRef` records which frame the in-flight request targeted;
     * an empty target (no SAM issued) or a PTS change means it is stale. */
    const bool targetIsLive = m_segmentTargetRef.isValid();
    m_segmentInFlight = false;
    if (targetIsLive && m_liveSessionRef.isValid())
    {
        const bool stillCurrent =
            m_liveSessionRef.mrl == m_segmentTargetRef.mrl
            && m_liveSessionRef.ptsMs == m_segmentTargetRef.ptsMs;
        if (!stillCurrent)
        {
            pbLogInfo(VLC_OBJECT(m_intf), "segmentation",
                      "discarding stale SAM mask for %lld ms (now at %lld ms)",
                      (long long)m_segmentTargetRef.ptsMs,
                      (long long)m_liveSessionRef.ptsMs);
            return; /* stale response for a different frame */
        }
    }

    m_objectMask = mask;          /* authoritative native (frame/canvas) mask */
    m_objectMaskScore = score;
    m_objectSelectBusy = false;
    m_objectSelected = !mask.isNull();
    if (m_objectSelected)
        m_selection.mask = mask;  /* keep the authoritative mask in the ctx  */

    if (m_canvas)
    {
        /* Live video selection: display the native mask through the
         * viewport-space live overlay (never rescaled destructively; only
         * the display path maps it into item coords). Compose/stills: show
         * it in document space as before. */
        if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
            && m_selection.validFrame()
            && mask.size() == m_selection.frame.size())
        {
            m_canvas->setLiveHarvestOverlay(mask, m_selection.contentRect,
                                            m_selection.frameToItem);
        }
        else
        {
            m_canvas->setObjectMask(mask, score);
        }
        m_canvas->setObjectSelectBusy(false);
    }
    updateObjectSelection();
    emit objectSelectionChanged();
    setStatus(QStringLiteral("Object selected (confidence %.0f%%)").arg(score * 100.0));
}

void PaintbrushController::onSegmentationError(const QString &message)
{
    if (m_boostAwaitingMask)
    {
        m_boostAwaitingMask = false;
        finishBoost(QStringLiteral("(No clean bridge pose was found, so the structure pass stands on its own.)"));
        return;
    }
    m_segmentInFlight = false;
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
    /* For a LIVE selection the mask is native frame space; report its true
     * bounds. For a compose selection use the document-space canvas bounds. */
    QRectF bounds;
    if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
        && !m_selection.mask.isNull())
    {
        int minX = m_selection.mask.width(), minY = m_selection.mask.height();
        int maxX = -1, maxY = -1;
        for (int y = 0; y < m_selection.mask.height(); ++y)
        {
            const uchar *line = m_selection.mask.constScanLine(y);
            for (int x = 0; x < m_selection.mask.width(); ++x)
                if (line[x] >= 128)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
        }
        if (maxX >= 0)
            bounds = QRectF(minX, minY, maxX - minX + 1, maxY - minY + 1);
    }
    else
    {
        bounds = m_canvas ? m_canvas->objectMaskBounds() : QRectF();
    }
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
    if (m_objectSelected)
        m_selection.mask = mask; /* keep the authoritative native mask in sync */
    m_objectMaskSoftEdge = false;
    if (m_canvas)
    {
        if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
            && m_selection.validFrame() && mask.size() == m_selection.frame.size())
            m_canvas->setLiveHarvestOverlay(mask, m_selection.contentRect,
                                            m_selection.frameToItem);
        else
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
    if (m_objectSelected)
        m_selection.mask = blurred; /* keep probe native mask in sync */
    m_objectMaskSoftEdge = true;
    if (m_canvas)
    {
        if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
            && m_selection.validFrame()
            && blurred.size() == m_selection.frame.size())
            m_canvas->setLiveHarvestOverlay(blurred, m_selection.contentRect,
                                            m_selection.frameToItem);
        else
            m_canvas->setObjectMask(blurred, 1.0);
        m_canvas->markCanvasDirty();
    }
    emit objectMaskEdgeChanged();
    emit objectSelectionChanged();
    setStatus(QStringLiteral("Object mask edge set to soft."));
}

FrameRef PaintbrushController::selectionFrameRef() const
{
    /* The one authoritative source ref for the current selection: the live
     * ObjectSelectionContext's exact FrameRef when media-backed, else the
     * document's mutable source ref. Every object action consumes this so
     * Keep / cutout / export / track / sticker all agree on which frame the
     * current mask belongs to (P0#6). */
    if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
        && m_selection.ref.isValid())
        return m_selection.ref;
    if (m_document.sourceFrame().isValid())
        return m_document.sourceFrame();
    return FrameRef{m_document.sourceMediaMRL(), m_document.sourceMediaTitle(),
                    m_document.sourceTimeMs()};
}

QImage PaintbrushController::pristineSourceFrame() const
{
    /* The reference layer holds the true captured video frame. The canvas
     * composite mixes user strokes and generated layers over it, so AI
     * operations must read the reference raster directly — otherwise the
     * VLM/segmenter is shown a synthetic reconstruction, not the frame.
     *
     * A19: the frame identity is the document's stored FrameRef, decoded
     * through the edit frame source (FrameProvider) — never "the first
     * LAYER_REFERENCE in the layer list", which could be a stale frame. */
    const FrameRef ref = m_document.sourceFrame();
    if (ref.isValid())
    {
        const DecodedFrame f = m_editSource.request(ref);
        if (!f.image.isNull())
            return f.image;
    }
    /* legacy fallback: a still captured before the edit source existed */
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
    /* For a LIVE selection the authoritative source pixels are the exact
     * decoded frame in the selection context (P0#6) — never guess. */
    if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
        && !m_selection.frame.isNull())
    {
        const QImage &frame = m_selection.frame;
        if (frame.size() != m_document.canvasSize())
            return frame.scaled(m_document.canvasSize(), Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
        return frame;
    }
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

ObjectSelectionContext PaintbrushController::captureLiveFrameContext(const QSize &viewportSize)
{
    ObjectSelectionContext ctx;
    ctx.viewportSize = viewportSize.isEmpty()
                           ? m_document.canvasSize()
                           : viewportSize;
    ctx.sourceKind = ObjectSelectionContext::SourceNone;
    ctx.itemToFrame = QTransform();
    ctx.frameToItem = QTransform();

    /* Live harvest: decode the CURRENT player PTS into an ephemeral frame —
     * always fresh, never a stale earlier frame. We do NOT touch the
     * document's mutable sourceFrame (that is captured-frame/composition
     * provenance, not the live player cursor). */
    const QString mrl = currentMediaMRL();
    if (!mrl.isEmpty() && playerHasVideo())
    {
        const DecodedFrame f = requestFrame(mrl, playerTimeMs());
        if (f.ok() && !f.image.isNull())
        {
            ctx.ref = FrameRef{mrl, currentMediaTitle(), f.ptsMs,
                               f.image.width(), f.image.height()};
            ctx.frame = f.image;
            ctx.sourceKind = ObjectSelectionContext::SourceLiveVideo;
            if (m_welcomeVisible)
            {
                m_welcomeVisible = false;
                emit welcomeChanged();
            }
        }
    }
    buildSelectionTransforms(ctx);
    return ctx;
}

ObjectSelectionContext PaintbrushController::composeFrameContext(const QSize &viewportSize) const
{
    ObjectSelectionContext ctx;
    ctx.viewportSize = viewportSize;
    ctx.sourceKind = ObjectSelectionContext::SourceStills;
    ctx.itemToFrame = QTransform();
    ctx.frameToItem = QTransform();
    QImage frame = pristineSourceFrame();
    if (frame.isNull() && m_canvas)
        frame = m_canvas->grabCanvasRegion(
            QRectF(QPointF(0, 0), m_document.canvasSize()));
    ctx.frame = frame;
    ctx.ref = m_document.sourceFrame();
    buildSelectionTransforms(ctx);
    return ctx;
}

bool PaintbrushController::adoptAsyncFrameIntoSelection(const FrameRef &ref,
                                                        const QImage &frame)
{
    if (frame.isNull())
        return false;
    m_selection.sourceKind = ObjectSelectionContext::SourceLiveVideo;
    m_selection.ref = ref;
    m_selection.frame = frame;
    m_selection.sessionId = m_objectSessionId;
    /* Build the frame<->viewport letterbox transform against the viewer the
     * seed click was made on — never the empty-viewport 1:1 default (P0#2). */
    m_selection.viewportSize = !m_asyncViewportSize.isEmpty()
                                   ? m_asyncViewportSize
                                   : (m_canvas ? m_canvas->size().toSize()
                                               : m_document.canvasSize());
    buildSelectionTransforms(m_selection);
    if (m_welcomeVisible)
    {
        m_welcomeVisible = false;
        emit welcomeChanged();
    }
    return m_selection.validFrame();
}

void PaintbrushController::onCanvasResized()
{
    /* Only a live selection carries viewport-dependent letterbox transforms.
     * If it is active and we have a native frame, recompute the frame<->
     * viewport mapping against the current canvas size and refresh the
     * display overlay so the selection stays glued to the video and a later
     * refine/seed click maps to the correct native pixel (P0#2/#3). */
    if (m_selection.sourceKind != ObjectSelectionContext::SourceLiveVideo
        || !m_selection.validFrame() || !m_canvas)
        return;
    const QSize viewport = m_canvas->size().toSize();
    if (viewport.isEmpty())
        return;
    /* Reuse the same letterbox transform the seed-time adopt built. The frame
     * is unchanged; only the viewport moved. */
    if (m_selection.frame.size() == m_selection.mask.size()
        && !m_selection.mask.isNull())
    {
        m_selection.viewportSize = viewport;
        buildSelectionTransforms(m_selection);
        m_canvas->setLiveHarvestOverlay(m_selection.mask,
                                        m_selection.contentRect,
                                        m_selection.frameToItem);
    }
}

/* Map the pending viewport click points into frame space using the current
 * selection transforms, and append them to the frame-space prompt sets. */
void PaintbrushController::drainPendingItemPoints()
{
    if (m_posItemPoints.isEmpty() && m_negItemPoints.isEmpty())
        return;
    if (!m_selection.validFrame())
    {
        /* No frame yet — keep the points pending. */
        return;
    }
    QVector<QPointF> pos = m_posPoints, neg = m_negPoints;
    pos.detach(); neg.detach();
    for (const QPointF &p : m_posItemPoints)
        pos.append(m_selection.itemToFramePoint(p));
    for (const QPointF &p : m_negItemPoints)
        neg.append(m_selection.itemToFramePoint(p));
    m_posItemPoints.clear();
    m_negItemPoints.clear();
    m_posPoints = pos;
    m_negPoints = neg;
}

void PaintbrushController::requestAsyncObjectFrame(const FrameRef &requested,
                                                   const QString &sessionId)
{
    if (m_editSource.canDecode())
    {
        m_asyncInFlight = true;
        m_asyncRequestRef = requested;
        m_asyncRequestSessionId = sessionId;
        if (!m_editSource.requestAsync(requested, sessionId))
        {
            /* Submission failed synchronously — no frameDecoded signal will
             * ever arrive, so release the in-flight bookkeeping and the busy
             * state instead of wedging the selection. */
            m_asyncInFlight = false;
            m_asyncRequestRef = FrameRef();
            m_asyncRequestSessionId.clear();
            m_asyncViewportSize = QSize();
            m_objectSelectBusy = false;
            if (m_canvas)
                m_canvas->setObjectSelectBusy(false);
            emit objectSelectionChanged();
            setStatus(QStringLiteral("Could not start the exact frame decode."));
        }
    }
    else
    {
        /* No dedicated decoder: fall back to the synchronous path so the
         * selection can still work (background/harness workflows). This is
         * the ONLY interactive place that may block, and only when the
         * async decoder is genuinely unavailable. */
        const DecodedFrame f = requestFrame(requested.mrl, requested.ptsMs);
        if (f.ok() && !f.image.isNull())
        {
            m_asyncInFlight = true;
            m_asyncRequestRef = requested;
            m_asyncRequestSessionId = sessionId;
            onAsyncObjectFrameDecoded(sessionId, f);
        }
        else
        {
            m_objectSelectBusy = false;
            if (m_canvas)
                m_canvas->setObjectSelectBusy(false);
            /* clear the in-flight flag so a later click can retry — the
             * synchronous fallback decode failed, so nothing is pending */
            m_asyncInFlight = false;
            m_asyncRequestRef = FrameRef();
            m_asyncRequestSessionId.clear();
            emit objectSelectionChanged();
            setStatus(QStringLiteral("Could not decode the exact video frame."));
        }
    }
}

void PaintbrushController::onAsyncObjectFrameDecoded(const QString &token,
                                                     const DecodedFrame &frame)
{
    if (!frame.ok() || frame.image.isNull())
    {
        onAsyncObjectFrameDecodeFailed(token, QStringLiteral("decode failed"));
        return;
    }

    /* Early stale check: if the session moved on (reset / replaced), a stale
     * async result must be discarded — the click points it maps onto are no
     * longer those of the current session. */
    if (token != m_objectSessionId)
        return;

    /* Adopt the decoded frame into the selection context. `frame.ptsMs` is
     * the VERIFIED actual PTS from the decoder — this becomes the session
     * ref so a later Keep uses the exact frame SAM segmented. */
    FrameRef ref = m_asyncRequestRef;
    ref.ptsMs = frame.ptsMs;
    ref.width = frame.image.width();
    ref.height = frame.image.height();

    m_asyncInFlight = false;
    m_asyncRequestRef = FrameRef();
    m_asyncRequestSessionId.clear();
    m_asyncViewportSize = QSize();

    if (!adoptAsyncFrameIntoSelection(ref, frame.image))
    {
        m_objectSelectBusy = false;
        if (m_canvas)
            m_canvas->setObjectSelectBusy(false);
        emit objectSelectionChanged();
        setStatus(QStringLiteral("Could not decode the exact video frame."));
        return;
    }
    /* Refresh the live session ref to the VERIFIED decoded PTS. */
    m_liveSessionRef = m_selection.ref;

    /* Map the pending viewport click points into frame space now that the
     * frame + transforms are known, then run SAM. */
    drainPendingItemPoints();

    runSegmentationForPendingPoints(m_selection.ref);
}

void PaintbrushController::onAsyncObjectFrameDecodeFailed(const QString &token,
                                                          const QString &message)
{
    if (token != m_objectSessionId)
        return; /* a stale session's failure; ignore */
    m_asyncInFlight = false;
    m_asyncRequestRef = FrameRef();
    m_asyncRequestSessionId.clear();
    m_asyncViewportSize = QSize();
    m_objectSelectBusy = false;
    if (m_canvas)
        m_canvas->setObjectSelectBusy(false);
    emit objectSelectionChanged();
    setStatus(message.isEmpty()
                  ? QStringLiteral("Could not decode the exact video frame.")
                  : message);
}

void PaintbrushController::runSegmentationForPendingPoints(const FrameRef &shot)
{
    if (!m_selection.validFrame() || !m_segmentation.isConfigured())
        return;
    m_segmentTargetRef = shot;
    m_segmentInFlight = true;
    m_objectSelectBusy = true;
    emit objectSelectionChanged();
    if (m_canvas)
        m_canvas->setObjectSelectBusy(true);
    m_segmentation.segment(m_selection.frame, m_posPoints, m_negPoints, {});
    setStatus(QStringLiteral("Selecting the object…"));
}

void PaintbrushController::resetLiveSelectionSession()
{
    m_selection = ObjectSelectionContext();
    m_liveSessionRef = FrameRef();
    m_posPoints.clear();
    m_negPoints.clear();
    m_posItemPoints.clear();
    m_negItemPoints.clear();
    m_objectMask = QImage();
    m_objectSelected = false;
    m_objectMaskScore = 0.0;
    m_objectSelectBusy = false;
    m_selectionFrozen = false;
    m_frozenPtsMs = -1;
    m_asyncInFlight = false;
    m_asyncRequestRef = FrameRef();
    m_asyncRequestSessionId.clear();
    m_asyncViewportSize = QSize();
    m_segmentInFlight = false;
    m_segmentTargetRef = FrameRef();
    /* a fresh session carries a fresh unique identity so a stale async
     * decode/SAM/VLM result from the previous frame is discarded */
    m_objectSessionId = QStringLiteral("sel-%1-%2")
                            .arg(QDateTime::currentMSecsSinceEpoch())
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)
                                        .left(8));
    /* invalidate any stale VLM result tied to the previous frame */
    m_visionHasResult = false;
    m_visionAnswer.clear();
    m_visionEvidence.clear();
    m_visionUncertainty.clear();
    m_visionConversationId.clear();
    m_segmentation.cancel();
    if (m_canvas)
    {
        m_canvas->clearObjectMask();
        m_canvas->clearLiveHarvestOverlay();
        m_canvas->setObjectSelectBusy(false);
    }
    emit objectSelectionChanged();
    emit visionStateChanged();
}

QImage PaintbrushController::objectCrop() const
{
    /* For a LIVE video selection the authoritative mask and frame live in the
     * ObjectSelectionContext (native frame space) — crop directly from them,
     * never rescaling the native mask. For a compose/stills selection fall
     * back to the document/canvas-space path. */
    if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
        && !m_selection.mask.isNull() && !m_selection.frame.isNull())
    {
        /* native mask bounds in frame coordinates */
        const QImage &m = m_selection.mask;
        int minX = m.width(), minY = m.height();
        int maxX = -1, maxY = -1;
        for (int y = 0; y < m.height(); ++y)
        {
            const uchar *line = m.constScanLine(y);
            for (int x = 0; x < m.width(); ++x)
                if (line[x] >= 128)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
        }
        if (maxX < 0)
            return {};
        const QRect bounds = QRect(minX, minY, maxX - minX + 1, maxY - minY + 1)
                                 .intersected(m.rect());
        if (bounds.isEmpty())
            return {};
        const QImage region = m_selection.frame.copy(bounds);
        const QImage mask = m.copy(bounds);
        return applyMaskAsAlpha(region, mask);
    }

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
    const bool live = m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo;
    /* Placement: for a compose/stills selection the sticker lands where the
     * object was in the frame; for a LIVE selection we center it on the
     * canvas (the native bounds are frame coords, not canvas coords). */
    QPointF place = m_canvas ? m_canvas->objectMaskBounds().topLeft() : QPointF();
    if (live || place.isNull())
        place = QPointF((m_document.canvasWidth() - cutout.width()) / 2.0,
                        (m_document.canvasHeight() - cutout.height()) / 2.0);
    QPoint trimOffset;
    const QImage trimmed = trimToContent(cutout, &trimOffset);
    if (trimmed.isNull())
    {
        setStatus(QStringLiteral("The selected object is empty."));
        return;
    }
    /* A20: the cutout becomes a Sticker asset — cutout pixels + FrameRef +
     * seed mask — inserted into the EXISTING document via InsertLayerCommand
     * (undoable, transformable, persisted). The provenance is the EXACT
     * selection context ref, never the document's mutable sourceFrame. */
    const QImage seedMask = trimmed.convertToFormat(QImage::Format_Alpha8);
    const FrameRef sref = selectionFrameRef();
    insertStickerLayer(trimmed, place + QPointF(trimOffset),
                       QStringLiteral("Cutout"), sref.mrl,
                       sref.title.isEmpty() ? m_document.sourceMediaTitle()
                                            : sref.title,
                       sref.ptsMs, seedMask);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Cutout added to the canvas as a sticker layer"));
    triggerCompanionEvent(QStringLiteral("cutout-added"));
}

void PaintbrushController::extractObject()
{
    /* A20 (collage main feature): pull the selected object out of the video
     * frame onto the CURRENT canvas. The selection becomes a Sticker asset
     * (cutout pixels + FrameRef + seed mask) and InsertLayerCommand inserts
     * it as a normal draggable/transformable layer into the EXISTING
     * document — the document is NEVER reset and the canvas is NEVER
     * re-created, so every other layer and the undo history survive. */
    const QImage cutout = objectCrop();
    if (cutout.isNull())
    {
        setStatus(QStringLiteral("Select an object first (Object Select tool)."));
        return;
    }
    const bool live = m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo;
    QPointF place = m_canvas ? m_canvas->objectMaskBounds().topLeft() : QPointF();
    if (live || place.isNull())
        place = QPointF((m_document.canvasWidth() - cutout.width()) / 2.0,
                        (m_document.canvasHeight() - cutout.height()) / 2.0);
    QPoint trimOffset;
    const QImage trimmed = trimToContent(cutout, &trimOffset);
    if (trimmed.isNull())
    {
        setStatus(QStringLiteral("The selected object is empty."));
        return;
    }
    const QImage seedMask = trimmed.convertToFormat(QImage::Format_Alpha8);
    const FrameRef sref = selectionFrameRef();
    insertStickerLayer(trimmed, place + QPointF(trimOffset),
                       QStringLiteral("Extracted object"),
                       sref.mrl,
                       sref.title.isEmpty() ? m_document.sourceMediaTitle()
                                            : sref.title,
                       sref.ptsMs, seedMask);
    if (m_canvas)
    {
        m_canvas->markCanvasDirty();
        m_canvas->fitToView();
    }
    setStatus(QStringLiteral("Object extracted as a sticker — drag it anywhere on the canvas"));
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
    /* Stale-result guard (P0#7): the VLM conversation id is the session key
     * for a given frame/selection. If a late response from a SUPERSEDED Ask
     * (different frame, or after a reset cleared the session) arrives, it no
     * longer belongs to this selection — discard it instead of overwriting
     * the current answer. */
    if (conversationId != m_visionConversationId)
        return;
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
    /* Ignore stale errors from a superseded conversation (same principle as
     * the response guard): a failed old-frame Ask must not surface on the
     * current selection. */
    if (conversationId != m_visionConversationId)
        return;
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

    /* The selection context is the ONE source of the seed frame, mask,
     * MRL and PTS for tracking (P0#6) — never the document's mutable
     * sourceFrame. For a live selection the mask and frame are native. */
    const FrameRef sref = selectionFrameRef();
    QImage seedFrame = m_selection.frame;
    QImage seedMask = m_selection.mask;
    if (seedMask.isNull())
        seedMask = m_objectMask;
    if (seedFrame.isNull())
        seedFrame = currentAiSource();
    if (seedFrame.isNull() || seedMask.isNull())
    {
        setStatus(QStringLiteral("Could not resolve the source frame for this selection."));
        return;
    }
    const qint64 seedPts = sref.isValid() ? sref.ptsMs : playerTimeMs();

    m_pendingTrack.clear();
    ObjectTrack track;
    track.id = m_trackIdCounter++;
    track.sourceMRL = sref.mrl.isEmpty() ? m_document.sourceMediaMRL()
                                         : sref.mrl;
    track.startMs = seedPts;
    track.complete = false;
    m_pendingTrack.append(track);

    /* the seed frame is decoded through the edit frame source (exact
     * PTS); tracking never steps or snapshots the playback player */
    m_tracking.startTrack(&m_editSource, track.sourceMRL,
                          seedFrame, seedMask,
                          positivePointsForTracking(), direction, frames,
                          seedPts);
    emit trackingStateChanged();
    pbLogInfo(VLC_OBJECT(m_intf), "tracking", "track started id=%d dir=%d seedPts=%lld",
              track.id, direction, (long long)seedPts);
}

QVector<QPointF> PaintbrushController::positivePointsForTracking() const
{
    /* the positive point lives in FRAME space for a live selection
     * (native mask coordinates); for a compose/stills selection it is the
     * center of the document-space mask. */
    if (m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
        && !m_selection.mask.isNull())
    {
        const QRect r = m_selection.mask.rect();
        return { QPointF(r.center().x(), r.center().y()) };
    }
    const QRectF bounds = m_canvas ? m_canvas->objectMaskBounds() : QRectF();
    if (bounds.isEmpty())
        return {};
    return { bounds.center() };
}

DecodedFrame PaintbrushController::requestFrame(const QString &mrl,
                                                qint64 ptsMs) const
{
    if (mrl.isEmpty())
        return {};
    return m_editSource.request(FrameRef{mrl, {}, ptsMs});
}

QImage PaintbrushController::decodeKeyframeFrame(const ObjectTrack &track,
                                                 const TrackKeyframe &kf) const
{
    /* the track remembers its own source MRL (survives relinks); fall back
     * to the document's media when it is missing */
    const QString mrl = track.sourceMRL.isEmpty()
                            ? m_document.sourceMediaMRL()
                            : track.sourceMRL;
    if (mrl.isEmpty() || kf.timeMs < 0)
        return {};

    const DecodedFrame f = m_editSource.request(FrameRef{mrl, {}, kf.timeMs});
    if (!f.ok())
    {
        pbLogWarn(VLC_OBJECT(m_intf), "export",
                  "keyframe %d at %lld ms not exported: decode failed or "
                  "PTS not verified (actual %lld ms)",
                  kf.frameIndex, (long long)kf.timeMs, (long long)f.ptsMs);
        return {};
    }
    return f.image;
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

    /* offline export (A17): every frame is decoded by the edit frame
     * source at the keyframe's exact PTS — no main-player seeks, no
     * sleep-polling, no unverified "first non-zero snapshot" */
    QDir().mkpath(dir);
    int written = 0;
    int skipped = 0;
    QSize lastSize;

    for (const TrackKeyframe &kf : track.keyframes)
    {
        const QImage frame = decodeKeyframeFrame(track, kf);
        if (frame.isNull())
        {
            ++skipped;
            continue;
        }
        const QImage sticker = renderStickerFrame(frame, kf.mask, 0,
                                                  QColor(), false, 1.0,
                                                  &track.effect);
        if (sticker.isNull())
        {
            ++skipped;
            continue;
        }
        lastSize = sticker.size();

        const QString path = dir + QStringLiteral("/sticker_%1.png")
                                     .arg(kf.frameIndex, 4, 10, QLatin1Char('0'));
        if (!sticker.save(path, "PNG"))
            return QStringLiteral("Failed to write %1").arg(path);
        ++written;
    }

    if (written == 0)
        return QStringLiteral("No frames could be exported (%1 keyframe frames unavailable).")
                   .arg(skipped);
    setStatus(QStringLiteral("Sticker sequence: %1 frames (%2x%3) in %4")
                  .arg(written).arg(lastSize.width()).arg(lastSize.height()).arg(dir));
    return dir;
}

/* --------------------------------------------------- animated sticker editor */

/* A frame whose PTS differs from the requested keyframe time by more than
 * this is NOT the frame the keyframe mask was computed for, so it is never
 * accepted as the sticker source (A17: "verify the first non-zero frame
 * against the requested keyframe time before accepting it"). 120 ms covers
 * one to a few frames of GOP-aligned seeks while still rejecting frames
 * that are visually a different moment. */
static constexpr qint64 kStickerFrameToleranceMs = 120;

static const ObjectTrack *findTrack(const QVector<ObjectTrack> &tracks, int trackId)
{
    for (const ObjectTrack &track : tracks)
        if (track.id == trackId)
            return &track;
    return nullptr;
}

/* Render a keyframe cutout with optional outline/shadow/scale and an
 * optional track effect: shared implementation lives in pb_sticker.cpp so
 * the controller, the harness and any future offline export pipeline use
 * exactly the same renderer. */

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
    /* real pixel preview (A16): the source frame is decoded by the edit
     * frame source at the keyframe's exact PTS, so the preview shows the
     * actual video pixels, not a neutral silhouette */
    const QImage frame = decodeKeyframeFrame(*track, kf);
    if (frame.isNull())
        return {};
    QImage rendered = renderStickerFrame(frame, kf.mask, outline,
                                         QColor(outlineColor), shadow, scale,
                                         &track->effect);
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

    /* offline export (A17): walk the track keyframes, decode each via the
     * edit frame source (exact PTS, verified), apply effect+outline+
     * shadow+scale, write sequentially. No main-player seeks, no
     * sleep-poll, no unverified snapshots. */
    QDir().mkpath(dir);
    int written = 0;
    int exported = 0;
    int skipped = 0;
    QSize lastSize;
    const QColor oc(outlineColor);

    /* A17 offline export: every frame is decoded by the edit frame source
     * at the keyframe's exact PTS — no main-player seeks, no sleep-polling,
     * no unverified "first non-zero snapshot" (Stream 3 final path). */
    for (int i = startFrame; i <= endFrame; ++i)
    {
        const TrackKeyframe &kf = track->keyframes[i];
        const QImage frame = decodeKeyframeFrame(*track, kf);
        if (frame.isNull())
        {
            ++skipped;
            continue;
        }

        QImage sticker = renderStickerFrame(frame, kf.mask, outline, oc,
                                            shadow, scale, &track->effect);
        if (sticker.isNull())
        {
            ++skipped;
            continue;
        }
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

    if (written == 0)
        return QStringLiteral("No frames could be exported (%1 keyframe frames unavailable).")
                   .arg(skipped);
    setStatus(QStringLiteral("Sticker sequence: %1 frames (%2x%3) in %4")
                  .arg(written).arg(lastSize.width()).arg(lastSize.height()).arg(dir));
    pbLogInfo(VLC_OBJECT(m_intf), "export",
              "sticker track=%d frames=%d size=%dx%d effect=%d dir=%s",
              trackId, written, lastSize.width(), lastSize.height(),
              track->effect.type, qUtf8Printable(dir));
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
    /* offline decode through the edit frame source: no player seeks, no
     * sleep-polling (A17) */
    const QImage frame = decodeKeyframeFrame(*track, kf);
    if (frame.isNull())
    {
        setStatus(QStringLiteral("Could not capture the frame for this keyframe; place it from a still instead."));
        return;
    }
    QImage sticker = renderStickerFrame(frame, kf.mask, outline,
                                        QColor(outlineColor), shadow, scale,
                                        &track->effect);
    if (sticker.isNull())
    {
        setStatus(QStringLiteral("The selected frame produced an empty cutout."));
        return;
    }
    const QPointF pos = QPointF((m_document.canvasSize().width() - sticker.width()) / 2.0,
                                (m_document.canvasSize().height() - sticker.height()) / 2.0);
    /* A20: a sticker layer — cutout pixels + FrameRef + seed mask — inserted
     * into the EXISTING document via InsertLayerCommand (undoable,
     * transformable, persisted). */
    insertStickerLayer(sticker, pos, QStringLiteral("Animated sticker frame"),
                       track->sourceMRL, {}, kf.timeMs, kf.mask);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    setStatus(QStringLiteral("Sticker frame placed on the canvas."));
    triggerCompanionEvent(QStringLiteral("sticker-made"));
}
void PaintbrushController::insertStickerLayer(const QImage &raster,
                                              const QPointF &position,
                                              const QString &name,
                                              const QString &sourceMRL,
                                              const QString &sourceTitle,
                                              qint64 sourceTimeMs,
                                              const QImage &seedMask)
{
    if (raster.isNull())
        return;
    CanvasLayer layer = CanvasLayer::createImage(raster, position, name);
    layer.type = LAYER_STICKER;
    if (!sourceMRL.isEmpty())
    {
        layer.metadata[QStringLiteral("sourceMRL")] = sourceMRL;
        layer.metadata[QStringLiteral("sourceTitle")] = sourceTitle;
        layer.metadata[QStringLiteral("sourceTimeMs")] = qint64(sourceTimeMs);
    }
    if (!seedMask.isNull())
    {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        seedMask.convertToFormat(QImage::Format_Grayscale8).save(&buffer, "PNG");
        layer.metadata[QStringLiteral("seedMask")] =
            QString(QStringLiteral("data:image/png;base64,") +
                    QString::fromLatin1(png.toBase64()));
    }

    /* A20: one typed InsertLayerCommand carries the immutable sticker asset
     * into the EXISTING document — undoable (undo removes the sticker),
     * transformable (SetTransformCommand), persisted (project store) and
     * rendered by the canonical DocumentRenderer like any other layer. */
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Add %1").arg(name);
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = m_document.layerAt(cmd.index) ? *m_document.layerAt(cmd.index)
                                              : CanvasLayer();
    m_undoStack.pushInsertLayer(cmd);
}

QImage PaintbrushController::stickerFrameForKeyframe(int trackId, int frameIndex,
                                                     bool *playerTouched) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    if (!track || frameIndex < 0 || frameIndex >= track->keyframes.size())
        return {};
    const TrackKeyframe &kf = track->keyframes[frameIndex];

    /* A16/A17 final path: the offline edit frame source decodes the exact
     * keyframe PTS — no player seeks at all. */
    if (!track->sourceMRL.isEmpty() || !m_document.sourceMediaMRL().isEmpty())
    {
        const QImage offline = decodeKeyframeFrame(*track, kf);
        if (!offline.isNull())
        {
            cacheStickerFrame(trackId, frameIndex, kf.timeMs, offline);
            return offline;
        }
    }

    StickerFrameCache::Entry cached;
    if (m_stickerFrames.get(trackId, frameIndex, &cached))
        return cached.frame;

    /* Last-resort fallback (legacy bridge): ONE seek + ONE capture through
     * FrameCaptureService, accepted only when the captured frame's PTS
     * matches the requested keyframe time. No UI-thread sleep-polling. */
    vlc_player_t *player = m_intf->p_mainPlayerController->getPlayer();
    if (!player || !playerHasVideo())
        return {};
    if (playerTouched)
        *playerTouched = true; /* the seek below moves the playhead */
    {
        PlayerLock lock(player);
        vlc_player_SetTime(player, kf.timeMs * 1000);
    }
    const QImage frame = FrameCaptureService::captureCurrentFrame(player);
    const qint64 actualMs = FrameCaptureService::currentTimeMs(player);
    if (frame.isNull() || qAbs(actualMs - kf.timeMs) > kStickerFrameToleranceMs)
        return {};
    cacheStickerFrame(trackId, frameIndex, kf.timeMs, frame);
    return frame;
}

void PaintbrushController::cacheStickerFrame(int trackId, int frameIndex,
                                             qint64 timeMs,
                                             const QImage &frame) const
{
    if (frame.isNull())
        return;
    m_stickerFrames.put(trackId, frameIndex, timeMs, frame);
}

void PaintbrushController::setSelectedTrackId(int trackId)
{
    int id = -1;
    const QVector<ObjectTrack> tracks = m_document.tracks();
    for (const ObjectTrack &track : tracks)
        if (track.id == trackId)
        {
            id = trackId;
            break;
        }
    if (id == m_selectedTrackId)
        return;
    m_selectedTrackId = id;
    emit selectedTrackChanged();
}

void PaintbrushController::sanitizeSelectedTrack()
{
    int fallback = -1;
    const QVector<ObjectTrack> tracks = m_document.tracks();
    for (const ObjectTrack &track : tracks)
    {
        if (track.id == m_selectedTrackId)
            return; /* current selection is still valid */
        if (fallback < 0)
            fallback = track.id;
    }
    if (m_selectedTrackId != fallback)
    {
        m_selectedTrackId = fallback;
        emit selectedTrackChanged();
    }
}


/* ------------------------------------------------------- track effects ---- */

int PaintbrushController::trackEffectType(int trackId) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    return track ? track->effect.type : 0;
}

int PaintbrushController::trackEffectIntensity(int trackId) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    return track ? track->effect.intensity : 50;
}

QString PaintbrushController::trackEffectColor(int trackId) const
{
    const ObjectTrack *track = findTrack(m_document.tracks(), trackId);
    return track ? track->effect.color : QString();
}

void PaintbrushController::setTrackEffect(int trackId, int type, int intensity,
                                          const QString &color)
{
    QVector<ObjectTrack> tracks = m_document.tracks();
    int idx = -1;
    for (int i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == trackId)
        {
            idx = i;
            break;
        }
    if (idx < 0)
        return;
    tracks[idx].effect.type = qBound(0, type, int(TrackEffect::Attach));
    tracks[idx].effect.intensity = qBound(0, intensity, 100);
    tracks[idx].effect.color = color;
    m_document.setTracks(tracks); /* emits tracksChanged + dirtyChanged */
    setStatus(QStringLiteral("Track effect updated."));
}

QString PaintbrushController::trackEffectPreviewDataUrl(int trackId, int frameIndex,
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
    /* real pixel preview (A16): decode the exact source frame offline */
    const QImage frame = decodeKeyframeFrame(*track, kf);
    if (frame.isNull())
        return {};
    QImage rendered = renderStickerFrame(frame, kf.mask, outline,
                                         QColor(outlineColor), shadow, scale,
                                         &track->effect);
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

    /* A16/A17: warm the sticker frame LRU with the REAL source pixels via
     * the offline edit frame source — never the playback player. */
    if (!m_pendingTrack[0].sourceMRL.isEmpty() ||
        !m_document.sourceMediaMRL().isEmpty())
    {
        const ObjectTrack &t = m_pendingTrack[0];
        const TrackKeyframe kf2 = kf;
        const QImage frame = decodeKeyframeFrame(t, kf2);
        if (!frame.isNull())
            cacheStickerFrame(m_pendingTrack[0].id, frameIndex, timeMs, frame);
    }

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
     * exports read from the remaining masks. Eviction operates on the REAL
     * track store the document owns (tracksRef) — never on a controller
     * copy (A32) — and drops the matching sticker-frame cache entries so
     * the frame cache cannot outlive the mask it renders. */
    const qint64 budget = qint64(512) * 1024 * 1024;
    const qint64 before = trackedMaskBytes();
    if (before <= budget)
        return before;

    /* gather candidate frames oldest-first across all tracks + pending */
    struct Candidate
    {
        int trackId;
        int frameIndex;
        TrackKeyframe *kf;
    };
    QList<Candidate> candidates;
    QVector<ObjectTrack> &docTracks = m_document.tracksRef();
    for (ObjectTrack &track : docTracks)
        for (TrackKeyframe &kf : track.keyframes)
            if (!kf.corrected && !kf.mask.isNull())
                candidates.append({track.id, kf.frameIndex, &kf});
    for (ObjectTrack &track : m_pendingTrack)
        for (TrackKeyframe &kf : track.keyframes)
            if (!kf.corrected && !kf.mask.isNull())
                candidates.append({track.id, kf.frameIndex, &kf});
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.kf->frameIndex < b.kf->frameIndex;
              });

    qint64 cur = before;
    for (const Candidate &c : candidates)
    {
        if (cur <= budget)
            break;
        if (!c.kf->mask.isNull())
        {
            cur -= qint64(c.kf->mask.sizeInBytes());
            c.kf->mask = QImage();   /* evict content, keep metadata */
            m_stickerFrames.remove(c.trackId, c.frameIndex);
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

        const int finishedTrackId = m_pendingTrack[0].id;
        QVector<ObjectTrack> tracks = m_document.tracks();
        tracks.append(m_pendingTrack[0]);
        m_document.setTracks(tracks);
        m_pendingTrack.clear();
        /* A15: a fresh track becomes the sticker-editor selection when
         * nothing else is selected yet; sanitizeSelectedTrack (connected to
         * tracksChanged) keeps the selection valid from here on. */
        if (m_selectedTrackId < 0)
            setSelectedTrackId(finishedTrackId);
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

    /* A20: the sticker is a LAYER_STICKER asset (cutout + FrameRef + seed
     * mask) inserted into the EXISTING document via InsertLayerCommand.
     * The provenance is the EXACT selection context ref (P0#6). */
    const bool live = m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo;
    QPointF pos = m_canvas ? m_canvas->objectMaskBounds().topLeft() : QPointF();
    if (live || pos.isNull())
        pos = QPointF((m_document.canvasWidth() - sticker.width()) / 2.0,
                      (m_document.canvasHeight() - sticker.height()) / 2.0);
    const QImage seedMask = trimmed.convertToFormat(QImage::Format_Alpha8);
    const FrameRef sref = selectionFrameRef();
    insertStickerLayer(sticker, pos, QStringLiteral("Sticker"),
                       sref.mrl,
                       sref.title.isEmpty() ? m_document.sourceMediaTitle()
                                            : sref.title,
                       sref.ptsMs, seedMask);
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
            l.type == LAYER_STICKER || l.type == LAYER_GENERATED)
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

    /* apply as ONE typed undoable step: every piece becomes a
     * SetTransformCommand (position/scale/rotation stored as the layer's
     * canonical transform — content stays pristine, no baking/resampling).
     * The presets sized `scale` against the piece's CURRENT canvas-space
     * footprint (bounds), so the new transform REPLACES the previous one:
     * A(q) = newPos + s*R(rot)*(q - qTL) with qTL the piece content top-left
     * in local raster space — the footprint lands exactly in its slot. */
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Auto collage");
    QList<CanvasLayer> layers = m_document.layers();
    for (int i = 0; i < pieces.size(); ++i)
    {
        const int idx = pieces[i].layerIndex;
        const QRectF b = pieces[i].bounds;
        CanvasLayer &l = layers[idx];
        const QPointF newPos = newPositions[i];
        const qreal scale = newScales[i];
        const qreal rot = newRotations[i];

        if (l.raster.isNull())
            continue;

        const QTransform before = l.transform();
        const QPointF qTL = before.inverted().map(b.topLeft());
        const QPointF rTL = QTransform().rotate(rot).map(qTL);

        QTransform t;
        t.translate(newPos.x() - scale * rTL.x(),
                    newPos.y() - scale * rTL.y());
        t.rotate(rot);
        t.scale(scale, scale);

        l.setTransform(t);
        const QTransform after = l.transform();
        composite->children.push_back(std::make_unique<SetTransformCommand>(
            SetTransformCommand{l.id, idx, before, after, composite->desc}));
    }
    m_document.setLayers(layers);
    if (!composite->children.empty())
        m_undoStack.pushCommand(std::move(composite));
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

    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Add %1").arg(name);
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = m_document.layerAt(cmd.index) ? *m_document.layerAt(cmd.index)
                                              : CanvasLayer();
    m_undoStack.pushInsertLayer(cmd);
}

void PaintbrushController::addResultLayer(const QImage &image, const QString &name)
{
    addImageLayer(image, QPointF(0, 0), name, LAYER_GENERATED);
}

QImage PaintbrushController::flattenedPreview() const
{
    /* the AI preview uses the same canonical renderer as the screen and
     * export, so the model sees exactly what the user sees */
    QImage flat = DocumentRenderer::render(m_document);
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

bool PaintbrushController::playerIsPlaying() const
{
    /* Real player play/pause state (P1#10): from the player controller, NOT
     * `playerTimeMs > 0`. The QML play/pause icon and the freeze logic both
     * derive from this. */
    return m_intf->p_mainPlayerController->getPlayingState()
           == PlayerController::PLAYING_STATE_PLAYING;
}

/* ------------------------------------------------------------ presentation */

void PaintbrushController::setPresentationMode(int mode)
{
    auto newMode = (mode == static_cast<int>(PresentationMode::HarvestVideo))
                       ? PresentationMode::HarvestVideo
                       : PresentationMode::ComposeCanvas;
    if (m_presentationMode == newMode)
        return;
    m_presentationMode = newMode;
    emit presentationModeChanged();
    if (m_canvas)
    {
        /* In HarvestVideo mode the canvas stops rendering its opaque document
         * tiles so the live video shows through (P0#1). */
        m_canvas->setVideoHarvestMode(
            m_presentationMode == PresentationMode::HarvestVideo);
        m_canvas->markCanvasDirty();
    }
}

void PaintbrushController::enterHarvestPresentation()
{
    setPresentationMode(static_cast<int>(PresentationMode::HarvestVideo));
}

void PaintbrushController::enterComposePresentation()
{
    setPresentationMode(static_cast<int>(PresentationMode::ComposeCanvas));
}

void PaintbrushController::playerFrameNext()
{
    /* stepping away from the frozen selection frame ends the live session */
    if (m_selectionFrozen)
        resetLiveSelectionSession();
    m_thumbnails.noteUserSeek();
    m_intf->p_mainPlayerController->frameNext();
    emit mediaStateChanged();
}

void PaintbrushController::playerFramePrev()
{
    if (m_selectionFrozen)
        resetLiveSelectionSession();
    m_thumbnails.noteUserSeek();
    m_intf->p_mainPlayerController->framePrev();
    emit mediaStateChanged();
}

void PaintbrushController::playerPlayPause()
{
    /* P1#10: real play/pause from the player state. */
    const bool wasPlaying = playerIsPlaying();
    /* Resuming playback while an Object Select selection is frozen ends that
     * live session (the selected frame is no longer the one frozen; the
     * result mask would float on a moving image). Kept harvest items are
     * untouched. */
    if (m_selectionFrozen && !wasPlaying)
        resetLiveSelectionSession();
    m_intf->p_mainPlaylistController->togglePlayPause();
    emit mediaStateChanged();
}

void PaintbrushController::playerSeek(qint64 timeMs)
{
    /* A frozen Object Select selection belongs to one exact FrameRef; seeking
     * to a different PTS ends that live session (P0#5/P0#7). The same-frame
     * tolerance matches the exact-frame decoder's PTS tolerance
     * (paintbrush-edit-pts-tolerance-ms) so the seek-invalidates-selection
     * threshold never disagrees with what the decoder considers "the same
     * frame". */
    const qint64 sameFrameTol = qBound<qint64>(
        qint64(20), qint64(pbConfigInt(VLC_OBJECT(m_intf),
                                       "paintbrush-edit-pts-tolerance-ms")),
        qint64(2000));
    if (m_selectionFrozen
        && (m_frozenPtsMs < 0 || qAbs(timeMs - m_frozenPtsMs) > sameFrameTol))
    {
        resetLiveSelectionSession();
        if (m_objectSelectBusy)
            setStatus(QStringLiteral("Selection ended by seeking."));
    }

    /* seek instrumentation kept as Paintbrush debug logging (no noisy
     * release probing): (C) C++ request, (D) immediate post-setTime, and an
     * asynchronous (E) probe from the ACTUAL player time/position
     * notification delivered by onMediaTimer after the seek — because an
     * immediate playerTimeMs() right after setTime() is not proof the seek
     * succeeded. */
    pbLogDebug(VLC_OBJECT(m_intf), "media",
               "pb seek C: requested %lld ms (current=%lld)",
               (long long)timeMs, (long long)playerTimeMs());
    if (m_seekProbeArmed
        && pbConfigInt(VLC_OBJECT(m_intf), "verbose") >= 2)
    {
        /* (D) — the immediate post-setTime value; only useful combined with
         * the later player notification (E). */
        pbLogDebug(VLC_OBJECT(m_intf), "media",
                   "pb seek D: after setTime=%lld ms (target=%lld)",
                   (long long)playerTimeMs(), (long long)timeMs);
    }

    /* the user took over the playhead: the thumbnail batch must not restore
     * its original position when it finishes. (During a slider drag the
     * batch is also suspended via playerSeekBegin/playerSeekEnd so it cannot
     * seek the player underneath the drag.) */
    m_thumbnails.noteUserSeek();

    /* Arm the asynchronous E probe: onMediaTimer will report the position
     * once the player actually lands near the target (P1#8). */
    m_seekProbeTargetMs = timeMs;
    m_seekProbeArmed = true;

    m_intf->p_mainPlayerController->setTime(VLCTime(timeMs * 1000));
    emit mediaStateChanged();
}

void PaintbrushController::playerSeekBegin()
{
    /* (A) press probe — gated behind Paintbrush debug logging (P1#8) */
    pbLogDebug(VLC_OBJECT(m_intf), "media",
               "pb seek A: slider pressed (current=%lld ms)",
               (long long)playerTimeMs());
    m_thumbnails.suspend();
}

void PaintbrushController::playerSeekEnd()
{
    /* user released the seek slider. The suspended thumbnail batch is NOT
     * resumed here: resuming would decode immediately and race the final
     * precise seek issued on release. onMediaTimer() resumes the batch
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
    /* thumbnails decode through the edit frame source (FrameProvider):
     * the main playback player is never seeked or snapshotted for batch
     * work (A8/A11) */
    const QString mediaKey = currentMediaMRL();
    if (mediaKey.isEmpty())
    {
        setStatus(QStringLiteral("No media is loaded for thumbnails."));
        return;
    }
    const qint64 duration = playerLengthMs();
    if (duration <= 0)
        return;
    const qint64 anchor = playerTimeMs();
    m_thumbnails.startBatch(&m_editSource, mediaKey, duration, anchor, 24);
    emit thumbnailsChanged();
}

void PaintbrushController::cancelThumbnails()
{
    m_thumbnails.cancel();
    emit thumbnailsChanged();
}

/* ----------------------------------------------------------- diagnostics */

/* ---- NAI Settings (P1#12) ------------------------------------------- */

QString PaintbrushController::settingsSamModel() const
{
    return pbConfigString(VLC_OBJECT(m_intf), "paintbrush-sam-model");
}
QString PaintbrushController::settingsSamDevice() const
{
    return pbConfigString(VLC_OBJECT(m_intf), "paintbrush-sam-device");
}
QString PaintbrushController::settingsVlmUrl() const
{
    return pbConfigString(VLC_OBJECT(m_intf), "paintbrush-vlm-base-url");
}
QString PaintbrushController::settingsVlmModel() const
{
    return pbConfigString(VLC_OBJECT(m_intf), "paintbrush-vlm-model");
}
QString PaintbrushController::settingsAssistantUrl() const
{
    return pbConfigString(VLC_OBJECT(m_intf), "paintbrush-assistant-base-url");
}
QString PaintbrushController::settingsAssistantModel() const
{
    return pbConfigString(VLC_OBJECT(m_intf), "paintbrush-assistant-model");
}
int PaintbrushController::settingsPtsToleranceMs() const
{
    return pbConfigInt(VLC_OBJECT(m_intf), "paintbrush-edit-pts-tolerance-ms");
}

QString PaintbrushController::trackingStatusText() const
{
    if (trackingAvailable())
        return QStringLiteral("Ready");
    const QString r = trackingUnavailableReason();
    return r.isEmpty() ? QStringLiteral("Unavailable") : r;
}

QString PaintbrushController::applyNaiSettings(const QString &samModel,
                                               const QString &samDevice,
                                               const QString &vlmUrl,
                                               const QString &vlmModel,
                                               const QString &assistantUrl,
                                               const QString &assistantModel,
                                               int ptsToleranceMs)
{
    /* Persist into the Paintbrush/VLC configuration layer (source of truth —
     * never a second settings store). Secret values are never read or shown;
     * changing them is out of scope here (the user sets them via env/VLC
     * preferences). */
    vlc_object_t *obj = VLC_OBJECT(m_intf);
    auto setStr = [obj](const char *name, const QString &value) {
        var_SetString(obj, name, value.toUtf8().constData());
    };
    auto setInt = [obj](const char *name, int value) {
        var_SetInteger(obj, name, value);
    };

    bool changedModel = samModel != settingsSamModel()
        || samDevice != settingsSamDevice()
        || vlmUrl != settingsVlmUrl()
        || vlmModel != settingsVlmModel()
        || assistantUrl != settingsAssistantUrl()
        || assistantModel != settingsAssistantModel();

    setStr("paintbrush-sam-model", samModel.trimmed());
    setStr("paintbrush-sam-device", samDevice.trimmed());
    setStr("paintbrush-vlm-base-url", vlmUrl.trimmed());
    setStr("paintbrush-vlm-model", vlmModel.trimmed());
    setStr("paintbrush-assistant-base-url", assistantUrl.trimmed());
    setStr("paintbrush-assistant-model", assistantModel.trimmed());
    setInt("paintbrush-edit-pts-tolerance-ms",
           qBound(20, ptsToleranceMs, 2000));

    /* The exact-frame PTS tolerance is safe to hot-apply to the edit decoder
     * (it is only read at decode time); mark it applied immediately. */
    m_thumbnails.setPtsToleranceMs(qMax<qint64>(
        20, int(pbConfigInt(obj, "paintbrush-edit-pts-tolerance-ms"))));

    /* Model/endpoint changes require a service re-configure to take effect,
     * which cannot be done safely mid-session; those need a restart. */
    m_settingsRestartRequired = changedModel;
    emit settingsChanged();

    if (changedModel)
        return QStringLiteral("Model and endpoint changes take effect after restart.");
    return QStringLiteral("Applied.");
}

void PaintbrushController::clearRestartRequired()
{
    if (m_settingsRestartRequired)
    {
        m_settingsRestartRequired = false;
        emit settingsChanged();
    }
}

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
    m_editSource.clearCache();
    setStatus(QStringLiteral("AI caches cleared"));
    emit serviceAvailabilityChanged();
}

void PaintbrushController::unloadAiModels()
{
    m_segmentation.shutdown();
    m_tracking.shutdown();
    m_thumbnails.cancel();
    m_editSource.shutdown();
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
    /* stopping playback ends any live selection and leaves Harvest mode */
    resetLiveSelectionSession();
    enterComposePresentation();
    /* stop batch work that decodes against the outgoing media */
    m_thumbnails.cancel();
    m_intf->p_mainPlaylistController->stop();
    emit mediaStateChanged();
}

void PaintbrushController::closeVideo()
{
    /* Reset the live selection session: the outgoing media's exact frame,
     * mask, busy state and any stale async decode/SAM/VLM tied to it are
     * cleared so the next Open Video starts clean (P0#7). */
    resetLiveSelectionSession();

    /* leave Harvest presentation: with no video source the canvas falls back
     * to the normal document composition */
    enterComposePresentation();

    /* cancel in-flight decode/thumbnail work against the outgoing media */
    m_thumbnails.cancel();
    m_editSource.clearCache();

    /* stop playback: with no video output attached, playerHasVideo() becomes
     * false and the media drawer + canvas video surface hide themselves. */
    m_intf->p_mainPlaylistController->stop();

    /* clear the current source-frame identity + source-media state so the
     * next Open Video starts fresh (does NOT touch the canvas or the Harvest
     * Tray — kept items keep their own MRL/PTS provenance). */
    m_document.clearSourceMedia();
    m_document.setSourceFrame(FrameRef{});
    m_thumbnailsAutoMedia.clear();
    m_thumbnailsAutoMedia.squeeze();

    emit mediaStateChanged();
    if (m_welcomeVisible)
        return;
    setStatus(QStringLiteral("Closed video."));
}

void PaintbrushController::onMediaTimer()
{
    emit mediaStateChanged();

    /* Asynchronous seek E-probe (P1#8): the ACTUAL player time/position
     * notification after a seek — delivered by the periodic position poll
     * rather than an immediate read-back which would not prove the seek
     * succeeded. If the player has moved at all toward/away from the target,
     * or any time has advanced, report it and disarm. */
    if (m_seekProbeArmed)
    {
        const qint64 now = playerTimeMs();
        const bool closeToTarget =
            m_seekProbeTargetMs >= 0
            && qAbs(now - m_seekProbeTargetMs) <= 600; /* within ~ half-sec */
        const bool moved = now != m_seekProbeTargetMs;
        if (closeToTarget || (moved && !playerIsPlaying()))
        {
            pbLogDebug(VLC_OBJECT(m_intf), "media",
                       "pb seek E: player notified at %lld ms (target %lld)",
                       (long long)now, (long long)m_seekProbeTargetMs);
            m_seekProbeArmed = false;
            m_seekProbeTargetMs = -1;
        }
    }

    /* A frozen Object Select session must end if playback resumed (the
     * selected frame is no longer frozen under the mask). The explicit
     * play/pause handlers cover the normal path; this catches e.g. a
     * keyboard media key or an external resume while the mask is shown. */
    if (m_selectionFrozen && playerIsPlaying())
        resetLiveSelectionSession();

    /* A batch suspended by a paused scrub resumes as soon as playback
     * starts: generation then no longer disturbs the exact frame a paused
     * user was inspecting. */
    if (m_thumbnails.isSuspended()
        && m_intf->p_mainPlayerController->getPlayingState()
               == PlayerController::PLAYING_STATE_PLAYING)
        m_thumbnails.resume();

    /* Auto-fill the thumbnail timeline: as soon as a video with a known
     * length is loaded, start generating thumbnails once. Generation
     * decodes through the edit frame source at exact PTS and never
     * touches the playback player, so it cannot disturb what the user is
     * watching. */
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

