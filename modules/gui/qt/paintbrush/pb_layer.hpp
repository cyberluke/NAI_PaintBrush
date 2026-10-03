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

namespace paintbrush {

/* A single canvas layer. Raster content is stored as a full-canvas RGBA
 * image (QImage is copy-on-write so per-command undo snapshots are cheap).
 * Image layers carry an optional position/size transform. */
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

    QString name;
    int type = LAYER_RASTER;
    bool visible = true;
    bool locked = false;
    bool alphaLocked = false;
    float opacity = 1.0f;
    int blendMode = BLEND_NORMAL;

    QImage raster;          /* full-canvas RGBA content */
    QImage mask;            /* optional layer mask (gray) */
    QString text;           /* text layer content */
    QPointF position;       /* image layer placement */
    /* image-layer transform (pivot at the layer content center) */
    float scale = 1.0f;     /* uniform scale factor */
    float rotation = 0.0f;  /* degrees, clockwise */
};

} // namespace paintbrush

#endif // PB_LAYER_HPP