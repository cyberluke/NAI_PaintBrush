/*****************************************************************************
 * pb_document.cpp : NAI Paintbrush — document model implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_document.hpp"
#include "pb_history.hpp"

#include <QPainter>
#include <QtMath>

namespace paintbrush {

PaintbrushDocument::PaintbrushDocument(QObject *parent)
    : QObject(parent)
{
}

void PaintbrushDocument::resetDocument(const QSize &size)
{
    m_canvasSize = size.isValid() && !size.isEmpty() ? size : QSize(1280, 720);
    m_layers.clear();
    m_nextLayerId = 1;
    m_currentLayerIndex = -1;
    m_selection = QPainterPath();
    m_hasSelection = false;
    m_sourceMRL.clear();
    m_sourceTitle.clear();
    m_sourceTimeMs = 0;
    m_tracks.clear();
    m_projectPath.clear();
    m_dirty = false;

    /* Start with one empty raster layer, like Paintbrush does. */
    m_layers.append(CanvasLayer::createRaster(m_canvasSize, QStringLiteral("Layer 1")));
    m_layers[0].id = m_nextLayerId++;
    m_currentLayerIndex = 0;

    emit canvasSizeChanged();
    emit layersChanged();
    emit currentLayerChanged(m_currentLayerIndex);
    emit selectionChanged();
    emit mediaChanged();
    emit tracksChanged();
    emit dirtyChanged(false);
    emit documentReset();
}

void PaintbrushDocument::setCanvasSize(const QSize &size)
{
    if (size == m_canvasSize || !size.isValid() || size.isEmpty())
        return;

    /* renderTarget only: layer rasters stay LOCAL-sized. A layer's placement
     * transform maps local content onto the (possibly resized) canvas. */
    m_canvasSize = size;
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::assignLayerIds()
{
    for (CanvasLayer &layer : m_layers)
        if (layer.id == 0)
            layer.id = m_nextLayerId++;
}

void PaintbrushDocument::setLayers(const QList<CanvasLayer> &layers)
{
    m_layers = layers;
    assignLayerIds();
    if (m_currentLayerIndex >= m_layers.size())
        m_currentLayerIndex = m_layers.isEmpty() ? -1 : m_layers.size() - 1;
    m_dirty = true;
    emit layersChanged();
    emit currentLayerChanged(m_currentLayerIndex);
    emit dirtyChanged(true);
}

void PaintbrushDocument::setCurrentLayerIndex(int index)
{
    if (index < -1 || index >= m_layers.size() || index == m_currentLayerIndex)
        return;
    m_currentLayerIndex = index;
    emit currentLayerChanged(index);
}

CanvasLayer *PaintbrushDocument::currentLayer()
{
    return layerAt(m_currentLayerIndex);
}

const CanvasLayer *PaintbrushDocument::currentLayer() const
{
    return layerAt(m_currentLayerIndex);
}

CanvasLayer *PaintbrushDocument::layerAt(int index)
{
    if (index < 0 || index >= m_layers.size())
        return nullptr;
    return &m_layers[index];
}

const CanvasLayer *PaintbrushDocument::layerAt(int index) const
{
    if (index < 0 || index >= m_layers.size())
        return nullptr;
    return &m_layers[index];
}

int PaintbrushDocument::indexOfLayerId(int id) const
{
    for (int i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].id == id)
            return i;
    return -1;
}

CanvasLayer *PaintbrushDocument::layerById(int id)
{
    const int index = indexOfLayerId(id);
    return index < 0 ? nullptr : &m_layers[index];
}

const CanvasLayer *PaintbrushDocument::layerById(int id) const
{
    const int index = indexOfLayerId(id);
    return index < 0 ? nullptr : &m_layers[index];
}

void PaintbrushDocument::setSelectionPath(const QPainterPath &path)
{
    m_selection = path;
    m_hasSelection = !path.isEmpty();
    emit selectionChanged();
}

void PaintbrushDocument::clearSelection()
{
    if (!m_hasSelection && m_selection.isEmpty())
        return;
    m_selection = QPainterPath();
    m_hasSelection = false;
    emit selectionChanged();
}

