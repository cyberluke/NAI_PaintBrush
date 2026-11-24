# FASHN human parser and semantic mask contract

> Branch: `feature/holo-material-integration` in the isolated worktree
> `D:\_SATIN_AI\NAI-OS\vlc-holo`. This document describes the native parser
> service added for the material engine. The service is built into the isolated
> worktree plugin and wired into the Play Pad. The MSVC runtime DLL and the
> 262 MB SegFormer model remain outside the repository; the runtime is staged
> under `win64-meson/bin/human-parser` for local testing and the model is loaded
> from the configured model directory.

## 1. What was delivered

A real SegFormer-B4 human parser runtime with the same leaf-DLL architecture as
neural matting:

```
MinGW Paintbrush plugin (Qt)
  pb_human_parser_service.{hpp,cpp}     async job, revision, cancellation
        |
  paintbrush::human_parser::Runtime     MinGW shim, QImage <-> C ABI
        |
  pb_human_parser_cuda.dll              MSVC + ONNX Runtime 1.30 + CUDA 13.3
        |
  fashn-human-parser.onnx               SegFormer-B4, 18 fashion classes
```

The parser is a **fashion human parser**, not an object or car segmenter. It
consumes the existing SAM3 subject alpha; it never replaces the subject
identity or changes its alpha.

Deliverables:

| File | Purpose |
|---|---|
| `modules/gui/qt/paintbrush/gpu/human_parser_cuda_api.h` | C99 ABI shared by MSVC and MinGW |
| `.../gpu/human_parser/human_parser_runtime.cpp` | ONNX Runtime session, preprocessing, inference, postprocessing |
| `.../gpu/human_parser/sha256.h` | model provenance hash (header-only) |
| `.../gpu/human_parser/CMakeLists.txt` | separate MSVC build |
| `.../gpu/human_parser/tools/human_parser_smoke.cpp` | headless acceptance smoke |
| `.../gpu/human_parser/tools/wic_image.h` | WIC PNG/WebP load, PNG save |
| `.../gpu/human_parser/tools/qimage_shim_smoke.cpp` | MinGW QImage -> C ABI integration smoke |
| `scripts/pb-test-human-parser-shim.ps1` | builds/stages/runs the QImage shim smoke |
| `.../gpu/pb_human_parser.h` | MinGW-facing Runtime API |
| `.../gpu/pb_human_parser_shim.cpp` | explicit DLL loading + QImage marshalling |
| `.../pb_human_parser_service.{hpp,cpp}` | asynchronous Qt service and canonical coverage helper |
| `scripts/pb-build-human-parser.ps1` | DLL + smoke build in a dedicated directory |
| `scripts/pb-check-human-parser-sources.ps1` | MinGW syntax check with the exact meson flags |
| `docs/HUMAN_PARSER.md` | this contract |

Meson registration (the only edit to `modules/gui/qt/meson.build`):

Added to `moc_headers`:

```meson
'paintbrush/pb_human_parser_service.hpp',
```

Added to `qt_plugin_sources` after the matting sources:

```meson
'paintbrush/pb_human_parser_service.cpp',
'paintbrush/pb_human_parser_service.hpp',
'paintbrush/gpu/pb_human_parser_shim.cpp',
'paintbrush/gpu/pb_human_parser.h',
'paintbrush/gpu/human_parser_cuda_api.h',
```

No matting/SAM source was modified. The MSVC runtime is **not** registered in
meson (it is built as a separate leaf DLL).

## 2. Model provenance

Pinned `fashn-ai/fashn-human-parser` revision
`1f80c34dbab321c5730dda5c3fea279fd3e97498` (SegFormer-B4).

