# NAI Paintbrush — HANDOFF (the lamp)

> Durable continuation state. **This is the only authoritative lamp.**
> Read this first after a context reset.
>
> Contract: `docs/ADAPTIVE_UI_AND_SUBJECTS.md` (partially implemented; its
> status line lists what is real and what is still pending).
>
> Superseded, for history only: `docs/LAMP.md` and `../HANDOFF.md` (banners at
> their tops). Do not act on either of them.

## 1. Repository / build

- Repo: `D:\_SATIN_AI\NAI-OS\vlc`, branch `master`.
- Build dir: `win64-meson` (native MSYS2/MINGW64 Meson build).
- **Code commits:** `453f11e913` — settings persistence/Apply, subject
  place/delete undo, QML contract guard. `c03ecc0bd1` — video presentation
  state (vout/track subscriptions). `317ca0ad1b` — toolbar theme resolution,
  VLCIcons glyph fallback, and capability refresh. `b11892c597` — capture/
  compose keeps the decoded keyframe instead of the black viewer snapshot.
  `452ac7e835` — lazy SAM load on first segmentation (no 706 MB startup
  stall). `01112f6900` — settings tracking status guarded before the service
  is ready. `793fc42d7b` — native `LAYER_VECTOR` + in-canvas node editing
  (plus the `samGpuBackendLinked` / Windows CPU-backend compile fixes).
  `12cc2fb37b` — powder subtool capabilities; invariant 8 complete.
  This lamp is the docs commit that contains this file.
- **Verified build (2026-11-22 00:55):**
  `ninja -C win64-meson modules/libqt_plugin.dll` → `EXIT=0`.
  Artifact: `win64-meson/modules/libqt_plugin.dll`,
  `22.11.2026 00:55:09`, 403,750,744 bytes. QML resource
  `paintbrush_assets.cpp` regenerated `00:54:01`.
- **Provenance:** app code is committed at `12cc2fb37b`; the temporary
  end-to-end QML probe was removed before the build. Four launch helper scripts
  (`scripts/launch-pb-noskip.ps1`, `launch-pb-sam.ps1`,
  `launch-pb-software-decode.ps1`, `run-paintbrush.ps1`) are modified by
  another workstream and remain uncommitted/not staged. The DLL was linked from
  the committed app code; the docs commit after it does not change the plugin.
- Running VLC from `C:\Program Files\VideoLAN` does not lock the build tree.
  Only a VLC/Paintbrush launched from `win64-meson` locks its DLLs.

