# Paintbrush 2.0 — Living Showroom

**Status: FROZEN as a product checkpoint** — no further engine features.
Remaining work is perceptual/product polish only.
(living harness 84/84 incl. REAL muxed MP4 + WebM alpha; art 141/141;
motion 92/92; plugin build `NINJA_EXIT=0`)
**Date:** 2026-10-24 — **Chapter:** closes Motion Toys as R&D; product name
for the artifact: **Living Poster**

## What this chapter adds

The Living Harvest (Stage 3) shipped the capability; the Showroom makes it
*visible* and *sharable*:

1. **Live canvas preview** — the living figure moves directly on the canvas,
   driven by one canonical clock, through the same compositor as everything
   else.
2. **Real video export** — MP4/H.264 (share-ready) and WebM/VP9 with
   transparency, muxed over the deterministic PNG sequence (which stays as
   the canonical intermediate and test oracle).
3. **Product naming** — the artifact is a **Living Poster** ("Nevygenerovali
   jsme video. Oživili jsme skutečný moment.").

## 1. Live canvas preview — AnimationClock

```
AnimationClock (QTimer, PreciseTimer)
   30 Hz  (10 Hz during strokes/interactions)
     │ gate: showroom enabled && canvas isVisible()
     │       && document.hasVisibleLivingLayer()
     ▼
 m_animTimeMs += interval
     ▼
 PaintbrushCanvasItem::setAnimTime(t)
     ▼
 markAllTilesDirty() → DocumentRenderer::composite(tile, region, doc, t)
     ▼
 tile upload (only dirty tiles) → scene graph
```

- **Single canonical axis**: the canvas item carries `m_animTimeMs` and the
  tile compositor calls `DocumentRenderer::composite(doc, t)` — the same
  time-aware renderer the export uses. There is no separate "animation
  mode"; time is a parameter of the one compositor.
- **Clock gate** (`PaintbrushController::refreshLivingClock()`): the timer
  runs only while (a) a visible layer carries a living asset
  (`PaintbrushDocument::hasVisibleLivingLayer()`), (b) the canvas is on
  screen (`QQuickItem::isVisible()` → minimized/hidden workspace stops it),
  and (c) preview is enabled. When it stops, the canvas rests at t=0 —
  byte-identical to the static poster.
- **Editing gestures**: while `canvas->strokeActive()`, the clock drops to
  10 Hz so the preview never fights the user's input.
- **Static layers stay cached** in their tiles; only the (dirty, whole-
  canvas) living composite re-renders — the tile system uploads only dirty
  tiles.
- Re-evaluated on every `layersChanged` (undo/redo, boost, puppet attach/
  detach, visibility toggle) and per tick (self-stopping gate).

## 2. Real video export (no fake buttons)

`ExportService::exportLivingVideo(doc, dir, stem, durationMs, fps, format, outPath)`

- Writes the canonical PNG sequence first (paper for MP4, transparent for
  WebM), then **muxes a real file** with ffmpeg:
  - `mp4`  → `-c:v libx264 -pix_fmt yuv420p -movflags +faststart`
  - `webm` → `-c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0` (alpha)
- **Probe gate**: `canExportLivingVideo(format)` returns true only when an
  ffmpeg binary is found (PATH → msys2 → contribs) AND the encoder is
  present (`ffmpeg -encoders` probe, cached). The QML export buttons stay
  disabled otherwise — there is no dead button.
- Real error strings for every failure mode (missing ffmpeg, missing
  encoder, mux failure, empty output); the frames remain on disk as the
  canonical intermediate.

**Verified on this machine** (harness, real subprocess): MP4 file starts
with the ISO-BMFF `ftyp` box, WebM starts with the EBML magic
`1A 45 DF A3`; both non-trivial size; the PNG sequence is kept next to the
video.

## 3. Living Poster — product naming

The artifact a user receives is a **Living Poster**: 3 s, 24 fps,
deterministic, loop-seamless, t=0 == the composed poster. UI strings,
export stems (`living_poster_%04d.png`) and this document use the name.

## Demo flow (investor-ready)

```
NewJeans video → Harvest 3 poses → Compose → Luxury Greige → glitter title
→ Boost (bridge pose, SAM3) → hero becomes Living (0.65, source motion)
→ the figure moves on the canvas (AnimationClock)
→ Export Living Poster: MP4 (share) / WebM alpha (compositing) / PNG frames
```

Every step is a shipped, harness-verified path; the only manual part is
launching the GUI (`scripts/launch-pb-noskip.ps1`) and doing the flow.

## Verification (requirement-specific)

New checks in `_REVIEW/harness/pb_living_harness/` (now 84 total):

- **Clock gate**: no living layer → off; visible living layer → on; hidden
  layer → off; re-shown → on (`hasVisibleLivingLayer`).
- **Preview semantics**: `render(doc, t1) != render(doc, t2)` for a living
  layer (the clock's ticks move the canvas); `render(doc, 0)` deterministic;
  static path byte-identical when the clock is off.
- **Real export**: MP4 mux + `ftyp` magic + non-trivial size + frames kept;
  WebM alpha mux + EBML magic (skips gracefully when ffmpeg/encoders are
  absent).

Regression: motion 92/92, art 141/141, plugin `NINJA_EXIT=0`.

## Architecture note

The Showroom adds no second renderer and no document-model change: the
document gained one query (`hasVisibleLivingLayer`), the canvas gained one
scalar (`m_animTimeMs`) and the controller gained one timer. Time is now a
first-class axis of the canonical renderer (`render(doc, t)`), ready for the
next temporal effects (glitter shimmer, badge pulse, animated glow,
paper-light drift, foil sparkle) as EffectNode/material state over the same
`t`.

## Eyes-on audit verdict (2026-10-24) — freeze definitive

Feel-sheet review (`living_synth_feel_sheet.png`, `living_live_feel_sheet.png`):

- **Synthetic Strong is clean** — light torso/arm deformation, no tearing,
  rubber or silhouette breakdown; the photo limiter does not overshoot.
- **Live SAM3 sheet is extremely subtle** — visually almost nothing changes
  between t0 and Strong. Not a problem: the solid-blue mask is not a good
  perceptual proxy for a real textured dancer. It certainly does not imply
  "0.65 is too much" — if anything the opposite.
- **`living_doc_t0` vs `t1` is practically identical by eye and pixel** —
  confirms determinism/static semantics, not motion feel (fixture not suited
  to judging motion).
- **Paper surface reads well** — fine structure, no tiled-noise pattern,
  no fight with content.

Product scale (fixed constants):

```text
Subtle  0.35   barely alive
Normal  0.65   product default
Strong  1.00   clearly alive, still photo-safe
```

Decision rule (recorded in code comments + demo runbook): keep 0.65; the
live GUI check at 100% zoom within ~0.5 s decides — if the hero is clearly
alive without the deformation reading as an effect, 0.65 stands; if you must
stare, go **straight to 0.75** (never micro-tune). Strong stays 1.00 as max.

**No reason to reopen the engine.** The only remaining step is the GUI demo
flow on the textured dancer in the real composition; if `Normal` reads well
there, the freeze is definitive.

## Known limits

- The clock logic is structural (gates, time advance, tile recomposite);
  the 30 Hz visual smoothness itself needs a GUI run to judge — the harness
  proves the semantics, not the perceived motion quality.
- Video export depends on an ffmpeg binary present on the machine (probed,
  cached, honest error when missing).
- WebM alpha was verified on this machine's ffmpeg; other builds may lack
  `libvpx-vp9` — the UI then simply hides the option.

## Product polish (checkpoint freeze)

| Polish | What |
|---|---|
| Export UX | ONE dominant choice: **Living Poster video (MP4)**; WebM alpha + PNG frames moved behind an advanced menu; every option disabled until its pipeline is verified (no dead buttons) |
| Before/After moment | **Compare** switch in the inspector Motion group: freezes the preview at t=0 (exactly the exported frame 0) vs the living loop |
| Motion-feel audit kit | `living_live_feel_sheet.png` / `living_synth_feel_sheet.png` — Subtle/Normal/Strong × 12 frames on one sheet, so the 30 Hz feel can be judged in a single image; tuning knobs documented (default amplitude 0.65, speed, phase) — no blind changes |
| Demo runbook | `_REVIEW/LIVING_POSTER_DEMO.md` — the full Harvest → Compose → Boost → Living → MP4 flow on the real dancer clip (`video_input.mkv`), with talking points, export cheat sheet and troubleshooting |
| Tagline | status line after a successful MP4 export: *"don't generate the moment, keep it alive"* |

**Remaining manual step (needs eyes, not code):** run
`pwsh scripts/launch-pb-noskip.ps1`, walk `LIVING_POSTER_DEMO.md`, and audit
the motion feel from the sheets + live preview. No further implementation is
planned until that audit asks for a specific, observed change.