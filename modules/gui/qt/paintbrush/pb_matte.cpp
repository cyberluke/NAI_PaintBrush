/*****************************************************************************
 * pb_matte.cpp : NAI Paintbrush — neural matting and rotoscoping core
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_matte.hpp"

#include <QPainter>
#include <QColor>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace paintbrush {
namespace matte {

namespace {

constexpr float kInf = 1e20f;

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float smoothstepf(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
        return x >= edge1 ? 1.0f : 0.0f;
    const float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* sRGB <-> linear light (IEC 61966-2-1). Foreground reconstruction is solved
 * in linear light; the stored Qt textures keep sRGB, which is what Qt's
 * premultiplied formats encode. */
inline float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c)
{
    c = clampf(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline int luminanceOf(QRgb p)
{
    return (qRed(p) * 299 + qGreen(p) * 587 + qBlue(p) * 114) / 1000;
}

/* Felzenszwalb-Huttenlocher exact 1-D squared distance transform. */
void dt1d(const float *f, float *d, int n, int *v, float *z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q)
    {
        float s = ((f[q] + float(q) * float(q))
                   - (f[v[k]] + float(v[k]) * float(v[k])))
                  / (2.0f * float(q) - 2.0f * float(v[k]));
        while (s <= z[k] && k > 0)
        {
            --k;
            s = ((f[q] + float(q) * float(q))
                 - (f[v[k]] + float(v[k]) * float(v[k])))
                / (2.0f * float(q) - 2.0f * float(v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q)
    {
        while (z[k + 1] < float(q))
            ++k;
        d[q] = float(q - v[k]) * float(q - v[k]) + f[v[k]];
    }
}

/* Squared distance to the nearest seed (seed = 1). */
std::vector<float> distanceSquaredFrom(const std::vector<uint8_t> &seed,
                                       int w, int h)
{
    std::vector<float> grid(size_t(w) * size_t(h), kInf);
    for (size_t i = 0; i < grid.size(); ++i)
        if (seed[i])
            grid[i] = 0.0f;

    const int n = std::max(w, h);
    std::vector<float> line(size_t(n), 0.0f);
    std::vector<float> out(size_t(n), 0.0f);
    std::vector<int> v(size_t(n) + 1, 0);
    std::vector<float> z(size_t(n) + 2, 0.0f);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
            line[size_t(x)] = grid[size_t(y) * size_t(w) + size_t(x)];
        dt1d(line.data(), out.data(), w, v.data(), z.data());
        for (int x = 0; x < w; ++x)
            grid[size_t(y) * size_t(w) + size_t(x)] = out[size_t(x)];
    }
    for (int x = 0; x < w; ++x)
    {
        for (int y = 0; y < h; ++y)
            line[size_t(y)] = grid[size_t(y) * size_t(w) + size_t(x)];
        dt1d(line.data(), out.data(), h, v.data(), z.data());
        for (int y = 0; y < h; ++y)
            grid[size_t(y) * size_t(w) + size_t(x)] = out[size_t(y)];
    }
    return grid;
}

struct Labels
{
    int count = 0;
    std::vector<int> label;    /* 1..count, 0 = background                */
    std::vector<int> area;     /* index by label (0 unused)               */
    std::vector<QRect> bounds;
};

Labels labelComponents(const std::vector<uint8_t> &binary, int w, int h,
                       bool eightConnected)
{
    Labels out;
    out.label.assign(size_t(w) * size_t(h), 0);
    /* index 0 is the background label */
    out.area.push_back(0);
    out.bounds.push_back(QRect());
    std::vector<int> stack;
    stack.reserve(1024);

    static const int dx4[4] = {1, -1, 0, 0};
    static const int dy4[4] = {0, 0, 1, -1};
    static const int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dy8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const int *dx = eightConnected ? dx8 : dx4;
    const int *dy = eightConnected ? dy8 : dy4;
    const int neighbours = eightConnected ? 8 : 4;

    int next = 0;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const size_t idx = size_t(y) * size_t(w) + size_t(x);
            if (!binary[idx] || out.label[idx] != 0)
                continue;
            const int id = ++next;
            int area = 0;
            int minX = x, minY = y, maxX = x, maxY = y;
            stack.clear();
            stack.push_back(int(idx));
            out.label[idx] = id;
            while (!stack.empty())
            {
                const int cur = stack.back();
                stack.pop_back();
                const int cx = cur % w;
                const int cy = cur / w;
                ++area;
                minX = std::min(minX, cx);
                maxX = std::max(maxX, cx);
                minY = std::min(minY, cy);
                maxY = std::max(maxY, cy);
                for (int k = 0; k < neighbours; ++k)
                {
                    const int nx = cx + dx[k];
                    const int ny = cy + dy[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const size_t nidx = size_t(ny) * size_t(w) + size_t(nx);
                    if (!binary[nidx] || out.label[nidx] != 0)
                        continue;
                    out.label[nidx] = id;
                    stack.push_back(int(nidx));
                }
            }
            out.area.push_back(area);
            out.bounds.push_back(QRect(QPoint(minX, minY),
                                       QPoint(maxX, maxY)));
        }
    }
    out.count = next;
    return out;
}

QImage grayToBinary(const QImage &levels)
{
    QImage bin(levels.size(), QImage::Format_Grayscale8);
    const int w = levels.width();
    const int h = levels.height();
    for (int y = 0; y < h; ++y)
    {
        const uchar *src = levels.constScanLine(y);
        uchar *dst = bin.scanLine(y);
        for (int x = 0; x < w; ++x)
            dst[x] = src[x] >= 128 ? 255 : 0;
    }
    return bin;
}

/* Separable box average of a binary image. A 2r+1 window produces the
 * sub-pixel coverage ramp used by Refined mode; it is deliberately tiny
 * (r = 1, or 2 on very large frames) so thin structures survive. */
QImage boxBlurBinary(const QImage &bin, int radius)
{
    if (bin.isNull() || radius <= 0)
        return bin;
    const int w = bin.width();
    const int h = bin.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    QImage out(w, h, QImage::Format_Grayscale8);
    const int span = radius * 2 + 1;
    for (int y = 0; y < h; ++y)
    {
        const uchar *src = bin.constScanLine(y);
        uchar *dst = tmp.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            int sum = 0;
            for (int dx = -radius; dx <= radius; ++dx)
                sum += src[clampi(x + dx, 0, w - 1)];
            dst[x] = uchar(sum / span);
        }
    }
    for (int y = 0; y < h; ++y)
    {
        uchar *dst = out.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            int sum = 0;
            for (int dy = -radius; dy <= radius; ++dy)
                sum += tmp.constScanLine(clampi(y + dy, 0, h - 1))[x];
            dst[x] = uchar(sum / span);
        }
    }
    return out;
}

/* Normalized structure energy (Sobel magnitude, spread by a small box blur).
 * Used ONLY to widen the unknown band around hair/fur/edges — never as a
 * confidence value. */
