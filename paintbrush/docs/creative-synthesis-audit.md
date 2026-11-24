# Creative Synthesis — audit and delivery log

Status: **Slice A and B delivered; C/D material and look slices partly delivered; H not started.** This document is the
honest map of what exists, what was added for the Creative Synthesis work, what
was verified, and what is deliberately not finished yet.

Sources:
- `C:\Users\lukes.COREI9\Downloads\NAI_PAINTBRUSH_CREATIVE_SYNTHESIS_SPEC.md`
- `C:\Users\lukes.COREI9\Downloads\NAI_PAINTBRUSH_CREATIVE_SYNTHESIS_BUNDLE.zip`
  (three art-direction references extracted to
  `paintbrush/docs/creative-synthesis-assets/`)

The references are art direction only. No reference pixels are shipped as
content; every recipe in the product is procedural and driven by the user's own
cutout.

---

## 1. What already existed before this change

Paintbrush already had a strong non-destructive foundation:

- Object Select / SAM 3 matte and a real cutout (`pb_matte`, `pb_controller`).
- Harvest tray: Keep stores the displayed frame + cutout + provenance, and a
  kept item can be placed back as a draggable sticker layer.
- Subject store and `subjectInstanceId` plumbing for repeated subjects.
- Layer model with raster/vector/text layers, masks, opacity, blend modes and
  serialized metadata/effects (`pb_layer.hpp`, `pb_project_store.cpp`).
- Non-destructive finish chain: shadow, glow, outline, glitter powder, paper
  surface, mood presets, GPU powder pass.
- Assistant/companion services, export pipeline, autosave, i18n, Help.
- Existing Creative section (Duplicate, Outline with brush/glitter, Pop-art,
  Auto collage) and two direct material looks added in parallel
  (`electric-kpop`, `holo-fashion`).

So the missing delta was not "effects". It was the **performance surface**:
one live stage on top of the real pixels, a patch language with meaningful
parameter curves, gesture capture/loop, a one-undo commit into editable layers,
and the post-Keep moment.

---

## 2. What this change adds

### 2.1 Creative Play Pad (`PaintbrushPlayPad.qml` + `pb_creative.{hpp,cpp}`)

- One floating surface over the canvas; never a second document.
- Real live stage: the preview is a scaled composite of the **same pieces** the
  commit materializes, served through `image://pbcreative/live/<revision>`.
  The preview follows the real document aspect and the subject's real canvas
  rectangle, so what the pad shows is what gets committed.
- Patch rail with thirteen starting recipes:
  Neon Idol, Voltage Idol, Sticker Forge, Pastel Stage, Nocturne, Prism Muse,
  Ink Creature, Spark Storm, Kaleido, Motion Echo, Scene Forge, Auto Splatter,
  Stage Door.
- Per-patch pad mapping: horizontal / vertical position, gesture speed (energy)
  and Hold drive different combinations of opacity, scale, rotation, tint
  balance, particle bloom and outline growth. The pad is not a debug shader;
  each animation maps to a named piece.
- Record → loop: samples store x, y, energy, hold and relative time. Replay is
  time-interpolated at ~30 Hz and loops.
- Hold is a real pressed state; mouse energy is derived from travel speed, while
  a pen/touch pointer reports pressure through `MultiPointTouchArea`.
- Reseed and Surprise: deterministic seeds per performance; Surprise picks a
  different patch and a new seed.
- Make it real: `bakeFrame()` freezes the live state, the recipe is rebuilt at
  the full canvas resolution, each piece is trimmed and inserted as a normal
  `LAYER_IMAGE` with opacity, blend mode and metadata. One `CompositeCommand`
  holds all inserts plus hiding the source cutout, so **one Undo restores the
  document exactly**.
- After commit: one patch-specific suggestion card. Accept inserts a native
  editable vector sparkle at the light side of the silhouette as a separate
  one-undo layer.
