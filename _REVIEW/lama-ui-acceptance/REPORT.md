# LaMa UI acceptance gate — real Paintbrush process

Date: 2026-11-24 (UTC)
Workspace: `D:\_SATIN_AI\NAI-OS\vlc`
Branch / HEAD: `master` / `13ac2c245347902739371e018aff351c85d16ba3`
Accepted gate build: `pwsh -NoProfile -File scripts\pb-build-plugin.ps1` → `NINJA_EXIT=0`, `plugins.dat` 2026-11-24 20:11:56 (the build that includes the cancellation-status fix). The gate matrix itself ran on the LaMa provider commit `cd34efb9bc` with the then-uncommitted hook; see **Commit provenance**. A later `plugins.dat` (20:23:01) exists in the shared build directory from unrelated concurrent work; it is not the accepted LaMa gate artifact and was not used for this evidence.

Phase timing and the test-hook compile gate were added after the gate and are **uncommitted**; they need the coordinator's plugin build. They do not change the accepted image result or any hash below.

No commit, stage, or push was performed for the post-gate bookkeeping change;
ComfyUI, OpenFill and unrelated creative features were not touched.
No `SetCursorPos`, `mouse_event`, `SendInput`, keystroke injection, or foreground stealing.
No app or build process remains.

## Method

UIA cannot reach the Qt Quick canvas or call `Q_INVOKABLE` methods, so the run used an
in-app, test-only loopback hook (`PB_TEST_HOOK=1`) inside the real `nai-paintbrush.exe`.
It calls the same `PaintbrushController` / `PaintbrushCanvasItem` entry points a QML
control would call:

- `openImageFile`, `openProject`, `saveProject`, `newBlankCanvas`
- `testApplyObjectMask` (env-gated; mirrors the production SAM3 mask state)
- `canvas()->setSelectionFromMask` / `setSelectionRect`
- `removeObject("")`, `generativeFill("")`, `cancelGeneration`, `undo`, `redo`
- exact canvas export via `flattenedPreview` / `DocumentRenderer` (no screen capture involved)

The hook listens on `127.0.0.1` with an ephemeral port and a per-process token, and is
never created in a normal launch. It is test infrastructure, not product behavior.

The hook is now **compile-time gated**: Meson option `paintbrush_test_hook` (auto =
debug/debugoptimized only), `PAINTBRUSH_TEST_HOOK` + `!NDEBUG` in
`pb_test_hook_gate.h`, and no controller hook-up in a release build. `PB_TEST_HOOK`
alone cannot enable it in a release binary. Build note:
`paintbrush/docs/LAMA_ONNX.md` → "Test hook build gate".

Window capture used the mandated
`pwsh scripts\pb-capture-live.ps1 -ProcessId <pid> -OutFile <png>` (PrintWindow,
`PW_RENDERFULLCONTENT`, no cursor). PrintWindow returns the app chrome but not the
GPU/video canvas surface, so the canvas evidence below is exported from the real document
renderer. Both kinds of files are kept.

Launch used `scripts\pb-test-launch.ps1`, which invokes the prescribed
`scripts\launch-pb-noskip.ps1 -Media <image> -NoAutosave -SamModel <ggml> -SamDevice gpu
-LogFile <log>` with the hook environment set. Because `--paintbrush-open` routes the PNG
to the media player (status becomes `Playing live_frame_B.png` with a blank 1280x720
canvas), the test then called the real controller `openImageFile` to load the image. That
is a launch-path gap, not a LaMa or canvas failure; it is recorded, not fixed, in
`KNOWN-ISSUE-paintbrush-open-png.md`.

## Recovery safety

Backed up and isolated with `scripts\pb-recovery-guard.ps1` before every run, restored
after every run. Final hashes are the expected ones and both files are present; no
`.bak-test` remains.

- `C:\Users\lukes.COREI9\AppData\Roaming\nai-paintbrush\paintbrush\recovery.npb`
  - 6048 bytes, SHA-256 `715313169C28D85C2798E86F7E3C1885BE3538BA3280E66422D6297A078A91AB`
- `C:\Users\lukes.COREI9\AppData\Roaming\paintbrush\paintbrush\recovery.npb`
  - 6057 bytes, SHA-256 `856E9E49A315AAB5BCCF5C51027A9C7CE8C883439AF91AB6449B9ED005D3809D`