| Field | Value |
|---|---|
| Artifact | `D:\_SATIN_AI\models\fashn-human-parser\fashn-human-parser.onnx` |
| Bytes | 262,264,726 |
| SHA-256 | `46a413ba51e331d4dfb8c103dbcd9319e0d941ef00ca03e510295b6b2ba81b82` |
| Input | `pixel_values` float32 `[1,3,576,384]`, RGB, ImageNet normalization |
| Output | `logits` float32 `[1,18,144,96]`, 1/4-resolution logits |
| Exporter parity | PyTorch/ONNX max abs 2.235e-05, class agreement 1.0000 |
| License | inherits the NVIDIA Source Code License for SegFormer; review distribution rights before shipping |

The native session re-verifies the SHA-256 against `manifest.json` on load
(unless a caller explicitly sets `verify_model_hash = 0` for development) and
reports the verified hash in `RunInfo::modelSha256`. Weights are not committed.

## 3. Class-to-region mapping

Every class belongs to exactly one material-facing group. The group plane is
the **sum** of its member class softmax probabilities (classes are mutually
exclusive), quantized to 0..255.

| Group | Classes | Use |
|---|---|---|
| Garment | `top` (3), `dress` (4), `skirt` (5), `pants` (6) | recolor / holographic material |
| Hair | `hair` (2) | independent hair treatment |
| Skin | `face` (1), `arms` (12), `hands` (13), `legs` (14), `feet` (15), `torso` (16) | protection / skin-aware treatment |
| Accessory | `belt` (7), `bag` (8), `hat` (9), `scarf` (10), `glasses` (11), `jewelry` (17) | hard-detail protection / separate material |
| Background | `background` (0) | never materialized |

Rationale: a scarf and belt behave like worn hard accessories for material
preservation, so they are grouped with the accessory protection mask rather
than with the garment surface. Change the mapping in one table
(`kClassGroup` in `human_parser_runtime.cpp`) if the art direction wants a
different split; the C ABI also exposes `class_group()` so callers do not
duplicate the table.

## 4. Preprocessing contract

The runtime reproduces the exporter's OpenCV preprocessing, not an
approximation:

1. Convert the source to RGB.
2. Resize to exactly **384 x 576** (width x height). No letterbox; the model
   was trained and validated with an aspect-ratio squeeze.
   - If **both** axes downscale: exact port of OpenCV `INTER_AREA`
     (`computeResizeAreaTab`, separable float accumulation). Verified
     pixel-identical to `cv2.resize(..., INTER_AREA)` on the 2324x1152
     model-card example.
   - If **either** axis zooms in: OpenCV's documented `INTER_AREA` fallback.
     This is not `INTER_LINEAR`; it uses `sx = floor(d*scale)` and the
     fractional part of the destination right edge, quantized to 11-bit fixed
     point for 8-bit images. Reproducing it matters for mixed-size inputs such
     as the 632x316 car cutout.
3. Normalize with ImageNet mean `[0.485, 0.456, 0.406]` and std
   `[0.229, 0.224, 0.225]`.
4. Write NCHW float32 `[1,3,576,384]`.

The smoke optionally writes the exact model-input RGB
(`*-model-input.png`, 384x576) for A/B comparison. It was compared against
`cv2.resize` and a NumPy port of the same algorithm: **0 differing pixels**.

## 5. Output contract

From `logits [1,18,144,96]`:

1. **Grid counts** are computed from the hard argmax at 96x144 before any
   upsample. They are exactly comparable to the exporter's
   `manifest.json` validation counts.
2. Each of the 18 logit planes is bilinearly upsampled to the source size with
   `align_corners=false`.
3. A max-subtracted softmax is computed per source pixel.
4. The hard argmax becomes `classMap` (Grayscale8, values 0..17).
5. Group probabilities are summed per group and quantized to 0..255.
   `garment`, `hair`, `skin`, `accessory` are always produced.
6. The full 18-class probabilities are opt-in (`-18*W*H` bytes; at 4K this is
   about 216 MB). The service exposes `setExportClassProbabilities()`.
7. Counts are reported at both source and grid resolution. Cancellation is
   checked between row batches; `RunOptions::SetTerminate()` cancels the ORT
   inference itself.