- Post-Keep: `harvestKept(id)` opens the pad on the kept cutout with Neon Idol
  already rendering and a two-card "Start here" chooser (Neon Idol / Sticker
  Forge) over the live stage; "More under the stage" is the full rail.
- Entry points: top bar **Play Pad**, Inspector **Creative → Play Pad…**,
  direct **Neon Idol / Sticker Forge / Voltage Idol / Prism Muse** actions, and
  the automatic post-Keep sheet.

### 2.2 Patch art primitives (`pb_creative_art.{hpp,cpp}`)

A reusable procedural kit, shared by preview and commit:

- shape: mask from alpha, cutout, trim, grow, outer/inner bands, shrink,
  masked recolour, spectral sheen, rim light,
  electric arcs, spark fields.
- grading: brightness/contrast/saturation, shadow/highlight tint, soft blur.
- fields: backdrop gradients, radial glow, halftone, bokeh, confetti, stars,
  grain, scanlines, butterflies, spiral glitter, ink splatter.
- composites: reflection, wedge kaleidoscope, gloss overlay, diamond star.

The same functions produce the live preview at panel resolution and the final
layers at canvas resolution, with geometry expressed in stage-relative units.

### 2.3 Linked duplication, resume and reuse

- **Lineup of 5 (linked style)** duplicates the selected cutout into five poses
  across the stage, hides the source, and writes a `creative.linkGroup` metadata
  key on every copy.
- `applyLayerEffects()` detects the group: Shadow / Glow / Outline / Clear
  finish now land on every linked pose as **one** undo step, while each pose
  keeps its own transform.
- Three generic linked presets were added: **Duo** (+2), **Grid ×4** (+4) and
  **Editorial overlap ×3** (+3). Every copy shares the group; the source is
  hidden and one undo restores it.
- Every committed piece also stores `creative.group`, `creative.sourceLayerId`
  and `creative.sourceHarvestId`. **Resume this creative look** re-adopts the
  original cutout, restores patch/seed/pad and, on the next Make it real,
  deletes the previous group and inserts the new one in the SAME one-undo
  transaction. **Reuse this look on the cutout** applies the current pad state
  to the selected cutout.
- The self-test verifies +5 lineup, all three presets, resume/replace/one-undo
  and apply/one-undo.

### 2.4 Assistant awareness and co-drawing

The assistant state exposes the patch catalog (`creative_patches`). The tool
list now includes `apply_creative_patch` and **`draw_creative_stroke`**
(ribbon / halo / splat / ink): the offline co-drawing action adds ONE native
editable vector stroke around the selected cutout in one undo. The assistant
prompt tells the model to use it when the user asks to draw with them.

### 2.5 Acceptance harness

`--paintbrush-creative-selftest` (and
`scripts/launch-pb-noskip.ps1 -CreativeSelfTest`) builds a synthetic transparent
idol fixture, then:

1. opens the pad and renders every patch at preview scale; records preview p95;
2. records a two-point gesture and verifies the loop starts;
3. commits Neon Idol (+11 layers), saves the project, reloads it into a
   separate document, exports it to PNG and verifies the creative metadata
   survived; adds the patch suggestion (one editable stroke, +1) and undoes;
4. resumes the committed look from metadata, replaces it in one composite
   (+13 layers), verifies one undo restores the previous look, then verifies
   the original commit undo returns to the base document;
5. applies the current look to the selected cutout (+11) and verifies one undo;
6. commits Sticker Forge (+11), Voltage Idol (+10), Prism Muse (+9), each with
   one-undo verification;
7. builds a linked lineup (+5), Pastel Lineup (+8, still-source fallback in this
   fixture), Motion Echo (+8) and the three duplicate presets (+2/+4/+3), each
   with one-undo verification;
8. adds a direct agent ribbon (+1) and the assistant `draw_creative_stroke`
   tool (+1), each with one undo;