- Backup dir: `C:\Users\lukes.COREI9\AppData\Local\Temp\kilo\pb-recovery-backup`

## PASS/FAIL matrix

| # | Acceptance item | Result | Evidence |
|---|---|---|---|
| 1 | Launch real app, wait for image, capture BEFORE, record pid/window | **PASS (note)** | `gate-before-remove.png`, `gate-before-remove-canvas.png`; pid 48548, hwnd 3412748, window 3810x1830; launch-path caveat: `KNOWN-ISSUE-paintbrush-open-png.md` |
| 2 | Remove object on real SAM3 mask; mask change / unmasked preservation; undo/redo | **PASS** | `gate-after-remove-canvas.png`, `gate-remove-layer.png`, `gate-remove-diff.png`, undo/redo canvases |
| 3 | Empty-prompt Fill on real rectangular selection; only selection changes; undo/redo | **PASS** | `gate-before-fill-canvas.png`, `gate-after-fill-canvas.png`, `gate-fill-diff.png`, undo/redo canvases |
| 4 | Cancellation while busy; no late commit; second fill only publisher | **PASS** (stale status text fixed post-gate) | `gate-after-stale-isolation-canvas.png`, `hook-gpu3` run, commit `13ac2c2453` |
| 5 | CPU fallback with `PB_LAMA_DEVICE=cpu`; small fill timing and success | **PASS** | pid 33556, `CPUExecutionProvider`, 9310 ms, `gate-cpu-*-canvas.png` |
| 6 | Save project and reopen; LaMa layer/result survives | **PASS** | `gate-project.npb`, saved / reopened / reopened-in-new-process canvases |
| 7 | Quality: seams/blur/512-crop limits reported honestly | **PASS (limits)** | remove and fill crops, boundary crops |

## Run details

### 1. Launch and BEFORE

- GPU run: pid 48548, window hwnd 3412748, 3810x1830.
- After `open_image`: 960x540 canvas, layers `Layer 1` + `live_frame_B`.
- Before canvas SHA-256 `11DB0C47A97E70C5213FACDDCBBB79C728E02C2FE87C261F964495C5EC887494`.
- Note: PrintWindow shows chrome but not the GPU canvas; exact canvas PNG is the pixel authority.

### 2. Remove object (real `live_mask_B.png`)

- Mask applied through the test-only controller hook; `objectSelected=true`, bounds `960 x 529`.
- Layers 2 → 3; wait 7923 ms; status `AI result added as a new layer`.
- Layer count after undo 2; after redo 3.
- Hashes:
  - before `11DB0C47…C887494`
  - after `0A0E5975…C4C87F`
  - undo `11DB0C47…C887494` (exact restore)
  - redo `0A0E5975…C4C87F` (exact republish)
  - generated layer `0A0E5975…C4C87F`
- Pixel diff against the mask:
  - outside mask: changed 0, max delta 0, mean 0
  - inside mask: 246,132 changed / 246,137 covered, max 231, mean 40.756
  - bbox `0,0,960,529`
- Evidence: `gate-mask-applied.png`, `gate-after-remove.png`, `gate-after-remove-canvas.png`,
  `gate-remove-layer.png`, `gate-remove-diff.png`, `gate-remove-crop-{before,after}.png`,
  `gate-remove-boundary-{before,after}.png`, `gate-remove-{undo,redo}-canvas.png`.

### 3. Empty-prompt Fill (GPU)

- Rect `620,40,260,100`; layers 3 → 4; status success.
- Before `0A0E5975…C4C87F`; after `0F5053A1…B9297452`; undo exact before; redo exact after.
- Outside rect: changed 0, max delta 0. Inside: 25,634 changed / 26,000, max 124, mean 10.0568.
- Evidence: `gate-before-fill{,-canvas}.png`, `gate-after-fill{,-canvas}.png`,
  `gate-fill-diff.png`, `gate-fill-crop-{before,after}.png`,
  `gate-fill-{undo,redo}-canvas.png`.

### 4. Cancellation and stale isolation (GPU, 5-layer project)

- Fill A started, genuinely busy, cancelled after 104 ms.
- Layers: 5 at cancel, 5 after 1.5 s grace (no late commit).
- Fill B completed in 5164 ms; layers 6; after 5 s grace still 6 (no stale A result).
- Canvas after `A7785688…FD5070EB`.
- At gate time the status text still read `Generating fill…` after cancellation even though
  busy was false; no result was committed (the PASS data above is the observed run).
