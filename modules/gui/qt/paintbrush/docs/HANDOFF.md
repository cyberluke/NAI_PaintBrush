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
- **Code repair commit: `453f11e913`** — settings persistence/Apply, subject
  place/delete undo, QML contract guard. This lamp is the docs commit that
  contains this file.
- **Verified build (2026-11-21 09:54):**
  `ninja -C win64-meson modules/libqt_plugin.dll` → `EXIT=0` (`417/417`,
  `Linking target modules/libqt_plugin.dll`).
  Artifact: `win64-meson/modules/libqt_plugin.dll`,
  `21.11.2026 09:54:17`, 417,197,170 bytes.
- **Provenance caveat:** the build ran against `453f11e913` **plus** concurrent
  uncommitted toolbar/canvas work in the same worktree (for example
  `pb_canvas_item.hpp`, `PaintbrushToolbar.qml`,
  `PaintbrushToolOptionsBar.qml`, later also `PaintbrushStrip.qml` and
  `PaintbrushWorkspace.qml`, and untracked `scripts/pb-capture-window.ps1`).
  Those files were not authored or staged by the audit repair, and the worktree
  kept changing during the session. The DLL is a build of the worktree at
  09:54, **not** a clean-HEAD artifact. Once that work is committed, rebuild
  for a reproducible artifact.
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

## 2. Open P0 — video preview plan (corrected; NOT implemented)

Symptom: video plays (transport advances, thumbnails decode) and the paintbrush
canvas area stays empty. The previous revision asserted a root cause without
decisive runtime evidence. Do not repeat that mistake.

### 2.1 Establish the facts first

Before changing native video presentation, collect and record:

- compositor mode: DComp vs Win7 vs platform
  (`compositor_dcomp.cpp:68` adds the video visual **below** the Qt UI visual;
  that z-order matters);
- `MainCtx.hasEmbededVideo`, whether the native vout is enabled, and the native
  video window/visual rect and visibility;
- `VideoSurface` geometry (`mapToScene`) and its lifecycle when the view loader
  switches between `Player.qml` and `PaintbrushWorkspace.qml`
  (`MainInterface.qml:321-329`);
- `PaintbrushCtx.presentationMode` and `PaintbrushCanvasItem::m_videoHarvestMode`;
- the QML window clear colour/transparency at the canvas area;
- whether removing the surface leaves the native video visual/window visible,
  frozen, or hidden.

The paintbrush is hosted **inside the main window's QML scene**, not a separate
window. “This window gets a hole” was imprecise; fix the observed failure, not
an imagined rendering architecture.

### 2.2 Decode path

**Never call `FrameProvider::request()` periodically from the GUI thread.** It
blocks the caller (up to 8 s) and is only safe for one-off actions
(`pb_media_logic.hpp:90-94`, `pb_frame_capture.hpp:87-90`).

Use the existing asynchronous mechanism:

- `EditFrameSource::requestAsync()` + request token;
- reject stale token results;
- `cancelInFlight()` / cancellation on mode switch, seek and close;
- at most one preview request in flight;
- throttle to 5–10 fps with a timer while harvesting and not painting;
- define a priority/backpressure policy against thumbnail batches and Object
  Select (they share the single thumbnailer thread). Do not blind-cancel their
  work.

`PaintbrushController::captureVideoFrame()` already uses the async pattern with
a token and explicit stale-result rejection (`pb_controller.cpp:726-750`); reuse
it rather than inventing a second path.

### 2.3 Render/interaction contract

- `setVideoHarvestMode(true)` deliberately prunes **all** document tiles
  (`pb_canvas_item.cpp:2060-2075`, `3966-3976`). “Painted before the document
  tiles” is a no-op in that mode. Put the preview in a dedicated viewport-space
  QSG node (like `m_liveHarvestOverlay`), or explicitly change the
  tile-suppression contract if the user must paint over the preview.
- Define **look-only vs paintable** in `HarvestVideo`. If look-only, the
  “draw over live video” promise is not delivered in that mode; if paintable,
  the canvas must render document tiles over the preview and the paint path must
  be tested.
- Reuse `buildSelectionTransforms` and
  `ObjectSelectionContext::itemToFrame/frameToItem` for the preview↔canvas
  mapping so Object Select prompt points cannot drift at zoom/pan.
- Exactness: call it the **decoded picture**, not an “encoded frame”; the
  pipeline returns raw pictures (no PNG encode). “Exactly the frame the user
  sees” is only defensible when paused and PTS-verified (`DecodedFrame::ok()`).
  While playing, the edit decoder is a separate software-only input and lags.
- If `VideoSurface` is removed, explicitly disable/hide the native vout or
  embedded visual for the paintbrush presentation and re-enable it on exit.
  There is no paintbrush-specific disable today (only the autoresize special
  case in `video_window_handler.cpp:121`).

