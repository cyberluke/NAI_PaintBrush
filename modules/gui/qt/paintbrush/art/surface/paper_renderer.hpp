/*****************************************************************************
 * paper_renderer.hpp : NAI Paintbrush — canonical paper texture renderer
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_PAPER_RENDERER_HPP
#define PB_ART_PAPER_RENDERER_HPP

#include <QImage>
#include <QSize>
#include <QRect>
#include <QRgb>
#include <cstdint>

#include "../runtime/art_scale.hpp"
#include "../runtime/light_rig.hpp"
#include "paper_stock.hpp"

namespace paintbrush {
namespace art {

/* The canonical paper texture renderer.
 *
 * Spec §8.3: paper must stay stable when zooming, remain stable across export
 * resolution, use a deterministic seed, respect document/world scale, not swim
 * when panning and not look like a repeated bitmap tile. It must participate
 * consistently in the light interaction.
 *
 * Design: the texture value of an output device pixel is computed as a
 * function of its position in WORLD space (u,v = device px / worldToDevice
 * scale), NOT as a function of the pixel index. That makes the texture
 * resolution-independent: zooming in samples more of the same world-heightmap
 * instead of scaling a fixed bitmap, and the seed makes it deterministic. */
class PaperRenderer
{
public:
    PaperRenderer() = default;

    /* Rendering state: the stock, the shared rig, and the scale that maps
     * world units to output pixels. `viewOriginDevice` is the canvas-space
     * device pixel at the top-left of the output so panning does not cause the
     * texture to "swim" (the world position is recomputed exactly). */
    struct State
    {
        const PaperStock *stock = nullptr;
        const ArtLightRig *rig = nullptr;
        ArtScale scale;
        QPoint viewOriginDevice{ 0, 0 };
        float worldViewX = 0.0f;   /* canvas-space world coordinate at origin */
        float worldViewY = 0.0f;
        bool blendWithBase = true; /* false: only the grain response (for layered
                                      matte compositing) */
    };

    /* Render `size` RGBA pixels of paper for the given state. `outputRect`
     * is the canvas rectangle (device px) this covers; combined with the
     * origin it pins the world coordinates per pixel. A mostly-opaque RGBA
     * surface is returned (the caller may place layers over it). */
    QImage render(const QSize &size, const State &state, const QRect &canvasRect) const;

    /* Sample a single world-space point as RGBA (used by the GPU path and the
     * harness). `wx`, `wy` are canvas world coordinates. */
    QRgb sample(const State &state, float wx, float wy) const;

    /* The world-scaled grain frequency in device px for a stock+scale. */
    float grainDevicePx(const State &state) const;

private:
    QRgb samplePipeline(const State &state, float wx, float wy) const;
};

} // namespace art
} // namespace paintbrush

#endif // PB_ART_PAPER_RENDERER_HPP