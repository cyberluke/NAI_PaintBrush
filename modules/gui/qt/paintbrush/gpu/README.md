# NAI Paintbrush — CUDA SAM3 runtime

Native, in-process GPU SAM3 segmentation and temporal tracking for the real
VLC 4 Paintbrush application. No Python, HTTP server, WSL or external
inference process.

## Architecture (why a DLL)

VLC on Windows is built with **MinGW (GCC)**. CUDA on Windows requires the
**MSVC** host compiler. Linking CUDA objects into `libqt_plugin.dll` is not
ABI-compatible, and we do not migrate VLC to another compiler.

```
MinGW: libqt_plugin.dll
  Paintbrush services (segmentation + tracking)
      -> pb_sam3_cuda_shim.cpp           (sam3_* API, MinGW STL)
          -> sam3_cuda_api.h             (narrow C ABI, PODs + opaque handles)
              -> sam3_cuda.dll           (MSVC + CUDA, native in-process)
                  -> sam3.lib / ggml-cuda.dll / cublas / cudart
```

- All C++ objects, STL containers, exceptions, CUDA contexts and allocations
  stay inside `sam3_cuda.dll`.
- The bridge exports one symbol, `sam3c_get_api()`, returning a versioned
  function table. Every call has an error buffer.
- The shim loads the DLL from the executable directory with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` (VLC's plugin host restricts the default
  search path; a bare `LoadLibraryA` fails with Win32 126 even when the DLL is
  beside the exe).
- `pb_sam3_cuda_shim.cpp` implements the existing `sam3_*` API, so both
  Paintbrush services keep their current flow; CPU and GPU share the same
  code path and results.

## Pinned dependencies

| Component | Source | Pin |
|---|---|---|
| SAM3 CUDA | `greenjava/sam3.cpp` branch `cuda` | `c0600a7ffce9ae097b112cf02ed663a73f94c169` |
| GGML | `greenjava/ggml` branch `sam3-ops` (submodule) | `9fff8202695fb970e138f0692cd340ffa0f65ead` |
| CUDA | NVIDIA toolkit | 13.3, `sm_89` (RTX 4090) |
| Host | MSVC | Visual Studio 2022 (14.44) |
| Model | `PABannier/sam3.cpp` F16 | `sam3-f16.ggml`, 1,837,873,470 bytes, SHA-256 `1C8CEF822A6F0F0908C8E7C51139C1061A842EBFBC88E8B0BB1D8E342EC50F8E` |

The upstream PR referenced ggml `499c8a76`; the fork's tested submodule pin is
`9fff8202`, which contains the CUDA `WIN_PART`/`WIN_UNPART`, attention and
transposed-convolution work plus later fixes. The submodule pin is the pair we
built and verified.

## Fork patch

`patches/sam3-backend-query-and-cpu-threads.patch`:

1. Adds `sam3_backend_name()` / `sam3_backend_is_gpu()` so the app reports the
   **actual** backend (never inferred from `use_gpu`). The loader silently
   falls back to CPU when CUDA init fails; without this, GPU execution would be
   claimed incorrectly.
2. Makes the tracking graph thread count backend-aware: CUDA uses 4 threads;
   the CPU backend uses 1. The fork's CPU tracking graphs crash with an access
   violation on the second propagation with >1 worker (verified with both q4_0
   and F16). CPU tracking is slower but correct.

## Build

```powershell
pwsh -File scripts/pb-build-sam3-cuda.ps1
```

This clones/checks out the pins outside the VLC worktree
(`D:\_SATIN_AI\NAI-OS\gpu-sam3`), applies the patch, builds the SAM3 static
runtime and `sam3_cuda.dll` with MSVC + CUDA (`sm_89`), builds the acceptance
smoke tool, and deploys the runtime closure into `win64-meson\bin`.

Requirements: CUDA toolkit with `nvcc`, MSVC 2022 C++ workload, CMake ≥ 3.20,
Git. The build tree is several GB; keep it outside the repository.

### Deploy only

```powershell
pwsh -File scripts/pb-package-sam3-cuda.ps1 `
  -Model D:\_SATIN_AI\models\sam3-f16.ggml
```

The deployed closure is:

```
sam3_cuda.dll      MSVC/CUDA bridge (C ABI)
ggml.dll           ggml dispatch
ggml-base.dll      ggml base
ggml-cpu.dll       ggml CPU backend
ggml-cuda.dll      ggml CUDA backend
cudart64_13.dll    CUDA runtime
cublas64_13.dll    cuBLAS
cublasLt64_13.dll  cuBLASLt
```

