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
#include <QRadialGradient>
#include <QHash>
#include <QLineF>

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

/* ---------------------------------------------------------------- dabs */

void stampDab(QImage &target, const QPointF &center, const DabSpec &spec)
{
    if (target.isNull() || spec.radius <= 0.0 || spec.opacity <= 0.0)
        return;

    const qreal r = qMax<qreal>(0.5, spec.radius);
    QPainter p(&target);
    p.setRenderHint(QPainter::Antialiasing, r > 2.0);
    p.setCompositionMode(spec.mode);
    p.setPen(Qt::NoPen);

    if (spec.mode == QPainter::CompositionMode_Clear)
    {
        /* eraser: color is irrelevant */
        p.setBrush(Qt::black);
        p.drawEllipse(center, r, r);
        p.end();
        return;
    }

    QColor c = spec.color;
    c.setAlphaF(qBound<qreal>(0.0, c.alphaF() * spec.opacity, 1.0));

    if (spec.hardness >= 0.99)
    {
        p.setBrush(c);
        p.drawEllipse(center, r, r);
    }
    else
    {
        /* soft tip: solid core up to `hardness`, then a radial falloff to
         * transparent at the rim */
        QRadialGradient g(center, r);
        g.setColorAt(0.0, c);
        g.setColorAt(qBound<qreal>(0.0, spec.hardness, 0.99), c);
        QColor edge = c;
        edge.setAlpha(0);
        g.setColorAt(1.0, edge);
        p.setBrush(g);
        p.drawEllipse(center, r, r);
    }
    p.end();
}

void stampDab(QImage &target, const QPointF &center, qreal radius,
              qreal hardness, qreal opacity, const QColor &color,
              QPainter::CompositionMode mode)
{
    DabSpec spec;
    spec.radius = radius;
    spec.hardness = hardness;
    spec.opacity = opacity;
    spec.color = color;
    spec.mode = mode;
    stampDab(target, center, spec);
}

/* ---------------------------------------------------------------- fill */

QRegion scanlineFill(const QImage &image, const QPoint &seed, QRgb target,
                     QRgb fill, int tolerance, const QPainterPath *selection,
                     qint64 maxPixels)
{
    QRegion region;
    if (image.isNull() || !image.rect().contains(seed) || target == fill)
        return region;

    const int w = image.width();
    const int h = image.height();
    const qint64 cap = maxPixels > 0 ? maxPixels : qint64(w) * qint64(h);
    if (cap <= 0)
        return region;

    auto similar = [&](QRgb a, QRgb b) {
        if (tolerance <= 0)
            return a == b;
        return qAbs(qRed(a) - qRed(b)) <= tolerance &&
               qAbs(qGreen(a) - qGreen(b)) <= tolerance &&
               qAbs(qBlue(a) - qBlue(b)) <= tolerance;
    };
    /* a pixel may be filled when it matches the target and, when a
     * selection is active, lies inside it (selection is a barrier) */
    auto allowed = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;
        if (!similar(image.pixel(x, y), target))
            return false;
        if (selection && !selection->contains(QPointF(x, y)))
            return false;
        return true;
    };

    /* per-row visited spans, kept sorted and merged. Memory is O(spans):
     * no full-canvas copy, no w*h visited buffer. */
    QHash<int, QVector<QPair<int, int>>> visited;
    auto rowContains = [&](int y, int x) {
        const auto it = visited.constFind(y);
        if (it == visited.constEnd())
            return false;
        for (const auto &iv : *it)
            if (x >= iv.first && x <= iv.second)
                return true;
        return false;
    };
    auto rowAdd = [&](int y, int l, int r) {
        QVector<QPair<int, int>> &ivs = visited[y];
        int i = 0;
        while (i < ivs.size() && ivs[i].first <= r + 1)
        {
            if (ivs[i].second + 1 < l)
            {
                ++i;
                continue;
            }
            l = qMin(l, ivs[i].first);
            r = qMax(r, ivs[i].second);
            ivs.removeAt(i);
        }
        ivs.insert(i, {l, r});
    };

    /* span stack: each entry is one maximal [l..r] run on row y */
    QVector<std::tuple<int, int, int>> stack; /* l, r, y */
    qint64 count = 0;

    auto pushSpan = [&](int x, int y) {
        if (count >= cap || !allowed(x, y) || rowContains(y, x))
            return;
        int l = x, r = x;
        while (allowed(l - 1, y) && !rowContains(y, l - 1))
            --l;
        while (allowed(r + 1, y) && !rowContains(y, r + 1))
            ++r;
        rowAdd(y, l, r);
        stack.append({l, r, y});
        count += r - l + 1;
        /* spans enter the region at push time so a cap that stops the loop
         * before the first pop still yields the seed span */
        region += QRect(l, y, r - l + 1, 1);
    };

    pushSpan(seed.x(), seed.y());
    while (!stack.isEmpty() && count < cap)
    {
        const auto [l, r, y] = stack.takeLast();

        /* check the rows above and below across the whole span (+1 so
         * diagonally connected spans are found too) */
        for (int yy = y - 1; yy <= y + 1; yy += 2)
        {
            if (yy < 0 || yy >= h)
                continue;
            for (int x = l - 1; x <= r + 1;)
            {
                if (count >= cap || rowContains(yy, x) || !allowed(x, yy))
                {
                    ++x;
                    continue;
                }
                int nl = x;
                int nr = x;
                while (allowed(nl - 1, yy) && !rowContains(yy, nl - 1))
                    --nl;
                while (allowed(nr + 1, yy) && !rowContains(yy, nr + 1))
                    ++nr;
                rowAdd(yy, nl, nr);
                stack.append({nl, nr, yy});
                count += nr - nl + 1;
                region += QRect(nl, yy, nr - nl + 1, 1);
                x = nr + 1;
            }
        }
    }
    return region;
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

void pbLogMsg(vlc_object_t *obj, int prio, const char *category,
              const char *file, unsigned line, const char *func,
              const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vlc_object_vaLog(VLC_OBJECT(obj), prio, category, file, line, func,
                     format, ap);
    va_end(ap);
}

void pbLogRequest(vlc_object_t *obj, const char *kind,
                  const QString &requestId, qint64 elapsedMs,
                  bool ok, const QString &detail)
{
    /* Log only ids, durations and outcomes — never keys, headers or bodies. */
    if (ok)
        pbLogDebug(obj, "network", "%s request %s ok in %lld ms",
                   kind, qUtf8Printable(requestId), (long long)elapsedMs);
    else
        pbLogWarn(obj, "network", "%s request %s failed in %lld ms: %s",
                  kind, qUtf8Printable(requestId), (long long)elapsedMs,
                  qUtf8Printable(detail));
}

} // namespace paintbrush