/*****************************************************************************
 * paper_renderer.cpp : NAI Paintbrush — canonical paper texture renderer
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "paper_renderer.hpp"

#include "../runtime/art_noise.hpp"

#include <QColor>
#include <cmath>
#include <algorithm>

namespace paintbrush {
namespace art {

/* Paper luminance model (world-space, so it is zoom/export stable):
 *   base = surface color under key + ambient
 *   grain = fbm(seed, wx, wy) around 0.5 scaled microHeight*tooth
 *   fibre = fine textile strands scaled fiberStrength
 *   spec  = soft glint on low-roughness stocks
 * Grain is evaluated in WORLD coordinates (wx,wy), so the same stock at any
 * zoom/export samples the same continuous heightmap — no swimming, no
 * repeated-bitmap tiling, and the seed makes it deterministic and replayable
 * (spec §8.3, §37). */

/* World-space frequency of the grain cells. `fiberScale` is authored in world
 * units per cell; its inverse is the fbm frequency in world coordinates. The
 * result is dimensionless world unit^-1. */
static inline float grainWorldFrequency(const PaperStock &stock)
{
    const float cell = stock.fiberScale > 1e-4f ? stock.fiberScale : 0.04f;
    return 1.0f / cell;
}

/* Same cubic as GLSL smoothstep, so paper.frag stays an exact port. */
static inline float smoothstepf(float edge0, float edge1, float x)
{
    const float t = qBound(0.0f, (x - edge0) / (edge1 - edge0), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

QRgb PaperRenderer::sample(const State &state, float wx, float wy) const
{
    if (!state.stock || !state.rig)
        return qRgba(0, 0, 0, 255);

    const PaperStock &stock = *state.stock;
    const ArtLightRig &rig = *state.rig;
    const std::uint32_t seed = stock.seed;

    /* base color under key + ambient */
    const QColor base = stock.surfaceColor(rig);

    const float freq = grainWorldFrequency(stock);

    /* fine grain (tooth/microheight) */
    const float g = artFbm(seed, wx, wy, 2, freq, 2.0f, 0.5f);
    const float grainVar = (g - 0.5f) * 2.0f * stock.microHeight *
                           (0.4f + stock.tooth);

    /* fibre strands */
    float fibre = 0.0f;
    if (stock.fiberStrength > 0.0f)
    {
        const float f = artFbm(seed ^ 0xFC01u, wx, wy * 1.3f, 3,
                               freq * 1.4f, 2.2f, 0.6f);
        const float strand = std::pow(qBound(0.0f, f, 1.0f), 3.0f);
        fibre = (strand - 0.5f) * 2.0f * stock.fiberStrength * 0.10f;
    }

    float lum = 1.0f + grainVar + fibre;

    /* Formation: low-frequency pulp cloudiness, applied as an absolute tonal
     * lift. On very dark stock a purely multiplicative wobble disappears, so
     * the structure is expressed in [0,1] colour units instead. */
    float lift = 0.0f;
    if (stock.formation > 0.0f)
    {
        const float cell = stock.formationScale > 1e-4f ? stock.formationScale : 0.30f;
        const float cloud = artFbm(seed ^ 0xF0A7u, wx, wy, 3, 1.0f / cell,
                                   2.0f, 0.55f);
        lift = (cloud - 0.5f) * 2.0f * stock.formation * 0.03f;
    }

    /* soft specular glint on coated/rough papers */
    if (stock.specular > 0.0f && stock.roughness < 0.6f)
    {
        const float glintNoise = artCellUnitf(seed ^ 0xBEAFu,
                                              int(wx * 0.5f), int(wy * 0.5f));
        if (glintNoise > 0.85f)
            lum += stock.specular * (glintNoise - 0.85f) * 8.0f;
    }

    lum = qBound(0.55f, lum, 1.45f);

    float cr = qBound(0.0f, base.redF() * lum + lift, 1.0f);
    float cg = qBound(0.0f, base.greenF() * lum + lift, 1.0f);
    float cb = qBound(0.0f, base.blueF() * lum + lift, 1.0f);

    /* Dust: sparse dark-grey flecks resting on the sheet. They are a neutral
     * grey lift of the lit surface, so they read as dust rather than sparkle,
     * and they are sampled in world space like every other paper property. */
    if (stock.dustStrength > 0.0f)
    {
        const float cell = stock.dustScale > 1e-4f ? stock.dustScale : 0.02f;
        const float dn = artFbm(seed ^ 0xD057u, wx, wy, 2, 1.0f / cell,
                                2.0f, 0.5f);
        const float f = smoothstepf(0.64f, 0.86f, dn) * stock.dustStrength;
        if (f > 0.0f)
        {
            const float dustR = qBound(0.0f, base.redF() + 0.09f + lift, 1.0f);
            const float dustG = qBound(0.0f, base.greenF() + 0.09f + lift, 1.0f);
            const float dustB = qBound(0.0f, base.blueF() + 0.09f + lift, 1.0f);
            cr += (dustR - cr) * f;
            cg += (dustG - cg) * f;
            cb += (dustB - cb) * f;
        }
    }

    return qRgba(qBound(0, int(cr * 255.0f), 255),
                 qBound(0, int(cg * 255.0f), 255),
                 qBound(0, int(cb * 255.0f), 255),
                 255);
}

QImage PaperRenderer::render(const QSize &size, const State &state,
                             const QRect &canvasRect) const
{
    QImage out(size, QImage::Format_ARGB32); /* QRgb byte order */
    if (size.width() <= 0 || size.height() <= 0 || !state.stock || !state.rig)
    {
        if (state.stock)
            out.fill(state.stock->baseColor);
        else
            out.fill(Qt::white);
        return out;
    }

    /* world units per device pixel at the document presentation */
    const ArtScale &scale = state.scale;
    const float perDev = scale.referenceDpi / scale.worldUnitsPerInch;
    const float denom = scale.devicePixelRatio * scale.exportScale;
    const float worldPerPx = perDev / denom;

    const int w = size.width();
    const int h = size.height();
    for (int y = 0; y < h; ++y)
    {
        const float wy = state.worldViewY + y * worldPerPx;
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const float wx = state.worldViewX + x * worldPerPx;
            row[x] = sample(state, wx, wy);
        }
    }
    (void)canvasRect;
    return out;
}

} // namespace art
} // namespace paintbrush
