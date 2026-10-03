/*****************************************************************************
 * pb_document.cpp : NAI Paintbrush — document model implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_document.hpp"

#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>

namespace paintbrush {

PaintbrushDocument::PaintbrushDocument(QObject *parent)
    : QObject(parent)
{
}

void PaintbrushDocument::resetDocument(const QSize &size)
{
    m_canvasSize = size.isValid() && !size.isEmpty() ? size : QSize(1280, 720);
    m_layers.clear();
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

    const QSize oldSize = m_canvasSize;
    m_canvasSize = size;
    for (CanvasLayer &layer : m_layers) {
        if (layer.raster.isNull())
            continue;
        QImage resized = QImage(size, QImage::Format_ARGB32);
        resized.fill(Qt::transparent);
        QPainter p(&resized);
        p.drawImage(QPoint(0, 0), layer.raster);
        p.end();
        layer.raster = std::move(resized);
        if (!layer.mask.isNull()) {
            QImage mask(size, QImage::Format_Grayscale8);
            mask.fill(0);
            QPainter mp(&mask);
            mp.drawImage(0, 0, layer.mask);
            mp.end();
            layer.mask = std::move(mask);
        }
    }
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
    Q_UNUSED(oldSize);
}

void PaintbrushDocument::setLayers(const QList<CanvasLayer> &layers)
{
    m_layers = layers;
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

QByteArray PaintbrushDocument::layersSnapshot() const
{
    /* Compact JSON: names, types, flags and PNG-encoded raster per layer.
     * See ProjectStore for the canonical format; this is intentionally the
     * same layer serialization so undo snapshots and project files agree. */
    QJsonArray array;
    for (const CanvasLayer &layer : m_layers) {
        QJsonObject obj;
        obj[QStringLiteral("name")] = layer.name;
        obj[QStringLiteral("type")] = layer.type;
        obj[QStringLiteral("visible")] = layer.visible;
        obj[QStringLiteral("locked")] = layer.locked;
        obj[QStringLiteral("alphaLocked")] = layer.alphaLocked;
        obj[QStringLiteral("opacity")] = layer.opacity;
        obj[QStringLiteral("blend")] = layer.blendMode;
        obj[QStringLiteral("text")] = layer.text;
        obj[QStringLiteral("posX")] = layer.position.x();
        obj[QStringLiteral("posY")] = layer.position.y();
        obj[QStringLiteral("scale")] = layer.scale;
        obj[QStringLiteral("rotation")] = layer.rotation;

        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        layer.raster.save(&buffer, "PNG");
        obj[QStringLiteral("image")] = QString::fromLatin1(png.toBase64());

        if (!layer.mask.isNull()) {
            QByteArray maskPng;
            QBuffer maskBuffer(&maskPng);
            maskBuffer.open(QIODevice::WriteOnly);
            layer.mask.save(&maskBuffer, "PNG");
            obj[QStringLiteral("mask")] = QString::fromLatin1(maskPng.toBase64());
        }
        array.append(obj);
    }

    QJsonDocument doc(array);
    return doc.toJson(QJsonDocument::Compact);
}

void PaintbrushDocument::restoreLayersSnapshot(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return;

    QList<CanvasLayer> layers;
    const QJsonArray array = doc.array();
    for (const QJsonValue &value : array) {
        const QJsonObject obj = value.toObject();
        CanvasLayer layer;
        layer.name = obj[QStringLiteral("name")].toString();
        layer.type = obj[QStringLiteral("type")].toInt(LAYER_RASTER);
        layer.visible = obj[QStringLiteral("visible")].toBool(true);
        layer.locked = obj[QStringLiteral("locked")].toBool(false);
        layer.alphaLocked = obj[QStringLiteral("alphaLocked")].toBool(false);
        layer.opacity = obj[QStringLiteral("opacity")].toDouble(1.0);
        layer.blendMode = obj[QStringLiteral("blend")].toInt(BLEND_NORMAL);
        layer.text = obj[QStringLiteral("text")].toString();
        layer.position = QPointF(obj[QStringLiteral("posX")].toDouble(0),
                                 obj[QStringLiteral("posY")].toDouble(0));
        layer.scale = float(obj[QStringLiteral("scale")].toDouble(1.0));
        layer.rotation = float(obj[QStringLiteral("rotation")].toDouble(0.0));

        const QByteArray png = QByteArray::fromBase64(
            obj[QStringLiteral("image")].toString().toLatin1());
        if (!png.isEmpty())
            layer.raster.loadFromData(png, "PNG");

        const QByteArray maskPng = QByteArray::fromBase64(
            obj[QStringLiteral("mask")].toString().toLatin1());
        if (!maskPng.isEmpty())
            layer.mask.loadFromData(maskPng, "PNG");

        layers.append(std::move(layer));
    }
    setLayers(layers);
}

QByteArray PaintbrushDocument::canvasSnapshot() const
{
    QJsonObject obj;
    obj[QStringLiteral("width")] = m_canvasSize.width();
    obj[QStringLiteral("height")] = m_canvasSize.height();
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

void PaintbrushDocument::restoreCanvasSnapshot(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return;
    const QJsonObject obj = doc.object();
    const int w = obj[QStringLiteral("width")].toInt(m_canvasSize.width());
    const int h = obj[QStringLiteral("height")].toInt(m_canvasSize.height());
    setCanvasSize(QSize(w, h));
}

QImage PaintbrushDocument::flattenedImage() const
{
    if (m_layers.isEmpty())
        return {};

    QImage result(m_canvasSize, QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    QPainter painter(&result);

    for (const CanvasLayer &layer : m_layers)
    {
        if (!layer.visible || layer.raster.isNull())
            continue;

        /* draw the layer into its own buffer so the layer mask can be
         * applied before blending with the composition mode */
        QImage layerImage(m_canvasSize, QImage::Format_ARGB32);
        layerImage.fill(Qt::transparent);
        {
            QPainter lp(&layerImage);
            if (layer.type == LAYER_IMAGE || layer.type == LAYER_TEXT)
                lp.drawImage(layer.position, layer.raster);
            else
                lp.drawImage(0, 0, layer.raster);
            lp.end();
        }

        if (!layer.mask.isNull())
        {
            QPainter mp(&layerImage);
            mp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            mp.drawImage(0, 0, maskToImage(layer.mask));
            mp.end();
        }

        painter.setOpacity(layer.opacity);
        painter.setCompositionMode(toCompositionMode(layer.blendMode));
        painter.drawImage(0, 0, layerImage);
    }
    painter.end();
    return result;
}

QImage PaintbrushDocument::aiSourceImage() const
{
    /* Prefer the captured media frame as the AI source so object selection
     * and ask-about-this operate on the true video pixels. */
    return flattenedImage();
}

} // namespace paintbrush