/*****************************************************************************
 * pb_layer_model.hpp : NAI Paintbrush — layer list model for the inspector
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_LAYER_MODEL_HPP
#define PB_LAYER_MODEL_HPP

#include "pb_document.hpp"

#include <QAbstractListModel>
#include <QHash>

namespace paintbrush {

/* The inspector's layer list as a real QAbstractListModel (A38). Replaces
 * the old `ListView model: document.layerCount` int hack: every row binds
 * to actual layer state through named roles, so the visibility checkbox
 * reflects the real `visible` flag, the label shows the real `name`, and
 * the transform/opacity controls initialize from the layer's transform.
 *
 * The model watches the document's structural signals (layersChanged /
 * currentLayerChanged) and the controller's documentChanged() (emitted for
 * in-place mutations like visibility/opacity/transform toggles) and
 * reloads. Thumbnails are cached per layer id and regenerated only when
 * the raster contents actually change (QImage::cacheKey signature), so a
 * stroke commit does not re-encode unchanged layers. */
class PaintbrushLayerModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        NameRole,
        VisibleRole,
        OpacityRole,
        ScaleRole,
        RotationRole,
        /* Decomposed placement transform {scale, rotation} so QML can
         * initialize the transform spinboxes from the layer's transform. */
        TransformRole,
        /* Small PNG data URL of the layer raster ("" when no raster). */
        ThumbRole
    };

    /* `syncSource` is the object whose documentChanged() fires on in-place
     * layer mutations (the controller); connected by signal name so the
     * model stays independent of the controller type. */
    PaintbrushLayerModel(PaintbrushDocument *document, QObject *syncSource,
                         QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /* Scalar accessors for controls that address the current layer directly
     * (opacity slider, scale/rotation spinboxes) without a delegate row. */
    Q_INVOKABLE double opacityOf(int row) const;
    Q_INVOKABLE double scaleOf(int row) const;
    Q_INVOKABLE double rotationOf(int row) const;

public slots:
    void reload();

private:
    QString thumbnailFor(const CanvasLayer &layer) const;

    PaintbrushDocument *m_document = nullptr;
    /* layer id -> cached thumbnail data URL / content signature */
    mutable QHash<int, QString> m_thumbCache;
    mutable QHash<int, qint64> m_thumbSignature;
};

} // namespace paintbrush

#endif // PB_LAYER_MODEL_HPP