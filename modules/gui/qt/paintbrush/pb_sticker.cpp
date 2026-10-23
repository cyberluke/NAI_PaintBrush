/*****************************************************************************
 * pb_sticker.cpp : NAI Paintbrush — sticker asset rendering and frame cache
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_sticker.hpp"

#include <QPainter>
#include <QBuffer>
#include <QtMath>

namespace paintbrush {

/* ------------------------------------------------------------------ utils */

QImage trimToContent(const QImage &image, QPoint *offset)
{
    if (image.isNull())
    {
        if (offset)
            *offset = QPoint(0, 0);
        return {};
    }
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    int minX = argb.width(), minY = argb.height();
    int maxX = -1, maxY = -1;
    for (int y = 0; y < argb.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            if (qAlpha(line[x]) != 0)
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
    }
    if (maxX < 0)
    {
        if (offset)
            *offset = QPoint(0, 0);
        return {};
    }
    if (offset)
        *offset = QPoint(minX, minY);
    return argb.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
}

/* ------------------------------------------------------------------ render */

QImage renderStickerFrame(const QImage &frame, const QImage &mask, int outline,
                          const QColor &outlineColor, bool shadow, double scale,
                          const TrackEffect *effect)
{
    if (frame.isNull())
        return {};

    QImage maskScaled = mask.isNull() ? QImage() : mask;
    if (!maskScaled.isNull() && maskScaled.size() != frame.size())
        maskScaled = maskScaled.scaled(frame.size(), Qt::IgnoreAspectRatio,
                                       Qt::FastTransformation);
    if (!maskScaled.isNull())
        maskScaled = maskScaled.convertToFormat(QImage::Format_Grayscale8);

    QImage cutout(frame.size(), QImage::Format_ARGB32);
    cutout.fill(Qt::transparent);
    {
        QPainter p(&cutout);
        p.drawImage(0, 0, frame);
        if (!maskScaled.isNull())
        {
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, maskScaled);
        }
        p.end();
    }

    /* -- track effect pass (applied to the masked cutout) ------------------ */
    if (effect != nullptr && effect->type != TrackEffect::None)
    {
        const float t = qBound(0.f, float(effect->intensity) / 100.f, 1.f);
        switch (effect->type)
        {
        case TrackEffect::Blur:
        {
            /* soften the cutout: blur the mask-shaped cutout */
            QImage blurred = cutout;
            const int radius = qMax(1, int(t * 10.f));
            for (int pass = 0; pass < radius; ++pass)
            {
                QImage tmp = blurred;
                QPainter bp(&tmp);
                bp.setRenderHint(QPainter::SmoothPixmapTransform);
                bp.drawImage(0, 0, blurred.scaled(qMax(1, blurred.width() / 2),
                                                  qMax(1, blurred.height() / 2),
                                                  Qt::IgnoreAspectRatio,
                                                  Qt::SmoothTransformation)
                                                  .scaled(blurred.size()));
                bp.end();
                blurred = tmp;
            }
            cutout = blurred;
            break;
        }
        case TrackEffect::Dim:
        {
            QPainter p(&cutout);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(cutout.rect(),
                       QColor(0, 0, 0, int(255 * t * 0.65f)));
            p.end();
            break;
        }
        case TrackEffect::Spotlight:
        {
            /* brighten the cutout (saturate toward white) */
            QImage bright = cutout;
            for (int y = 0; y < bright.height(); ++y)
            {
                QRgb *line = reinterpret_cast<QRgb *>(bright.scanLine(y));
                for (int x = 0; x < bright.width(); ++x)
                {
                    const QRgb px = line[x];
                    if (qAlpha(px) == 0)
                        continue;
                    const int a = qAlpha(px);
                    const int r = qMin(255, qRed(px) + int(96 * t));
                    const int g = qMin(255, qGreen(px) + int(96 * t));
                    const int b = qMin(255, qBlue(px) + int(96 * t));
                    line[x] = qRgba(r, g, b, a);
                }
            }
            cutout = bright;
            break;
        }
        case TrackEffect::Outline:
        {
            QColor oc = effect->color.isEmpty()
                            ? QColor(255, 255, 255)
                            : QColor(effect->color);
            if (!oc.isValid())
                oc = QColor(255, 255, 255);
            const int width = qMax(1, int(t * 12.f) + 1);
            QImage alpha = cutout.convertToFormat(QImage::Format_Alpha8);
            QImage gray = alpha.convertToFormat(QImage::Format_Grayscale8);
            QPainterPath path = maskToPath(gray);
            QImage result(cutout.size(), QImage::Format_ARGB32);
            result.fill(Qt::transparent);
            QPainter p(&result);
            p.setPen(QPen(oc, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(oc);
            p.drawPath(path);
            p.end();
            QPainter c(&result);
            c.drawImage(0, 0, cutout);
            c.end();
            cutout = result;
            break;
        }
        case TrackEffect::Attach:
        {
            /* composite the cutout back over the source frame at the mask
             * position: the result is the full frame with the object
             * re-attached (and the rest of the frame preserved). */
            QImage composited = frame;
            QPainter p(&composited);
            p.drawImage(0, 0, cutout);
            p.end();
            cutout = composited;
            break;
        }
        default:
            break;
        }
    }

    /* trim to non-empty bounds */
    QPoint trimOffset;
    QImage trimmed = trimToContent(cutout, &trimOffset);
    Q_UNUSED(trimOffset);
    if (trimmed.isNull())
        return {};

    if (scale != 1.0)
    {
        const QSize scaledSize(qMax(1, int(trimmed.width() * scale)),
                               qMax(1, int(trimmed.height() * scale)));
        trimmed = trimmed.scaled(scaledSize, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
    }

    if (outline <= 0 && !shadow)
        return trimmed;

    /* shadow: dark silhouette offset below-right */
    QImage result(trimmed.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    {
        QPainter p(&result);
        if (shadow)
        {
            QImage shadowMask(trimmed.size(), QImage::Format_ARGB32);
            shadowMask.fill(Qt::transparent);
            QImage alpha = trimmed.convertToFormat(QImage::Format_Alpha8);
            QPainterPath path;
            QImage gray = alpha.convertToFormat(QImage::Format_Grayscale8);
            path = maskToPath(gray);
            QPainter sp(&shadowMask);
            sp.setPen(QPen(QColor(0, 0, 0, 160), 6, Qt::SolidLine,
                           Qt::RoundCap, Qt::RoundJoin));
            sp.setBrush(QColor(0, 0, 0, 160));
            sp.drawPath(path);
            sp.end();
            p.drawImage(QPoint(4, 4), shadowMask);
        }
        if (outline > 0)
        {
            QPainterPath path;
            QImage alpha = trimmed.convertToFormat(QImage::Format_Alpha8);
            QImage gray = alpha.convertToFormat(QImage::Format_Grayscale8);
            path = maskToPath(gray);
            QPainter op(&result);
            op.setPen(QPen(outlineColor, outline * 2 + 1, Qt::SolidLine,
                           Qt::RoundCap, Qt::RoundJoin));
            op.setBrush(outlineColor);
            op.drawPath(path);
            op.end();
        }
        p.drawImage(0, 0, trimmed);
        p.end();
    }
    return result;
}

QString stickerPreviewDataUrl(const QImage &frame, const QImage &mask,
                              const TrackEffect *effect, int outline,
                              const QColor &outlineColor, bool shadow,
                              double scale)
{
    if (frame.isNull() || mask.isNull())
        return {};
    const QImage rendered = renderStickerFrame(frame, mask, outline,
                                               outlineColor, shadow, scale,
                                               effect);
    if (rendered.isNull())
        return {};
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!rendered.save(&buffer, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,%1")
        .arg(QString::fromLatin1(png.toBase64()));
}

/* ------------------------------------------------------------- frame cache */

bool StickerFrameCache::get(int trackId, int frameIndex, Entry *out) const
{
    const quint64 k = key(trackId, frameIndex);
    auto it = m_entries.constFind(k);
    if (it == m_entries.constEnd())
        return false;
    if (out)
        *out = it.value();
    /* touch LRU order (m_order is mutable through the const method) */
    m_order.removeAll(k);
    m_order.prepend(k);
    return true;
}

void StickerFrameCache::put(int trackId, int frameIndex, qint64 timeMs,
                            const QImage &frame)
{
    if (frame.isNull())
        return;
    const quint64 k = key(trackId, frameIndex);
    const auto it = m_entries.constFind(k);
    if (it != m_entries.constEnd())
    {
        m_bytes -= qint64(it->frame.sizeInBytes());
        m_entries.erase(it);
    }
    Entry e;
    e.timeMs = timeMs;
    e.frame = frame;
    m_entries.insert(k, e);
    m_bytes += qint64(frame.sizeInBytes());
    m_order.removeAll(k);
    m_order.prepend(k);
    evict();
}

void StickerFrameCache::remove(int trackId, int frameIndex)
{
    const quint64 k = key(trackId, frameIndex);
    const auto it = m_entries.find(k);
    if (it == m_entries.end())
        return;
    m_bytes -= qint64(it->frame.sizeInBytes());
    m_entries.erase(it);
    m_order.removeAll(k);
}

void StickerFrameCache::clear()
{
    m_entries.clear();
    m_order.clear();
    m_bytes = 0;
}

void StickerFrameCache::evict()
{
    while (m_bytes > m_budget && !m_order.isEmpty())
    {
        const quint64 k = m_order.takeLast();
        const auto it = m_entries.find(k);
        if (it != m_entries.end())
        {
            m_bytes -= qint64(it->frame.sizeInBytes());
            m_entries.erase(it);
        }
    }
}

} // namespace paintbrush