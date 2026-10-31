/*****************************************************************************
 * pb_canvas_item.cpp : NAI Paintbrush — tile-based raster canvas
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_canvas_item.hpp"
#include "pb_document.hpp"

#include <QSGTransformNode>
#include <QSGSimpleTextureNode>
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGTexture>
#include <QQuickWindow>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QDateTime>
#include <QQueue>
#include <QRandomGenerator>
#include <QtMath>
#include <QLineF>
#include <QMatrix4x4>
#include <QTransform>
#include <QUrl>
#include <QClipboard>
#include <QGuiApplication>
#include <QCursor>
#include <QPixmap>
#include <cstdio>

#include <QFile>
#include <QMutex>

/* Crash-path instrumentation: writes ordered markers to stdout and to a
 * persistent trace file under the project so we can see exactly how far the
 * post-stroke commit got before a crash, even when the GUI dies. */
namespace {
QMutex g_traceMutex;
void pbCrashTrace(const char *msg)
{
    QMutexLocker lock(&g_traceMutex);
    const QString line = QStringLiteral("[pbtrace] %1").arg(QLatin1String(msg));
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
    QFile f(QStringLiteral("D:/_SATIN_AI/NAI-OS/vlc/pb_stroke_trace.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(line.toUtf8());
        f.write("\n");
        f.close();
    }
}
} // namespace

namespace paintbrush {

PaintbrushCanvasItem::PaintbrushCanvasItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsDrops, true);
    setFlag(ItemIsFocusScope, true);
    /* Without ItemHasContents the scene graph never calls updatePaintNode()
     * and the canvas tiles are never painted — drawing appears to do
     * nothing even though strokes are committed to the document. */
    setFlag(ItemHasContents, true);

    /* The spray can keeps emitting while the button is held, even when the
     * mouse is perfectly still — like a real can under pressure. */
    m_sprayTimer.setTimerType(Qt::PreciseTimer);
    m_sprayTimer.setInterval(12); /* ~83 Hz emission */
    connect(&m_sprayTimer, &QTimer::timeout,
            this, &PaintbrushCanvasItem::emitSprayPuff);

    /* Coalesced repaint pipeline ("streaming"): dabs mark tiles dirty at
     * input rate (up to ~1 kHz under a fast stroke) but the composite+upload
     * runs at a fixed ~120 Hz cadence, so synchronous per-dab compositing
     * never piles up on the UI thread. Input events flow freely; the render
     * side batches one pass per tick and the scene graph paces the frames.
     *
     * The timer is SINGLE-SHOT: the 8 ms deadline must never be restarted by
     * every high-rate WM_MOUSEMOVE (that would keep sliding the deadline under
     * fast strokes and turn "fixed cadence" into "ink stutters behind the
     * cursor"). Input paths only START the timer when it is not already armed;
     * the timeout handler re-arms itself for the next tick whenever pending
     * input or dirty tiles remain after this pass. */
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(8);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this]() {
        const bool powderStroke = m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR;
        /* consume the latest sampled input position first, then flush */
        if (m_movePending)
        {
            m_movePending = false;
            if (m_strokeActive || m_dragging)
                continueStroke(m_pendingMove);
        }
        /* Long-press breath for the powder/smear tools: while a stroke is held
         * still, track the idle time on the fixed tick and start blowing once it
         * passes ~0.6 s; keep stepping the breath until the loose powder is gone
         * or the breath runs out. */
        if (powderStroke && m_strokeActive && !m_powderBlowing)
        {
            if (m_powderBlowSince < 0.6f)
                m_powderBlowSince += 1.0 / 120.0;
            if (m_powderBlowSince >= 0.6f)
                startPowderBlow();
        }
        else if (m_powderBlowing)
        {
            stepPowderBlow(1.0 / 120.0);
        }
        compositeDirtyTiles();
        /* single-shot: re-arm for the next tick if input, dabs arrived, or a
         * powder hold/breath is still in progress (the cadence must keep
         * flowing through a hold so the long-press is actually recognised). */
        if (!m_dirtyTiles.isEmpty() || m_movePending || m_powderBlowing
            || (powderStroke && m_strokeActive))
            m_repaintTimer.start();
        update();
    });

    updateCursor();
}

PaintbrushCanvasItem::~PaintbrushCanvasItem()
{
    /* Textures are owned by the scene graph; they are deleted when the
     * nodes are destroyed. Release our references. */
    m_textures.clear();
    m_tileNodes.clear();
}

void PaintbrushCanvasItem::setDocument(PaintbrushDocument *doc)
{
    if (m_doc == doc)
        return;
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    if (m_doc)
    {
        connect(m_doc, &PaintbrushDocument::layersChanged, this,
                [this]() { markAllTilesDirty(); });
        connect(m_doc, &PaintbrushDocument::canvasSizeChanged, this,
                [this]() { markAllTilesDirty(); });
        connect(m_doc, &PaintbrushDocument::selectionChanged, this,
                [this]() {
                    markAllTilesDirty();
                    emit selectionChanged(hasSelection());
                });
        /* switching layers is a per-layer material switch: invalidate the
         * in-memory powder/grain state so the next edit re-derives from that
         * layer's own materialPreset/materialState (never a stale sim). */
        connect(m_doc, &PaintbrushDocument::currentLayerChanged, this,
                [this](int) {
                    m_powderReady = false;
                    m_powderUnion = QRect();
                    m_powderBlowing = false;
                    m_glitterUnion = QRect();
                    m_strokeHadPaint = false;
                });
    }
    markAllTilesDirty();
    update();
}

void PaintbrushCanvasItem::setUndoStack(UndoStack *stack)
{
    m_undoStack = stack;
}

/* ---------------------------------------------------------------- tools */

void PaintbrushCanvasItem::setTool(int tool)
{
    if (m_tool == tool)
        return;
    m_tool = tool;
    /* leaving the spray tool stops the emission timer and clears the
     * physics state so the next stroke starts clean */
    if (tool != TOOL_SPRAY)
        resetSprayState();
    /* leaving object select clears the pending AI mask so stale overlays
     * never confuse the user */
    if (tool != TOOL_OBJECT_SELECT)
    {
        m_maskPaintActive = false;
        m_maskPaintUnion = QRect();
        clearObjectMask();
    }
    /* Glitter-family tools (glitter, powder, smear) default to a 12px brush —
     * the old shared 6px default produced tiny, faint deposits. Only RAISE on
     * tool entry so picking the tool always starts at a usable size; the user
     * can still lower it afterwards via the palette Size slider. */
    if ((tool == TOOL_GLITTER || tool == TOOL_GLITTER_POWDER
         || tool == TOOL_SMEAR)
        && m_brushSize < 12.0)
    {
        setBrushSize(12.0);
    }
    /* first-ever entry into the workbench arms the default mode (Glitter); later
     * re-entries PRESERVE the last selected subtool instead of resetting it
     * back to Glitter every time (P0.4). The legacy TOOL_SMEAR is a direct Push
     * shortcut (always forced to Push while it is the active rail tool). */
    if (tool == TOOL_GLITTER_POWDER ||
        tool == TOOL_SMEAR)
    {
        if (tool == TOOL_SMEAR)
            setPowderTool(int(POWDER_PUSH));
        else if (m_powderToolNeedsDefault)
        {
            m_powderToolNeedsDefault = false;
            setPowderTool(int(POWDER_GLITTER));
        }
    }
    emit toolChanged(tool);
    updateStatus(m_lastCanvasPos);
    updateCursor();
}

void PaintbrushCanvasItem::setPowderTool(int t)
{
    /* the workbench keeps only the four mutual-exclusive stroke modes as its
     * sticky state; Pour/Blow are momentary and never persist here */
    int clamped = t;
    switch (t)
    {
    case POWDER_GLUE:
    case POWDER_GLITTER:
    case POWDER_PUSH:
    case POWDER_ERASER:
        break;   /* valid sticky modes */
    default:
        clamped = int(POWDER_GLITTER);
        break;
    }
    if (int(m_powderTool) == clamped)
        return;
    m_powderTool = PowderToolId(clamped);
    emit powderToolChanged(int(m_powderTool));
    updateStatus(m_lastCanvasPos);
    updateCursor();   /* the ring reflects the newly selected sub-tool's radius */
}

void PaintbrushCanvasItem::setPowderGlueWidth(qreal v)
{
    if (qFuzzyCompare(v, m_powderGlueWidth)) return;
    m_powderGlueWidth = qBound<qreal>(4.0, v, 60.0);
    emit powderSizeChanged();
}
void PaintbrushCanvasItem::setPowderGlitterSize(qreal v)
{
    if (qFuzzyCompare(v, m_powderGlitterSize)) return;
    m_powderGlitterSize = qBound<qreal>(6.0, v, 80.0);
    emit powderSizeChanged();
}
void PaintbrushCanvasItem::setPowderPushSize(qreal v)
{
    if (qFuzzyCompare(v, m_powderPushSize)) return;
    m_powderPushSize = qBound<qreal>(8.0, v, 60.0);
    emit powderSizeChanged();
}
void PaintbrushCanvasItem::setPowderEraserWidth(qreal v)
{
    if (qFuzzyCompare(v, m_powderEraserWidth)) return;
    m_powderEraserWidth = qBound<qreal>(8.0, v, 120.0);
    emit powderEraserWidthChanged();
}

/* Physics radius for the active powder sub-tool. Glue/Eraser store a VISIBLE
 * WIDTH (radius = width/2); Glitter/Push store a REFERENCE RADIUS directly
 * (P0.5 semantics — never divide all four by two). */
float PaintbrushCanvasItem::powderSubRadius() const
{
    switch (m_powderTool)
    {
    case POWDER_GLUE:    return float(qMax<qreal>(2.0, m_powderGlueWidth)   * 0.5);
    case POWDER_PUSH:    return float(qMax<qreal>(4.0, m_powderPushSize));
    case POWDER_ERASER:  return float(qMax<qreal>(4.0, m_powderEraserWidth) * 0.5);
    case POWDER_GLITTER:
    default:             return float(qMax<qreal>(3.0, m_powderGlitterSize));
    }
}

/* Momentary Pour: deposit a heap of the CURRENT colour OVER existing glue/
 * material (what is already present stays); one undo action (P0.9/P0.10). The
 * pour target is derived from the glue/material bounds when glue exists, NOT
 * from stale last-pointer coordinates. Manual local sprinkling stays the
 * Glitter sub-tool. */
void PaintbrushCanvasItem::pourPowderNow()
{
    if (m_tool != TOOL_GLITTER_POWDER && m_tool != TOOL_SMEAR)
        return;
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
    if (!layer)
        return;
    layer = ensurePowderLayer();   /* create the dedicated powder layer on first edit */
    if (!layer)
        return;
    ensurePowder();

    /* begin the material capture (atomic undo: material before/after) */
    if (!m_powderMaterialCaptured) {
        m_powderMaterialCmd = SetMaterialStateCommand();
        m_powderMaterialCmd.layerIndex = m_doc->currentLayerIndex();
        m_powderMaterialCmd.layerId = layer->id;
        m_powderMaterialCmd.before.preset = layer->materialPreset;
        m_powderMaterialCmd.before.state = layer->materialState;
        m_powderMaterialCmd.after = m_powderMaterialCmd.before;
        m_powderMaterialCaptured = true;
    }

    /* P0.9/P1: pour over the authoritative glue/material (reference pourMain).
     * ONE fitted anisotropic heap over the BARE glue (or the material bounds/
     * field centre when no bare glue exists): the engine picks the target, so
     * this is never a regular grid of repeated stamp centres. Existing glitter
     * and colour slots are preserved. */
    /* Capture the material bounds BEFORE the pour so the dirty rect covers old
     * ∪ new material (P0.6). */
    float obx = 0, oby = 0, ox2 = 0, oy2 = 0;
    const bool hasOld = m_powder.materialBoundsWorld(obx, oby, ox2, oy2);

    m_powder.pourGlitterMain(true, m_powderParams);
    m_powder.relax();
    m_powder.decayFlow();
    m_powder.endBlow();

    /* dirty bounds from old ∪ new material bounds + margin (P0.6). */
    float nb0 = 0, nb1 = 0, nb2 = 0, nb3 = 0;
    const bool hasNew = m_powder.materialBoundsWorld(nb0, nb1, nb2, nb3);
    const float margin = qMax<float>(qreal(powderSubRadius()) + 8.0f,
                                     m_powder.worldCellSize() * 3.0f);
    const int mi = int(margin);
    QRect dirty;
    if (hasOld) dirty = QRect(QPoint(int(obx), int(oby)), QPoint(int(ox2), int(oy2))).adjusted(-mi, -mi, mi, mi);
    if (hasNew)
    {
        const QRect nbRect = QRect(QPoint(int(nb0), int(nb1)), QPoint(int(nb2), int(nb3)));
        const QRect nr = nbRect.adjusted(-mi, -mi, mi, mi);
        dirty = dirty.isNull() ? nr : dirty.united(nr);
    }
    m_powderUnion = dirty.isNull() ? QRect() : dirty.intersected(layer->raster.rect());
    if (m_powderUnion.isNull() || m_powderUnion.isEmpty())
    {
        m_powderMaterialCaptured = false;
        return;
    }
    m_strokeHadPaint = true;
    renderPowderUnion();
    /* route through the shared atomic commit so Pour is EXACTLY one undo
     * (P0.10): raster + material state + deferred layer fold to one command. */
    commitScratch(QStringLiteral("Pour glitter"));
    m_powderUnion = QRect();
    m_strokeHadPaint = false;
    m_powderMaterialCaptured = false;
}

/* Momentary Blow: run the full breath over the existing material state (loose
 * leaves/bonds differently, bonded stays); one undo action (P0.10). Starts and
 * re-arms the blow timer itself; it does NOT depend on an active pointer
 * stroke existing. */
void PaintbrushCanvasItem::blowPowderNow()
{
    if (m_tool != TOOL_GLITTER_POWDER && m_tool != TOOL_SMEAR)
        return;
    /* ensure a powder material layer exists (creating one on first Blow edit):
     * the breath then moves that layer's loose powder, bonded stays */
    if (!ensurePowderLayer())
        return;
    ensurePowder();
    /* begin the material capture so the blow commits as exactly one undo */
    if (!m_powderMaterialCaptured)
    {
        CanvasLayer *layer = m_doc->currentLayer();
        if (layer)
        {
            m_powderMaterialCmd = SetMaterialStateCommand();
            m_powderMaterialCmd.layerIndex = m_doc->currentLayerIndex();
            m_powderMaterialCmd.layerId = layer->id;
            m_powderMaterialCmd.before.preset = layer->materialPreset;
            m_powderMaterialCmd.before.state = layer->materialState;
            m_powderMaterialCmd.after = m_powderMaterialCmd.before;
            m_powderMaterialCaptured = true;
        }
    }
    m_blowStandalone = !m_strokeActive;   /* commit on finish without a stroke */
    startPowderBlow();
}

QString PaintbrushCanvasItem::toolName() const
{
    switch (m_tool)
    {
    case TOOL_RECT_SELECT:     return QStringLiteral("Rect select");
    case TOOL_FREEFORM_SELECT: return QStringLiteral("Lasso");
    case TOOL_OBJECT_SELECT:   return QStringLiteral("Object select");
    case TOOL_PENCIL:          return QStringLiteral("Pencil");
    case TOOL_BRUSH:           return QStringLiteral("Brush");
    case TOOL_ERASER:          return QStringLiteral("Eraser");
    case TOOL_FILL:            return QStringLiteral("Fill");
    case TOOL_EYEDROPPER:      return QStringLiteral("Eyedropper");
    case TOOL_SPRAY:           return QStringLiteral("Spray");
    case TOOL_LINE:            return QStringLiteral("Line");
    case TOOL_RECTANGLE:       return QStringLiteral("Rectangle");
    case TOOL_ROUNDED_RECT:    return QStringLiteral("Rounded rect");
    case TOOL_ELLIPSE:         return QStringLiteral("Ellipse");
    case TOOL_TEXT:            return QStringLiteral("Text");
    case TOOL_PAN_ZOOM:        return QStringLiteral("Move canvas");
    case TOOL_BRISTLE:         return QStringLiteral("Bristle");
    case TOOL_GLITTER:         return QStringLiteral("Glitter");
    case TOOL_GLITTER_POWDER:  return QStringLiteral("Glitter powder");
    case TOOL_SMEAR:           return QStringLiteral("Smear");
    case TOOL_ASK:             return QStringLiteral("Ask");
    default:                   return QStringLiteral("Tool");
    }
}

void PaintbrushCanvasItem::updateCursor()
{
    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
    case TOOL_SPRAY:
    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
    {
        /* round tip cursor at the exact on-screen dab size, filled with the
         * current color — the cursor IS the brush (eraser: white fill) */
        const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
        /* the cursor shows the real affected footprint: the powder workbench
         * uses the active sub-tool's own radius (P0.5), not the generic brush */
        const qreal r = (m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR)
                        ? qMax<qreal>(3.0, qreal(powderSubRadius()) * m_zoom)
                        : qMax<qreal>(3.0, m_brushSize * 0.5 * m_zoom);
        const int sizeL = int(qCeil(r * 2 + 8));      /* logical px */
        const int sizePx = int(qCeil(sizeL * dpr));
        QPixmap pm(sizePx, sizePx);
        pm.fill(Qt::transparent);
        pm.setDevicePixelRatio(dpr);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            const QPointF c(sizeL / 2.0, sizeL / 2.0);
            const bool erase = (m_tool == TOOL_ERASER);
            QColor fill = erase ? QColor(255, 255, 255, 210) : m_fgColor;
            fill.setAlpha(erase ? 210 : 170);
            p.setPen(QPen(erase ? QColor(90, 90, 90, 220)
                                : QColor(0, 0, 0, 200), 1.0));
            p.setBrush(fill);
            p.drawEllipse(c, r, r);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 220));
            p.drawEllipse(c, 1.0, 1.0);   /* precision center dot */
        }
        setCursor(QCursor(pm, sizeL / 2, sizeL / 2));
        break;
    }
    case TOOL_TEXT:
        setCursor(Qt::IBeamCursor);
        break;
    case TOOL_PAN_ZOOM:
        setCursor(Qt::OpenHandCursor);
        break;
    case TOOL_FILL:
    case TOOL_EYEDROPPER:
        setCursor(Qt::CrossCursor);
        break;
    default:
        setCursor(Qt::CrossCursor);
        break;
    }
}

void PaintbrushCanvasItem::setFgColor(const QColor &color)
{
    if (m_fgColor == color)
        return;
    m_fgColor = color;
    emit fgColorChanged();
    updateCursor();
}

void PaintbrushCanvasItem::setBgColor(const QColor &color)
{
    if (m_bgColor == color)
        return;
    m_bgColor = color;
    emit bgColorChanged();
}

void PaintbrushCanvasItem::setBrushSize(qreal size)
{
    size = qBound<qreal>(1.0, size, 256.0);
    if (qFuzzyCompare(m_brushSize, size))
        return;
    m_brushSize = size;
    emit brushSizeChanged();
    updateCursor();
}

void PaintbrushCanvasItem::setSprayDensity(qreal density)
{
    density = qBound<qreal>(1.0, density, 500.0);
    if (qFuzzyCompare(m_sprayDensity, density))
        return;
    m_sprayDensity = density;
    emit sprayDensityChanged();
}

