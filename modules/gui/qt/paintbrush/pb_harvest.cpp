/*****************************************************************************
 * pb_harvest.cpp : NAI Paintbrush — Harvest items and the Harvest Tray model
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_harvest.hpp"
#include "pb_common.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QPainter>
#include <algorithm>

namespace paintbrush {

/* ------------------------------------------------------------- HarvestItem */

QPointF HarvestItem::centroid() const
{
    if (mask.isNull())
        return QPointF(sourceBounds.center());
    double sx = 0.0, sy = 0.0;
    qint64 n = 0;
    for (int y = 0; y < mask.height(); ++y)
    {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
        {
            if (row[x] >= 128)
            {
                sx += x;
                sy += y;
                ++n;
            }
        }
    }
    if (n == 0)
        return QPointF(sourceBounds.center());
    return QPointF(sourceBounds.x() + sx / double(n), sourceBounds.y() + sy / double(n));
}

double HarvestItem::maskCoverage() const
{
    if (mask.isNull() || mask.width() == 0 || mask.height() == 0)
        return 0.0;
    qint64 n = 0;
    for (int y = 0; y < mask.height(); ++y)
    {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            if (row[x] >= 128)
                ++n;
    }
    return double(n) / double(qint64(mask.width()) * mask.height());
}

QVariantMap HarvestItem::toManifest() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("sourceMRL"), sourceFrame.mrl);
    m.insert(QStringLiteral("sourceTitle"), sourceFrame.title);
    m.insert(QStringLiteral("sourceTimeMs"), double(sourceFrame.ptsMs));
    m.insert(QStringLiteral("frameWidth"), sourceFrame.width);
    m.insert(QStringLiteral("frameHeight"), sourceFrame.height);
    m.insert(QStringLiteral("boundsX"), sourceBounds.x());
    m.insert(QStringLiteral("boundsY"), sourceBounds.y());
    m.insert(QStringLiteral("boundsW"), sourceBounds.width());
    m.insert(QStringLiteral("boundsH"), sourceBounds.height());
    m.insert(QStringLiteral("label"), semanticLabel);
    m.insert(QStringLiteral("identity"), objectIdentity);
    m.insert(QStringLiteral("score"), segmentationScore);
    m.insert(QStringLiteral("favorite"), favorite);
    m.insert(QStringLiteral("hash"), contentHash);
    m.insert(QStringLiteral("metadata"), metadata);
    return m;
}

HarvestItem HarvestItem::fromManifest(const QVariantMap &m)
{
    HarvestItem h;
    h.id = m.value(QStringLiteral("id")).toInt();
    h.sourceFrame.mrl = m.value(QStringLiteral("sourceMRL")).toString();
    h.sourceFrame.title = m.value(QStringLiteral("sourceTitle")).toString();
    h.sourceFrame.ptsMs = qint64(m.value(QStringLiteral("sourceTimeMs")).toDouble());
    h.sourceFrame.width = m.value(QStringLiteral("frameWidth")).toInt();
    h.sourceFrame.height = m.value(QStringLiteral("frameHeight")).toInt();
    h.sourceBounds = QRect(m.value(QStringLiteral("boundsX")).toInt(),
                           m.value(QStringLiteral("boundsY")).toInt(),
                           m.value(QStringLiteral("boundsW")).toInt(),
                           m.value(QStringLiteral("boundsH")).toInt());
    h.semanticLabel = m.value(QStringLiteral("label")).toString();
    h.objectIdentity = m.value(QStringLiteral("identity")).toString();
    h.segmentationScore = m.value(QStringLiteral("score")).toDouble();
    h.favorite = m.value(QStringLiteral("favorite")).toBool();
    h.contentHash = m.value(QStringLiteral("hash")).toString();
    h.metadata = m.value(QStringLiteral("metadata")).toMap();
    return h;
}

