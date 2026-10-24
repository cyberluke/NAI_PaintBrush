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

    /* soft specular glint on coated/rough papers */
    if (stock.specular > 0.0f && stock.roughness < 0.6f)
    {
        const float glintNoise = artCellUnitf(seed ^ 0xBEAFu,
                                              int(wx * 0.5f), int(wy * 0.5f));
        if (glintNoise > 0.85f)
            lum += stock.specular * (glintNoise - 0.85f) * 8.0f;
    }

    lum = qBound(0.55f, lum, 1.45f);

    return qRgba(qBound(0, int(base.redF() * 255.0f * lum), 255),
                 qBound(0, int(base.greenF() * 255.0f * lum), 255),
                 qBound(0, int(base.blueF() * 255.0f * lum), 255),
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