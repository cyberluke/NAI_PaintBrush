/*****************************************************************************
 * pb_layer.hpp : NAI Paintbrush — canvas layer data
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_LAYER_HPP
#define PB_LAYER_HPP

#include "pb_common.hpp"

#include <QString>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <QVariantMap>

namespace paintbrush {

/* A single canvas layer. Raster content is always LOCAL-sized (a stroke on a
 * 64x48 image layer lives in a 64x48 buffer); the full placement transform
 * (position + center-pivot scale + rotation) maps local content onto the
 * canvas. setCanvasSize() never touches layer rasters — only the document's
 * render target changes. */
class CanvasLayer
{
public:
    CanvasLayer() = default;

    static CanvasLayer createRaster(const QSize &size, const QString &name);
    static CanvasLayer createImage(const QImage &image, const QPointF &position,
                                   const QString &name);
    static CanvasLayer createText(const QString &text, const QPointF &position,
                                  const QSize &size, const QString &name);
    static CanvasLayer createReference(const QImage &image, const QString &name);

    /* content bounds of non-empty pixels (for trimming stickers/cutouts) */
    QRect contentBounds() const;
    bool isEmpty() const;

    /* -- canonical placement transform ------------------------------------
     * transform() maps LOCAL raster coordinates to canvas coordinates as
     *   T = translate(position + c) * rotate(rotation) * scale(scale)
     *       * translate(-c)
     * where c is the center of the local raster (the pivot). For the
     * default raster layer (position 0, scale 1, rotation 0) this is the
     * identity. setTransform() is the exact inverse: it decomposes any
     * uniform affine (translation + uniform scale + rotation) back into
     * position/scale/rotation for this layer's current raster, so
     * transform()/setTransform() round-trip exactly. */
    QTransform transform() const;
    void setTransform(const QTransform &t);
    QPointF mapLocalToCanvas(const QPointF &local) const;
    QPointF mapCanvasToLocal(const QPointF &canvas) const;

    QString name;
    /* stable runtime id for typed undo commands (0 = unassigned; the
     * document assigns monotonically increasing ids on insert/load). */
    int id = 0;
    int type = LAYER_RASTER;
    bool visible = true;
    bool locked = false;
    bool alphaLocked = false;
    float opacity = 1.0f;
    int blendMode = BLEND_NORMAL;

    QImage raster;          /* LOCAL-sized RGBA content (never canvas-resized) */
    QImage mask;            /* optional layer mask (gray, local space) */
    QString text;           /* text layer content */
    QPointF position;       /* image layer placement (see transform()) */
    /* image-layer transform (pivot at the layer content center) */
    float scale = 1.0f;     /* uniform scale factor */
    float rotation = 0.0f;  /* degrees, clockwise */

    /* Sticker provenance (LAYER_STICKER assets): the FrameRef the cutout
     * came from and the seed mask that produced it. Keys:
     *   "sourceMRL"    — media MRL of the source frame (string)
     *   "sourceTitle"  — media title of the source frame (string)
     *   "sourceTimeMs" — timestamp of the source frame (double)
     *   "seedMask"     — object mask as a PNG data: URL (string)
     * Persisted by the project store; additive, never required for
     * rendering (the cutout alpha already encodes the mask). */
    QVariantMap metadata;
};

} // namespace paintbrush

#endif // PB_LAYER_HPP