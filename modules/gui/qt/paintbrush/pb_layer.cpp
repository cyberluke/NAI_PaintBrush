/*****************************************************************************
 * pb_layer.cpp : NAI Paintbrush — canvas layer data implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_layer.hpp"

namespace paintbrush {

CanvasLayer CanvasLayer::createRaster(const QSize &size, const QString &name)
{
    CanvasLayer layer;
    layer.name = name;
    layer.type = LAYER_RASTER;
    if (size.isValid() && !size.isEmpty())
        layer.raster = QImage(size, QImage::Format_ARGB32);
    if (!layer.raster.isNull())
        layer.raster.fill(Qt::transparent);
    return layer;
}

CanvasLayer CanvasLayer::createImage(const QImage &image, const QPointF &position,
                                     const QString &name)
{
    CanvasLayer layer;
    layer.name = name;
    layer.type = LAYER_IMAGE;
    layer.raster = image.convertToFormat(QImage::Format_ARGB32);
    layer.position = position;
    return layer;
}

CanvasLayer CanvasLayer::createText(const QString &text, const QPointF &position,
                                    const QSize &size, const QString &name)
{
    CanvasLayer layer;
    layer.name = name;
    layer.type = LAYER_TEXT;
    layer.text = text;
    layer.position = position;
    if (size.isValid() && !size.isEmpty())
        layer.raster = QImage(size, QImage::Format_ARGB32);
    if (!layer.raster.isNull())
        layer.raster.fill(Qt::transparent);
    return layer;
}

CanvasLayer CanvasLayer::createReference(const QImage &image, const QString &name)
{
    CanvasLayer layer;
    layer.name = name;
    layer.type = LAYER_REFERENCE;
    layer.raster = image.convertToFormat(QImage::Format_ARGB32);
    return layer;
}

QRect CanvasLayer::contentBounds() const
{
    if (raster.isNull())
        return {};
    if (type == LAYER_IMAGE || type == LAYER_REFERENCE || type == LAYER_TEXT)
    {
        /* image/reference layers: the whole raster is content, offset by position */
        const QRect r = raster.rect();
        return r.translated(position.toPoint());
    }

    const int w = raster.width();
    const int h = raster.height();
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(raster.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            if (qAlpha(line[x]) != 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return {};
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

bool CanvasLayer::isEmpty() const
{
    return raster.isNull() || contentBounds().isEmpty();
}

} // namespace paintbrush