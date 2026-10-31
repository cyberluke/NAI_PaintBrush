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

/* The material-preset identity for a Glitter-POWDER layer in the paintbrush
 * material registry (`materialPreset` on a CanvasLayer). The grain "glitter"
 * material uses "material.glitter.fine"; powder uses this so save/load, export
 * and the document renderer can re-derive from the serialized physical state. */
const char kPowderMaterialPreset[] = "material.glitter.powder";

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

/* A single airborne flake caught in the breath (reference's particle pool).
 * Ported ~1:1 from cray4pasta/glitter-powder `spawn`/`stepParticles`: a flat
 * flake carries a per-flake MASS, its own terminal fall velocity `vt`, flutter
 * strength `flutter` / phase `phase`, mirror-axis spin `axis`, thin-film cycle
 * `film`, and falls until `z` crosses the bed, where it splats() its mass back
 * into the heightfield (never silently destroyed). `vz` is the vertical
 * launch velocity (reference passes `4 + 14*random` there), shared by the
 * bottom-lift path; `size` is the fanned-out visual flake radius generated
 * independently. World units (x/y/mm all meters/cells); cell-space physics,
 * world/device conversion happens only in the renderer. */
struct AirborneFlake
{
    float x = 0.0f, y = 0.0f, z = 0.0f;   /* world position (z = height above bed) */
    float vx = 0.0f, vy = 0.0f, vz = 0.0f; /* world velocity (vz = vertical launch) */
    float mass = 0.0f;                     /* powder mass the flake carries */
    float spin = 0.0f, rate = 0.0f;        /* current spin + spin rate */
    float axis = 0.0f;                     /* mirror axis of the spinning flake */
    float film = 0.0f;                     /* thin-film cycle phase seed */
    float size = 1.0f;                     /* visual radius scale (reference 1.6..5) */
    float vt  = 0.0f;                      /* terminal fall speed (clamps vz) */
    float flutter = 0.0f;                  /* flutter strength of the drift */
    float phase = 0.0f;                    /* per-flake flutter phase offset */
    int   slot = 0;                        /* palette slot it lifts from */
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

    /* POUR MAIN (reference pourMain): ONE physical heap of the current colour
     * fitted to the material. `fit=true` (the momentary Pour action) shapes the
     * heap to just cover the BARE glue — cells with wet glue but not yet enough
     * bonded glitter — so pouring again over a piece in progress keeps the
     * earlier colours; with no bare glue left it fits the whole glue art, and
     * with no glue it drops a heap at the material centre (or the middle of the
     * field). A single anisotropic heap replaces any regular grid of repeated
     * stamp centres (no rows/columns/checker/harmonics), and it PRESERVES all
     * existing glitter and colour slots. One call = one heap event. */
    void pourGlitterMain(bool fit, const GlitterParams &p);

    /* SMEAR / plow: push loose grains outward biased by (fx,fy); bonded
     * (stuck) stay. `carried` is IGNORED — the cling mass and its colour live
     * in the engine (`m_carried` / `m_carriedSlot`), persistent across the
     * whole Push gesture so powder really sticks to the finger and sheds
     * behind (reference CLING_MAX = 6, not 0.6). The controller calls
     * beginPushGesture() once when the Push stroke starts and endPushGesture()
     * when it lets go, so carrying is reset per gesture exactly like the
     * reference. */
    void smear(float wx, float wy, float fx, float fy, float radiusWorld,
               float /*carried*/ = 0.0f);
    /* Push-gesture bookkeeping: reset/read the persistent finger-carry state
     * so powder clings across dabs within one stroke but is dropped at its
     * end. */
    void beginPushGesture();
    void endPushGesture();

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

    /* Finish a breath cleanly: clears the blow transient buffers so a later
     * persist/serialize captures a settled heightfield (drops per-step
     * momentum). Safe to call any time. */
    void endBlow();

