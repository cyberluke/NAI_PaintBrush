/*****************************************************************************
 * pb_canvas_item.hpp : NAI Paintbrush — tile-based raster canvas
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CANVAS_ITEM_HPP
#define PB_CANVAS_ITEM_HPP

#include "pb_common.hpp"
#include "pb_history.hpp"
#include "pb_document.hpp"
#include "art/stroke/bristle_brush.hpp"
#include "art/materials/glitter/glitter_material.hpp"

#include <QQuickItem>
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QHash>
#include <QSet>
#include <QPair>
#include <QFont>
#include <QTimer>
#include <QSGTexture>

class QSGNode;
class QSGTransformNode;
class QSGTexture;

namespace paintbrush {

class PaintbrushDocument;
class PaintbrushController;

/* The native high-performance canvas. Raster content lives in per-layer
 * QImages (owned by the document); this item composites them into a tile
 * backing store and uploads only dirty tiles to the scene graph, so a
 * stroke never repaints or re-uploads the whole canvas. */
class PaintbrushCanvasItem : public QQuickItem
{
    Q_OBJECT

    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(QColor fgColor READ fgColor WRITE setFgColor NOTIFY fgColorChanged FINAL)
    Q_PROPERTY(QColor bgColor READ bgColor WRITE setBgColor NOTIFY bgColorChanged FINAL)
    Q_PROPERTY(qreal brushSize READ brushSize WRITE setBrushSize NOTIFY brushSizeChanged FINAL)
    Q_PROPERTY(qreal sprayDensity READ sprayDensity WRITE setSprayDensity NOTIFY sprayDensityChanged FINAL)
    Q_PROPERTY(qreal sprayFlow READ sprayFlow WRITE setSprayFlow NOTIFY sprayFlowChanged FINAL)
    Q_PROPERTY(bool gridVisible READ gridVisible WRITE setGridVisible NOTIFY gridVisibleChanged FINAL)
    Q_PROPERTY(bool rulersVisible READ rulersVisible WRITE setRulersVisible NOTIFY rulersVisibleChanged FINAL)
    Q_PROPERTY(int gridSpacing READ gridSpacing WRITE setGridSpacing NOTIFY gridSpacingChanged FINAL)
    Q_PROPERTY(bool snapToGrid READ snapToGrid WRITE setSnapToGrid NOTIFY snapToGridChanged FINAL)
    Q_PROPERTY(bool pressureEnabled READ pressureEnabled WRITE setPressureEnabled NOTIFY pressureEnabledChanged FINAL)
    Q_PROPERTY(bool pressureToOpacity READ pressureToOpacity WRITE setPressureToOpacity NOTIFY pressureToOpacityChanged FINAL)
    Q_PROPERTY(qreal pressureMin READ pressureMin WRITE setPressureMin NOTIFY pressureMinChanged FINAL)
    Q_PROPERTY(qreal pressureMax READ pressureMax WRITE setPressureMax NOTIFY pressureMaxChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(QPointF panOffset READ panOffset WRITE setPanOffset NOTIFY panOffsetChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool objectSelectBusy READ objectSelectBusy NOTIFY objectSelectBusyChanged FINAL)
    Q_PROPERTY(bool objectMaskVisible READ objectMaskVisible WRITE setObjectMaskVisible NOTIFY objectMaskVisibleChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged FINAL)
    Q_PROPERTY(qreal fillTolerance READ fillTolerance WRITE setFillTolerance NOTIFY fillToleranceChanged FINAL)
    /* Paintbrush 2.0 art tool controls (spec §14.3 primary controls) */
    Q_PROPERTY(qreal glitterAmount READ glitterAmount WRITE setGlitterAmount NOTIFY artParamsChanged FINAL)
    Q_PROPERTY(qreal glitterFlakeSize READ glitterFlakeSize WRITE setGlitterFlakeSize NOTIFY artParamsChanged FINAL)
    Q_PROPERTY(qreal glitterShimmer READ glitterShimmer WRITE setGlitterShimmer NOTIFY artParamsChanged FINAL)
    Q_PROPERTY(qreal glitterAdhesion READ glitterAdhesion WRITE setGlitterAdhesion NOTIFY artParamsChanged FINAL)
    Q_PROPERTY(qreal brushFlow READ brushFlow WRITE setBrushFlow NOTIFY artParamsChanged FINAL)
    Q_PROPERTY(qreal brushColorVariation READ brushColorVariation WRITE setBrushColorVariation NOTIFY artParamsChanged FINAL)

public:
    qreal glitterAmount() const { return m_glitterParams.amount; }
    void setGlitterAmount(qreal v);
    qreal glitterFlakeSize() const { return m_glitterParams.flakeSize; }
    void setGlitterFlakeSize(qreal v);
    qreal glitterShimmer() const { return m_glitterParams.shimmer; }
    void setGlitterShimmer(qreal v);
    qreal glitterAdhesion() const { return m_glitterParams.adhesion; }
    void setGlitterAdhesion(qreal v);
    qreal brushFlow() const { return m_brushFlow; }
    void setBrushFlow(qreal v);
    qreal brushColorVariation() const { return m_brushColorVariation; }
    void setBrushColorVariation(qreal v);
    /* Glitter grain palette from the mood (bounded set, spec §14.4). */
    void setGlitterPalette(const QVector<QColor> &palette, const QString &id);

signals:
    void artParamsChanged();

private:
    qreal m_brushFlow = 0.75;
    qreal m_brushColorVariation = 0.15;

public:
    explicit PaintbrushCanvasItem(QQuickItem *parent = nullptr);
    ~PaintbrushCanvasItem() override;

    /* -- configuration (set by the controller) -------------------------- */
    void setDocument(PaintbrushDocument *doc);
    PaintbrushDocument *document() const { return m_doc; }
    void setUndoStack(UndoStack *stack);
    void setController(PaintbrushController *controller) { m_controller = controller; }

    /* -- properties ------------------------------------------------------ */
    int tool() const { return m_tool; }
    void setTool(int tool);
    QColor fgColor() const { return m_fgColor; }
    void setFgColor(const QColor &color);
    QColor bgColor() const { return m_bgColor; }
    void setBgColor(const QColor &color);
    qreal brushSize() const { return m_brushSize; }
    void setBrushSize(qreal size);
    qreal sprayDensity() const { return m_sprayDensity; }
    void setSprayDensity(qreal density);
    /* Spray flow (how much paint the can releases, 0.1–4.0). The mouse
     * wheel adjusts it while the Spray tool is active. */
    qreal sprayFlow() const { return m_sprayFlow; }
    void setSprayFlow(qreal flow);
    /* Precision drawing: optional canvas grid (display + snap). */
    bool gridVisible() const { return m_gridVisible; }
    void setGridVisible(bool visible);
    /* Pixel-coordinate rulers along the canvas edges. Independent from the
     * grid so a user can keep the grid off and still read coordinates. */
    bool rulersVisible() const { return m_rulersVisible; }
    void setRulersVisible(bool visible);
    int gridSpacing() const { return m_gridSpacing; }
    void setGridSpacing(int spacing);
    bool snapToGrid() const { return m_snapToGrid; }
    void setSnapToGrid(bool snap);
    /* Pen/tablet pressure (mouse is unaffected: pressure reads 1.0). */
    bool pressureEnabled() const { return m_pressureEnabled; }
    void setPressureEnabled(bool enabled);
    bool pressureToOpacity() const { return m_pressureToOpacity; }
    void setPressureToOpacity(bool enabled);
    qreal pressureMin() const { return m_pressureMin; }
    void setPressureMin(qreal min);
    qreal pressureMax() const { return m_pressureMax; }
    void setPressureMax(qreal max);
    /* QML hint: true while a tablet/pen is the active pointing device. */
    bool tabletActive() const { return m_tabletActive; }
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal zoom);
    QPointF panOffset() const { return m_panOffset; }
    void setPanOffset(const QPointF &offset);
    bool hasSelection() const { return m_doc ? !m_doc->selectionPath().isEmpty() : false; }
    bool objectSelectBusy() const { return m_objectSelectBusy; }
    bool objectMaskVisible() const { return m_objectMaskVisible; }
    void setObjectMaskVisible(bool visible);
    QString statusText() const { return m_statusText; }
    qreal fillTolerance() const { return m_fillTolerance; }
    void setFillTolerance(qreal tolerance);