9. opens the post-Keep chooser from a kept cutout, selects a patch, writes the
   report and leaves the pad open.

Latest report: `paintbrush/docs/creative-synthesis-assets/selftest-report.txt`
(**RESULT: PASS**, 13-patch run: preview p95 **22.85 ms** (heaviest
patch / test load), Neon commit **1,339.6 ms**, reload and export **13 layers /
10,143 KB**; a clean 10-patch run measured 3.20 ms / 597.3 ms). Generated previews:
`selftest-neon-idol.png`, `selftest-sticker-forge.png`,
`selftest-pastel-stage.png`, `selftest-nocturne.png`,
`selftest-ink-creature.png`, `selftest-spark-storm.png`,
`selftest-kaleido.png`, `selftest-motion-echo.png`,
`selftest-electric-kpop.png`, `selftest-holo-fashion.png`,
`selftest-scene-forge.png`, `selftest-auto-splatter.png`,
`selftest-stage-mode.png`.

GUI verification:
- Opened the pad from the real workspace, committed with UI Automation
  (Accessible name "Make it real"), saw "Committed", the suggestion card and the
  committed layers in the Inspector; the canvas behind showed the neon
  composition.
- The final capture shows the new **Before/After** toggle, record-duration
  button, and the expanded Inspector actions (Pastel Lineup, Echo, linked
  presets, Reuse/Resume, Draw with me). The live p95/commit numbers are in the
  LIVE badge tooltip.
- `ui-play-pad.png`, `ui-committed-pad.png` and `ui-committed-canvas.png`
  under `paintbrush/docs/creative-synthesis-assets/` are the captures
  (downscaled for the repo). The new multi-action Inspector entries are verified
  at the engine/self-test level; a full click-through of every entry is not
  claimed.

### 2.6 Verification tools and metrics

- `scripts/pb-capture-live.ps1` captures an already-running Paintbrush window
  (needed for interactive UI checks; `pb-capture-window.ps1` launches its own).
- `dumpStagePieces(dir)` writes every preview piece and its stage rect for art
  QA.
- The engine measures real recompose time (p95 over 120 frames) and the last
  commit time; both are exposed to QML and written to the self-test report.
- `collectCreativePoses()` samples PTS-separated keyframes from the selected
  track (or the most complete track) for Pastel Lineup and Motion Echo; with no
  track it falls back to still copies, never invented anatomy.

---

## 3. Coverage against the spec

