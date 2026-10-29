# Paintbrush — Living Harvest (Motion Toys Stage 3)

**Status: IMPLEMENTED + VERIFIED** (68/68 living harness, 92/92 motion harness,
141/141 art harness, plugin build `NINJA_EXIT=0`)
**Date:** 2026-10-24 — **Branch/work:** uncommitted working tree

## What Stage 3 is

A harvested photographic person becomes a **living poster**: the immutable
`HarvestItem` is rigged once (`rigHarvest`), a source-motion window is chosen
automatically from the tray track, the layer references the asset with a
photo-safe amplitude knob, and a canonical `renderAtTime(t)` drives a
deterministic 3-second PNG frame export. All motion is photo-regime adapted
(±12° cap from Stage 2); the original bitmap is never modified; **one Undo
restores the static figure**.

## Product rules enforced (from the Stage 3 spec)

| Rule | Where |
|---|---|
| ONE living layer per poster | `BoostEngine::animateHero` + layer scan |
| Photo regime default; amplitudes Subtle 0.35 / Normal 0.65 / Strong 1.0 | `PuppetAsset::defaultAmplitude = 0.65`, controller clamp 0.01..1.0 |
| 1.0 = photo-safe adapted motion, never raw source | `retargetClip` photo cap (Stage 2, 92/92) |
| HarvestItem bitmap immutable | factory never writes; harness check `cutout == cutoutBefore` |
| One typed Undo → static figure | `SetPuppetCommand` (before/after pair); `AddPuppetCommand` (store add/remove) |
| Rig once; rebuild deterministically on load | `PuppetAssetStore` manifest = ids + clip + settings (no pixels); rigs rebuilt from tray |
| No pose estimation; row-profile fit only | unchanged Stage-2 pipeline |

## Implementation

### Runtime (`modules/gui/qt/paintbrush/motion/`)
- `pb_puppet.hpp/.cpp` (extended, still VLC-free):
  - `PuppetEffect::phase` 0..1 + serialization v2 (23 B; v1 19 B loads with
    phase 0) — `renderAtTime` samples `time + phase*period` scaled by speed.
  - `PuppetRegime` (Photo/Illustration), `PuppetAsset` (id, sourceHarvestId,
    rig, motion, regime, defaultAmplitude=0.65; rig-less manifest
    serialize/fromManifest).
  - `PuppetMotionScanner`: `FrameSample`, `ScanOptions`, `ScanResult`,
    deterministic `scan()` (variation, loop-closure cost, character match to
    the harvest pose, energy penalty, occlusion, quality, anchor bonus for
    windows starting at the harvest PTS) + `proceduralFallback()` — **opens
    and closes at REST**, so t=0 with phase 0 is exactly the static figure
    and the loop is seamless by construction.
  - `PuppetRuntime::renderAtTime(asset, effect, t)`.
  - `PuppetBuilder::build` now binarizes texture alpha and mask (asset
    semantics: alpha = binary mask; soft SAM edges no longer erode the
    silhouette).
- `pb_living.hpp/.cpp` (new): `makeLivingAsset(harvest, samples, opts)` (rig
  once → scan → `retargetClip` photo-adapted → procedural sway fallback;
  Photo regime, 0.65 default) + `PuppetAssetStore` (rig builder, add/remove/
  byId/assetByHarvest/assetIdByHarvest, manifest persistence; `loadManifest`
  replaces the store and drops assets whose harvest cannot be rebuilt).

### Product layer
- `pb_layer.hpp`: `puppetAssetId`, `puppetEffect`, `hasPuppet()`.
- `pb_history.hpp/.cpp`: `SetPuppetCommand` (layerId + before/after
  assetId+effect; apply/revert never touches the raster), `AddPuppetCommand`.
- `pb_document.hpp/.cpp`: `PuppetAssetStore m_puppets` (tray-fed rig builder
  in the ctor), `puppets()`, `prunePuppetRefs()`; time-aware renderer:
  `effectiveSourceAt` resolves living layers inside the existing scale/mask/
  finish path, `compositeLayerAt`, `composite(doc,t)`, `render(doc,t)`,
  `renderRegion(doc,region,t)`, `renderScaled(doc,scale,t)`,
  `renderTransparentAt(doc,scale,t)`; the old signatures delegate (t=0).
- `pb_boost.hpp/.cpp`: `BoostTransaction::setPuppet` / `addPuppetAsset`
  (returns assigned id); `BoostEngine::animateHero(tx, a)` — hidden boost
  tool: hero-only, skips when any living layer exists, builds the asset in
  the SAME undo step (`tx.note`), amplitude = photo-safe default 0.65,
  `Loop` clip; `PaintbrushController::boost()` calls it after
  `planDeterministic`.
- `pb_project_store.cpp`: layer `puppetAssetId` + `puppetEffect` (base64,
  v2), root `puppets` manifest; load order tray → assets → `prunePuppetRefs`.
- `pb_export.hpp/.cpp`: `exportLivingPoster(doc, dir, stem, durationMs, fps,
  paper, scale)` — canonical time grid `t = i*duration/frames`, frame 0 ==
  the static poster, `living_%04d.png`, deterministic.