std::vector<float> detailEnergy(const QImage &rgb, int w, int h)
{
    std::vector<float> gray(size_t(w) * size_t(h), 0.0f);
    QImage src = rgb.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x)
            gray[size_t(y) * size_t(w) + size_t(x)] = float(luminanceOf(line[x]));
    }

    std::vector<float> mag(size_t(w) * size_t(h), 0.0f);
    for (int y = 1; y < h - 1; ++y)
    {
        for (int x = 1; x < w - 1; ++x)
        {
            const float gx =
                -gray[size_t(y - 1) * size_t(w) + size_t(x - 1)]
                + gray[size_t(y - 1) * size_t(w) + size_t(x + 1)]
                - 2.0f * gray[size_t(y) * size_t(w) + size_t(x - 1)]
                + 2.0f * gray[size_t(y) * size_t(w) + size_t(x + 1)]
                - gray[size_t(y + 1) * size_t(w) + size_t(x - 1)]
                + gray[size_t(y + 1) * size_t(w) + size_t(x + 1)];
            const float gy =
                -gray[size_t(y - 1) * size_t(w) + size_t(x - 1)]
                - 2.0f * gray[size_t(y - 1) * size_t(w) + size_t(x)]
                - gray[size_t(y - 1) * size_t(w) + size_t(x + 1)]
                + gray[size_t(y + 1) * size_t(w) + size_t(x - 1)]
                + 2.0f * gray[size_t(y + 1) * size_t(w) + size_t(x)]
                + gray[size_t(y + 1) * size_t(w) + size_t(x + 1)];
            mag[size_t(y) * size_t(w) + size_t(x)] =
                std::sqrt(gx * gx + gy * gy) / 1020.0f;
        }
    }

    /* 5px box blur so the widening is spatially coherent, then normalize by a
     * high percentile (not the max: a single specular pixel must not flatten
     * the whole band). */
    std::vector<float> blurred(size_t(w) * size_t(h), 0.0f);
    constexpr int r = 2;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0f;
            int n = 0;
            for (int dy = -r; dy <= r; ++dy)
            {
                const int yy = clampi(y + dy, 0, h - 1);
                for (int dx = -r; dx <= r; ++dx)
                {
                    const int xx = clampi(x + dx, 0, w - 1);
                    sum += mag[size_t(yy) * size_t(w) + size_t(xx)];
                    ++n;
                }
            }
            blurred[size_t(y) * size_t(w) + size_t(x)] = sum / float(n);
        }
    }
    std::vector<float> sample = blurred;
    const size_t p90 = size_t(double(sample.size()) * 0.90);
    std::nth_element(sample.begin(), sample.begin() + std::min(p90, sample.size() - 1),
                     sample.end());
    const float norm = std::max(1e-4f, sample[std::min(p90, sample.size() - 1)]);
    for (float &v : blurred)
        v = clampf(v / norm, 0.0f, 1.0f);
    return blurred;
}

/* --- small float pyramid used for background / foreground colour inpainting */
struct Level
{
    int w = 0;
    int h = 0;
    std::vector<float> rgb; /* 3 floats per pixel, linear light */
    std::vector<float> wt;
    /* Reliability of the pulled colour, 0..1: 1 at a real sample and a smooth
     * distance falloff away from one. Pulled colours do not create support, so
     * "no background visible" cannot be mistaken for a reliable estimate. */
    std::vector<float> support;
};

struct Pyramid
{
    std::vector<Level> levels;

    static Pyramid build(const QImage &rgbLinear, int maxShortEdge,
                         const std::vector<uint8_t> &known,
                         float falloffPx = 6.0f)
    {
        Pyramid p;
        const int fw = rgbLinear.width();
        const int fh = rgbLinear.height();
        const double s = std::min(1.0, double(maxShortEdge) / double(std::max(1, std::min(fw, fh))));
        const int w = std::max(1, int(std::lround(fw * s)));
        const int h = std::max(1, int(std::lround(fh * s)));
        Level base;
        base.w = w;
        base.h = h;
        base.rgb.assign(size_t(w) * size_t(h) * 3, 0.0f);
        base.wt.assign(size_t(w) * size_t(h), 0.0f);
        base.support.assign(size_t(w) * size_t(h), 0.0f);

        QImage src = rgbLinear.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < h; ++y)
        {
            const int sy = std::min(fh - 1, int(double(y) / s));
            const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(sy));
            for (int x = 0; x < w; ++x)
            {
                const int sx = std::min(fw - 1, int(double(x) / s));
                const size_t fi = size_t(sy) * size_t(fw) + size_t(sx);
                if (!known[fi])
                    continue;
                const size_t bi = size_t(y) * size_t(w) + size_t(x);
                base.rgb[bi * 3 + 0] = srgbToLinear(float(qRed(line[sx])) / 255.0f);
                base.rgb[bi * 3 + 1] = srgbToLinear(float(qGreen(line[sx])) / 255.0f);
                base.rgb[bi * 3 + 2] = srgbToLinear(float(qBlue(line[sx])) / 255.0f);
                base.wt[bi] = 1.0f;
            }
        }
        /* Reliability falls off with distance from a real sample. A boundary
         * pixel 1-2 px away from visible background is trustworthy; a pixel
         * deep inside an occluded region is not. */
        {
            std::vector<uint8_t> baseSeed(size_t(w) * size_t(h), 0);
            for (size_t i = 0; i < baseSeed.size(); ++i)
                baseSeed[i] = base.wt[i] > 0.0f ? 1 : 0;
            const std::vector<float> d2 = distanceSquaredFrom(baseSeed, w, h);
            const float falloff = std::max(1.0f, falloffPx);
            for (size_t i = 0; i < base.support.size(); ++i)
                base.support[i] = 1.0f / (1.0f + std::sqrt(d2[i]) / falloff);
        }
        p.levels.push_back(std::move(base));

        while (p.levels.back().w > 1 || p.levels.back().h > 1)
        {
            const Level &prev = p.levels.back();
            Level next;
            next.w = std::max(1, (prev.w + 1) / 2);
            next.h = std::max(1, (prev.h + 1) / 2);
            next.rgb.assign(size_t(next.w) * size_t(next.h) * 3, 0.0f);
            next.wt.assign(size_t(next.w) * size_t(next.h), 0.0f);
            next.support.assign(size_t(next.w) * size_t(next.h), 0.0f);
            for (int y = 0; y < next.h; ++y)
            {
                for (int x = 0; x < next.w; ++x)
                {
                    float sum[3] = {0, 0, 0};
                    float weight = 0.0f;
                    for (int dy = 0; dy < 2; ++dy)
                    {
                        const int sy = y * 2 + dy;
                        if (sy >= prev.h)
                            continue;
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            const int sx = x * 2 + dx;
                            if (sx >= prev.w)
                                continue;
                            const size_t si = size_t(sy) * size_t(prev.w) + size_t(sx);
                            const float wgt = prev.wt[si];
                            if (wgt <= 0.0f)
                                continue;
                            sum[0] += prev.rgb[si * 3 + 0] * wgt;
                            sum[1] += prev.rgb[si * 3 + 1] * wgt;
                            sum[2] += prev.rgb[si * 3 + 2] * wgt;
                            weight += wgt;
                        }
                    }
                    const size_t di = size_t(y) * size_t(next.w) + size_t(x);
                    if (weight > 0.0f)
                    {
                        next.rgb[di * 3 + 0] = sum[0] / weight;
                        next.rgb[di * 3 + 1] = sum[1] / weight;
                        next.rgb[di * 3 + 2] = sum[2] / weight;
                        next.wt[di] = std::min(1.0f, weight * 0.25f);
                    }
                    /* Reliability of the coarser cell is the maximum of its
                     * children: one real sample makes the region usable. */
                    float childSupport = 0.0f;
                    for (int dy = 0; dy < 2; ++dy)
                    {
                        const int sy = y * 2 + dy;
                        if (sy >= prev.h)
                            continue;
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            const int sx = x * 2 + dx;
                            if (sx >= prev.w)
                                continue;
                            childSupport = std::max(
                                childSupport,
                                prev.support[size_t(sy) * size_t(prev.w) + size_t(sx)]);
                        }
                    }
                    next.support[di] = childSupport;
                }
            }
            p.levels.push_back(std::move(next));
        }

        /* pull: coarse colours fill the holes of the finer level */
        for (int li = int(p.levels.size()) - 2; li >= 0; --li)
        {
            Level &fine = p.levels[size_t(li)];
            const Level &coarse = p.levels[size_t(li) + 1];
            for (int y = 0; y < fine.h; ++y)
            {
                const float cy = (float(y) + 0.5f) * float(coarse.h) / float(fine.h) - 0.5f;
                const int y0 = clampi(int(std::floor(cy)), 0, coarse.h - 1);
                const int y1 = clampi(y0 + 1, 0, coarse.h - 1);
                const float fy = clampf(cy - float(y0), 0.0f, 1.0f);
                for (int x = 0; x < fine.w; ++x)
                {
                    const size_t di = size_t(y) * size_t(fine.w) + size_t(x);
                    const float cx = (float(x) + 0.5f) * float(coarse.w) / float(fine.w) - 0.5f;
                    const int x0 = clampi(int(std::floor(cx)), 0, coarse.w - 1);
                    const int x1 = clampi(x0 + 1, 0, coarse.w - 1);
                    const float fx = clampf(cx - float(x0), 0.0f, 1.0f);
                    const size_t i00 = size_t(y0) * size_t(coarse.w) + size_t(x0);
                    const size_t i10 = size_t(y0) * size_t(coarse.w) + size_t(x1);
                    const size_t i01 = size_t(y1) * size_t(coarse.w) + size_t(x0);
                    const size_t i11 = size_t(y1) * size_t(coarse.w) + size_t(x1);
                    float c[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const float a = coarse.rgb[i00 * 3 + k] * (1 - fx)
                                        + coarse.rgb[i10 * 3 + k] * fx;
                        const float b = coarse.rgb[i01 * 3 + k] * (1 - fx)
                                        + coarse.rgb[i11 * 3 + k] * fx;
                        c[k] = a * (1 - fy) + b * fy;
                    }
                    const float wgt = fine.wt[di];
                    if (wgt >= 0.999f)
                        continue;
                    const float blend = 1.0f - wgt;
                    fine.rgb[di * 3 + 0] = fine.rgb[di * 3 + 0] * wgt + c[0] * blend;
                    fine.rgb[di * 3 + 1] = fine.rgb[di * 3 + 1] * wgt + c[1] * blend;
                    fine.rgb[di * 3 + 2] = fine.rgb[di * 3 + 2] * wgt + c[2] * blend;
                    fine.wt[di] = 1.0f;
                }
            }
        }
        return p;
    }

    int fullW_ = 0;
    int fullH_ = 0;

    void sample(float fullX, float fullY, float out[3]) const
    {
        const Level &l = levels.front();
        const float cx = (fullX + 0.5f) * float(l.w) / float(fullW_) - 0.5f;
        const float cy = (fullY + 0.5f) * float(l.h) / float(fullH_) - 0.5f;
        const int x0 = clampi(int(std::floor(cx)), 0, l.w - 1);
        const int x1 = clampi(x0 + 1, 0, l.w - 1);
        const int y0 = clampi(int(std::floor(cy)), 0, l.h - 1);
        const int y1 = clampi(y0 + 1, 0, l.h - 1);
        const float fx = clampf(cx - float(x0), 0.0f, 1.0f);
        const float fy = clampf(cy - float(y0), 0.0f, 1.0f);
        const size_t i00 = size_t(y0) * size_t(l.w) + size_t(x0);
        const size_t i10 = size_t(y0) * size_t(l.w) + size_t(x1);
        const size_t i01 = size_t(y1) * size_t(l.w) + size_t(x0);
        const size_t i11 = size_t(y1) * size_t(l.w) + size_t(x1);
        for (int k = 0; k < 3; ++k)
        {
            const float a = l.rgb[i00 * 3 + k] * (1 - fx) + l.rgb[i10 * 3 + k] * fx;
            const float b = l.rgb[i01 * 3 + k] * (1 - fx) + l.rgb[i11 * 3 + k] * fx;
            out[k] = a * (1 - fy) + b * fy;
        }
    }

    /* Bilinear support (0 = no real background/foreground sample anywhere
     * near this pixel, 1 = full support). */
    float supportAt(float fullX, float fullY) const
    {
        const Level &l = levels.front();
        const float cx = (fullX + 0.5f) * float(l.w) / float(std::max(1, fullW_)) - 0.5f;
        const float cy = (fullY + 0.5f) * float(l.h) / float(std::max(1, fullH_)) - 0.5f;
        const int x0 = clampi(int(std::floor(cx)), 0, l.w - 1);
        const int x1 = clampi(x0 + 1, 0, l.w - 1);
        const int y0 = clampi(int(std::floor(cy)), 0, l.h - 1);
        const int y1 = clampi(y0 + 1, 0, l.h - 1);
        const float fx = clampf(cx - float(x0), 0.0f, 1.0f);
        const float fy = clampf(cy - float(y0), 0.0f, 1.0f);
        const float a = float(l.support[size_t(y0) * size_t(l.w) + size_t(x0)]) * (1 - fx)
                        + float(l.support[size_t(y0) * size_t(l.w) + size_t(x1)]) * fx;
        const float b = float(l.support[size_t(y1) * size_t(l.w) + size_t(x0)]) * (1 - fx)
                        + float(l.support[size_t(y1) * size_t(l.w) + size_t(x1)]) * fx;
        return a * (1 - fy) + b * fy;
    }
};

