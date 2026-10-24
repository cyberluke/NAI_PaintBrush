/*****************************************************************************
 * paper_stock.hpp : NAI Paintbrush — paper is a material, not a color
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_PAPER_STOCK_HPP
#define PB_ART_PAPER_STOCK_HPP

#include <QString>
#include <QColor>
#include <QVector>
#include <QtGlobal>
#include <cstdint>

#include "../runtime/art_scale.hpp"
#include "../runtime/light_rig.hpp"

namespace paintbrush {
namespace art {

/* Paper has physical/visual identity (spec §8.1). A white RGB rectangle is
 * never the product default. The exact fields may evolve, but the invariant
 * is: paper is a material with fiber, tooth, roughness, absorption and light
 * response — expressed in world units through ArtScale, so grain does not
 * shrink into invisible noise at 4K or swell into blobs at small export. */
struct PaperStock
{
    QString id;                 /* "luxury.greige", "saturated.pink", ... */
    QString displayName;
    QColor baseColor{ 0xF0, 0xEC, 0xE4 }; /* warm cream default */
    /* Fiber: fine cotton vs smooth coated. fiberStrength 0..1, fiberScale in
     * world units (distance between fiber strands' noise cells). */
    float fiberScale = 0.04f;   /* world units per fiber cell (small) */
    float fiberStrength = 0.5f; /* 0 = smooth, 1 = heavily fibrous */
    /* Tooth: micro-height variation that textures brushing and catches light. */
    float tooth = 0.5f;
    /* Surface roughness 0 (glossy coated) .. 1 (rough matte). */
    float roughness = 0.6f;
    /* Absorption 0 (impermeable coated) .. 1 (blotter): controls how paint /
     * pigment pools and edge-darkens. */
    float absorption = 0.5f;
    /* microHeight: tonal variance magnitude of the grain (0..1). */
    float microHeight = 0.12f;
    /* Specular response strength (coated paper glints). */
    float specular = 0.08f;
    /* Warmth shift applied to the base color under neutral light. */
    float warmth = 0.0f;        /* -1 cool .. +1 warm */
    /* Deterministic seed (spec §37): the same paper reproduces identically. */
    std::uint32_t seed = 0x50415031u; /* "PAP1" */
    /* Reference scale this preset was authored at (world units per inch). The
     * renderer multiplies the fiber/grain size by the ratio to the document
     * ArtScale so paper stays stable across zoom and export. */
    float authoringUnitsPerInch = 96.0f;

    /* Pre-multiplied-ish sample of the base color tinted by warmth. */
    QColor surfaceColor(const ArtLightRig &rig) const
    {
        float r = baseColor.redF();
        float g = baseColor.greenF();
        float b = baseColor.blueF();
        r = qBound(0.0f, r + warmth * 0.04f, 1.0f);
        g = qBound(0.0f, g + warmth * 0.02f, 1.0f);
        b = qBound(0.0f, b - warmth * 0.04f, 1.0f);
        /* soft ambient lift so paper is never pure black in a shadow */
        float ambient = qBound(0.0f, rig.ambient, 1.0f);
        r = qBound(0.0f, r * (0.75f + 0.25f * ambient), 1.0f);
        g = qBound(0.0f, g * (0.75f + 0.25f * ambient), 1.0f);
        b = qBound(0.0f, b * (0.75f + 0.25f * ambient), 1.0f);
        return QColor::fromRgbF(r, g, b);
    }
};

namespace paper {

/* The small exceptional initial set (spec §8.2). Exactly six, later grown by
 * Mood presets referencing them, never by dumping a 50-preset menu. */
const PaperStock &luxuryGreige();
const PaperStock &saturatedPink();
const PaperStock &bubblegumY2k();
const PaperStock &emoAsh();
const PaperStock &warmEditorialCream();
const PaperStock &matteBlack();

/* Return the six built-ins in display order. */
QVector<const PaperStock *> presets();

/* Look up a preset by id; returns nullptr if unknown. */
const PaperStock *findById(const QString &id);

} // namespace paper
} // namespace art
} // namespace paintbrush

#endif // PB_ART_PAPER_STOCK_HPP