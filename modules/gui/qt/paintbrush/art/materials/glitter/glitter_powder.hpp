/*****************************************************************************
 * glitter_powder.hpp : NAI Paintbrush — glitter POWDER engine
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * Faithful principle/behaviour port of cray4pasta/glitter-powder (MIT).
 * That project runs its physics on the CPU as a heightfield of Float32Array
 * cells (powder height `h`, bonded `stuck`, glue, per-cell 2-slot colour,
 * flow/momentum, wind) and uploads the heightfield to a texture each frame
 * so the GPU only does the lighting/specular render. We keep that same
 * split: this engine is a CPU cell heightfield, rendered through the GPU
 * glitter fragment path. 1:1 on-par with the reference's physics.
 *
 * Tools ported: powder/pour (heap deposition with fbm rim + dust), smear/plow
 * (finger pushes loose grains, bonded stay, carries + sheds a little cling),
 * and glue (adhesion so powder bonds). Blow is exposed but the interactive
 * brush only drives pour + plow.
 *****************************************************************************/

#ifndef PB_ART_GLITTER_POWDER_HPP
#define PB_ART_GLITTER_POWDER_HPP

#include <QSize>
#include <QRect>
#include <QVector>
#include <QColor>
#include <QImage>
#include <QRandomGenerator>
#include <cstdint>

#include "../../runtime/art_scale.hpp"
#include "../../runtime/light_rig.hpp"
#include "../glitter/glitter_material.hpp"
#include "../../surface/paper_stock.hpp"

namespace paintbrush {
namespace art {
namespace glitter {

/* The glitter colours in the reference's default look. `#f2a7c8` pink is the
 * signature "add next" colour, `shimmer` 0.55 (partly iridescent). */
struct PowderPalette
{
    QVector<QColor> colors;   /* palette slots */
    float holographic = 0.55f;
    QColor paper{ 255, 255, 255 };
};
PowderPalette defaultPowderPalette();

/* One cell of the heightfield. Mirrors the reference's per-cell arrays:
 * `height`  total powder (loose + bonded),  `stuck`  bonded to the glue,
 * `glue`    wet-glue coverage 0..1, `seed` shuffle seed, `flow` (avalanche
 * momentum), `vx/vy` (loose glitter velocity), plus the two colour slots of
 * the LOOSE layer (slotA, slotB, blendF) and of the BONDED layer. */
struct PowderCell
{
    float height = 0.0f;
    float stuck = 0.0f;
    float glue = 0.0f;
    float seed = 0.0f;
    float flow = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    /* loose + bonded colour slots (index into palette; -1 none) */
    int  lA = -1, lB = -1;   float lF = 0.0f;
    int  sA = -1, sB = -1;   float sF = 0.0f;
};

/* A single airborne flake caught in the breath (reference's particle pool):
 * world position, velocity, a shallow gravitation, spin, size, brightness and
 * the palette slot it lifts from. Short-lived; spawned as the wind lifts flat
 * flakes and rendered as a bright, additive sparkle that arcs and falls. */
struct AirborneFlake
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    float rot = 0.0f, spin = 0.0f;
    float size = 1.0f;
    float life = 0.0f;
    int   slot = 0;
    bool  active = false;
};

/* Glitter powder engine: a bounded cell heightfield + the reference's step
 * math (pour, relax/avalanche, plow/smear, glue bonding). Deterministic from
 * a seeded RNG so exports/reopens reproduce the same layout. */
class GlitterPowder
{
public:
    GlitterPowder() = default;

    void reset(const QSize &cells, float worldCellSize, std::uint32_t seed = 1);

    int cellW() const { return m_cells.width(); }
    int cellH() const { return m_cells.height(); }
    int cellCount() const { return m_cells.width() * m_cells.height(); }
    float worldCellSize() const { return m_worldCellSize; }
    /* read-only per-cell access (flat row-major index), for the GPU surface
     * pass and diagnostics */
    const PowderCell &powderCellAt(int i) const { return m_cellsData[i]; }

    /* Adhesion: paint wet glue (coverage 0..1, -1 clears) into a disc. */
    void paintGlue(float wx, float wy, float radiusWorld, float coverage);

    /* POUR glitter: deposit a heap at (wx,wy) with the reference profile
     * (shape/stretch/rag/lumps/dust) and relax for `relaxPasses` steps. */
    void pourGlitter(float wx, float wy, float radiusWorld,
                     const GlitterParams &p);

    /* SMEAR / plow: push loose grains outward biased by (fx,fy); bonded
     * (stuck) stay. Some clings to the finger and is shed behind. */
    void smear(float wx, float wy, float fx, float fy, float radiusWorld,
               float carried);

    /* Bond loose powder to the glue (called each step). `cellRect` scopes the
     * sweep to a cell-coords rect; an invalid rect sweeps the ACTIVE region
     * (where powder actually lives), which keeps per-stroke cost proportional
     * to the touched area instead of the whole heightfield. */
    void absorb(const QRect &cellRect = QRect(), float rate = 1.0f);

    /* One relaxation (avalanche) pass — angle-of-repose flow with momentum.
     * Scoped exactly like absorb(): the active region by default. */
    void relax(const QRect &cellRect = QRect());
    void decayFlow(const QRect &cellRect = QRect());

    /* BLOW: the reference's breath. One physics step advects loose powder with
     * a wind cone (gusts/eddies/momentum), blowing it across and off the page;
     * loose powder over glue gets caught and bonds. Run repeatedly (e.g. ~60
     * steps/sec for ~2-3 s) for a full breath. Returns the number of cells that
     * moved (0 = all loose powder gone or settled). */
    int blowStep(float dtSec);