void PaintbrushDocument::setSourceMedia(const QString &mrl, const QString &title,
                                        qint64 timeMs)
{
    if (m_sourceMRL == mrl && m_sourceTitle == title && m_sourceTimeMs == timeMs)
        return;
    m_sourceMRL = mrl;
    m_sourceTitle = title;
    m_sourceTimeMs = timeMs;
    /* keep the exact-frame identity in sync (A19) */
    m_sourceFrame.mrl = mrl;
    m_sourceFrame.title = title;
    m_sourceFrame.ptsMs = qMax<qint64>(0, timeMs);
    m_dirty = true;
    emit mediaChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::setSourceFrame(const FrameRef &ref)
{
    m_sourceFrame = ref;
    m_sourceMRL = ref.mrl;
    m_sourceTitle = ref.title;
    m_sourceTimeMs = qMax<qint64>(0, ref.ptsMs);
    m_dirty = true;
    emit mediaChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::clearSourceMedia()
{
    setSourceMedia({}, {}, 0);
}

void PaintbrushDocument::setTracks(const QVector<ObjectTrack> &tracks)
{
    m_tracks = tracks;
    m_dirty = true;
    emit tracksChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::setProjectPath(const QString &path)
{
    if (m_projectPath == path)
        return;
    m_projectPath = path;
}

void PaintbrushDocument::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(dirty);
}

/* ------------------------------------------------------ canvas transforms */

QTransform pbCanvasRotateTransform(const QSize &oldCanvasSize, bool clockwise)
{
    const QPointF cOld(oldCanvasSize.width() / 2.0, oldCanvasSize.height() / 2.0);
    const QPointF cNew(oldCanvasSize.height() / 2.0, oldCanvasSize.width() / 2.0);
    QTransform t;
    t.translate(cNew.x(), cNew.y());
    t.rotate(clockwise ? 90 : -90);
    t.translate(-cOld.x(), -cOld.y());
    return t;
}

QTransform pbCanvasCropShift(const QRect &cropRect)
{
    QTransform t;
    t.translate(-cropRect.left(), -cropRect.top());
    return t;
}

std::unique_ptr<UndoCommand> PaintbrushDocument::makeRotate90Command(bool clockwise)
{
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Rotate");

    const QSize oldSize = m_canvasSize;
    const QSize newSize(oldSize.height(), oldSize.width());
    composite->children.push_back(std::make_unique<CanvasResizeCommand>(
        CanvasResizeCommand{oldSize, newSize, composite->desc}));

    const QTransform rot = pbCanvasRotateTransform(oldSize, clockwise);
    for (int i = 0; i < m_layers.size(); ++i)
    {
        CanvasLayer &layer = m_layers[i];
        if (layer.raster.isNull())
            continue;
        const QTransform before = layer.transform();
        layer.setTransform(rot * before);
        const QTransform after = layer.transform();
        composite->children.push_back(std::make_unique<SetTransformCommand>(
            SetTransformCommand{layer.id, i, before, after, composite->desc}));
    }

    m_canvasSize = newSize;
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
    return composite;
}

std::unique_ptr<UndoCommand> PaintbrushDocument::makeCropCommand(const QRect &rect)
{
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Crop");

    const QSize oldSize = m_canvasSize;
    const QSize newSize = rect.size();
    composite->children.push_back(std::make_unique<CanvasResizeCommand>(
        CanvasResizeCommand{oldSize, newSize, composite->desc}));

    const QTransform shift = pbCanvasCropShift(rect);
    for (int i = 0; i < m_layers.size(); ++i)
    {
        CanvasLayer &layer = m_layers[i];
        if (layer.raster.isNull())
            continue;
        const QTransform before = layer.transform();
        layer.setTransform(shift * before);
        const QTransform after = layer.transform();
        composite->children.push_back(std::make_unique<SetTransformCommand>(
            SetTransformCommand{layer.id, i, before, after, composite->desc}));
    }

    m_canvasSize = newSize;
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
    return composite;
}

/* ------------------------------------------------------- canonical render */

/* One layer through the canonical transform. `region` is the canvas region
 * whose top-left corner sits at target pixel (0,0). */
void DocumentRenderer::compositeLayer(QImage &target, const QRect &region,
                                      const CanvasLayer &layer)
{
    if (layer.raster.isNull() || !layer.visible)
        return;

    const QTransform tf = layer.transform();

    /* exact path: pure integer translation (the default for raster layers
     * and untransformed image/text layers) — blit the source sub-rect, no
     * resampling, pixel-identical to the old fast path */
    const bool exact = tf.type() <= QTransform::TxTranslate &&
                       tf.dx() == qreal(int(tf.dx())) &&
                       tf.dy() == qreal(int(tf.dy()));

    if (exact)
    {
        const QPoint off(int(tf.dx()), int(tf.dy()));
        const QRect srcRect = region.translated(-off);
        const QRect layerRect = layer.raster.rect();
        const QRect validSrc = srcRect.intersected(layerRect);
        if (validSrc.isEmpty())
            return;

        const QRect dstRect = validSrc.translated(-region.topLeft());
        const QRect srcLocal = validSrc.translated(-off);

        QImage layerPart(dstRect.size(), QImage::Format_ARGB32);
        layerPart.fill(Qt::transparent);
        {
            QPainter p(&layerPart);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(QPoint(0, 0), layer.raster, srcLocal);
            p.end();
        }

        if (!layer.mask.isNull())
        {
            QImage mask = layer.mask.scaled(layer.raster.size(),
                                            Qt::IgnoreAspectRatio,
                                            Qt::FastTransformation);
            QPainter p(&layerPart);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(QPoint(0, 0), maskToImage(mask), srcLocal);
            p.end();
        }

        QPainter p(&target);
        p.setOpacity(layer.opacity);
        p.setCompositionMode(toCompositionMode(layer.blendMode));
        p.drawImage(dstRect.topLeft(), layerPart);
        p.end();
        return;
    }

    /* transformed path: render the whole local content once with the
     * canonical center-pivot transform, then composite the part that lands
     * in this region */
    const QRect layerRect = layer.raster.rect();
    const QPointF contentCenter(layerRect.center());
    QImage layerPart(layerRect.size(), QImage::Format_ARGB32);
    layerPart.fill(Qt::transparent);
    {
        QPainter p(&layerPart);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(0, 0), layer.raster);
        if (!layer.mask.isNull())
        {
            QImage mask = layer.mask.scaled(layerRect.size(),
                                            Qt::IgnoreAspectRatio,
                                            Qt::FastTransformation);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(QPoint(0, 0), maskToImage(mask));
        }
        p.end();
    }

    /* transform around the content center — identical semantics to the
     * tile compositor, so screen and export cannot diverge */
    QTransform tf2;
    tf2.translate(contentCenter.x(), contentCenter.y());
    tf2.rotate(layer.rotation);
    tf2.scale(layer.scale, layer.scale);
    tf2.translate(-contentCenter.x(), -contentCenter.y());

    QImage drawn = layerPart.transformed(tf2, Qt::SmoothTransformation);

    /* The transformed image's (0,0) is the BOUNDING-BOX top-left of the
     * transformed content, not the transformed origin. Compute that offset
     * so the content lands exactly where layer.transform() maps it —
     * otherwise the placement disagrees with transform() by the bbox
     * shift (screen, export and the transform math would diverge). */
    QPointF bboxTL = tf2.map(QPointF(0, 0));
    const QPointF c1 = tf2.map(QPointF(layerRect.width(), 0));
    const QPointF c2 = tf2.map(QPointF(0, layerRect.height()));
    const QPointF c3 = tf2.map(QPointF(layerRect.width(), layerRect.height()));
    bboxTL.setX(qMin(qMin(bboxTL.x(), c1.x()), qMin(c2.x(), c3.x())));
    bboxTL.setY(qMin(qMin(bboxTL.y(), c1.y()), qMin(c2.y(), c3.y())));

    const QPointF drawAt = layer.position + bboxTL;

    const QRect drawRect(drawAt.toPoint(), drawn.size());
    const QRect visible = drawRect.intersected(region);
    if (visible.isEmpty())
        return;

    QImage tilePart(visible.size(), QImage::Format_ARGB32);
    tilePart.fill(Qt::transparent);
    {
        QPainter p(&tilePart);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(0, 0), drawn, visible.translated(-drawRect.topLeft()));
        p.end();
    }

    QPainter p(&target);
    p.setOpacity(layer.opacity);
    p.setCompositionMode(toCompositionMode(layer.blendMode));
    p.drawImage(visible.topLeft() - region.topLeft(), tilePart);
    p.end();
}

void DocumentRenderer::composite(QImage &target, const QRect &region,
                                 const PaintbrushDocument &doc)
{
    const auto &layers = doc.layers();
    for (const CanvasLayer &layer : layers)
        compositeLayer(target, region, layer);
}

QImage DocumentRenderer::render(const PaintbrushDocument &doc)
{
    const QSize size = doc.canvasSize();
    if (size.isEmpty())
        return {};
    return renderRegion(doc, QRect(QPoint(0, 0), size));
}

QImage DocumentRenderer::renderRegion(const PaintbrushDocument &doc,
                                      const QRect &region)
{
    if (region.isEmpty())
        return {};
    QImage result(region.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    composite(result, region, doc);
    return result;
}

QImage PaintbrushDocument::flattenedImage() const
{
    /* thin wrapper over the canonical renderer — identical transform
     * semantics as the screen/export/eyedropper paths */
    return DocumentRenderer::render(*this);
}

QImage PaintbrushDocument::aiSourceImage() const
{
    /* Prefer the captured media frame as the AI source so object selection
     * and ask-about-this operate on the true video pixels. */
    return flattenedImage();
}

} // namespace paintbrush