QImage levelMaskImage(const std::vector<float> &values, int w, int h,
                      float scale, float offset = 0.0f)
{
    QImage img(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y)
    {
        uchar *line = img.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const float v = values[size_t(y) * size_t(w) + size_t(x)];
            line[x] = uchar(clampi(int(std::lround((v + offset) * scale)), 0, 255));
        }
    }
    return img;
}

} // namespace

/* --------------------------------------------------------------- Parameters */

double Parameters::scaleFor(int shortEdge) const
{
    if (shortEdge <= 0 || referenceShortEdge <= 0)
        return 1.0;
    return clampd(double(shortEdge) / double(referenceShortEdge), 0.25, 4.0);
}

int Parameters::innerBandFor(int shortEdge) const
{
    return std::max(1, int(std::lround(innerBandPx * scaleFor(shortEdge))));
}

int Parameters::outerBandFor(int shortEdge) const
{
    return std::max(1, int(std::lround(outerBandPx * scaleFor(shortEdge))));
}

int Parameters::detailBandFor(int shortEdge) const
{
    return std::max(innerBandFor(shortEdge) + 1,
                    int(std::lround(fineDetailBandPx * scaleFor(shortEdge))));
}

int Parameters::maxBandFor(int shortEdge) const
{
    return std::max(detailBandFor(shortEdge),
                    int(std::lround(maxBandPx * scaleFor(shortEdge))));
}

/* ------------------------------------------------------------------- alpha */

AlphaPlane::AlphaPlane(int width, int height)
    : m_width(std::max(0, width)), m_height(std::max(0, height))
{
    if (m_width > 0 && m_height > 0)
        m_data.assign(size_t(m_width) * size_t(m_height), 0);
}

void AlphaPlane::fill(float value)
{
    const quint16 v = quint16(clampi(int(std::lround(clampf(value, 0.0f, 1.0f) * 65535.0f)), 0, 65535));
    std::fill(m_data.begin(), m_data.end(), v);
}

float AlphaPlane::at(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return 0.0f;
    return float(m_data[size_t(y) * size_t(m_width) + size_t(x)]) / 65535.0f;
}

void AlphaPlane::setAt(int x, int y, float value)
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return;
    m_data[size_t(y) * size_t(m_width) + size_t(x)] =
        quint16(clampi(int(std::lround(clampf(value, 0.0f, 1.0f) * 65535.0f)), 0, 65535));
}

QImage AlphaPlane::toGrayscale8() const
{
    if (isNull())
        return {};
    QImage img(m_width, m_height, QImage::Format_Grayscale8);
    for (int y = 0; y < m_height; ++y)
    {
        uchar *line = img.scanLine(y);
        for (int x = 0; x < m_width; ++x)
            line[x] = uchar((m_data[size_t(y) * size_t(m_width) + size_t(x)] * 255u + 32767u) / 65535u);
    }
    return img;
}

QImage AlphaPlane::toGrayscale16() const
{
    if (isNull())
        return {};
    QImage img(m_width, m_height, QImage::Format_Grayscale16);
    for (int y = 0; y < m_height; ++y)
    {
        quint16 *line = reinterpret_cast<quint16 *>(img.scanLine(y));
        for (int x = 0; x < m_width; ++x)
            line[x] = m_data[size_t(y) * size_t(m_width) + size_t(x)];
    }
    return img;
}