    /* Start a fresh breath: resets the blow-time/intensity ramp so the next
     * blowStep calls begin with a gentle puff that builds to full strength. */
    void beginBlow();

    /* Hydrate the heightfield from an already-rasterised powder layer (world
     * coords, `worldPerPx` converts layer px to world units). Used so a long-
     * press breath can clear powder that a PREVIOUS stroke already committed to
     * the raster, not just the current gesture's powder. Cells whose centre
     * samples a non-paper powder pixel get a representative height + colour. */
    void hydrateFromRaster(const QImage &layer, float worldPerPx);

    /* Render into an RGBA surface (world coords). The cell colours map to
     * palette slots exactly like the reference's uCol/uPal render.
     * `timeMs` drives the holographic shimmer so flakes catch the light and
     * fade over time exactly like the GPU reference (pass 0 for a static
     * deterministic frame). */
    QImage render(const QSize &outSize, float outOriginWorldX,
                  float outOriginWorldY, float worldPerPx,
                  const ArtLightRig &rig, const PowderPalette &pal,
                  const PaperStock *paper, const ArtScale &scale,
                  qint64 timeMs = 0) const;

    /* -- inspection / diagnostics -------------------------------------- */
    int looseCount() const;         /* cells with loose powder */
    int bondedCount() const;        /* cells with bonded powder */
    float looseMass() const;
    /* Loose (free) powder mass inside a world-space rect — used to tell whether
     * a plow/smear actually pushed anything (cheap, scoped to the rect). */
    float looseMassInRect(float wx0, float wy0, float wx1, float wy1) const;
    bool isEmpty() const;
    /* World-space bounding box (minx,miny,maxx,maxy) of cells carrying any
     * powder (loose or bonded), or false if the field is empty. */
    bool powderBoundsWorld(float &minx, float &miny, float &maxx, float &maxy) const;

    /* True if cell (cx,cy) was detected as carrying committed powder by the
     * most recent hydrateFromRaster(). Used to know which raster pixels the sim
     * now owns (so a blow can erase previously-committed powder instead of
     * leaving the old blobs behind). */
    bool hydatedPowderAt(int cx, int cy) const;
    const QVector<quint8> &powderMask() const { return m_powderMask; }
    bool hasPowderMask() const { return !m_powderMask.isEmpty(); }
    /* Erase previously-committed powder from `img` in the cells the mask
     * flagged (restoring `paper` under them), so a blow can clear old powder
     * instead of leaving the blobs in the raster. `originWorldX/Y/worldPerPx`
     * map img pixels to world/cell space. */
    void clearMaskedPowder(QImage &img, float originWorldX, float originWorldY,
                           float worldPerPx, const QColor &paper) const;

    /* seeded RNG for reproducible step math */
    void setSeed(std::uint32_t seed);

private:
    int idx(int cx, int cy) const { return cy * m_cells.width() + cx; }
    void mixInto(int i, float m1, int a2, int b2, float f2, float m2,
                 bool bond);
    void addLoose(int i, float m, int a, int b, float f);
    void setStuck(int i, float ns);
    int clampCell(float cx, float cy) const;
    PowderCell &cellAt(int i) { return m_cellsData[i]; }
    const PowderCell &cellAt(int i) const { return m_cellsData[i]; }

    QSize m_cells;
    float m_worldCellSize = 1.0f;
    QVector<PowderCell> m_cellsData;
    QRandomGenerator m_rng;
    /* the palette slots currently in use (grow on demand, freed when empty) */
    QVector<QColor> m_palette;
    /* per-step momentum bookkeeping for blowStep (reference outM/mIn/pX/pY/dh)
     * — only allocated lazily on the first blow */
    QVector<float> m_dh, m_outM, m_mIn, m_pX, m_pY;
    mutable QVector<float> m_floatScratch;
    /* persistent blow incoming-colour lanes (reference iA/iB/iF), sized once by
     * ensureBlowBuffers instead of re-allocating every 120 Hz blow tick */
    QVector<int> m_inA, m_inB;
    QVector<float> m_inF;
    /* per-cell committed-powder mask filled by hydrateFromRaster (1 = that cell
     * held powder when hydrated); lets a blow clear previously-committed powder
     * instead of leaving the old blobs in the raster */
    QVector<quint8> m_powderMask;
    /* Active-region tracking: union of every cell rect that ever received
     * powder (pour/smear/hydrate, and the moving-mass targets of each blow
     * step). relax()/decayFlow()/blowStep() sweep ONLY this rect (+ a small
     * margin) instead of the whole heightfield, so a 2 MP grid costs nothing
     * until powder actually spreads there. */
    QRect m_activeCells;
    float m_blowTime = 0.0f;
    bool m_particlesActive = false;
    /* Airborne-flake fleet (bounded pool). */
    void ensureParticlePool();
    void spawnFlake(float wx, float wy, float ux, float uy, float speed,
                    float size, float mass, int slot);
    void stepParticles(float dtSec);
    QVector<AirborneFlake> m_flakes;
    int m_maxFlakes = 0;
    void ensureBlowBuffers();
};

/* Convenience: world cell size for powder so flakes keep a stable size. */
float powderWorldCellSize(const GlitterParams &p, float flakeScale);

} // namespace glitter
} // namespace art
} // namespace paintbrush

#endif // PB_ART_GLITTER_POWDER_HPP