    /* -- AI object selection --------------------------------------------- */
    Q_INVOKABLE void setObjectMask(const QImage &mask, double score);
    Q_INVOKABLE void clearObjectMask();
    Q_INVOKABLE QImage currentObjectMask() const;
    Q_INVOKABLE void cancelObjectSelection();
    Q_INVOKABLE QRectF objectMaskBounds() const;
    Q_INVOKABLE void setObjectSelectBusy(bool busy);

    /* Freehand mask refinement: paint add (true) or subtract (false) into
     * the current object mask with the active brush size. Emits
     * objectMaskEdited() after the stroke completes. */
    Q_INVOKABLE void paintObjectMask(const QPointF &canvasPos, bool subtract);
    Q_INVOKABLE void paintObjectMaskSegment(const QPointF &from, const QPointF &to,
                                            bool subtract);
    Q_INVOKABLE void endObjectMaskStroke();

    /* -- manual selection operations -------------------------------------- */
    Q_INVOKABLE void selectionInvert();
    Q_INVOKABLE void selectionClear();
    Q_INVOKABLE void selectionGrow(int px);
    Q_INVOKABLE void selectionShrink(int px);
    Q_INVOKABLE void selectionFeather(int px);
    Q_INVOKABLE void selectionFillHoles();
    Q_INVOKABLE void selectionSmooth();
    Q_INVOKABLE void selectionSelectAll();
    Q_INVOKABLE void cutSelection();
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void pasteClipboard(const QPointF &at);
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE QRectF selectionBounds() const;

