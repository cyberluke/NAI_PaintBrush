# Creative looks — subject-aware procedural effects

Paintbrush can turn a kept cutout into a complete poster look without a
generative image model. A look reads the layer's own alpha silhouette,
luminance and transform, then derives a stack of **ordinary Paintbrush
layers** — recolour, sheen, rim light, electric arcs, silhouette sparks,
butterflies, aura and backdrop. Every piece is editable, and the whole look is
one undo step.

## Status (2026-11-24)

Implemented and verified locally: the two looks, the shared
`applyCreativePatch` path used by both the Inspector and the assistant, the
one-undo layer commit, the deterministic art library and the acceptance
harness. The plugin was relinked from `21d296b45f` and its embedded
`PB_BUILD_SHA` matches that commit.

Not implemented yet: independent garment / hair / skin / accessory masks,
cancellable async rendering, and full-resolution effect synthesis above the
1600 px build cap. A four-recipe holographic material engine and the pinned
FASHN parser export are implemented and tested as libraries; exposing them in
the pad and adding the native parser service are the next integration steps
(see `docs/HOLO_MATERIALS.md`).

`master` is pushed to `origin/master`; `11e2cb1b47` and the follow-up fixes
are visible on GitHub (`cyberluke/NAI_PaintBrush`).

## Where it lives

| Surface | Entry point |
|---|---|
| Inspector → Creative | `Electric K-pop look`, `Holographic fashion look` (one tap, commits immediately) |
| Inspector → Creative → Play Pad… | the full pad: preview, gesture performance, record/loop, then **Make it real** |
| Top bar | `Play Pad` |
| Assistant | the `apply_creative_patch` tool (see below) |

The one-tap Inspector actions call
`PaintbrushCtx.applyCreativePatch(patchId)`, which adopts the current
selection, builds the look and commits it. The pad exposes the same recipes
with a live stage, so a look can be performed before it is committed.

## The two flagship looks

### `electric-kpop` — Electric K-pop

- **Subject**: Oklab selective recolour toward cyan / rose / magenta / lime.
  Lightness is preserved (the harness measures a worst-case Oklab ΔL of 0.013
  on a 220×300 test cutout), so fabric folds, shading and texture survive.
- **Light**: a spectral sheen that only appears on bright pixels, plus a
  two-scale cyan/rose rim.
- **Energy**: two seeded bolts from off-frame to the silhouette, with branches
  and sparks, plus silhouette-emitted sparks.
- **Scene**: an opaque neon backdrop (light stripes, speed rays, focal bloom,
  stars, vignette) placed at the bottom of the committed stack.
- **Accents**: four-point stars.

### `holo-fashion` — Holographic fashion (Prism Muse in the pad)

- **Subject**: the same Oklab recolour with a stronger iridescent hue drift and
  a higher `textureKeep`, so original chroma variation survives.
- **Material**: a position-moving spectral sheen (light travelling across the
  fabric, not a gradient painted on top) and a sharp silver chrome edge.
- **Scene**: a dark graded studio backdrop, a violet aura, chromatic bloom.
- **Accents**: luminous butterflies, prism sparkle, and a fading floor
  reflection of the source pixels.

Both looks use an approximate skin guard: skin-like Oklab hue/chroma/lightness
clusters receive less recolour. It is a heuristic, **not** a face or garment
segmentation. Part masks (hair / clothing / accessories) remain future work.

## Guarantees

- **Deterministic**: the same layer, patch, seed and gesture state produce
  byte-identical output. Seeds live in the layer metadata
  (`creative.seed`, `creative.params`).
- **Alpha discipline**: nothing is painted outside the selected silhouette;
  the harness asserts this.
- **Non-destructive**: the source layer is hidden, never modified. Every result
  layer is a normal layer with its own blend mode, opacity and finish chain.
- **Repeatable**: a second look reuses the hidden source cutout, never the
  previous look's effect layers.
- **No generation**: the looks cannot invent clothing, hairstyles, anatomy,
  poses or a new camera perspective. They only restyle pixels that already
  exist.

## Assistant contract

`apply_creative_patch` takes `{ "patch": "<id>" }` and commits the look on the
current selection in one undo. Valid ids include `electric-kpop`,
`holo-fashion`, and the earlier `neon-idol`, `sticker-forge`, `pastel-stage`,
`nocturne`, `ink-creature`, `spark-storm`, `kaleido`, `motion-echo`.
`get_canvas_state` returns the same catalogue under `creative_patches`.
The assistant is instructed that this is a real edit made of layers, not image
generation.

## Performance bounds

The commit builds at most at 1600 px on the long side and maps every piece onto
its real canvas rectangle; the subject recolour is resampled to at most 1.25×
the subject's displayed size. Larger canvases therefore get a slightly softer
effect layer instead of a multi-hundred-megabyte buffer, and commit time stays
bounded. The look is still built synchronously on the UI thread; on a 4K canvas
expect a short pause. If that becomes a problem, the next step is to move the
builder to a worker and stream progress.

## Verification

```powershell
# deterministic primitives, alpha discipline, luminance preservation
pwsh scripts/pb-creative-art-tests.ps1

# controller/QML surface + existing probes
pwsh modules/gui/qt/paintbrush/probes/run-probes.ps1

# in-app acceptance: needs a pristine canvas, writes previews to
# %TEMP%\nai-paintbrush-creative-selftest
pwsh scripts/launch-pb-noskip.ps1 -CreativeSelfTest

# rebuild the plugin
pwsh scripts/pb-build-plugin.ps1
```

The art harness compiles `tests/creative_art_test.cpp` with
`pb_creative_art.cpp` against Qt Core/Gui only — no VLC build, no GUI.