void PaintbrushCanvasItem::setSprayFlow(qreal flow)
{
    flow = qBound<qreal>(0.1, flow, 4.0);
    if (qFuzzyCompare(m_sprayFlow, flow))
        return;
    m_sprayFlow = flow;
    emit sprayFlowChanged();
    updateStatus(m_lastCanvasPos);
}

/* --------------------------------------------------- precision drawing */

void PaintbrushCanvasItem::setGridVisible(bool visible)
{
    if (m_gridVisible == visible)
        return;
    m_gridVisible = visible;
    emit gridVisibleChanged();
    update(); /* grid is a scene-graph overlay; it never dirties the raster */
}

void PaintbrushCanvasItem::setRulersVisible(bool visible)
{
    if (m_rulersVisible == visible)
        return;
    m_rulersVisible = visible;
    emit rulersVisibleChanged(); /* the QML rulers are plain items */
}

void PaintbrushCanvasItem::setGridSpacing(int spacing)
{
    spacing = qBound(4, spacing, 512);
    if (m_gridSpacing == spacing)
        return;
    m_gridSpacing = spacing;
    emit gridSpacingChanged();
    update();
}

void PaintbrushCanvasItem::setSnapToGrid(bool snap)
{
    if (m_snapToGrid == snap)
        return;
    m_snapToGrid = snap;
    emit snapToGridChanged();
    updateStatus(m_lastCanvasPos);
}

void PaintbrushCanvasItem::setPressureEnabled(bool enabled)
{
    if (m_pressureEnabled == enabled)
        return;
    m_pressureEnabled = enabled;
    emit pressureEnabledChanged();
    updateStatus(m_lastCanvasPos);
}

void PaintbrushCanvasItem::setPressureToOpacity(bool enabled)
{
    if (m_pressureToOpacity == enabled)
        return;
    m_pressureToOpacity = enabled;
    emit pressureToOpacityChanged();
}

void PaintbrushCanvasItem::setPressureMin(qreal min)
{
    min = qBound<qreal>(0.0, min, 1.0);
    if (qFuzzyCompare(m_pressureMin, min))
        return;
    m_pressureMin = min;
    emit pressureMinChanged();
}

void PaintbrushCanvasItem::setPressureMax(qreal max)
{
    max = qBound<qreal>(0.0, max, 1.0);
    if (qFuzzyCompare(m_pressureMax, max))
        return;
    m_pressureMax = max;
    emit pressureMaxChanged();
}

QPointF PaintbrushCanvasItem::snapPoint(const QPointF &p) const
{
    if (!m_snapToGrid || m_gridSpacing <= 0)
        return p;
    const qreal step = m_gridSpacing;
    return QPointF(qRound(p.x() / step) * step, qRound(p.y() / step) * step);
}

bool PaintbrushCanvasItem::shouldSnap() const
{
    if (!m_snapToGrid || !m_gridVisible)
        return false;
    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
    case TOOL_SPRAY:
    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
    case TOOL_LINE:
    case TOOL_RECTANGLE:
    case TOOL_ROUNDED_RECT:
    case TOOL_ELLIPSE:
    case TOOL_RECT_SELECT:
    case TOOL_FREEFORM_SELECT:
    case TOOL_TEXT:
    case TOOL_FILL:
    case TOOL_EYEDROPPER:
        return true;
    default:
        return false;
    }
}

qreal PaintbrushCanvasItem::pressureFactor() const
{
    /* mouse pressure reads 1.0; only scale when pressure is enabled and a
     * pressure-capable device (tablet/pen) is actually active */
    if (!m_pressureEnabled || !m_tabletActive)
        return 1.0;
    const qreal p = qBound<qreal>(0.0, m_currentPressure, 1.0);
    const qreal min = qBound<qreal>(0.0, m_pressureMin, 1.0);
    const qreal max = qBound<qreal>(min, m_pressureMax, 1.0);
    return min + (max - min) * p;
}

QSGTexture::Filtering PaintbrushCanvasItem::adaptiveFiltering() const
{
    /* pixel-art zoom stays crisp (nearest); continuous zoom smooths */
    return qAbs(m_zoom - qreal(qRound(m_zoom))) < 0.0001
               ? QSGTexture::Nearest
               : QSGTexture::Linear;
}

/* Grid overlay rendered as scene-graph geometry (never part of the raster,
 * never part of the document). Lines are drawn in canvas coordinates under
 * the same pan/zoom transform as the tiles, so they scale with the canvas
 * and stay crisp at integer zoom levels. */
QSGNode *PaintbrushCanvasItem::buildGridNode()
{
    auto *node = new QSGGeometryNode();
    const int rows = m_doc ? m_doc->canvasSize().height() / m_gridSpacing + 1 : 0;
    const int cols = m_doc ? m_doc->canvasSize().width() / m_gridSpacing + 1 : 0;
    const int vertexCount = (rows + cols) * 2;

QSGGeometry *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(),
                                        vertexCount);
    geometry->setLineWidth(1.0f);
    geometry->setDrawingMode(QSGGeometry::DrawLines);

    QSGGeometry::Point2D *vertices = geometry->vertexDataAsPoint2D();
    int idx = 0;
    const int w = m_doc ? m_doc->canvasSize().width() : 0;
    const int h = m_doc ? m_doc->canvasSize().height() : 0;
    for (int x = 0; x <= w; x += m_gridSpacing)
    {
        vertices[idx++].set(x, 0);
        vertices[idx++].set(x, h);
    }
    for (int y = 0; y <= h; y += m_gridSpacing)
    {
        vertices[idx++].set(0, y);
        vertices[idx++].set(w, y);
    }
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    QSGFlatColorMaterial *material = new QSGFlatColorMaterial();
    material->setColor(QColor(0, 0, 0, 60));
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    return node;
}

void PaintbrushCanvasItem::setFillTolerance(qreal tolerance)
{
    tolerance = qBound<qreal>(0.0, tolerance, 255.0);
    if (qFuzzyCompare(m_fillTolerance, tolerance))
        return;
    m_fillTolerance = tolerance;
    emit fillToleranceChanged();
}

/* ------------------------------------------------- Paintbrush 2.0 art tools */

