# NAI Paintbrush 2.0 — Product / Art Materials / Harvest / Boost: stage report

Spec: `NAI_Paintbrush_2_Product_Art_Materials_Harvest_Boost_Spec.md` (§70 report
format). Base: `master` @ `4160e275a3` (the 2.0 canonical document/render core,
typed undo, media split and honest AI were already landed; this stage builds the
product reality on top of them).

## STATUS

Implemented end to end, built into `libqt_plugin.dll`, and verified by five
harnesses (339 checks, all green) plus a live launch — including the Showroom
GPU Art Runtime first port (QRhi), editable typography UI, 4K export, and a
LIVE acceptance run on a real K-pop video with real SAM3 segmentation.
Remaining limitations are listed honestly at the end; none of them is hidden
behind a passing check.

## Stage completed

P0 verify → P1 Harvest → P2 Compose → P3 Paper + bristle brush + shared scale/light
→ P4 Glitter → P5 Finish runtime → P6 Typography + badges → P7 Teen Boost v1 →
P8 Moods → P9 build/harness hardening. One continuous build, one coherent product.

## Files changed / added

**Art Materials Runtime** (`modules/gui/qt/paintbrush/art/`, new)
- `runtime/art_scale.hpp` — `ArtScale`: world units ↔ device px (1x/2x/4x export).
- `runtime/light_rig.hpp` — `ArtLightRig`: the ONE document light every material/finisher samples.
- `runtime/art_noise.hpp` — seedable hashing / value noise / fbm (no global randomness).
- `runtime/material_registry.{hpp,cpp}` — uniform material manifest + registry (paper, bristle, glitter, 5 finishers).
- `surface/paper_stock.{hpp,cpp}` — `PaperStock` + exactly six stocks (greige, K-pop pink, bubblegum Y2K, emo ash, editorial cream, matte black).
- `surface/paper_renderer.{hpp,cpp}` — world-space procedural paper (zoom/export stable, no tiling, deterministic).
- `stroke/stroke_sampler.hpp` — stabilizer, velocity EMA, spacing scheduler.
- `stroke/bristle_brush.{hpp,cpp}` — stable per-stroke bristle set, filament deposition, per-bristle pigment, pressure.
- `materials/glitter/glitter_material.{hpp,cpp}` — flagship: glue/adhesion, bonded vs loose grains, bounded grid, transport/displacement, bounded palette identity, facet lighting + twinkle from the shared rig, contact shadows, paper tooth/absorption interaction, byte-exact serialize/deserialize.
- `finish/effects.{hpp,cpp}` — non-destructive `EffectNode` chain (Shadow/Glow/MotionBlur/Outline/Ghost), all premultiplied, world units, rig-driven shadow direction, `LayerRole` vocabulary + per-role defaults.
- `style/mood.{hpp,cpp}` — six Moods (paper + light + palette roles + type role + badge family + finish/glitter defaults).
- `style/typography_style.{hpp,cpp}` — seven curated type roles with open-font preference + graceful fallback; text rendered at export scale.
- `style/badges.{hpp,cpp}` — eleven procedural recolorable badges, seeded organic variation, mood families.

