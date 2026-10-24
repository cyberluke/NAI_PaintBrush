/*****************************************************************************
 * pb_harvest.hpp : NAI Paintbrush — Harvest items and the Harvest Tray model
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_HARVEST_HPP
#define PB_HARVEST_HPP

#include "pb_media_logic.hpp"

#include <QAbstractListModel>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QByteArray>

namespace paintbrush {

/* A harvested moment (spec §5.3). Immutable creative source material once
 * created: editing on canvas creates layer state, never mutation of the
 * harvest asset. Keyed to the EXACT source frame (FrameRef/PTS) so Boost can
 * reason temporally (§26), find bridge poses in the real video (§27) and
 * re-segment or re-render later (§54). */
struct HarvestItem
{
    int id = 0;                     /* stable per-document id (HarvestId) */
    FrameRef sourceFrame;           /* mrl/title/ptsMs/width/height */
    QImage cutout;                  /* ARGB32, trimmed to sourceBounds (AssetRef) */
    QImage mask;                    /* Grayscale8 binary mask, sourceBounds-sized (MaskRef) */
    QRect sourceBounds;             /* cutout rect in frame coordinates */
    QString semanticLabel;          /* optional user/AI label */
    QString objectIdentity;         /* track/object identity ("track:3") */
    double segmentationScore = 0.0;
    bool favorite = false;
    QVariantMap metadata;           /* dominant palette, user label, ... */
    QString contentHash;            /* content-addressed asset key (§53) */

    bool isValid() const { return id > 0 && !cutout.isNull(); }
    /* Mask centroid in FRAME coordinates (motion inference, §29). */
    QPointF centroid() const;
    /* Fraction of sourceBounds covered by the mask (completeness score). */
    double maskCoverage() const;

    QVariantMap toManifest() const;   /* without pixel data */
    static HarvestItem fromManifest(const QVariantMap &m);
};

/* Build a HarvestItem from a decoded source frame + object mask. Trims to
 * the mask bounds, applies the mask as alpha, computes the content hash and
 * dominant palette metadata. Returns an invalid item when the mask is empty. */
HarvestItem makeHarvestItem(int id, const FrameRef &frame, const QImage &frameImage,
                            const QImage &objectMask, double score,
                            const QString &objectIdentity = QString());

/* The Harvest Tray (spec §5.4): a compact list of harvested moments. Exposed
 * to QML as a list model; shows thumbnails + timestamps only — never AI
 * diagnostics, mask morphology or transform controls. */
class HarvestTrayModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        ThumbnailRole,      /* data: URL PNG of the cutout (tray-sized) */
        TimeMsRole,
        TimeTextRole,       /* "00:04.20" */
        LabelRole,
        FavoriteRole,
        SourceTitleRole,
        SourceMrlRole,
        WidthRole,
        HeightRole,
        ScoreRole
    };

    explicit HarvestTrayModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_items.size(); }
    const QVector<HarvestItem> &items() const { return m_items; }
    const HarvestItem *itemById(int id) const;
    int indexOfId(int id) const;

    /* Append (takes a copy; the asset is immutable from here on). Returns the
     * assigned id. */
    int add(HarvestItem item);
    void remove(int id);
    void setFavorite(int id, bool favorite);
    void setLabel(int id, const QString &label);
    void clear();
    /* Replace the whole list (project load). */
    void setItems(const QVector<HarvestItem> &items);
    int nextId() const { return m_nextId; }
    void setNextId(int id) { m_nextId = qMax(1, id); }

    /* Items from the same source media sorted by PTS — the motion sequence
     * Boost reasons about (§26). */
    QVector<const HarvestItem *> sameSourceSequence(const QString &mrl) const;

    Q_INVOKABLE QString thumbnailDataUrl(int id) const;
    Q_INVOKABLE QString timeText(int id) const;

signals:
    void countChanged();
    void itemsChanged();

private:
    QVector<HarvestItem> m_items;
    int m_nextId = 1;
};

/* "00:04.20" style timestamp used by the tray. */
QString harvestTimeText(qint64 ms);

} // namespace paintbrush

#endif // PB_HARVEST_HPP