void PaintbrushCanvasItem::setGlitterAmount(qreal v)
{
    v = qBound<qreal>(0.05, v, 2.0);
    if (qFuzzyCompare(qreal(m_glitterParams.amount), v))
        return;
    m_glitterParams.amount = float(v);
    m_powderParams.amount = float(v);   /* mirror: next powder stroke uses it */
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setGlitterFlakeSize(qreal v)
{
    v = qBound<qreal>(0.2, v, 2.5);
    if (qFuzzyCompare(qreal(m_glitterParams.flakeSize), v))
        return;
    m_glitterParams.flakeSize = float(v);
    m_powderParams.flakeSize = float(v);
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setGlitterShimmer(qreal v)
{
    v = qBound<qreal>(0.0, v, 1.0);
    if (qFuzzyCompare(qreal(m_glitterParams.shimmer), v))
        return;
    m_glitterParams.shimmer = float(v);
    m_powderParams.shimmer = float(v);
    /* P0.12: ONE semantic holographic control. The UI value is the SAME value
     * the render pipeline consumes (CPU fallback + GPU uHolo via the render
     * palette). Keep m_powderPal.holographic in lockstep so a later
     * m_powderPal.colors reassignment never splits the two. */
    m_powderPal.holographic = float(v);
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setGlitterAdhesion(qreal v)
{
    v = qBound<qreal>(0.0, v, 1.0);
    if (qFuzzyCompare(qreal(m_glitterParams.adhesion), v))
        return;
    m_glitterParams.adhesion = float(v);
    m_powderParams.adhesion = float(v);
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setPowderLoose(qreal v)
{
    v = qBound<qreal>(0.0, v, 1.0);
    if (qFuzzyCompare(qreal(m_glitterParams.looseMix), v))
        return;
    m_glitterParams.looseMix = float(v);
    m_powderParams.looseMix = float(v);
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setBrushFlow(qreal v)
{
    v = qBound<qreal>(0.05, v, 1.0);
    if (qFuzzyCompare(m_brushFlow, v))
        return;
    m_brushFlow = v;
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setBrushColorVariation(qreal v)
{
    v = qBound<qreal>(0.0, v, 1.0);
    if (qFuzzyCompare(m_brushColorVariation, v))
        return;
    m_brushColorVariation = v;
    emit artParamsChanged();
}

void PaintbrushCanvasItem::setGlitterPalette(const QVector<QColor> &palette, const QString &id)
{
    m_glitterParams.palette = palette;
    m_glitterPaletteId = id;
    emit artParamsChanged();
}

/* Glitter flake diameter in layer-local px at flakeSize == 1: a fine craft
 * glitter. The grain grid lives in layer-local units, which ARE world units at
 * 1x, so export re-renders the same state at any scale (spec §11, §55). */
static constexpr float kGlitterFlakeWorld = 1.6f;

/* Bounded art-material grid: grows the world cell size so a layer-sized grid
 * never exceeds the memory cap (~2M cells). Used by BOTH the glitter grain
 * grid and the powder heightfield so a small flake-size slider value or a very
 * large canvas NEVER silently disables the tool (the old behaviour switched to
 * the brush — which looked like "the tool does nothing"). Returns the final
 * cell size to use (cell is in/out). */
static QSize boundedArtGrid(const QSize &layer, float &cell, qint64 maxCells)
{
    const qint64 w = qMax(1, layer.width()), h = qMax(1, layer.height());
    const qint64 ideal = (qint64(w / qMax(0.01f, cell)) + 2)
                       * (qint64(h / qMax(0.01f, cell)) + 2);
    if (ideal > maxCells)
    {
        const float grow = float(qSqrt(double(ideal) / double(maxCells)));
        cell *= qMax(1.0f, grow);
    }
    return QSize(int(w / cell) + 2, int(h / cell) + 2);
}

void PaintbrushCanvasItem::beginArtStroke(const QPointF &local)
{
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
    if (!layer || layer->raster.isNull() || layer->locked)
        return;
    m_strokeActive = true;
    m_scratchTile = QImage();
    m_scratchSeeded = QRect();
    m_strokeLastDab = local;
    m_strokeSmooth = local;
    m_stabilizer.clear();
    m_stabilizer.append(local);

    if (m_tool == TOOL_BRISTLE)
    {
        art::BristleBrushParams p;
        p.size = float(qMax<qreal>(2.0, m_brushSize));
        p.flow = float(m_brushFlow);
        p.hardness = 0.8f;
        p.smoothing = 0.4f;
        p.colorVariation = float(m_brushColorVariation);
        p.color = m_fgColor;
        /* document seed + stroke count: reproducible, never global randomness */
        p.seed = m_doc->artSeed() ^ (std::uint32_t(m_undoStack ? m_undoStack->count() : 0) * 2654435761u);
        m_bristle.beginStroke(p);
        artStrokeSegment(local, local);
    }
    else if (m_tool == TOOL_GLITTER)
    {
        m_glitterParams.seed = m_doc->artSeed() ^
            (std::uint32_t(m_undoStack ? m_undoStack->count() : 0) * 40503u + 7u);
        m_glitterParams.color = m_fgColor;
        /* the grain grid covers the whole layer in layer-local world units */
        float cell = art::glitter::glitterWorldCellSize(m_glitterParams, kGlitterFlakeWorld);
        const QSize cells = boundedArtGrid(layer->raster.size(), cell, 2 * 1024 * 1024);
        m_glitterState.reset(cells, cell);
        m_glitterLast = local;
        m_glitterUnion = QRect();
        artStrokeSegment(local, local);
    }
    else if (m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR)
    {
        m_glitterParams.seed = m_doc->artSeed() ^
            (std::uint32_t(m_undoStack ? m_undoStack->count() : 0) * 40503u + 7u);
        m_glitterParams.color = m_fgColor;
        m_powderReady = false;
        m_powderBlowing = false;
        m_powderBlowSince = 0.0f;
        m_powderBlowAge = 0.0f;
        m_powderRenderAcc = 0.0f;
        m_powderHoldPos = local;
        m_powderHoldEver = false;
        ensurePowder(true);
        qInfo("pb powder: stroke begin (grid %dx%d cell %.2f, brush %.1f, subtool %d)",
              m_powder.cellW(), m_powder.cellH(), m_powder.worldCellSize(),
              m_brushSize, int(m_powderTool));
        m_powderUnion = QRect();
        /* P1 #6: a Push gesture keeps a finger-carry across all its dabs
         * (reference carried/CLING_MAX = 6). Reset it at the stroke start. */
        if (m_powderTool == POWDER_PUSH)
            m_powder.beginPushGesture();
        artStrokeSegment(local, local);
    }
}

void PaintbrushCanvasItem::artStrokeSegment(const QPointF &fromLocal, const QPointF &toLocal)
{
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
    if (!layer || layer->raster.isNull() || layer->locked || !m_strokeActive)
        return;
    const qreal pf = pressureFactor();
    const float pressure = float(m_pressureEnabled ? pf : 0.75);

    if (m_tool == TOOL_BRISTLE)
    {
        /* the engine needs the affected rect BEFORE painting so the undo
         * before-region and the scratch tile cover it: predict it from the
         * segment + brush radius (same bound the engine returns) */
        const qreal r = qMax<qreal>(2.0, m_brushSize) * 0.5 * 1.4 + 2.0;
        const QRectF seg = QRectF(fromLocal, toLocal).normalized().adjusted(-r, -r, r, r);
        const QRect affected = seg.toAlignedRect().intersected(layer->raster.rect());
        if (affected.isEmpty())
            return;
        captureBeforeRegion(affected);
        ensureScratch(affected);
        const QRect painted = m_bristle.strokeSegment(
            m_scratchTile, fromLocal - QPointF(m_scratchOrigin),
            toLocal - QPointF(m_scratchOrigin), pressure);
        if (!painted.isEmpty())
        {
            m_strokeHadPaint = true;
            markTilesDirty(mappedRect(painted.translated(m_scratchOrigin)));
        }
        m_strokeLastDab = toLocal;
        return;
    }

if (m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR)
    {
        /* ensure the (per-edition) powder layer exists and the engine covers it */
        CanvasLayer *pwLayer = ensurePowderLayer();
        if (!pwLayer)
            return;
        if (layer != pwLayer)
            layer = pwLayer;   /* a dedicated powder layer was created for this edit */
        /* refresh the live interaction colour each segment so the NEXT deposited
         * material uses the current foreground colour (P0.7/P0.8), without
         * touching the persisted sim's existing grains. m_powderParams always
         * mirrors the live UI params (the current colour/amount/flake/shimmer)
         * while keeping the sim's bounded palette authoritative for existing
         * grains, so a fresh pour extends the palette via ensurePowderPaletteSlot. */
        m_glitterParams.color = m_fgColor;
        ensurePowder();
        m_powderParams = m_glitterParams;
        m_powderParams.palette = m_powder.palette();
        m_powderParams.seed = std::uint32_t(m_doc->artSeed() ^
            (std::uint32_t(m_undoStack ? m_undoStack->count() : 0) * 40503u + 7u));
        /* capture the powder layer's material state before the first edit of
         * this physical operation so one undo restores exactly one op */
        if (!m_powderMaterialCaptured)
        {
            m_powderMaterialCmd = SetMaterialStateCommand();
            m_powderMaterialCmd.layerIndex = m_doc->currentLayerIndex();
            m_powderMaterialCmd.layerId = pwLayer->id;
            m_powderMaterialCmd.before.preset = pwLayer->materialPreset;
            m_powderMaterialCmd.before.state = pwLayer->materialState;
            m_powderMaterialCmd.after = m_powderMaterialCmd.before;
            m_powderMaterialCaptured = true;
        }
        /* the active powder sub-tool's own reference radius (P0.5): never the
         * generic brushSize, so Glue/Eraser width vs Glitter/Push radius and
         * per-subtool defaults/ranges are respected */
        const float radius = powderSubRadius();
        const QLineF seg(fromLocal, toLocal);
        const qreal len = seg.length();
        const qreal step = qMax<qreal>(1.0, radius * 0.35);
        const QPointF delta = toLocal - fromLocal;
        const QPointF trans = len > 0.0 ? QPointF(delta.x() / len, delta.y() / len) : QPointF();
        int steps = qMax(1, int(len / step));

        /* A plow/smear can only change things when there is loose powder to push.
         * For the PHYSICAL ERASER, however, loose mass alone is not enough: it
         * also removes fully-bonded powder and wet glue (whose loose mass is
         * zero). Use the before/after material-state fingerprint so erasing a
         * glue-only or 100%-bonded area still commits an undo (P0/P1 #4).
         * We snapshot here and compare against the post-stroke fingerprint at
         * the end of the stroke. */
        const QRectF beforeB = QRectF(fromLocal, toLocal).normalized()
                                   .adjusted(-radius - 6, -radius - 6, radius + 6, radius + 6);
        bool toStringHasMaterial =
            m_powderTool == POWDER_GLUE || m_powderTool == POWDER_GLITTER
            || m_powder.looseMassInRect(float(beforeB.left()), float(beforeB.top()),
                                        float(beforeB.right()), float(beforeB.bottom()))
                   > 0.002f;
        if (m_powderTool == POWDER_ERASER)
            m_eraseFingerprintBefore =
                m_powder.materialRectFingerprint(float(beforeB.left()),
                                                 float(beforeB.top()),
                                                 float(beforeB.right()),
                                                 float(beforeB.bottom()));
        /* after the erase runs below, a changed physical fingerprint means the
         * operation really removed material and must commit */
        m_eraseFingerprintAfter = m_eraseFingerprintBefore;

        /* P0.6: the authoritative dirty bounds come from the PHYSICAL MATERIAL
         * bounds (old ∪ new), never just the pointer segment. The sim expands
         * beyond the pointer via heap profile / dust / relax / Push / Blow, and
         * a pointer-derived render rect would CLIP the spilled material. */
        float obx = 0, oby = 0, ox2 = 0, oy2 = 0;
        const bool hasOld = m_powder.materialBoundsWorld(obx, oby, ox2, oy2);

        for (int i = 0; i <= steps; ++i)
        {
            const QPointF p = len > 0.0 ? seg.pointAt(qreal(i) / steps) : fromLocal;
            switch (m_powderTool)
            {
            case POWDER_GLUE:
                /* draw a visible wet transparent glue stroke; changes glue only,
                 * never emits glitter */
                m_powder.paintGlue(float(p.x()), float(p.y()), radius,
                                   m_powderParams.adhesion);
                break;
            case POWDER_GLITTER:
                /* sprinkle loose glitter; no glue deposition (the reference's
                 * Glitter mode) */
                m_powder.pourGlitter(float(p.x()), float(p.y()), radius,
                                     m_powderParams);
                break;
            case POWDER_PUSH:
                /* smear/plow existing loose grains; bonded stay; creates nothing.
                 * The finger-carry lives in the engine (beginPushGesture kept it
                 * for this whole gesture), so no per-dab carried is passed. */
                m_powder.smear(float(p.x()), float(p.y()), float(trans.x()),
                               float(trans.y()), radius, 0.0f);
                break;
            case POWDER_ERASER:
                /* physically erase glue + bonded + loose under the disc */
                m_powder.erase(float(p.x()), float(p.y()), radius, 1.0f);
                break;
            case POWDER_POUR:
            case POWDER_BLOW:
            default:
                /* momentary actions are invoked explicitly (pourPowderNow /
                 * blowPowderNow); they are never a stroke mode */
                break;
            }
        }
        m_powder.relax();
        m_powder.decayFlow();

        /* union the OLD and NEW physical material bounds + a presentation
         * support margin (grain radius, flake radius, contact shadow, wet-glue
         * gradient, bloom), then fold into the running operation dirty union.
         * The physics decides the bounds — the pointer never does (P0.6). */
        float nbx = 0, nby = 0, nx2 = 0, ny2 = 0;
        const bool hasNew = m_powder.materialBoundsWorld(nbx, nby, nx2, ny2);
        const float margin = qMax<float>(qreal(powderSubRadius()) + 8.0f,
                                         m_powder.worldCellSize() * 3.0f);
        const int mi = int(margin);
        QRect opDirty;
        if (hasOld)
            opDirty = QRect(QPoint(int(obx), int(oby)), QPoint(int(ox2), int(oy2)))
                          .adjusted(-mi, -mi, mi, mi);
        if (hasNew)
        {
            const QRect nbRect = QRect(QPoint(int(nbx), int(nby)), QPoint(int(nx2), int(ny2)));
            const QRect nr = nbRect.adjusted(-mi, -mi, mi, mi);
            opDirty = opDirty.isNull() ? nr : opDirty.united(nr);
        }
        /* Eraser must also invalidate its OLD material so deleted material is
         * actually cleared from the raster (P0.6). */
        if (m_powderTool == POWDER_ERASER)
        {
            const QRect obRect = QRect(QPoint(int(obx), int(oby)), QPoint(int(ox2), int(oy2)));
            const QRect er = obRect.adjusted(-mi, -mi, mi, mi);
            opDirty = opDirty.isNull() ? er : opDirty.united(er);
        }
        const QRect affected = opDirty.isNull()
            ? QRect() : opDirty.intersected(layer->raster.rect());
        if (affected.isEmpty())
            return;
        m_powderUnion = m_powderUnion.isNull() ? affected : m_powderUnion.united(affected);
        captureBeforeRegion(affected);
        ensureScratch(affected);
        /* live preview: a presentation clock (not wall-clock-as-material)
         * drives the holographic shimmer while drawing; the committed pass
         * (renderPowderUnion) uses phase 0 for a deterministic static result */
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastPowderPreviewMs > 0)
            m_powderShimmerMs += now - m_lastPowderPreviewMs;
        m_lastPowderPreviewMs = now;
        /* render with the sim's AUTHORITATIVE palette so grains deposited as
         * new colours resolve to their true slots (never clamped to paper), and
         * ONE holographic control drives the material (P0.12) */
        m_powderPal.colors = m_powder.palette();
        m_powderPal.holographic = m_powderParams.shimmer;
        QImage flakes = m_powder.render(affected.size(), float(affected.x()),
                                        float(affected.y()), 1.0f,
                                        m_doc->lightRig(), m_powderPal,
                                        m_doc->paperStock(), m_doc->artScale(1.0f),
                                        m_powderShimmerMs);
        {
            QPainter p(&m_scratchTile);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(affected.topLeft() - m_scratchOrigin, layer->raster, affected);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.drawImage(affected.topLeft() - m_scratchOrigin, flakes);
            p.end();
        }
        if (m_powderTool == POWDER_ERASER)
        {
            /* the physical eraser removed loose+bonded+glue; compare the
             * before/after material-state fingerprint so erasing a glue-only or
             * fully-bonded region (loose mass zero) still commits the operation
             * (P0/P1 #4). */
            m_eraseFingerprintAfter =
                m_powder.materialRectFingerprint(float(beforeB.left()),
                                                 float(beforeB.top()),
                                                 float(beforeB.right()),
                                                 float(beforeB.bottom()));
            if (m_eraseFingerprintAfter != m_eraseFingerprintBefore)
                toStringHasMaterial = true;
        }
        if (toStringHasMaterial)
            m_strokeHadPaint = true;
        markTilesDirty(mappedRect(affected));
        m_strokeLastDab = toLocal;
        return;
    }

    if (m_tool == TOOL_GLITTER)
    {
        const float radius = float(qMax<qreal>(3.0, m_brushSize) * 0.5);
        const QLineF seg(fromLocal, toLocal);
        const qreal len = seg.length();
        const qreal step = qMax<qreal>(1.0, radius * 0.35);
        /* pour along the segment; the hand's motion is the "push" vector for
         * loose grains (spec §14.2 displacement) */
        const QPointF transport = (toLocal - fromLocal) * 0.15;
        int steps = qMax(1, int(len / step));
        for (int i = 0; i <= steps; ++i)
        {
            const QPointF p = len > 0.0 ? seg.pointAt(qreal(i) / steps) : fromLocal;
            m_glitterState.paintGlue(float(p.x()), float(p.y()), radius,
                                     m_glitterParams.adhesion * 0.5f);
            m_glitterState.applyGlitter(float(p.x()), float(p.y()), radius,
                                        m_glitterParams, transport,
                                        0.6f + 0.4f * pressure);
        }
        const QRectF r = QRectF(fromLocal, toLocal).normalized()
                             .adjusted(-radius - 3, -radius - 3, radius + 3, radius + 3);
        const QRect affected = r.toAlignedRect().intersected(layer->raster.rect());
        if (affected.isEmpty())
            return;
        m_glitterUnion = m_glitterUnion.isNull() ? affected : m_glitterUnion.united(affected);
        captureBeforeRegion(affected);
        ensureScratch(affected);
        /* live preview: render the touched region of the grain grid into the
         * scratch tile (deterministic, so the preview IS the final result) */
        const art::ArtLightRig &rig = m_doc->lightRig();
        const art::ArtScale scale = m_doc->artScale(1.0f);
        QImage flakes = m_glitterState.render(affected.size(), float(affected.x()),
                                              float(affected.y()), 1.0f, rig,
                                              m_doc->paperStock(), scale);
        {
            QPainter p(&m_scratchTile);
            /* reseed the region from the layer first so re-rendering the
             * (grown) pile never stacks previews on top of previews */
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(affected.topLeft() - m_scratchOrigin, layer->raster, affected);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.drawImage(affected.topLeft() - m_scratchOrigin, flakes);
            p.end();
        }
        m_strokeHadPaint = true;
        markTilesDirty(mappedRect(affected));
        m_glitterLast = toLocal;
        m_strokeLastDab = toLocal;
    }
}

void PaintbrushCanvasItem::endArtStroke(const QPointF &local)
{
    pbCrashTrace("endArtStroke enter");
    if (!m_strokeActive) {
        pbCrashTrace("endArtStroke exit (not active)");
        return;
    }
    artStrokeSegment(m_strokeLastDab, local);
    m_strokeActive = false;
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;

    if (m_tool == TOOL_GLITTER && layer && m_strokeHadPaint && !m_glitterUnion.isEmpty())
    {
        /* final pass: render the whole pile once over the full affected
         * union (segments only previewed their own rect) */
        const QRect u = m_glitterUnion.intersected(layer->raster.rect());
        captureBeforeRegion(u);
        ensureScratch(u);
        QImage flakes = m_glitterState.render(u.size(), float(u.x()), float(u.y()), 1.0f,
                                              m_doc->lightRig(), m_doc->paperStock(),
                                              m_doc->artScale(1.0f));
        QPainter p(&m_scratchTile);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(u.topLeft() - m_scratchOrigin, layer->raster, u);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawImage(u.topLeft() - m_scratchOrigin, flakes);
        p.end();
        markTilesDirty(mappedRect(u));
    }
else if ((m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR)
         && layer && m_strokeHadPaint && !m_powderUnion.isEmpty())
    {
        /* final powder pass over the full union (reseeds + clears masked
         * committed powder + overlays current sim, then marks tiles dirty) */
        pbCrashTrace("endArtStroke -> renderPowderUnion enter");
        renderPowderUnion();
        pbCrashTrace("endArtStroke -> renderPowderUnion return");
    }
    /* P1 #6: drop the persistent Push finger-carry at the stroke end exactly as
     * the reference resets `carried` when the finger lifts. */
    if (m_powderTool == POWDER_PUSH)
        m_powder.endPushGesture();
if (m_strokeHadPaint)
    {
        const QString name = m_tool == TOOL_GLITTER ? QStringLiteral("Glitter")
                            : m_tool == TOOL_GLITTER_POWDER ? QStringLiteral("Glitter powder")
                            : m_tool == TOOL_SMEAR ? QStringLiteral("Smear")
                            : QStringLiteral("Bristle paint");
        pbCrashTrace("commitScratch begin");
        commitScratch(name);
        pbCrashTrace("commitScratch end");
    }
    else if (m_powderInsertCmd)
    {
        /* The stroke painted nothing but a dedicated powder layer was
         * auto-created for it: cancel the creation — remove the empty layer
         * and restore the previous selection, discarding the deferred insert
         * so no phantom layer or bogus undo entry lingers (P0.11). */
        if (m_doc)
        {
            const int insertedIdx = m_powderInsertCmd->index;
            const int prevIdx = m_powderInsertCmd->prevCurrentIndex;
            QList<CanvasLayer> layers = m_doc->layers();
            if (insertedIdx >= 0 && insertedIdx < layers.size()
                && layers.at(insertedIdx).id == m_powderInsertCmd->layer.id)
                layers.removeAt(insertedIdx);
            m_doc->setLayers(layers);
            if (prevIdx >= 0 && prevIdx < layers.size())
                m_doc->setCurrentLayerIndex(prevIdx);
            m_doc->setDirty(true);
        }
        m_powderInsertCmd.reset();
    }
    if (m_tool == TOOL_BRISTLE)
        m_bristle.endStroke();
    m_strokeBefore.clear();
    m_strokeUnion = QRect();
    m_scratchTile = QImage();
    m_scratchSeeded = QRect();
    m_powderReady = false;
    m_powderBlowing = false;
    m_powderBlowSince = 0.0f;
    m_powderBlowAge = 0.0f;
    m_powderRenderAcc = 0.0f;
    m_powderHoldEver = false;
    m_powderShimmerMs = 0;
    m_lastPowderPreviewMs = 0;
    m_glitterUnion = QRect();
    m_powderUnion = QRect();
    m_powderMaterialCaptured = false;
    m_powderInsertCmd.reset();
    pbCrashTrace("endArtStroke exit");
}

void PaintbrushCanvasItem::ensurePowder(bool force)
{
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
    if (!layer)
        return;
    /* powder cells are larger than glitter grains (a coarse flake grid); the
     * grid is ADAPTIVELY bounded so a small flake-size or a huge canvas never
     * silently disables the tool (see boundedArtGrid) */
    float cell = art::glitter::powderWorldCellSize(m_glitterParams, kGlitterFlakeWorld);
    const QSize cells = boundedArtGrid(layer->raster.size(), cell, 2 * 1024 * 1024);

    const bool existingPowder =
        layer->materialPreset == QLatin1String(art::glitter::kPowderMaterialPreset)
        && !layer->materialState.isEmpty();

    /* If the layer already carries a persisted physical powder state, recover it
     * (switch-to-powder or switch-layers): never wipe it with a fresh reset. */
    if (existingPowder && (!m_powderReady
                           || m_powder.cellW() != cells.width()
                           || m_powder.cellH() != cells.height()))
    {
        art::glitter::GlitterPowder restored;
        if (restored.deserialize(layer->materialState)
            && restored.cellW() == cells.width()
            && restored.cellH() == cells.height())
        {
            /* adopt the persisted state + its palette so edits continue the sim */
            m_powder = std::move(restored);
            m_powderPal.colors = m_powder.palette();
            if (const art::PaperStock *ps = m_doc->paperStock())
                m_powderPal.paper = ps->baseColor;
            /* P0.8: the CURRENT UI interaction parameters (colour/amount/flake
             * size/shimmer/adhesion) must describe the NEXT deposited material.
             * Refresh m_powderParams from the live UI params WITHOUT destroying
             * the restored persisted palette (existing grains keep their slots).
             * The engine's own palette is authoritative for existing grains;
             * m_powderParams.palette stays in sync so pours extend it via
             * ensurePowderPaletteSlot. */
            m_powderParams = m_glitterParams;
            m_powderParams.palette = m_powder.palette();
            m_powderParams.seed = std::uint32_t(m_doc->artSeed() ^
                (std::uint32_t(m_undoStack ? m_undoStack->count() : 0) * 40503u + 7u));
            m_powderReady = true;
            qInfo("pb powder: restored material layer %dx%d (cell %.2f)",
                  m_powder.cellW(), m_powder.cellH(), m_powder.worldCellSize());
            return;
        }
        /* fall through: persisted state is incompatible (e.g. grid changed) */
    }

    /* Idempotent within a stroke: once the grid covers the current layer we
     * don't reset it every segment. Only `force` (stroke begin) OR a grid/layer
     * change rebuilds; a NON-powder layer always gets a fresh grid. */
    if (!force && m_powderReady && m_powder.cellW() == cells.width()
        && m_powder.cellH() == cells.height())
        return;
    std::uint32_t seed = m_doc->artSeed() ^
        (std::uint32_t(m_undoStack ? m_undoStack->count() : 0) * 40503u + 7u);
    m_powder.reset(cells, cell, seed);
    /* the reference's palette is the single source of truth: deposit slot 0 = the
     * signature pink so the sim and the render agree, and cache it for clearing
     * committed powder back to the paper colour during a hydrate-blow. */
    m_powderPal = art::glitter::defaultPowderPalette();
    m_powderParams = m_glitterParams;
    m_powderParams.seed = seed;
    m_powderParams.palette = m_powderPal.colors;
    /* clear/hydrate-restore committed powder back onto the document's actual
     * paper colour, not a hardcoded white */
    if (const art::PaperStock *ps = m_doc->paperStock())
        m_powderPal.paper = ps->baseColor;
    m_powderReady = true;
}

/* Ensure the current layer is a Glitter-POWDER material layer. On the first
 * physical powder edit of any non-powder layer, create a DEDICATED transparent
 * powder material layer (never destructively convert the selected raster/image
 * layer), switch to it and mark it as the powder material preset. */
CanvasLayer *PaintbrushCanvasItem::ensurePowderLayer()
{
    if (!m_doc)
        return nullptr;
    CanvasLayer *cur = m_doc->currentLayer();
    if (cur && cur->materialPreset == QLatin1String(art::glitter::kPowderMaterialPreset))
        return cur;   /* already a powder material layer: edit it */

    /* create a dedicated transparent powder layer sized to the canvas */
    const QSize cs = m_doc->canvasSize();
    CanvasLayer layer = CanvasLayer::createRaster(cs, QStringLiteral("Glitter powder"));
    layer.materialPreset = art::glitter::kPowderMaterialPreset;
    layer.materialState.clear();       /* empty physical state; filled on commit */

    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Glitter powder layer");
    QList<CanvasLayer> layers = m_doc->layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_doc->currentLayerIndex();
    m_doc->setLayers(layers);
    m_doc->setCurrentLayerIndex(cmd.index);
    cmd.layer = *m_doc->layerAt(cmd.index);
    /* DEFER the insert until commit (P0.11): it becomes the first child of the
     * operation's CompositeCommand, so the auto-created layer + the raster
     * stroke + the material state are ONE undo step. Not pushed here. */
    m_powderInsertCmd = std::make_unique<InsertLayerCommand>(cmd);
    m_doc->setDirty(true);
    /* the powder grid must size to this new layer */
    m_powderReady = false;
    return m_doc->currentLayer();
}

void PaintbrushCanvasItem::renderPowderPreview()
{
    pbCrashTrace("renderPowderPreview enter");
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
    if (!layer || layer->raster.isNull()) {
        pbCrashTrace("renderPowderPreview exit (no layer)");
        return;
    }
    if (m_powderUnion.isNull() || m_powderUnion.isEmpty()) {
        pbCrashTrace("renderPowderPreview exit (no union)");
        return;
    }
    const QRect u = m_powderUnion.intersected(layer->raster.rect());
    if (u.isEmpty()) {
        pbCrashTrace("renderPowderPreview exit (empty union)");
        return;
    }
    captureBeforeRegion(u);
    ensureScratch(u);

    /* Deterministic COMMIT bake: t=0 material bake. The CPU material renderer
     * used by the live preview is the CANONICAL commit path (transparent
     * material layer, no baked paper). The GPU powder.frag surface is an
     * EXPERIMENTAL path only while a real-GUI parity gate proves its
     * MATERIAL_LAYER output matches the CPU material renderer.
     *
     * Gate semantics (temporary containment):
     *   PB_POWDER_COMMIT=gpu   -> GPU experimental path
     *   everything else        -> CPU canonical path
     */
    QImage flakes;
    bool bakedGpu = false;
    /* render with the sim's AUTHORITATIVE palette so the committed material
     * matches the live preview (grains deposited as new colours resolve to
     * their true slots) */
    m_powderPal.colors = m_powder.palette();
    m_powderPal.holographic = m_powderParams.shimmer;
    const QByteArray commitMode = qgetenv("PB_POWDER_COMMIT").trimmed();
    const bool allowGpuCommit =
        !commitMode.isNull() && !commitMode.compare("gpu", Qt::CaseInsensitive);
    pbCrashTrace("GpuRuntime initialize begin");
    if (!m_gpuRenderTried) {
        m_gpuRenderTried = true;
        m_gpuRender.initialize(true);
    }
    pbCrashTrace("GpuRuntime initialize end");
    if (allowGpuCommit && m_gpuRender.isReady()) {
        pbCrashTrace("renderPowder begin");
        flakes = m_gpuRender.renderPowder(u.size(), m_powder, m_powderPal,
                                          float(u.x()), float(u.y()), 1.0f,
                                          m_doc->lightRig(), m_doc->paperStock(),
                                          /*timeMs=*/0, /*uiDpr=*/1,
                                          art::GpuRuntime::PowderOutput::MaterialLayer);
        pbCrashTrace("renderPowder end");
        /* Redo the bake semantics via the renderer result, NOT by guessing a
         * background colour on a 32-bit image. `renderPowder` returns an opaque
         * ARGB32 image; treat a non-null, non-empty image as a renderer success
         * and verify content on actual scanlines/format below. */
        if (!flakes.isNull() && !flakes.size().isEmpty()) {
            pbCrashTrace("GPU result validation");
            bakedGpu = true;
            /* content sanity: a format-aware scanline probe (never .color(0)) */
            if (flakes.format() == QImage::Format_ARGB32
                || flakes.format() == QImage::Format_RGBA8888
                || flakes.format() == QImage::Format_RGB32) {
                const int h = flakes.height();
                const int w = flakes.width();
                const int bpl = flakes.bytesPerLine();
                const uchar *p0 = flakes.constBits();
                int nonZero = 0;
                /* scan a sparse set of scanlines for any non-black pixel */
                const int stepY = qMax(1, h / 16);
                for (int y = 0; y < h && nonZero < 4; y += stepY) {
                    const uchar *sl = p0 + qint64(y) * bpl;
                    for (int x = 0; x < w; x += 3) {
                        if (sl[x * 4 + 0] || sl[x * 4 + 1] || sl[x * 4 + 2] || sl[x * 4 + 3]) {
                            ++nonZero;
                            break;
                        }
                    }
                }
                if (nonZero == 0)
                    bakedGpu = false;   /* genuinely empty frame: don't bake*/
            }
        }
    }
    if (!bakedGpu) {
        pbCrashTrace("CPU Voronoi fallback render");
        flakes = m_powder.render(u.size(), float(u.x()), float(u.y()), 1.0f,
                                 m_doc->lightRig(), m_powderPal,
                                 m_doc->paperStock(), m_doc->artScale(1.0f),
                                 0);
    }

    pbCrashTrace("QPainter composite");
    QPainter p(&m_scratchTile);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    /* The powder layer is a MATERIAL layer: its raster is a deterministic
     * re-bake of layer.materialState (the authoritative physics). Regenerate
     * the union region from scratch (clear to transparent then draw the fresh
     * material render) so removed material actually vanishes — never re-seed
     * stale committed powder from the old raster. */
    p.fillRect(u.translated(-m_scratchOrigin), QColor(0, 0, 0, 0));
    p.drawImage(u.topLeft() - m_scratchOrigin, flakes);
    p.end();
    pbCrashTrace("markTilesDirty");
    markTilesDirty(mappedRect(u));
    m_strokeHadPaint = true;
    pbCrashTrace("renderPowderPreview exit");
    qDebug() << "pb_powder preview" << u.size() << "gpu=" << bakedGpu;
}

/* Commit-bake path: render the union then persist the authoritative material
 * state onto the layer (the raster stays a cached bake). Used at operation
 * FINALIZE (stroke end / pour / finished blow AFTER endBlow has been called
 * once) — never during a live blow's per-step preview. */
void PaintbrushCanvasItem::renderPowderUnion()
{
    renderPowderPreview();
    CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
    if (layer && !m_powder.isEmpty())
        persistPowderStateToLayer(layer);
}

/* Persist the authoritative GlitterPowder physical state onto the layer. The
 * raster remains a cached bake; save/load, export and tool switches re-derive
 * from this material state (never from raster re-hydration). */
void PaintbrushCanvasItem::persistPowderStateToLayer(CanvasLayer *layer)
{
    if (!layer)
        return;
    /* PERSISTENCE IS SIDE-EFFECT FREE (P0 reviewer): we do NOT call
     * m_powder.endBlow() here. Ending a breath belongs to the OPERATION
     * lifecycle (finalizePowderOperation), never to serialization — otherwise
     * every preview render during a live breath resets the transient blow
     * state and the breath self-cancels. This function only records the
     * authoritative physics state; callers finalize the operation explicitly. */
    if (m_powder.isEmpty()) {
        /* nothing left (fully erased/blown): record no material state */
        if (layer->materialPreset == art::glitter::kPowderMaterialPreset)
            layer->materialPreset.clear();
        layer->materialState.clear();
        /* keep the undo command's `after` in sync with what we persist */
        m_powderMaterialCmd.after.preset = layer->materialPreset;
        m_powderMaterialCmd.after.state = layer->materialState;
        return;
    }
    layer->materialPreset = art::glitter::kPowderMaterialPreset;
    layer->materialState = m_powder.serialize();
    m_powderMaterialCmd.after.preset = layer->materialPreset;
    m_powderMaterialCmd.after.state = layer->materialState;
}

void PaintbrushCanvasItem::startPowderBlow()
{
    if (m_powderBlowing
        || (m_tool != TOOL_GLITTER_POWDER && m_tool != TOOL_SMEAR))
        return;
    /* A pure press-and-hold (no drag: `m_powderHoldEver` stays false) is the
     * reference's "blow" gesture — it should clear powder from a PREVIOUS
     * stroke already committed.
     *
     * The authoritative material state lives in layer.materialState (CanvasLayer
     * is the single source of truth), NEVER reconstructed from the cached
     * raster. `ensurePowder` deserializes that state into `m_powder` when the
     * layer carries one; the breath then operates on the true physics. We do
     * not call hydrateFromRaster here — that is a legacy/import recovery path
     * only, never the normal persistent-material blow flow. */
    if (!m_powderHoldEver)
    {
        CanvasLayer *layer = m_doc ? m_doc->currentLayer() : nullptr;
        if (layer && !layer->raster.isNull())
        {
            ensurePowder(false);
            /* the breath sweeps powder anywhere across (and off) the page, so
             * render the MATERIAL bounds (powder + glue, P0.6) grown by a
             * travel margin — keeps pushed flakes visible without a
             * full-canvas render budget */
            float minx = 0, miny = 0, maxx = 0, maxy = 0;
            if (m_powder.materialBoundsWorld(minx, miny, maxx, maxy))
            {
                const float margin = std::max(96.0f, layer->raster.width() * 0.12f);
                m_powderUnion = QRectF(minx, miny, (maxx - minx) + margin,
                                       (maxy - miny) + 24.0f).toAlignedRect()
                                    .intersected(layer->raster.rect());
            }
            else
            {
                m_powderUnion = QRect();
            }
        }
    }
    m_powder.beginBlow();
    m_powderBlowing = true;
    m_powderBlowAge = 0.0f;
    m_powderRenderAcc = 0.0f;
    /* initial live preview of the yet-running field: render-only (never
     * persists or ends the breath — P0 side-effect-free preview). */
    renderPowderPreview();
}

void PaintbrushCanvasItem::stepPowderBlow(float dtSec)
{
    if (!m_powderBlowing)
        return;
    m_powderBlowAge += dtSec;
    const int moved = m_powder.blowStep(dtSec);
    /* A breath runs a few seconds, or until the loose powder is all gone. The
     * first fraction of a second is a gentle ramp where the wind hasn't built
     * velocity yet, so `moved` may be 0 briefly — only end on `moved == 0`
     * after the breath has visibly run (>= 0.8 s), never on the opening ramp.
     * P0 #1: we MUST NOT finalize/commit while airborne flakes are still in
     * flight — a canonical bake that serializes `materialState` would otherwise
     * contain transient particle mass that `materialState` doesn't hold,
     * breaking the raster == materialState invariant. So the breath finishes
     * only once the loose bed is settled AND no flake is airborne (the flakes
     * keep stepping until they land/off-page). A generous safety cap still
     * force-ends a runaway (even then the transient fleet is dropped at the
     * settled commit). */
    const bool bedSettled = m_powderBlowAge > 3.5f
                            || (moved == 0 && m_powderBlowAge > 0.8f);
    const bool airborne = m_powder.hasActiveParticles();
    const bool finished = (bedSettled && !airborne)
                          || m_powderBlowAge > 12.0f;
    /* Render the evolving field at ~30 Hz. This is a LIVE PREVIEW (render-only),
     * so it must NOT persist material state or end the breath — the preview is
     * side-effect free (P0): persistence/endBlow/undo belong to finalize. */
    m_powderRenderAcc += dtSec;
    if (m_powderRenderAcc >= 1.0 / 30.0)
    {
        m_powderRenderAcc = 0.0f;
        renderPowderPreview();
    }
    if (finished)
    {
        /* ---- FINALIZE the breath exactly once ----
         * 1) end the breath's transient state ONCE (drop per-step momentum),
         * 2) bake the settled raster + persist the authoritative material state,
         * 3) commit ONE atomic undo. Never render-persist mid-breath again. */
        m_powder.endBlow();
        m_powderBlowing = false;
        m_powderBlowSince = 0.0f;
        m_powderBlowAge = 0.0f;
        m_powderRenderAcc = 0.0f;
        if (m_strokeActive)
        {
            /* commit the stroke (renderPowderUnion persists the settled state) */
            endArtStroke(m_powderHoldPos);
        }
        else if (m_blowStandalone)
        {
            /* P0.10: standalone Blow (no active pointer stroke) must commit the
             * blown result + material state as exactly ONE atomic undo */
            if (m_strokeHadPaint)
            {
                renderPowderUnion();   /* render + persist settled material */
                commitScratch(QStringLiteral("Blow"));
            }
            m_powderUnion = QRect();
            m_strokeHadPaint = false;
            m_powderMaterialCaptured = false;
            m_powderInsertCmd.reset();
            m_blowStandalone = false;
        }
    }
}

void PaintbrushCanvasItem::setZoom(qreal zoom)
{
    zoom = qBound<qreal>(0.05, zoom, 32.0);
    if (qFuzzyCompare(m_zoom, zoom))
        return;
    m_zoom = zoom;
    emit zoomChanged();
    update();
    updateCursor();   /* the brush-tip cursor size tracks the on-screen scale */
}

void PaintbrushCanvasItem::setPanOffset(const QPointF &offset)
{
    if (m_panOffset == offset)
        return;
    m_panOffset = offset;
    emit panOffsetChanged();
    update();
}

void PaintbrushCanvasItem::setObjectMaskVisible(bool visible)
{
    if (m_objectMaskVisible == visible)
        return;
    m_objectMaskVisible = visible;
    emit objectMaskVisibleChanged(visible);
    markAllTilesDirty();
    update();
}

/* ------------------------------------------------------- coordinate maps */

QPointF PaintbrushCanvasItem::canvasFromItem(const QPointF &itemPos) const
{
    return QPointF((itemPos.x() - m_panOffset.x()) / m_zoom,
                   (itemPos.y() - m_panOffset.y()) / m_zoom);
}

QPointF PaintbrushCanvasItem::itemFromCanvas(const QPointF &canvasPos) const
{
    return QPointF(canvasPos.x() * m_zoom + m_panOffset.x(),
                   canvasPos.y() * m_zoom + m_panOffset.y());
}

QRect PaintbrushCanvasItem::canvasRect() const
{
    if (!m_doc)
        return {};
    return QRect(QPoint(0, 0), m_doc->canvasSize());
}

/* --------------------------------------------------------------- tiles */

QRect PaintbrushCanvasItem::tileRect(int tx, int ty) const
{
    return QRect(tx * TILE, ty * TILE, TILE, TILE).intersected(canvasRect());
}

void PaintbrushCanvasItem::markTilesDirty(const QRect &canvasRect_)
{
    const QRect clipped = canvasRect_.intersected(canvasRect());
    if (clipped.isEmpty())
        return;

    const int x0 = clipped.left() / TILE;
    const int y0 = clipped.top() / TILE;
    const int x1 = clipped.right() / TILE;
    const int y1 = clipped.bottom() / TILE;

    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx)
            m_dirtyTiles.insert({tx, ty});

    /* coalesced: the repaint timer flushes the dirty set at a fixed cadence
     * (compositeDirtyTiles + update), never synchronously per call. Only arm
     * the timer if it is not already running — restarting it on every dab at
     * ~1 kHz input would keep pushing the 8 ms deadline out and stutter the
     * stroke behind the cursor. */
    if (!m_repaintTimer.isActive())
        m_repaintTimer.start();
    update();
}

void PaintbrushCanvasItem::markAllTilesDirty()
{
    const QRect r = canvasRect();
    if (r.isEmpty())
        return;
    markTilesDirty(r);
}

void PaintbrushCanvasItem::compositeTile(int tx, int ty, QImage &tile) const
{
    const QRect r = tileRect(tx, ty);
    tile = QImage(r.size(), QImage::Format_ARGB32);
    tile.fill(Qt::transparent);

    if (!m_doc || r.isEmpty())
        return;

    /* layers, bottom to top — through the single canonical compositor so
     * the screen always shows exactly what export/eyedropper/AI see. The
     * Showroom time axis: living layers render at m_animTimeMs, everything
     * else is unchanged (t=0 == the static path). */
    DocumentRenderer::composite(tile, r, *m_doc, m_animTimeMs);

    /* in-progress stroke / transient-material preview: dabs live in the scratch
     * tile until release (commitScratch writes them into the layer), so the
     * composite must blend the scratch on top or the stroke is invisible while
     * drawing and only shows up on a later unrelated repaint.
     *
     * A transient ART preview exists ANY time material is mid-operation but not
     * yet committed to the layer — that includes an active pointer stroke AND
     * a standalone momentary Blow (m_powderBlowing && m_blowStandalone), whose
     * scratch preview must also be visible while the breath runs. P0: standalone
     * Blow's preview is invisible without this (it never sets m_strokeActive). */
    const bool hasTransientArtPreview =
        m_strokeActive
        || (m_powderBlowing && m_blowStandalone);
    if (hasTransientArtPreview && !m_scratchTile.isNull())
    {
        const QRect overlap =
            r.intersected(QRect(m_scratchOrigin, m_scratchTile.size()));
        if (!overlap.isEmpty())
        {
            QPainter p(&tile);
            p.setClipRect(tile.rect());
            p.drawImage(overlap.topLeft() - r.topLeft(), m_scratchTile,
                        overlap.translated(-m_scratchOrigin));
            p.end();
        }
    }

    /* object selection mask overlay (AI cutout preview) */
    if (m_objectMaskVisible && !m_objectMask.isNull())
    {
        QPainter p(&tile);
        for (int y = 0; y < r.height(); ++y)
        {
            const int my = r.top() + y;
            if (my < 0 || my >= m_objectMask.height())
                continue;
            const uchar *mline = m_objectMask.constScanLine(my);
            QRgb *tline = reinterpret_cast<QRgb *>(tile.scanLine(y));
            for (int x = 0; x < r.width(); ++x)
            {
                const int mx = r.left() + x;
                if (mx < 0 || mx >= m_objectMask.width())
                    continue;
                if (mline[mx] >= 128)
                {
                    const QRgb src = tline[x];
                    tline[x] = qRgba(qRed(src) * 0.65 + 60,
                                     qGreen(src) * 0.65 + 90,
                                     qBlue(src) * 0.65 + 200,
                                     qMin(255, qAlpha(src) + 60));
                }
            }
        }
        /* mask contour */
        p.setPen(QPen(QColor(30, 90, 220), 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(maskToPath(m_objectMask));
        p.end();
    }

    /* manual selection tint + marching border */
    if (m_doc && !m_doc->selectionPath().isEmpty())
    {
        const QPainterPath &sel = m_doc->selectionPath();
        QPainter p(&tile);
        p.setClipRect(tile.rect());
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        /* dark tint outside the selection */
        QPainterPath outside;
        outside.addRect(r);
        outside = outside.subtracted(sel.translated(-r.topLeft()));
        p.fillPath(outside, QColor(0, 0, 0, 60));
        QPen pen(QColor(255, 255, 255), 1, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(sel.translated(-r.topLeft()));
        p.end();
    }

    /* shape tool preview (rubber band) */
    if (!m_shapePreviewPath.isEmpty())
    {
        QPainter p(&tile);
        p.setClipRect(tile.rect());
        p.translate(-r.topLeft());
        QPen pen(m_fgColor, 1, Qt::SolidLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(m_shapePreviewPath);
        p.end();
    }
}

void PaintbrushCanvasItem::compositeDirtyTiles()
{
    if (m_dirtyTiles.isEmpty())
        return;

    const QSet<TileKey> dirty = m_dirtyTiles;
    m_dirtyTiles.clear();
    for (const TileKey &key : dirty)
    {
        QImage tile;
        compositeTile(key.x, key.y, tile);
        m_tiles.insert(key, tile);
    }
    /* only the tiles composited now need their GPU textures re-uploaded */
    m_changedTiles += dirty;
}

/* ---------------------------------------------------------- scene graph */

QSGNode *PaintbrushCanvasItem::updatePaintNode(QSGNode *oldNode,
                                               UpdatePaintNodeData *)
{
    if (!m_doc)
    {
        m_gridNode = nullptr; /* children are freed with the old root */
        delete oldNode;
        return nullptr;
    }

    /* The returned top node is a container that owns the document-transform
     * root (all document content in canvas coords) plus the viewport-space
     * live-harvest overlay node (item coords, on top of the video). */
    QSGNode *container = oldNode;
    QSGTransformNode *root = nullptr;
    if (container)
    {
        /* oldNode is the container; child 0 = document root, child 1 =
         * overlay node (when present). */
        for (QSGNode *child = container->firstChild(); child;
             child = child->nextSibling())
        {
            if (child->type() == QSGNode::TransformNodeType)
            {
                root = static_cast<QSGTransformNode *>(child);
                break;
            }
        }
    }
    if (!root)
    {
        if (!container)
            container = new QSGNode();
        root = new QSGTransformNode();
        container->appendChildNode(root);
    }

    QMatrix4x4 matrix;
    matrix.translate(m_panOffset.x(), m_panOffset.y());
    matrix.scale(m_zoom);
    root->setMatrix(matrix);

    QQuickWindow *win = window();

    /* (re)create textures only for tiles whose content changed this pass;
     * under a fast stroke that is 1-4 tiles per frame, never the whole doc */
    if (win && !m_changedTiles.isEmpty())
    {
        const auto changed = m_changedTiles;
        m_changedTiles.clear();
        for (const TileKey &key : changed)
        {
            if (!m_tiles.contains(key))
                continue;
            QSGTexture *&tex = m_textures[key];
            if (tex)
                delete tex;
            tex = win->createTextureFromImage(m_tiles.value(key));
            if (tex)
                tex->setFiltering(adaptiveFiltering());
        }
    }
    else if (win)
    {
        /* zoom-only changes: keep the textures, refresh the filtering
         * (nearest on integer zoom, linear otherwise) */
        const QSGTexture::Filtering filtering = adaptiveFiltering();
        const auto textureKeys = m_textures.keys();
        for (const TileKey &key : textureKeys)
        {
            QSGTexture *tex = m_textures.value(key);
            if (tex && tex->filtering() != filtering)
                tex->setFiltering(filtering);
        }
    }

    /* prune textures for tiles that no longer exist */
    const auto textureKeys = m_textures.keys();
    for (const TileKey &key : textureKeys)
    {
        if (!m_tiles.contains(key))
        {
            delete m_textures.take(key);
            if (QSGTransformNode *node = m_tileNodes.take(key))
                delete node;
        }
    }

    /* grid overlay: kept as a single geometry node under the same transform;
     * rebuilt whenever visibility/spacing/zoom changes (gridVisibleChanged
     * and zoomChanged both call update()) */
    if (m_gridVisible)
    {
        if (m_gridNode)
        {
            root->removeChildNode(m_gridNode);
            delete m_gridNode;
            m_gridNode = nullptr;
        }
        m_gridNode = buildGridNode();
        root->appendChildNode(m_gridNode);
    }
    else if (m_gridNode)
    {
        root->removeChildNode(m_gridNode);
        delete m_gridNode;
        m_gridNode = nullptr;
    }

    /* In HarvestVideo mode the document tiles are NOT rendered (the live
     * video anchored behind shows through); prune any existing tile nodes so
     * nothing opaque is drawn over the video. */
    if (m_videoHarvestMode)
    {
        const auto keys = m_tileNodes.keys();
        for (const TileKey &key : keys)
        {
            if (QSGTransformNode *node = m_tileNodes.take(key))
            {
                root->removeChildNode(node);
                delete node;
            }
        }
        /* fall through to the overlay node below */
    }
    else
    {
        /* ensure one transform node per tile, positioned in canvas coords */
        const auto tileKeys = m_tiles.keys();
        for (const TileKey &key : tileKeys)
        {
            QSGTransformNode *node = m_tileNodes.value(key);
            if (!node)
            {
                node = new QSGTransformNode();
                m_tileNodes.insert(key, node);
                root->appendChildNode(node);
                QMatrix4x4 tileMatrix;
                tileMatrix.translate(key.x * TILE, key.y * TILE);
                node->setMatrix(tileMatrix);
            }
            QSGSimpleTextureNode *texNode = nullptr;
            for (QSGNode *child = node->firstChild(); child; child = child->nextSibling())
            {
                if (child->type() == QSGNode::GeometryNodeType)
                {
                    texNode = static_cast<QSGSimpleTextureNode *>(child);
                    break;
                }
            }
            if (!texNode)
            {
                texNode = new QSGSimpleTextureNode();
                node->appendChildNode(texNode);
            }
            const QRect r = tileRect(key.x, key.y);
            QSGTexture *tex = m_textures.value(key);
            if (tex)
            {
                texNode->setTexture(tex);
                texNode->setRect(0, 0, r.width(), r.height());
            }
            else
            {
                texNode->setTexture(nullptr);
                texNode->setRect(0, 0, 0, 0);
            }
        }
    }

    /* ---- live harvest overlay node (viewport space, over the video) ------ */
    /* A dedicated node OUTSIDE the document-transform root so the mask stays
     * glued to the video regardless of document zoom/pan. */
    if (m_liveHarvestOverlayValid && !m_liveHarvestOverlay.isNull()
        && win)
    {
        if (m_liveOverlayNodeChanged || !m_liveOverlayNode)
        {
            /* release the previous texture before rebuilding */
            if (m_liveOverlayTexture)
            {
                delete m_liveOverlayTexture;
                m_liveOverlayTexture = nullptr;
            }
            if (m_liveOverlayNode)
            {
                container->removeChildNode(m_liveOverlayNode);
                delete m_liveOverlayNode;
                m_liveOverlayNode = nullptr;
            }
            m_liveOverlayNode = new QSGSimpleTextureNode();
            m_liveOverlayTexture = win->createTextureFromImage(m_liveHarvestOverlay);
            m_liveOverlayNode->setTexture(m_liveOverlayTexture);
            m_liveOverlayNode->setRect(0, 0, m_liveHarvestOverlay.width(),
                                       m_liveHarvestOverlay.height());
            m_liveOverlayNode->setFiltering(QSGTexture::Linear);
            container->appendChildNode(m_liveOverlayNode);
            m_liveOverlayNodeChanged = false;
        }
    }
    else if (m_liveOverlayNode)
    {
        container->removeChildNode(m_liveOverlayNode);
        delete m_liveOverlayNode;
        m_liveOverlayNode = nullptr;
        if (m_liveOverlayTexture)
        {
            delete m_liveOverlayTexture;
            m_liveOverlayTexture = nullptr;
        }
        m_liveOverlayNodeChanged = false;
    }

    return container;
}

void PaintbrushCanvasItem::geometryChange(const QRectF &newGeometry,
                                          const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
    {
        /* The live harvest overlay is item-sized AND its letterbox transform
         * depends on the viewport size. Rebuild it from the authoritative
         * native mask against the new size (P0#3); on failure fall back to
         * hiding the overlay rather than keeping a stale one glued to the
         * old geometry. */
        if (m_liveHarvestOverlayValid && !m_liveHarvestOverlay.isNull()
            && !m_objectMask.isNull())
        {
            const int oldW = qMax(1, int(oldGeometry.width()));
            const int oldH = qMax(1, int(oldGeometry.height()));
            const int newW = qMax(1, int(newGeometry.width()));
            const int newH = qMax(1, int(newGeometry.height()));
            /* skip rebuild if the item was momentarily zero-sized */
            if (newW > 0 && newH > 0)
            {
                rebuildLiveHarvestOverlayForCurrentSize();
                return; /* rebuilt and re-rendered; no extra generic update */
            }
            Q_UNUSED(oldW); Q_UNUSED(oldH);
        }
        m_liveHarvestOverlay = QImage();
        m_liveHarvestOverlayValid = false;
        m_liveOverlayNodeChanged = true;
        update();
    }
}

void PaintbrushCanvasItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange && value.window)
    {
        markAllTilesDirty();
        updateCursor();   /* window is known now: regenerate at the real DPR */
    }
}

/* ------------------------------------------------------------- painting */

void PaintbrushCanvasItem::captureBeforeRegion(const QRect &localRect)
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull())
        return;

    /* rects are in LAYER-LOCAL coordinates */
    const QRect clipped = localRect.intersected(layer->raster.rect());
    if (clipped.isEmpty())
        return;

    /* skip sub-rects already covered by earlier captures */
    for (const auto &pair : m_strokeBefore)
    {
        if (pair.first.contains(clipped))
            return;
    }

    m_strokeBefore.append({clipped, layer->raster.copy(clipped)});
    m_strokeUnion = m_strokeUnion.isNull() ? clipped : m_strokeUnion.united(clipped);
}

/* Map a canvas position into the current layer's LOCAL raster space so
 * strokes land correctly on transformed image/text layers. */
QPointF PaintbrushCanvasItem::localFromCanvas(const QPointF &canvasPos) const
{
    if (!m_doc)
        return canvasPos;
    const CanvasLayer *layer = m_doc->currentLayer();
    if (!layer)
        return canvasPos;
    return layer->mapCanvasToLocal(canvasPos);
}

/* Map a layer-local rect onto canvas coordinates (conservative bounding box
 * for rotated layers) for dirty-tile marking. */
QRect PaintbrushCanvasItem::mappedRect(const QRect &localRect) const
{
    if (!m_doc)
        return localRect;
    const CanvasLayer *layer = m_doc->currentLayer();
    if (!layer)
        return localRect;
    const QTransform tf = layer->transform();
    if (tf.type() <= QTransform::TxTranslate)
        return tf.mapRect(localRect);
    return tf.mapRect(QRectF(localRect)).toAlignedRect().adjusted(-2, -2, 2, 2);
}

/* Grow the per-stroke scratch tile to cover `localRect`, seeding the
 * untouched portion with the real layer content so dabs (SourceOver paint
 * and Clear erase) produce exactly what painting the layer directly would. */
void PaintbrushCanvasItem::ensureScratch(const QRect &localRect)
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull())
        return;

    const QRect need = localRect.intersected(layer->raster.rect());
    if (need.isEmpty())
        return;

    if (m_scratchTile.isNull())
    {
        m_scratchTile = QImage(need.size(), QImage::Format_ARGB32);
        m_scratchTile.fill(Qt::transparent);
        m_scratchOrigin = need.topLeft();
        m_scratchSeeded = QRect();
    }
    else
    {
        const QRect cur(m_scratchOrigin, m_scratchTile.size());
        if (!cur.contains(need))
        {
            const QRect u = cur.united(need);
            QImage grown(u.size(), QImage::Format_ARGB32);
            grown.fill(Qt::transparent);
            QPainter p(&grown);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(m_scratchOrigin - u.topLeft(), m_scratchTile);
            p.end();
            m_scratchTile = std::move(grown);
            m_scratchOrigin = u.topLeft();
        }
    }

    /* seed only the areas that have not been seeded yet */
    const QRegion unseeded = m_scratchSeeded.isEmpty()
                                 ? QRegion(need)
                                 : QRegion(need) - QRegion(m_scratchSeeded);
    if (!unseeded.isEmpty())
    {
        QPainter p(&m_scratchTile);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect &r : unseeded)
        {
            const QRect rLocal = r.intersected(need);
            if (rLocal.isEmpty())
                continue;
            p.drawImage(rLocal.topLeft() - m_scratchOrigin, layer->raster, rLocal);
        }
        p.end();
        m_scratchSeeded = m_scratchSeeded.united(need);
    }
}

/* Stamp one dab into the per-stroke scratch tile (layer-local coords). */
void PaintbrushCanvasItem::stampDabAt(const QPointF &localCenter, qreal radius,
                                      qreal hardness, qreal opacity, bool erase)
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    const QRectF dabRect(localCenter.x() - radius, localCenter.y() - radius,
                         radius * 2, radius * 2);
    const QRect affected = dabRect.adjusted(-2, -2, 2, 2).toAlignedRect()
                               .intersected(layer->raster.rect());
    if (affected.isEmpty())
        return;

    captureBeforeRegion(affected);
    ensureScratch(affected);

    paintbrush::stampDab(m_scratchTile,
                         localCenter - QPointF(m_scratchOrigin),
                         radius, hardness, opacity,
                         erase ? QColor() : m_fgColor,
                         erase ? QPainter::CompositionMode_Clear
                               : QPainter::CompositionMode_SourceOver);
    m_strokeHadPaint = true;
    markTilesDirty(mappedRect(affected));
}