8. Postprocessing is row-parallel (up to 16 threads).

The C ABI also accepts an optional `model_rgb` diagnostic buffer
(`384*576*3`). It is not a material mask and is unused by the service.

## 6. Coverage semantics

The parser supplies independent semantic probabilities. The subject alpha is
finished coverage owned by SAM3. They are combined only through the canonical
`paintbrush::creative::composeCoverage()` function:

| Mode | Formula | Meaning |
|---|---|---|
| `InheritedCoverage` | `min(a,b)` | `b` already carries final coverage (cutout/matting alpha) |
| `IndependentProbability` | `(a*b + 127) / 255` | two independent probabilities -> product |
| `UserConstraint` | `min(a,b)` | hard artist/assistant cap |

`composeSemanticCoverage(subjectAlpha, semanticPlane, mode)` in
`pb_human_parser_service.{hpp,cpp}` applies that function per pixel and returns
Grayscale8. It is the integration helper for the material engine.

Important observed property: the supplied Lamborghini cutout alpha is binary
(0 or 255), and for a binary alpha `min(255,b) == (255*b+127)/255`; therefore
`InheritedCoverage` and `IndependentProbability` produce identical results on
that fixture. This is mathematically expected, not a failure. The smoke also
runs a diagnostic soft alpha (half of the cutout alpha) to exercise the
distinction:

```
canonical 128+128 -> inherited=128 independent=64 userConstraint=128
soft garment       inherited 40181.5 px, independent 23228.8 px, 104487 differing
soft foreground    inherited 50976.0 px, independent 38290.1 px, 104270 differing
```

## 7. Integration points for the creative materials

The service is additive and not yet wired into `pb_controller_art.cpp`, the
Play Pad catalog or QML (another agent owns those files). The intended wiring:

```cpp
// Own one service in the controller / art subsystem.
HumanParserService parser;
parser.configure(vlcObject);
parser.setModelDir(modelDir);          // or PB_HUMAN_PARSER_MODEL_DIR
parser.setDeviceTier(0);               // auto, 1 gpu, 2 cpu

connect(&parser, &HumanParserService::parseReady, this,
        [this](const paintbrush::HumanParseResult &r, quint64 revision,
               quint64 requestId) {
            if (revision != currentRevision) return; // stale
            // r.masks.garment / hair / skin / accessory
            // r.masks.classMap / r.masks.classPixels / r.masks.groupPixels
        });

// The subject alpha is the existing SAM3 result. The parser consumes it.
const quint64 id = parser.request(rgb, subjectAlpha, revision);
parser.cancel(id);                     // optional, supersedes the job
```

Material routing:

| Material need | Input |
|---|---|
| Garment mask | `result.masks.garment` (soft probability) |
| Hair treatment | `result.masks.hair` |
| Skin protection | `result.masks.skin`; compose with the subject alpha using `InheritedCoverage` or a threshold for a hard guard |
| Accessory / hard detail protection | `result.masks.accessory` combined with the existing detail-protection mask |
| Subject silhouette | unchanged SAM3 alpha; never replaced by a parser class |
| Detail preservation (windows, lights, plate, wheels) | existing `lamborghini-protection.png` or the production detail mask; this is **not coverage** and must not be passed as a region mask |

Do not feed the protection mask into `composeSemanticCoverage()`. Protection
attenuates sheen/material strength; coverage decides where the material
exists. The two are separate inputs by design.

The service snapshots both the RGB and alpha on the caller's thread
(`QImage::copy()` after format conversion), so a later canvas edit cannot race
the inference worker. Revision numbers and request IDs let the caller discard
stale completions. Changing the model directory or device marks the loaded
session stale; the worker unloads and reloads at the next job instead of
destroying a session while a parse is in flight.

## 8. Build and run

The runtime is built with MSVC in a dedicated directory, never in the shared
`win64-meson` tree:

```powershell
cd D:\_SATIN_AI\NAI-OS\vlc-holo
pwsh -File scripts/pb-build-human-parser.ps1
```