Build:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:PATH
ninja -C win64-meson modules/libqt_plugin.dll
```

Full build + plugin cache:

```powershell
ninja -C win64-meson
```

Probes (actual result, not historical):

```powershell
modules\gui\qt\paintbrush\probes\run-probes.ps1
```

```
check-qml-contracts          PASS (271 controller members declared)
check-palettes               PASS (27 palettes, 27 unique ids)
ProbeCanvasAttach            PASS
ProbeModelBinding            PASS
5/5 probes passed
EXIT=0
```

Launch:

```powershell
scripts\launch-pb-noskip.ps1
```

QML files are listed **explicitly** in `modules/gui/qt/meson.build`. A new QML
file must be added there or it will not exist at runtime even though the DLL
links. `PaintbrushSettingsDialog.qml` is already registered (line 1042 at the
time of writing).

## 2. P0 — video presentation (native surface fixed; decoded preview conditional)

Symptom: video plays (transport advances, thumbnails decode) and the paintbrush
canvas area stays empty.

### 2.1 Root cause and fix (committed `c03ecc0bd1`)

`openVideoFile()` calls `enterHarvestPresentation()`, appends the media, and
emits `mediaStateChanged()` **immediately** — before the input is parsed and
before the vout starts. `PaintbrushController` subscribed to neither
`PlayerController::hasVideoOutputChanged` nor the video track list, so
`playerHasVideo` (`mediaHasVideoTrack() && videoOutputAttached()`) stayed false.
The QML `VideoSurface.visible`/`renderingEnabled` binding therefore stayed
false, the checkerboard stayed visible, and the surface never reported its
`mapToScene` geometry to `MainCtx.videoSurfaceProvider`. The native vout was
playing with no QML anchor — exactly “transport advances, canvas empty”.

Fix:

- subscribe to `PlayerController::hasVideoOutputChanged`,
  `PlayerController::voutListChanged`, and the video
  `TrackListModel::countChanged`;
- re-emit `mediaStateChanged()` on every real transition;
- log `presentation: player video state: hasVideoOutput=… playerHasVideo=…`
  and `presentation: video track count=… playerHasVideo=…` for native diagnosis.

What now happens:

- when the vout and track list actually arrive, `playerHasVideo` flips true;
- the QML `VideoSurface` becomes visible, `VideoSurface::synchronize()` reports
  its scene geometry, and the compositor moves the existing native visual/window
  to the canvas rect;
- the checkerboard hides and `setVideoHarvestMode(true)` keeps document tiles
  pruned, so the QML canvas area is transparent over the native video;
- no second renderer, no surface removal, no blocking frame request.

Runtime smoke on the built app (2026-11-21 10:38, `1.mp4` as a positional
input) confirmed the two transitions:

```
paintbrush: video track count=1 playerHasVideo=0
paintbrush: player video state: hasVideoOutput=1 playerHasVideo=1
```

The first line is the old failure state (track known, gate still false); the
second proves the new vout notification flips the gate. The temp canvas probe
is gone from the committed worktree; **visual native acceptance is pending**.

Code-level compositor notes (the existing infrastructure this uses):

- DComp adds the video visual **below** the Qt UI visual
  (`compositor_dcomp.cpp:68`); the QML scene must be transparent at the canvas.
- Win7 positions a native video window and applies pending geometry on commit.
- The paintbrush is hosted in the main window's QML scene
  (`MainInterface.qml:321-329`), not a separate window.
- `PaintbrushWorkspace`/`PaintbrushCanvasView` roots have no opaque background;
  the checkerboard is the only opaque canvas layer and hides in HarvestVideo
  when `playerHasVideo` is true. `setVideoHarvestMode(true)` prunes tiles
  (`pb_canvas_item.cpp:2060-2075`, `3966-3976`).

If native acceptance still fails, capture the runtime facts before changing the
architecture: compositor mode (DComp/Win7), `MainCtx.hasEmbededVideo`, the
native vout window/visual rect and visibility, `VideoSurface` geometry
(`mapToScene`), `presentationMode`, `videoHarvestMode`, and the window clear
colour. The fix above is upstream of the compositor and is common to both
Windows paths.

### 2.2 Capture/compose path (fixed `b11892c597`)

The live surface was not the only failure. “Add frame to canvas” still produced
a dark canvas because:

- `DecodedFrame::ok()` requires `ptsVerified`;
- the shared thumbnailer can only return the nearest keyframe, so the capture
  path discarded a valid keyframe;
- it then fell through to `FrameCaptureService::captureCurrentFrame()`, which
  returns a black image on the native Windows vout;
- `insertCapturedFrame()` entered Compose and stopped playback around that
  black frame.

Fix:

- require only a non-null image in `onAddFrameDecoded()`;
- insert an unverified keyframe with its REAL PTS and mark it unverified in the
  status/provenance;
- force the frame to `QImage::Format_RGB32` before the layer conversion so an
  alpha channel cannot make it invisible;
- remove the black viewer-snapshot fallback; a genuine decode failure keeps
  `HarvestVideo` alive and reports the error instead of presenting a black
  canvas;
- logs: `capture: request ...`, `capture: decoded requested=... actual=...
  verified=...`, `capture: inserted size=... format=... blank=... layers=...
  canvas=... verified=...`, `capture: decode failed ...`.

The exact-frame trade-off: a keyframe may be visually different from the
requested PTS; it is inserted with its actual PTS and marked unverified. The
strict `DecodedFrame::ok()` remains for thumbnail/tracking/export paths.

### 2.3 If a decoded-frame preview is required later

The native surface shows the video but is not a paintable QImage. Only if the
product needs painting/tracking over live video inside the document (not just
the native backdrop), use the existing asynchronous path:

- `EditFrameSource::requestAsync()` + request token;
- reject stale token results;
- `cancelInFlight()` / cancellation on mode switch, seek and close;
- at most one preview request in flight;
- throttle to 5–10 fps with a timer while harvesting and not painting;
- define a priority/backpressure policy against thumbnail batches and Object
  Select (they share the single thumbnailer thread). Do not blind-cancel their
  work.

**Never call `FrameProvider::request()` periodically from the GUI thread.** It
blocks the caller (up to 8 s) and is only safe for one-off actions
(`pb_media_logic.hpp:90-94`, `pb_frame_capture.hpp:87-90`).
`captureVideoFrame()` already uses the async pattern (`pb_controller.cpp:726-750`);
reuse it.

Render/interaction contract for that future preview:

- `setVideoHarvestMode(true)` deliberately prunes **all** document tiles.
  “Painted before the document tiles” is a no-op there; use a dedicated
  viewport-space QSG node (like `m_liveHarvestOverlay`) or explicitly change the
  tile-suppression contract.
- Define **look-only vs paintable** in `HarvestVideo`. If paintable, the canvas
  must render document tiles over the preview and the paint path must be tested.
- Reuse `buildSelectionTransforms` and
  `ObjectSelectionContext::itemToFrame/frameToItem` so Object Select prompt
  points cannot drift at zoom/pan.
- Call it the **decoded picture**, not an “encoded frame”; exactness is only
  defensible when paused and PTS-verified (`DecodedFrame::ok()`). While playing,
  the edit decoder is a separate software-only input and lags.

### 2.4 Acceptance

- the native video appears in the canvas after opening a video, at 100% and
  after zoom/pan (the surface rect is the canvas view);
- leaving/entering playback hides/shows the surface and checkerboard coherently;
- no GUI stall from video-state updates;
- “Add frame to canvas” inserts a visible decoded frame/keyframe; if the exact
  decoder genuinely fails, the live video remains and the status reports the
  error;
- Object Select mapping is untouched (same `buildSelectionTransforms` path);
- the committed worktree no longer contains the temporary
  `PaintbrushCanvasView.visible: false` / geometry probe; the canvas is visible.

## 3. Delivered and committed

| Area | Commits / files |
|---|---|
| **Settings persistence (repaired)** — `config_PutPsz`/`config_PutInt` + `config_SaveConfigFile`, read-back validation, empty-on-success, sticky restart state, SAM device normalization (`auto/gpu/cpu`, empty/unknown → auto), PTS clamp 20–2000 ms, API keys stay environment-only | `pb_controller.{hpp,cpp}`, `pb_common.{hpp,cpp}`, `pb_segmentation_service.cpp`, `pb_tracking_service.cpp`, `PaintbrushSettingsDialog.qml`; `453f11e913` |
| **Subject transactions (repaired)** — `placeSubject`/`deleteInstance` are one `CompositeCommand` covering both the layer command and the persisted `SubjectInstance` record; the instance id is captured in the `InsertLayerCommand` payload before it is pushed; deleting the only layer is refused instead of orphaning the record | `pb_controller_palettes.cpp`, `pb_history.hpp`, `pb_controller.{hpp,cpp}`; `453f11e913` |
| **QML contract guard (repaired)** — legitimate JS members such as `model.indexOf` are allowed; delegate `model.<role>` misuse is still flagged; actual gate 5/5 PASS | `probes/check-qml-contracts.ps1`; `453f11e913` |
| **Video presentation (native, fixed)** — root cause: one-shot `mediaStateChanged` before parsing, with no vout/track subscriptions; the controller now re-emits on `hasVideoOutputChanged`, `voutListChanged` and video track `countChanged`, logs the real state, and the QML `VideoSurface` becomes visible and reports geometry to the existing compositor. No renderer rewrite. | `pb_controller.cpp`; `c03ecc0bd1` |
| **Capture/compose (fixed)** — keep the nearest decoded keyframe when the exact PTS is unverified, with its real PTS; force RGB32; remove the black viewer-snapshot fallback; a genuine decode failure keeps HarvestVideo alive and reports the error. | `pb_controller.cpp`; `b11892c597` |
| **Native `LAYER_VECTOR` + in-canvas node editing** — local editable `QPainterPath` + stroke/fill/width; canonical renderer draws the path (preview raster is thumbnail-only); vector content bounds/transform; project-store round-trip; double-click arms the QML node overlay; each drag is one `VectorPathCommand` with undo/redo and preview refresh. | `pb_common.hpp`, `pb_layer.{hpp,cpp}`, `pb_document.cpp`, `pb_project_store.cpp`, `pb_controller.{hpp,cpp}`, `pb_history.{hpp,cpp}`, `pb_canvas_item.{hpp,cpp}`, `PaintbrushCanvasView.qml`; `793fc42d7b` |
| **Semantic tool capabilities (invariant 8 complete)** — object-select/size/flow/bristle/glitter/powder + powder-subtool + harvest-presentation capabilities; no QML capability decision uses a tool number. | `pb_controller.{hpp,cpp}`, QML; `36dd5725ea`, `12cc2fb37b` |
| **C++ media harness** — pure-Qt `media_logic_test` (FrameRef/DecodedFrame/PTS/LRU/priority); gate 5/5. | `tests/media_logic_test.cpp`, `probes/run-media-logic-tests.ps1`, `probes/run-probes.ps1`; `36dd5725ea` |
| **Stylus pressure** — existing tablet pressure path feeds `pressureFactor()`/bristle; precision toggle + pressure-to-opacity/min/max; native tablet acceptance pending. | `pb_canvas_item.{hpp,cpp}`, `bristle_brush.cpp`, `PaintbrushToolOptionsBar.qml` |
| Adaptive strip baseline (one 64dp surface, RowLayout, flex sliders, intrinsic icons, clipping) | `PaintbrushStrip.qml`, `PaintbrushToolbar.qml`, `PaintbrushToolOptionsBar.qml`, `PaintbrushWorkspace.qml`; commits `57469a8b71`…`7e8faff59e`; the one-surface/implicit-width refactor is committed at `317ca0ad1b`. |
| **Toolbar theme/icon repair** — explicit `VLCStyle.palette` on all 15 paintbrush `ColorContext`s; `Widgets.IconLabel` for VLCIcons glyph fallback (sets the icon font family); options bar refreshes on `toolCapabilitiesChanged`; canvas `toolName` + attach-time capability emission | `PaintbrushToolbar.qml`, `PaintbrushToolOptionsBar.qml`, all paintbrush `ColorContext` QML, `pb_canvas_item.hpp`, `pb_controller.cpp`; `317ca0ad1b` |
| Compact colour group: FG/BG chips, 8 recent selected colours, palette popover over all 27 persisted palettes | `PaintbrushStrip.qml`, `b971b9f847`, `7a38f8dd63` |
| Semantic capabilities: `toolUsesColor`, `toolHasPrimaryAction`, `toolIsBusy` (+ `toolCapabilitiesChanged`) | `pb_controller.{hpp,cpp}`, `43a8edc285` |
| Honest history semantics: `recentSelectedColors` | `pb_controller.{hpp,cpp}` |
| Keyboard-operable swatches (Button, Accessible.name/role, StrongFocus, focus ring) | `67cac3e041` |
| Palette store: 27 locked built-ins incl. Art Studio paper/stocks/18 directions, user palettes as JSON, import/export | `pb_palette_store.{hpp,cpp}`, `pb_controller_palettes.cpp`, `palettes/builtin-palettes.json`, `probes/sync-builtin-palettes.ps1` |
| Subject model: `SubjectAsset` / `SubjectInstance` / `DerivedArtwork`, JSON persistence, cut-out PNG | `pb_subject_store.{hpp,cpp}` |
| Object Select: layer-space segmentation (`SourceCanvasLayer`), mask baked to document space, Keep/cut-out use layer pixels | `pb_selection.hpp`, `pb_controller.cpp` |
| SAM: **lazy model load on first segmentation** (`452ac7e835`; worker idle at startup, no 706 MB eager launch stall), bounded 640px encode with mapped prompt points, encode-keyed cache, phase markers, **row-stride mask fix**, device ladder **auto/gpu/cpu with CPU fallback**; tracking now normalizes empty/unknown to auto and falls back to CPU for auto | `pb_segmentation_service.{hpp,cpp}`, `pb_tracking_service.cpp` |
| Progress: service `busyChanged`/`progressChanged` forwarded to QML, 3dp indeterminate rail + phase text; Windows taskbar progress via pimpl `ITaskbarList3`, queued to the GUI thread | `pb_controller.cpp`, `pb_taskbar.{hpp,cpp}`, `PaintbrushCanvasView.qml` |
| Placements: cut-out → persisted instance + real layer in one undo step, cascade offset, layer↔instance link, transform sync, restore on project open | `pb_controller_palettes.cpp`, `pb_controller.cpp`, `pb_layer.hpp` |
| Styling: `applySubjectStyle` (Outline/Glow/Ghost on the linked layer), `deriveArtwork` (`mode=raster`), `deriveContour` (selection `QPainterPath`, path persisted as `nai-paintbrush/contour/v1`, `mode=vector`) | `pb_controller_palettes.cpp` |
| Transactions: `UndoStack::beginComposite/endComposite` over `CompositeCommand`; layer edits plus subject place/delete/instance records | `pb_history.{hpp,cpp}`, `pb_controller_palettes.cpp` |
| Layers: atomic visibility change (forced `update()` + model reload), drag-to-reorder `moveLayerTo` + `DragHandler` | `pb_controller.cpp`, `PaintbrushInspectorDock.qml` |
| Thumbnails: nearest-keyframe acceptance (thumbnailer only returns keyframes), `modelData.*` delegate access | `pb_thumbnail_service.cpp`, `PaintbrushMediaDrawer.qml` |
| Frame placement: centre the **unscaled** raster (layer transform scales about raster centre) | `pb_controller.cpp` |
| Metadata: structured values round-trip (vector path was silently dropped by the scalar-only switch) | `pb_project_store.cpp` |
| Probes/guards: `ProbeModelBinding`, `ProbeCanvasAttach`, `check-qml-contracts`, `check-palettes`, runner. **Coverage is UI-contract only** — no C++ behavior harness yet. | `probes/` |

## 4. Invariants — do not reopen

1. `SubjectAsset` holds source identity + track ids only. **No transform
   fields**; placement lives on `SubjectInstance`.
2. `ObjectSelectionContext` is transient; never persist it as a Subject.
3. Masks are authoritative in the space they were produced; display copies are
   derived.
4. Segmentation failures must never produce a "ready" subject.
5. Palette/subject/instance mutators return an **empty string on success**,
   otherwise a readable reason.
6. Source media is never destructively rewritten.
7. A raster artefact is never labelled `vector`; vector means stored, editable
   path geometry.
8. **Enforced.** QML consumes capability properties for capability decisions:
   `toolUsesColor`, `toolHasPrimaryAction`, `toolIsBusy`,
   `toolIsObjectSelect`, `toolHasSizeParameter/Flow/Bristle/Glitter/Powder`,
   `powderToolIsGlue/Glitter/Push/Eraser`, and `isHarvestPresentation`. The
   only remaining numeric comparisons are the tool rail’s active-tool identity
   (`canvas.tool === src.toolId`) and action payload values that set a subtool;
   neither is a capability decision.
9. History is history: recent colours are *selected* colours, not palettes and
   not painted-with.
10. Settings persistence uses `config_PutPsz`/`config_PutInt` (VLC config,
    marked dirty) + `config_SaveConfigFile`. `var_Set*` is runtime-only and
    never persists. Apply returns an **empty string on success**;
    restart-required is separate sticky state, not a failure message. Secrets
    stay in environment variables; the settings dialog never stores or displays
    a key.
11. Video visibility is driven by the **real vout/track state**, never by the
    open call alone. `playerHasVideo` must be re-evaluated when
    `hasVideoOutputChanged`, `voutListChanged` or the video track count changes;
    a one-shot `mediaStateChanged()` is not enough.
12. A captured frame is never silently replaced by an unverified viewer
    snapshot. Use the decoded keyframe with its REAL PTS and label it
    unverified; a real decode failure leaves the live video visible and reports
    the error.
13. Vector layers are path-native. `LAYER_VECTOR` renders `vectorPath` through
    the canonical compositor; the raster is a thumbnail preview only. Node
    edits are one undoable `VectorPathCommand` and serialize with the project.

## 5. Known limitations / unverified

- **Settings**: code repaired and compiled. **Native acceptance is pending**:
  Apply closes the dialog; values survive an application restart; device
  normalization and PTS range behave; a restart notice persists.
- **Subject undo**: code repaired and compiled. **Native acceptance is
  pending**: place → undo/redo, delete → undo/redo, project save/reload after
  undo, only-layer refusal.
- **Video presentation**: root cause fixed in `c03ecc0bd1`; build green.
  Runtime smoke on the built app (10:38) confirmed
  `video track count=1 playerHasVideo=0` followed by
  `player video state: hasVideoOutput=1 playerHasVideo=1`.
  **Visual native acceptance pending** — the temp canvas probe is gone from the
  committed worktree; the canvas view is visible. The decoded-frame preview is
  not implemented and is only needed if painting over live video requires a
  QImage.
- **Toolbar theme/icons**: repaired at `317ca0ad1b`; build green. **Visual
  native acceptance pending** (magenta `ColorContext` fallback, hollow VLCIcons
  glyphs, capability-driven colour/Keep group).
- **Capture/compose**: fixed at `b11892c597`; build green. The exact decoder’s
  nearest keyframe is inserted with its real PTS and marked unverified; the
  black viewer-snapshot fallback is gone. **Native visual acceptance pending**:
  “Add frame” should show a frame; if the decode genuinely fails,
  `capture: decode failed` is logged and the live video remains.
- **Video compositor evidence**: the fix was derived from the QML/provider
  signal graph (common to DComp/Win7); the actual runtime mode/rects were not
  captured in this session. If acceptance still fails, capture the §2.1
  evidence before changing the renderer.
- **Invariant 8**: enforced (§4.8); only active-tool identity and action
  payload values remain numeric.
- **Probe coverage**: 2 QML probes + 2 static checks + the C++ media harness;
  gate 5/5. Settings/undo paths still lack a dedicated controller-level test.
- **Native vector/node/stylus acceptance**: native `LAYER_VECTOR`, the
  double-click node editor, and the tablet-pressure path are code-complete;
  verify them on the target tablet/display. Vector node undo/redo and project
  round-trip need the native pass.
- **Startup responsiveness**: the eager SAM preload was removed in
  `452ac7e835`; the worker waits idle and the 706 MB model loads on the first
  segmentation request. This removes the startup “Loading SAM model…” stall
  that starved the workspace. **Native acceptance pending.**
- **Worktree**: app code committed at `12cc2fb37b`; four launch helper scripts
  and the concurrent docs (`DEMO.md`, root/superseded handoffs) are modified by
  another workstream and uncommitted (not staged). Verify in the built app.
- **Store/document atomicity**: the composite makes undo/redo coherent across
  the layer and the store, but they are separate files with no cross-file crash
  journal. A process crash between the two writes can leave a mismatch; store
  I/O failure during undo/redo is logged (`qWarning`), not rolled back.
- **Build warnings**: pre-existing warnings remain in unrelated code (for
  example the thumbnail log format string and glitter unused parameters). The
  repaired translation units compile without new code warnings.
- **Layer show/hide**: implemented (`545746ab4b`). Awaiting runtime
  confirmation. If it fails, the `layers` log line
  `setLayerVisible: index=… currentBefore=…` distinguishes a wrong row index
  from a toggle that never reaches the controller.
- **Strip appearance**: baseline structurally complete; the implicit-width
  refactor is committed at `317ca0ad1b`. **Visual acceptance pending.**
- **`sam3_params` has only `use_gpu`** — no adapter index, no VRAM query. NAI
  cannot choose the dGPU by name; a per-application driver preference is still
  required.
- **Taskbar progress** is wired but not runtime-verified.
- **API keys**: environment-only (`PAINTBRUSH_VLM_API_KEY`,
  `PAINTBRUSH_IMAGE_API_KEY`, `PAINTBRUSH_ASSISTANT_API_KEY`). No plaintext
  credential storage is supported by this dialog. If persistent secrets are
  required, use OS secure storage, not `vlcrc`.

## 6. Uncommitted work / next owner

- **The audit repair is committed at `453f11e913`; the video-state fix is
  committed at `c03ecc0bd1`; the toolbar theme/icon fix is committed at
  `317ca0ad1b`; the capture keyframe fix is committed at `b11892c597`; the
  lazy SAM fix at `452ac7e835`; the settings guard at `01112f6900`; the native
  vector layer/node editor at `793fc42d7b`; the powder capabilities at
  `12cc2fb37b`.**
- No app files from this work are left uncommitted; app code is at
  `12cc2fb37b`. The four modified launch helper scripts and concurrent docs
  belong to another workstream and are not staged.
- **Do not** add a plaintext API-key field or a `var_SetString`-based setter.
  The supported secret path is the environment, read by `pbApiKey()`.
- For native verification, use the built app; the app is the UI gate.

## 7. Next actions, in order

1. **Native visual acceptance** of the toolbar, live video presentation,
   “Add frame”/compose, settings/undo, the native vector layer + double-click
   node editor, and tablet pressure in the built app.
2. If live video still fails, capture the §2.1 runtime evidence (mode, native
   rect, surface geometry, `hasEmbededVideo`); do not start with a renderer
   rewrite. If “Add frame” fails, read `capture: decoded/inserted/decode failed`
   before changing anything; a nearest keyframe with its real PTS is an
   acceptable unverified capture.
3. Implement a decoded-frame preview only if painting/tracking over live video
   requires a QImage; use `requestAsync` + token + cancellation.
4. Runtime-confirm layer show/hide and strip visuals.
5. Extend the C++ harness to settings-Apply/subject-undo controller contracts
   (the pure media harness and invariant-8 gate are complete).
6. Remaining ADR refinement: richer bezier-handle node UX (native path editing
   is live) and any future decoded preview.

## 8. Diagnostics cheat-sheet

`pb_noskip.log` categories:

- `segmentation`: `queued`, `encode START/DONE/FAILED/reused`, `mask ready`,
  `error`, `model: load START/DONE/FAILED (tier=… use_gpu=…)` — the model load
  now starts on the first segmentation request, not at launch.
- `objectselect`: click route, `layer route`, `compose route`
- `layers`: `setLayerVisible: index=… visible=… currentBefore=…`
- `media`: thumbnail slot latency / nearest-keyframe delta
- `presentation`: `setCanvasItem: attached WxH, harvestMode=…`, mode switches,
  `player video state: hasVideoOutput=… playerHasVideo=…`,
  `video track count=… playerHasVideo=…`
- `capture`: request mrl/pts, decoded requested/actual/verified/size/format,
  inserted size/format/blank/layers/canvas/verified, decode failed
- `PB_QML`: attachCheck geometry, statusText, place/style/derive errors

## 9. Build / most-recent evidence

```
ninja -C win64-meson modules/libqt_plugin.dll   EXIT=0 (vector/powder objects + link
                                                00:55:09)