/* Walk a stroke segment from `fromLocal` to `toLocal`, stamping dabs at the
 * velocity-scaled spacing so fast strokes never leave gaps. */
void PaintbrushCanvasItem::strokeSegmentDabs(const QPointF &fromLocal,
                                             const QPointF &toLocal)
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    const bool erase = (m_tool == TOOL_ERASER);
    const bool pencil = (m_tool == TOOL_PENCIL);
    qreal radius = pencil ? 1.0
                  : (erase ? qMax(2.0, m_brushSize) : m_brushSize / 2.0);
    const qreal pf = pressureFactor();
    if (pf != 1.0 && !erase)
        radius *= pf;

    const qreal hardness = pencil ? 1.0 : (erase ? 1.0 : 0.85);
    const qreal opacity = pencil ? 1.0
                         : (!erase && m_pressureToOpacity && pf != 1.0 ? pf
                                                                       : 1.0);

    /* dab spacing: ~20% of the diameter, stretched slightly on fast strokes */
    const qreal spacing =
        qMax<qreal>(0.5, radius * 2.0 * 0.20) *
        (1.0 + qMin<qreal>(1.0, m_strokeSpeed * 0.05));

    const QLineF seg(fromLocal, toLocal);
    const qreal len = seg.length();
    if (len <= 0.0)
    {
        /* press point: one dab */
        stampDabAt(fromLocal, radius, hardness, opacity, erase);
        return;
    }

    const QPointF dir = seg.unitVector().p2() - seg.unitVector().p1();
    qreal travel = m_strokeRemainder;
    while (travel < len)
    {
        stampDabAt(fromLocal + dir * travel, radius, hardness, opacity, erase);
        travel += spacing;
    }
    m_strokeRemainder = travel - len;
    m_strokeLastDab = toLocal;
}