**Document / product core** (modified)
- `pb_layer.hpp` — layer gains `effects`, `role`, `materialPreset`, `materialState`, `harvestId`.
- `pb_document.{hpp,cpp}` — paper stock, light rig, mood, art seed, document-scoped `HarvestTrayModel`; `DocumentRenderer` paints paper, resolves effect chains with exact placement, re-renders material state and finishers at export scale (`renderScaled`); **fixed a pre-existing bug** in the exact (translation-only) composite path that mixed local/canvas rects (any translated sticker rendered nothing in tiles). Layer ids are now stable across reopen.
- `pb_history.{hpp,cpp}` — typed `SetEffectsCommand`, `SetRoleCommand`, `SetArtStateCommand`, `AddHarvestCommand`.
- `pb_project_store.cpp` — persists effects/role/material state/harvest provenance per layer, document art state, and the harvest tray (manifest + assets). Older projects load with defaults.
- `pb_export.cpp` — export at scale ≠ 1 goes through `renderScaled` (world-unit re-evaluation, never a bitmap upscale).
- `pb_common.hpp` — `TOOL_BRISTLE`, `TOOL_GLITTER`.
- `pb_canvas_item.{hpp,cpp}` — both art tools ride the existing scratch-tile + `StrokeCommand` commit; glitter previews deterministically and keeps its grain grid; QML properties for the primary controls.
- `pb_harvest.{hpp,cpp}` (new) — `HarvestItem` (exact `FrameRef`/PTS, trimmed cutout, mask, centroid, content hash, palette metadata) + `HarvestTrayModel`.
- `pb_boost.{hpp,cpp}` (new) — `BoostEngine` (same-source temporal sequence, motion vector, hero, spacing evenness, sparsest gap), deterministic plan (roles → restrained finish → depth → bounded spacing), real-frame bridge candidates (midpoint/distinctness/sharpness scoring, A→B centroid seed), `BoostTransaction` (= one `CompositeCommand`, rollback on failure).
- `pb_controller.{hpp,cpp}` + `pb_controller_art.cpp` (new) — `keepSelection`, `placeHarvest`, `boost`/`cancelBoost` (decodes verified PTS frames, segments the best candidate with the same SAM path as Keep, rejects fragmentary masks), moods/paper, layer role/finish, styled text, badges, previews.

**UI** (`qml/`)
- `PaintbrushHarvestTray.qml` (new) — thumbnails + timestamps only; drag/click to place; favorite/jump/remove; collapses when empty.
- `PaintbrushBoostBar.qml` (new) — Keep / Boost / Undo Boost / Cancel + one fading summary sentence.
- `PaintbrushMoodPicker.qml` (new) — six moods, six papers (live previews), styled text entry, badges.
- `PaintbrushWorkspace.qml`, `PaintbrushToolRail.qml`, `PaintbrushPaletteBar.qml` — wiring, two new tools, size/flow/pigment/amount/flake/shimmer controls, shortcuts (P, L, K, Ctrl+B, M).

**Build / tooling**
- `modules/gui/qt/meson.build` — authoritative source + QML lists.
- `scripts/pb-ninja-add-sources.py` — patches the existing `win64-meson/build.ninja` (meson regen is broken on this tree, see HANDOFF); also added the three pre-existing meson entries (`pb_capability/pb_sticker/pb_layer_model`) that were missing from the stale graph and broke the link.
- `scripts/pb-build-plugin.ps1`, `scripts/pb-syntax-check.ps1`.
- `_REVIEW/harness/pb_art_harness/` — acceptance harness + shared stub + golden images; `build_run_s1.ps1` re-runs the preserved S1 core harness against current sources. `s5_capability_harness/build_run.ps1` repaired (path + stub drift).

## Runtime behavior verified (not "compiles")

`_REVIEW/harness/pb_art_harness/build_run.ps1` — **141/141**
- Scale: 10 world units = 40 px at 4x and back.
- Paper: deterministic; 2x export samples the identical world heightmap; visible grain; not a 64px tile; seed changes grain; all six stocks near base lightness.
- Glitter: >2000 fine grains per stroke; bonded + loose coexist in the adhesion ratio; render deterministic; serialized state renders byte-identical; three distinct grain colors survive (no averaged soup); no alpha contamination; coverage stable at 2x; responds to light direction; pushing never moves bonded grains; wet glue bonds more than dry; ~2% of flakes twinkle white.
- Finish: directed blur has no dark halo and trails opposite to motion; shadow pads/offsets away from the rig light; glow/outline never touch subject pixels; ghost opacity/desaturation exact; deterministic; variant round-trip; padding scales with export; Transition echo stays legible.
- Brush: deterministic bristle identity and render; no gaps at fast pointer rate; pigment variation within hue; pressure widens; stabilizer/velocity/spacing.
- Style: six moods reference real paper + type roles; seven type roles render glyph alpha at export scale; eleven deterministic badges; seeded tape tears.
- Harvest + compose + persistence: Keep from frame+mask (trimmed, exact PTS, alpha = mask, hash, centroid, `00:04.20`); fresh document is paper; tray never clutters canvas; same-source sequence by PTS; translated sticker renders at its position (exact path regression guard) and tile == full render; effects change the render but never the subject pixels or placement; rotated+scaled+effects render; **2x export vs 1x screen mean abs diff 0.62/255**; project round-trip of paper/mood/light/tray/PTS/favorite/cutout/effects/role/provenance/material bytes; reopened project renders byte-identical.
- **4K export (new)**: `renderScaled(4)` → 3840×2160; text re-renders crisp from its treatment (1.03% vs 7.12% intermediate-alpha pixels); glitter re-renders from world state at 4x; finish padding 23 → 89; 4K-vs-1x diff 4.33/255; deterministic; ~1.2 s.
- Boost: 3-pose sequence found and ordered; rightward motion inferred; hero = largest/central; sparsest gap selected; roles hero/ghost/support; hero brought forward; ghost before hero carries directed blur; no reflexive glow; bridge PTS inside the interval; midpoint/distinct/sharp frame beats a blurry near-duplicate; seed interpolates centroids; bridge layer Transition, behind both, centered between, provenance recorded; one concise summary; **one top-level undo** restores order, tray, roles, effects and renders byte-identical; redo reproduces exactly.