`-BundleMsvc` additionally copies `msvcp140.dll`, `vcruntime140.dll` and
`vcruntime140_1.dll` for a machine without the VC++ redistributable.

The F16 model stays outside the application bin; point NAI Settings
(`paintbrush-sam-model`) at its absolute path.

## Device contract

`paintbrush-sam-device` (`gpu` default, `auto`, `cpu`) is persisted in `vlcrc`.
The launch scripts no longer pass a hardcoded `--paintbrush-sam-device=cpu`;
a command-line value would override the saved setting for the whole run.

| Requested | Behaviour | Actual backend |
|---|---|---|
| `gpu` | `create_context(require_gpu=1)`. Fails with the specific reason if CUDA/DLL/model init fails. Never silently runs CPU. | Reported by `sam3_backend_name` / `sam3_backend_is_gpu` |
| `auto` | Tries GPU; accepts CPU if CUDA cannot initialise. | Reported (CPU fallback is explicit in the log) |
| `cpu` | Forces the CPU backend. | `CPU` |

The app logs, for every load:

```
sam3_shim: loaded model requested=gpu actual=CUDA0 gpu=1
           device='NVIDIA GeForce RTX 4090' modelVram=1750MB
```

Settings distinguishes the requested tier, the detected adapter (`probe_gpu`
via the CUDA runtime) and the actual backend/device (segmentation status
`Ready · CUDA0 · NVIDIA GeForce RTX 4090`). GPU failures are shown as the
specific load error and the user can switch to CPU.

## Verification (RTX 4090, 23 GB, driver 615.21)

Acceptance tool: `sam3_cuda_smoke.exe` loads the DLL through the same
`LoadLibrary` + C ABI path as the plugin.

| Measurement | GPU (CUDA0) | CPU |
|---|---|---|
| F16 model cold load | 1.81–1.89 s | ~2.0 s |
| Model VRAM / buffer | 1,750 MB (3.27 GB device-used after load) | — |
| Encode 600×365 | 476 ms | 22.2 s |
| Encode 1546×1213 dog | 594 ms | 26.2 s |
| PVS segmentation | 134–195 ms | 197–241 ms |
| Track frame 600×365 | 506–624 ms | 26.6–27.6 s |
| Tracking IoU (frame 1, GPU vs CPU) | 0.9876 | — |
| Segmentation mask IoU (dog) | **0.999834** | — |

- GPU backend evidence: `ggml_cuda_init: found 1 CUDA devices`, backend
  `CUDA0`; F16 masks match CPU at IoU 0.999834. This is actual CUDA kernel
  execution, not a linked-library claim.
- In the **real VLC app** (PNG dog image, Object Select): F16 loaded on CUDA0,
  `modelVram=1750MB`, app encode `640×502` in 558 ms, PVS 153 ms, best
  detection score 0.9877, foreground 110,340 / 321,280 px. The mask is visible
  over the dog and `Keep` is enabled.
- CPU mode works in the same app pipeline; CPU tracking no longer crashes
  after the 1-thread tracking-graph patch.

## Secondary backend candidate (evaluated, not integrated)

`https://github.com/ropoctl/sam3cpp` advertises CUDA/Vulkan/Metal/CPU but uses
a different GGUF model format and API; its video-tracking compatibility is not
established. It is not a drop-in for the current SAM3 runtime. The C ABI in
`sam3_cuda_api.h` is the extension point: a Vulkan/Intel backend can be added
behind the same table without changing the Paintbrush services, but no
speculative abstraction is committed before such a backend exists.

## Known limitations

- The CUDA build requires MSVC + CUDA; it is a separate binary, not part of
  the MinGW plugin link.
- CPU tracking is functional but slow (1-thread tracking graphs in this fork;
  the encode itself dominates: ~22 s at 640×502).
- The GPU DLL is loaded lazily at the first SAM job; the Settings probe loads
  the CUDA runtime for the adapter name but not the model.
- `cublasLt64_13.dll` is large (~464 MB). It is required by `ggml-cuda.dll`.
- Exact-frame selection/capture/tracking forces software decode in the VLC
  preparser and snaps PTS to the selected video track's frame grid. The live
  video path still depends on VLC's preparser; on this Windows build, a
  precise-seek request can fail to deliver its completion callback even with
  software decode. That is a frame-source/VLC core issue, not the CUDA path:
  the CUDA SAM3 segmentation and tracking are verified by the native smoke
  harness, and the compose Object Select path is verified in the app.
