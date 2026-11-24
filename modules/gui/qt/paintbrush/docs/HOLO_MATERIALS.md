# Holographic materials and fashion parsing — feature branch status

> Branch: `feature/holo-material-integration`, based on `499cc77620`.
> Isolated tree: `D:\_SATIN_AI\NAI-OS\vlc-holo`. Not merged into `master`.
>
> Status 2026-11-24: the material engine is hardened, eight recipes are wired
> through the existing patch pipeline, and the FASHN export is now
> numerically identical to PyTorch. The native parser service, cancellable
> full-resolution rendering and app-level preview capture are still pending
> (see §5).

## 1. Material engine

One engine, eight parameterised recipes. Existing fashion ids and behaviour
are preserved; `holo-fashion` (Prism Muse) is untouched.

### 1.1 Recipes

| Id | Name | Surface intent |
|---|---|---|
| `holo-liquid` | Liquid Hologram | broad rainbow diffraction, moving specular bands |
| `holo-opal` | Opal Dream | pastel low-frequency interference, pearlescent sheen |
| `holo-chrome` | Chromatic Chrome | high metallic, sharp reflections, silver/blue |
| `holo-prism` | Prismatic Fantasy | micro-grating, strong seeded sparkle |
| `holo-wrap` | Holographic Wrap | deep teal/blue iridescent wrap, clearcoat |
| `holo-chameleon` | Chameleon Paint | view-angle hue shift, green/violet/copper |
| `holo-glitter` | Glitter Metallic | dense metallic flakes, hard glints |
| `holo-livery` | Electric Livery | dark bodywork with electric spectral accents |

The shader combines two-beam thin-film interference at three wavelengths, a
diffraction grating, Fresnel/specular/sheen, a recipe clearcoat, view-angle
palette shift and metallic flakes. It is physically inspired, **not** a
calibrated spectral/BRDF simulation; do not describe it as physically accurate
chrome.

### 1.2 Corrections from the adversarial review

- **Normalized object-space coordinates.** The grating, thickness noise and
  glints use `u,v` (object space), not raw pixels. The same material at 1x and
  2x downsamples back with an Oklab **0.9995** correlation (harness metric).
- **Linear-light accumulation.** Source, palette and environment are converted
  to linear before diffuse/specular/sheen accumulation, then back to sRGB.
- **Normal confidence.** `estimateNormals(..., &confidence)` returns a
  Grayscale8 reliability map. A wide blur separates a coherent fold from
  printed stripes: observed **fold 0.8055 / stripes 0.0942**; the material
  attenuates the normal where confidence is low, so a logo does not become
  fake geometry.
- **Mask composition contract.** `CoverageMode` distinguishes
  `InheritedCoverage` (a finished cutout: `min`), `IndependentProbability`
  (semantic probability: product) and `UserConstraint` (hard cap: `min`).
  Tests: 128+128 -> **128 / 64 / 128**.
- **No universal `min()`.** `composeCoverage()` is the only place the two
  coverages meet; the current subject path uses `InheritedCoverage`.

### 1.3 Verification (library harness)

`scripts/pb-creative-art-tests.ps1` now checks:

- **all 8 recipes** deterministic and spatially distinct (minimum pairwise
  Oklab RMS **0.0426**, pair liquid|wrap; mean Oklab 0.0129, mean RGB 0.0201);
- **full-image alpha** equality (0 mismatches) and sheen confined to the mask;
- **absolute lightness error** per recipe, not only correlation: Liquid keeps
  **0.8853** correlation; the gates are mean < 0.15, p95 < 0.35, clipped
  pixels < 25% on the albedo and < 30% on the albedo+sheen composite;
  observed example (Electric Livery): mean 0.0529, p95 0.1177, clipping 0.05%;
- **composite** (albedo + screen sheen) metrics, not only the albedo;
- **resolution parity**, **normal confidence separation**, and coverage modes.

The harness is a library contract, not a visual-quality proof. Real
fashion/automotive reference comparisons remain in §5.

### 1.4 AAF mapping (no new service layer)

- **Strategy**: the `HoloMaterial` parameter set; the shader is
  parameter-driven, no object-category branches.
- **Adapter**: `CoverageMode` + `composeCoverage()` connect subject alpha,
  semantic probability and user constraints to the same material input.
- **Factory/Registry**: `holoMaterialPreset()` and the existing Play Pad patch
  catalog. No second registry.
- **Decorator**: clearcoat, flakes, colour shift and diffraction are optional
  parameters composed in one shader, not separate renderers.
- **Open/Closed**: a new surface recipe is a preset plus a catalog row; no
  change to `pb_creative_art` logic or controller special cases.
- **Dependency inversion**: the engine consumes source, mask, normals and
  confidence; it has no dependency on fashion or automotive classifiers.

## 2. Product integration (this branch)

- Catalog: eight new patch ids in `pb_creative.cpp`; existing
  `holo-fashion`/Prism Muse unchanged; families `Material` and `Automotive`.
- Play Pad: discovers them through the existing `patchList` (no special
  controller logic).
- Inspector → Creative: a material combo (fashion + automotive) plus **Apply**,
  calling the existing `applyCreativePatch(id)`.
