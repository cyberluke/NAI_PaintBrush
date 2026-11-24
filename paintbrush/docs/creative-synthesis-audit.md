# Creative Synthesis — audit and delivery log

Status: **Slice A delivered; Slice B/C partially delivered.** This document is the
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
- Patch rail with ten starting recipes:
  Neon Idol, Voltage Idol, Sticker Forge, Pastel Stage, Nocturne, Prism Muse,
  Ink Creature, Spark Storm, Kaleido, Motion Echo.
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

The same functions produce the live preview at a 4K-class resolution (native up
to a 3840x2160 frame budget) and the final layers at canvas resolution, with
geometry expressed in stage-relative units. Motion frames compose at a <=1 MP
interactive target and settle back to the full still after the gesture, so the
Play Pad stays responsive without giving up preview detail.

### 2.2a N-dimensional vector modulation (`pb_creative_vector.hpp`)

Every recipe evaluates one graph per build. Pad X/Y, energy, hold and time feed
routes into named destinations: particle count/speed/spread/brightness, arc
count/bend/jitter, glow, bloom, shadow, hue, rims and the overlay amount.

- `VectorNode` is a composite: a leaf of routes, a `Sequential` chain (the
  charge -> discharge dependency) or `Parallel` branches whose deltas are summed
  once, not per child.
- `Curve` is the strategy (linear, smooth, exponential, sine, square);
  `profileFor(patchId)` is the factory/registry.
- Voltage Idol owns a richer profile. Every other mode shares the default
  profile and receives the same vector-driven decorator, coloured from its own
  catalog accent/secondary, so the modulation belongs to the selected look
  instead of one global cyber palette.
- Voltage Idol's bolt count, arc bend, projection offset and spark wind all come
  from the graph, not from a fixed pad transform. The recipe harness proves the
  bolt count moves 4 -> 5 and the projection image changes when the pad moves.
- Preview builds carry a `Vector live` piece instead of the baked field.
  `compose()` evaluates the profile at the current frame and redraws the spark
  field and Voltage bolts at the compose target; commits keep the baked fields.
  The harness measures 30 ms for the live field at 859x1073 (static compose
  ~18 ms), and the preview dumps show the particles/arcs changing with the
  pad's frame. The in-app creative self-test passes with the live field: all
  19 previews at 1440x1800 and the fashion fixture at 47% protection.

### 2.3 Linked duplication

- **Lineup of 5 (linked style)** duplicates the selected cutout into five poses
  across the stage, hides the source, and writes a `creative.linkGroup` metadata
  key on every copy.
- `applyLayerEffects()` detects the group: Shadow / Glow / Outline / Clear
  finish now land on every linked pose as **one** undo step, while each pose
  keeps its own transform.
- The self-test verifies +5 layers and a one-undo restore.

### 2.4 Assistant awareness

The assistant state now exposes the patch catalog (`creative_patches`) and the
creative tool descriptions include the stage/material vocabulary, so the
assistant can suggest a patch by name without inventing one.

### 2.5 Acceptance harness

`--paintbrush-creative-selftest` (and
`scripts/launch-pb-noskip.ps1 -CreativeSelfTest`) builds a synthetic transparent
idol fixture, then:

1. opens the pad and renders every patch at 1440x1800-class preview resolution;
2. records a two-point gesture and verifies the loop starts;
3. commits Neon Idol (+12 layers), adds the suggestion (+1), undoes twice and
   verifies the exact layer count returns;
4. commits Sticker Forge (+12), Voltage Idol (+14) and Prism Muse (+10), each
   with one-undo verification;
5. builds a linked lineup (+5) and verifies one-undo;
6. writes previews + a text report and leaves the pad open on Neon Idol.

