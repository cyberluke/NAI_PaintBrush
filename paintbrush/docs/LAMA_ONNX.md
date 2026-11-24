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