QImage AlphaPlane::toPremultipliedRgba(const QImage &foregroundRgb) const
{
    if (isNull() || foregroundRgb.isNull())
        return {};
    QImage src = foregroundRgb.convertToFormat(QImage::Format_RGB32);
    if (src.size() != size())
        src = src.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage out(m_width, m_height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < m_height; ++y)
    {
        const QRgb *sline = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *dline = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < m_width; ++x)
        {
            const int a = int((m_data[size_t(y) * size_t(m_width) + size_t(x)] * 255u + 32767u) / 65535u);
            const int r = qRed(sline[x]) * a / 255;
            const int g = qGreen(sline[x]) * a / 255;
            const int b = qBlue(sline[x]) * a / 255;
            dline[x] = qRgba(r, g, b, a);
        }
    }
    return out;
}

AlphaPlane AlphaPlane::fromFloat(const float *values, int width, int height)
{
    AlphaPlane plane(width, height);
    if (!values || plane.isNull())
        return plane;
    for (size_t i = 0; i < plane.m_data.size(); ++i)
        plane.m_data[i] = quint16(clampi(int(std::lround(clampf(values[i], 0.0f, 1.0f) * 65535.0f)), 0, 65535));
    return plane;
}

/* --------------------------------------------------------------- utilities */

QImage levelsOf(const QImage &mask)
{
    if (mask.isNull())
        return {};

    if (mask.format() == QImage::Format_Grayscale8)
        return mask;

    QImage gray(mask.size(), QImage::Format_Grayscale8);
    const int w = mask.width();
    const int h = mask.height();
    QImage src = mask.convertToFormat(QImage::Format_ARGB32);
    const bool useAlpha = mask.hasAlphaChannel();
    for (int y = 0; y < h; ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *dst = gray.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            if (useAlpha)
            {
                const int a = qAlpha(line[x]);
                /* An opaque white-on-black mask stored as ARGB must use its
                 * luminance, not the (opaque) alpha. Use alpha only when it is
                 * actually carrying the mask (some pixel is not opaque). */
                dst[x] = uchar(a == 255 ? luminanceOf(line[x]) : a);
            }
            else
                dst[x] = uchar(luminanceOf(line[x]));
        }
    }
    return gray;
}

bool hasFractionalLevels(const QImage &mask)
{
    if (mask.isNull())
        return false;
    const QImage levels = levelsOf(mask);
    for (int y = 0; y < levels.height(); ++y)
    {
        const uchar *line = levels.constScanLine(y);
        for (int x = 0; x < levels.width(); ++x)
        {
            const uchar v = line[x];
            if (v != 0 && v != 255)
                return true;
        }
    }
    return false;
}

QImage distanceTransformSquared(const QImage &seeds)
{
    if (seeds.isNull())
        return {};
    const QImage bin = grayToBinary(levelsOf(seeds));
    const int w = bin.width();
    const int h = bin.height();
    std::vector<uint8_t> seed(size_t(w) * size_t(h), 0);
    for (int y = 0; y < h; ++y)
    {
        const uchar *line = bin.constScanLine(y);
        for (int x = 0; x < w; ++x)
            seed[size_t(y) * size_t(w) + size_t(x)] = line[x] ? 1 : 0;
    }
    const std::vector<float> d2 = distanceSquaredFrom(seed, w, h);
    /* Grayscale16, 1/16 px units, clamped. */
    QImage out(w, h, QImage::Format_Grayscale16);
    for (int y = 0; y < h; ++y)
    {
        quint16 *line = reinterpret_cast<quint16 *>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const float d = std::sqrt(d2[size_t(y) * size_t(w) + size_t(x)]);
            line[x] = quint16(clampi(int(std::lround(d * 16.0f)), 0, 65535));
        }
    }
    return out;
}

QImage signedDistance(const QImage &binaryMask)
{
    if (binaryMask.isNull())
        return {};
    const QImage bin = grayToBinary(levelsOf(binaryMask));
    const int w = bin.width();
    const int h = bin.height();
    std::vector<uint8_t> fg(size_t(w) * size_t(h), 0);
    std::vector<uint8_t> bg(size_t(w) * size_t(h), 0);
    for (int y = 0; y < h; ++y)
    {
        const uchar *line = bin.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const bool on = line[x] != 0;
            fg[size_t(y) * size_t(w) + size_t(x)] = on ? 1 : 0;
            bg[size_t(y) * size_t(w) + size_t(x)] = on ? 0 : 1;
        }
    }
    const std::vector<float> dOut = distanceSquaredFrom(fg, w, h);
    const std::vector<float> dIn = distanceSquaredFrom(bg, w, h);
    /* Signed in 1/16 px units, offset into Grayscale16 (128 px bias). */
    QImage out(w, h, QImage::Format_Grayscale16);
    for (int y = 0; y < h; ++y)
    {
        quint16 *line = reinterpret_cast<quint16 *>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float sd = std::sqrt(dOut[i]) - std::sqrt(dIn[i]);
            line[x] = quint16(clampi(int(std::lround((sd + 128.0f) * 16.0f)), 0, 65535));
        }
    }
    return out;
}

QImage erodeBinary(const QImage &binaryMask, int radius)
{
    if (binaryMask.isNull() || radius <= 0)
        return levelsOf(binaryMask);
    const QImage bin = grayToBinary(levelsOf(binaryMask));
    const int w = bin.width();
    const int h = bin.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    QImage out(w, h, QImage::Format_Grayscale8);

    /* separable min filter: horizontal, then vertical */
    for (int y = 0; y < h; ++y)
    {
        const uchar *src = bin.constScanLine(y);
        uchar *dst = tmp.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            uchar best = 255;
            const int lo = std::max(0, x - radius);
            const int hi = std::min(w - 1, x + radius);
            for (int xx = lo; xx <= hi; ++xx)
                best = std::min(best, src[xx]);
            dst[x] = best;
        }
    }
    for (int y = 0; y < h; ++y)
    {
        uchar *dst = out.scanLine(y);
        const int lo = std::max(0, y - radius);
        const int hi = std::min(h - 1, y + radius);
        for (int x = 0; x < w; ++x)
        {
            uchar best = 255;
            for (int yy = lo; yy <= hi; ++yy)
                best = std::min(best, tmp.constScanLine(yy)[x]);
            dst[x] = best;
        }
    }
    return out;
}

QImage dilateBinary(const QImage &binaryMask, int radius)
{
    if (binaryMask.isNull() || radius <= 0)
        return levelsOf(binaryMask);
    const QImage bin = grayToBinary(levelsOf(binaryMask));
    const int w = bin.width();
    const int h = bin.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    QImage out(w, h, QImage::Format_Grayscale8);

    for (int y = 0; y < h; ++y)
    {
        const uchar *src = bin.constScanLine(y);
        uchar *dst = tmp.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            uchar best = 0;
            const int lo = std::max(0, x - radius);
            const int hi = std::min(w - 1, x + radius);
            for (int xx = lo; xx <= hi; ++xx)
                best = std::max(best, src[xx]);
            dst[x] = best;
        }
    }
    for (int y = 0; y < h; ++y)
    {
        uchar *dst = out.scanLine(y);
        const int lo = std::max(0, y - radius);
        const int hi = std::min(h - 1, y + radius);
        for (int x = 0; x < w; ++x)
        {
            uchar best = 0;
            for (int yy = lo; yy <= hi; ++yy)
                best = std::max(best, tmp.constScanLine(yy)[x]);
            dst[x] = best;
        }
    }
    return out;
}

/* -------------------------------------------------------------- topology */