/* Commit the per-stroke scratch tile onto the layer as one region delta:
 * replace the touched region, apply selection/alpha-lock masking, push the
 * typed StrokeCommand and notify. */
void PaintbrushCanvasItem::commitScratch(const QString &desc)
{
    if (!m_doc || m_scratchTile.isNull())
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull())
        return;

    {
        QPainter p(&layer->raster);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(m_scratchOrigin, m_scratchTile);
        p.end();
    }

    maskStrokeToSelectionAndAlphaLock();

    /* ---- powder: atomic undo (P0.11) ----------------------------------
     * A powder operation is committed as ONE CompositeCommand whose children
     * (in redo/apply order) are:
     *   1. the auto-created dedicated powder layer (InsertLayerCommand, only on
     *      the first material edit of a non-powder layer),
     *   2. the raster stroke (StrokeCommand),
     *   3. the physical material state (SetMaterialStateCommand).
     * One Ctrl+Z reverts them in reverse (material -> raster -> remove layer),
     * restoring the previous layer selection in the same step. */
    const bool isPowder =
        m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR;
    bool pushed = false;
    if (isPowder && m_undoStack)
    {
        auto composite = std::make_unique<CompositeCommand>();
        composite->desc = desc;

        if (m_powderInsertCmd)
            composite->children.push_back(std::move(m_powderInsertCmd));

        if (std::unique_ptr<StrokeCommand> sc = makeStrokeCommand(desc))
            composite->children.push_back(std::move(sc));

        if (m_powderMaterialCaptured)
        {
            m_powderMaterialCaptured = false;
            const bool materialChanged =
                m_powderMaterialCmd.before.preset != m_powderMaterialCmd.after.preset
                || m_powderMaterialCmd.before.state != m_powderMaterialCmd.after.state;
            if (materialChanged)
            {
                m_powderMaterialCmd.desc = desc;
                m_powderMaterialCmd.layerIndex = m_doc->currentLayerIndex();
                composite->children.push_back(
                    std::make_unique<SetMaterialStateCommand>(m_powderMaterialCmd));
            }
        }

        if (!composite->children.empty())
        {
            m_undoStack->pushCommand(std::move(composite));
            m_doc->setDirty(true);
            pushed = true;
        }
    }

    if (!pushed)
    {
        /* generic (non-powder) commit: stroke raster, and pend any material
         * state as a separate typed command (non-powder paths don't reach
         * material state, but keep the old separate push as a safe fallback) */
        pushStrokeEntry(desc);
        if (m_powderMaterialCaptured && m_undoStack)
        {
            m_powderMaterialCaptured = false;
            const bool materialChanged =
                m_powderMaterialCmd.before.preset != m_powderMaterialCmd.after.preset
                || m_powderMaterialCmd.before.state != m_powderMaterialCmd.after.state;
            if (materialChanged)
            {
                m_powderMaterialCmd.desc = desc;
                m_powderMaterialCmd.layerIndex = m_doc->currentLayerIndex();
                m_undoStack->pushMaterialState(m_powderMaterialCmd);
            }
        }
    }

    const QRect localUnion = m_strokeUnion.intersected(layer->raster.rect());
    if (!localUnion.isEmpty())
    {
        /* refresh the display now: the scratch was just written into the
         * layer raster, and nothing else triggers a repaint on release */
        markTilesDirty(mappedRect(localUnion));
        emit strokeCommitted(mappedRect(localUnion));
        emit canvasModified();
    }
    m_scratchTile = QImage();
    m_scratchSeeded = QRect();
}

void PaintbrushCanvasItem::strokeSegment(const QPointF &from, const QPointF &to)
{
    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
        strokeSegmentDabs(localFromCanvas(from), localFromCanvas(to));
        break;
    case TOOL_SPRAY:
        /* spray is emitted continuously by the physics timer; a single
         * strokeSegment call should never happen (beginStroke routes to
         * emitSprayPuff) but keep a safe fallback */
        if (from == to)
            emitSprayPuff();
        break;
    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
        artStrokeSegment(localFromCanvas(from), localFromCanvas(to));
        break;
    default:
        break;
    }
}

void PaintbrushCanvasItem::beginStroke(const QPointF &canvasPos, bool primary)
{
    if (!m_doc)
        return;

    m_strokePrimary = primary;
    m_strokeHadPaint = false;
    m_strokeUnion = QRect();
    m_strokeBefore.clear();
    m_prevPreviewRect = QRect();

    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
        m_strokeActive = true;
        m_lastCanvasPos = canvasPos;
        /* arm the dab pipeline: fresh scratch tile, empty stabilizer */
        m_scratchTile = QImage();
        m_scratchSeeded = QRect();
        m_strokeRemainder = 0.0;
        m_strokeSpeed = 0.0;
        m_stabilizer.clear();
        m_stabilizer.append(localFromCanvas(canvasPos));
        m_strokeSmooth = m_stabilizer.first();
        m_strokeLastDab = m_strokeSmooth;
        /* the initial dab captures its own before-region */
        strokeSegmentDabs(m_strokeSmooth, m_strokeSmooth);
        break;

    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
        m_lastCanvasPos = canvasPos;
        m_strokeRemainder = 0.0;
        m_strokeSpeed = 0.0;
        beginArtStroke(localFromCanvas(canvasPos));
        break;

    case TOOL_SPRAY:
    {
        m_strokeActive = true;
        m_lastCanvasPos = canvasPos;
        /* the can starts exactly where the hand pressed */
        m_sprayNozzle = canvasPos;
        m_sprayTarget = canvasPos;
        m_sprayVel = QPointF();
        m_sprayWobblePhase = 0.0;
        m_sprayWobbleAmp = 0.0;
        /* puffs capture their own before-regions and paint into the shared
         * scratch tile (same commit path as the brush) */
        m_scratchTile = QImage();
        m_scratchSeeded = QRect();
        emitSprayPuff();
        /* keep emitting while the button is held, even without motion */
        m_sprayTimer.start();
        break;
    }

    case TOOL_FILL:
        if (primary)
            floodFillAt(canvasPos);
        break;

    case TOOL_EYEDROPPER:
        if (primary)
            sampleColorAt(canvasPos);
        break;

    case TOOL_LINE:
    case TOOL_RECTANGLE:
    case TOOL_ROUNDED_RECT:
    case TOOL_ELLIPSE:
        m_strokeActive = true;
        m_lastCanvasPos = canvasPos;
        m_dragStartCanvas = canvasPos;
        m_shapePreviewPath = buildShapePath(canvasPos, canvasPos);
        m_prevPreviewRect = m_shapePreviewPath.boundingRect().toAlignedRect();
        markAllTilesDirty();
        break;

    case TOOL_RECT_SELECT:
    case TOOL_FREEFORM_SELECT:
        m_strokeActive = true;
        m_dragStartCanvas = canvasPos;
        m_dragLastCanvas = canvasPos;
        m_dragging = true;
        if (m_tool == TOOL_FREEFORM_SELECT)
        {
            m_pendingPath = QPainterPath();
            m_pendingPath.moveTo(canvasPos);
        }
        /* dragging inside an existing selection moves it */
        if (hasSelection() && m_doc->selectionPath().contains(canvasPos))
            m_movingSelection = true;
        else if (hasSelection())
            m_doc->clearSelection();
        break;

    case TOOL_OBJECT_SELECT:
        if (primary)
        {
            if (!m_objectMask.isNull())
            {
                /* freehand add onto the existing mask: arm the stroke but
                 * wait for real movement so a plain click stays a SAM
                 * point refinement */
                m_strokeActive = true;
                m_maskPaintActive = true;
                m_maskPaintSubtract = false;
                m_maskPaintDirty = false;
                m_maskPaintUnion = QRect();
                m_maskPaintLastPos = canvasPos;
                emit maskRefineStarted(m_objectMask);
            }
            else
                emit objectSelectClick(canvasPos, true, itemFromCanvas(canvasPos));
        }
        break;

    case TOOL_ASK:
        /* Ask-anything: a single click asks the VLM about the point (the
         * controller picks the context: object / selection / whole frame). */
        if (primary)
            emit askClick(canvasPos, itemFromCanvas(canvasPos));
        break;

    case TOOL_PAN_ZOOM:
        m_panning = true;
        m_panLastItem = eventLastItemPos();
        break;

    default:
        break;
    }
}