Defaults:

- ORT root: `D:\_SATIN_AI\NAI-OS\gpu-matting\deps\ort-1.30.0`
- Build: `D:\_SATIN_AI\NAI-OS\gpu-human-parser\build-msvc`
- Output: `...\bin\Release\pb_human_parser_cuda.dll`,
  `...\bin\Release\human_parser_smoke.exe`

The MinGW service/shim are checked without building the plugin:

```powershell
pwsh -File scripts/pb-check-human-parser-sources.ps1
# SYNTAX PASS
```

The MinGW QImage -> C ABI path is exercised by a standalone Qt console test
(no plugin build):

```powershell
pwsh -File scripts/pb-test-human-parser-shim.ps1
# GRID 11530 61 236 619 2 0 568 0 0 0 0 12 206 35 279 158 118 0
# SHIM SMOKE PASS
```

It stages only the small ORT core closure and uses CPUExecutionProvider for
exact parity; the MSVC smoke covers the CUDA provider.

Smoke, GPU:

```powershell
& 'D:\_SATIN_AI\NAI-OS\gpu-human-parser\build-msvc\bin\Release\human_parser_smoke.exe' `
  --model 'D:\_SATIN_AI\models\fashn-human-parser\fashn-human-parser.onnx' `
  --manifest 'D:\_SATIN_AI\models\fashn-human-parser\manifest.json' `
  --fashion 'C:\Users\lukes.COREI9\AppData\Local\Temp\pb-holo-parser\fashn-example.webp' `
  --car 'D:\_SATIN_AI\models\holo-reference\lamborghini-cutout.png' `
  --dll 'D:\_SATIN_AI\NAI-OS\gpu-human-parser\build-msvc\bin\Release\pb_human_parser_cuda.dll' `
  --runtime-dir 'D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin\matting' `
  --out-dir 'D:\_SATIN_AI\NAI-OS\gpu-human-parser\smoke-out' `
  --device auto
```

Use `--device cpu` for the exact exporter-parity gate. The ORT/CUDA closure is
shared read-only from the deployed matting directory; no file in the live tree
is modified.

## 9. Verification evidence

Measured on 2026-11-24 with ORT 1.30.0, CUDA 13.3, NVIDIA RTX 4090:

| Check | Result |
|---|---|
| Model SHA-256 | `46a413ba...2ba81b82` verified on both runs |
| CUDA EP assignment | 2,096 CUDA nodes, 0 CPU nodes |
| Fashion example, CPU | grid counts **MATCH** the manifest exactly (total abs diff 0) |
| Fashion example, GPU | total abs diff 4 of 13,824 cells (0.029%); within the 0.1% tolerance |
| Determinism | two identical runs, byte-identical class/group masks |
| Probability sanity | per-pixel 18-class sum range 248..263 (quantization of 18 classes around 255) |
| GPU timings | load 2.6-5.3 s (includes 262 MB hash + ORT init), first 626-986 ms, warm 35-47 ms; pre 8-15 ms, post 57-193 ms at 2324x1152 depending on host load |
| CPU timings | load 2.1-3.6 s; inference 1.85-2.12 s; post 233-365 ms at 2324x1152 |
| Car 632x316 | GPU 37-85 ms inference, 17-143 ms post; CPU 1.9 s, 148 ms |
| Model-input parity | 0 differing pixels vs `cv2.resize(..., INTER_AREA)` |
| Composition | canonical 128/128 -> 128/64/128; soft alpha separates min/product |
| MinGW QImage shim | CPU grid counts exact; `SHIM SMOKE PASS` |
| Smoke result | `SMOKE PASS` on both `--device auto` and `--device cpu` |

The CUDA EP can flip a handful of near-tie pixels because its kernel
accumulation order differs from the CPU EP. That is why the smoke reports the
delta and allows 14 grid cells (0.1%); the CPU run is the exact reproducibility
gate. The graph parity itself was already proven by the exporter.