Latest report: `paintbrush/docs/creative-synthesis-assets/selftest-report.txt`
(**RESULT: PASS**). Generated previews:
`selftest-neon-idol.png`, `selftest-sticker-forge.png`,
`selftest-pastel-stage.png`, `selftest-nocturne.png`,
`selftest-ink-creature.png`, `selftest-spark-storm.png`,
`selftest-kaleido.png`, `selftest-motion-echo.png`,
`selftest-electric-kpop.png`, `selftest-holo-fashion.png`,
`selftest-holo-liquid.png`, `selftest-holo-opal.png`,
`selftest-holo-chrome.png`, `selftest-holo-prism.png`,
`selftest-holo-wrap.png`, `selftest-holo-chameleon.png`,
`selftest-holo-glitter.png`, `selftest-holo-livery.png`,
`selftest-fashion-real.png`.

GUI verification:
- Opened the pad from the real workspace, committed with UI Automation
  (Accessible name "Make it real"), saw "Committed", the suggestion card and the
  committed layers in the Inspector; the canvas behind showed the neon
  composition.
- `ui-play-pad.png`, `ui-committed-pad.png` and `ui-committed-canvas.png`
  under `paintbrush/docs/creative-synthesis-assets/` are the captures
  (downscaled for the repo).

### 2.6 Verification tools

- `scripts/pb-capture-live.ps1` captures an already-running Paintbrush window
  (needed for interactive UI checks; `pb-capture-window.ps1` launches its own).
- `dumpStagePieces(dir)` on the creative engine writes every preview piece and
  its stage rect for art QA.

---

## 3. Coverage against the spec

| Spec area | Status | Notes |
|---|---|---|
| 12.1 live stage, real pixels, one composition | **Done** | Preview is the commit's pieces at full preview resolution, not a mock. |
| 12.1 large stage / pan / layers / Before-After | **Not done** | Pad preview is fitted to the canvas; no pan/zoom/compare yet. |
| 12.2 graphic systems (recolor, rim, halftone, arcs, sheen, silhouettes, typography) | **Partial** | Recolor, rim, halftone, arcs, sheen, bokeh, halftone, ink, butterflies, kaleidoscope exist. No typography or independent silhouette library. |
| 12.3 Neon Idol reference | **Done (first pass)** | Electric blue/pink stage, oversized echo, halftone, bokeh, rim split, scanlines. |
| 12.3 Sticker Forge reference | **Done (first pass)** | Die-cut white/colour bands, backing card, shadow, gloss, confetti, sparkles. |
| 12.3 Pastel Lineup reference | **Partial** | Pastel Stage is a stylization patch; Lineup of 5 is a linked duplication action, not a single "Pastel Lineup" performance recipe. |
| 12.3 Nocturne reference | **Done (first pass)** | Night field, aura, violet/teal rim, butterflies, petals, grain. |
| 12.4 pad mapping / hold / pressure | **Done** | Position, energy, hold; pressure on pen/touch, speed on mouse. |
| 12.5 gesture recording / loop / surprise / reseed | **Done** | Recording duration is not displayed; no undo/delete of a recording yet. |
| 12.6 smart duplication, lineup, linked effects | **Partial** | Lineup + linked finish propagation. Generic duplicate layers are not linked. |
| 12.7 Scene Forge, Ink Creature family, Motion Calligraphy | **Partial** | Ink Creature and Motion Echo exist; Scene Forge / true contour calligraphy are not built. |
| 12.9 performance must become real layers, one undo | **Done** | Verified. |
| 12.10 save / reopen / continue editing the performance | **Partial** | Every committed piece is a normal layer with `creative.patch`, `creative.role`, `creative.seed`, `creative.params` metadata, so the layers save and reopen through the existing project store. Restoring the pad to the exact previous performance state from that metadata is not implemented. |
| 12.11 UX at rest / live / after creation | **Partial** | Pad is alive at rest (idle shimmer), live under the puck, and has a post-commit suggestion. No performance history, Before/After compare, or large-stage pan. |
| Slice A end-to-end | **Done** | Keep → chooser → Neon/Sticker → pad → record → Make it real → suggestion → undo. Verified with the synthetic fixture and a real GUI commit. |
| Slice B (Pastel Stage, Lineup, generic smart duplicate) | **Partial** | Pastel Stage + lineup shipped; generic smart duplication pending. |
| Slice C (Nocturne, floral/butterfly/scene layer family) | **Partial** | Nocturne shipped; broader family pending. |
| Slice D (Scene Forge / Motion Calligraphy / ink family) | **Partial** | Ink Creature + Motion Echo only. |
| Slice E (generative patch language, authoring, savable looks) | **Not done** | Patch catalog is compiled in; no authoring/catalog UI. |
| Slice F (beat/audio energy) | **Not done** | — |
| Slice G (subject awareness / multi-subject) | **Partial** | Subject picking understands layers/roles; no automatic face/garment graph. |
| Slice H (Photo Regime H / dark stage / Auto Splatter / Stage mode) | **Not done** | The existing puppet photo regime remains the precedent; not wired into the pad. |
| Slice I (compact workflow) | **Partial** | Keep → pad → commit is compact. No persistent "current look" state machine. |