static QRect maskBounds(const QImage &mask)
{
    int minX = mask.width(), minY = mask.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < mask.height(); ++y)
    {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
        {
            if (row[x] >= 128)
            {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

static QVariantList dominantPalette(const QImage &cutout, int count)
{
    /* coarse 4-bit histogram of opaque pixels; cheap and deterministic */
    QHash<quint32, int> hist;
    for (int y = 0; y < cutout.height(); y += 2)
    {
        const QRgb *row = reinterpret_cast<const QRgb *>(cutout.constScanLine(y));
        for (int x = 0; x < cutout.width(); x += 2)
        {
            if (qAlpha(row[x]) < 128)
                continue;
            const quint32 key = ((qRed(row[x]) >> 4) << 8) | ((qGreen(row[x]) >> 4) << 4) |
                                (qBlue(row[x]) >> 4);
            hist[key]++;
        }
    }
    QVector<QPair<int, quint32>> sorted;
    for (auto it = hist.cbegin(); it != hist.cend(); ++it)
        sorted.append({ it.value(), it.key() });
    std::sort(sorted.begin(), sorted.end(),
              [](const QPair<int, quint32> &a, const QPair<int, quint32> &b)
              { return a.first > b.first; });
    QVariantList out;
    for (int i = 0; i < sorted.size() && i < count; ++i)
    {
        const quint32 k = sorted[i].second;
        out.append(QColor(((k >> 8) & 0xF) * 17, ((k >> 4) & 0xF) * 17, (k & 0xF) * 17).name());
    }
    return out;
}

HarvestItem makeHarvestItem(int id, const FrameRef &frame, const QImage &frameImage,
                            const QImage &objectMask, double score,
                            const QString &objectIdentity)
{
    HarvestItem h;
    if (frameImage.isNull() || objectMask.isNull())
        return h;
    QImage gray = maskToImage(objectMask);
    if (gray.size() != frameImage.size())
        gray = gray.scaled(frameImage.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const QRect bounds = maskBounds(gray);
    if (bounds.isEmpty())
        return h;

    const QImage rgb = frameImage.convertToFormat(QImage::Format_ARGB32);
    QImage cut(bounds.size(), QImage::Format_ARGB32);
    cut.fill(Qt::transparent);
    for (int y = 0; y < bounds.height(); ++y)
    {
        const QRgb *src = reinterpret_cast<const QRgb *>(rgb.constScanLine(bounds.y() + y));
        const uchar *m = gray.constScanLine(bounds.y() + y);
        QRgb *dst = reinterpret_cast<QRgb *>(cut.scanLine(y));
        for (int x = 0; x < bounds.width(); ++x)
        {
            const int fx = bounds.x() + x;
            const int a = m[fx]; /* soft masks keep their edge alpha */
            dst[x] = qRgba(qRed(src[fx]), qGreen(src[fx]), qBlue(src[fx]), a);
        }
    }

    h.id = id;
    h.sourceFrame = frame;
    h.sourceFrame.width = frameImage.width();
    h.sourceFrame.height = frameImage.height();
    h.cutout = cut;
    h.mask = gray.copy(bounds);
    h.sourceBounds = bounds;
    h.segmentationScore = score;
    h.objectIdentity = objectIdentity;
    h.metadata.insert(QStringLiteral("palette"), dominantPalette(cut, 5));
    h.metadata.insert(QStringLiteral("coverage"), h.maskCoverage());

    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(cut.constBits()),
                                qsizetype(cut.sizeInBytes())));
    hash.addData(frame.mrl.toUtf8());
    hash.addData(QByteArray::number(qlonglong(frame.ptsMs)));
    h.contentHash = QString::fromLatin1(hash.result().toHex().left(16));
    return h;
}

/* -------------------------------------------------------------- utilities */

QString harvestTimeText(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 totalSec = ms / 1000;
    const qint64 hundredths = (ms % 1000) / 10;
    const qint64 min = totalSec / 60;
    const qint64 sec = totalSec % 60;
    return QStringLiteral("%1:%2.%3")
        .arg(min, 2, 10, QLatin1Char('0'))
        .arg(sec, 2, 10, QLatin1Char('0'))
        .arg(hundredths, 2, 10, QLatin1Char('0'));
}

static QString imageDataUrl(const QImage &img, int maxEdge)
{
    if (img.isNull())
        return QString();
    QImage t = img;
    if (t.width() > maxEdge || t.height() > maxEdge)
        t = t.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    t.save(&buf, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

/* --------------------------------------------------------- HarvestTrayModel */

HarvestTrayModel::HarvestTrayModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int HarvestTrayModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant HarvestTrayModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const HarvestItem &h = m_items.at(index.row());
    switch (role)
    {
    case IdRole: return h.id;
    case ThumbnailRole: return imageDataUrl(h.cutout, 160);
    case TimeMsRole: return double(h.sourceFrame.ptsMs);
    case TimeTextRole: return harvestTimeText(h.sourceFrame.ptsMs);
    case LabelRole: return h.semanticLabel;
    case FavoriteRole: return h.favorite;
    case SourceTitleRole: return h.sourceFrame.title;
    case SourceMrlRole: return h.sourceFrame.mrl;
    case WidthRole: return h.cutout.width();
    case HeightRole: return h.cutout.height();
    case ScoreRole: return h.segmentationScore;
    case Qt::DisplayRole: return harvestTimeText(h.sourceFrame.ptsMs);
    default: return {};
    }
}

QHash<int, QByteArray> HarvestTrayModel::roleNames() const
{
    return {
        { IdRole, "harvestId" },
        { ThumbnailRole, "thumbnail" },
        { TimeMsRole, "timeMs" },
        { TimeTextRole, "timeText" },
        { LabelRole, "label" },
        { FavoriteRole, "favorite" },
        { SourceTitleRole, "sourceTitle" },
        { SourceMrlRole, "sourceMrl" },
        { WidthRole, "cutoutWidth" },
        { HeightRole, "cutoutHeight" },
        { ScoreRole, "score" },
    };
}

const HarvestItem *HarvestTrayModel::itemById(int id) const
{
    for (const HarvestItem &h : m_items)
        if (h.id == id)
            return &h;
    return nullptr;
}

int HarvestTrayModel::indexOfId(int id) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == id)
            return i;
    return -1;
}

int HarvestTrayModel::add(HarvestItem item)
{
    if (item.id <= 0)
        item.id = m_nextId;
    m_nextId = qMax(m_nextId, item.id + 1);
    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append(item);
    endInsertRows();
    emit countChanged();
    emit itemsChanged();
    return item.id;
}

void HarvestTrayModel::remove(int id)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    beginRemoveRows(QModelIndex(), i, i);
    m_items.removeAt(i);
    endRemoveRows();
    emit countChanged();
    emit itemsChanged();
}