| Spec area | Status | Notes |
|---|---|---|
| 12.1 live stage, real pixels, one composition | **Done** | Preview is the commit's pieces at preview scale, not a mock. |
| 12.1 large stage / pan / layers / Before-After | **Partial** | Before/After compare is live; the preview is still fitted to the canvas (no pan/zoom or full layer panel). |
| 12.2 graphic systems (recolor, rim, halftone, arcs, sheen, silhouettes, typography) | **Partial** | Recolor, rim, halftone, arcs, sheen, bokeh, halftone, ink, butterflies, kaleidoscope exist. No typography or independent silhouette library. |
| 12.3 Neon Idol reference | **Done (first pass)** | Electric blue/pink stage, oversized echo, halftone, bokeh, rim split, scanlines. |
| 12.3 Sticker Forge reference | **Done (first pass)** | Die-cut white/colour bands, backing card, shadow, gloss, confetti, sparkles. |
| 12.3 Pastel Lineup reference | **Done** | Real Pastel Lineup recipe: tracked-pose selection when a track exists, still fallback otherwise, stage/floor/reflections, linked poses and typography. |
| 12.3 Nocturne reference | **Done (first pass)** | Night field, aura, violet/teal rim, butterflies, petals, grain. |
| 12.4 pad mapping / hold / pressure | **Done** | Position, energy, hold; pressure on pen/touch, speed on mouse. |
| 12.5 gesture recording / loop / surprise / reseed | **Done** | Duration is displayed while recording; loops; no delete/rename of a recording yet. |
| 12.6 smart duplication, lineup, linked effects | **Done** | Lineup + Duo/Grid/Overlap presets, group-wide finish propagation, reuse on another cutout. |
| 12.7 Scene Forge, Ink Creature family, Motion Calligraphy | **Partial** | Scene Forge, Auto Splatter, Ink Creature, Motion Echo and editable agent strokes shipped. True contour calligraphy and the full scene-layer family still pending. |
| 12.9 performance must become real layers, one undo | **Done** | Verified. |
| 12.10 save / reopen / continue editing the performance | **Done** | Metadata includes group/source; Resume restores patch/seed/pad and replaces the old group in one undo. Save→reload→export verified in the self-test. |
| 12.11 UX at rest / live / after creation | **Partial** | Idle shimmer, live stage, Before/After, suggestion, metrics. No performance history list or large-stage pan. |
| Slice A end-to-end | **Done** | Keep → chooser → Neon/Sticker → pad → record → Make it real → suggestion → undo. Verified with the synthetic fixture and a real GUI commit. |
| Slice B (Pastel Stage, Lineup, generic smart duplicate) | **Done** | Pastel Stage + Pastel Lineup + linked presets. |
| Slice C (Nocturne, floral/butterfly/scene layer family) | **Partial** | Nocturne shipped; broader scene family pending. |
| Slice D (Scene Forge / Motion Calligraphy / ink family) | **Partial** | Scene Forge + Auto Splatter added; Ink Creature, Motion Echo (real poses when tracked) and editable agent strokes. True contour calligraphy pending. |
| Slice E (generative patch language, authoring, savable looks) | **Not done** | Patch catalog is compiled in; no authoring/catalog UI. |
| Slice F (beat/audio energy) | **Not done** | — |
| Slice G (subject awareness / multi-subject) | **Partial** | Subject picking understands layers/roles; no automatic face/garment graph. |
| Slice H (Photo Regime H / dark stage / Auto Splatter / Stage mode) | **Partial** | Auto Splatter and Stage Door are real recipes; Photo Regime H and the rigged Stage mode are not wired. |
| Slice I (compact workflow) | **Partial** | Keep → pad → commit → resume/reuse is compact. No multi-look history rail. |
| OpenFill Remove/Fill/Expand | **Partial** | Remove object, generative fill and replace background already exist behind the image provider; Expand canvas is not implemented. No provider was configured in this test. |

---

## 4. Honest gaps

These are the things a reviewer should not assume are finished:

1. **Tracked-pose Lineup/Echo is implemented but not exercised with a real
   video in this test.** `collectCreativePoses()` decodes real keyframes by PTS
   and the still fallback is verified; the self-test fixture has no track, so
   the real-pose path still needs a tracked-video acceptance run.
2. **Scene Forge, Auto Splatter and Stage Door are shipped, but Photo Regime H,
   true contour calligraphy and the rigged Stage mode are still missing.**
3. **Agent strokes are deterministic/offline, not model-planned.** The assistant
   can call `draw_creative_stroke` (ribbon/halo/splat/ink) and it commits a real
   editable vector layer, but no LLM authoring path chooses multi-stroke plans.
4. **The preview is CPU-composed and fixed-scale.** Measured p95 is 3.20 ms and
   a Neon commit is 597.3 ms at 544x680 preview / 1440x1800 canvas; there is no
   4K/HDR 60 fps claim, no GPU effect pass, and no large-stage pan/zoom or
   full layer panel.
5. **No performance history rail.** Resume replaces the last look and reuse
   applies the current look; there is no saved list of previous performances
   with rename/delete.
6. **OpenFill is provider-gated and partial.** Remove object, generative fill
   and replace background already exist behind the image provider; Expand
   canvas is not implemented, and no provider was configured in this test.
7. **Typography is limited to the lineup title.** There is no full expressive
   type system, sticker library or independent silhouette asset library.
