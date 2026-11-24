# Native LaMa ONNX inpainting

Paintbrush's Remove object and empty-prompt heal use a local Big-LaMa ONNX
model through the same ONNX Runtime CUDA infrastructure as neural matting. No
ComfyUI, OpenFill Python application, or network image upload is used.

## Model pin

| Field | Value |
|---|---|
| Source | `Carve/LaMa-ONNX` (https://huggingface.co/Carve/LaMa-ONNX) |
| Revision | `c3c0c9e468934d62e79c329e35d82dd09ff8c444` |
| File | `lama_fp32.onnx` |
| Bytes | 208,044,816 |
| SHA-256 | `1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6` |
| License | Apache-2.0 |
| Inputs | `image` float32 `[1,3,512,512]` RGB 0..1; `mask` float32 `[1,1,512,512]`, 1 = masked |
| Output | `output` float32 `[1,3,512,512]`; this export already clamps to `*255` |

The graph is a **fixed 512x512** export. Paintbrush extracts a context ROI
around the mask, squares/dilates it, resizes to 512, runs the graph, decodes the
result, resizes back and feathered-composites only the mask. Unmasked pixels
are preserved byte-identically. This is **not native-resolution synthesis**;
4K detail is reconstruction-limited by the 512 graph.

## Build and package

```powershell
# Model (once; verify the SHA above)
# https://huggingface.co/Carve/LaMa-ONNX/resolve/main/lama_fp32.onnx
# -> D:\_SATIN_AI\models\lama\lama_fp32.onnx

pwsh -File scripts/pb-build-lama-cuda.ps1
```

The build script writes the MSVC DLL, ONNX Runtime/CUDA/cuDNN closure, model and
`DEPENDENCIES.json` into `win64-meson/bin/lama/`. The MinGW plugin loads that
directory through `gpu/pb_lama_cuda_shim.cpp`; no system PATH dependency.

Config keys: `paintbrush-lama-model`, `paintbrush-lama-device` (auto/gpu/cpu).
Environment overrides: `PB_LAMA_DIR`, `PB_LAMA_DEVICE`, `PB_LAMA_DLL`,
`PB_LAMA_SHA256`.

## Phase timing

Every successful run reports a bounded set of phase totals (milliseconds,
`std::chrono::steady_clock`). There is no per-pixel logging.

| Key | Measured in | Meaning |
|---|---|---|
| `load` | `lama_runtime.cpp` `create_session` | persistent session + 208 MB graph load; identical for every run of that session |
| `mask` | `lama_runtime.cpp` `pblm_inpaint` | source setup copy + mask scan + connected components/pass planning |
| `preprocess` | `lama_runtime.cpp` `process_pass` | ROI crop + mask dilation + 512x512 tensor resampling (summed over passes) |
| `infer` | `lama_runtime.cpp` `run_model` | `Ort::Session::Run` (summed over passes) |
| `composite` | `lama_runtime.cpp` `process_pass` | output decode + feather alpha + feathered write-back (`postprocess_ms` in the C ABI) |
| `publish` | `pb_lama_cuda_shim.cpp` + `pb_lama_service.cpp` | RGBA buffer -> owned QImage, plus the queued `inpaintReady` handoff |
| `total` | `lama_runtime.cpp` `pblm_inpaint` | DLL-side total; excludes `publish` |

The C ABI appends `load_ms` and `mask_analysis_ms` to `PblmRunInfo`; every
publication uses a `struct_size`-bounded copy, so older callers keep working.
`paintbrush::lama::RunInfo` exposes `loadMs`, `maskAnalysisMs`, `preprocessMs`,
`inferenceMs`, `compositeMs`/`postprocessMs`, `publishMs`, `totalMs`;
`LamaService::lastTimings()` returns the same values for the last successful
run. The worker log line is:

```
LaMa inpaint ready: backend=… gpu=… roi=… components=…
load=…ms mask=…ms preprocess=…ms infer=…ms composite=…ms publish=…ms total=…ms
```

`lama_smoke --model <graph> [--device auto|gpu|cpu]` prints the same DLL-side
keys per case, so timing can be checked without the GUI.

The C struct grew by appended fields only, so an old `pb_lama_cuda.dll` still
works with a new shim; its appended fields then read as unmeasured (`loadMs`
is reported as -1, not a fake 0). Rebuild both sides for complete data:
`scripts/pb-build-lama-cuda.ps1` (native DLL) and the MinGW plugin build.
`publish` exists only on the MinGW/service side and is not in the DLL.

## Test hook build gate

The real-process acceptance hook (`pb_test_hook.cpp`: loopback JSON channel on
127.0.0.1 with a per-process token) is test infrastructure and is **never part
of a public release build**.

- Meson option `paintbrush_test_hook` (type `feature`, default `auto`).
  - `auto` compiles the hook only when `buildtype` is `debug` or
    `debugoptimized`.
  - An explicit `-Dpaintbrush_test_hook=enabled` is refused for `release` and
    `minsize` (Meson warning; the sources stay out).
- When enabled, Meson adds `-DPAINTBRUSH_TEST_HOOK` and registers
  `pb_test_hook.{cpp,hpp}`.
- `pb_test_hook_gate.h` additionally vetoes `NDEBUG`, so a build that defines
  `PAINTBRUSH_TEST_HOOK` while asserting `NDEBUG` still compiles the hook out.
- `PaintbrushController` only creates the hook inside
  `#if PAINTBRUSH_HAS_TEST_HOOK`; `testApplyObjectMask` /
  `testLamaBackend` are inert stubs otherwise.
- `PB_TEST_HOOK` / `PB_TEST_HOOK_FILE` only decide whether an
  already-compiled-in hook listens. They cannot create one in a release build.

Dev/test rebuild (hook included):

```powershell
pwsh -NoProfile -File scripts\pb-build-plugin.ps1
```

Release rebuild (hook excluded):

```powershell
meson configure win64-meson -Dbuildtype=release
pwsh -NoProfile -File scripts\pb-build-plugin.ps1
```

## Verified

- GPU smoke 5/5: center, boundary, disconnected (both components), tiny, large;
  unmasked pixels preserved, 100% of masked pixels changed, CUDA node placement
  confirmed, 94–1078 ms/inference.
- CPU smoke 5/5: 5.2–12.5 s/inference.
- Reference parity: masked-interior MAE 3.41, PSNR 31.85 dB vs the model repo's
  `output_onnx_fp32.png` (WIC-vs-PIL resampling; not bit-exact).
- Cancellation: GPU after 125 ms, CPU after 156 ms.
- MinGW shim -> packaged DLL -> ORT/CUDA standalone smoke passed; model SHA
  verified.
- Mingw plugin build green; changed QML linted with 0 errors.

## Routing truth

- **Remove object** -> local LaMa when available; otherwise configured provider;
  otherwise local onion-peel with the appropriate capability label.
- **Empty-prompt heal / fill** -> local LaMa.
- **Prompt-guided fill** -> configured image provider only; with no provider and
  LaMa installed it reports that LaMa cannot follow a prompt instead of silently
  running a pixel fallback.
- **Replace Background** / Make sticker / Finish painting are unchanged.

LaMa is a neural reconstructor, not a prompt-guided generative model.