void PaintbrushCanvasItem::continueStroke(const QPointF &canvasPos)
{
    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
        if (m_strokeActive)
        {
            /* normalize (canvas -> local) + stabilize (short moving
             * average) + velocity estimate, then dab along the smoothed
             * segment. Sparse events (coalesced moves under load) must
             * follow the cursor directly, or the stroke trails behind. */
            const QPointF local = localFromCanvas(canvasPos);
            const qreal dist = QLineF(m_strokeSmooth, local).length();
            m_strokeSpeed = m_strokeSpeed * 0.7 + dist * 0.3;
            if (dist > 8.0)
            {
                m_strokeSmooth = local;
                m_stabilizer.clear();
                m_stabilizer.append(local);
            }
            else
            {
                m_stabilizer.append(local);
                if (m_stabilizer.size() > 3)
                    m_stabilizer.removeFirst();
                QPointF avg;
                for (const QPointF &s : m_stabilizer)
                    avg += s;
                avg /= m_stabilizer.size();
                m_strokeSmooth = avg;
            }
            strokeSegmentDabs(m_strokeLastDab, m_strokeSmooth);
            m_lastCanvasPos = canvasPos;
        }
        break;

    case TOOL_SPRAY:
        /* the timer pulls the nozzle toward the hand; here we only feed it
         * the latest hand position and velocity so the physics stays smooth */
        if (m_strokeActive)
        {
            m_sprayVel = m_sprayVel * 0.6
                         + (canvasPos - m_sprayTarget) * 0.4;
            m_sprayTarget = canvasPos;
            m_lastCanvasPos = canvasPos;
        }
        break;

    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
        if (m_strokeActive)
        {
            /* same normalize + short-window stabilize as the brush so the
             * art tools inherit the proven stroke feel */
            const QPointF local = localFromCanvas(canvasPos);
            const qreal dist = QLineF(m_strokeSmooth, local).length();
            if (dist > 8.0)
            {
                m_strokeSmooth = local;
                m_stabilizer.clear();
                m_stabilizer.append(local);
            }
            else
            {
                m_stabilizer.append(local);
                if (m_stabilizer.size() > 3)
                    m_stabilizer.removeFirst();
                QPointF avg;
                for (const QPointF &s : m_stabilizer)
                    avg += s;
                avg /= m_stabilizer.size();
                m_strokeSmooth = avg;
            }
            artStrokeSegment(m_strokeLastDab, m_strokeSmooth);
            m_lastCanvasPos = canvasPos;
            /* powder/smear: moving the pointer cancels/defers the long-press
             * breath; a real (non-click) drag is what a breath should wait for.
             * `m_powderHoldEver` is set ONLY once the pointer has actually
             * travelled (>2 px), so sub-pixel pressure jitter on a pure
             * press-and-hold doesn't flip the gesture out of hydrate mode. */
            if (m_tool == TOOL_GLITTER_POWDER || m_tool == TOOL_SMEAR)
            {
                if (QLineF(m_powderHoldPos, m_strokeSmooth).length() > 2.0)
                {
                    m_powderHoldEver = true;
                    m_powderBlowSince = 0.0f;
                    m_powderHoldPos = m_strokeSmooth;
                    m_powderBlowing = false;
                }
            }
        }
        break;

    case TOOL_LINE:
    case TOOL_RECTANGLE:
    case TOOL_ROUNDED_RECT:
    case TOOL_ELLIPSE:
        if (m_strokeActive)
        {
            const QPainterPath newPath = buildShapePath(m_dragStartCanvas, canvasPos);
            const QRect newRect = newPath.boundingRect().adjusted(-8, -8, 8, 8)
                                    .toAlignedRect();
            m_shapePreviewPath = newPath;
            markTilesDirty(m_prevPreviewRect.adjusted(-8, -8, 8, 8).united(newRect));
            m_prevPreviewRect = newRect;
        }
        break;

    case TOOL_RECT_SELECT:
    case TOOL_FREEFORM_SELECT:
        if (m_dragging)
        {
            if (m_movingSelection)
            {
                moveSelection(canvasPos - m_dragLastCanvas);
                m_dragLastCanvas = canvasPos;
            }
            else
            {
                updateSelectionFromDrag(canvasPos);
                m_dragLastCanvas = canvasPos;
            }
        }
        break;

    case TOOL_OBJECT_SELECT:
        if (m_maskPaintActive)
        {
            if (!m_maskPaintDirty)
            {
                /* first real movement: paint from the press point */
                if ((canvasPos - m_maskPaintLastPos).manhattanLength() < 3.0)
                    break; /* still a click, keep waiting */
                paintObjectMaskSegment(m_maskPaintLastPos, canvasPos,
                                       m_maskPaintSubtract);
            }
            else
            {
                paintObjectMaskSegment(m_maskPaintLastPos, canvasPos,
                                       m_maskPaintSubtract);
            }
            m_maskPaintLastPos = canvasPos;
        }
        break;

    case TOOL_PAN_ZOOM:
        if (m_panning)
        {
            const QPointF itemPos = eventLastItemPos();
            setPanOffset(m_panOffset + (itemPos - m_panLastItem));
            m_panLastItem = itemPos;
        }
        break;

    default:
        break;
    }
    updateStatus(canvasPos);
}

void PaintbrushCanvasItem::endStroke(const QPointF &canvasPos, bool primary)
{
    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
        if (m_strokeActive)
        {
            /* finish the last segment and commit the scratch tile as one
             * region delta */
            const QPointF local = localFromCanvas(canvasPos);
            m_stabilizer.append(local);
            if (m_stabilizer.size() > 3)
                m_stabilizer.removeFirst();
            QPointF avg;
            for (const QPointF &s : m_stabilizer)
                avg += s;
            avg /= m_stabilizer.size();
            m_strokeSmooth = avg;
            strokeSegmentDabs(m_strokeLastDab, m_strokeSmooth);
            m_strokeActive = false;
            if (m_strokeHadPaint)
                commitScratch(QStringLiteral("Draw"));
            m_strokeBefore.clear();
            m_strokeUnion = QRect();
            m_scratchTile = QImage();
            m_scratchSeeded = QRect();
        }
        break;

    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
        if (m_strokeActive)
            endArtStroke(localFromCanvas(canvasPos));
        break;

    case TOOL_SPRAY:
        if (m_strokeActive)
        {
            m_sprayTimer.stop();
            m_sprayTarget = canvasPos;
            emitSprayPuff(); /* final puff at the release point */
            m_strokeActive = false;
            if (m_strokeHadPaint)
                commitScratch(QStringLiteral("Spray"));
            m_strokeBefore.clear();
            m_strokeUnion = QRect();
            m_scratchTile = QImage();
            m_scratchSeeded = QRect();
        }
        break;

    case TOOL_LINE:
    case TOOL_RECTANGLE:
    case TOOL_ROUNDED_RECT:
    case TOOL_ELLIPSE:
        if (m_strokeActive)
        {
            const QPainterPath path = buildShapePath(m_dragStartCanvas, canvasPos);
            if (!path.isEmpty())
                commitShape(path, primary);
            m_shapePreviewPath = QPainterPath();
            m_strokeActive = false;
            markAllTilesDirty();
        }
        break;

    case TOOL_RECT_SELECT:
    case TOOL_FREEFORM_SELECT:
        if (m_dragging)
        {
            m_dragging = false;
            m_strokeActive = false;
            if (m_movingSelection)
            {
                m_movingSelection = false;
                emit canvasModified();
            }
            else
            {
                finishSelection();
            }
            markAllTilesDirty();
        }
        break;

    case TOOL_OBJECT_SELECT:
        if (m_maskPaintActive)
        {
            const bool painted = m_maskPaintDirty;
            if (painted)
            {
                paintObjectMaskSegment(m_maskPaintLastPos, canvasPos,
                                       m_maskPaintSubtract);
                m_maskPaintLastPos = canvasPos;
            }
            endObjectMaskStroke();
            if (!painted)
            {
                /* click without movement: SAM point refinement (positive =
                 * primary, negative = alt/right, which armed the stroke) */
                m_strokeActive = false;
                emit objectSelectClick(canvasPos, !m_maskPaintSubtract,
                                       itemFromCanvas(canvasPos));
            }
            updateStatus(canvasPos);
        }
        break;

    case TOOL_PAN_ZOOM:
        m_panning = false;
        break;

    default:
        break;
    }
    updateStatus(canvasPos);
}

/* ----------------------------------------------------------- spray physics */

void PaintbrushCanvasItem::resetSprayState()
{
    m_sprayTimer.stop();
    m_sprayNozzle = QPointF();
    m_sprayTarget = QPointF();
    m_sprayVel = QPointF();
    m_sprayWobblePhase = 0.0;
    m_sprayWobbleAmp = 0.0;
}

/* One emission of the spray can. The can tip lags behind the hand (the
 * faster the hand moves, the more the nozzle trails — exactly like a real
 * can that can't keep up with a flick), the nozzle wobbles with a smoothed
 * hand-shake, and the cone widens with velocity. The result is the loose,
 * "under the bridge in Prague" curve — never a rigid straight line.
 *
 * Droplets are stamped through the same dab generator as the brush and
 * land in the per-stroke scratch tile, so spray participates in the same
 * region-delta undo and selection/alpha-lock masking as every other tool.
 * (The old code captured the before-region AFTER painting, which made
 * spray undo and selection masking no-ops — fixed here.) */
void PaintbrushCanvasItem::emitSprayPuff()
{
    if (!m_doc || m_tool != TOOL_SPRAY)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    QRandomGenerator *rng = QRandomGenerator::global();

    /* --- nozzle lag: pull the can tip toward the hand --------------------- */
    const QPointF toTarget = m_sprayTarget - m_sprayNozzle;
    const qreal handSpeed = QLineF(QPointF(), m_sprayVel).length();

    /* the faster the hand, the lazier the can: more lag = more curve */
    const qreal lag = qBound<qreal>(0.10, 0.55 - handSpeed * 0.02, 0.9);
    m_sprayNozzle += toTarget * lag;

    /* movement direction (for the cone slope) */
    QPointF dir(1, 0);
    if (handSpeed > 0.5)
        dir = m_sprayVel / handSpeed;
    const QPointF perp(-dir.y(), dir.x());

    /* --- hand wobble: smoothed sinusoidal shake --------------------------- */
    m_sprayWobblePhase += 0.35 + handSpeed * 0.05;
    const qreal targetAmp = qMin<qreal>(1.8, 0.3 + handSpeed * 0.04 + m_sprayFlow * 0.2);
    m_sprayWobbleAmp += (targetAmp - m_sprayWobbleAmp) * 0.15;
    const QPointF tip = m_sprayNozzle + perp * (qSin(m_sprayWobblePhase) * m_sprayWobbleAmp);

    /* --- cone geometry ------------------------------------------------------ */
    const qreal base = qMax(2.0, m_brushSize / 2.0);
    const qreal coreRadius = base * (0.7 + m_sprayFlow * 0.12);
    const qreal mistRadius = base * (1.3 + handSpeed * 0.10 + m_sprayFlow * 0.35);

    /* gaussian-ish falloff (sum of two uniforms) */
    auto gauss = [rng]() {
        return (rng->generateDouble() + rng->generateDouble()) * 0.5;
    };

    const int coreDrops = qMax(3, int(9 * m_sprayFlow));
    const int mistDrops = qMax(8, int(16 * m_sprayFlow + handSpeed * 0.4));

    /* build the drop list first so the affected region (and its undo
     * before-content) is captured exactly once, before any paint lands */
    struct Drop { QPointF pos; qreal radius; QColor color; };
    QVector<Drop> drops;
    drops.reserve(coreDrops + mistDrops + 1);

    auto addDrops = [&](int n, qreal positionRadius, qreal dropRadius,
                        int alphaLo, int alphaHi) {
        for (int i = 0; i < n; ++i)
        {
            const qreal ang = rng->generateDouble() * 2 * M_PI;
            const qreal r = gauss() * positionRadius;
            QColor c = m_fgColor;
            c.setAlpha(alphaLo + int(rng->generateDouble() * (alphaHi - alphaLo)));
            drops.append({tip + QPointF(qCos(ang), qSin(ang)) * r,
                          dropRadius, c});
        }
    };

    /* dense core: small, fairly opaque dots near the nozzle */
    addDrops(coreDrops, coreRadius, 0.4 + 1.0, 170, 255);
    /* translucent mist: feathered, wider, lighter */
    addDrops(mistDrops, mistRadius, 0.2 + 0.5, 28, 100);

    /* occasional heavy drip that runs down, like over-saturated paint */
    if (m_sprayFlow > 1.1 && rng->generateDouble() < 0.18)
    {
        QColor c = m_fgColor;
        c.setAlpha(120);
        drops.append({tip + QPointF((rng->generateDouble() - 0.5) * coreRadius,
                                    coreRadius * (0.5 + rng->generateDouble())),
                      0.7 + rng->generateDouble() * 1.3, c});
    }

    /* droplet positions are canvas-space (the physics runs there); map them
     * into the layer's local raster space for stamping and region tracking */
    QRect localAffected;
    for (const Drop &d : drops)
    {
        const QPointF localPos = layer->mapCanvasToLocal(d.pos);
        const QRectF rc(localPos.x() - d.radius - 1, localPos.y() - d.radius - 1,
                        d.radius * 2 + 2, d.radius * 2 + 2);
        localAffected = localAffected.isNull() ? rc.toAlignedRect()
                                               : localAffected.united(rc.toAlignedRect());
    }
    const QRect clipped = localAffected.intersected(layer->raster.rect());
    if (clipped.isEmpty())
        return;

    captureBeforeRegion(clipped);
    ensureScratch(clipped);

    const QPointF origin(m_scratchOrigin);
    for (const Drop &d : drops)
    {
        const QPointF localPos = layer->mapCanvasToLocal(d.pos);
        paintbrush::stampDab(m_scratchTile, localPos - origin,
                             qMax<qreal>(0.4, d.radius), 1.0,
                             d.color.alphaF(), d.color,
                             QPainter::CompositionMode_SourceOver);
    }

    m_strokeHadPaint = true;
    m_strokeUnion = m_strokeUnion.isNull() ? clipped : m_strokeUnion.united(clipped);
    markTilesDirty(mappedRect(clipped));
}

/* Restore stroke pixels that fall outside the active selection or on fully
 * transparent pixels of an alpha-locked layer. */
void PaintbrushCanvasItem::maskStrokeToSelectionAndAlphaLock()
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || m_strokeBefore.isEmpty())
        return;

    const bool hasSel = hasSelection();
    const QPainterPath &sel = m_doc->selectionPath();
    const bool alphaLock = layer->alphaLocked;
    const QTransform tf = layer->transform(); /* local -> canvas */

    if (!hasSel && !alphaLock)
        return;

    for (const auto &pair : m_strokeBefore)
    {
        const QRect &rect = pair.first;
        const QImage &before = pair.second;
        QImage &raster = layer->raster;
        const int w = qMin(rect.width(), raster.width() - rect.left());
        const int h = qMin(rect.height(), raster.height() - rect.top());
        for (int y = 0; y < h; ++y)
        {
            const QRgb *src = reinterpret_cast<const QRgb *>(before.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb *>(raster.scanLine(rect.top() + y) + rect.left() * 4);
            for (int x = 0; x < w; ++x)
            {
                const bool inside = !hasSel ||
                                    sel.contains(tf.map(QPointF(rect.left() + x,
                                                                rect.top() + y)));
                const bool lockedPixel = alphaLock && qAlpha(src[x]) == 0;
                if (!inside || lockedPixel)
                    dst[x] = src[x];
            }
        }
    }
}

void PaintbrushCanvasItem::pushStrokeEntry(const QString &desc)
{
    if (!m_undoStack || !m_doc || m_strokeBefore.isEmpty())
        return;

    std::unique_ptr<StrokeCommand> cmd = makeStrokeCommand(desc);
    if (cmd)
    {
        m_undoStack->pushCommand(std::move(cmd));
        m_doc->setDirty(true);
    }
}

/* Build a region-delta StrokeCommand for the current stroke (before/after
 * tiles only for the touched area). Used directly by the generic paint push
 * AND as a child of the powder CompositeCommand so undo restores the raster
 * atomically with the material state + auto-created layer (P0.11). Returns
 * nullptr when there is no stroke content to record. */
std::unique_ptr<StrokeCommand> PaintbrushCanvasItem::makeStrokeCommand(const QString &desc)
{
    if (!m_doc || m_strokeBefore.isEmpty())
        return nullptr;

    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull())
        return nullptr;

    StrokeCommand cmd;
    cmd.data.layerIndex = m_doc->currentLayerIndex();
    cmd.data.layerId = layer->id;
    cmd.data.unionRect = m_strokeUnion.intersected(layer->raster.rect());
    cmd.data.beforeRects = m_strokeBefore;
    cmd.data.after = layer->raster.copy(cmd.data.unionRect);
    cmd.data.description = desc;
    return std::make_unique<StrokeCommand>(std::move(cmd));
}

QPainterPath PaintbrushCanvasItem::buildShapePath(const QPointF &a,
                                                  const QPointF &b) const
{
    QPainterPath path;
    const QRectF rect = QRectF(a, b).normalized();
    switch (m_tool)
    {
    case TOOL_LINE:
        path.moveTo(a);
        path.lineTo(b);
        break;
    case TOOL_RECTANGLE:
        path.addRect(rect);
        break;
    case TOOL_ROUNDED_RECT:
        path.addRoundedRect(rect, qMin(rect.width(), rect.height()) * 0.15,
                            qMin(rect.width(), rect.height()) * 0.15);
        break;
    case TOOL_ELLIPSE:
        path.addEllipse(rect);
        break;
    default:
        break;
    }
    return path;
}

void PaintbrushCanvasItem::commitShape(const QPainterPath &path, bool filled)
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    /* shapes paint in LAYER-LOCAL space so they land correctly on
     * transformed image/text layers too */
    const QPainterPath localPath = layer->transform().inverted().map(path);
    const QRect affected = localPath.boundingRect().adjusted(-4, -4, 4, 4)
                               .toAlignedRect().intersected(layer->raster.rect());
    if (affected.isEmpty())
        return;

    m_strokeBefore.clear();
    m_strokeUnion = affected;
    captureBeforeRegion(affected);

    QPainter p(&layer->raster);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (filled && m_tool != TOOL_LINE)
    {
        p.setPen(QPen(m_fgColor, 1.0));
        p.setBrush(m_bgColor);
    }
    else
    {
        p.setPen(QPen(m_fgColor, 1.0));
        p.setBrush(Qt::NoBrush);
    }
    p.drawPath(localPath);
    p.end();

    m_strokeHadPaint = true;
    markTilesDirty(mappedRect(affected));
    maskStrokeToSelectionAndAlphaLock();
    pushStrokeEntry(QStringLiteral("Shape"));
    emit strokeCommitted(mappedRect(affected));
    emit canvasModified();
}

void PaintbrushCanvasItem::floodFillAt(const QPointF &canvasPos)
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    /* run on the layer at LOCAL dims; the affected region is mapped through
     * the layer transform for dirty-tile tracking */
    const QPoint start = layer->mapCanvasToLocal(canvasPos).toPoint();
    if (!layer->raster.rect().contains(start))
        return;

    QImage &img = layer->raster;
    const QRgb target = img.pixel(start);
    const QRgb fill = m_fgColor.rgba();
    if (target == fill)
        return;

    const QPainterPath *sel = hasSelection() ? &m_doc->selectionPath() : nullptr;

    /* bounded scanline/span fill: no full-canvas copy, no w*h visited
     * buffer — visited spans are tracked per row as merged intervals */
    const QRegion region = scanlineFill(img, start, target, fill,
                                        int(m_fillTolerance), sel,
                                        qint64(img.width()) * img.height());
    if (region.isEmpty())
        return;

    const QRect affected = region.boundingRect().intersected(img.rect());
    if (affected.isEmpty())
        return;

    m_strokeBefore.clear();
    m_strokeUnion = affected;
    captureBeforeRegion(affected);

    {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect &r : region)
            p.fillRect(r, m_fgColor);
        p.end();
    }

    m_strokeHadPaint = true;
    markTilesDirty(mappedRect(affected));
    pushStrokeEntry(QStringLiteral("Fill"));
    emit strokeCommitted(mappedRect(affected));
    emit canvasModified();
}

void PaintbrushCanvasItem::sampleColorAt(const QPointF &canvasPos)
{
    if (!m_doc)
        return;
    /* the eyedropper reads the SAME canonical render the screen and export
     * use, so it samples exactly what the user sees */
    const QImage flat = DocumentRenderer::render(*m_doc);
    const QPoint pt = canvasPos.toPoint();
    if (!flat.rect().contains(pt))
        return;
    const QColor color = flat.pixelColor(pt);
    setFgColor(color);
    emit colorPicked(color);
}