    /* ERASER (reference physical erase): physically remove powder and glue
     * under a disc. Loose powder, bonded powder and wet glue within `radiusWorld`
     * of (wx,wy) are all cleared, so erasing part of a stroke really takes the
     * material away — it is NOT the normal raster eraser. */
    void erase(float wx, float wy, float radiusWorld, float amount = 1.0f);

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
    /* Total PHYSICAL material inside a world-space rect: loose + bonded powder
     * + wet glue. Used to tell whether an Eraser actually changed material even
     * when it removed a 100%-bonded or glue-only area (whose loose mass is
     * zero) — the loose-mass heuristic alone is not enough for the physical
     * eraser. */
    bool hasMaterialInRect(float wx0, float wy0, float wx1, float wy1) const;
    /* Deterministic before/after material fingerprint of a rect: a 64-bit hash
     * over the affected cells' (height, stuck, glue, low colour slot s) and the
     * sub-res glue field in that window. The Eraser controller compares this
     * before vs after an eraser dab to drive undo/commit exactly, instead of
     * guessing from loose mass. */
    quint64 materialRectFingerprint(float wx0, float wy0,
                                    float wx1, float wy1) const;
    /* True while any airborne flake is still in flight. The Blow controller
     * MUST NOT finalize/commit while this is true: a canonical bake that
     * serializes `materialState` must never contain transient particle mass
     * that `materialState` doesn't hold (the raster == materialState invariant,
     * reviewer P0 #1). */
    bool hasActiveParticles() const;
    bool isEmpty() const;
    /* Material-state predicates. A plain raster/paint layer is not a material;
     * a Glitter Powder physical layer is a material when it carries any powder
     * OR any wet glue. `isEmpty()` for a Glitter Powder material must return
     * false when meaningful glue coverage exists (otherwise glue-only layers
     * would be dropped from persistence). */
    bool hasGlue() const;      /* any cell with meaningful wet-glue coverage */
    bool hasPowder() const;    /* any cell with loose or bonded powder */
    bool hasMaterial() const { return hasPowder() || hasGlue(); }

    /* Full-resolution (sub-cell) world-anchored glue coverage, sampled with
     * bilinear interpolation at an arbitrary world position (P0.2). The coarse
     * cell `glue` is the physics adhesion approximation; this field carries the
     * crisp paint edge / wet-glue normal / bonding boundary used by the render
     * path. Returns 0..1. */
    float gluePresence(float wx, float wy) const;
    /* Sub-cell resolution of the glue field (P0.2). */
    static int glueSubRes() { return kGlueSubRes; }
    bool hasGlueField() const { return !m_glueField.isEmpty(); }
    const QVector<quint8> &glueFieldBytes() const { return m_glueField; }
    /* World-space bounding box (minx,miny,maxx,maxy) of cells carrying any
     * loose or bonded powder, or false if the field holds no powder. */
    bool powderBoundsWorld(float &minx, float &miny, float &maxx, float &maxy) const;
    /* World-space bounding box of ALL physical material: wet glue + bonded +
     * loose glitter. Used to drive the dirty/commit render union so glue-only
     * strokes are never clipped to an empty (powder-derived) rect. */
    bool materialBoundsWorld(float &minx, float &miny, float &maxx, float &maxy) const;

    /* True if cell (cx,cy) was detected as carrying committed powder by the
     * most recent hydrateFromRaster(). Used to know which raster pixels the sim
     * now owns (so a blow can erase previously-committed powder instead of
     * leaving the old blobs behind). */
    bool hydatedPowderAt(int cx, int cy) const;
    /* The engine's working colour palette (the reference's per-cell slots),
     * recoverable after a deserialize so the render palette can be rebuilt. */
    QVector<QColor> palette() const { return m_palette; }

    /* Resolve the CURRENT foreground colour to a bounded material-palette slot
     * (0..15), appending a colour when a free slot exists and reusing the
     * nearest existing colour when the palette is full. Existing grains keep
     * their original slot IDs, so changing the UI colour deposits NEW glitter
     * of that colour instead of rewriting the whole body. Returns -1 only if
     * the colour is invalid. */
    int ensurePowderPaletteSlot(const QColor &color);
    const QVector<quint8> &powderMask() const { return m_powderMask; }
    bool hasPowderMask() const { return !m_powderMask.isEmpty(); }
    /* Erase previously-committed powder from `img` in the cells the mask
     * flagged, restoring TRANSPARENT ((0,0,0,0)) under them — never a baked
     * paper colour (the document compositor owns PaperStock). Matches the
     * transparent material-layer contract. `paper` is retained as a
     * compatibility parameter and ignored. `originWorldX/Y/worldPerPx` map img
     * pixels to world/cell space. */
    void clearMaskedPowder(QImage &img, float originWorldX, float originWorldY,
                           float worldPerPx, const QColor &paper) const;