TopologyReport analyseTopology(const Inputs &inputs, const Parameters &params)
{
    TopologyReport report;
    if (inputs.mask.isNull())
        return report;

    const QImage levels = levelsOf(inputs.mask);
    const QImage bin = grayToBinary(levels);
    const int w = bin.width();
    const int h = bin.height();
    const int shortEdge = std::min(w, h);

    std::vector<uint8_t> fg(size_t(w) * size_t(h), 0);
    for (int y = 0; y < h; ++y)
    {
        const uchar *line = bin.constScanLine(y);
        for (int x = 0; x < w; ++x)
            fg[size_t(y) * size_t(w) + size_t(x)] = line[x] ? 1 : 0;
    }

    const Labels comps = labelComponents(fg, w, h, true);
    if (comps.count == 0)
    {
        report.repairedMask = bin;
        report.removedMask = QImage(w, h, QImage::Format_Grayscale8);
        report.removedMask.fill(0);
        report.filledHoleMask = report.removedMask;
        report.deferredHoleMask = report.removedMask;
        return report;
    }

    int mainLabel = 1;
    for (int id = 2; id <= comps.count; ++id)
        if (comps.area[size_t(id)] > comps.area[size_t(mainLabel)])
            mainLabel = id;
    const int mainArea = comps.area[size_t(mainLabel)];

    /* Distance to the main component (in pixels). */
    std::vector<uint8_t> mainSeed(size_t(w) * size_t(h), 0);
    for (size_t i = 0; i < fg.size(); ++i)
        if (comps.label[i] == mainLabel)
            mainSeed[i] = 1;
    const std::vector<float> mainDist2 = distanceSquaredFrom(mainSeed, w, h);

    auto labelAt = [&](const QPointF &p) -> int {
        const int x = int(std::floor(p.x()));
        const int y = int(std::floor(p.y()));
        if (x < 0 || y < 0 || x >= w || y >= h)
            return 0;
        return comps.label[size_t(y) * size_t(w) + size_t(x)];
    };

    std::vector<char> positiveLabels(size_t(comps.count) + 1, 0);
    if (params.keepUserComponents)
        for (const QPointF &p : inputs.positivePoints)
            if (const int id = labelAt(p))
                positiveLabels[size_t(id)] = 1;

    std::vector<char> keep(size_t(comps.count) + 1, 0);
    keep[size_t(mainLabel)] = 1;

    const double nearBand = double(params.maxBandFor(shortEdge)) * 1.5;

    for (int id = 1; id <= comps.count; ++id)
    {
        if (id == mainLabel)
            continue;
        ++report.detachedFound;
        Component c;
        c.kind = Component::Kind::Foreground;
        c.label = id;
        c.area = comps.area[size_t(id)];
        c.relativeArea = mainArea > 0 ? double(c.area) / double(mainArea) : 0.0;
        c.bounds = comps.bounds[size_t(id)];
        c.userSeeded = positiveLabels[size_t(id)] != 0;

        double minDist = 1e18;
        for (int y = c.bounds.top(); y <= c.bounds.bottom(); ++y)
        {
            for (int x = c.bounds.left(); x <= c.bounds.right(); ++x)
            {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                if (comps.label[i] != id)
                    continue;
                minDist = std::min(minDist, double(std::sqrt(mainDist2[i])));
            }
        }
        c.distanceToMain = std::isfinite(minDist) ? minDist : 1e9;

        /* Every removal needs BOTH a user-facing suppression level and
         * evidence that the component is an isolated speck: tiny relative to
         * the FRAME (not merely to the subject) and far from the subject. A
         * detached braid or a second visible part can be a few percent of the
         * subject and is never deleted by size alone. */
        const double isolation = clampd(c.distanceToMain / std::max(1.0, nearBand), 0.0, 1.0);
        const double sizeScore = 1.0 - clampd(c.relativeArea / 0.02, 0.0, 1.0);
        const double artifactScore = isolation * sizeScore;
        const bool frameTiny = double(c.area) <= 0.0035 * double(w) * double(h);
        const bool nearSubject = c.distanceToMain <= nearBand;

        if (c.userSeeded)
        {
            c.kept = true;
            c.reason = QStringLiteral("positively seeded by the user");
        }
        else if (frameTiny && params.artifactSuppression >= 0.15
                 && artifactScore >= 0.55)
        {
            c.kept = false;
            c.reason = QStringLiteral("isolated speck: %1 px, %2 px away")
                           .arg(c.area)
                           .arg(c.distanceToMain, 0, 'f', 1);
        }
        else if (nearSubject)
        {
            c.kept = true;
            c.reason = QStringLiteral("near the subject (%1 px)")
                           .arg(c.distanceToMain, 0, 'f', 1);
        }
        else
        {
            c.kept = true;
            c.reason = QStringLiteral("preserved: %1 px (%2% of the subject)")
                           .arg(c.area)
                           .arg(c.relativeArea * 100.0, 0, 'f', 2);
        }

        if (c.kept)
            ++report.detachedKept;
        else
            ++report.detachedRemoved;
        keep[size_t(id)] = c.kept ? 1 : 0;
        report.components.push_back(c);
    }

    /* Repaired foreground = kept components. */
    std::vector<uint8_t> repaired(size_t(w) * size_t(h), 0);
    for (size_t i = 0; i < fg.size(); ++i)
        if (comps.label[i] && keep[size_t(comps.label[i])])
            repaired[i] = 1;

    /* Enclosed holes: 4-connected background components not touching the
     * border. 4-connectivity prevents a diagonal gap from making a hole leak
     * into the outer background. */
    std::vector<uint8_t> inv(size_t(w) * size_t(h), 0);
    for (size_t i = 0; i < repaired.size(); ++i)
        inv[i] = repaired[i] ? 0 : 1;
    const Labels holes = labelComponents(inv, w, h, false);

    std::vector<char> borderHole(size_t(holes.count) + 1, 0);
    for (int x = 0; x < w; ++x)
    {
        const int a = holes.label[size_t(x)];
        const int b = holes.label[size_t(h - 1) * size_t(w) + size_t(x)];
        if (a)
            borderHole[size_t(a)] = 1;
        if (b)
            borderHole[size_t(b)] = 1;
    }
    for (int y = 0; y < h; ++y)
    {
        const int a = holes.label[size_t(y) * size_t(w)];
        const int b = holes.label[size_t(y) * size_t(w) + size_t(w - 1)];
        if (a)
            borderHole[size_t(a)] = 1;
        if (b)
            borderHole[size_t(b)] = 1;
    }

    QImage filledMask(w, h, QImage::Format_Grayscale8);
    QImage deferredMask(w, h, QImage::Format_Grayscale8);
    filledMask.fill(0);
    deferredMask.fill(0);

    QImage source = inputs.rgb.isNull() ? QImage() : inputs.rgb.convertToFormat(QImage::Format_RGB32);

    for (int id = 1; id <= holes.count; ++id)
    {
        if (borderHole[size_t(id)])
            continue;
        ++report.holesFound;
        const QRect b = holes.bounds[size_t(id)];
        Component c;
        c.kind = Component::Kind::EnclosedHole;
        c.label = id;
        c.area = holes.area[size_t(id)];
        c.relativeArea = mainArea > 0 ? double(c.area) / double(mainArea) : 0.0;
        c.bounds = b;

        for (const QPointF &p : inputs.negativePoints)
        {
            const int x = int(std::floor(p.x()));
            const int y = int(std::floor(p.y()));
            if (x >= 0 && y >= 0 && x < w && y < h
                && holes.label[size_t(y) * size_t(w) + size_t(x)] == id)
                c.userExcluded = true;
        }
        for (const QPointF &p : inputs.positivePoints)
        {
            const int x = int(std::floor(p.x()));
            const int y = int(std::floor(p.y()));
            if (x >= 0 && y >= 0 && x < w && y < h
                && holes.label[size_t(y) * size_t(w) + size_t(x)] == id)
                c.userSeeded = true;
        }

        /* Colour evidence: hole interior vs a 2 px foreground ring around it. */
        double holeSum[3] = {0, 0, 0};
        double ringSum[3] = {0, 0, 0};
        int holeN = 0, ringN = 0;
        for (int y = std::max(0, b.top() - 2); y <= std::min(h - 1, b.bottom() + 2); ++y)
        {
            for (int x = std::max(0, b.left() - 2); x <= std::min(w - 1, b.right() + 2); ++x)
            {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                if (holes.label[i] == id)
                {
                    if (source.isNull())
                        continue;
                    const QRgb px = source.pixel(x, y);
                    holeSum[0] += qRed(px);
                    holeSum[1] += qGreen(px);
                    holeSum[2] += qBlue(px);
                    ++holeN;
                }
                else if (repaired[i])
                {
                    /* Only the immediate ring: a foreground pixel within
                     * Chebyshev distance 2 of a hole pixel. Counting the whole
                     * bounding box would pull the mean toward the subject and
                     * make every hole look like the same material. */
                    bool adjacent = false;
                    for (int dy = -2; dy <= 2 && !adjacent; ++dy)
                    {
                        const int yy = y + dy;
                        if (yy < 0 || yy >= h)
                            continue;
                        for (int dx = -2; dx <= 2; ++dx)
                        {
                            const int xx = x + dx;
                            if (xx < 0 || xx >= w)
                                continue;
                            if (holes.label[size_t(yy) * size_t(w) + size_t(xx)] == id)
                            {
                                adjacent = true;
                                break;
                            }
                        }
                    }
                    if (!adjacent || source.isNull())
                        continue;
                    const QRgb px = source.pixel(x, y);
                    ringSum[0] += qRed(px);
                    ringSum[1] += qGreen(px);
                    ringSum[2] += qBlue(px);
                    ++ringN;
                }
            }
        }
        if (holeN > 0 && ringN > 0)
        {
            double d2 = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                const double a = holeSum[k] / double(holeN);
                const double bb = ringSum[k] / double(ringN);
                d2 += (a - bb) * (a - bb);
            }
            c.meanColorDistance = std::sqrt(d2) / 255.0;
        }
        else
            c.meanColorDistance = 1.0; /* no evidence -> assume different */

        const bool tiny = c.area <= std::max(12.0, 0.0008 * double(mainArea));
        const bool small = c.relativeArea <= 0.02;
        const bool medium = c.relativeArea <= 0.12;
        const bool sameColour = c.meanColorDistance <= 0.14;

        if (c.userSeeded)
        {
            c.filled = true;
            c.reason = QStringLiteral("positively seeded by the user");
        }
        else if (c.userExcluded)
        {
            c.kept = false;
            c.reason = QStringLiteral("negatively seeded by the user");
        }
        else if (!params.restoreEnclosedHoles || params.holeRecovery < 0.05)
        {
            c.kept = false;
            c.deferredToUnknown = false;
            c.reason = QStringLiteral("hole recovery disabled");
        }
        else if (tiny && c.meanColorDistance <= 0.25)
        {
            c.filled = true;
            c.reason = QStringLiteral("tiny enclosed speck (%1 px) surrounded by "
                                      "similar colour").arg(c.area);
        }
        else if (small && sameColour && params.holeRecovery >= 0.25)
        {
            c.filled = true;
            c.reason = QStringLiteral("enclosed %1% hole with matching colour "
                                      "(distance %2)")
                           .arg(c.relativeArea * 100.0, 0, 'f', 2)
                           .arg(c.meanColorDistance, 0, 'f', 3);
        }
        else if (medium)
        {
            c.kept = false;
            c.deferredToUnknown = true;
            c.reason = QStringLiteral("plausible opening, deferred to the "
                                      "unknown matting region");
        }
        else
        {
            c.kept = false;
            c.reason = QStringLiteral("large opening kept transparent");
        }

        if (c.filled)
        {
            ++report.holesFilled;
            for (int y = b.top(); y <= b.bottom(); ++y)
                for (int x = b.left(); x <= b.right(); ++x)
                    if (holes.label[size_t(y) * size_t(w) + size_t(x)] == id)
                    {
                        repaired[size_t(y) * size_t(w) + size_t(x)] = 1;
                        filledMask.scanLine(y)[x] = 255;
                    }
        }
        else if (c.deferredToUnknown)
        {
            ++report.holesDeferred;
            for (int y = b.top(); y <= b.bottom(); ++y)
                for (int x = b.left(); x <= b.right(); ++x)
                    if (holes.label[size_t(y) * size_t(w) + size_t(x)] == id)
                        deferredMask.scanLine(y)[x] = 255;
        }
        else
            ++report.holesKept;

        report.components.push_back(c);
    }

    QImage repairedImage(w, h, QImage::Format_Grayscale8);
    QImage removedImage(w, h, QImage::Format_Grayscale8);
    removedImage.fill(0);
    for (int y = 0; y < h; ++y)
    {
        uchar *dst = repairedImage.scanLine(y);
        uchar *rem = removedImage.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            dst[x] = repaired[i] ? 255 : 0;
            rem[x] = (fg[i] && !repaired[i]) ? 255 : 0;
        }
    }

    report.repairedMask = repairedImage;
    report.removedMask = removedImage;
    report.filledHoleMask = filledMask;
    report.deferredHoleMask = deferredMask;
    return report;
}

