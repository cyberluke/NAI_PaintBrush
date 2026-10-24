/*****************************************************************************
 * bristle_brush.cpp : NAI Paintbrush — premium bristle paintbrush engine
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "bristle_brush.hpp"

#include "../runtime/art_noise.hpp"

#include <QRectF>

namespace paintbrush {
namespace art {

/* ------------------------------------------------------------------ set-up */

int BristleBrushEngine::effectiveBristleCount() const
{
    if (m_params.bristleCount > 0)
        return m_params.bristleCount;
    /* roughly one strand per 0.8 px of diameter: enough to cover the width,
     * few enough that individual strands stay visible (bounded) */
    return qBound(6, int(m_params.size * 1.25f), 140);
}

void BristleBrushEngine::spawnBristles(const BristleBrushParams &p)
{
    m_bristles.clear();
    const int n = effectiveBristleCount();
    m_bristles.reserve(n);
    const std::uint32_t seed = p.seed;
    for (int i = 0; i < n; ++i)
    {
        Bristle b;
        /* Radial disc fill: ring radius squared then angle; deterministic from
         * the seed (spec §37). */
        const float ang = 6.2831853f * artHashUnitf(seed, std::uint32_t(i));
        const float radiusFrac = std::sqrt(artHashUnitf(seed, std::uint32_t(i) + 777u));
        const float c = std::cos(ang);
        b.lateralOffset = radiusFrac * c;
        b.thickness = 0.28f + 0.72f * artHashUnitf(seed, std::uint32_t(i) + 2000u);
        b.stiffness = 0.5f + 0.5f * artHashUnitf(seed, std::uint32_t(i) + 3000u);
        b.opacityVariation = 0.5f * artHashUnitf(seed, std::uint32_t(i) + 4000u);
        b.colorVariation = artHashUnitf(seed, std::uint32_t(i) + 5000u);
        b.seedIndex = std::uint32_t(i);
        m_bristles.append(b);
    }
}

void BristleBrushEngine::beginStroke(const BristleBrushParams &brushParams)
{
    m_params = brushParams;
    m_hasLast = false;
    m_segmentIndex = 0;
    spawnBristles(brushParams);
}

void BristleBrushEngine::endStroke()
{
    m_bristles.clear();
    m_hasLast = false;
}

/* ------------------------------------------------------------------ render */

/* Per-bristle color jitter: shift hue/lightness deterministically so bristles
 * lay down slightly different pigment (no muddy averaging, spec §14.4). */
static QColor bristleColor(const QColor &base, float variation,
                           std::uint32_t seedIndex, std::uint32_t seed)
{
    if (variation <= 0.0f)
        return base;
    const float amount = artHashUnitf(seed, seedIndex + 7000u);
    /* vary lightness ± and hue a touch */
    int h, s, v, a;
    base.getHsv(&h, &s, &v, &a);
    /* variation 0.2 -> about +-14 lightness levels and +-4 deg of hue: clearly
     * individual bristles, never a muddy average; 1.0 -> bold variation */
    const float dv = (amount - 0.5f) * 2.0f * variation * 110.0f;
    const float dh = (amount - 0.5f) * 2.0f * variation * 30.0f;
    int nv = qBound(0, int(v + dv), 255);
    int nh = h;
    if (h >= 0)
        nh = (h + int(dh) % 360 + 360) % 360;
    int ns = s;
    if (variation > 0.3f)
        ns = qBound(0, int(s + (amount - 0.5f) * 2.0f * variation * 30.0f), 255);
    QColor c = QColor::fromHsv(nh < 0 ? 0 : nh, ns, nv, a);
    return c;
}

QRect BristleBrushEngine::strokeSegment(QImage &target,
                                        const QPointF &from, const QPointF &to,
                                        float pressure)
{
    if (!isActive() || target.isNull())
        return QRect();

    /* Base radius from size (diameter -> radius), modulated by pressure. */
    const float radius = m_params.size * 0.5f *
        (1.0f + (pressure - 0.5f) * 2.0f * m_params.pressureAmount * 0.35f);
    if (radius <= 0.4f)
        return QRect();

    /* Stroke axis direction (normalized). */
    QPointF axis = to - from;
    const float axisLen = std::sqrt(axis.x() * axis.x() + axis.y() * axis.y());
    if (axisLen < 1e-5f)
        axis = QPointF(1.0f, 0.0f);
    else
        axis /= axisLen;
    /* Perpendicular unit vector (bristle offset direction). */
    const QPointF perp(-axis.y(), axis.x());

    QRectF all;
    const std::uint32_t seed = m_params.seed;
    const auto &bris = m_bristles;

    /* segment index drives deterministic micro-wobble of soft bristles */
    const std::uint32_t segIndex = std::uint32_t(m_segmentIndex++);

    for (int i = 0; i < bris.size(); ++i)
    {
        const Bristle &b = bris.at(i);
        /* Each bristle is a THIN filament: a few percent of the brush width,
         * laid at its own lateral offset across the full diameter. The union
         * of filaments gives the stroke its width while each strand keeps its
         * own pigment and weight (react-artboard principle: one stroke ->
         * many bristles -> per-bristle offset/thickness/color). */
        const float strandR = std::max(0.35f, radius * (0.035f + 0.075f * b.thickness));
        /* soft bristles wander slightly; stiff ones track the axis exactly */
        const float wobble = (1.0f - b.stiffness) * strandR * 0.6f *
                             (artHashUnitf(seed ^ segIndex, b.seedIndex + 9000u) - 0.5f) * 2.0f;
        const QPointF perpOff = perp * (b.lateralOffset * radius * 0.92f + wobble);
        const QPointF fromB = from + perpOff;
        const QPointF toB = to + perpOff;

        /* partial per-strand opacity: overlapping strands ACCUMULATE instead of
         * the last one covering everything, so the strand pattern (and each
         * strand's pigment) stays visible — flow sets the ceiling */
        const float bristleOpacity = qBound(0.0f,
            m_params.flow * (0.28f + 0.45f * (1.0f - b.opacityVariation)) *
            (0.65f + 0.35f * pressure), 1.0f);

        const QColor col = bristleColor(m_params.color, m_params.colorVariation,
                                        b.seedIndex, seed);

        /* stamp along the filament densely so each strand is continuous */
        const float step = std::max(0.4f, strandR * 0.6f);
        const float dist = axisLen;
        const float pad = strandR + 1.0f;
        float t = 0.0f;
        do
        {
            const QPointF p = fromB + (toB - fromB) * t;
            stampDab(target, p, strandR, m_params.hardness, bristleOpacity, col);
            all = all.united(QRectF(p.x() - pad, p.y() - pad,
                                    2.0f * pad, 2.0f * pad));
            t += step / std::max(1e-4f, dist);
        } while (t < 1.0f);
        stampDab(target, toB, strandR, m_params.hardness, bristleOpacity, col);
        all = all.united(QRectF(toB.x() - pad, toB.y() - pad,
                                2.0f * pad, 2.0f * pad));
    }

    m_lastCenter = to;
    m_hasLast = true;
    return all.toAlignedRect().intersected(target.rect());
}

} // namespace art
} // namespace paintbrush