void HarvestTrayModel::setFavorite(int id, bool favorite)
{
    const int i = indexOfId(id);
    if (i < 0 || m_items[i].favorite == favorite)
        return;
    m_items[i].favorite = favorite;
    emit dataChanged(index(i), index(i), { FavoriteRole });
    emit itemsChanged();
}

void HarvestTrayModel::setLabel(int id, const QString &label)
{
    const int i = indexOfId(id);
    if (i < 0 || m_items[i].semanticLabel == label)
        return;
    m_items[i].semanticLabel = label;
    emit dataChanged(index(i), index(i), { LabelRole });
    emit itemsChanged();
}

void HarvestTrayModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
    emit countChanged();
    emit itemsChanged();
}

void HarvestTrayModel::setItems(const QVector<HarvestItem> &items)
{
    beginResetModel();
    m_items = items;
    for (const HarvestItem &h : m_items)
        m_nextId = qMax(m_nextId, h.id + 1);
    endResetModel();
    emit countChanged();
    emit itemsChanged();
}

QVector<const HarvestItem *> HarvestTrayModel::sameSourceSequence(const QString &mrl) const
{
    QVector<const HarvestItem *> out;
    for (const HarvestItem &h : m_items)
        if (h.sourceFrame.mrl == mrl)
            out.append(&h);
    std::sort(out.begin(), out.end(), [](const HarvestItem *a, const HarvestItem *b)
              { return a->sourceFrame.ptsMs < b->sourceFrame.ptsMs; });
    return out;
}

QString HarvestTrayModel::thumbnailDataUrl(int id) const
{
    const HarvestItem *h = itemById(id);
    return h ? imageDataUrl(h->cutout, 160) : QString();
}

QString HarvestTrayModel::timeText(int id) const
{
    const HarvestItem *h = itemById(id);
    return h ? harvestTimeText(h->sourceFrame.ptsMs) : QString();
}

} // namespace paintbrush