- Assistant: the `apply_creative_patch` tool enum includes all eight ids.
- All results use the existing layer stack and `CompositeCommand`; one undo.
- Isolated build: full ninja build **EXIT=0**; QML contracts **345 members
  PASS**; the isolated launcher is `scripts/launch-pb-noskip-holo.ps1`.

**App-level self-test caveat:** the self-test flag now enables
`QStandardPaths` test mode before the controller, so a test run uses a
separate `qttest` project store and cannot touch the user's recovery. The
isolated build still fails to register the Paintbrush options at startup
(`--paintbrush` unknown) when launched against its separate plugin directory,
so the eight app previews and save/reopen/timings were **not captured** here.
The library harness, the full build and the QML contract pass. Do not present
the library harness as app acceptance.

## 3. FASHN Human Parser

Pinned `fashn-ai/fashn-human-parser` revision
`1f80c34dbab321c5730dda5c3fea279fd3e97498` (SegFormer-B4, 18 classes).
License: inherits the **NVIDIA Source Code License for SegFormer**; review
distribution rights before shipping. Weights are not committed.

### 3.1 The legacy export was wrong

The previous artifact used the legacy TorchScript exporter (opset 17,
`dynamo=False`). PyTorch/ONNX parity on the same input was:

```
max abs 15.3563   mean abs 0.940238   class agreement 0.0229
```

The per-class pixel counts happened to match, but the spatial prediction was
wrong — consistent with a layout/miscompilation issue rather than a different
input, and exactly the failure mode the review warned about. **Fixed** by
using the `torch.export`-based exporter:

```python
torch.onnx.export(..., opset_version=18, dynamo=True, external_data=False)
```

### 3.2 Corrected artifact

| Field | Value |
|---|---|
| File | `D:\_SATIN_AI\models\fashn-human-parser\fashn-human-parser.onnx` |
| Bytes / SHA-256 | 262,264,726 / `46a413ba51e331d4dfb8c103dbcd9319e0d941ef00ca03e510295b6b2ba81b82` |
| Input / output | `pixel_values` float32 `[1,3,576,384]` -> `logits` `[1,18,144,96]` |
| Preprocessing | RGB, `cv2.INTER_AREA` to 384x576, ImageNet mean/std |
| Parity | max abs **2.235e-05**, mean abs **1.065e-06**, class agreement **1.0000** |
| Exporter | torch 2.13.0+cpu, transformers 4.57.6, onnx 1.23.2, onnxruntime 1.32.0, opencv 4.11.0, numpy 2.5.4, pillow 10.4.0, huggingface_hub 0.36.2 |

Validation classes on the model-card fashion example: background 11530,
face 61, hair 236, top 619, pants 568, arms 206, legs 279, feet 158, torso
118, glasses 12. The manifest next to the ONNX records the revision, hashes,
input contract, exporter versions and parity.

**Still pending:** the native ONNX Runtime service and the garment/hair/skin/
accessory routing. The model is provisioned; it is not yet used by the app.

## 4. Automotive regression reference (supplied Lamborghini)

Source: `C:\Users\lukes.COREI9\Downloads\lamborghini.jpeg` (632×316).

`scripts/pb-auto-reference-prep.py` uses **Mask R-CNN (COCO car)** to segment
the complete car. The first orange-hull attempt clipped the front bumper and
the rear wheel; the Mask R-CNN instance includes both wheels, the bumper, roof
and wing. A detail-protection mask (dark glass/tyres/grille and
low-saturation highlights) covers **40.2%** of the car: windows, lights,
plate, wheels and dark trim keep the original pixels; the paint shader
receives the rest.

Artifacts (derived from the user's image; not committed):
- `D:\_SATIN_AI\models\holo-reference\lamborghini-cutout.png`
- `...\lamborghini-protection.png`
- `...\auto-wrap.png`, `auto-chameleon.png`, `auto-glitter.png`,
  `auto-livery.png` — the four material composites on the real car

The paint recipes use `baseReplace` (0.55–0.75) so the palette owns the hue
while the source lightness keeps panel shading and texture; fashion recipes
stay film-like (~0.10–0.18). The four car previews are visually distinct:
pastel pink/lavender iridescent wrap, mint-green chameleon, peach/gold
metallic glitter and pink/cyan striped electric livery.

Part-level preservation (precise panel seams, badge outlines) still relies on
the heuristic mask; a car-part parser or manual masks would replace it.

## 5. Remaining gaps (do not overclaim)

1. Native parser service, class masks and SAM3-alpha combination.
2. Cancellable async rendering and native-resolution effect synthesis; the
   commit is still synchronous and capped at 1600 px.
3. App-level previews, save/reopen and P50/P95 for the eight new recipes.
4. Real fashion and automotive reference comparisons.
5. Material-aware continuous Play Pad modulation (the pad currently modulates
   opacity/motion; the diffraction phase is fixed per preview build).

## 6. Reproduce

```powershell
cd D:\_SATIN_AI\NAI-OS\vlc-holo

# hardware-independent material contract
pwsh scripts/pb-creative-art-tests.ps1

# isolated app build and (when the shared recovery is clear) self-test
ninja -C win64-meson
pwsh scripts/launch-pb-noskip-holo.ps1 -CreativeSelfTest

# pinned parser export + PyTorch/ONNX parity (downloads ~257 MB once)
$env:PYTHONIOENCODING = "utf-8"
python scripts/pb-fashn-export.py --test-image <fashion-photo.webp>
```