void PaintbrushCanvasItem::applyText(const QString &text, const QPointF &at,
                                     const QFont &font, const QColor &color)
{
    if (!m_doc || text.trimmed().isEmpty())
        return;

    QFont f = font;
    if (f.pixelSize() <= 0 && f.pointSize() <= 0)
        f.setPixelSize(32);

    QFontMetrics fm(f);
    const QRect textRect = fm.boundingRect(QRect(0, 0, 1, 1), Qt::AlignLeft | Qt::AlignTop, text);
    const QSize size = textRect.size().grownBy(QMargins(4, 4, 4, 4));

    /* create a text layer (kept editable in the project file) */
    CanvasLayer layer = CanvasLayer::createText(text, at, size,
                                                QStringLiteral("Text"));
    layer.raster.fill(Qt::transparent);
    {
        QPainter p(&layer.raster);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setPen(color);
        p.setFont(f);
        p.drawText(layer.raster.rect(), Qt::AlignLeft | Qt::AlignTop, text);
        p.end();
    }

    if (!m_undoStack)
        return;
    /* typed insert command: the layer asset is immutable once pushed */
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Add text");
    QList<CanvasLayer> layers = m_doc->layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_doc->currentLayerIndex();
    m_doc->setLayers(layers);
    m_doc->setCurrentLayerIndex(cmd.index);
    cmd.layer = m_doc->layerAt(cmd.index) ? *m_doc->layerAt(cmd.index)
                                          : CanvasLayer();
    m_undoStack->pushInsertLayer(cmd);
    markAllTilesDirty();
    emit canvasModified();
}

/* ------------------------------------------------------------ selection */

QImage PaintbrushCanvasItem::selectionMaskImage() const
{
    if (!m_doc || m_doc->selectionPath().isEmpty())
        return {};
    const QSize size = m_doc->canvasSize();
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter p(&mask);
    p.fillPath(m_doc->selectionPath(), QColor(255, 255, 255));
    p.end();
    return mask;
}

void PaintbrushCanvasItem::updateSelectionFromDrag(const QPointF &canvasPos)
{
    if (!m_doc)
        return;
    QPainterPath path;
    if (m_tool == TOOL_RECT_SELECT)
    {
        path.addRect(QRectF(m_dragStartCanvas, canvasPos).normalized());
    }
    else
    {
        m_pendingPath.lineTo(canvasPos);
        path = m_pendingPath;
    }
    m_doc->setSelectionPath(path);
    markAllTilesDirty();
}

void PaintbrushCanvasItem::finishSelection()
{
    if (!m_doc)
        return;
    if (m_tool == TOOL_FREEFORM_SELECT)
    {
        m_pendingPath.closeSubpath();
        if (m_pendingPath.boundingRect().isEmpty())
            m_doc->clearSelection();
        else
            m_doc->setSelectionPath(m_pendingPath);
    }
    m_pendingPath = QPainterPath();
    markAllTilesDirty();
}