Diagnostic images are under `D:\_SATIN_AI\NAI-OS\gpu-human-parser\smoke-out`
and `...\smoke-out-cpu`:

- `fashion-classmap.png`, `fashion-classmap-index.png`
- `fashion-garment.png`, `fashion-hair.png`, `fashion-skin.png`,
  `fashion-accessory.png`
- `fashion-model-input.png`
- `car-classmap.png`, `car-garment.png`, `car-alpha.png`,
  `car-foreground.png`, `car-model-input.png`

The car class labels are **not** car-part segmentation. The smoke says this
explicitly; the car fixture exists only to test alpha x semantic probability
composition.

## 10. Deployment layout

The shim resolves the runtime in this order:

1. `PB_HUMAN_PARSER_DIR`
2. `<exe>\human-parser`
3. `<plugin>\human-parser`
4. `<exe>`

It preloads the full ONNX Runtime/CUDA closure from that directory with
absolute paths; it never relies on `PATH`. This is mandatory, not cosmetic:
this machine has an unrelated `C:\Windows\System32\onnxruntime.dll` version
1.17.1, while the parser is compiled against ORT 1.30.0. If the packaged
1.30.0 core is not preloaded from the parser directory, Windows loads the
System32 copy and the ABI fails with "requested API version [30] ... only
[1, 17] are supported". The shim smoke validates the preload path.

Recommended package layout:

```
<exe>\human-parser\
  pb_human_parser_cuda.dll
  onnxruntime.dll
  onnxruntime_providers_shared.dll
  onnxruntime_providers_cuda.dll
  cudnn*.dll  cublas*.dll  cudart*.dll
  models\
    fashn-human-parser.onnx
    manifest.json
```

The model directory can be overridden with `PB_HUMAN_PARSER_MODEL_DIR`.
The service requires both `fashn-human-parser.onnx` and `manifest.json` and
refuses to load an unverified model. `PB_HUMAN_PARSER_PROFILE=1` enables ORT
profiling into `<models>\profiles`.

The DLL uses the MSVC dynamic CRT, like `pb_matting_cuda.dll`. The target
machine therefore needs the x64 Visual C++ 2015-2022 runtime
(`vcruntime140.dll`, `msvcp140.dll`) or the MSIX must carry it.

The service reads three optional VLC config values (the integration owner must
register them; defaults apply when absent):

- `paintbrush-human-parser-model-dir` (string)
- `paintbrush-human-parser-device` (`auto`/`gpu`/`cpu`)
- `paintbrush-human-parser-class-probabilities` (integer/bool)

Packaging into `win64-meson\bin\human-parser` was intentionally **not**
performed here: the shared app tree is owned by other agents and was being
built concurrently. The DLL is built and tested in
`D:\_SATIN_AI\NAI-OS\gpu-human-parser\build-msvc`.

## 11. Limits and non-claims

- Fashion-only model. Do not describe car labels as automotive segmentation.
- Aspect-ratio squeeze is the model's training contract; no letterbox.
- Group planes are soft probabilities, not binary selections. Threshold them
  when a hard region is required.
- No temporal consistency or identity tracking. Cache results by revision or
  frame identity in the caller.
- The 18-class probability planes are opt-in for memory reasons.
- CUDA and CPU can differ on a few near-tie pixels; use the CPU exact gate when
  bit-reproducing the exporter counts.
- The runtime depends on the same ORT 1.30.0 / CUDA 13.3 closure as matting. If
  that closure is upgraded, rebuild both `pb_matting_cuda.dll` and
  `pb_human_parser_cuda.dll`.
- The service is built into the worktree plugin and wired: the controller owns
  it, the Play Pad requests a parse when a subject is adopted, and the QML
  toggle selects whether the garment protection keeps skin/hair original. The
  parser stays optional: a missing runtime/model reports honestly and the
  material falls back to the whole subject. The MSVC runtime DLL and the 262 MB
  model remain deployment artifacts, not repository files.