/* ----------------------------------------------------------------- trimap */

QImage buildTrimap(const QImage &repairedMask, const QImage &rgb,
                   const QImage &probability, const TopologyReport &topology,
                   const Parameters &params,
                   QImage *signedDistanceOut,
                   QImage *detailOut,
                   double *unknownFractionOut)
{
    if (repairedMask.isNull())
        return {};
    const QImage bin = grayToBinary(levelsOf(repairedMask));
    const int w = bin.width();
    const int h = bin.height();
    const int shortEdge = std::min(w, h);

    std::vector<uint8_t> fg(size_t(w) * size_t(h), 0);
    std::vector<uint8_t> bg(size_t(w) * size_t(h), 0);
    for (int y = 0; y < h; ++y)
    {
        const uchar *line = bin.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const bool on = line[x] != 0;
            fg[size_t(y) * size_t(w) + size_t(x)] = on ? 1 : 0;
            bg[size_t(y) * size_t(w) + size_t(x)] = on ? 0 : 1;
        }
    }
    const std::vector<float> dOut = distanceSquaredFrom(fg, w, h);
    const std::vector<float> dIn = distanceSquaredFrom(bg, w, h);

    QImage probLevels;
    if (!probability.isNull())
    {
        probLevels = levelsOf(probability);
        if (probLevels.size() != bin.size())
            probLevels = probLevels.scaled(bin.size(), Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
        probLevels = probLevels.convertToFormat(QImage::Format_Grayscale8);
    }

    const std::vector<float> detail =
        rgb.isNull() ? std::vector<float>() : detailEnergy(rgb, w, h);

    /* Thin structures: anything that disappears after erosion and does not
     * come back under dilation is narrower than ~2*thinRadius. Those pixels
     * are NOT deleted (wires, hair and fingers must survive); they are handed
     * to the matting model as unknown so a false bridge between ankles or a
     * 1-px mask leak cannot become hard foreground. */
    const int thinRadius = std::max(1, int(std::lround(2.0 * params.scaleFor(shortEdge))));
    QImage thinMask;
    {
        const QImage eroded = erodeBinary(bin, thinRadius);
        const QImage regrown = dilateBinary(eroded, thinRadius);
        thinMask = QImage(bin.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y)
        {
            const uchar *src = bin.constScanLine(y);
            const uchar *grown = regrown.constScanLine(y);
            uchar *dst = thinMask.scanLine(y);
            for (int x = 0; x < w; ++x)
                dst[x] = (src[x] && !grown[x]) ? 255 : 0;
        }
    }

    const int inner = params.innerBandFor(shortEdge);
    const int outer = params.outerBandFor(shortEdge);
    const int detailBand = params.detailBandFor(shortEdge);
    const int maxBand = params.maxBandFor(shortEdge);

    QImage sdImage(w, h, QImage::Format_Grayscale16);
    QImage trimap(w, h, QImage::Format_Grayscale8);
    long long unknownCount = 0;

    for (int y = 0; y < h; ++y)
    {
        quint16 *sdLine = reinterpret_cast<quint16 *>(sdImage.scanLine(y));
        uchar *tri = trimap.scanLine(y);
        const uchar *prob = probLevels.isNull() ? nullptr : probLevels.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float sd = std::sqrt(dOut[i]) - std::sqrt(dIn[i]);
            sdLine[x] = quint16(clampi(int(std::lround((sd + 128.0f) * 16.0f)), 0, 65535));

            float d = 0.0f;
            if (!detail.empty())
            {
                const int r = 1;
                for (int dy = -r; dy <= r; ++dy)
                {
                    const int yy = clampi(y + dy, 0, h - 1);
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        const int xx = clampi(x + dx, 0, w - 1);
                        d = std::max(d, detail[size_t(yy) * size_t(w) + size_t(xx)]);
                    }
                }
            }

            const float innerWide = float(inner)
                + params.detailRecovery * float(std::max(0, detailBand / 2 - inner)) * d;
            const float outerWide = float(outer)
                + params.detailRecovery * float(std::max(0, detailBand - outer)) * d;

            bool unknown = false;
            if (sd < 0.0f)
                unknown = -sd <= innerWide;
            else
                unknown = sd <= outerWide;

            /* Hard ceiling: never let a detail spike create an unbounded
             * unknown region. */
            if (!unknown && std::abs(sd) <= float(maxBand)
                && d > 0.92f && params.detailRecovery > 0.5)
                unknown = std::abs(sd) <= float(detailBand);

            if (prob)
            {
                const float p = float(prob[x]) / 255.0f;
                if (p > 0.12f && p < 0.88f)
                    unknown = true;
            }

            if (!topology.deferredHoleMask.isNull()
                && topology.deferredHoleMask.size() == bin.size()
                && topology.deferredHoleMask.constScanLine(y)[x])
                unknown = true;

            /* Areas around removed artifacts stay uncertain: if the speck was
             * real, the matting stage can recover it; if it was noise it stays
             * transparent because the trimap's confirmed background is far
             * away. */
            if (!topology.removedMask.isNull()
                && topology.removedMask.size() == bin.size()
                && topology.removedMask.constScanLine(y)[x])
            {
                unknown = true;
            }

            /* Thin protrusion / possible bridge: let the model decide. */
            if (thinMask.constScanLine(y)[x])
                unknown = true;

            uchar value;
            if (unknown)
            {
                value = 128;
                ++unknownCount;
            }
            else
                value = sd < 0.0f ? 255 : 0;
            tri[x] = value;
        }
    }

    if (signedDistanceOut)
        *signedDistanceOut = sdImage;
    if (detailOut)
        *detailOut = detail.empty() ? QImage() : levelMaskImage(detail, w, h, 255.0f);
    if (unknownFractionOut)
        *unknownFractionOut = double(unknownCount)
                              / double(std::max(1, w * h));
    return trimap;
}

