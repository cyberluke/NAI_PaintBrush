/*****************************************************************************
 * pb_powder_pack.hpp : CPU sim -> GPU powder texture data (shared)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#ifndef PB_POWDER_PACK_HPP
#define PB_POWDER_PACK_HPP

#include "pb_powder_scene_node.hpp"

#include "../materials/glitter/glitter_powder.hpp"

namespace paintbrush {
namespace art {

/* Pack the authoritative CPU heightfield/colour/glue state into the exact
 * textures the powder fragment shader expects. This is the only CPU work
 * left in the zero-readback preview path:
 *   - the physics itself (fast, ~0.6 ms/pour),
 *   - this packing (WINDOWED: only the cells the dirty union touches),
 *   - a partial texture upload (no readback).
 *
 * `cellWindow` is the grid-space window to pack; it is clamped to the grid.
 * `packGlue` must be false unless the glue field revision advanced (the glue
 * field is kGlueSubRes^2 = 64 bytes per cell, so re-packing it every frame was
 * the dominant cost of the live pass). The full-grid texture stays resident;
 * the caller uploads the returned window at cellX/cellY. */
PowderSceneData packPowderScene(
    const glitter::GlitterPowder &powder,
    const glitter::PowderPalette &pal,
    const ArtLightRig &rig, const PaperStock *paper,
    double worldPerPx, double timeMs, float flakeWorld, int dpr,
    const QRect &cellWindow, bool packGlue);

} // namespace art
} // namespace paintbrush

#endif // PB_POWDER_PACK_HPP
