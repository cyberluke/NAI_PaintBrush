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
  state (vout/track subscriptions). This lamp is the docs commit that contains
  this file.
- **Verified build (2026-11-21 10:30):**
  `ninja -C win64-meson modules/libqt_plugin.dll` → `EXIT=0` (incremental:
  controller object + link). Artifact:
  `win64-meson/modules/libqt_plugin.dll`, `21.11.2026 10:30:51`,
  417,223,436 bytes.
- **Provenance caveat:** the build ran against `453f11e913` + `c03ecc0bd1`
  **plus** concurrent uncommitted toolbar/canvas work in the same worktree
  (`pb_canvas_item.hpp`, `PaintbrushToolbar.qml`,
  `PaintbrushToolOptionsBar.qml`, `PaintbrushStrip.qml`,
  `PaintbrushWorkspace.qml`, and untracked `scripts/pb-capture-window.ps1`).
  That worktree also contains a temporary
  `PaintbrushCanvasView.visible: false` and a red geometry probe in
  `PaintbrushWorkspace.qml`, plus an uncommitted `toolCapabilitiesChanged`
  emission in `pb_controller.cpp` from the concurrent toolbar session. Those
  probes hide the canvas (and the `VideoSurface` inside it) in the built DLL.
  They are not part of this fix and were not staged. The DLL is a worktree
  build, **not** a clean-HEAD artifact; once the concurrent work is committed
  and the probe removed, rebuild.
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
check-qml-contracts          PASS (264 controller members declared)
check-palettes               PASS (27 palettes, 27 unique ids)
ProbeCanvasAttach            PASS
ProbeModelBinding            PASS
2/2 probes passed
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
second proves the new vout notification flips the gate. Visual acceptance still
depends on the concurrent `visible:false` probe being removed (§5).

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

### 2.2 If a decoded-frame preview is required later

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

### 2.3 Acceptance

- the native video appears in the canvas after opening a video, at 100% and
  after zoom/pan (the surface rect is the canvas view);
- leaving/entering playback hides/shows the surface and checkerboard coherently;
- no GUI stall from video-state updates;
- Object Select mapping is untouched (same `buildSelectionTransforms` path);
- **the concurrent `PaintbrushCanvasView.visible: false` temp probe must be
  removed by its owner first** (see §5); otherwise the canvas and the
  `VideoSurface` inside it are hidden in the built DLL.

## 3. Delivered and committed