    /* -- transform ---------------------------------------------------------- */
    Q_INVOKABLE void flipHorizontal();
    Q_INVOKABLE void flipVertical();
    Q_INVOKABLE void rotate90(bool clockwise);
    Q_INVOKABLE void cropToSelection();

    /* -- text --------------------------------------------------------------- */
    Q_INVOKABLE void applyText(const QString &text, const QPointF &at,
                               const QFont &font, const QColor &color);

    /* -- view ---------------------------------------------------------------- */
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void fitToView();
    Q_INVOKABLE void zoomTo(qreal zoom);

    /* -- helpers -------------------------------------------------------------- */
    Q_INVOKABLE QPointF canvasFromItem(const QPointF &itemPos) const;
    Q_INVOKABLE QPointF itemFromCanvas(const QPointF &canvasPos) const;
    Q_INVOKABLE QImage grabCanvasRegion(const QRectF &canvasRect) const;
    Q_INVOKABLE QImage grabSelectionImage() const;
    Q_INVOKABLE void setSelectionFromMask(const QImage &mask);
    Q_INVOKABLE QImage selectionMaskImage() const;   /* binary mask of current selection */

    /* -- painting entry points used by the controller ------------------------ */
    void markCanvasDirty();          /* full re-composite */

    /* -- Living Showroom: the canonical time axis ------------------------ */
    /* The canvas composites living layers at this document time. Static
     * documents (no living layer) always render at t=0 — byte-identical to
     * the pre-Showroom path. The AnimationClock drives this at 24-30 Hz. */
    void setAnimTime(qint64 timeMs);
    qint64 animTimeMs() const { return m_animTimeMs; }
    /* True while a stroke/interaction is in progress (the clock may drop
     * its refresh rate during editing gestures). */
    bool strokeActive() const { return m_strokeActive; }

signals:
    void toolChanged(int tool);
    void fgColorChanged();
    void bgColorChanged();
    void brushSizeChanged();
    void sprayDensityChanged();
    void sprayFlowChanged();
    void gridVisibleChanged();
    void rulersVisibleChanged();
    void gridSpacingChanged();
    void snapToGridChanged();
    void pressureEnabledChanged();
    void pressureToOpacityChanged();
    void pressureMinChanged();
    void pressureMaxChanged();
    void zoomChanged();
    void panOffsetChanged();
    void selectionChanged(bool hasSelection);
    void objectSelectBusyChanged(bool busy);
    void objectMaskVisibleChanged(bool visible);
    void objectMaskReady(const QImage &mask, double score);
    void objectMaskEdited(const QImage &mask);
    void maskRefineStarted(const QImage &mask);
    void objectSelectClick(QPointF canvasPos, bool positive, QPointF itemPos);
    void strokeCommitted(const QRect &region);
    void canvasModified();
    void statusTextChanged();
    void hoverCanvasChanged(bool hovered);
    void colorPicked(const QColor &color);
    void imageDropped(QUrl url, QPointF canvasPos);
    void fillToleranceChanged();
    /* Double-click landed on a text layer: QML opens the inline text editor
     * for that layer (spec §57 — text stays truly editable). */
    void textLayerDoubleClicked(int layerIndex);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    static constexpr int TILE = 256;