modules/libqt_plugin.dll                        22.11.2026 00:55:09, 403,750,744 bytes
QML resource paintbrush_assets.cpp              regenerated 00:54:01
probes                                          check-qml-contracts PASS (271),
                                                check-palettes PASS (27),
                                                5/5 probes passed, EXIT=0
                                                (after 12cc2fb37b)
code repair commit                              453f11e913
video-state commit                              c03ecc0bd1
toolbar theme/icon commit                       317ca0ad1b
capture keyframe commit                         b11892c597
lazy SAM commit                                 452ac7e835
settings status guard commit                    01112f6900
native vector layer/node editor commit          793fc42d7b
powder capabilities commit                      12cc2fb37b
runtime smoke (built app, 1.mp4, 10:38)         video track count=1 playerHasVideo=0
                                                player video state: hasVideoOutput=1
                                                playerHasVideo=1
app load check (after 793fc42d7b)                workspace loaded, setCanvasItem
                                                logged, no QML load errors in the
                                                app log; overlay hidden by default
capture UI smoke                                temporary QML timer probe did not
                                                fire in the automated workspace;
                                                probe removed; native UI acceptance
                                                pending
HEAD                                            this docs commit
worktree                                        app code 12cc2fb37b; four launch
                                                scripts + concurrent docs modified
                                                by another workstream (not staged)
```

## 10. Superseded documents

- `docs/LAMP.md` — superseded 2026-11-21. Its last verified commit was
  `8e2a6eadc5` (more than 30 commits behind the repair commit) and its
  “settings rewritten” claim was contradicted by the `var_SetString` no-op.
  Kept for history only.
- `../HANDOFF.md` — 2026-10-22 Phase 5 handoff. Superseded 2026-11-21. Kept for
  history only.
- `docs/ADAPTIVE_UI_AND_SUBJECTS.md` — the contract; its status line now
  distinguishes implemented features from pending work.
