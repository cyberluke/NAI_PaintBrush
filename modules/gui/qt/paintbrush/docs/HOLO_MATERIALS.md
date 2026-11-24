# Holographic materials and fashion parsing — implementation notes

> Status 2026-11-24: the **material engine** and the **pinned parser export**
> are implemented and tested as libraries. They are **not yet exposed in the
> Play Pad or the native inference service**; that wiring is the next
> integration step and is documented below. The existing Prism Muse
> (`holo-fashion`), Electric K-pop, one-undo transaction and Play Pad are
> unchanged.

## 1. What is implemented

### 1.1 One material engine, four recipes

`pb_creative_art` now has a physically inspired material shader instead of
piling more gradient overlays on top:

| API | Purpose |
|---|---|
| `creative::HoloMaterialId` | `LiquidHologram`, `OpalDream`, `ChromaticChrome`, `PrismaticFantasy` |
| `creative::holoMaterialPreset(id)` | optical parameters for a recipe |
| `creative::estimateNormals(source, mask, strength)` | screen-space orientation from the source's own shading |
| `creative::holographicMaterial(source, mask, normals, material, strength, seed)` | returns an **albedo** layer (replaces the subject inside the mask) and a transparent **sheen** layer |

The shader combines:

- a two-beam thin-film interference term at three wavelengths, with film
  thickness driven by shading, orientation and seeded low-frequency noise;
- a diffraction grating term with a recipe-specific spatial frequency;
- Fresnel response, a roughness-driven specular lobe, a broad sheen and
  discrete seeded glints;
- an environment tint sampled from the recipe palette by the surface normal.

The source RGB is multiplied by the diffuse response (so folds and texture
remain) and the spectral/specular light is added on top. It is not a flat
rainbow map.

| Recipe | Character |
|---|---|
| **Liquid Hologram** | broad rainbow diffraction, moving specular bands, glossy coating |
| **Opal Dream** | low-frequency pastel interference, pearlescent sheen, low metallic |
| **Chromatic Chrome** | high metallic response, sharp directional reflections, silver/blue |
| **Prismatic Fantasy** | micro-grating diffraction, embossed highlights, strong seeded sparkle |

`scripts/pb-creative-art-tests.ps1` verifies the contract:

- all four recipes are deterministic for a given seed;
- alpha is zero outside the mask and fully present inside;
- the four albedo+sheen fingerprints are distinct;
- the recipe names are distinct;
- the Liquid Hologram albedo keeps a **0.9235** Pearson correlation with the
  source's Oklab lightness (texture and folds survive).

### 1.2 FASHN Human Parser, pinned and exported

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

That is a meaningful segmentation, not just a shape check. The contract is
pinned in the generated `manifest.json` next to the ONNX (revision, hashes,
input/output, class map, validation counts). The weights are **not committed**
to the repository (size and license); the export script recreates them.

## 2. What is not implemented yet

1. **Pad/UI exposure of the four recipes.** The engine and presets exist, but
   the Play Pad catalog and the Inspector one-tap actions still expose only
   Prism Muse. The wiring is a small branch in `pb_creative.cpp` plus four
   catalog entries and four assistant ids.
2. **Native parser service.** The ONNX is exported; the app still uses the
   SAM3 silhouette plus a heuristic skin guard. The next step is a native
   inference service using the existing ONNX Runtime CUDA infrastructure,
   returning per-class probability masks intersected with the committed SAM3
   subject.
3. **Garment/hair/skin/accessory routing.** Once (2) exists, the material
   shader consumes the class masks (top/dress/skirt/pants for garments, hair
   for chromatic highlights, face/arms/hands/legs for skin preservation,
   bag/hat/scarf/glasses/jewelry for accents).
4. **Cancellable async rendering and full-resolution export.** The commit is
   still synchronous and capped at 1600 px; large exports upscale the effect
   layers. The next step is the snapshot/cancellable job/tiled-render design,
   not a blanket `QtConcurrent::run`.
5. **Material-aware Play Pad performance.** The pad currently modulates
   opacity/motion, not the diffraction phase. Continuous material modulation
   needs a debounced preview rebuild or a cheaper parameter path.

The active creative-file work in the working tree (Play Pad linked-group
resume/suggestions) is intentionally untouched; the wiring above should land
after that work is committed to avoid sweeping it into this change.

## 3. Reproduce

```powershell
# material engine + all existing art primitives
pwsh scripts/pb-creative-art-tests.ps1

# pinned parser ONNX (downloads ~257 MB; cached under the output dir)
python scripts/pb-fashn-export.py
python scripts/pb-fashn-export.py --test-image <fashion-photo.webp>

# the plugin build (after the active creative-file work is committed)
pwsh scripts/pb-build-plugin.ps1
```