- Fixed post-gate in `13ac2c2453`: `PaintbrushController::cancelGeneration()` now sets
  `Generation cancelled.` and still performs no document mutation. Result/cancel/stale
  isolation behavior is unchanged.

### 5. CPU fallback

- Relaunched with `PB_LAMA_DEVICE=cpu`; pid 33556; backend `CPUExecutionProvider`.
- Rect `600,60,160,100`; one fill in 9310 ms; layers 2 → 3; success.
- Before `11DB0C47…C887494`; after `4901BBAD…AA7E621C`.
- Outside rect: changed 0, max delta 0. Inside: 15,741 changed / 16,000, max 116, mean 8.4843.
- Evidence: `gate-cpu-before-fill{,-canvas}.png`, `gate-cpu-after-fill{,-canvas}.png`,
  `gate-cpu-fill-diff.png`, `gate-cpu-fill-crop-{before,after}.png`.

### 6. Persistence

- `gate-project.npb`: 2,825,561 bytes, SHA-256 `FC2BD84C16B627E162C81AC4F2CCE359C381C24B7103128FE4F48CC095F76625`.
- 5 layers: raster, image, and three `LAYER_GENERATED` AI result layers.
- Saved canvas `65ED8AFA…C3813B6`.
- Reopened in the same process: same 5 layers, exact same canvas hash.
- Reopened in a new process (pid 2208): same 5 layers, exact same canvas hash.
- Evidence: `gate-project.npb`, `gate-project-saved-canvas.png`,
  `gate-project-reopened-canvas.png`, `gate-project-reopened-process2.png`.

### 7. Quality (honest limits)

- The supplied `live_mask_B.png` is a large wall/background region, not a compact object:
  it covers 246,137 / 518,400 pixels (47.5%) and leaves the dancers outside the mask.
- Remove result: unmasked pixels are byte-identical (outside delta 0), and there is no
  hard color seam. The reconstructed wall loses detail: ghosted/smeared fixture shapes and
  low-frequency banding. A 960x529 hole is far beyond the 512x512 LaMa context ROI, so this
  detail loss is the expected large-region limitation, not a preservation failure.
- GPU fill: the light strip inside the 260x100 rectangle is removed cleanly; the replacement
  wall is smooth with slight banding and no hard seam.
- CPU fill: similar clean wall replacement; slightly softer/low-frequency result; no hard seam.
- No generative invention beyond the LaMa model was observed.

### 8. Phase timing (native runtime)

Measured with the rebuilt native runtime (MSVC `pb_lama_cuda.dll` 2026-11-24 20:32:42,
SHA-256 `F62893D0…5D92F94A`) and the `lama_smoke` harness, not by the GUI. The packaged
acceptance DLL (`e0325cf2…d36fdd5`) was left untouched; the smoke ran from a staging copy
with the packaged ORT/CUDA/cuDNN closure. Model SHA verified:
`1faef530…4eb68d6`. Both runs: `RESULT PASS (5 checks)`, unmasked pixels 0 bad, alpha 0 bad.

Keys (milliseconds, `std::chrono::steady_clock`): `load` = persistent session/model graph load;
`mask` = source setup + mask scan + component/pass planning; `preprocess` = ROI crop +
dilation + 512 resampling; `infer` = `Ort::Session::Run`; `composite` = decode + feather +
write-back; `total` = DLL-side total. `publish` (RGBA -> QImage + queued handoff) is
instrumented in `pb_lama_cuda_shim.cpp` / `pb_lama_service.cpp` but **unmeasured here** —
it requires the MinGW plugin build, which the coordinator owns.

GPU (`CUDAExecutionProvider`, RTX 4090, ORT 1.30.0, session load 8807.6 ms):

| case | mask | preprocess | infer | composite | total |
|---|---:|---:|---:|---:|---:|
| centre 640x480 | 0.8 | 6.1 | 668.9 | 1.3 | 677.1 |
| boundary 480x360 | 0.1 | 3.5 | 90.3 | 0.9 | 94.9 |
| disconnected 800x420 | 0.5 | 10.5 | 181.2 | 1.3 | 193.6 |
| tiny 320x240 | 0.0 | 5.8 | 97.5 | 0.7 | 104.1 |
| large 720x540 | 1.6 | 5.3 | 75.6 | 4.5 | 87.0 |

