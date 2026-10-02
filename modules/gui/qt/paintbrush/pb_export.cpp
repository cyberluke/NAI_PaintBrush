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
    if (!doc)
        return QStringLiteral("No document to export.");
    const QImage flat = doc->flattenedImage();
    if (flat.isNull())
        return QStringLiteral("The canvas is empty.");

    const QFileInfo info(path);
    QString fmt = format;
    if (fmt.isEmpty())
        fmt = info.suffix();
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