- `pb_controller.hpp` + `pb_controller_art.cpp`: `rigHarvest`,
  `setLayerPuppet`, `removeLayerPuppet`, `layerHasPuppet`,
  `layerPuppetAmplitude`, `layerPuppetAssetId`, `riggedAssetCount`,
  `firstRiggedAssetId`, `exportLivingPoster` + boost integration.
- QML: inspector **Motion** group (Off/Subtle/Normal/Strong), tray context
  menu **"Make it living"**, workspace **export living poster (3 s)** button.
- Build: `meson.build` + `pb-ninja-add-sources.py` register
  `motion/pb_living.cpp`.

## Verification (requirement-specific)

### New harness `_REVIEW/harness/pb_living_harness/` — **68/68**
Real sources: pb_common/layer/document/history/harvest/boost/project_store/
export + motion/pb_puppet + motion/pb_living + art runtime, stub VLC.

- **Scanner**: deterministic (identical result twice); picks the dancing
  window (start=12 span=6 score=39.06 on a 20-sample synthetic track);
  static-only track → no window.
- **makeLivingAsset (synth humanoid + REAL SAM3 photo)**: rig valid; regime
  Photo; default amplitude 0.65; clip present; harvest bitmap byte-identical
  after rigging; **t=0 == static figure** (opaque-mean diff 1.3 / 4.3, bounds
  4 / 5 — same methodology and bounds as the Stage-1 identity check);
  `renderAtTime(0) == identity rest render`; **loop seamless** (worst seam
  mad 0.0000/255); **phase 0.5 == half-period shift** (byte-identical by
  construction) and phase 0.5 ≠ phase 0 at the same wall time;
  **amplitude ordering** 0.35 < 0.65 < 1.0 (strictly increasing
  deformation); **Strong (1.0) photo-safe** (opaque overlap 0.97+ vs the
  static figure); PuppetEffect v2 round-trip + v1 compat.
- **Document flow**: SetPuppet apply/revert restores the exact static layer
  (raster byte-identical); renderer varies with t only for living layers;
  deterministic per t; `renderScaled`/`renderTransparentAt` at t; AddPuppet
  undo; **animateHero** makes the hero living at 0.65 Loop, one composite
  undo restores the static figure, existing living layer blocks a second;
  prunePuppetRefs clears dangling refs.
- **Persistence**: living project round-trips; store count, layer ref +
  amplitude (0.35) survive; **loaded render at t=0 is byte-identical to the
  original** (deterministic rig rebuild); puppet-less projects load clean.
- **Export**: 3 s @ 24 fps → exactly 72 frames; two runs byte-identical;
  frame 0 == `render(doc, 0)` exactly.
- **Real photo**: live_frame_A/mask_A (SAM3) rig + living loop verified.

### Regression
- Motion Toys harness: **92/92** (identity, photo regime, retarget 177.2° →
  12.00° cap, IoU 0.9974 etc. still green).
- Paintbrush 2.0 art/harvest/boost harness: **141/141**.
- Plugin build: `scripts/pb-build-plugin.ps1` → **NINJA_EXIT=0**.

## Bugs found & fixed during this stage
1. Declaration order: `PuppetMotionClip` used before its definition →
   moved above `PuppetAsset`/`PuppetMotionScanner`.
2. `scan()` default arg on a nested type → overload without defaults.
3. `puppets()` returns a pointer — call sites `->`.
4. `loadManifest` read the harvest id from the *manifest* header instead of
   the *entry* (rigs rebuilt from a garbage id → assets dropped).
5. `PuppetEffect::deserialize` is static — the project store discarded its
   return value (amplitude lost on reload).
6. Soft SAM mask/alpha eroded the identity render (border ring) → build()
   binarizes mask + texture alpha (Stage-1 asset semantics).
7. Procedural fallback key 0 was sway-at-phase-0 (not rest) → opens/closes
   at rest (t=0 == static figure contract).
8. `fx.regime` on PuppetEffect doesn't exist (regime lives on the asset) —
   removed from the controller.

## Artifacts
- `_REVIEW/harness/pb_living_harness/` (build_run.ps1, pb_living_harness.cpp,
  build/living_synth_t0.png, living_live_t0.png, living_live_strong.png,
  living_doc_t0/t1.png, living_export_frame0.png)
- `_REVIEW/PAINTBRUSH_MOTION_TOYS.md` (Stage 1+2, kept green)

## Known limitations
- UI amplitude knob exists (Motion group), but a real-time canvas preview of
  the loop (playing animation on the canvas item) is not implemented — the
  living motion is visible via the export frames and the Boost hero rule.
- GIF preview / MP4 (paper background) assembly is not implemented; the
  export writes the deterministic PNG sequence (frame 0 = static poster),
  which is the input for both.
- Illustration-regime full dance remains a later stage (`PuppetRegime::Illustration` reserved).
- Work is uncommitted (consistent with the repo state; the report and all
  harness code are in the working tree).