/* ------------------------------------------------------- deterministic alpha */

AlphaPlane refineAlphaDeterministic(const QImage &repairedMask,
                                    const QImage &rgb,
                                    const Parameters &params,
                                    const QImage &trimap)
{
    Q_UNUSED(rgb);
    Q_UNUSED(params);
    if (repairedMask.isNull())
        return {};
    QImage bin = grayToBinary(levelsOf(repairedMask));
    const int w = bin.width();
    const int h = bin.height();
    const int shortEdge = std::min(w, h);

    /* Refined mode is an honest anti-aliased edge, not neural matting.
     * A tiny separable box average gives true sub-pixel coverage on the
     * boundary; smoothstep sharpens it back to a ~1.5 px ramp so the result
     * never looks like a blurred sticker. Radius stays 1 (2 above 1400 px)
     * so a 2 px wire does not disappear. */
    const int radius = shortEdge >= 1400 ? 2 : 1;
    const QImage soft = boxBlurBinary(bin, radius);
    const bool canThreshold = !trimap.isNull() && trimap.size() == bin.size();

    AlphaPlane alpha(w, h);
    for (int y = 0; y < h; ++y)
    {
        const uchar *tri = canThreshold ? trimap.constScanLine(y) : nullptr;
        const uchar *cov = soft.constScanLine(y);
        const uchar *hard = bin.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const float coverage = float(cov[x]) / 255.0f;
            float a = smoothstepf(0.30f, 0.70f, coverage);
            if (tri)
            {
                if (tri[x] == 255)
                    a = std::max(a, hard[x] ? 1.0f : 0.0f);
                else if (tri[x] == 0)
                    a = std::min(a, hard[x] ? 1.0f : 0.0f);
            }
            alpha.setAt(x, y, a);
        }
    }
    return alpha;
}

/* ------------------------------------------------- foreground decontamination */

QImage reconstructForeground(const QImage &rgb, const AlphaPlane &alpha,
                             const QImage &trimap,
                             const Parameters &params,
                             QImage *backgroundOut,
                             QImage *foregroundOut)
{
    if (rgb.isNull() || alpha.isNull())
        return {};
    QImage src = rgb.convertToFormat(QImage::Format_RGB32);
    if (src.size() != alpha.size())
        src = src.scaled(alpha.size(), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    const int w = alpha.width();
    const int h = alpha.height();

    QImage tri;
    if (!trimap.isNull() && trimap.size() == alpha.size())
        tri = levelsOf(trimap);
    else
    {
        tri = QImage(w, h, QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y)
        {
            uchar *line = tri.scanLine(y);
            for (int x = 0; x < w; ++x)
            {
                const float a = alpha.at(x, y);
                line[x] = a >= 0.9f ? 255 : (a <= 0.1f ? 0 : 128);
            }
        }
    }

    std::vector<uint8_t> bgKnown(size_t(w) * size_t(h), 0);
    std::vector<uint8_t> fgKnown(size_t(w) * size_t(h), 0);
    for (int y = 0; y < h; ++y)
    {
        const uchar *line = tri.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float a = alpha.at(x, y);
            bgKnown[i] = (line[x] == 0 || a <= 0.02f) ? 1 : 0;
            fgKnown[i] = (line[x] == 255 || a >= 0.98f) ? 1 : 0;
        }
    }

    Pyramid bg;
    Pyramid fg;
    if (params.decontamination > 0.001)
    {
        const int shortEdge = std::min(w, h);
        const float falloff = std::max(2.0f, float(params.outerBandFor(shortEdge)) * 1.5f);
        bg = Pyramid::build(src, 512, bgKnown, falloff);
        fg = Pyramid::build(src, 512, fgKnown, falloff);
        bg.fullW_ = fg.fullW_ = w;
        bg.fullH_ = fg.fullH_ = h;
    }

    QImage premul(w, h, QImage::Format_ARGB32_Premultiplied);
    QImage foreground(w, h, QImage::Format_RGB32);
    QImage backgroundEstimate(w, h, QImage::Format_RGB32);

    const float strength = float(clampd(params.decontamination, 0.0, 1.0));
    /* Correction ceiling: a boundary pixel may not be repainted into a
     * completely different colour. This is the "conservative reconstruction"
     * rule for weakly constrained pixels. */
    const float maxCorrection = 0.25f + 0.55f * strength;

    for (int y = 0; y < h; ++y)
    {
        const QRgb *sline = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *pline = reinterpret_cast<QRgb *>(premul.scanLine(y));
        QRgb *fline = reinterpret_cast<QRgb *>(foreground.scanLine(y));
        QRgb *bline = reinterpret_cast<QRgb *>(backgroundEstimate.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const float a = alpha.at(x, y);
            const int a8 = clampi(int(std::lround(a * 255.0f)), 0, 255);
            const int ir = qRed(sline[x]);
            const int ig = qGreen(sline[x]);
            const int ib = qBlue(sline[x]);

            if (a <= 0.002f)
            {
                pline[x] = qRgba(0, 0, 0, 0);
                /* Straight-alpha storage keeps a plausible FOREGROUND colour
                 * under fully transparent pixels. A zero RGB there is the
                 * classic source of dark fringes when a layer texture is
                 * filtered or rescaled. */
                float P[3] = { srgbToLinear(float(ir) / 255.0f),
                               srgbToLinear(float(ig) / 255.0f),
                               srgbToLinear(float(ib) / 255.0f) };
                if (params.decontamination > 0.001 && !fg.levels.empty()
                    && fg.supportAt(float(x), float(y)) > 0.05f)
                    fg.sample(float(x), float(y), P);
                fline[x] = qRgb(clampi(int(std::lround(linearToSrgb(P[0]) * 255.0f)), 0, 255),
                                clampi(int(std::lround(linearToSrgb(P[1]) * 255.0f)), 0, 255),
                                clampi(int(std::lround(linearToSrgb(P[2]) * 255.0f)), 0, 255));
                bline[x] = sline[x];
                continue;
            }
            if (a >= 0.998f || strength <= 0.001f)
            {
                /* Fully opaque: preserve the source colour exactly. */
                fline[x] = sline[x];
                bline[x] = sline[x];
                pline[x] = qRgba(ir * a8 / 255, ig * a8 / 255, ib * a8 / 255, a8);
                continue;
            }

            const float I[3] = { srgbToLinear(float(ir) / 255.0f),
                                 srgbToLinear(float(ig) / 255.0f),
                                 srgbToLinear(float(ib) / 255.0f) };
            float B[3] = { I[0], I[1], I[2] };
            float P[3] = { I[0], I[1], I[2] };
            float bWeight = 0.0f;
            if (params.decontamination > 0.001 && !bg.levels.empty())
            {
                bg.sample(float(x), float(y), B);
                fg.sample(float(x), float(y), P);
                bWeight = bg.supportAt(float(x), float(y));
            }

            /* Fewer samples -> less trust in B; fewer foreground samples ->
             * the prior is the background colour (neutral fallback). */
            const float reliability = clampf(bWeight, 0.0f, 1.0f);
            const float localStrength = strength * (0.35f + 0.65f * reliability);
            const float lambda = 0.02f + 0.35f * std::pow(1.0f - a, 3.0f);

            float F[3];
            for (int k = 0; k < 3; ++k)
            {
                const float solved =
                    (a * (I[k] - (1.0f - a) * B[k]) + lambda * P[k])
                    / (a * a + lambda);
                const float corrected = I[k] + clampf(solved - I[k],
                                                     -maxCorrection,
                                                     maxCorrection);
                F[k] = clampf(I[k] + (corrected - I[k]) * localStrength,
                              0.0f, 1.0f);
            }

            const int fr = clampi(int(std::lround(linearToSrgb(F[0]) * 255.0f)), 0, 255);
            const int fgc = clampi(int(std::lround(linearToSrgb(F[1]) * 255.0f)), 0, 255);
            const int fb = clampi(int(std::lround(linearToSrgb(F[2]) * 255.0f)), 0, 255);

            fline[x] = qRgb(fr, fgc, fb);
            bline[x] = qRgb(clampi(int(std::lround(linearToSrgb(B[0]) * 255.0f)), 0, 255),
                            clampi(int(std::lround(linearToSrgb(B[1]) * 255.0f)), 0, 255),
                            clampi(int(std::lround(linearToSrgb(B[2]) * 255.0f)), 0, 255));

            /* Premultiplied storage: every channel is clamped to alpha, so a
             * transparent pixel can never carry invalid RGB into a texture
             * filter (the classic bright-fringe bug). */
            pline[x] = qRgba(std::min(a8, fr * a8 / 255),
                             std::min(a8, fgc * a8 / 255),
                             std::min(a8, fb * a8 / 255),
                             a8);
        }
    }

    if (backgroundOut)
        *backgroundOut = backgroundEstimate;
    if (foregroundOut)
        *foregroundOut = foreground;
    return premul;
}