### 2.4 Acceptance

The preview appears in the canvas; Object Select clicks map correctly at 100%,
zoomed and panned; the GUI never stalls on decode; leaving/entering playback
does not leave a native video rectangle over the UI.

## 3. Delivered and committed

| Area | Commits / files |
|---|---|
| **Settings persistence (repaired)** — `config_PutPsz`/`config_PutInt` + `config_SaveConfigFile`, read-back validation, empty-on-success, sticky restart state, SAM device normalization (`auto/gpu/cpu`, empty/unknown → auto), PTS clamp 20–2000 ms, API keys stay environment-only | `pb_controller.{hpp,cpp}`, `pb_common.{hpp,cpp}`, `pb_segmentation_service.cpp`, `pb_tracking_service.cpp`, `PaintbrushSettingsDialog.qml`; `453f11e913` |
| **Subject transactions (repaired)** — `placeSubject`/`deleteInstance` are one `CompositeCommand` covering both the layer command and the persisted `SubjectInstance` record; the instance id is captured in the `InsertLayerCommand` payload before it is pushed; deleting the only layer is refused instead of orphaning the record | `pb_controller_palettes.cpp`, `pb_history.hpp`, `pb_controller.{hpp,cpp}`; `453f11e913` |
| **QML contract guard (repaired)** — legitimate JS members such as `model.indexOf` are allowed; delegate `model.<role>` misuse is still flagged; actual gate 2/2 PASS | `probes/check-qml-contracts.ps1`; `453f11e913` |
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

## 5. Known limitations / unverified

- **Settings**: code repaired and compiled. **Native acceptance is pending**:
  Apply closes the dialog; values survive an application restart; device
  normalization and PTS range behave; a restart notice persists.
- **Subject undo**: code repaired and compiled. **Native acceptance is
  pending**: place → undo/redo, delete → undo/redo, project save/reload after
  undo, only-layer refusal.
- **Video P0**: not implemented. §2 is the corrected plan; it needs the runtime
  evidence from §2.1 before native presentation changes.
- **Invariant 8**: the numeric-id checks listed in §4.8 remain.
- **Probe coverage**: 2 QML probes + 2 static checks. `pb_media_logic.hpp` has
  no standalone C++ harness; settings/undo paths are not probe-covered.
- **Concurrent worktree**: a toolbar/canvas workstream is editing
  `pb_canvas_item.hpp` and several QML files (`PaintbrushToolbar.qml`,
  `PaintbrushToolOptionsBar.qml`, `PaintbrushStrip.qml`,
  `PaintbrushWorkspace.qml`) plus untracked `scripts/pb-capture-window.ps1`.
  `git status` is authoritative. Those changes are not part of this lamp and
  were not staged; the built DLL may lag the newest QML edits.
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

- **This audit repair is committed at `453f11e913`; no audit-repair files are
  left uncommitted.**
- The only uncommitted work is the separate toolbar/canvas workstream listed in
  §5. Coordinate with its owner; do not fold it into this repair commit.
- **Do not** add a plaintext API-key field or a `var_SetString`-based setter.
  The supported secret path is the environment, read by `pbApiKey()`.
- If the toolbar work is completed and committed, rebuild the DLL and update
  §1/§9 so the artifact is reproducible from HEAD.

## 7. Next actions, in order

1. **Native acceptance** of the settings and subject-undo repairs (§5).
2. **Video preview** (§2): collect the §2.1 evidence, then implement the async
   preview plan; do not start with a renderer rewrite.
3. Commit/coordinate the concurrent toolbar work; rebuild a clean-HEAD artifact.
4. Migrate the remaining numeric tool-id checks (§4.8).
5. Add a C++ harness for `pb_media_logic.hpp` and probe the settings Apply /
   subject undo contracts.
6. Runtime-confirm layer show/hide and strip visuals.
7. Remaining ADR ambitions: native `LAYER_VECTOR` in-canvas node editing
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
- `presentation`: `setCanvasItem: attached WxH, harvestMode=…`, mode switches
- `PB_QML`: attachCheck geometry, statusText, place/style/derive errors

## 9. Build / most-recent evidence

```
ninja -C win64-meson modules/libqt_plugin.dll   EXIT=0 (417/417, link 09:54:17)
modules/libqt_plugin.dll                        21.11.2026 09:54:17, 417,197,170 bytes
probes                                          check-qml-contracts PASS (264),
                                                check-palettes PASS (27),
                                                2/2 probes passed, EXIT=0
code repair commit                              453f11e913
HEAD                                            this docs commit
worktree caveat                                 built with concurrent uncommitted
                                                toolbar/canvas changes (see §1)
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
