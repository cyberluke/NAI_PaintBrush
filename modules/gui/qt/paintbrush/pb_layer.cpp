/*****************************************************************************
 * pb_layer.cpp : NAI Paintbrush — canvas layer data implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_layer.hpp"

#include <QtMath>

namespace paintbrush {

QTransform CanvasLayer::transform() const
{
    QTransform t;
    if (qFuzzyIsNull(rotation) && qFuzzyCompare(scale, 1.0f))
    {
        /* the common case (raster layers, untouched image layers): pure
         * translation — identity when position is the origin */
        t.translate(position.x(), position.y());
        return t;
    }
    const QPointF c = raster.isNull() ? QPointF() : QPointF(raster.rect().center());
    t.translate(position.x() + c.x(), position.y() + c.y());
    t.rotate(rotation);
    t.scale(scale, scale);
    t.translate(-c.x(), -c.y());
    return t;
}

void CanvasLayer::setTransform(const QTransform &t)
{
    /* decompose a uniform affine A(x) = L*x + t into
     *   A(x) = position + c + L*(x - c)
     * with c the center of the current local raster (the pivot). Exact for
     * any uniform L (translation + uniform scale + rotation); the forward
     * transform() rebuilds A bit-for-bit. */
    const qreal m11 = t.m11(), m12 = t.m12();
    const qreal m21 = t.m21(), m22 = t.m22();

    const qreal s = std::hypot(m11, m12);
    rotation = float(qRadiansToDegrees(std::atan2(m12, m11)));
    scale = s < 0.001 ? 1.0f : float(s);

    const QPointF c = raster.isNull() ? QPointF() : QPointF(raster.rect().center());
    const QPointF Lc(m11 * c.x() + m21 * c.y(),
                     m12 * c.x() + m22 * c.y());
    position = QPointF(t.m31(), t.m32()) - c + Lc;
}

QPointF CanvasLayer::mapLocalToCanvas(const QPointF &local) const
{
    return transform().map(local);
}

QPointF CanvasLayer::mapCanvasToLocal(const QPointF &canvas) const
{
    return transform().inverted().map(canvas);
}

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
        /* image/reference layers: the whole raster is content, offset by
         * position and scaled/rotated by the layer transform. The scaled
         * size is exact; rotation is treated as its bounding box so the
         * collage slot stays generous. */
        QSizeF scaled = QSizeF(raster.size()) * (type == LAYER_IMAGE || type == LAYER_TEXT
                                                     ? qMax<qreal>(0.001, scale) : 1.0);
        QRectF r(position, scaled);
        if (!qFuzzyIsNull(rotation))
        {
            const qreal rad = qDegreesToRadians(rotation);
            const qreal c = qAbs(qCos(rad)), s = qAbs(qSin(rad));
            const qreal w = scaled.width() * c + scaled.height() * s;
            const qreal h = scaled.width() * s + scaled.height() * c;
            r.setSize(QSizeF(w, h));
        }
        return r.toAlignedRect();
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