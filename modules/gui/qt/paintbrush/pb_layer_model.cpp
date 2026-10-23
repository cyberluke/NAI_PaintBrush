/*****************************************************************************
 * pb_layer_model.cpp : NAI Paintbrush — layer list model implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_layer_model.hpp"

#include <QBuffer>
#include <QPainter>

namespace paintbrush {

PaintbrushLayerModel::PaintbrushLayerModel(PaintbrushDocument *document,
                                           QObject *syncSource,
                                           QObject *parent)
    : QAbstractListModel(parent)
    , m_document(document)
{
    connect(m_document, &PaintbrushDocument::layersChanged,
            this, &PaintbrushLayerModel::reload);
    connect(m_document, &PaintbrushDocument::currentLayerChanged,
            this, &PaintbrushLayerModel::reload);
    /* In-place mutations (visibility / opacity / transform toggles) mutate
     * the layer fields directly and emit the controller's documentChanged()
     * instead of a document signal; connect by name to stay independent of
     * the controller type. */
    connect(syncSource, SIGNAL(documentChanged()),
            this, SLOT(reload()));
}

int PaintbrushLayerModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document)
        return 0;
    return m_document->layerCount();
}

QVariant PaintbrushLayerModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_document)
        return {};
    const CanvasLayer *layer = m_document->layerAt(index.row());
    if (!layer)
        return {};

    switch (role)
    {
    case IdRole:
        return layer->id;
    case NameRole:
        return layer->name;
    case VisibleRole:
        return layer->visible;
    case OpacityRole:
        return double(layer->opacity);
    case ScaleRole:
        return double(layer->scale);
    case RotationRole:
        return double(layer->rotation);
    case TransformRole:
    {
        QVariantMap transform;
        transform.insert(QStringLiteral("scale"), double(layer->scale));
        transform.insert(QStringLiteral("rotation"), double(layer->rotation));
        return transform;
    }
    case ThumbRole:
        return thumbnailFor(*layer);
    default:
        return {};
    }
}

QHash<int, QByteArray> PaintbrushLayerModel::roleNames() const
{
    QHash<int, QByteArray> names;
    names.insert(IdRole, "id");
    names.insert(NameRole, "name");
    names.insert(VisibleRole, "visible");
    names.insert(OpacityRole, "opacity");
    names.insert(ScaleRole, "scale");
    names.insert(RotationRole, "rotation");
    names.insert(TransformRole, "transform");
    names.insert(ThumbRole, "thumb");
    return names;
}

double PaintbrushLayerModel::opacityOf(int row) const
{
    const CanvasLayer *layer = m_document ? m_document->layerAt(row) : nullptr;
    return layer ? double(layer->opacity) : 1.0;
}

double PaintbrushLayerModel::scaleOf(int row) const
{
    const CanvasLayer *layer = m_document ? m_document->layerAt(row) : nullptr;
    return layer ? double(layer->scale) : 1.0;
}

double PaintbrushLayerModel::rotationOf(int row) const
{
    const CanvasLayer *layer = m_document ? m_document->layerAt(row) : nullptr;
    return layer ? double(layer->rotation) : 0.0;
}

void PaintbrushLayerModel::reload()
{
    beginResetModel();
    endResetModel();
}

QString PaintbrushLayerModel::thumbnailFor(const CanvasLayer &layer) const
{
    if (layer.raster.isNull())
        return {};

    /* Regenerate only when the raster (or its mask) actually changed;
     * cacheKey() changes whenever the pixel contents change. */
    const qint64 signature = qint64(layer.raster.cacheKey())
                           + qint64(layer.mask.cacheKey());
    const QString cached = m_thumbCache.value(layer.id);
    if (!cached.isEmpty() && m_thumbSignature.value(layer.id) == signature)
        return cached;

    QImage thumb = layer.raster.scaled(64, 48, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
    if (thumb.isNull())
        return {};

    /* paint the transparency checkerboard-lite: flat neutral backdrop so
     * transparent rasters read clearly in the list */
    QImage framed(64, 48, QImage::Format_ARGB32);
    framed.fill(QColor(0x3a, 0x3a, 0x3e));
    {
        QPainter p(&framed);
        p.drawImage((64 - thumb.width()) / 2, (48 - thumb.height()) / 2, thumb);
        p.end();
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    framed.save(&buffer, "PNG");
    buffer.close();

    const QString url = QStringLiteral("data:image/png;base64,")
                      + QString::fromLatin1(bytes.toBase64());
    m_thumbCache.insert(layer.id, url);
    m_thumbSignature.insert(layer.id, signature);
    return url;
}

} // namespace paintbrush