void PaintbrushCanvasItem::moveSelection(const QPointF &delta)
{
    if (!m_doc || !hasSelection())
        return;
    m_doc->setSelectionPath(m_doc->selectionPath().translated(delta));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::clearSelectionInternal()
{
    if (!m_doc)
        return;
    m_doc->clearSelection();
    markAllTilesDirty();
}

QRectF PaintbrushCanvasItem::selectionBounds() const
{
    if (!m_doc)
        return {};
    return m_doc->selectionPath().boundingRect();
}

void PaintbrushCanvasItem::selectionClear()
{
    clearSelectionInternal();
}

void PaintbrushCanvasItem::selectionSelectAll()
{
    if (!m_doc)
        return;
    QPainterPath all;
    all.addRect(QRectF(QPointF(0, 0), m_doc->canvasSize()));
    m_doc->setSelectionPath(all);
    markAllTilesDirty();
}

void PaintbrushCanvasItem::selectionInvert()
{
    if (!m_doc || m_doc->selectionPath().isEmpty())
        return;
    QPainterPath full;
    full.addRect(QRectF(QPointF(0, 0), m_doc->canvasSize()));
    m_doc->setSelectionPath(full.subtracted(m_doc->selectionPath()));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::selectionGrow(int px)
{
    if (!m_doc || m_doc->selectionPath().isEmpty())
        return;
    const QImage mask = selectionMaskImage();
    /* dilate: paint the mask with a thicker brush */
    QImage dilated(mask.size(), QImage::Format_Grayscale8);
    dilated.fill(0);
    QPainter p(&dilated);
    p.setPen(QPen(QColor(255, 255, 255), px * 2 + 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(m_doc->selectionPath());
    p.end();
    m_doc->setSelectionPath(maskToPath(dilated));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::selectionShrink(int px)
{
    if (!m_doc || m_doc->selectionPath().isEmpty())
        return;
    const QImage mask = selectionMaskImage();

    /* stroke the selection outline: the stroked band is what we remove */
    QImage stroked(mask.size(), QImage::Format_Grayscale8);
    stroked.fill(0);
    QPainter p(&stroked);
    p.setPen(QPen(QColor(255, 255, 255), px * 2 + 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(m_doc->selectionPath());
    p.end();

    /* eroded = original AND NOT outline band */
    QImage eroded(mask.size(), QImage::Format_Grayscale8);
    eroded.fill(0);
    for (int y = 0; y < mask.height(); ++y)
    {
        const uchar *mline = mask.constScanLine(y);
        const uchar *sline = stroked.constScanLine(y);
        uchar *bline = eroded.scanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            bline[x] = mline[x] && !sline[x] ? 255 : 0;
    }

    m_doc->setSelectionPath(maskToPath(eroded));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::selectionSmooth()
{
    if (!m_doc || m_doc->selectionPath().isEmpty())
        return;
    QImage mask = selectionMaskImage();

    /* 3x3 majority filter: a pixel stays selected only if at least 5 of its
     * neighbours are selected. Two passes remove speckle and fill thin
     * holes, giving a rounder silhouette. */
    auto majorityPass = [](const QImage &src) {
        QImage out(src.size(), QImage::Format_Grayscale8);
        out.fill(0);
        for (int y = 0; y < src.height(); ++y)
        {
            const uchar *prev = src.constScanLine(qMax(0, y - 1));
            const uchar *cur = src.constScanLine(y);
            const uchar *next = src.constScanLine(qMin(src.height() - 1, y + 1));
            uchar *dst = out.scanLine(y);
            for (int x = 0; x < src.width(); ++x)
            {
                const int x0 = qMax(0, x - 1), x1 = qMin(src.width() - 1, x + 1);
                int count = 0;
                for (int yy = -1; yy <= 1; ++yy)
                {
                    const uchar *line = yy == -1 ? prev : (yy == 0 ? cur : next);
                    for (int xx = x0; xx <= x1; ++xx)
                        if (line[xx] >= 128)
                            ++count;
                }
                dst[x] = count >= 5 ? 255 : 0;
            }
        }
        return out;
    };

    mask = majorityPass(mask);
    mask = majorityPass(mask);

    m_doc->setSelectionPath(maskToPath(mask));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::selectionFeather(int px)
{
    if (!m_doc || m_doc->selectionPath().isEmpty() || px <= 0)
        return;
    const QImage mask = selectionMaskImage();
    /* blur the mask */
    QImage blurred(mask.size(), QImage::Format_Grayscale8);
    {
        QPainter p(&blurred);
        p.fillRect(blurred.rect(), Qt::black);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));
        p.drawPath(m_doc->selectionPath());
        p.end();
        blurred = blurred.convertToFormat(QImage::Format_Grayscale8);
    }
    QImage soft = blurred;
    /* cheap box blur */
    const int radius = qMax(1, px);
    const int kw = radius * 2 + 1;
    QImage tmp = soft;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int y = 0; y < soft.height(); ++y)
        {
            int acc = 0;
            for (int x = 0; x < qMin(kw, soft.width()); ++x)
                acc += qGray(soft.pixel(x, y));
            for (int x = 0; x < soft.width(); ++x)
            {
                if (x + radius < soft.width())
                    acc += qGray(soft.pixel(x + radius, y));
                if (x - radius - 1 >= 0)
                    acc -= qGray(soft.pixel(x - radius - 1, y));
                tmp.setPixel(x, y, qBound(0, acc / kw, 255));
            }
        }
        for (int x = 0; x < soft.width(); ++x)
        {
            int acc = 0;
            for (int y = 0; y < qMin(kw, soft.height()); ++y)
                acc += qGray(tmp.pixel(x, y));
            for (int y = 0; y < soft.height(); ++y)
            {
                if (y + radius < soft.height())
                    acc += qGray(tmp.pixel(x, y + radius));
                if (y - radius - 1 >= 0)
                    acc -= qGray(tmp.pixel(x, y - radius - 1));
                soft.setPixel(x, y, qBound(0, acc / kw, 255));
            }
        }
    }
    /* threshold at 50% for the path, keep feather by using the soft mask as
     * a selection approximation through the contour of the blurred shape */
    m_doc->setSelectionPath(maskToPath(soft));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::selectionFillHoles()
{
    if (!m_doc || m_doc->selectionPath().isEmpty())
        return;
    const QImage mask = selectionMaskImage();
    /* flood fill from all border pixels with the inverse */
    QImage filled(mask.size(), QImage::Format_Grayscale8);
    filled.fill(255);
    QQueue<QPoint> queue;
    const int w = mask.width(), h = mask.height();
    auto isFg = [&](int x, int y) { return qGray(mask.pixel(x, y)) >= 128; };
    auto isBg = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h &&
               qGray(mask.pixel(x, y)) < 128 &&
               qGray(filled.pixel(x, y)) == 255;
    };
    for (int x = 0; x < w; ++x)
    {
        if (!isFg(x, 0)) { filled.setPixel(x, 0, 0); queue.enqueue(QPoint(x, 0)); }
        if (!isFg(x, h - 1)) { filled.setPixel(x, h - 1, 0); queue.enqueue(QPoint(x, h - 1)); }
    }
    for (int y = 0; y < h; ++y)
    {
        if (!isFg(0, y)) { filled.setPixel(0, y, 0); queue.enqueue(QPoint(0, y)); }
        if (!isFg(w - 1, y)) { filled.setPixel(w - 1, y, 0); queue.enqueue(QPoint(w - 1, y)); }
    }
    while (!queue.isEmpty())
    {
        const QPoint pt = queue.dequeue();
        const int x = pt.x(), y = pt.y();
        if (isBg(x - 1, y)) { filled.setPixel(x - 1, y, 0); queue.enqueue(QPoint(x - 1, y)); }
        if (isBg(x + 1, y)) { filled.setPixel(x + 1, y, 0); queue.enqueue(QPoint(x + 1, y)); }
        if (isBg(x, y - 1)) { filled.setPixel(x, y - 1, 0); queue.enqueue(QPoint(x, y - 1)); }
        if (isBg(x, y + 1)) { filled.setPixel(x, y + 1, 0); queue.enqueue(QPoint(x, y + 1)); }
    }
    m_doc->setSelectionPath(maskToPath(filled));
    markAllTilesDirty();
}

void PaintbrushCanvasItem::copySelection()
{
    if (!m_doc || !hasSelection())
        return;
    const QImage mask = selectionMaskImage();
    /* the canonical render — identical transforms to the screen */
    const QImage flat = DocumentRenderer::render(*m_doc);
    const QRectF bounds = m_doc->selectionPath().boundingRect();
    const QRect b = bounds.toAlignedRect().intersected(flat.rect());
    if (b.isEmpty())
        return;

    QImage result(b.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(0, 0, flat, b.x(), b.y(), b.width(), b.height());
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, mask, b.x(), b.y(), b.width(), b.height());
    p.end();

    m_clipboard = std::move(result);
    m_clipboardAnchor = b.topLeft();
}

void PaintbrushCanvasItem::cutSelection()
{
    if (!m_doc || !hasSelection())
        return;
    copySelection();

    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    /* rasterize the selection in LAYER-LOCAL space so cutting works on
     * transformed image/text layers too */
    const QPainterPath localSel =
        layer->transform().inverted().map(m_doc->selectionPath());
    QImage localMask(layer->raster.size(), QImage::Format_Grayscale8);
    localMask.fill(0);
    {
        QPainter p(&localMask);
        p.fillPath(localSel, QColor(255, 255, 255));
        p.end();
    }
    const QRect affected = localMask.rect().intersected(layer->raster.rect());
    if (affected.isEmpty())
        return;

    m_strokeBefore.clear();
    m_strokeUnion = affected;
    captureBeforeRegion(affected);

    QPainter p(&layer->raster);
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawImage(0, 0, localMask);
    p.end();

    m_strokeHadPaint = true;
    pushStrokeEntry(QStringLiteral("Cut"));
    markTilesDirty(mappedRect(affected));
    emit canvasModified();
}

void PaintbrushCanvasItem::pasteClipboard(const QPointF &at)
{
    if (!m_doc || m_clipboard.isNull())
        return;

    const QPointF pos = at.isNull() ? m_clipboardAnchor : at;
    CanvasLayer layer = CanvasLayer::createImage(m_clipboard, pos,
                                                 QStringLiteral("Pasted"));
    if (!m_undoStack)
        return;
    /* typed insert command: the pasted raster is the immutable asset */
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Paste");
    QList<CanvasLayer> layers = m_doc->layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_doc->currentLayerIndex();
    m_doc->setLayers(layers);
    m_doc->setCurrentLayerIndex(cmd.index);
    cmd.layer = m_doc->layerAt(cmd.index) ? *m_doc->layerAt(cmd.index)
                                          : CanvasLayer();
    m_undoStack->pushInsertLayer(cmd);
    markAllTilesDirty();
    emit canvasModified();
}

void PaintbrushCanvasItem::deleteSelection()
{
    if (!m_doc || !hasSelection())
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    /* selection mask in LAYER-LOCAL space (see cutSelection) */
    const QPainterPath localSel =
        layer->transform().inverted().map(m_doc->selectionPath());
    QImage localMask(layer->raster.size(), QImage::Format_Grayscale8);
    localMask.fill(0);
    {
        QPainter p(&localMask);
        p.fillPath(localSel, QColor(255, 255, 255));
        p.end();
    }
    const QRect affected = localMask.rect().intersected(layer->raster.rect());
    if (affected.isEmpty())
        return;

    m_strokeBefore.clear();
    m_strokeUnion = affected;
    captureBeforeRegion(affected);

    QPainter p(&layer->raster);
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawImage(0, 0, localMask);
    p.end();

    m_strokeHadPaint = true;
    pushStrokeEntry(QStringLiteral("Delete selection"));
    markTilesDirty(mappedRect(affected));
    emit canvasModified();
}

void PaintbrushCanvasItem::setSelectionFromMask(const QImage &mask)
{
    if (!m_doc || mask.isNull())
        return;
    m_doc->setSelectionPath(maskToPath(mask));
    markAllTilesDirty();
}

/* -------------------------------------------------------------- transform */

void PaintbrushCanvasItem::flipHorizontal()
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    m_strokeBefore.clear();
    m_strokeUnion = layer->raster.rect();
    captureBeforeRegion(m_strokeUnion);

    QImage flipped = layer->raster.flipped(Qt::Horizontal);
    layer->raster = std::move(flipped);

    m_strokeHadPaint = true;
    pushStrokeEntry(QStringLiteral("Flip horizontal"));
    markTilesDirty(mappedRect(m_strokeUnion));
    emit canvasModified();
}

void PaintbrushCanvasItem::flipVertical()
{
    if (!m_doc)
        return;
    CanvasLayer *layer = m_doc->currentLayer();
    if (!layer || layer->raster.isNull() || layer->locked)
        return;

    m_strokeBefore.clear();
    m_strokeUnion = layer->raster.rect();
    captureBeforeRegion(m_strokeUnion);

    QImage flipped = layer->raster.flipped(Qt::Vertical);
    layer->raster = std::move(flipped);

    m_strokeHadPaint = true;
    pushStrokeEntry(QStringLiteral("Flip vertical"));
    markTilesDirty(mappedRect(m_strokeUnion));
    emit canvasModified();
}

void PaintbrushCanvasItem::rotate90(bool clockwise)
{
    if (!m_doc || !m_undoStack)
        return;
    m_strokeBefore.clear();

    /* typed command: canvas resize + one SetTransformCommand per layer.
     * Rasters are never resampled, so undo/redo restores the exact pre-op
     * canvas size and layer transforms. */
    m_undoStack->pushCommand(m_doc->makeRotate90Command(clockwise));
    markAllTilesDirty();
    emit canvasModified();
}

void PaintbrushCanvasItem::cropToSelection()
{
    if (!m_doc || !hasSelection() || !m_undoStack)
        return;
    const QRect b = m_doc->selectionPath().boundingRect().toAlignedRect()
                        .intersected(canvasRect());
    if (b.isEmpty() || b == canvasRect())
    {
        m_doc->clearSelection();
        return;
    }

    /* typed command: canvas resize + per-layer SetTransformCommand (pure
     * transforms — the crop rect becomes the new origin; out-of-crop
     * content is clipped by the renderer, rasters stay local) */
    m_undoStack->pushCommand(m_doc->makeCropCommand(b));
    m_doc->clearSelection();
    markAllTilesDirty();
    emit canvasModified();
}

/* ---------------------------------------------------------------- object */

void PaintbrushCanvasItem::setObjectMask(const QImage &mask, double score)
{
    m_objectMask = maskToImage(mask);
    m_objectScore = score;
    setObjectMaskVisible(true);
    emit objectMaskReady(m_objectMask, m_objectScore);
    markAllTilesDirty();
}

void PaintbrushCanvasItem::clearObjectMask()
{
    m_objectMask = QImage();
    m_objectScore = 0.0;
    m_maskPaintActive = false;
    m_maskPaintDirty = false;
    m_maskPaintUnion = QRect();
    setObjectMaskVisible(false);
    markAllTilesDirty();
}

void PaintbrushCanvasItem::setLiveHarvestOverlay(const QImage &nativeMask,
                                                 const QRectF &contentRect,
                                                 const QTransform &frameToItem)
{
    /* Build a VIEWPORT-space display image from the AUTHORITATIVE native
     * (frame-space) mask. `nativeMask` is never modified here — it is mapped
     * into item coordinates for display only (P0#3). */
    m_liveHarvestOverlayValid = !nativeMask.isNull() && !contentRect.isEmpty();
    if (!m_liveHarvestOverlayValid)
    {
        clearLiveHarvestOverlay();
        return;
    }

    const int iw = qMax(1, int(width()));
    const int ih = qMax(1, int(height()));
    QImage overlay(iw, ih, QImage::Format_ARGB32);
    overlay.fill(Qt::transparent);

    /* Trace the native mask silhouette (frame coords) and map it into item
     * coordinates with the frame<->viewport transform. This preserves the
     * native mask byte-for-byte (it is not rescaled); only the display path
     * is transformed. */
    const QPainterPath nativePath = maskToPath(nativeMask);
    if (!nativePath.isEmpty())
    {
        const QPainterPath itemPath = frameToItem.map(nativePath);
        QPainter p(&overlay);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor(30, 90, 220), 2));
        p.setBrush(QColor(60, 90, 200, 90));
        p.drawPath(itemPath);
        p.end();
    }

    m_liveHarvestOverlay = overlay;
    m_liveHarvestContentRect = contentRect;
    m_liveHarvestFrameToItem = frameToItem;
    /* overlay node update is driven by the scene graph in updatePaintNode */
    m_liveOverlayNodeChanged = true;
    update();
}

void PaintbrushCanvasItem::clearLiveHarvestOverlay()
{
    m_liveHarvestOverlay = QImage();
    m_liveHarvestOverlayValid = false;
    m_liveOverlayNodeChanged = true;
    update();
}

void PaintbrushCanvasItem::rebuildLiveHarvestOverlayForCurrentSize()
{
    /* A live overlay is active only when the authoritative native mask and
     * the item are both present. `m_objectMask` carries that native mask for
     * a live selection (frame space), so its size IS the native frame size. */
    if (!m_liveHarvestOverlayValid || m_objectMask.isNull())
        return;

    const QSizeF vs = QSizeF(width(), height());
    const QSizeF fs = QSizeF(m_objectMask.size());
    if (vs.isEmpty() || fs.isEmpty())
        return;

    /* Recomputed letterbox aspect-fit for the CURRENT viewport size — the
     * same geometry as buildSelectionTransforms (pb_selection.hpp), kept in
     * lock-step so the displayed overlay matches the selection context. */
    const qreal fit = qMin(vs.width() / fs.width(), vs.height() / fs.height());
    const qreal cw = fs.width() * fit;
    const qreal ch = fs.height() * fit;
    const QRectF contentRect((vs.width() - cw) / 2.0,
                             (vs.height() - ch) / 2.0, cw, ch);

    /* item -> frame (see buildSelectionTransforms: scale then translate). */
    const QTransform itemToFrame =
        QTransform().scale(1.0 / fit, 1.0 / fit)
                    .translate(-contentRect.left(), -contentRect.top());
    const QTransform frameToItem = itemToFrame.inverted();

    setLiveHarvestOverlay(m_objectMask, contentRect, frameToItem);
}

void PaintbrushCanvasItem::setVideoHarvestMode(bool on)
{
    if (m_videoHarvestMode == on)
        return;
    m_videoHarvestMode = on;
    /* In HarvestVideo mode the canvas stops rendering its opaque document
     * tiles so the live video (anchored behind) shows through. Rebuild the
     * scene graph accordingly. */
    markAllTilesDirty();
    update();
}

QImage PaintbrushCanvasItem::currentObjectMask() const
{
    return m_objectMask;
}

QRectF PaintbrushCanvasItem::objectMaskBounds() const
{
    if (m_objectMask.isNull())
        return {};
    int minX = m_objectMask.width(), minY = m_objectMask.height();
    int maxX = -1, maxY = -1;
    for (int y = 0; y < m_objectMask.height(); ++y)
    {
        const uchar *line = m_objectMask.constScanLine(y);
        for (int x = 0; x < m_objectMask.width(); ++x)
        {
            if (line[x] >= 128)
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return {};
    return QRectF(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

void PaintbrushCanvasItem::cancelObjectSelection()
{
    m_objectSelectBusy = false;
    emit objectSelectBusyChanged(false);
    clearObjectMask();
}

void PaintbrushCanvasItem::setObjectSelectBusy(bool busy)
{
    if (m_objectSelectBusy == busy)
        return;
    m_objectSelectBusy = busy;
    emit objectSelectBusyChanged(busy);
    update();
}

/* Freehand mask refinement. The mask is a binary grayscale image in canvas
 * coordinates; painting stamps a circular brush into it (add = raise to 255,
 * subtract = drop to 0) and dirties only the touched tiles. Stroke lifecycle
 * (active/subtract/dirty) is managed by the input handlers so a plain click
 * can still fall back to a SAM point refinement. */
void PaintbrushCanvasItem::paintObjectMask(const QPointF &canvasPos, bool subtract)
{
    if (m_objectMask.isNull())
        return;
    if (m_objectMask.format() != QImage::Format_Grayscale8)
        m_objectMask = maskToImage(m_objectMask);

    /* In live Harvest mode `m_objectMask` is the AUTHORITATIVE native
     * (frame-space) mask and the freehand pointer must be mapped back into
     * frame space (P0#3): document->item->frame. The display overlay is
     * rebuilt from the refined native mask so the authoritative mask is never
     * destructively rescaled. */
    const bool live = m_liveHarvestOverlayValid;
    QPointF target = canvasPos;
    qreal radius = qMax(qreal(1.0), m_brushSize * 0.5 * pressureFactor());
    if (live)
    {
        const QTransform frameToItem = m_liveHarvestFrameToItem;
        const QTransform itemToFrame = frameToItem.inverted();
        target = itemToFrame.map(itemFromCanvas(canvasPos));
        /* scale the brush radius from on-screen px to frame px */
        const qreal scale = qMax(qreal(0.001),
                                 qSqrt(qAbs(frameToItem.m11() * frameToItem.m22())));
        radius = radius / scale;
    }

    const QRectF af = QRectF(target.x() - radius, target.y() - radius,
                             radius * 2.0, radius * 2.0);
    const QRect affected = af.toAlignedRect().intersected(m_objectMask.rect());
    if (affected.isEmpty())
        return;

    const qreal r2 = radius * radius;
    for (int y = affected.top(); y <= affected.bottom(); ++y)
    {
        uchar *line = m_objectMask.scanLine(y);
        for (int x = affected.left(); x <= affected.right(); ++x)
        {
            const qreal dx = (x + 0.5) - target.x();
            const qreal dy = (y + 0.5) - target.y();
            if (dx * dx + dy * dy <= r2)
            {
                if (subtract)
                    line[x] = 0;
                else
                    line[x] = 255;
            }
        }
    }
    m_maskPaintDirty = true;
    m_maskPaintUnion = m_maskPaintUnion.isNull()
        ? affected : m_maskPaintUnion.united(affected);
    if (live)
    {
        /* rebuild the viewport display overlay from the refined native mask */
        setLiveHarvestOverlay(m_objectMask, m_liveHarvestContentRect,
                              m_liveHarvestFrameToItem);
    }
    else
    {
        markTilesDirty(affected);
    }
}

void PaintbrushCanvasItem::paintObjectMaskSegment(const QPointF &from,
                                                  const QPointF &to,
                                                  bool subtract)
{
    const QLineF seg(from, to);
    const qreal len = seg.length();
    const qreal step = qMax(qreal(1.0), m_brushSize * 0.25);
    const int n = qMax(1, int(qCeil(len / step)));
    const QPointF delta = to - from;
    for (int i = 0; i <= n; ++i)
    {
        const qreal t = n ? qreal(i) / qreal(n) : 0.0;
        paintObjectMask(from + delta * t, subtract);
    }
}

void PaintbrushCanvasItem::endObjectMaskStroke()
{
    if (!m_maskPaintActive)
        return;
    m_maskPaintActive = false;
    const bool painted = m_maskPaintDirty;
    m_maskPaintDirty = false;
    m_maskPaintUnion = QRect();
    if (painted && !m_objectMask.isNull())
        emit objectMaskEdited(m_objectMask);
}

/* ------------------------------------------------------------------ view */

void PaintbrushCanvasItem::zoomIn() { setZoom(m_zoom * 1.25); }
void PaintbrushCanvasItem::zoomOut() { setZoom(m_zoom / 1.25); }

void PaintbrushCanvasItem::fitToView()
{
    if (!m_doc || width() <= 0 || height() <= 0)
        return;
    const QSize canvas = m_doc->canvasSize();
    const qreal scale = qMin(width() / qreal(canvas.width()),
                             height() / qreal(canvas.height()));
    setZoom(scale);
    setPanOffset(QPointF((width() - canvas.width() * scale) / 2.0,
                         (height() - canvas.height() * scale) / 2.0));
}

void PaintbrushCanvasItem::zoomTo(qreal zoom) { setZoom(zoom); }

QImage PaintbrushCanvasItem::grabCanvasRegion(const QRectF &canvasRect_) const
{
    if (!m_doc)
        return {};
    const QRect r = canvasRect_.toAlignedRect().intersected(canvasRect());
    if (r.isEmpty())
        return {};

    /* same canonical compositor as the tiles and export */
    QImage result(r.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    DocumentRenderer::composite(result, r, *m_doc);
    return result;
}

QImage PaintbrushCanvasItem::grabSelectionImage() const
{
    if (!m_doc || !hasSelection())
        return {};
    const QImage mask = selectionMaskImage();
    const QImage flat = DocumentRenderer::render(*m_doc);
    const QRect b = m_doc->selectionPath().boundingRect().toAlignedRect()
                        .intersected(flat.rect());
    if (b.isEmpty())
        return {};

    QImage result(b.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(0, 0, flat, b.x(), b.y(), b.width(), b.height());
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, mask, b.x(), b.y(), b.width(), b.height());
    p.end();
    return result;
}

/* ------------------------------------------------------------ input events */

QPointF PaintbrushCanvasItem::eventLastItemPos() const
{
    return m_lastItemPos;
}

void PaintbrushCanvasItem::mousePressEvent(QMouseEvent *event)
{
    if (!m_doc)
    {
        event->ignore();
        return;
    }

    forceActiveFocus();
    m_lastItemPos = event->position();
    /* tablet/pen detection (Qt 6 exposes device + pressure on the point) */
    const QPointingDevice *device = event->point(0).device();
    m_tabletActive = device && device->type() != QPointingDevice::DeviceType::Mouse;
    m_currentPressure = m_tabletActive ? event->point(0).pressure() : 1.0;
    QPointF canvasPos = canvasFromItem(event->position());
    if (shouldSnap())
        canvasPos = snapPoint(canvasPos);
    m_lastCanvasPos = canvasPos;

    const bool primary = event->button() == Qt::LeftButton;
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);

    /* Object Select: right-click / alt-click adds a negative point, or
     * paints subtract onto an existing mask (freehand refinement) */
    if (m_tool == TOOL_OBJECT_SELECT &&
        (event->button() == Qt::RightButton || alt))
    {
        if (!m_objectMask.isNull())
        {
            /* arm a subtract stroke; a plain click stays a negative SAM point */
            m_strokeActive = true;
            m_maskPaintActive = true;
            m_maskPaintSubtract = true;
            m_maskPaintDirty = false;
            m_maskPaintUnion = QRect();
            m_maskPaintLastPos = canvasPos;
            emit maskRefineStarted(m_objectMask);
            event->accept();
            return;
        }
        emit objectSelectClick(canvasPos, false, itemFromCanvas(canvasPos));
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton)
    {
        m_panning = true;
        m_panLastItem = event->position();
        event->accept();
        return;
    }

    if (primary || event->button() == Qt::RightButton)
        beginStroke(canvasPos, primary);
    event->accept();
}

void PaintbrushCanvasItem::mouseMoveEvent(QMouseEvent *event)
{
    m_lastItemPos = event->position();
    const QPointingDevice *device = event->point(0).device();
    m_tabletActive = device && device->type() != QPointingDevice::DeviceType::Mouse;
    m_currentPressure = m_tabletActive ? event->point(0).pressure() : 1.0;
    QPointF canvasPos = canvasFromItem(event->position());
    if (shouldSnap())
        canvasPos = snapPoint(canvasPos);
    if (m_panning)
    {
        setPanOffset(m_panOffset + (event->position() - m_panLastItem));
        m_panLastItem = event->position();
        event->accept();
        return;
    }
    if (m_strokeActive || m_dragging)
    {
        /* Coalesce the input stream: remember only the LATEST position and
         * let the repaint tick sample it (~120 Hz). Processing every
         * WM_MOUSEMOVE 1:1 queues per-event dab work behind fast moves and
         * the stroke visibly lags the cursor; the latest-sample model keeps
         * latency bounded to one tick. */
        m_pendingMove = canvasPos;
        m_movePending = true;
        /* Arm the flush cadence only if it is not already waiting; restarting
         * the 8 ms timer on every WM_MOUSEMOVE keeps sliding the deadline and
         * the stroke spins behind the cursor under fast moves. */
        if (!m_repaintTimer.isActive())
            m_repaintTimer.start();
        event->accept();
        return;
    }
    updateStatus(canvasPos);
    event->accept();
}

void PaintbrushCanvasItem::mouseReleaseEvent(QMouseEvent *event)
{
    m_lastItemPos = event->position();
    const QPointF canvasPos = canvasFromItem(event->position());
    m_movePending = false;   /* drop any unconsumed sampled move */
    if (m_panning)
    {
        m_panning = false;
        event->accept();
        return;
    }
    if (m_strokeActive || m_dragging)
    {
        endStroke(canvasPos, event->button() == Qt::LeftButton);
        event->accept();
        return;
    }
    event->accept();
}

void PaintbrushCanvasItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    /* double-click on a text layer opens its inline editor (spec §57): the
     * click is hit-tested top-most-first against the text layers' glyph
     * alpha, and the layer is selected before the editor opens */
    const QPointF canvasPos = canvasFromItem(event->position());
    if (m_doc && event->button() == Qt::LeftButton)
    {
        for (int i = m_doc->layerCount() - 1; i >= 0; --i)
        {
            const CanvasLayer &l = *m_doc->layerAt(i);
            if (!l.visible || l.raster.isNull() || l.type != LAYER_TEXT)
                continue;
            const QPointF local = l.mapCanvasToLocal(canvasPos);
            if (local.x() < 0.0 || local.y() < 0.0 ||
                local.x() >= double(l.raster.width()) ||
                local.y() >= double(l.raster.height()))
                continue;
            /* only opaque glyph pixels count as a hit */
            if (qAlpha(l.raster.pixel(int(local.x()), int(local.y()))) > 16)
            {
                m_doc->setCurrentLayerIndex(i);
                emit textLayerDoubleClicked(i);
                event->accept();
                return;
            }
        }
    }
    /* fall back: double-click = single click (avoid accidental double strokes) */
    mousePressEvent(event);
}

void PaintbrushCanvasItem::hoverEnterEvent(QHoverEvent *event)
{
    m_hovered = true;
    emit hoverCanvasChanged(true);
    updateStatus(canvasFromItem(event->position()));
    event->accept();
}

void PaintbrushCanvasItem::hoverMoveEvent(QHoverEvent *event)
{
    updateStatus(canvasFromItem(event->position()));
    event->accept();
}

void PaintbrushCanvasItem::hoverLeaveEvent(QHoverEvent *event)
{
    m_hovered = false;
    emit hoverCanvasChanged(false);
    m_statusText.clear();
    emit statusTextChanged();
    event->accept();
}

void PaintbrushCanvasItem::wheelEvent(QWheelEvent *event)
{
    if (!m_doc)
        return;

    /* With the Spray tool active, the wheel throttles the can (flow) —
     * "how hard the spray comes out". Ctrl+wheel keeps zooming so zoom
     * stays reachable from every tool. */
    if (m_tool == TOOL_SPRAY &&
        !event->modifiers().testFlag(Qt::ControlModifier))
    {
        const qreal delta = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        setSprayFlow(m_sprayFlow * delta);
        event->accept();
        return;
    }

    const QPointF itemPos = event->position();
    const QPointF canvasPos = canvasFromItem(itemPos);
    const qreal delta = event->angleDelta().y() > 0 ? 1.25 : 1.0 / 1.25;
    const qreal newZoom = qBound<qreal>(0.05, m_zoom * delta, 32.0);
    if (qFuzzyCompare(newZoom, m_zoom))
        return;

    /* zoom around the cursor */
    setZoom(newZoom);
    const QPointF newItemPos = itemFromCanvas(canvasPos);
    setPanOffset(m_panOffset + (itemPos - newItemPos));
    event->accept();
}

void PaintbrushCanvasItem::keyPressEvent(QKeyEvent *event)
{
    if (!m_doc)
        return;

    switch (event->key())
    {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (hasSelection())
        {
            deleteSelection();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Left:
        if (hasSelection()) { moveSelection(QPointF(-1, 0)); event->accept(); return; }
        break;
    case Qt::Key_Right:
        if (hasSelection()) { moveSelection(QPointF(1, 0)); event->accept(); return; }
        break;
    case Qt::Key_Up:
        if (hasSelection()) { moveSelection(QPointF(0, -1)); event->accept(); return; }
        break;
    case Qt::Key_Down:
        if (hasSelection()) { moveSelection(QPointF(0, 1)); event->accept(); return; }
        break;
    case Qt::Key_Escape:
        if (hasSelection())
        {
            m_doc->clearSelection();
            markAllTilesDirty();
            event->accept();
            return;
        }
        clearObjectMask();
        break;
    default:
        break;
    }
    QQuickItem::keyPressEvent(event);
}

void PaintbrushCanvasItem::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        const QUrl url = event->mimeData()->urls().first();
        if (url.isLocalFile() || url.scheme() == QStringLiteral("file"))
            event->acceptProposedAction();
    }
}

void PaintbrushCanvasItem::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    const QUrl url = event->mimeData()->urls().first();
    const QPointF canvasPos = canvasFromItem(event->position());
    event->acceptProposedAction();
    emit imageDropped(url, canvasPos);
}

void PaintbrushCanvasItem::markCanvasDirty()
{
    markAllTilesDirty();
}

void PaintbrushCanvasItem::setAnimTime(qint64 timeMs)
{
    if (m_animTimeMs == timeMs)
        return;
    m_animTimeMs = timeMs;
    /* living layers span the whole canvas: recomposite all tiles at the
     * canonical time and upload only the dirty ones (tile system) */
    markAllTilesDirty();
}

void PaintbrushCanvasItem::notifyModified()
{
    emit canvasModified();
}

void PaintbrushCanvasItem::updateStatus(const QPointF &canvasPos)
{
    if (!m_doc)
        return;
    QString text = QStringLiteral("x: %1  y: %2").arg(int(canvasPos.x()))
                                                 .arg(int(canvasPos.y()));
    if (hasSelection())
    {
        const QRectF b = m_doc->selectionPath().boundingRect();
        text += QStringLiteral("   sel: %1 x %2").arg(int(b.width()))
                                                 .arg(int(b.height()));
    }
    text += QStringLiteral("   zoom: %1%").arg(int(m_zoom * 100));
    /* the app state is always visible: active tool + brush size + color,
     * so the user's mental model matches what the next stroke will do */
    text += QStringLiteral("   %1").arg(toolName());
    switch (m_tool)
    {
    case TOOL_PENCIL:
    case TOOL_BRUSH:
    case TOOL_ERASER:
    case TOOL_SPRAY:
    case TOOL_BRISTLE:
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
    case TOOL_SMEAR:
        text += QStringLiteral(" %1px %2").arg(QString::number(m_brushSize, 'f', 0),
                                               m_fgColor.name());
        break;
    default:
        break;
    }
    if (m_tool == TOOL_SPRAY)
        text += QStringLiteral("   flow: %1").arg(m_sprayFlow, 0, 'f', 1);
    if (m_gridVisible)
        text += QStringLiteral("   grid: %1px").arg(m_gridSpacing);
    if (m_snapToGrid)
        text += QStringLiteral("   snap");
    if (m_tabletActive)
        text += QStringLiteral("   pen %1").arg(m_currentPressure, 0, 'f', 2);
    if (m_objectMaskVisible)
    {
        if (m_tool == TOOL_OBJECT_SELECT && m_maskPaintActive)
            text += QStringLiteral("   refining mask (%1)  radius %2 px").arg(
                m_maskPaintSubtract ? QStringLiteral("erase") : QStringLiteral("paint"))
                .arg(qMax<qreal>(1.0, m_brushSize * 0.5));
        else
            text += QStringLiteral("   object selected");
    }
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusTextChanged();
}

} // namespace paintbrush