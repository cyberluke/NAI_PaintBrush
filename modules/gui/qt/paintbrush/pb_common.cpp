/*****************************************************************************
 * pb_common.cpp : NAI Paintbrush — shared helpers implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_common.hpp"

#include <QImageWriter>
#include <QSet>
#include <QDir>
#include <QStandardPaths>
#include <QProcessEnvironment>

namespace paintbrush {

QStringList supportedImageFormats()
{
    QSet<QString> formats;
    for (const QByteArray &ba : QImageWriter::supportedImageFormats())
        formats.insert(QString::fromLatin1(ba).toLower());

    QStringList list;
    for (const QString &f : { QStringLiteral("png"), QStringLiteral("jpg"),
                              QStringLiteral("jpeg"), QStringLiteral("webp") })
    {
        if (formats.contains(f))
            list << f;
    }
    return list;
}

QImage maskToImage(const QImage &mask)
{
    if (mask.isNull())
        return {};
    if (mask.format() == QImage::Format_Grayscale8)
        return mask;
    QImage gray(mask.size(), QImage::Format_Grayscale8);
    const int w = mask.width();
    const int h = mask.height();
    for (int y = 0; y < h; ++y) {
        uchar *dst = gray.scanLine(y);
        for (int x = 0; x < w; ++x)
            dst[x] = qGray(mask.pixel(x, y)) >= 128 ? 255 : 0;
    }
    return gray;
}

QImage applyMaskAsAlpha(const QImage &source, const QImage &mask)
{
    if (source.isNull() || mask.isNull())
        return {};

    QImage src = source.convertToFormat(QImage::Format_ARGB32);
    QImage m = maskToImage(mask).scaled(src.size(), Qt::IgnoreAspectRatio,
                                        Qt::FastTransformation);

    const int w = src.width();
    const int h = src.height();
    for (int y = 0; y < h; ++y) {
        QRgb *dst = reinterpret_cast<QRgb *>(src.scanLine(y));
        const uchar *mk = m.constScanLine(y);
        for (int x = 0; x < w; ++x)
            dst[x] = qRgba(qRed(dst[x]), qGreen(dst[x]), qBlue(dst[x]), mk[x]);
    }
    return src;
}

QImage toRgb888(const QImage &image)
{
    if (image.isNull())
        return {};
    return image.convertToFormat(QImage::Format_RGB888);
}

/* Moore-neighbor contour tracing. Follows the boundary of the foreground
 * (mask >= 128) region, producing a closed path of pixel corners. */
static QPainterPath traceContour(const QImage &mask, int startX, int startY)
{
    QPainterPath path;
    const int w = mask.width();
    const int h = mask.height();

    auto foreground = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;
        return qGray(mask.pixel(x, y)) >= 128;
    };

    /* find the top-most, left-most foreground pixel */
    int sx = -1, sy = -1;
    for (int y = 0; y < h && sx < 0; ++y)
        for (int x = 0; x < w; ++x)
            if (foreground(x, y)) { sx = x; sy = y; break; }
    if (sx < 0)
        return path;
    Q_UNUSED(startX); Q_UNUSED(startY);

    /* Moore neighborhood offsets (clockwise, starting from the pixel left
     * of the current one) */
    const int dirX[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    const int dirY[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };

    int bx = sx, by = sy;       /* boundary pixel (background) */
    int cx = sx, cy = sy;       /* current foreground pixel */
    int startDir = 4;           /* 4 = up from boundary pixel */
    QVector<QPoint> contour;

    int guard = 0;
    const int maxSteps = w * h * 4 + 16;
    while (guard++ < maxSteps)
    {
        contour.append(QPoint(cx, cy));
        bool found = false;
        int dir = startDir;
        for (int i = 0; i < 8; ++i)
        {
            const int nd = (dir + i) % 8;
            const int nx = cx + dirX[nd];
            const int ny = cy + dirY[nd];
            if (foreground(nx, ny))
            {
                bx = cx; by = cy;
                cx = nx; cy = ny;
                /* next search starts two steps back from the found dir */
                startDir = (nd + 6) % 8;
                found = true;
                break;
            }
        }
        if (!found)
            break; /* isolated pixel */
        if (cx == sx && cy == sy)
            break; /* closed the loop */
    }

    if (contour.size() < 3)
        return path;

    /* Build a path through the pixel corners, then shrink slightly so the
     * outline hugs the mask interior. */
    path.moveTo(contour[0].x() + 0.5, contour[0].y() + 0.5);
    for (int i = 1; i < contour.size(); ++i)
        path.lineTo(contour[i].x() + 0.5, contour[i].y() + 0.5);
    path.closeSubpath();
    return path;
}

QPainterPath maskToPath(const QImage &mask)
{
    if (mask.isNull())
        return {};
    QImage gray = maskToImage(mask);
    return traceContour(gray, 0, 0);
}

QString pbConfigString(vlc_object_t *obj, const char *name)
{
    char *value = var_InheritString(obj, name);
    if (!value)
        return {};
    QString result = QString::fromUtf8(value);
    free(value);
    return result;
}

bool pbConfigBool(vlc_object_t *obj, const char *name)
{
    return var_InheritBool(obj, name);
}

int pbConfigInt(vlc_object_t *obj, const char *name)
{
    return var_InheritInteger(obj, name);
}

QString pbApiKey(vlc_object_t *obj, const char *envVar, const char *configName)
{
    /* Environment takes precedence over config so keys never need to be
     * stored in the VLC config file. The value is never logged. */
    const QString env = QProcessEnvironment::systemEnvironment().value(
        QString::fromLatin1(envVar));
    if (!env.isEmpty())
        return env;
    return pbConfigString(obj, configName);
}

} // namespace paintbrush