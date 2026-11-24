# Holographic materials and fashion parsing — implementation notes

> Status 2026-11-24: the **material engine** is implemented and exposed in the
> Creative Play Pad. Eight surface recipes share one shader and the existing
> preview/commit/export path; four of them are painted-body (automotive)
> recipes. The pinned parser export remains a library/export artifact; the
> app still styles the committed subject mask, not parser classes. The
> existing Prism Muse (`holo-fashion`), Electric K-pop and the one-undo
> transaction are unchanged; the Play Pad gains eight material chips.

## 1. What is implemented

### 1.1 One material engine, eight recipes

`pb_creative_art` has a physically inspired material shader instead of
piling more gradient overlays on top:

| API | Purpose |
|---|---|
| `creative::HoloMaterialId` | `LiquidHologram`, `OpalDream`, `ChromaticChrome`, `PrismaticFantasy`, `HolographicWrap`, `ChameleonPaint`, `GlitterMetallic`, `ElectricLivery` |
| `creative::holoMaterialPreset(id)` | optical parameters for a recipe |
| `creative::estimateNormals(source, mask, strength, &confidence)` | screen-space orientation from the source's own shading, with a texture-confidence map |
| `creative::composeCoverage(alpha, region, mode)` | explicit coverage semantics (inherited cutout / independent probability / user cap) |
| `creative::holographicMaterial(source, mask, normals, material, strength, seed, coverageMode, confidence, protection)` | returns an **albedo** layer (replaces the subject inside the mask) and a transparent **sheen** layer |

The shader combines:

- a two-beam thin-film interference term at three wavelengths, with film
  thickness driven by shading, orientation and seeded low-frequency noise;
- a diffraction grating term with a recipe-specific spatial frequency;
- Fresnel response, a roughness-driven specular lobe, a broad sheen and
  discrete seeded glints;
- an environment tint sampled from the recipe palette by the surface normal;
- **paint response** for the automotive recipes: palette-owned hue with the
  source lightness preserving panel shading (`baseReplace`), a second narrow
  white **clearcoat** lobe, sparse seeded **metallic flakes**, and a
  view-angle **colour shift** (`colorShift`).

The source RGB is converted to linear light, multiplied by the diffuse
response (so folds and panel texture remain) and the spectral/specular light
is added on top. The result is converted back to sRGB. It is not a flat
rainbow map.

The grating, thickness noise, flake and glint hashes are evaluated in
**object-space UV**, not pixel coordinates, so a preview and a full-resolution
export describe the same pattern. Normals and their blur radii scale with the
source, so the estimated slope is also object-space.

| Recipe | Character |
|---|---|
| **Liquid Hologram** | broad rainbow diffraction, moving specular bands, glossy coating |
| **Opal Dream** | low-frequency pastel interference, pearlescent sheen, low metallic |
| **Chromatic Chrome** | high metallic response, sharp directional reflections, silver/blue |
| **Prismatic Fantasy** | micro-grating diffraction, embossed highlights, strong seeded sparkle |
| **Holographic Wrap** | full-body iridescent wrap, clearcoat, light flake, blue/violet shift |
| **Chameleon Paint** | painted panel with a strong green/teal/plum view-angle shift |
| **Glitter Metallic** | dense silver/gold metallic flake under a clearcoat |
| **Electric Livery** | dark base with neon cyan/magenta/lime print and light flake |

The four fashion recipes keep their original ids (0..3), names and accepted
character, using the accepted reference engine parameters. The four body
recipes are additive ids 4..7.

`scripts/pb-creative-art-tests.ps1` verifies the contract:

- all eight recipes are deterministic for a given seed;
- alpha over the whole image matches `min(source alpha, mask)` and the sheen
  never paints outside the subject;
- the eight albedo+sheen fingerprints and names are distinct;
- the recipes are perceptually distinct (mean and spatial RGB difference);
- the Liquid Hologram albedo keeps a **luminance correlation above 0.60** with
  the source (texture and folds survive);
- the same material rendered at 2x downscales back to the 1x look
  (preview/export agreement);
- normals confidence separates a smooth fold from printed stripes (a decal or
  livery print does not become fake geometry);
- a `protection` mask keeps the original pixels and suppresses the sheen
  without changing alpha;
- coverage modes are explicit and unit-tested.

An optional real-car regression fixture runs when `PB_AUTOMOTIVE_REFERENCE`
points at a directory containing `lamborghini-cutout.png` and
`lamborghini-protection.png`; it renders the four body recipes to
`auto-wrap.png`, `auto-chameleon.png`, `auto-glitter.png` and
`auto-livery.png`. No network is used. The locally accepted reference images
are `selftest-holo-*.png` under `paintbrush/docs/creative-synthesis-assets/`.

### 1.2 Pad integration

`pb_creative.cpp` exposes eight material chips in the Play Pad:

| Patch id | Name | Family |
|---|---|---|
| `holo-liquid` | Liquid Hologram | Material |
| `holo-opal` | Opal Dream | Material |
| `holo-chrome` | Chromatic Chrome | Material |
| `holo-prism` | Prismatic Fantasy | Material |
| `holo-wrap` | Holographic Wrap | Automotive |
| `holo-chameleon` | Chameleon Paint | Automotive |
| `holo-glitter` | Glitter Metallic | Automotive |
| `holo-livery` | Electric Livery | Automotive |

