/*****************************************************************************
 * pb_export.cpp : NAI Paintbrush — export helpers
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_export.hpp"
#include "pb_document.hpp"

#include <QImageWriter>
#include <QFileInfo>
#include <QDir>
#include <QPainter>

namespace paintbrush {

static QString writeImage(const QImage &image, const QString &path,
                          const QString &format)
{
    QImageWriter writer(path);
    if (!format.isEmpty())
        writer.setFormat(format.toLower().toLatin1());
    writer.setQuality(92);
    if (!writer.write(image))
        return QStringLiteral("Export failed: %1").arg(writer.errorString());
    return {};
}

QString ExportService::exportImage(PaintbrushDocument *doc, const QString &path,
                                   const QString &format)
{
    return exportImageOptions(doc, path, format, 1.0, false);
}

QString ExportService::exportImageOptions(PaintbrushDocument *doc,
                                          const QString &path,
                                          const QString &format,
                                          double scale,
                                          bool cropToContent,
                                          const QColor &background)
{
    if (!doc)
        return QStringLiteral("No document to export.");
    /* the canonical renderer: export shows exactly what the screen shows */
    QImage flat = DocumentRenderer::render(*doc);
    if (flat.isNull())
        return QStringLiteral("The canvas is empty.");

    if (cropToContent)
    {
        /* trim fully-transparent margins so exports are tight to the art */
        int minX = flat.width(), minY = flat.height();
        int maxX = -1, maxY = -1;
        for (int y = 0; y < flat.height(); ++y)
        {
            const QRgb *line = reinterpret_cast<const QRgb *>(flat.constScanLine(y));
            for (int x = 0; x < flat.width(); ++x)
            {
                if (qAlpha(line[x]) != 0)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX >= 0)
            flat = flat.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
    }

    if (scale > 0.0 && !qFuzzyCompare(scale, 1.0))
        flat = flat.scaled(qMax(1, int(flat.width() * scale)),
                           qMax(1, int(flat.height() * scale)),
                           Qt::KeepAspectRatio, Qt::SmoothTransformation);

    const QFileInfo info(path);
    QString fmt = format;
    if (fmt.isEmpty())
        fmt = info.suffix();

    /* opaque formats get a background fill so alpha doesn't render black */
    const QString lower = fmt.toLower();
    if (background.alpha() > 0 &&
        (lower == QStringLiteral("jpg") || lower == QStringLiteral("jpeg")))
    {
        QImage opaque(flat.size(), QImage::Format_RGB32);
        opaque.fill(background);
        QPainter p(&opaque);
        p.drawImage(0, 0, flat);
        p.end();
        flat = opaque;
    }

    return writeImage(flat, path, fmt);
}

QString ExportService::exportImageFile(const QImage &image, const QString &path,
                                       const QString &format)
{
    if (image.isNull())
        return QStringLiteral("Nothing to export.");

    const QFileInfo info(path);
    QString fmt = format;
    if (fmt.isEmpty())
        fmt = info.suffix();
    return writeImage(image, path, fmt);
}

QString ExportService::exportMask(const QImage &mask, const QString &path)
{
    if (mask.isNull())
        return QStringLiteral("No mask to export.");
    const QImage gray = maskToImage(mask);
    return writeImage(gray, path, QStringLiteral("png"));
}

QString ExportService::exportCutout(const QImage &cutout, const QString &path)
{
    if (cutout.isNull())
        return QStringLiteral("No cutout to export.");

    /* trim transparent bounds */
    int minX = cutout.width(), minY = cutout.height();
    int maxX = -1, maxY = -1;
    for (int y = 0; y < cutout.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(cutout.constScanLine(y));
        for (int x = 0; x < cutout.width(); ++x)
        {
            if (qAlpha(line[x]) != 0)
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return QStringLiteral("The cutout is empty.");

    const QImage trimmed = cutout.copy(QRect(minX, minY,
                                             maxX - minX + 1, maxY - minY + 1));
    return writeImage(trimmed, path, QStringLiteral("png"));
}

} // namespace paintbrush