| Area | Commits / files |
|---|---|
| **Settings persistence (repaired)** — `config_PutPsz`/`config_PutInt` + `config_SaveConfigFile`, read-back validation, empty-on-success, sticky restart state, SAM device normalization (`auto/gpu/cpu`, empty/unknown → auto), PTS clamp 20–2000 ms, API keys stay environment-only | `pb_controller.{hpp,cpp}`, `pb_common.{hpp,cpp}`, `pb_segmentation_service.cpp`, `pb_tracking_service.cpp`, `PaintbrushSettingsDialog.qml`; `453f11e913` |
| **Subject transactions (repaired)** — `placeSubject`/`deleteInstance` are one `CompositeCommand` covering both the layer command and the persisted `SubjectInstance` record; the instance id is captured in the `InsertLayerCommand` payload before it is pushed; deleting the only layer is refused instead of orphaning the record | `pb_controller_palettes.cpp`, `pb_history.hpp`, `pb_controller.{hpp,cpp}`; `453f11e913` |
| **QML contract guard (repaired)** — legitimate JS members such as `model.indexOf` are allowed; delegate `model.<role>` misuse is still flagged; actual gate 2/2 PASS | `probes/check-qml-contracts.ps1`; `453f11e913` |
| **Video presentation (native, fixed)** — root cause: one-shot `mediaStateChanged` before parsing, with no vout/track subscriptions; the controller now re-emits on `hasVideoOutputChanged`, `voutListChanged` and video track `countChanged`, logs the real state, and the QML `VideoSurface` becomes visible and reports geometry to the existing compositor. No renderer rewrite. | `pb_controller.cpp`; `c03ecc0bd1` |
| Adaptive strip baseline (one 64dp surface, RowLayout, flex sliders, intrinsic icons, clipping) | `PaintbrushStrip.qml`, `PaintbrushToolbar.qml`, `PaintbrushToolOptionsBar.qml`, `PaintbrushWorkspace.qml`; commits `57469a8b71`…`7e8faff59e`. **A further implicit-width/toolbar refactor is currently uncommitted and not part of this lamp.** |
| Compact colour group: FG/BG chips, 8 recent selected colours, palette popover over all 27 persisted palettes | `PaintbrushStrip.qml`, `b971b9f847`, `7a38f8dd63` |
| Semantic capabilities: `toolUsesColor`, `toolHasPrimaryAction`, `toolIsBusy` (+ `toolCapabilitiesChanged`) | `pb_controller.{hpp,cpp}`, `43a8edc285` |
| Honest history semantics: `recentSelectedColors` | `pb_controller.{hpp,cpp}` |
| Keyboard-operable swatches (Button, Accessible.name/role, StrongFocus, focus ring) | `67cac3e041` |
| Palette store: 27 locked built-ins incl. Art Studio paper/stocks/18 directions, user palettes as JSON, import/export | `pb_palette_store.{hpp,cpp}`, `pb_controller_palettes.cpp`, `palettes/builtin-palettes.json`, `probes/sync-builtin-palettes.ps1` |
| Subject model: `SubjectAsset` / `SubjectInstance` / `DerivedArtwork`, JSON persistence, cut-out PNG | `pb_subject_store.{hpp,cpp}` |
| Object Select: layer-space segmentation (`SourceCanvasLayer`), mask baked to document space, Keep/cut-out use layer pixels | `pb_selection.hpp`, `pb_controller.cpp` |
| SAM: eager preload, bounded 640px encode with mapped prompt points, encode-keyed cache, phase markers, **row-stride mask fix**, device ladder **auto/gpu/cpu with CPU fallback**; tracking now normalizes empty/unknown to auto and falls back to CPU for auto | `pb_segmentation_service.{hpp,cpp}`, `pb_tracking_service.cpp` |
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
8. **Partially enforced.** QML must consume capability properties for
   capability decisions, not tool numbers. `PaintbrushStrip.qml` uses
   `toolUsesColor` / `toolHasPrimaryAction` / `toolIsBusy`, but the following
   still use numeric ids and are a known migration gap:
   `PaintbrushPaletteBar.qml:116`, `PaintbrushBoostBar.qml:39`,
   `PaintbrushToolOptionsBar.qml:115/121/133/148`, and
   `PaintbrushCanvasView.qml:170` (`presentationMode === 0`). Do not claim full
   compliance until these are migrated.
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
  **Visual native acceptance pending** — the current worktree contains the
  concurrent temp probe `PaintbrushCanvasView.visible: false` (+ red geometry
  rectangle) in `PaintbrushWorkspace.qml`; until its owner removes it, the
  canvas and the `VideoSurface` inside it are hidden in the DLL. The
  decoded-frame preview is not implemented and is only needed if painting over
  live video requires a QImage.
- **Video compositor evidence**: the fix was derived from the QML/provider
  signal graph (common to DComp/Win7); the actual runtime mode/rects were not
  captured in this session. If acceptance still fails, capture the §2.1
  evidence before changing the renderer.
- **Invariant 8**: the numeric-id checks listed in §4.8 remain.
- **Probe coverage**: 2 QML probes + 2 static checks. `pb_media_logic.hpp` has
  no standalone C++ harness; settings/undo paths are not probe-covered.