    struct TileKey { int x = 0; int y = 0; };
    friend bool operator==(const TileKey &a, const TileKey &b)
    { return a.x == b.x && a.y == b.y; }
    friend size_t qHash(const TileKey &k, size_t seed = 0)
    { return qHashMulti(seed, k.x, k.y); }

    /* -- composite --------------------------------------------------------- */
    void compositeTile(int tx, int ty, QImage &tile) const;
    void compositeTileRegion(int tx, int ty, QImage &tile, const QRect &region) const;
    void markTilesDirty(const QRect &canvasRect);
    void markAllTilesDirty();
    void compositeDirtyTiles();
    QRect canvasRect() const;

    /* -- painting ------------------------------------------------------------ */
    void beginStroke(const QPointF &canvasPos, bool primary);
    void continueStroke(const QPointF &canvasPos);
    void endStroke(const QPointF &canvasPos, bool primary);
    void strokeSegment(const QPointF &from, const QPointF &to);
    /* dab pipeline (A22): pointer sample -> normalizer -> stabilizer ->
     * velocity -> spacing -> dab -> per-stroke scratch tile -> commit
     * region delta on stroke end */
    void strokeSegmentDabs(const QPointF &fromLocal, const QPointF &toLocal);
    void stampDabAt(const QPointF &localCenter, qreal radius, qreal hardness,
                    qreal opacity, bool erase);
    void ensureScratch(const QRect &localRect);
    void commitScratch(const QString &desc);
    QPointF localFromCanvas(const QPointF &canvasPos) const;
    QRect mappedRect(const QRect &localRect) const;
    void floodFillAt(const QPointF &canvasPos);
    void sampleColorAt(const QPointF &canvasPos);
    QPainterPath buildShapePath(const QPointF &a, const QPointF &b) const;
    void commitShape(const QPainterPath &path, bool filled);
    void captureBeforeRegion(const QRect &localRect);
    void pushStrokeEntry(const QString &desc);
    void maskStrokeToSelectionAndAlphaLock();
    void notifyModified();
    QPointF eventLastItemPos() const;

    /* -- spray (graffiti physics) -------------------------------------------- */
    void emitSprayPuff();            /* one paint emission, driven by the timer */
    void resetSprayState();

    /* -- selection ------------------------------------------------------------ */
    void updateSelectionFromDrag(const QPointF &canvasPos);
    void finishSelection();
    void clearSelectionInternal();
    void moveSelection(const QPointF &delta);

    /* -- misc ------------------------------------------------------------------ */
    void updateStatus(const QPointF &canvasPos);
    void rebuildTransformNode(QSGTransformNode *root);
    QRect tileRect(int tx, int ty) const;
    QPointF snapPoint(const QPointF &p) const;
    QSGNode *buildGridNode();
    bool shouldSnap() const;
    qreal pressureFactor() const;   /* 0..1 applied when pressure enabled */
    /* adaptive texture filtering: nearest on integer (pixel-art) zoom,
     * linear on continuous zoom (A25) */
    QSGTexture::Filtering adaptiveFiltering() const;
    /* per-tool mouse cursor: paint tools get a round tip at the exact
     * on-screen dab size filled with the current color, so the cursor
     * always mirrors what the stroke will do */
    void updateCursor();
    QString toolName() const;

    PaintbrushDocument *m_doc = nullptr;
    UndoStack *m_undoStack = nullptr;
    PaintbrushController *m_controller = nullptr;

    int m_tool = TOOL_BRUSH;
    QColor m_fgColor = Qt::black;
    QColor m_bgColor = Qt::white;
    qreal m_brushSize = 6.0;
    qreal m_sprayDensity = 24.0;
    qreal m_sprayFlow = 1.0;
    qreal m_fillTolerance = 0.0;
    qreal m_zoom = 1.0;
    QPointF m_panOffset;
    QString m_statusText;
    bool m_objectSelectBusy = false;
    bool m_objectMaskVisible = false;

    /* precision drawing: grid display/snap, rulers, tablet pressure */
    bool m_gridVisible = false;
    bool m_rulersVisible = false;
    int m_gridSpacing = 32;
    bool m_snapToGrid = false;
    bool m_pressureEnabled = false;
    bool m_pressureToOpacity = false;
    qreal m_pressureMin = 0.2;
    qreal m_pressureMax = 1.0;
    bool m_tabletActive = false;