---

## 4. Honest gaps

These are the things a reviewer should not assume are finished:

1. **Not every spec patch exists.** The collection is ten real recipes, not the
   full eventual library. "Pastel Lineup", "Scene Forge", "Auto Splatter",
   "Motion Calligraphy" (true contour-following strokes), "Stage mode" and the
   full Nocturne layer family are not complete.
2. **Performance state is not restorable after reopen.** Committed layers keep
   the patch/seed/params metadata, but there is no "continue this performance"
   action that rebuilds the pad state from that metadata.
3. **Linked effects only apply inside a lineup group.** Ordinary duplicates and
   independently created copies do not share a style.
4. **The suggestion is deterministic, not model-generated.** It is a real
   editable vector sparkle, but the text/action is authored per patch; no LLM
   tool call decides the next embellishment yet.
5. **Performance is measured at the preview boundary, not a full trace.** The
   recipe harness measures a 2048x2560 preview compose at ~35 ms and the
   859x1073 interactive compose at ~20 ms on this workstation. Motion frames use
   the interactive target at the 20 Hz pad cadence; the full 4K still is built
   on the preview worker after the gesture and published by revision. The
   opt-in `-FourK` harness renders a real 3840x2160 frame: Neon Idol built in
   ~0.60 s and composed in ~0.12 s; holo-liquid built in ~6.65 s and composed in
   ~0.09 s on this workstation. No p95 frame-time trace or 4K GPU capture is
   claimed yet.
6. **No save/reopen/export end-to-end test.** The layers are ordinary persisted
   layers, and the existing export path sees them, but this document does not
   claim a tested round trip for this specific flow.
7. **The art previews use a synthetic fixture.** The recipes are real, but the
   editorial impact on a real idol cutout with hair, fabric folds and a
   transparent background still needs human art review.
8. **No typography, sticker library, or independent silhouette assets.**
9. **The UI copy for the two material looks is "Voltage Idol" and "Prism Muse"**
   (internal patch ids remain `electric-kpop` / `holo-fashion`); the rest of the
   copy is patch-specific and deliberately non-technical.

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

The report is written to
`paintbrush/docs/creative-synthesis-assets/selftest-report.txt`; the process log
is the `-LogFile` target. The app is left open on the pad so the GUI can be
inspected without replaying the test.

## 7. Next move (recommended)

Slice A is shippable. The highest-value next work is, in order:

1. **Restore the performance** from committed metadata (pad reopen with the same
   seed/params, "Continue editing this look").
2. **Generic linked duplicates** (style group on every duplicate, not only
   lineup) and a small "Current look" state so a look can be applied to another
   cutout.
3. **Pastel Lineup / Scene Forge** as real recipes, then **Auto Splatter /
   Stage mode**.
4. **Measure** pad commit and preview frame time on the 4090 target and record
   the p95 numbers here.
5. Save/reopen/export round-trip test with the committed layers.