- **Concurrent worktree**: a toolbar/canvas workstream is editing
  `pb_canvas_item.hpp` and several QML files (`PaintbrushToolbar.qml`,
  `PaintbrushToolOptionsBar.qml`, `PaintbrushStrip.qml`,
  `PaintbrushWorkspace.qml`) plus untracked `scripts/pb-capture-window.ps1`.
  `PaintbrushWorkspace.qml` currently contains the temporary
  `PaintbrushCanvasView.visible: false` and red geometry probe. `git status` is
  authoritative. Those changes are not part of this lamp and were not staged;
  the built DLL may lag the newest QML edits.
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
- **Strip appearance**: baseline structurally complete; a concurrent
  implicit-width refactor is uncommitted. **Visual acceptance pending.**
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
  committed at `c03ecc0bd1`. No files from this task are left uncommitted.**
- The only uncommitted work is the separate toolbar/canvas workstream listed in
  §5, including the `visible:false` temp probe and red geometry rectangle.
  Coordinate with its owner; do not fold it into this fix.
- **Before native acceptance**, that probe must be removed/reverted; otherwise
  the canvas is hidden independently of the video-state fix.
- **Do not** add a plaintext API-key field or a `var_SetString`-based setter.
  The supported secret path is the environment, read by `pbApiKey()`.
- If the toolbar work is completed and committed, remove the probe, rebuild the
  DLL, and update §1/§9 so the artifact is reproducible from HEAD.

## 7. Next actions, in order

1. **Remove/coordinate the concurrent `visible:false` probe**, then native
   acceptance of video presentation, settings persistence/Apply, and subject
   undo/redo.
2. If the video still fails after the probe is gone, capture the §2.1 runtime
   evidence (mode, native rect, surface geometry, `hasEmbededVideo`); do not
   start with a renderer rewrite.
3. Implement a decoded-frame preview only if painting/tracking over live video
   requires a QImage; use `requestAsync` + token + cancellation.
4. Commit/coordinate the concurrent toolbar work; rebuild a clean-HEAD artifact.
5. Migrate the remaining numeric tool-id checks (§4.8).
6. Add a C++ harness for `pb_media_logic.hpp` and probe the settings Apply /
   subject undo contracts.
7. Runtime-confirm layer show/hide and strip visuals.
8. Remaining ADR ambitions: native `LAYER_VECTOR` in-canvas node editing
   (re-count the call sites before scoping; the old “15 call sites” figure is
   not reproducible from the current tree) and true stylus pressure feeding
   `pressureFactor()`.

## 8. Diagnostics cheat-sheet

`pb_noskip.log` categories:

- `segmentation`: `queued`, `encode START/DONE/FAILED/reused`, `mask ready`,
  `error`, `model: load START/DONE/FAILED (tier=… use_gpu=…)`
- `objectselect`: click route, `layer route`, `compose route`
- `layers`: `setLayerVisible: index=… visible=… currentBefore=…`
- `media`: thumbnail slot latency / nearest-keyframe delta
- `presentation`: `setCanvasItem: attached WxH, harvestMode=…`, mode switches,
  `player video state: hasVideoOutput=… playerHasVideo=…`,
  `video track count=… playerHasVideo=…`
- `PB_QML`: attachCheck geometry, statusText, place/style/derive errors

## 9. Build / most-recent evidence

```
ninja -C win64-meson modules/libqt_plugin.dll   EXIT=0 (incremental, link 10:30:51)
modules/libqt_plugin.dll                        21.11.2026 10:30:51, 417,223,436 bytes
probes                                          check-qml-contracts PASS (264),
                                                check-palettes PASS (27),
                                                2/2 probes passed, EXIT=0
                                                (re-run after c03ecc0bd1)
code repair commit                              453f11e913
video-state commit                              c03ecc0bd1
runtime smoke (built app, 1.mp4, 10:38)         video track count=1 playerHasVideo=0
                                                player video state: hasVideoOutput=1
                                                playerHasVideo=1
HEAD                                            this docs commit
worktree caveat                                 concurrent toolbar/canvas changes,
                                                including PaintbrushCanvasView.visible:false
                                                temp probe (see §1/§5)
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
