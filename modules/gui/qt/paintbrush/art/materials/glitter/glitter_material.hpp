/*****************************************************************************
 * glitter_material.hpp : NAI Paintbrush — flagship glitter art material
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_GLITTER_MATERIAL_HPP
#define PB_ART_GLITTER_MATERIAL_HPP

#include <QString>
#include <QColor>
#include <QVector>
#include <QSize>
#include <QRect>
#include <QImage>
#include <QRgb>
#include <QPointF>
#include <QByteArray>
#include <QtGlobal>
#include <cstdint>

#include "../../runtime/art_scale.hpp"
#include "../../runtime/light_rig.hpp"
#include "../../surface/paper_stock.hpp"

namespace paintbrush {
namespace art {

/* Glitter parameter set (spec §14.3 primary + advanced).
 *
 * The material models, following cray4pasta/glitter-powder as the quality
 * reference (MIT, principle-port only):
 *   - adhesion  : how much wet "glue" is applied (0 none .. 1 fully glued)
 *   - glued     : fraction of the applied glitter that BONDS to the surface
 *   - looseMix  : fraction treated as LOOSE grains (free to shear/displace)
 *   - density   : grains per world-area (drops toward 0 with distance)
 *   - flakeSize : world-units scale of each grain (stable across zoom/export)
 *   - shimmer   : strength of the faceted specular sparkle
 *   - holographic : how much each grain shifts hue with facet angle
 *   - palette   : a bounded set of named grain colors (not averaged RGB soup)
 */
struct GlitterParams
{
    QColor color{ 0xE8, 0xC6, 0x2A };   /* base glitter color */
    float amount = 1.0f;                /* coverage 0..2 */
    float flakeSize = 0.8f;             /* world-unit flake scale */
    float shimmer = 0.55f;              /* specular sparkle 0..1 */
    float adhesion = 0.6f;              /* glue strength 0..1 */
    float looseMix = 0.3f;              /* 0..1 fraction loose vs bonded */
    float holographic = 0.0f;           /* 0..1 hue-shift per facet */
    float dispersion = 0.5f;            /* how spread/uneven the grains are */
    /* Bounded palette: when non-empty, grains sample from these named colors
     * (weighted by the base color) instead of one averaged color. */
    QVector<QColor> palette;
    /* Deterministic seed (spec §37): the same seed reproduces the same grain
     * layout at any zoom/export. */
    std::uint32_t seed = 1;
};

namespace glitter {

/* A bounded palette slot: one grain color identity (not averaged). */
struct PaletteSlot
{
    QColor color;
    float weight = 1.0f;
    QString name;
};

/* Build the default bounded palette (a few harmonious glitter colors) from a
 * seed so multi-color glitter stays grain-like. `count` entries. */
QVector<PaletteSlot> defaultPalette(std::uint32_t seed, int count = 6);

/* A single glitter grain's mutable sim state within a cell. Grains live in a
 * bounded grid; only cells under active glue / near a painted dab are
 * populated. */
struct Grain
{
    float x = 0.5f;     /* position within cell, 0..1 */
    float y = 0.5f;
    float size = 1.0f;  /* flake size multiplier */
    float rot = 0.0f;   /* facet rotation (rad) */
    std::uint16_t height = 0; /* 0..1024: bonded layering (loose sit higher) */
    std::uint8_t slot = 0;    /* palette slot index */
    bool bonded = false;
};

/* The glitter SIMULATION state over a bounded grid. Rationalized vs spec
 * §14.5: fixed/bounded grids, no per-particle CPU object allocation, only
 * active regions update, deterministic from seed + state. The grid is in
 * *world* coordinates so it is resolution independent. */
class GlitterState
{
public:
    ~GlitterState() = default;

    /* Fleet capacity: cells * maxGrainsPerCell. resize() must be called before
     * use and should reflect the document's painted region at the chosen world
     * cell size. */
    static constexpr int kMaxGrainsPerCell = 8;

    /* Resize the grid to cover a world area at `worldCellSize` world units per
     * cell. Existing grain data is dropped (content reset). */
    void reset(const QSize &cells, float worldCellSize);

    int cellW() const { return m_cells.width(); }
    int cellH() const { return m_cells.height(); }

    /* The adhesion model: paint "glue" intensity (0..1 coverage, -1 clears)
     * into the cell at world coords. Pure deposit, cheap, no allocation. */
    void paintGlue(float wx, float wy, float radiusWorld, float coverage);

    /* Disperse glitter into the given world-radius disc: apply/clear grains
     * based on transport, coverage and adhesion. `transport` is a push vector
     * (loose grains shear along it), simulating the "push/blow" displacement
     * while bonded grains stay. */
    void applyGlitter(float wx, float wy, float radiusWorld,
                      const GlitterParams &p, const QPointF &transport,
                      float coverage);

    /* Render the glitter into an RGBA surface covering `canvasRectWorldBuffer`.
     * `outOriginWorld`/`worldPerPx` convert device pixels <-> world space.
     * Uses the shared rig for the faceted specular response. Deterministic
     * from seed + the stored grain state (screen == export). */
    QImage render(const QSize &outSize, float outOriginWorldX,
                  float outOriginWorldY, float worldPerPx,
                  const ArtLightRig &rig,
                  const PaperStock *paper, const ArtScale &scale) const;

    /* -- inspection (harness / diagnostics) ------------------------------ */
    int grainCount() const;
    int bondedCount() const;
    int looseCount() const;
    float worldCellSize() const { return m_worldCellSize; }
    bool isEmpty() const { return m_grid.isEmpty(); }

    /* GPU path: per-cell grain access (read-only) and the palette, so the
     * shader vertex list is built from the same state the CPU renders. */
    struct CellView
    {
        float glue = 0.0f;
        int count = 0;
        const Grain *grains = nullptr;
    };
    CellView cellView(int cx, int cy) const;
    /* The palette in effect (the stored one, or the seeded default when the
     * state carries none — same fallback the CPU render uses). */
    QVector<PaletteSlot> paletteForRender() const;

    /* -- persistence ------------------------------------------------------ */
    /* Compact binary material state (spec §52: binary state lives in
     * assets/chunks, never giant base64 JSON). Round-trips exactly, so
     * reopen + export reproduce the identical grain layout. */
    QByteArray serialize() const;
    bool deserialize(const QByteArray &bytes);

private:
    /* Cell: fixed-capacity grain array. To keep the header light we store a
     * plain struct array; allocation is bounded at reset(). */
    struct Cell
    {
        Grain grains[kMaxGrainsPerCell];
        std::uint8_t count = 0;
        float glue = 0.0f;   /* wet glue coverage 0..1 */
    };

    Cell &cell(int cx, int cy);
    const Cell &cell(int cx, int cy) const;

    QSize m_cells;
    float m_worldCellSize = 1.0f;
    QVector<Cell> m_grid;    /* dense, bounded */
    QVector<PaletteSlot> m_palette;
};

/* Convenience: compute world cell size for a glitter material so flakes keep a
 * stable apparent size (each flake ~ flakeSize * cell). */
float glitterWorldCellSize(const GlitterParams &p, float flakeScale);

} // namespace glitter
} // namespace art
} // namespace paintbrush

#endif // PB_ART_GLITTER_MATERIAL_HPP