Regression: S1 core harness **81/81**, S5 capability harness **67/67**.

GPU Art Runtime harness (`_REVIEW/harness/pb_gpu_harness/build_run.ps1`) — **19/19** on D3D11
- Paper: all six stocks at 1x + 2x, mean|diff| 0.370–1.027 < 2 (near-bit-exact integer hashing).
- Glitter: dense deterministic pile (4836 grains, 4634 opaque px), mean|diff| 3.683 < 12, size matches.
- Finish: directed blur 0.007, shadow 0.059, glow 5.598 (all < 16) — the CPU golden is reproduced to rounding noise.

LIVE acceptance harness (`_REVIEW/harness/pb_live_harness/build_run.ps1`) — **31/31**
- Real video: libvlc 4.x (µs-time vmem callbacks) decodes the 553 MB NewJeans "Hype Boy Dance Practice" clip at 960×540.
- Real SAM3 (`sam3-q4_0.ggml`, CPU, PVS center-point + PCS "person" fallback) segments a dancer in every Keep frame and the bridge candidate.
- Keep A/B/C → HarvestTray with exact PTS → canvas poses → Boost analysis finds the sequence and the sparsest gap (64.3 s → 96.5 s).
- Real mid-gap frame scored best, segmented, inserted as a Transition bridge pose behind both neighbours.
- 4K export 4800×2400 deterministic; one Undo Boost restores the raw composition byte-identical; Boost visibly changes the render (mean diff 28.42/255).

Build: `ninja modules/libqt_plugin.dll` → `NINJA_EXIT=0`; `vlc-cache-gen` regenerated; `vlc.exe --paintbrush` launches, Qt interface up, no QML errors on stderr; `qmllint` clean on all six QML files.

## Golden scenes (`_REVIEW/harness/pb_art_harness/golden/`)

`paper_*.png` (six stocks, 1x/2x greige), `glitter_1x.png`, `glitter_2x.png`,
`glitter_on_black_4x.png`, `brush_stroke.png`, `brush_stroke_varied.png`,
`finish_motion_blur.png`, `finish_shadow.png`, `finish_glow_outline.png`,
`type_condensed.png`, `badge_heart.png`, `compose_raw.png`,
`compose_shadow_rotated.png`, `compose_export_2x.png`, `boost_before.png`,
`boost_after.png`. Reviewed visually during the build (glitter tuned from
"colored dots" to a dense fine pile with twinkle; brush from a flat ribbon to
visible strands; boost ghosts from a smear wall to legible echoes).

## Remaining limitations (honest)

