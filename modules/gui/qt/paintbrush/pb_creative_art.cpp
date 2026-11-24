/*****************************************************************************
 * pb_creative_art.cpp : NAI Paintbrush — procedural creative effects
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_creative_art.hpp"
/* The layer adapter needs the canvas model (and therefore VLC). The standalone
 * art harness defines PB_CREATIVE_ART_STANDALONE and compiles this file with
 * Qt Core/Gui only; it exercises the pure primitives, not the layer adapter. */
#ifndef PB_CREATIVE_ART_STANDALONE
#include "pb_layer.hpp"
#endif

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace paintbrush {
namespace creative {

namespace {

/* Small deterministic xorshift. QRandomGenerator is fine for gameplay but its
 * stream is allowed to change between Qt versions; recipe output must not.
 * xorshift32 is stable forever, which keeps saved projects reproducible. */
struct Rng
{
    quint32 state;

    explicit Rng(quint32 seed)
        : state(seed ? seed : 0x9E3779B9u)
    {
    }
    quint32 next()
    {
        quint32 x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
    float unit() { return float(next() & 0xFFFFFFu) / float(0x1000000u); }
    float range(float a, float b) { return a + (b - a) * unit(); }
    int pick(int count) { return count <= 0 ? 0 : int(next() % quint32(count)); }
};

inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float smoothstep01(float edge0, float edge1, float x)
{
    const float t = clamp01((x - edge0) / std::max(1e-6f, edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline quint32 hash2(quint32 seed, int x, int y)
{
    quint32 h = seed ^ (quint32(x) * 374761393u) ^ (quint32(y) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float hash01(quint32 seed, int x, int y)
{
    return float(hash2(seed, x, y) & 0xFFFFFFu) / float(0x1000000u);
}

/* --- Oklab (Björn Ottosson) -------------------------------------------- */

struct Lab
{
    float L = 0.0f;
    float a = 0.0f;
    float b = 0.0f;
};

inline float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

/* 8-bit -> linear table: the recolor loop is the hot path and the input is
 * always 8-bit, so the expensive pow() is paid 256 times, not per pixel. */
const float *srgbTable()
{
    static float table[256];
    static bool ready = false;
    if (!ready)
    {
        for (int i = 0; i < 256; ++i)
            table[i] = srgbToLinear(float(i) / 255.0f);
        ready = true;
    }
    return table;
}

Lab toLab(int r8, int g8, int b8)
{
    const float *lut = srgbTable();
    const float r = lut[r8 & 0xFF];
    const float g = lut[g8 & 0xFF];
    const float b = lut[b8 & 0xFF];

    const float l = 0.4122214708f * r + 0.5363325363f * g
                    + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g
                    + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g
                    + 0.6299787005f * b;

    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);

    Lab out;
    out.L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    out.a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    out.b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    return out;
}

QRgb fromLab(const Lab &lab, int alpha)
{
    const float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    const float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    const float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    const float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    const int r8 = int(clamp01(linearToSrgb(r)) * 255.0f + 0.5f);
    const int g8 = int(clamp01(linearToSrgb(g)) * 255.0f + 0.5f);
    const int b8 = int(clamp01(linearToSrgb(b)) * 255.0f + 0.5f);
    return qRgba(r8, g8, b8, alpha);
}

QImage grayscale(const QImage &mask)
{
    if (mask.isNull())
        return {};
    if (mask.format() == QImage::Format_Grayscale8)
        return mask;
    return mask.convertToFormat(QImage::Format_Grayscale8);
}

QImage argb(const QImage &image)
{
    if (image.isNull())
        return {};
    if (image.format() == QImage::Format_ARGB32)
        return image;
    return image.convertToFormat(QImage::Format_ARGB32);
}

QImage luminanceImage(const QImage &image)
{
    const QImage src = argb(image);
    if (src.isNull())
        return {};
    QImage out(src.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *dst = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x)
        {
            const int l = (54 * qRed(line[x]) + 183 * qGreen(line[x])
                           + 19 * qBlue(line[x])) >> 8;
            dst[x] = uchar(l);
        }
    }
    return out;
}

float valueNoise(quint32 seed, float x, float y)
{
    const int x0 = int(std::floor(x));
    const int y0 = int(std::floor(y));
    const float fx = x - float(x0);
    const float fy = y - float(y0);
    const float a = hash01(seed, x0, y0);
    const float b = hash01(seed, x0 + 1, y0);
    const float c = hash01(seed, x0, y0 + 1);
    const float d = hash01(seed, x0 + 1, y0 + 1);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

QImage tinted(const QImage &band, const QColor &color, float intensity)
{
    if (band.isNull())
        return {};
    const QImage g = grayscale(band);
    QImage out(g.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    const int cr = color.red(), cg = color.green(), cb = color.blue();
    const int scale = qBound(0, int(clamp01(intensity) * 256.0f), 256);
    for (int y = 0; y < g.height(); ++y)
    {
        const uchar *src = g.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < g.width(); ++x)
        {
            const int a = (int(src[x]) * scale) >> 8;
            if (a <= 0)
                continue;
            dst[x] = qRgba(cr, cg, cb, a > 255 ? 255 : a);
        }
    }
    return out;
}

QColor sampleColor(const QVector<QColor> &palette, float t)
{
    if (palette.isEmpty())
        return QColor(255, 255, 255);
    if (palette.size() == 1)
        return palette.first();
    const float p = clamp01(t) * float(palette.size() - 1);
    const int i = std::min(int(p), int(palette.size()) - 2);
    const float f = p - float(i);
    const QColor &a = palette.at(i);
    const QColor &b = palette.at(i + 1);
    return QColor(int(lerp(a.red(), b.red(), f) + 0.5f),
                  int(lerp(a.green(), b.green(), f) + 0.5f),
                  int(lerp(a.blue(), b.blue(), f) + 0.5f));
}

std::vector<Lab> paletteLab(const QVector<QColor> &palette)
{
    std::vector<Lab> out;
    out.reserve(size_t(palette.size()));
    for (const QColor &c : palette)
        out.push_back(toLab(c.red(), c.green(), c.blue()));
    return out;
}

Lab sampleLab(const std::vector<Lab> &palette, float t)
{
    if (palette.empty())
        return Lab{ 0.7f, 0.0f, 0.0f };
    if (palette.size() == 1)
        return palette.front();
    const float p = clamp01(t) * float(palette.size() - 1);
    const int i = std::min(int(p), int(palette.size()) - 2);
    const float f = p - float(i);
    const Lab &a = palette[size_t(i)];
    const Lab &b = palette[size_t(i) + 1];
    return Lab{ lerp(a.L, b.L, f), lerp(a.a, b.a, f), lerp(a.b, b.b, f) };
}

/* Skin-like cluster in Oklab. Approximate on purpose: this only DIMS the
 * effect over face-like pixels, it never claims to segment a person. */
float skinAffinity(const Lab &l, float chroma)
{
    const float hue = std::atan2(l.b, l.a) * (180.0f / 3.14159265f);
    if (hue < 5.0f || hue > 78.0f)
        return 0.0f;
    if (chroma < 0.008f || chroma > 0.14f || l.L < 0.18f || l.L > 0.99f)
        return 0.0f;
    const float fh = 1.0f - std::fabs(hue - 42.0f) / 36.0f;
    const float fc = 1.0f - std::fabs(chroma - 0.055f) / 0.085f;
    const float fl = smoothstep01(0.18f, 0.30f, l.L)
                     * (1.0f - smoothstep01(0.94f, 0.996f, l.L));
    return clamp01(fh) * clamp01(fc) * fl;
}

QImage binaryStamp(const QImage &mask)
{
    QImage stamp(mask.size(), QImage::Format_ARGB32_Premultiplied);
    stamp.fill(Qt::transparent);
    for (int y = 0; y < mask.height(); ++y)
    {
        const uchar *src = mask.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(stamp.scanLine(y));
        for (int x = 0; x < mask.width(); ++x)
            if (src[x] >= 40)
                dst[x] = qRgba(255, 255, 255, 255);
    }
    return stamp;
}

QPointF normalised(const QPointF &p)
{
    const qreal len = std::hypot(p.x(), p.y());
    if (len < 1e-6)
        return QPointF(0.0, 0.0);
    return p / len;
}

QPointF perpendicular(const QPointF &p)
{
    return QPointF(-p.y(), p.x());
}

/* Recursive midpoint displacement. Deterministic for a given Rng state. */
void displace(QVector<QPointF> &points, int depth, float amplitude, Rng &rng)
{
    if (depth <= 0 || points.size() < 2)
        return;
    QVector<QPointF> out;
    out.reserve(points.size() * 2);
    for (int i = 0; i < points.size() - 1; ++i)
    {
        const QPointF &a = points.at(i);
        const QPointF &b = points.at(i + 1);
        out.append(a);
        const QPointF n = perpendicular(normalised(b - a));
        const qreal offset = qreal(rng.unit() * 2.0f - 1.0f) * amplitude;
        out.append((a + b) * 0.5 + n * offset);
    }
    out.append(points.last());
    points = out;
    displace(points, depth - 1, amplitude * 0.55f, rng);
}

QPainterPath pathFor(const QVector<QPointF> &points)
{
    QPainterPath path;
    if (points.isEmpty())
        return path;
    path.moveTo(points.first());
    for (int i = 1; i < points.size(); ++i)
        path.lineTo(points.at(i));
    return path;
}

void strokeBolt(QPainter &p, const QPainterPath &path, float scaleFactor,
                const ArcParams &params)
{
    QPen pen;
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    QColor halo = params.halo;
    halo.setAlpha(36);
    pen.setColor(halo);
    pen.setWidthF(std::max(1.0, double(params.glowWidth * scaleFactor)));
    p.setPen(pen);
    p.drawPath(path);

    halo.setAlpha(96);
    pen.setColor(halo);
    pen.setWidthF(std::max(1.0, double(params.glowWidth * 0.42f * scaleFactor)));
    p.setPen(pen);
    p.drawPath(path);

    QColor core = params.core;
    core.setAlpha(236);
    pen.setColor(core);
    pen.setWidthF(std::max(1.0, double(params.coreWidth * scaleFactor)));
    p.setPen(pen);
    p.drawPath(path);
}

} // namespace

/* ------------------------------------------------------------------------ */

float oklabLightness(const QColor &color)
{
    if (!color.isValid())
        return 0.0f;
    return toLab(color.red(), color.green(), color.blue()).L;
}

QImage maskFromAlpha(const QImage &raster)
{
    const QImage src = argb(raster);
    if (src.isNull())
        return {};
    QImage mask(src.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *scan = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *dst = mask.scanLine(y);
        for (int x = 0; x < src.width(); ++x)
            dst[x] = uchar(qAlpha(scan[x]));
    }
    return mask;
}

#ifndef PB_CREATIVE_ART_STANDALONE
QImage layerMask(const CanvasLayer &layer)
{
    if (!layer.mask.isNull())
    {
        QImage mask = grayscale(layer.mask);
        if (!layer.raster.isNull() && mask.size() != layer.raster.size())
            mask = mask.scaled(layer.raster.size(), Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
        return mask;
    }
    return maskFromAlpha(layer.raster);
}
#endif

QImage growMask(const QImage &mask, int radius)
{
    const QImage m = grayscale(mask);
    if (m.isNull() || radius <= 0)
        return m;
    radius = qMin(radius, 96);

    const QImage stamp = binaryStamp(m);
    QImage acc(m.size(), QImage::Format_ARGB32_Premultiplied);
    acc.fill(Qt::transparent);
    {
        QPainter p(&acc);
        constexpr int steps = 16;
        for (int i = 0; i < steps; ++i)
        {
            const double a = 6.283185307179586 * double(i) / double(steps);
            p.drawImage(QPointF(std::cos(a) * radius, std::sin(a) * radius),
                        stamp);
        }
    }
    QImage grown(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < acc.height(); ++y)
    {
        const QRgb *src = reinterpret_cast<const QRgb *>(acc.constScanLine(y));
        uchar *dst = grown.scanLine(y);
        for (int x = 0; x < acc.width(); ++x)
            dst[x] = uchar(qAlpha(src[x]));
    }
    return grown;
}

QImage shrinkMask(const QImage &mask, int radius)
{
    const QImage m = grayscale(mask);
    if (m.isNull() || radius <= 0)
        return m;
    QImage inverted(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < m.height(); ++y)
    {
        const uchar *src = m.constScanLine(y);
        uchar *dst = inverted.scanLine(y);
        for (int x = 0; x < m.width(); ++x)
            dst[x] = uchar(255 - src[x]);
    }
    const QImage grown = growMask(inverted, radius);
    QImage out(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < grown.height(); ++y)
    {
        const uchar *src = grown.constScanLine(y);
        uchar *dst = out.scanLine(y);
        for (int x = 0; x < grown.width(); ++x)
            dst[x] = uchar(255 - src[x]);
    }
    return out;
}

QImage outerBand(const QImage &mask, int radius)
{
    const QImage m = grayscale(mask);
    if (m.isNull())
        return {};
    const QImage grown = growMask(m, radius);
    QImage band(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < m.height(); ++y)
    {
        const uchar *g = grown.constScanLine(y);
        const uchar *s = m.constScanLine(y);
        uchar *dst = band.scanLine(y);
        for (int x = 0; x < m.width(); ++x)
        {
            const int v = int(g[x]) - int(s[x]);
            dst[x] = uchar(v > 0 ? v : 0);
        }
    }
    return band;
}

QImage innerBand(const QImage &mask, int radius)
{
    const QImage m = grayscale(mask);
    if (m.isNull())
        return {};
    const QImage shrunk = shrinkMask(m, radius);
    QImage band(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < m.height(); ++y)
    {
        const uchar *s = m.constScanLine(y);
        const uchar *t = shrunk.constScanLine(y);
        uchar *dst = band.scanLine(y);
        for (int x = 0; x < m.width(); ++x)
        {
            const int v = int(s[x]) - int(t[x]);
            dst[x] = uchar(v > 0 ? v : 0);
        }
    }
    return band;
}

QImage colorizeBand(const QImage &band, const QColor &color, bool glitter,
                    quint32 seed)
{
    const QImage g = grayscale(band);
    if (g.isNull())
        return {};
    QImage out(g.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    const QColor fleck(255, 244, 205);
    for (int y = 0; y < g.height(); ++y)
    {
        const uchar *src = g.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < g.width(); ++x)
        {
            const int a = src[x];
            if (a == 0)
                continue;
            const QColor &c =
                (glitter && hash01(seed, x, y) < 0.18f) ? fleck : color;
            dst[x] = qRgba(c.red(), c.green(), c.blue(), a);
        }
    }
    return out;
}

QImage softBlur(const QImage &image, int downscale)
{
    if (image.isNull())
        return {};
    const int maxSide = qMax(image.width(), image.height());
    if (maxSide < 12)
        return image;
    if (downscale <= 0)
        downscale = qMax(2, maxSide / 42);
    if (downscale <= 1)
        return image;
    const QSize small(qMax(1, image.width() / downscale),
                      qMax(1, image.height() / downscale));
    QImage reduced = image.scaled(small, Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation);
    reduced = reduced.scaled(image.size(), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    return reduced;
}

QImage recolorMasked(const QImage &source, const QImage &mask,
                     const RecolorParams &params)
{
    const QImage src = argb(source);
    if (src.isNull() || params.palette.isEmpty())
        return src;
    QImage m = grayscale(mask);
    if (m.isNull())
        return src;
    if (m.size() != src.size())
        m = m.scaled(src.size(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);

    const std::vector<Lab> palette = paletteLab(params.palette);
    const float strength = clamp01(params.strength);
    const float textureKeep = clamp01(params.textureKeep);
    const float skinGuard = clamp01(params.skinGuard);

    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const uchar *mk = m.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
        {
            const QRgb px = s[x];
            const int alpha = qAlpha(px);
            const int coverage = qMin(alpha, int(mk[x]));
            if (coverage <= 2)
            {
                dst[x] = px;
                continue;
            }

            const Lab l = toLab(qRed(px), qGreen(px), qBlue(px));
            const float chroma = std::hypot(l.a, l.b);

            /* gradient map by perceptual lightness: the fold that was dark
             * stays dark, the highlight stays bright, only the hue moves */
            const float t = clamp01((l.L - 0.10f) / 0.78f);
            const Lab pal = sampleLab(palette, t);
            const float palChroma = std::hypot(pal.a, pal.b);

            float a2 = lerp(pal.a, l.a, textureKeep);
            float b2 = lerp(pal.b, l.b, textureKeep);
            float c2 = std::hypot(a2, b2);
            float hue2 = (c2 > 1e-5f) ? std::atan2(b2, a2)
                                      : std::atan2(pal.b, pal.a);
            const float floorChroma = palChroma * 0.55f;
            if (c2 < floorChroma)
                c2 = floorChroma;

            const float drift = params.hueDrift
                                + 0.28f * std::sin(l.L * 6.2831853f
                                                  + float(params.seed % 97)
                                                        * 0.017f);
            hue2 += drift;
            a2 = c2 * std::cos(hue2);
            b2 = c2 * std::sin(hue2);

            /* lightness is preserved, with a sub-LSB dither only to avoid
             * banding on large flat garments */
            const float dither =
                (hash01(params.seed, x, y) - 0.5f) * 0.004f;
            Lab target;
            target.L = clamp01(l.L + dither);
            target.a = a2;
            target.b = b2;

            float weight = (float(coverage) / 255.0f) * strength;
            weight *= 1.0f - 0.75f * skinAffinity(l, chroma) * skinGuard;
            if (weight <= 0.002f)
            {
                dst[x] = px;
                continue;
            }

            const QRgb styled = fromLab(target, alpha);
            const int r = int(lerp(float(qRed(px)), float(qRed(styled)), weight)
                              + 0.5f);
            const int g = int(lerp(float(qGreen(px)), float(qGreen(styled)),
                                   weight) + 0.5f);
            const int bl = int(lerp(float(qBlue(px)), float(qBlue(styled)),
                                    weight) + 0.5f);
            dst[x] = qRgba(qBound(0, r, 255), qBound(0, g, 255),
                           qBound(0, bl, 255), alpha);
        }
    }
    return out;
}

QImage spectralSheen(const QImage &source, const QImage &mask,
                     const QVector<QColor> &palette, float strength,
                     float angleDeg, float phase)
{
    const QImage src = argb(source);
    if (src.isNull() || palette.isEmpty() || strength <= 0.0f)
        return QImage(src.size(), QImage::Format_ARGB32);
    QImage m = grayscale(mask);
    if (m.isNull())
        return QImage(src.size(), QImage::Format_ARGB32);
    if (m.size() != src.size())
        m = m.scaled(src.size(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);

    const float rad = qDegreesToRadians(angleDeg);
    const float ca = std::cos(rad), sa = std::sin(rad);
    const float diag = std::max(1.0f, std::hypot(float(src.width()),
                                                 float(src.height())));
    const float frequency = 6.2831853f * 2.6f / diag;

    QImage out(src.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const uchar *mk = m.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
        {
            const int alpha = qAlpha(s[x]);
            const int coverage = qMin(alpha, int(mk[x]));
            if (coverage <= 2)
                continue;

            const Lab l = toLab(qRed(s[x]), qGreen(s[x]), qBlue(s[x]));
            float highlight = smoothstep01(0.52f, 0.96f, l.L);
            highlight *= highlight;

            const float u = 0.5f + 0.5f
                                  * std::sin((float(x) * ca + float(y) * sa)
                                                 * frequency
                                             + phase);
            QColor c = sampleColor(palette, u);
            /* lift the sheen towards light instead of painting dark colour */
            Lab cl = toLab(c.red(), c.green(), c.blue());
            cl.L = clamp01(lerp(cl.L, 0.86f, 0.55f));
            c = QColor::fromRgba(fromLab(cl, 255));

            const int a = qBound(0, int(highlight * (float(coverage) / 255.0f)
                                            * clamp01(strength) * 235.0f),
                                 255);
            if (a <= 0)
                continue;
            dst[x] = qRgba(c.red(), c.green(), c.blue(), a);
        }
    }
    return out;
}

HoloMaterial holoMaterialPreset(HoloMaterialId id)
{
    HoloMaterial m;
    switch (id)
    {
    case HoloMaterialId::OpalDream:
        m.name = QStringLiteral("Opal dream");
        m.metallic = 0.08f;
        m.roughness = 0.38f;
        m.filmThickness = 520.0f;
        m.filmVariation = 80.0f;
        m.grating = 0.35f;
        m.dispersion = 0.50f;
        m.sheen = 0.95f;
        m.sparkle = 0.06f;
        m.saturation = 0.52f;
        m.normalStrength = 0.35f;
        m.palette = { QColor(184, 242, 230), QColor(255, 214, 224),
                      QColor(201, 182, 255), QColor(232, 255, 201) };
        break;
    case HoloMaterialId::ChromaticChrome:
        m.name = QStringLiteral("Chromatic chrome");
        m.metallic = 0.98f;
        m.roughness = 0.04f;
        m.filmThickness = 180.0f;
        m.filmVariation = 24.0f;
        m.grating = 0.15f;
        m.dispersion = 0.20f;
        m.sheen = 0.10f;
        m.sparkle = 0.18f;
        m.saturation = 0.20f;
        m.normalStrength = 0.85f;
        m.baseReplace = 0.60f;
        m.clearcoat = 1.0f;
        m.palette = { QColor(235, 248, 255), QColor(110, 140, 190),
                      QColor(200, 205, 215), QColor(255, 255, 255) };
        break;
    case HoloMaterialId::PrismaticFantasy:
        m.name = QStringLiteral("Prismatic fantasy");
        m.metallic = 0.40f;
        m.roughness = 0.16f;
        m.filmThickness = 300.0f;
        m.filmVariation = 260.0f;
        m.grating = 2.4f;
        m.dispersion = 1.0f;
        m.sheen = 0.50f;
        m.sparkle = 0.80f;
        m.saturation = 1.0f;
        m.normalStrength = 0.95f;
        m.baseReplace = 0.18f;
        m.palette = { QColor(255, 47, 185), QColor(0, 229, 255),
                      QColor(182, 255, 0), QColor(255, 224, 102) };
        break;
    case HoloMaterialId::HolographicWrap:
        m.name = QStringLiteral("Holographic wrap");
        m.metallic = 0.50f;
        m.roughness = 0.10f;
        m.filmThickness = 340.0f;
        m.filmVariation = 220.0f;
        m.grating = 1.20f;
        m.dispersion = 1.0f;
        m.sheen = 0.75f;
        m.sparkle = 0.22f;
        m.saturation = 1.0f;
        m.normalStrength = 0.70f;
        m.clearcoat = 0.90f;
        m.colorShift = 0.30f;
        m.baseReplace = 0.55f;
        m.flakeDensity = 0.05f;
        m.flakeSize = 1.2f;
        m.palette = { QColor(0, 80, 90), QColor(0, 140, 255),
                      QColor(110, 60, 200), QColor(220, 255, 250) };
        break;
    case HoloMaterialId::ChameleonPaint:
        m.name = QStringLiteral("Chameleon paint");
        m.metallic = 0.55f;
        m.roughness = 0.12f;
        m.filmThickness = 220.0f;
        m.filmVariation = 40.0f;
        m.grating = 0.10f;
        m.dispersion = 0.25f;
        m.sheen = 0.20f;
        m.sparkle = 0.05f;
        m.saturation = 0.95f;
        m.normalStrength = 0.80f;
        m.clearcoat = 1.0f;
        m.colorShift = 1.0f;
        m.baseReplace = 0.70f;
        m.flakeDensity = 0.10f;
        m.flakeSize = 0.6f;
        m.palette = { QColor(0, 80, 45), QColor(0, 195, 135),
                      QColor(0, 175, 160), QColor(120, 40, 185) };
        break;
    case HoloMaterialId::GlitterMetallic:
        m.name = QStringLiteral("Glitter metallic");
        m.metallic = 0.85f;
        m.roughness = 0.12f;
        m.filmThickness = 160.0f;
        m.filmVariation = 30.0f;
        m.grating = 0.30f;
        m.dispersion = 0.35f;
        m.sheen = 0.25f;
        m.sparkle = 0.95f;
        m.saturation = 0.70f;
        m.normalStrength = 0.75f;
        m.clearcoat = 0.80f;
        m.colorShift = 0.05f;
        m.baseReplace = 0.60f;
        m.flakeDensity = 0.85f;
        m.flakeSize = 0.8f;
        m.palette = { QColor(232, 246, 255), QColor(255, 224, 130),
                      QColor(255, 120, 190), QColor(120, 180, 255) };
        break;
    case HoloMaterialId::ElectricLivery:
        m.name = QStringLiteral("Electric livery");
        m.metallic = 0.50f;
        m.roughness = 0.14f;
        m.filmThickness = 260.0f;
        m.filmVariation = 200.0f;
        m.grating = 2.80f;
        m.dispersion = 1.0f;
        m.sheen = 0.50f;
        m.sparkle = 0.40f;
        m.saturation = 1.0f;
        m.normalStrength = 0.80f;
        m.clearcoat = 0.50f;
        m.colorShift = 0.20f;
        m.baseReplace = 0.75f;
        m.flakeDensity = 0.15f;
        m.flakeSize = 0.9f;
        m.palette = { QColor(8, 8, 18), QColor(0, 234, 255),
                      QColor(255, 0, 180), QColor(183, 255, 24) };
        break;
    case HoloMaterialId::LiquidHologram:
    default:
        m.name = QStringLiteral("Liquid hologram");
        m.metallic = 0.35f;
        m.roughness = 0.12f;
        m.filmThickness = 320.0f;
        m.filmVariation = 180.0f;
        m.grating = 0.90f;
        m.dispersion = 0.90f;
        m.sheen = 0.65f;
        m.sparkle = 0.12f;
        m.saturation = 0.95f;
        m.normalStrength = 0.60f;
        m.palette = { QColor(0, 229, 255), QColor(124, 77, 255),
                      QColor(255, 61, 218), QColor(255, 230, 109) };
        break;
    }
    return m;
}

QString holoMaterialName(HoloMaterialId id)
{
    return holoMaterialPreset(id).name;
}

int composeCoverage(int sourceAlpha, int regionMask, CoverageMode mode)
{
    const int a = qBound(0, sourceAlpha, 255);
    const int b = qBound(0, regionMask, 255);
    switch (mode)
    {
    case CoverageMode::IndependentProbability:
        /* two independent probabilities: P(A and B) = P(A) * P(B) */
        return (a * b + 127) / 255;
    case CoverageMode::UserConstraint:
        /* a hard cap chosen by the artist or assistant */
        return qMin(a, b);
    case CoverageMode::InheritedCoverage:
    default:
        /* the region already carries the final alpha (a cutout mask) */
        return qMin(a, b);
    }
}

QImage estimateNormals(const QImage &source, const QImage &mask, float strength)
{
    return estimateNormals(source, mask, strength, nullptr);
}

QImage estimateNormals(const QImage &source, const QImage &mask, float strength,
                       QImage *confidenceOut)
{
    if (confidenceOut)
        *confidenceOut = QImage();
    const QImage src = argb(source);
    if (src.isNull())
        return {};
    const QImage lum = luminanceImage(src);
    const int minSide = qMin(src.width(), src.height());
    /* Blur and sampling radii scale with the source so the estimated slope is
     * the same object-space slope at preview and export resolutions. */
    const int coarseStep = qMax(2, minSide / 28);
    const int fineStep = qMax(1, minSide / 90);
    const int coarseOff = qMax(1, minSide / 80);
    const int fineOff = qMax(1, minSide / 200);
    const QImage coarse = softBlur(lum, coarseStep);
    const QImage fine = softBlur(lum, fineStep);
    /* A much wider blur removes printed stripes and logos. Comparing the
     * fine gradient with this wide gradient separates a coherent fold from
     * alternating texture. */
    const QImage confCoarse = softBlur(lum, qMax(4, minSide / 12));
    const float k = qBound(0.0f, strength, 3.0f) / 64.0f;

    QImage m = grayscale(mask);
    if (!m.isNull() && m.size() != src.size())
        m = m.scaled(src.size(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);

    auto sampleAt = [](const QImage &img, int x, int y) -> int {
        const int cx = qBound(0, x, img.width() - 1);
        const int cy = qBound(0, y, img.height() - 1);
        return int(img.constScanLine(cy)[cx]);
    };

    QImage out(src.size(), QImage::Format_ARGB32);
    QImage conf(src.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < src.height(); ++y)
    {
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        uchar *cf = conf.scanLine(y);
        for (int x = 0; x < src.width(); ++x)
        {
            const float gx =
                0.5f * float(sampleAt(coarse, x + coarseOff, y)
                             - sampleAt(coarse, x - coarseOff, y))
                + 0.5f * float(sampleAt(fine, x + fineOff, y)
                               - sampleAt(fine, x - fineOff, y));
            const float gy =
                0.5f * float(sampleAt(coarse, x, y + coarseOff)
                             - sampleAt(coarse, x, y - coarseOff))
                + 0.5f * float(sampleAt(fine, x, y + fineOff)
                               - sampleAt(fine, x, y - fineOff));
            const float cx =
                float(sampleAt(confCoarse, x + coarseOff, y)
                      - sampleAt(confCoarse, x - coarseOff, y));
            const float cy =
                float(sampleAt(confCoarse, x, y + coarseOff)
                      - sampleAt(confCoarse, x, y - coarseOff));
            const float fx =
                float(sampleAt(fine, x + fineOff, y)
                      - sampleAt(fine, x - fineOff, y));
            const float fy =
                float(sampleAt(fine, x, y + fineOff)
                      - sampleAt(fine, x, y - fineOff));
            const float cLen = std::hypot(cx, cy);
            const float fLen = std::hypot(fx, fy);
            const float align =
                (cLen > 1e-3f && fLen > 1e-3f)
                    ? (cx * fx + cy * fy) / (cLen * fLen)
                    : 0.0f;
            const float ratio = cLen / (fLen + 1.0f);
            const float c = clamp01(align) * clamp01(ratio * 1.5f);

            float nx = -gx * k;
            float ny = -gy * k;
            float nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f)
            {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            const int a = m.isNull() ? 255 : int(m.constScanLine(y)[x]);
            dst[x] = qRgba(int((nx * 0.5f + 0.5f) * 255.0f + 0.5f),
                           int((ny * 0.5f + 0.5f) * 255.0f + 0.5f),
                           int((nz * 0.5f + 0.5f) * 255.0f + 0.5f), a);
            cf[x] = uchar(int(clamp01(c) * 255.0f + 0.5f));
        }
    }
    if (confidenceOut)
        *confidenceOut = conf;
    return out;
}

HoloMaterialResult holographicMaterial(
    const QImage &source, const QImage &mask, const QImage &normals,
    const HoloMaterial &material, float strength, quint32 seed,
    CoverageMode coverageMode, const QImage &confidence,
    const QImage &protection)
{
    HoloMaterialResult result;
    const QImage src = argb(source);
    if (src.isNull())
        return result;

    QImage m = grayscale(mask);
    if (!m.isNull() && m.size() != src.size())
        m = m.scaled(src.size(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);
    if (m.isNull())
        m = maskFromAlpha(src);

    QImage nrm = argb(normals);
    if (!nrm.isNull() && nrm.size() != src.size())
        nrm = nrm.scaled(src.size(), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);

    QImage conf = grayscale(confidence);
    if (!conf.isNull() && conf.size() != src.size())
        conf = conf.scaled(src.size(), Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
    QImage prot = grayscale(protection);
    if (!prot.isNull() && prot.size() != src.size())
        prot = prot.scaled(src.size(), Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);

    const float power = clamp01(strength);
    const float metallic = clamp01(material.metallic);
    const float roughness = qBound(0.02f, material.roughness, 1.0f);
    const float shininess = qBound(8.0f, 2.0f / (roughness * roughness) - 2.0f,
                                   512.0f);
    const float nFilm = 1.45f;
    const float lightX = -0.42f, lightY = -0.36f, lightZ = 0.83f;
    const float halfX = lightX, halfY = lightY, halfZ = lightZ + 1.0f;
    const float halfLen =
        std::sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ);
    const float hx = halfX / halfLen, hy = halfY / halfLen,
                hz = halfZ / halfLen;
    const float phaseSeed = float(seed % 628) * 0.01f;
    const float gratingCos = std::cos(0.73f);
    const float gratingSin = std::sin(0.73f);
    const float pi = 3.14159265358979323846f;

    result.albedo = QImage(src.size(), QImage::Format_ARGB32);
    result.albedo.fill(Qt::transparent);
    result.sheen = QImage(src.size(), QImage::Format_ARGB32);
    result.sheen.fill(Qt::transparent);

    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const uchar *mk = m.constScanLine(y);
        const QRgb *nline =
            nrm.isNull() ? nullptr
                         : reinterpret_cast<const QRgb *>(nrm.constScanLine(y));
        const uchar *cf = conf.isNull() ? nullptr : conf.constScanLine(y);
        const uchar *pr = prot.isNull() ? nullptr : prot.constScanLine(y);
        QRgb *albedo = reinterpret_cast<QRgb *>(result.albedo.scanLine(y));
        QRgb *sheen = reinterpret_cast<QRgb *>(result.sheen.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
        {
            const int alpha = qAlpha(line[x]);
            const int coverage =
                composeCoverage(alpha, int(mk[x]), coverageMode);
            if (coverage <= 2)
            {
                /* keep the exact coverage even on the softest edge pixels */
                albedo[x] = qRgba(qRed(line[x]), qGreen(line[x]),
                                  qBlue(line[x]), coverage);
                continue;
            }

            const float l = (54.0f * float(qRed(line[x]))
                             + 183.0f * float(qGreen(line[x]))
                             + 19.0f * float(qBlue(line[x])))
                            / (255.0f * 256.0f);
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            if (nline)
            {
                nx = (float(qRed(nline[x])) - 128.0f) / 127.0f;
                ny = (float(qGreen(nline[x])) - 128.0f) / 127.0f;
                /* printed texture scores low confidence and must not become
                 * fake geometry; a real fold keeps most of its slope */
                const float confv = cf ? float(cf[x]) / 255.0f : 1.0f;
                const float nScale = 0.10f + 0.90f * confv;
                nx *= nScale;
                ny *= nScale;
                nz = 1.0f;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= len;
                ny /= len;
                nz /= len;
            }
            const float ndl = qMax(0.0f, nx * lightX + ny * lightY + nz * lightZ);
            const float ndv = qMax(0.02f, nz);
            const float ndh = qMax(0.0f, nx * hx + ny * hy + nz * hz);
            const float fresnel = 0.04f
                                  + 0.96f * std::pow(1.0f - ndv, 5.0f);
            const float spec = std::pow(ndh, shininess)
                               * (0.20f + 0.80f * fresnel);

            /* object-space coordinates: the grating and the thickness noise
             * keep the same spatial frequency when the source is rendered at
             * a different resolution (preview vs native export) */
            const float u = (float(x) + 0.5f) / float(src.width());
            const float v = (float(y) + 0.5f) / float(src.height());

            /* thin-film interference: two-beam reflectance at three
             * representative wavelengths; thickness follows shading and the
             * estimated orientation so the bands bend with the fabric */
            float d = material.filmThickness
                      + (l - 0.5f) * material.filmVariation
                      + (nx * 0.6f + ny * 0.4f) * material.filmVariation
                      + valueNoise(seed + 91, u * 8.0f, v * 8.0f)
                            * material.filmVariation * 0.85f;
            d = qBound(40.0f, d, 1400.0f);
            const float cosT = std::sqrt(
                qMax(0.0f, 1.0f - (1.0f - nz * nz) / (nFilm * nFilm)));
            const float phase = 4.0f * pi * nFilm * d * cosT;
            float fr = 0.5f + 0.5f * std::cos(phase / 650.0f);
            float fg = 0.5f + 0.5f * std::cos(phase / 550.0f + 2.1f);
            float fb = 0.5f + 0.5f * std::cos(phase / 450.0f + 4.2f);

            const float gPhase =
                material.grating
                    * (u * gratingCos + v * gratingSin) * 6.2831853f * 4.0f
                + phaseSeed;
            fr = lerp(fr, 0.5f + 0.5f * std::sin(gPhase),
                      material.dispersion * 0.55f);
            fg = lerp(fg, 0.5f + 0.5f * std::sin(gPhase + 2.1f),
                      material.dispersion * 0.55f);
            fb = lerp(fb, 0.5f + 0.5f * std::sin(gPhase + 4.2f),
                      material.dispersion * 0.55f);

            const QColor pal = sampleColor(
                material.palette,
                clamp01(0.5f + 0.5f * nx * 0.8f
                        + 0.20f * std::sin(gPhase * 0.7f)
                        + material.colorShift * (1.0f - ndv) * 0.7f));
            const float sat = clamp01(material.saturation);
            float sr = lerp(0.5f, fr, sat);
            float sg = lerp(0.5f, fg, sat);
            float sb = lerp(0.5f, fb, sat);
            const float palR = srgbToLinear(float(pal.redF()));
            const float palG = srgbToLinear(float(pal.greenF()));
            const float palB = srgbToLinear(float(pal.blueF()));
            sr = lerp(sr, palR, 0.35f);
            sg = lerp(sg, palG, 0.35f);
            sb = lerp(sb, palB, 0.35f);

            const float srcR = srgbToLinear(float(qRed(line[x])) / 255.0f);
            const float srcG = srgbToLinear(float(qGreen(line[x])) / 255.0f);
            const float srcB = srgbToLinear(float(qBlue(line[x])) / 255.0f);
            const float diffuse = (0.55f + 0.45f * ndl)
                                  * (1.0f - 0.35f * metallic);
            const float baseR = srcR * diffuse;
            const float baseG = srcG * diffuse;
            const float baseB = srcB * diffuse;

            /* Automotive paint: the recipe palette owns the hue while the
             * source lightness keeps texture and panel shading. Fashion
             * recipes keep a low replace value and behave as a film. */
            const float replace = clamp01(material.baseReplace);
            const float paintShade = (0.35f + 0.90f * l)
                                     * (0.55f + 0.45f * ndl);
            const float paintR = palR * paintShade;
            const float paintG = palG * paintShade;
            const float paintB = palB * paintShade;
            float r = lerp(baseR, paintR, replace);
            float g = lerp(baseG, paintG, replace);
            float b = lerp(baseB, paintB, replace);

            const QColor env = sampleColor(material.palette,
                                           clamp01(0.5f + 0.5f * nx));
            r += srgbToLinear(float(env.redF())) * 0.16f * metallic;
            g += srgbToLinear(float(env.greenF())) * 0.16f * metallic;
            b += srgbToLinear(float(env.blueF())) * 0.16f * metallic;

            const float spectral =
                (0.16f + 0.48f * clamp01(material.sheen) * fresnel)
                * (0.5f + 0.5f * clamp01(material.dispersion));
            r += sr * spectral;
            g += sg * spectral;
            b += sb * spectral;

            const float specR = lerp(1.0f, sr, metallic);
            const float specG = lerp(1.0f, sg, metallic);
            const float specB = lerp(1.0f, sb, metallic);
            const float specAmt = spec * (0.45f + 0.55f * metallic);
            r += specR * specAmt;
            g += specG * specAmt;
            b += specB * specAmt;

            /* clearcoat: a second, narrow white lobe above the paint. It is
             * what separates a painted panel from a printed film. */
            const float clear = std::pow(ndh, 90.0f)
                                * clamp01(material.clearcoat)
                                * (0.25f + 0.75f * fresnel);
            r += clear;
            g += clear;
            b += clear;

            /* metallic flake: sparse deterministic flakes, fixed in
             * object-space so preview and export agree */
            const bool flake =
                hash01(seed + 31, int(u * 512.0f), int(v * 512.0f))
                < clamp01(material.flakeDensity) * 0.06f;
            if (flake)
            {
                const float fa = 0.35f + 0.30f * clamp01(material.flakeSize);
                r = lerp(r, 0.95f, fa);
                g = lerp(g, 0.95f, fa);
                b = lerp(b, 0.95f, fa);
            }

            const bool glint =
                hash01(seed + 7, int(u * 512.0f), int(v * 512.0f))
                    < clamp01(material.sparkle) * 0.012f
                && ndh > 0.20f;
            if (glint)
            {
                r = lerp(r, 0.98f, 0.85f);
                g = lerp(g, 0.97f, 0.85f);
                b = lerp(b, 0.92f, 0.85f);
            }

            const float protect = pr ? float(pr[x]) / 255.0f : 0.0f;
            if (protect > 0.001f)
            {
                r = lerp(r, srcR, protect);
                g = lerp(g, srcG, protect);
                b = lerp(b, srcB, protect);
            }

            const int outR = qBound(
                0, int(linearToSrgb(clamp01(lerp(srcR, r, power)))
                           * 255.0f + 0.5f), 255);
            const int outG = qBound(
                0, int(linearToSrgb(clamp01(lerp(srcG, g, power)))
                           * 255.0f + 0.5f), 255);
            const int outB = qBound(
                0, int(linearToSrgb(clamp01(lerp(srcB, b, power)))
                           * 255.0f + 0.5f), 255);
            albedo[x] = qRgba(outR, outG, outB, coverage);

            const float sheenAlpha =
                clamp01(clamp01(material.sheen) * (0.12f + 0.88f * fresnel)
                        + spec * 0.55f + clear * 0.5f
                        + (flake ? 0.25f : 0.0f)
                        + (glint ? 0.55f : 0.0f))
                * (float(coverage) / 255.0f) * (1.0f - protect);
            if (sheenAlpha > 0.01f)
            {
                sheen[x] = qRgba(
                    qBound(0, int(linearToSrgb(clamp01(sr * 0.7f + 0.3f))
                                      * 255.0f), 255),
                    qBound(0, int(linearToSrgb(clamp01(sg * 0.7f + 0.3f))
                                      * 255.0f), 255),
                    qBound(0, int(linearToSrgb(clamp01(sb * 0.7f + 0.3f))
                                      * 255.0f), 255),
                    int(clamp01(sheenAlpha * power) * 235.0f + 0.5f));
            }
        }
    }
    return result;
}

QImage glowField(const QImage &mask, int radius, const QColor &color,
                 float intensity)
{
    const QImage m = grayscale(mask);
    if (m.isNull() || radius <= 0 || intensity <= 0.0f)
        return {};
    QImage band = outerBand(m, radius);
    band = softBlur(band, qMax(2, radius / 2));
    return tinted(band, color, clamp01(intensity) * 0.85f);
}

QImage rimLight(const QImage &mask, int innerPx, int outerPx,
                const QColor &inner, const QColor &outer, float intensity)
{
    const QImage m = grayscale(mask);
    if (m.isNull())
        return {};
    const float amount = clamp01(intensity);
    QImage out(m.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);

    QImage outerImg;
    if (outerPx > 0)
    {
        outerImg = softBlur(outerBand(m, outerPx),
                            qMax(2, outerPx / 2));
        outerImg = tinted(outerImg, outer, amount * 0.9f);
    }
    QImage innerImg;
    if (innerPx > 0)
    {
        innerImg = softBlur(innerBand(m, innerPx), 2);
        innerImg = tinted(innerImg, inner, amount);
    }

    QPainter p(&out);
    if (!outerImg.isNull())
        p.drawImage(0, 0, outerImg);
    if (!innerImg.isNull())
        p.drawImage(0, 0, innerImg);
    return out;
}

QImage electricArc(const QSize &size, const QPointF &from, const QPointF &to,
                   quint32 seed, const ArcParams &params)
{
    if (!size.isValid() || size.isEmpty())
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);

    Rng rng(seed);
    QVector<QPointF> main;
    main.append(from);
    main.append(to);
    const float length = float(std::hypot(to.x() - from.x(),
                                          to.y() - from.y()));
    displace(main, 5, std::max(2.0f, length * params.displacement), rng);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);

    strokeBolt(p, pathFor(main), 1.0f, params);

    /* secondary bolts leave the main channel at random points */
    const int branches = qBound(0, params.branches, 6);
    for (int i = 0; i < branches; ++i)
    {
        const int at = 1 + rng.pick(std::max(1, int(main.size()) - 2));
        const QPointF base = main.at(std::min(at, int(main.size()) - 1));
        const QPointF dir = normalised(to - from);
        const QPointF side = perpendicular(dir)
                             * (rng.unit() < 0.5f ? -1.0 : 1.0);
        const QPointF end = base
                            + (side + dir * rng.range(-0.4f, 0.4f))
                                  * (length * rng.range(0.14f, 0.28f));
        QVector<QPointF> twig;
        twig.append(base);
        twig.append((base + end) * 0.5
                    + perpendicular(normalised(end - base))
                          * (length * rng.range(-0.08f, 0.08f)));
        twig.append(end);
        displace(twig, 2, length * 0.035f, rng);
        strokeBolt(p, pathFor(twig), 0.55f, params);
    }

    /* loose sparks around the channel */
    const int sparks = qBound(0, params.sparkCount, 400);
    QPen pen;
    pen.setCapStyle(Qt::RoundCap);
    for (int i = 0; i < sparks; ++i)
    {
        if (main.size() < 2)
            break;
        const int at = rng.pick(main.size() - 1);
        const QPointF a = main.at(at);
        const QPointF b = main.at(at + 1);
        const QPointF dir = normalised(b - a);
        const QPointF n = perpendicular(dir);
        const QPointF origin = a + n * rng.range(-params.glowWidth,
                                                 params.glowWidth);
        const QPointF tip = origin
                            + n * (rng.unit() < 0.5f ? -1.0 : 1.0)
                                  * rng.range(2.0f, 2.0f + params.glowWidth * 0.6f)
                            + dir * rng.range(-6.0f, 6.0f);
        QColor c = (rng.unit() < 0.22f) ? params.core : params.halo;
        c.setAlpha(int(rng.range(90.0f, 230.0f)));
        pen.setColor(c);
        pen.setWidthF(std::max(1.0, double(params.coreWidth * 0.55f)));
        p.setPen(pen);
        p.drawLine(origin, tip);
    }
    return out;
}

QImage sparkField(const QImage &mask, const QSize &size, quint32 seed,
                  const QVector<QColor> &palette, int count, float energy)
{
    const QImage m = grayscale(mask);
    if (m.isNull() || count <= 0)
        return {};
    const int w = m.width();
    const int h = m.height();

    struct Boundary
    {
        QPointF point;
        QPointF normal;
    };
    std::vector<Boundary> boundary;
    const int stride = qMax(1, qMin(w, h) / 180);
    for (int y = 1; y < h - 1; y += stride)
    {
        const uchar *row = m.constScanLine(y);
        const uchar *up = m.constScanLine(y - 1);
        const uchar *down = m.constScanLine(y + 1);
        for (int x = 1; x < w - 1; x += stride)
        {
            if (row[x] < 110)
                continue;
            if (row[x - 1] >= 64 && row[x + 1] >= 64 && up[x] >= 64
                && down[x] >= 64)
                continue;
            const float gx = float(row[x + 1]) - float(row[x - 1]);
            const float gy = float(down[x]) - float(up[x]);
            Boundary b;
            b.point = QPointF(x + 0.5, y + 0.5);
            /* gradient points into the shape; particles leave the shape */
            b.normal = -normalised(QPointF(gx, gy));
            if (b.normal.isNull())
                b.normal = QPointF(0.0, -1.0);
            boundary.push_back(b);
        }
    }
    if (boundary.empty())
        return {};

    QImage out(size.isValid() && !size.isEmpty() ? size : m.size(),
               QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);

    Rng rng(seed);
    const float e = clamp01(energy);
    for (int i = 0; i < count; ++i)
    {
        const Boundary &b = boundary[size_t(rng.pick(int(boundary.size())))];
        const QPointF tangent = perpendicular(b.normal);
        const float sizePx = rng.range(0.7f, 2.6f);
        const QColor c = palette.isEmpty()
                             ? QColor(255, 255, 255)
                             : palette.at(rng.pick(palette.size()));
        QColor dot = c;
        dot.setAlpha(int(rng.range(110.0f, 235.0f) * e));
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(b.point, sizePx, sizePx);

        /* a short streak along the outward normal reads as motion */
        const float len = rng.range(3.0f, 6.0f + 12.0f * e);
        const QPointF tip = b.point + b.normal * len
                            + tangent * rng.range(-5.0f, 5.0f);
        QPen pen(dot);
        pen.setCapStyle(Qt::RoundCap);
        pen.setWidthF(std::max(0.8, double(sizePx * 0.7f)));
        p.setPen(pen);
        p.drawLine(b.point, tip);
    }
    return out;
}

QImage backdrop(const QSize &size, quint32 seed, const QVector<QColor> &palette,
                float strength)
{
    if (!size.isValid() || size.isEmpty())
        return {};
    const float amount = clamp01(strength);
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::black);

    const QColor deep = palette.isEmpty() ? QColor(18, 4, 32) : palette.first();
    const QColor glow = palette.size() > 1 ? palette.at(1) : QColor(0, 234, 255);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient vertical(0, 0, 0, size.height());
    vertical.setColorAt(0.0, QColor(deep.red() * 30 / 100,
                                    deep.green() * 30 / 100,
                                    deep.blue() * 34 / 100));
    vertical.setColorAt(0.55, QColor(10, 8, 18));
    vertical.setColorAt(1.0, QColor(glow.red() * 12 / 100,
                                    glow.green() * 12 / 100,
                                    glow.blue() * 16 / 100));
    p.fillRect(QRect(QPoint(0, 0), size), vertical);

    /* light-stripe texture */
    const int stripeStep = qMax(6, size.width() / 64);
    p.setPen(QPen(QColor(255, 255, 255, 14), 1));
    for (int x = 0; x < size.width(); x += stripeStep)
        p.drawLine(x, 0, x, size.height());

    /* focal bloom */
    const QPointF focus(size.width() * 0.54, size.height() * 0.38);
    const qreal bloomRadius = std::hypot(qreal(size.width()),
                                         qreal(size.height())) * 0.32;
    QRadialGradient bloom(focus, bloomRadius);
    QColor bloomColor = glow;
    bloomColor.setAlpha(int(78 * amount));
    bloom.setColorAt(0.0, bloomColor);
    bloom.setColorAt(0.55, QColor(glow.red(), glow.green(), glow.blue(), 22));
    bloom.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(QRect(QPoint(0, 0), size), bloom);

    /* diagonal speed rays converging on the subject area */
    Rng rng(seed);
    const int rays = 9;
    for (int i = 0; i < rays; ++i)
    {
        const qreal angle = 6.283185307179586 * (double(i) / double(rays))
                            + rng.range(-0.16f, 0.16f);
        const qreal reach = std::hypot(qreal(size.width()),
                                       qreal(size.height())) * 0.85;
        const QPointF edge = focus
                             + QPointF(std::cos(angle), std::sin(angle))
                                   * reach;
        QColor ray = palette.isEmpty()
                         ? QColor(255, 255, 255)
                         : palette.at((i + 1) % palette.size());
        ray.setAlpha(int(rng.range(40.0f, 95.0f) * amount));
        p.setPen(QPen(ray, qMax(1.0, size.width() / 640.0)));
        p.drawLine(focus, edge);
        ray.setAlpha(int(rng.range(14.0f, 30.0f) * amount));
        p.setPen(QPen(ray, qMax(2.0, size.width() / 190.0)));
        p.drawLine(focus, edge);
    }

    /* vignette so the frame reads as lit scenery, not wallpaper */
    QRadialGradient vignette(focus, bloomRadius * 1.9);
    vignette.setColorAt(0.0, QColor(0, 0, 0, 0));
    vignette.setColorAt(0.72, QColor(0, 0, 0, 0));
    vignette.setColorAt(1.0, QColor(0, 0, 0, 150));
    p.fillRect(QRect(QPoint(0, 0), size), vignette);

    p.end();
    return out;
}

QImage starField(const QSize &size, quint32 seed, const QVector<QColor> &palette,
                 int count)
{
    if (!size.isValid() || size.isEmpty() || count <= 0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    Rng rng(seed);
    for (int i = 0; i < count; ++i)
    {
        const QPointF c(rng.range(0.0f, float(size.width())),
                        rng.range(0.0f, float(size.height())));
        const float s = rng.range(1.6f, 6.4f);
        const QColor base = palette.isEmpty()
                                ? QColor(255, 255, 255)
                                : palette.at(rng.pick(palette.size()));
        QColor c1 = base;
        c1.setAlpha(int(rng.range(110.0f, 235.0f)));
        QPen pen(c1);
        pen.setCapStyle(Qt::RoundCap);
        pen.setWidthF(rng.range(0.8f, 1.8f));
        p.setPen(pen);
        p.drawLine(QPointF(c.x() - s, c.y()), QPointF(c.x() + s, c.y()));
        p.drawLine(QPointF(c.x(), c.y() - s), QPointF(c.x(), c.y() + s));
        p.setPen(Qt::NoPen);
        p.setBrush(c1);
        p.drawEllipse(c, s * 0.28, s * 0.28);
    }
    return out;
}

QImage butterflyField(const QSize &size, quint32 seed,
                      const QList<QColor> &palette,
                      const QList<QPointF> &anchors, int count)
{
    if (!size.isValid() || size.isEmpty() || count <= 0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    Rng rng(seed);
    for (int i = 0; i < count; ++i)
    {
        QPointF c;
        if (!anchors.isEmpty())
        {
            const QPointF a = anchors.at(rng.pick(anchors.size()));
            c = a + QPointF(rng.range(-90.0f, 90.0f),
                            rng.range(-90.0f, 90.0f));
        }
        else
            c = QPointF(rng.range(0.0f, float(size.width())),
                        rng.range(0.0f, float(size.height())));
        c.setX(qBound(6.0, c.x(), qreal(size.width() - 6)));
        c.setY(qBound(6.0, c.y(), qreal(size.height() - 6)));

        const float s = rng.range(5.0f, 15.0f);
        const QColor base = palette.isEmpty()
                                ? QColor(255, 255, 255)
                                : palette.at(rng.pick(palette.size()));
        QColor fill = base;
        fill.setAlpha(int(rng.range(150.0f, 230.0f)));

        QColor auraColor = base;
        auraColor.setAlpha(46);
        QRadialGradient aura(c, s * 3.2);
        aura.setColorAt(0.0, auraColor);
        aura.setColorAt(1.0, QColor(base.red(), base.green(), base.blue(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(aura);
        p.drawEllipse(c, s * 3.2, s * 3.2);

        p.save();
        p.translate(c);
        p.rotate(rng.range(-40.0f, 40.0f));
        p.setBrush(fill);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(-s * 0.95, -s * 0.9, s * 0.9, s * 0.8));
        p.drawEllipse(QRectF(s * 0.05, -s * 0.9, s * 0.9, s * 0.8));
        p.drawEllipse(QRectF(-s * 0.72, -s * 0.05, s * 0.68, s * 0.66));
        p.drawEllipse(QRectF(s * 0.04, -s * 0.05, s * 0.68, s * 0.66));
        QColor body = base.lighter(130);
        body.setAlpha(fill.alpha());
        p.setPen(QPen(body, qMax(1.0, double(s * 0.16))));
        p.drawLine(QPointF(0, -s * 0.9), QPointF(0, s * 0.72));
        p.setPen(QPen(body, qMax(0.7, double(s * 0.07))));
        p.drawLine(QPointF(0, -s * 0.88), QPointF(-s * 0.34, -s * 1.28));
        p.drawLine(QPointF(0, -s * 0.88), QPointF(s * 0.34, -s * 1.28));
        p.restore();
    }
    return out;
}

/* ------------------------------------------------------------------------ */

QImage trimTransparent(const QImage &image, QPoint *offset)
{
    const QImage src = argb(image);
    if (offset)
        *offset = QPoint(0, 0);
    if (src.isNull())
        return {};

    int minX = src.width(), minY = src.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
        {
            if (qAlpha(line[x]) == 0)
                continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < 0)
        return {};
    if (offset)
        *offset = QPoint(minX, minY);
    return src.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
}

QImage maskToCutout(const QImage &raster, const QImage &mask)
{
    QImage out = argb(raster);
    if (out.isNull())
        return out;
    QImage m = grayscale(mask);
    if (m.isNull())
        return out;
    if (m.size() != out.size())
        m = m.scaled(out.size(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);
    for (int y = 0; y < out.height(); ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        const uchar *mk = m.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]),
                           qMin(qAlpha(row[x]), int(mk[x])));
    }
    return out;
}

QImage bandFromMask(const QImage &mask, int radius)
{
    return outerBand(mask, radius);
}

QImage gradeImage(const QImage &image, const Grade &grade)
{
    const QImage src = argb(image);
    if (src.isNull())
        return src;
    const bool hasTint = grade.tint.isValid() && grade.tintAmount > 0.0f;
    const bool hasLight = grade.lightTint.isValid() && grade.lightAmount > 0.0f;
    const float tr = hasTint ? float(grade.tint.redF()) : 0.0f;
    const float tg = hasTint ? float(grade.tint.greenF()) : 0.0f;
    const float tb = hasTint ? float(grade.tint.blueF()) : 0.0f;
    const float lr = hasLight ? float(grade.lightTint.redF()) : 0.0f;
    const float lg = hasLight ? float(grade.lightTint.greenF()) : 0.0f;
    const float lb = hasLight ? float(grade.lightTint.blueF()) : 0.0f;

    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
        {
            const int alpha = qAlpha(line[x]);
            if (alpha == 0)
            {
                dst[x] = 0;
                continue;
            }
            float r = float(qRed(line[x])) / 255.0f;
            float g = float(qGreen(line[x])) / 255.0f;
            float b = float(qBlue(line[x])) / 255.0f;

            r *= grade.brightness;
            g *= grade.brightness;
            b *= grade.brightness;
            r = (r - 0.5f) * grade.contrast + 0.5f;
            g = (g - 0.5f) * grade.contrast + 0.5f;
            b = (b - 0.5f) * grade.contrast + 0.5f;

            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = luma + (r - luma) * grade.saturation;
            g = luma + (g - luma) * grade.saturation;
            b = luma + (b - luma) * grade.saturation;

            if (hasTint)
            {
                const float w = clamp01(grade.tintAmount)
                                * (1.0f - clamp01(luma));
                r = lerp(r, tr, w);
                g = lerp(g, tg, w);
                b = lerp(b, tb, w);
            }
            if (hasLight)
            {
                const float w = clamp01(grade.lightAmount) * clamp01(luma);
                r = lerp(r, lr, w);
                g = lerp(g, lg, w);
                b = lerp(b, lb, w);
            }

            dst[x] = qRgba(int(clamp01(r) * 255.0f + 0.5f),
                           int(clamp01(g) * 255.0f + 0.5f),
                           int(clamp01(b) * 255.0f + 0.5f), alpha);
        }
    }
    return out;
}

QImage hueShift(const QImage &image, int degrees)
{
    const QImage src = argb(image);
    if (src.isNull() || degrees == 0)
        return src;
    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
        {
            const QRgb px = line[x];
            const int alpha = qAlpha(px);
            if (alpha == 0)
            {
                dst[x] = 0;
                continue;
            }
            int h = 0, s = 0, v = 0;
            QColor::fromRgba(px).getHsv(&h, &s, &v);
            if (h < 0)
            {
                dst[x] = px;
                continue;
            }
            h = (h + degrees) % 360;
            if (h < 0)
                h += 360;
            dst[x] = QColor::fromHsv(h, s, v, alpha).rgba();
        }
    }
    return out;
}

QImage radialGlow(const QSize &size, const QPointF &center, qreal radius,
                  const QColor &color, float intensity)
{
    if (!size.isValid() || size.isEmpty() || radius <= 0.0 || intensity <= 0.0f)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRadialGradient glow(center, radius);
    QColor c = color;
    c.setAlpha(qBound(0, int(clamp01(intensity) * 255.0f), 255));
    glow.setColorAt(0.0, c);
    c.setAlpha(qBound(0, int(clamp01(intensity) * 90.0f), 255));
    glow.setColorAt(0.45, c);
    glow.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(center, radius, radius);
    return out;
}

QImage glossOverlay(const QSize &stage, const QImage &subject,
                    const QRectF &subjectRect, const QColor &color)
{
    if (!stage.isValid() || stage.isEmpty() || subject.isNull())
        return {};
    QImage out(stage, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QLinearGradient sheet(0.0, 0.0, 0.0, qreal(stage.height()));
    QColor c = color;
    c.setAlpha(210);
    sheet.setColorAt(0.0, c);
    c.setAlpha(110);
    sheet.setColorAt(0.42, c);
    c.setAlpha(22);
    sheet.setColorAt(0.72, c);
    c.setAlpha(0);
    sheet.setColorAt(1.0, c);
    p.fillRect(QRectF(0, 0, stage.width(), stage.height()), sheet);

    /* DestinationIn must see the subject silhouette across the WHOLE stage.
     * Drawing the subject at subjectRect only composites inside that rect and
     * leaves the gradient outside it — which reads as a lit rectangle behind
     * the subject. Build a stage-sized alpha mask first. */
    QImage mask(stage, QImage::Format_ARGB32);
    mask.fill(Qt::transparent);
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        mp.drawImage(subjectRect, subject);
    }
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, mask);
    return out;
}

QImage reflection(const QImage &subject, qreal opacity, qreal fadeStart)
{
    const QImage src = argb(subject);
    if (src.isNull())
        return {};
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    QImage out = src.flipped(Qt::Vertical);
#else
    QImage out = src.mirrored(false, true);
#endif
    const qreal fade = qBound<qreal>(0.0, fadeStart, 0.98);
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    QLinearGradient g(0, 0, 0, out.height());
    g.setColorAt(0.0, QColor(0, 0, 0, int(clamp01(float(opacity)) * 255.0f)));
    g.setColorAt(fade, QColor(0, 0, 0, int(clamp01(float(opacity)) * 235.0f)));
    g.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(out.rect(), g);
    return out;
}

QImage gradientField(const QSize &size, const QColor &top, const QColor &bottom,
                     const QColor &accent, qreal accentAmount)
{
    if (!size.isValid() || size.isEmpty())
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient vertical(0, 0, 0, size.height());
    vertical.setColorAt(0.0, top);
    vertical.setColorAt(1.0, bottom);
    p.fillRect(QRectF(0, 0, size.width(), size.height()), vertical);

    if (accent.isValid() && accentAmount > 0.0)
    {
        const QPointF c(size.width() * 0.5, size.height() * 0.38);
        const qreal radius = std::hypot(qreal(size.width()),
                                        qreal(size.height())) * 0.62;
        QRadialGradient glow(c, radius);
        QColor a = accent;
        a.setAlpha(qBound(0, int(clamp01(float(accentAmount))
                                  * float(accent.alpha())),
                          255));
        glow.setColorAt(0.0, a);
        a.setAlpha(qBound(0, int(clamp01(float(accentAmount)) * 30.0f), 255));
        glow.setColorAt(0.55, a);
        glow.setColorAt(1.0, QColor(accent.red(), accent.green(), accent.blue(),
                                    0));
        p.fillRect(QRectF(0, 0, size.width(), size.height()), glow);
    }
    return out;
}

QImage halftoneField(const QSize &size, const QColor &color, qreal cell,
                     qreal dotRadius, qreal angleDeg, qreal phase)
{
    if (!size.isValid() || size.isEmpty() || cell <= 0.5)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.translate(size.width() / 2.0, size.height() / 2.0);
    p.rotate(angleDeg);
    p.translate(-size.width() / 2.0, -size.height() / 2.0);

    const qreal reach = std::hypot(qreal(size.width()), qreal(size.height()))
                        * 0.75;
    const qreal cx = size.width() / 2.0;
    const qreal cy = size.height() / 2.0;
    for (qreal y = cy - reach; y <= cy + reach; y += cell)
    {
        for (qreal x = cx - reach; x <= cx + reach; x += cell)
        {
            const qreal wave = 0.42
                               + 0.58
                                     * (0.5
                                        + 0.5
                                              * std::sin((x + y) * 0.035
                                                         + phase * 6.2831853));
            QColor c = color;
            c.setAlpha(qBound(0, int(float(color.alpha()) * float(wave)), 255));
            p.setBrush(c);
            p.drawEllipse(QPointF(x, y), dotRadius * wave, dotRadius * wave);
        }
    }
    return out;
}

QImage bokehField(const QSize &size, quint32 seed, const QList<QColor> &palette,
                  int count, qreal minRadius, qreal maxRadius)
{
    if (!size.isValid() || size.isEmpty() || count <= 0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    Rng rng(seed);
    for (int i = 0; i < count; ++i)
    {
        const QPointF c(rng.range(0.0f, float(size.width())),
                        rng.range(0.0f, float(size.height())));
        const qreal r = minRadius
                        + (maxRadius - minRadius) * qreal(rng.unit());
        const QColor base = palette.isEmpty()
                                ? QColor(255, 255, 255)
                                : palette.at(rng.pick(palette.size()));
        QRadialGradient disc(c, r);
        QColor inner = base;
        inner.setAlpha(int(rng.range(70.0f, 190.0f)));
        disc.setColorAt(0.0, inner);
        inner.setAlpha(int(rng.range(40.0f, 110.0f)));
        disc.setColorAt(0.72, inner);
        disc.setColorAt(0.92, QColor(base.red(), base.green(), base.blue(), 26));
        disc.setColorAt(1.0, QColor(base.red(), base.green(), base.blue(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(disc);
        p.drawEllipse(c, r, r);
    }
    return out;
}

QImage grainField(const QSize &size, quint32 seed, int amount)
{
    if (!size.isValid() || size.isEmpty() || amount <= 0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    const int peak = qBound(0, amount, 255);
    for (int y = 0; y < size.height(); ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
        {
            const quint32 h = hash2(seed, x, y);
            const int v = int(h & 0xFF);
            const int a = int((h >> 8) & 0xFF) * peak / 255;
            row[x] = qRgba(v, v, v, a);
        }
    }
    return out;
}

QImage scanlineField(const QSize &size, const QColor &color, int spacing,
                     qreal alpha)
{
    if (!size.isValid() || size.isEmpty() || spacing <= 0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    QColor c = color;
    c.setAlpha(qBound(0, int(clamp01(float(alpha)) * 255.0f), 255));
    p.setPen(QPen(c, 1));
    for (int y = 0; y < size.height(); y += spacing)
        p.drawLine(0, y, size.width(), y);
    return out;
}

QImage inkSplatter(const QSize &size, quint32 seed, const QRectF &area,
                   const QColor &color, int count)
{
    if (!size.isValid() || size.isEmpty() || count <= 0 || area.isEmpty())
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    Rng rng(seed);

    for (int i = 0; i < count; ++i)
    {
        const bool big = rng.unit() < 0.22f;
        const qreal radius = (big ? area.height() * rng.range(0.02f, 0.06f)
                                  : area.height() * rng.range(0.002f, 0.012f));
        const QPointF c(area.x() + rng.range(0.0f, 1.0f) * float(area.width()),
                        area.y() + rng.range(0.0f, 1.0f) * float(area.height()));
        QColor blob = color;
        blob.setAlpha(int(rng.range(150.0f, 235.0f)));
        QRadialGradient g(c, radius);
        g.setColorAt(0.0, blob);
        blob.setAlpha(120);
        g.setColorAt(0.82, blob);
        g.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
        p.setBrush(g);
        /* an irregular blob: several overlapping discs */
        for (int k = 0; k < 5; ++k)
        {
            const QPointF o(c.x() + rng.range(-0.55f, 0.55f) * radius,
                            c.y() + rng.range(-0.55f, 0.55f) * radius);
            p.drawEllipse(o, radius * rng.range(0.55f, 1.0f),
                          radius * rng.range(0.55f, 1.0f));
        }
        /* satellites */
        const int drops = rng.pick(6);
        for (int d = 0; d < drops; ++d)
        {
            const qreal a = rng.range(0.0f, 6.2831853f);
            const qreal dist = radius * rng.range(1.4f, 3.6f);
            const qreal dr = radius * rng.range(0.08f, 0.24f);
            p.setBrush(blob);
            p.drawEllipse(c + QPointF(std::cos(a) * dist, std::sin(a) * dist),
                          dr, dr);
        }
    }
    return out;
}

QImage spiralField(const QSize &size, const QPointF &center, qreal radius,
                   quint32 seed, const QList<QColor> &palette, int count)
{
    if (!size.isValid() || size.isEmpty() || count <= 0 || radius <= 0.0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    Rng rng(seed);
    const qreal phase = qreal(rng.unit()) * 6.2831853;
    for (int i = 0; i < count; ++i)
    {
        const qreal t = qreal(i) / qreal(count);
        const qreal r = radius * (0.10 + 0.9 * t)
                        * (0.94 + 0.06 * qreal(rng.unit()));
        const qreal angle = phase + t * 15.708 + qreal(rng.range(-0.09f, 0.09f));
        const QPointF pos = center + QPointF(std::cos(angle) * r,
                                             std::sin(angle) * r);
        const qreal s = (1.0 + 3.4 * (1.0 - t)) * (0.7 + 0.6 * qreal(rng.unit()));
        QColor c = palette.isEmpty() ? QColor(255, 255, 255)
                                     : palette.at(rng.pick(palette.size()));
        c.setAlpha(int(clamp01(0.25f + 0.75f * float(1.0 - t)) * 235.0f));
        p.setBrush(c);
        p.drawEllipse(pos, s, s);
        if (rng.unit() < 0.35f)
        {
            QColor tail = c;
            tail.setAlpha(int(qreal(c.alpha()) * 0.45));
            QPen pen(tail);
            pen.setCapStyle(Qt::RoundCap);
            pen.setWidthF(std::max(0.8, s * 0.6));
            p.setPen(pen);
            p.drawLine(pos, pos - QPointF(std::cos(angle), std::sin(angle))
                                  * (s * 3.0));
            p.setPen(Qt::NoPen);
        }
    }
    return out;
}

QImage confettiField(const QSize &size, quint32 seed,
                     const QList<QColor> &palette, int count, qreal minSize,
                     qreal maxSize)
{
    if (!size.isValid() || size.isEmpty() || count <= 0)
        return {};
    QImage out(size, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    Rng rng(seed);
    for (int i = 0; i < count; ++i)
    {
        const QPointF c(rng.range(0.0f, float(size.width())),
                        rng.range(0.0f, float(size.height())));
        const qreal w = minSize + (maxSize - minSize) * qreal(rng.unit());
        const qreal h = w * qreal(rng.range(0.35f, 0.8f));
        const QColor base = palette.isEmpty()
                                ? QColor(255, 255, 255)
                                : palette.at(rng.pick(palette.size()));
        QColor cfill = base;
        cfill.setAlpha(int(rng.range(170.0f, 255.0f)));
        p.save();
        p.translate(c);
        p.rotate(rng.range(0.0f, 360.0f));
        p.setPen(Qt::NoPen);
        p.setBrush(cfill);
        if (rng.unit() < 0.55f)
            p.drawRoundedRect(QRectF(-w / 2.0, -h / 2.0, w, h), h * 0.28,
                              h * 0.28);
        else
            p.drawEllipse(QPointF(0, 0), w * 0.42, h * 0.42);
        p.restore();
    }
    return out;
}

QImage starField(const QSize &size, quint32 seed, const QColor &color,
                 int count)
{
    return starField(size, seed, QList<QColor>{ color }, count);
}

QImage wedgeKaleidoscope(const QImage &subjectCanvas, const QSize &stage,
                         int wedges, qreal zoom, qreal rotation)
{
    if (!stage.isValid() || stage.isEmpty() || subjectCanvas.isNull()
        || wedges < 2)
        return {};
    QImage out(stage, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QPointF c(stage.width() / 2.0, stage.height() / 2.0);
    const qreal reach = std::hypot(qreal(stage.width()), qreal(stage.height()));
    const qreal half = 180.0 / qreal(wedges);
    const QRectF circle(c.x() - reach, c.y() - reach, reach * 2.0,
                        reach * 2.0);

    for (int i = 0; i < wedges; ++i)
    {
        const qreal angle = rotation + 360.0 * qreal(i) / qreal(wedges);
        QPainterPath base;
        base.moveTo(c);
        base.arcTo(circle, -half, half * 2.0);
        base.closeSubpath();
        QTransform rot;
        rot.translate(c.x(), c.y());
        rot.rotate(angle);
        rot.translate(-c.x(), -c.y());
        p.save();
        p.setClipPath(rot.map(base));
        p.translate(c);
        p.rotate(angle);
        p.scale(zoom, zoom);
        p.translate(-c);
        p.drawImage(0, 0, subjectCanvas);
        p.restore();
    }
    return out;
}

} // namespace creative
} // namespace paintbrush