    /* spray physics state (graffiti can) */
    QTimer m_sprayTimer;             /* continuous emission while held */
    QTimer m_repaintTimer;           /* coalesced dirty-tile flush (~120 Hz) */
    QPointF m_sprayNozzle;           /* the can tip (canvas coords)      */
    QPointF m_sprayTarget;           /* where the hand points now        */
    QPointF m_sprayVel;              /* smoothed hand velocity           */
    qreal m_sprayWobblePhase = 0.0;  /* hand-shake phase                 */
    qreal m_sprayWobbleAmp = 0.0;    /* smoothed shake amplitude         */

    QImage m_objectMask;               /* grayscale binary mask (canvas size) */
    double m_objectScore = 0.0;

    /* freehand mask refinement stroke state */
    bool m_maskPaintActive = false;
    bool m_maskPaintSubtract = false;
    bool m_maskPaintDirty = false;  /* a dot was actually painted (drag, not click) */
    QPointF m_maskPaintLastPos;
    QRect m_maskPaintUnion;

    /* tiles */
    QHash<TileKey, QImage> m_tiles;
    QSet<TileKey> m_dirtyTiles;
    QHash<TileKey, QSGTexture *> m_textures;
    QHash<TileKey, QSGTransformNode *> m_tileNodes;
    QSet<TileKey> m_changedTiles;    /* tiles re-uploaded this frame */
    QSGNode *m_gridNode = nullptr;   /* overlay geometry, owned by scene graph */

    /* stroke state */
    bool m_strokeActive = false;
    bool m_strokePrimary = true;
    QPointF m_lastCanvasPos;
    QPointF m_pendingMove;           /* latest sampled input position */
    bool m_movePending = false;      /* a sampled move awaits the next tick */
    QRect m_strokeUnion;              /* layer-local coords */
    QList<QPair<QRect, QImage>> m_strokeBefore;

    /* Living Showroom: canonical animation time (0 = static poster) */
    qint64 m_animTimeMs = 0;
    bool m_strokeHadPaint = false;
    qreal m_currentPressure = 1.0;   /* 1.0 for mouse; tablet reports 0..1 */

    /* dab-brush pipeline state (A22): per-stroke scratch tile in LAYER-LOCAL
     * coordinates, seeded with the real layer content, committed as one
     * region delta on stroke end */
    QImage m_scratchTile;
    QPoint m_scratchOrigin;
    QRect m_scratchSeeded;
    QPointF m_strokeSmooth;          /* stabilized sample (local coords) */
    QPointF m_strokeLastDab;         /* last dab position (local coords) */
    QVector<QPointF> m_stabilizer;   /* recent samples for smoothing */
    qreal m_strokeRemainder = 0.0;   /* spacing carry-over along a segment */
    qreal m_strokeSpeed = 0.0;       /* smoothed sample speed (px/event) */

    /* Paintbrush 2.0 art tools. Both ride the same scratch-tile + typed
     * StrokeCommand commit path as the brush (spec §13.1, §46). */
    art::BristleBrushEngine m_bristle;   /* stable bristle set per stroke */
    /* glitter: a per-stroke bounded grain grid in layer-local world units;
     * the render is stamped into the scratch tile at stroke end, the state
     * is kept on the layer for export re-rendering (spec §55) */
    art::glitter::GlitterState m_glitterState;
    art::GlitterParams m_glitterParams;
    QPointF m_glitterLast;
    QRect m_glitterUnion;            /* layer-local affected rect */
    QString m_glitterPaletteId;
    void beginArtStroke(const QPointF &local);
    void artStrokeSegment(const QPointF &fromLocal, const QPointF &toLocal);
    void endArtStroke(const QPointF &local);

    /* selection state */
    QPointF m_dragStartCanvas;
    QPointF m_dragLastCanvas;
    bool m_dragging = false;
    bool m_movingSelection = false;
    QPainterPath m_pendingPath;        /* freeform build-up */
    QPainterPath m_shapePreviewPath;   /* shape rubber band */
    QRect m_prevPreviewRect;           /* previous preview area */
    QImage m_clipboard;
    QPointF m_clipboardAnchor;

    bool m_hovered = false;
    bool m_panning = false;
    QPointF m_panLastItem;
    QPointF m_lastItemPos;
};

} // namespace paintbrush

#endif // PB_CANVAS_ITEM_HPP