/*****************************************************************************
 * art_scale.hpp : NAI Paintbrush — physical/world scale for art materials
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_SCALE_HPP
#define PB_ART_SCALE_HPP

#include <QString>
#include <QSizeF>
#include <QtGlobal>

namespace paintbrush {
namespace art {

/* Physical/world scale for the Art Materials Runtime.
 *
 * Spec §11: a material must never be defined only as "flakeSize = 3 px". The
 * same glitter must not become boulders when exported 4x, and the same paper
 * grain must not shrink into invisible noise at 4K. Every material expresses
 * its meaningful sizes against this one shared scale.
 *
 * The model: the document has a reference DPI and a canvas size given in
 * logical document units (world units). A material parameter stored in world
 * units (millimetres-ish, fraction-of-canvas, or microns) is converted into
 * *device* pixels through this struct. `exportScale = 1` means device ==
 * canvas logical; 2 renders at 2x into the final raster.
 *
 * The canonical invariant is: material feature size in *world* units ×
 * worldUnitsPerInch / referenceDpi × devicePixelRatio × exportScale
 *   = feature size in actual output pixels.
 * Equivalently, keep `worldUnitsPerInch == referenceDpi` at 1x and let
 * devicePixelRatio * exportScale carry zoom/export. */
struct ArtScale
{
    /* Design DPI the material parameters were authored for. */
    float referenceDpi = 96.0f;
    /* How many document/world units map to one physical inch. For a 960x720
     * canvas shown at 10 in x 7.5 in, this is 96.0 units/inch. */
    float worldUnitsPerInch = 96.0f;
    /* Window device pixel ratio (Retina / 4K scaling). */
    float devicePixelRatio = 1.0f;
    /* Export scale (1x screen, 2x/4x export). */
    float exportScale = 1.0f;

    /* World units -> device pixels at current presentation. */
    float worldToDevice(float worldUnits) const
    {
        const float unitPerPx = referenceDpi / worldUnitsPerInch;
        return worldUnits * unitPerPx * devicePixelRatio * exportScale;
    }
    /* Device pixels -> world units. */
    float deviceToWorld(float devicePx) const
    {
        const float pxPerUnit = devicePixelRatio * exportScale;
        return devicePx / pxPerUnit * (referenceDpi / worldUnitsPerInch);
    }
    /* Combined zoom factor vs. 1:1 design (includes UI zoom + export scale). */
    float totalScale() const { return devicePixelRatio * exportScale; }
};

} // namespace art
} // namespace paintbrush

#endif // PB_ART_SCALE_HPP