/*****************************************************************************
 * light_rig.hpp : NAI Paintbrush — document-level shared light rig
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_LIGHT_RIG_HPP
#define PB_ART_LIGHT_RIG_HPP

#include <QString>
#include <QColor>
#include <QVector3D>
#include <QtGlobal>
#include <cmath>

namespace paintbrush {
namespace art {

/* One document-level light rig.
 *
 * Spec §12: glitter, glossy stickers, metallic accents, emboss, paper and
 * future foil must all react to the SAME light. If a glitter title sits next
 * to a metallic badge, highlight direction must match, shadows agree, and
 * perceived surface orientation agrees. Never let each material invent its
 * own arbitrary fake lighting.
 *
 * The keyframe is the unit light DIRECTION: direction.xy() is the azimuth
 * projection onto the paper plane; z/elevation is how high the light sits
 * above the surface. There is exactly one rig per document and every
 * material/effect samples it. */
struct ArtLightRig
{
    /* Light azimuth. The xy part is normalized when sampled by BRDFs. */
    QVector3D direction{ 0.5f, -0.35f, 0.6f }; /* default: up-right-ish */
    /* Elevation (degrees above the paper plane). -1 uses the z of direction;
     * >= 0 overrides it. */
    float elevationDeg = -1.0f;
    /* Softness 0..1: 1 = widely soft, 0 = hard directional key light. */
    float softness = 0.35f;
    /* Ambient fill 0..1. */
    float ambient = 0.30f;
    /* Light intensity 0..2 (multiplies the specular/diffuse response). */
    float intensity = 1.0f;
    /* Temperature 0=cool .. 1=warm .. 2=very warm amber. */
    float temperature = 0.55f;

    /* The normalized unit light direction used by BRDFs. Elevation and
     * azimuth are combined into a down-pointing world vector; the surface
     * normal is +z (paper up), so luminance = max(0, dot(N, L)), with L from
     * the surface toward the light. */
    QVector3D unitDirection() const
    {
        const float elevDeg = elevationDeg >= 0.0f ? elevationDeg : 45.0f;
        const float elevRad = elevDeg * float(M_PI) / 180.0f;
        const float len = std::sqrt(direction.x() * direction.x() +
                                    direction.y() * direction.y());
        QVector3D d;
        if (len > 1e-6f)
        {
            d.setX(direction.x() / len);
            d.setY(direction.y() / len);
        }
        else
        {
            d.setX(0.0f);
            d.setY(0.0f);
        }
        d.setZ(std::sin(elevRad));
        d.normalize();
        /* Make it a down-pointing light: negative z means arriving from above
         * onto the +z surface normal. Flip so luminance=max(0, N.L) works
         * with N=(0,0,1): the light direction should have a negative z. */
        if (d.z() > 0.0f)
            d.setZ(-d.z());
        return d.normalized();
    }

    /* Ambient fill color (temperature-tinted *, scaled by ambient). */
    QColor ambientColor() const
    {
        return tinted(QColor::fromRgbF(0.6f, 0.6f, 0.62f), ambient + 0.15f);
    }

    /* The material's diffuse key light color (temperature-tinted * intensity). */
    QColor keyColor() const
    {
        return tinted(QColor::fromRgbF(1.0f, 1.0f, 1.0f), intensity);
    }

    /* Tint a base color toward warm/cool by this rig's temperature and scale
     * all channels by `gain`. Approximates a simple blackbody tint used by
     * every material consistently. */
    QColor tinted(QColor base, float gain) const
    {
        /* temperature offset: 0 cool (-0.25r,+0.25b) .. 1 neutral .. 2 warm */
        const float warm = (temperature - 1.0f) * 0.35f;
        float r = qBound(0.0f, base.redF() + warm, 1.0f);
        float g = base.greenF();
        float b = qBound(0.0f, base.blueF() - warm, 1.0f);
        r = qBound(0.0f, r * gain, 1.0f);
        g = qBound(0.0f, g * gain, 1.0f);
        b = qBound(0.0f, b * gain, 1.0f);
        return QColor::fromRgbF(r, g, b);
    }
};

} // namespace art
} // namespace paintbrush

#endif // PB_ART_LIGHT_RIG_HPP