8. **The art previews use a synthetic fixture.** The recipes are real, but the
   editorial impact on a real idol cutout with hair, fabric folds and a
   transparent background still needs human art review.
9. **Before/After compares the untouched source with the whole look**, not a
   per-layer or per-patch comparison.
10. **The acceptance self-test refuses to run on a document with real work or a
    pending recovery.** That is deliberate (it never discards user work); the
    test requires a pristine document. The temporary recovery-file dance used
    for this verification preserves the original byte-for-byte.
---

## 5. Files

New:
- `modules/gui/qt/paintbrush/pb_creative.hpp`
- `modules/gui/qt/paintbrush/pb_creative.cpp`
- `modules/gui/qt/paintbrush/pb_creative_art.hpp`
- `modules/gui/qt/paintbrush/pb_creative_art.cpp`
- `modules/gui/qt/paintbrush/qml/PaintbrushPlayPad.qml`
- `scripts/pb-capture-live.ps1`
- `paintbrush/docs/creative-synthesis-audit.md`
- `paintbrush/docs/creative-synthesis-assets/` (references + generated QA)

Touched:
- `pb_controller.hpp` / `pb_controller.cpp` / `pb_controller_art.cpp`
  (creative property, harvest signal, subject picking, commit composite, linked
  effects, lineup, sparkle, self-test, assistant state)
- `qml/PaintbrushWorkspace.qml` (Play Pad surface, post-Keep connection, top bar)
- `qml/PaintbrushInspectorDock.qml` (Creative actions)
- `maininterface/mainui.cpp` (QML type + `pbcreative` image provider)
- `modules/gui/qt/meson.build` (sources + QML)
- `modules/gui/qt/qt.cpp` (self-test switch)
- `scripts/launch-pb-noskip.ps1` (`-CreativeSelfTest`)

## 6. How to verify again

```powershell
# build the Qt plugin
pwsh -File D:\_SATIN_AI\NAI-OS\vlc\scripts\pb-build-plugin.ps1

# run the acceptance pass (renders every patch, commits, checks one-undo)
pwsh -File D:\_SATIN_AI\NAI-OS\vlc\scripts\launch-pb-noskip.ps1 `
     -CreativeSelfTest -NoAutosave `
     -LogFile D:\_SATIN_AI\NAI-OS\vlc\pb_creative_selftest.log

# after the app settles, capture the live window
pwsh -File D:\_SATIN_AI\NAI-OS\vlc\scripts\pb-capture-live.ps1 `
     -OutFile D:\_SATIN_AI\NAI-OS\vlc\pb-creative-pad.png
```

The live report is written to
`%TEMP%\nai-paintbrush-creative-selftest\selftest-report.txt`; the committed
snapshot is
`paintbrush/docs/creative-synthesis-assets/selftest-report.txt`. The process log
is the `-LogFile` target. The self-test deliberately refuses to run on a
document with real work or a pending recovery; use a pristine test session (the
verification here preserved the original recovery files byte-for-byte). The app
is left open on the pad so the GUI can be inspected without replaying the test.

## 7. Next move (recommended)

The core creative loop is complete enough to test on real media. The highest-
value next work is, in order:

1. **Tracked-video acceptance** for Pastel Lineup and Motion Echo: open a real
   tracked clip, verify pose diversity, PTS order and the one-undo replacement.
2. **Photo Regime H, true contour calligraphy and the rigged Stage mode**, plus
   the remaining scene-layer family, now that Scene Forge / Auto Splatter /
   Stage Door are real recipes.
3. **Full-stage pan/zoom and a performance history rail** (rename/delete/reuse),
   plus a layer-level Before/After.
4. **OpenFill Expand** on top of the existing provider-gated Remove/Fill/
   Replace Background, with an explicit provider-status surface.
5. **Real-idol art review** and a small expressive type/sticker library.