- **GPU path**: paper, glitter and the finish chain are CPU, cached per render
  (paper has a full-canvas cache; glitter previews per segment). Interactive at
  1280×720; a QSG/shader path for paper + glitter is the natural next step for
  4K canvases.

  **UPDATE (Showroom)**: the QRhi GPU Art Runtime first port is now GREEN —
  `_REVIEW/harness/pb_gpu_harness/build_run.ps1` **19/19** on D3D11:
  paper 12/12 near-bit-exact (< 2), glitter 3/3 on a dense 4836-grain pile
  (3.68 < 12), finish 3/3 (directed blur 0.007, shadow 0.059, glow 5.598 < 16).
  Bugs fixed along the way: (a) `readBackTexture` returns the ENTIRE grow-only
  target — the image must be built from `result.pixelSize`, not the requested
  size; (b) the D3D11 backend flips viewport Y, so the crop is BOTTOM-anchored
  when the target is taller than the viewport; (c) shadow/glow are a 3× box
  blur (B-spline kernel, exact integer counts) with the SUBJECT composited
  back over the effect — the first port used a 7×7 gaussian and dropped the
  subject; (d) `textureLod` (explicit LOD) instead of gradient `texture()` in
  dynamic loops — HLSL force-unrolls the loop otherwise (X3570/X3511); (e)
  glitter coverage had a +0.5px half-pixel double-count. Shaders live in
  `modules/gui/qt/paintbrush/art/gpu/shaders/`, baked to `shaders_qsb.h` by
  `scripts/pb-bake-shaders.py`; `pb_gpu_runtime.cpp` is registered in
  `meson.build` + `pb-ninja-add-sources.py`; the plugin still builds
  (`NINJA_EXIT=0`). The GPU runtime stays a preview accelerator — the CPU
  golden remains authoritative for export and acceptance.

- **4K export** is now verified end-to-end (new `[4K export]` section in the
  art harness, **141/141**): `renderScaled(4)` produces 3840×2160; editable
  text re-renders from its treatment (1.03% intermediate-alpha pixels vs
  7.12% for a bitmap upscale — vector-style crisp); glitter re-renders from
  world state at 4x; finish padding scales (pad 23 → 89); 4K vs 1x mean abs
  diff 4.33/255; deterministic byte-identical re-render; ~1.2 s for a full
  4K export of a text+glitter+shadow document.
- **Bridge pose in the live app** needs SAM configured and a decodable source;
  the deterministic structure pass runs regardless and the summary says so when
  no clean bridge was found. The harness proves the scoring/insertion/undo path
  with synthetic frames; a real-video end-to-end run is still to be captured
  (SAM3 model + libvlc + a real K-pop clip are staged for the live acceptance
  run).

  **UPDATE (Showroom)**: the LIVE acceptance run is DONE and green —
  `_REVIEW/harness/pb_live_harness/build_run.ps1` **31/31** on the real
  pipeline: libvlc 4.x (µs-time vmem callbacks) decodes the 553 MB "NewJeans
  Hype Boy Dance Practice" clip, real SAM3 (`sam3-q4_0.ggml`, CPU, PVS
  center-point + PCS text fallback) segments a dancer in all three Keep
  frames, Keep A/B/C → HarvestTray with exact PTS, poses placed on the
  canvas, Boost analysis finds the same-source sequence + sparsest gap
  (64.3 s → 96.5 s), the deterministic plan assigns hero/ghost/support, a
  REAL mid-gap frame is decoded and scored best for the bridge, SAM3
  segments it, `insertBridgePose` inserts a Transition layer behind both
  neighbours, the summary names the real bridge PTS, 4K export renders
  (4800×2400, deterministic), and one Undo Boost restores the raw
  composition byte-identical. Boost visibly changes the render (mean diff
  28.42/255). Artifacts (frames, masks, bridge frame, before/after boost,
  undo, 4K export) land in `_REVIEW/harness/pb_live_harness/build/live/`.
  Note: non-ASCII source paths are staged to an ASCII copy because native
  argv is ANSI-lossy; libvlc time units here are microseconds.
- **Text stays editable only via metadata** (`textTreatment` on the layer);
  there is no in-place text re-edit UI yet.

  **UPDATE (Showroom)**: editable typography UI is DONE — double-click a text
  layer on the canvas (`pb_canvas_item` glyph-alpha hit-test →
  `textLayerDoubleClicked`), and an inline editor bar appears in the workspace
  (`PaintbrushTextEditorBar.qml`: word field, typography role combo, size and
  tracking sliders that commit on release, palette swatches, close). Every
  edit is one typed `SetTextCommand` with full undo/redo (`pb_history`), and
  `pb_document::effectiveSource` re-renders text layers from their stored
  treatment at export scale. Wired through `pb_controller::editTextLayer`.
- **Metallic/foil materials, 3D sticker depth, animation** are out of this
  stage (spec §30, §62).
- `meson setup --reconfigure` remains broken on this tree (pre-existing); the
  ninja graph is patched by `scripts/pb-ninja-add-sources.py` with
  `meson.build` kept authoritative.