Each chip selects a preset, builds the same piece list (backdrop, material
albedo, sheen, rim/bloom, optional butterflies or glitter sparkle, reflection)
and therefore runs through the existing live preview, commit, undo, save/reopen
and export path. There is no second renderer and no per-patch C++ switch in the
commit code: the patch id selects the preset, the engine renders it.

The pre-existing `holo-fashion` (Prism Muse) recipe is unchanged; the four
fashion material recipes are new first-class chips rather than a replacement.

### 1.3 Masks

Coverage comes from the existing **subject mask** — the layer's editable mask /
cutout alpha — so anything the user has masked out of the cutout (windows,
trim, background) is already excluded without new segmentation.

The engine also accepts an optional **protection** plane (glass, lights, plate,
badges, trim): 255 keeps the original pixels and suppresses the sheen, 0
receives the full material; it never changes alpha. This is a contract for an
existing part/detail mask supplied by the caller. This worktree has no
part-level parser mask in the pad path, so the pad passes no protection plane
today and no segmentation is invented here. The standalone fixture exercises
the plane with a supplied car protection mask.

### 1.4 Determinism

- Presets are compile-time constants; the seed comes from the pad's existing
  `creative.seed` (stored in committed layer metadata and restored on resume).
- No wall clock, `QRandomGenerator`, threads or unordered iteration is used in
  the shader; all noise is `hash01`/`valueNoise` over the seed and object-space
  coordinates.
- The same source, mask, preset and seed produce byte-identical albedo and
  sheen images on every run and at preview or export resolution (apart from
  deliberately resolution-specific glints, disabled in the resolution test).

### 1.5 FASHN Human Parser, pinned and exported

`scripts/pb-fashn-export.py` produces a reproducible ONNX artifact from
[`fashn-ai/fashn-human-parser`](https://huggingface.co/fashn-ai/fashn-human-parser):

| Field | Value |
|---|---|
| Model | SegFormer-B4, 18-class fashion human parser |
| Revision | `1f80c34dbab321c5730dda5c3fea279fd3e97498` |
| License | inherits the **NVIDIA Source Code License for SegFormer**; review distribution rights before shipping |
| ONNX | `D:\_SATIN_AI\models\fashn-human-parser\fashn-human-parser.onnx` |
| Size / SHA-256 | 257,257,748 bytes / `085df81aa180cb9045783430828948d35984bf602250c8192fe700c895f18850` |
| Input | `pixel_values` float32 `[batch, 3, 576, 384]`, RGB, ImageNet mean/std |
| Resize | `cv2.INTER_AREA` to 384×576 (the training preprocessing) |
| Output | `logits` float32 `[batch, 18, 144, 96]` (1/4 resolution) |
| Classes | background, face, hair, top, dress, skirt, pants, belt, bag, hat, scarf, glasses, arms, hands, legs, feet, torso, jewelry |

Validation on the model card's fashion example (a real person):

```
background 11530  face 61  hair 236  top 619  dress 2  pants 568
arms 206  hands 35  legs 279  feet 158  torso 118  glasses 12
```

The contract is pinned in the generated `manifest.json` next to the ONNX
(revision, hashes, input/output, class map, validation counts). The weights are
**not committed** to the repository (size and license); the export script
recreates them.

## 2. What is not implemented yet

1. **Part-level masks in the pad.** The shader honors a protection plane, but
   the app has no car part parser (body vs glass/trim) and this change
   deliberately does not add one. Until an existing mask is supplied, the
   material covers the whole committed cutout.
2. **Native parser service for fashion.** The ONNX is exported; the app still
   uses the SAM3 silhouette plus a heuristic skin guard. A native inference
   service returning per-class probability masks is a separate change and is
   not part of this work.
3. **Garment/hair/skin/accessory routing.** Depends on (2).
4. **Cancellable async rendering and full-resolution export.** The commit is
   still synchronous and capped at 1600 px; large exports upscale the effect
   layers. The material itself is object-space, so the pattern is consistent,
   but a 4K/8K render is not tiled or cancellable yet.
5. **Material-aware Play Pad performance.** The pad modulates opacity/motion,
   not the diffraction phase. Continuous material modulation needs a debounced
   preview rebuild or a cheaper parameter path.

## 3. Reproduce

```powershell
# material engine + all existing art primitives (standalone, no VLC build)
pwsh scripts/pb-creative-art-tests.ps1

# the same probe against the local car fixture (no network)
$env:PB_AUTOMOTIVE_REFERENCE = "D:\_SATIN_AI\models\holo-reference"
pwsh scripts/pb-creative-art-tests.ps1

# pinned parser ONNX (downloads ~257 MB; cached under the output dir)
python scripts/pb-fashn-export.py
python scripts/pb-fashn-export.py --test-image <fashion-photo.webp>

# the plugin build
pwsh scripts/pb-build-plugin.ps1
```