    /* seeded RNG for reproducible step math */
    void setSeed(std::uint32_t seed);

    /* -- persistence ------------------------------------------------------
     * Versioned binary serialization of the full physical material state so a
     * powder layer can be saved/loaded, switched between tools without loss,
     * and re-rendered at export from state instead of only the cached raster.
     * Round-trips every field the physics needs (PowderCell all, plus grid /
     * world-cell / palette / RNG seed / active region). NOT a raster snapshot.
     * Magic "GLTP" + version + u32 header + packed cell records. */
    QByteArray serialize() const;
    bool deserialize(const QByteArray &bytes);

private:
    int idx(int cx, int cy) const { return cy * m_cells.width() + cx; }
    void mixInto(int i, float m1, int a2, int b2, float f2, float m2,
                 bool bond);
    void addLoose(int i, float m, int a, int b, float f);
    void setStuck(int i, float ns);
    int clampCell(float cx, float cy) const;
    PowderCell &cellAt(int i) { return m_cellsData[i]; }
    const PowderCell &cellAt(int i) const { return m_cellsData[i]; }
    /* Core heap deposition (reference pour()): deposits one irregular fbm-rim
     * heap into `slot` at cell centre (cx,cy) with the reference profile
     * (shape/stretch/rag/lumps/dust/peak) and relaxes it. Shared by the brush
     * sprinkle (pourGlitter) and the fitted pourMain. */
    void pourHeap(int slot, float cx, float cy, float rCells, float peak,
                  float shape, float stretch, float rag, float lumps,
                  float dust);

    QSize m_cells;
    float m_worldCellSize = 1.0f;
    QVector<PowderCell> m_cellsData;
    /* Full-resolution world-anchored glue field (P0.2): kGlueSubRes sub-samples
     * per cell edge, stored as coverage 0..255. Independent of the coarse cell
     * `glue` (which stays the physics adhesion approximation). Painted by
     * paintGlue alongside the cell, sampled by gluePresence() for crisp glue
     * edges / wet-glue normals / bonding boundary, and persisted in state. */
    enum { kGlueSubRes = 8 };
    QVector<quint8> m_glueField;
    inline int glueFieldIndex(int subX, int subY) const
    { return subY * (m_cells.width() * kGlueSubRes) + subX; }
    QRandomGenerator m_rng;
    /* Deterministic-RNG accounting (reviewer P2 #8): every stochastic draw the
     * engine makes goes through nextFloat()/nextBounded(), which bump
     * m_rngCounter and draw from a stream seeded entirely by the persisted
     * {m_baseSeed, m_rngCounter}. serialize() persists m_rngCounter (+ the base
     * seed), so reload reproduces the SAME random sequence for a given
     * {grid, seed, operation ordering} — a reloaded document continues the
     * deterministic persisted material model instead of inventing a fresh
     * sequence (which is what seeding only from gridW/gridH did before). */
    std::uint32_t m_baseSeed = 1u;
    std::uint64_t m_rngCounter = 0u;
    float nextFloat();
    float nextFloatUnit(float hi);
    int nextBounded(int n);
    /* persistent Push-gesture carry (reference `carried` / `carriedSlot`) */
    float m_carried = 0.0f;
    int   m_carriedSlot = -1;
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
    void spawnFlake(float wx, float wy, float hx, float hy, float hSpeed,
                    float vz, float mass, int slot);
    void stepParticles(float dtSec);
    void splat(float x, float y, float m, int slot);
    QVector<AirborneFlake> m_flakes;
    int m_maxFlakes = 0;
    int m_flakeCount = 0;
    /* wind geometry snapshot published by blowStep so stepParticles can push
     * in-flight flakes with the same cone/gust as the bed advection */
    float m_windMx = 0.0f, m_windMy = 0.0f, m_windD0 = 0.0f;
    float m_windGust = 0.0f, m_windAim = 0.0f, m_windCA = 1.0f, m_windSA = 0.0f;
    void ensureBlowBuffers();
};

/* Convenience: world cell size for powder so flakes keep a stable size. */
float powderWorldCellSize(const GlitterParams &p, float flakeScale);

} // namespace glitter
} // namespace art
} // namespace paintbrush

#endif // PB_ART_GLITTER_POWDER_HPP