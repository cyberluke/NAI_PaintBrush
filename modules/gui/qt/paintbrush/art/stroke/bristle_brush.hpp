/*****************************************************************************
 * bristle_brush.hpp : NAI Paintbrush — premium bristle paintbrush engine
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_BRISTLE_BRUSH_HPP
#define PB_ART_BRISTLE_BRUSH_HPP

#include <QPointF>
#include <QColor>
#include <QVector>
#include <QImage>
#include <QRect>
#include <QtGlobal>
#include <cmath>
#include <algorithm>

#include "stroke_sampler.hpp"
#include "../../pb_common.hpp" /* DabSpec, stampDab */

namespace paintbrush {
namespace art {

/* A bristle for the duration of a stroke (spec §13.2). Bristle identity is
 * STABLE for the whole stroke: the same bristles are created when the stroke
 * begins and move together as the stroke advances. Each bristle has a lateral
 * offset from the stroke axis, a thickness, a per-bristle color variation and
 * a stiffness that affects how the bristle lags the stroke center. */
struct Bristle
{
    float lateralOffset = 0.0f; /* perpendicular offset, fraction of radius */
    float thickness = 1.0f;     /* fraction of the tip diameter */
    float stiffness = 1.0f;     /* 0 soft/wide, 1 stiff/focused */
    float opacityVariation = 0.0f;
    float colorVariation = 0.0f; /* 0..1 random tint on the hue/lightness */
    std::uint32_t seedIndex = 0; /* deterministic hashing offset */
};

/* A single brush instance's parameter set. `size` is in world units (diameter).
 * `hardness` 0..1 (1 = crisp edge on the union of bristle dabs), `flow` 0..1 =
 * how much ink each dab deposits (scales opacity), `smoothing` drives the
 * stabilizer, `pressureAmount` 0..1 scales how much pointer pressure
 * modulates size vs deposition, `colorVariation` 0..1 adds per-bristle hue
 * jitter (deterministic seeded), `seed` for reproducibility (spec §37). */
struct BristleBrushParams
{
    float size = 24.0f;        /* world units, diameter */
    float hardness = 0.85f;
    float flow = 0.7f;
    float smoothing = 0.5f;
    float pressureAmount = 0.6f;
    float colorVariation = 0.1f;
    QColor color = Qt::black;
    std::uint32_t seed = 1;
    int bristleCount = 0;      /* 0 = auto (from size) */
};

/* Renders a bristle brush stroke. The engine stamps per-bristle dabs into the
 * target image through the shared `stampDab` path so every deposit rides the
 * same commit + undo machinery (spec §13.1: "typed StrokeCommand"). Bristles
 * are spawned once per stroke and kept stable for its duration. */
class BristleBrushEngine
{
public:
    BristleBrushEngine() = default;

    /* Begin a stroke: create the bristle set from the parameters. */
    void beginStroke(const BristleBrushParams &brushParams);

    /* End the stroke (frees the bristle set); optional taper paints the last
     * few dabs with reduced radius so the tail does not end blunt. */
    void endStroke();

    /* Paint a dab-centered segment. `from`/`to` are in the target raster's
     * coordinate space (stroke positions already stabilised/spaced by the
     * caller). `pressure` 0..1 modulates size and deposition. Returns the
     * affected raster region (union of per-bristle stamps) so the caller can
     * commit it as a StrokeCommand region. */
    QRect strokeSegment(QImage &target, const QPointF &from, const QPointF &to,
                        float pressure);

    const QVector<Bristle> &bristles() const { return m_bristles; }
    bool isActive() const { return !m_bristles.isEmpty(); }

private:
    void spawnBristles(const BristleBrushParams &p);
    /* Sample the bristle layout at a reference point; stagger the lateral
     * offsets layer-by-layer. */
    int effectiveBristleCount() const;

    BristleBrushParams m_params;
    QVector<Bristle> m_bristles;
    QPointF m_lastCenter{ 0.0f, 0.0f };
    bool m_hasLast = false;
    int m_segmentIndex = 0; /* deterministic per-segment wobble seed */
};

/* Convenience: stamp a single "soft round" dab into a target at `center` with
 * radius/size in *world* units converted by the scale. Shares stampDab. */
inline void stampSoftDab(QImage &target, const QPointF &at, float radiusWorld,
                         float hardness, float opacity, const QColor &color,
                         float worldToDevice)
{
    const float r = radiusWorld * worldToDevice;
    DabSpec spec;
    spec.radius = r;
    spec.hardness = hardness;
    spec.opacity = opacity;
    spec.color = color;
    stampDab(target, at, spec);
}

} // namespace art
} // namespace paintbrush

#endif // PB_ART_BRISTLE_BRUSH_HPP