CPU (`CPUExecutionProvider`, same host, session load 9258.4 ms):

| case | mask | preprocess | infer | composite | total |
|---|---:|---:|---:|---:|---:|
| centre 640x480 | 0.5 | 6.2 | 1569.7 | 1.3 | 1577.6 |
| boundary 480x360 | 0.1 | 4.0 | 1716.7 | 0.9 | 1721.7 |
| disconnected 800x420 | 0.4 | 7.6 | 3060.4 | 2.0 | 3070.4 |
| tiny 320x240 | 0.0 | 3.4 | 1714.8 | 0.4 | 1718.7 |
| large 720x540 | 2.2 | 8.5 | 1733.8 | 7.8 | 1752.3 |

Evidence: `phase-timing-gpu.log`, `phase-timing-cpu.log` in this directory. The GUI worker
log line carries the same keys plus `publish`:
`LaMa inpaint ready: … load=… mask=… preprocess=… infer=… composite=… publish=… total=…`.

## Commit provenance

- LaMa provider (native ONNX Runtime path): `cd34efb9bcc56f99f05ff21ce2a7ab7a05c063c9`
  — "paintbrush: add native LaMa ONNX inpaint provider".
- Test hook + cancellation-status fix: `13ac2c245347902739371e018aff351c85d16ba3`
  — "paintbrush: add non-blocking LaMa UI acceptance hook".
- The gate matrix above ran on `cd34efb9bc` with the hook present as uncommitted work;
  `13ac2c2453` is the commit that captured the hook and the status fix. HEAD is
  `13ac2c2453`.
- Post-gate bookkeeping (this file, `results.json`, phase timing, compile-time hook gate,
  known-issue note) is **uncommitted**.

## Scope / files

Committed acceptance surface (`13ac2c2453`):

- `modules/gui/qt/paintbrush/pb_test_hook.{hpp,cpp}`
- `modules/gui/qt/paintbrush/pb_controller.{hpp,cpp}` (`testApplyObjectMask`,
  `testLamaBackend`, hook lifecycle, `cancelGeneration` status)
- `modules/gui/qt/meson.build` (hook source + moc header)
- `scripts/pb-hook-client.ps1`, `scripts/pb-test-launch.ps1`,
  `scripts/pb-recovery-guard.ps1`, `scripts/pb-image-diff.ps1`

Uncommitted post-gate bookkeeping:

- `gpu/lama/lama_runtime.{h,cpp}`, `gpu/pb_lama.h`, `gpu/pb_lama_cuda_shim.cpp`,
  `pb_lama_service.{hpp,cpp}`, `gpu/lama/tools/lama_smoke.cpp` (phase timing)
- `pb_test_hook_gate.h` (new), `pb_test_hook.{hpp,cpp}`, `pb_controller.{hpp,cpp}`,
  `modules/gui/qt/meson.build`, `meson_options.txt` (compile-time hook gate)
- `paintbrush/docs/LAMA_ONNX.md` (timing keys + build gate note)
- `_REVIEW/lama-ui-acceptance/`: `phase-timing-{gpu,cpu}.log`, updated `REPORT.md` /
  `results.json`, `KNOWN-ISSUE-paintbrush-open-png.md`

Evidence directory: `_REVIEW\lama-ui-acceptance\` (`gate-*` files, `phase-timing-*.log`,
`results.json`, this report). Older files in that directory are from the previous attempt
and were not used.

No commit, stage, or push was performed for this bookkeeping change. HEAD is
`13ac2c245347902739371e018aff351c85d16ba3`; the working tree also contains unrelated
concurrent work that is not part of this LaMa change.

## Post-gate fix (observed failure)

Cancellation left the status text as `Generating fill…` while `generationBusy` was false.
`PaintbrushController::cancelGeneration()` now sets `Generation cancelled.` and still
performs no document mutation (commit `13ac2c2453`). Rebuilt with
`scripts\pb-build-plugin.ps1` → `NINJA_EXIT=0`, `plugins.dat` 2026-11-24 20:11:56.
The result/cancel/stale-isolation behavior is unchanged; this only clears the stale status
line.