QImage straightRgba(const QImage &foregroundRgb, const AlphaPlane &alpha)
{
    if (foregroundRgb.isNull() || alpha.isNull())
        return {};
    QImage src = foregroundRgb.convertToFormat(QImage::Format_RGB32);
    if (src.size() != alpha.size())
        src = src.scaled(alpha.size(), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    QImage out(alpha.width(), alpha.height(), QImage::Format_ARGB32);
    for (int y = 0; y < alpha.height(); ++y)
    {
        const QRgb *sline = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *dline = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < alpha.width(); ++x)
        {
            const int a = clampi(int(std::lround(alpha.at(x, y) * 255.0f)), 0, 255);
            /* RGB stays the reconstructed foreground even at a = 0, so
             * filtering cannot pull in black. */
            dline[x] = qRgba(qRed(sline[x]), qGreen(sline[x]), qBlue(sline[x]), a);
        }
    }
    return out;
}

/* -------------------------------------------------------------- pipelines */

Result refineDeterministic(const Inputs &inputs, const Parameters &params)
{
    Result result;
    if (inputs.rgb.isNull() || inputs.mask.isNull())
        return result;

    result.backend = QStringLiteral("refined");
    result.width = inputs.rgb.width();
    result.height = inputs.rgb.height();

    TopologyReport topology = analyseTopology(inputs, params);
    result.topology = topology;
    if (topology.repairedMask.isNull())
        return result;

    double unknownFraction = 0.0;
    QImage sd;
    QImage detail;
    QImage trimap = buildTrimap(topology.repairedMask, inputs.rgb,
                                inputs.probability, topology, params,
                                result.diagnostics.enabled ? &sd : nullptr,
                                result.diagnostics.enabled ? &detail : nullptr,
                                &unknownFraction);
    result.trimap = trimap;
    result.trimapUnknownFraction = unknownFraction;

    AlphaPlane alpha = refineAlphaDeterministic(topology.repairedMask,
                                                inputs.rgb, params, trimap);
    if (alpha.isNull())
        return result;
    result.alpha = alpha;

    QImage background;
    QImage foreground;
    result.premultipliedRgba = reconstructForeground(
        inputs.rgb, alpha, trimap, params,
        result.diagnostics.enabled ? &background : nullptr,
        &foreground);
    if (!foreground.isNull())
        result.foregroundRgb = foreground;
    else
        result.foregroundRgb = inputs.rgb.convertToFormat(QImage::Format_RGB32);

    if (result.diagnostics.enabled)
    {
        result.diagnostics.original = inputs.rgb;
        result.diagnostics.semanticMask = inputs.mask;
        result.diagnostics.repairedMask = topology.repairedMask;
        result.diagnostics.trimap = trimap;
        QImage color(trimap.size(), QImage::Format_RGB32);
        for (int y = 0; y < trimap.height(); ++y)
        {
            const uchar *line = trimap.constScanLine(y);
            QRgb *out = reinterpret_cast<QRgb *>(color.scanLine(y));
            for (int x = 0; x < trimap.width(); ++x)
            {
                if (line[x] == 255)
                    out[x] = qRgb(255, 255, 255);
                else if (line[x] == 0)
                    out[x] = qRgb(10, 12, 20);
                else
                    out[x] = qRgb(0, 200, 90);
            }
        }
        result.diagnostics.trimapColor = color;
        result.diagnostics.alphaPreview = alpha.toGrayscale8();
        result.diagnostics.backgroundEstimate = background;
        result.diagnostics.foregroundRgb = foreground;
        result.diagnostics.premultipliedRgba = result.premultipliedRgba;
    }
    return result;
}

Result applyNeuralAlpha(const Inputs &inputs, Result result,
                        const AlphaPlane &neuralAlpha,
                        const Parameters &params,
                        const QString &backend,
                        const QString &modelRevision,
                        double inferenceMs)
{
    if (result.alpha.isNull() || neuralAlpha.isNull())
        return result;
    if (neuralAlpha.size() != result.alpha.size())
        return result;

    result.alpha = neuralAlpha;
    result.backend = backend;
    result.modelRevision = modelRevision;
    result.inferenceMs = inferenceMs;

    QImage background;
    QImage foreground;
    result.premultipliedRgba = reconstructForeground(
        inputs.rgb, result.alpha, result.trimap, params,
        result.diagnostics.enabled ? &background : nullptr,
        &foreground);
    if (!foreground.isNull())
        result.foregroundRgb = foreground;

    if (result.diagnostics.enabled)
    {
        result.diagnostics.alphaPreview = result.alpha.toGrayscale8();
        result.diagnostics.backgroundEstimate = background;
        result.diagnostics.foregroundRgb = foreground;
        result.diagnostics.premultipliedRgba = result.premultipliedRgba;
    }
    return result;
}

QString FrameIdentity::cacheKey(const Parameters &params,
                                const QString &backend) const
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12")
        .arg(mediaId)
        .arg(ptsMs)
        .arg(subjectId)
        .arg(selectionRevision)
        .arg(refinementRevision)
        .arg(mediaGeneration)
        .arg(params.innerBandPx)
        .arg(params.outerBandPx)
        .arg(params.fineDetailBandPx)
        .arg(params.maxBandPx)
        .arg(int(std::lround(params.decontamination * 1000.0)))
        .arg(backend);
}

} // namespace matte
} // namespace paintbrush
