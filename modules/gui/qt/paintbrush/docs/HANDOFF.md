# NAI Paintbrush — HANDOFF (lamp)

> Durable continuation state. Read this first after a context reset.
> Contract: `docs/ADAPTIVE_UI_AND_SUBJECTS.md`. This file is the position on
> the map; do not keep a second source of truth.

## 1. Repository / build

- Repo: `D:\_SATIN_AI\NAI-OS\vlc`, branch `master`.
- Build dir: `win64-meson`. Full build verified green (`ninja`, EXIT=0,
  plugin cache regenerated).
- Last verified commit: **`67cac3e041`**.
- Uncommitted: `modules/gui/qt/paintbrush/qml/PaintbrushSettingsDialog.qml`
  (settings palette + device dropdown + API-key draft; see §6).

Build (close Paintbrush/VLC first — they lock the DLLs):

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:PATH
ninja -C win64-meson modules/libqt_plugin.dll
```

Full build + plugin cache:

```powershell
ninja -C win64-meson
```

Probes:

```powershell
modules\gui\qt\paintbrush\probes\run-probes.ps1
```

Launch:

```powershell
scripts\launch-pb-noskip.ps1
```

QML files are listed **explicitly** in `modules/gui/qt/meson.build`. A new QML
file must be added there or it will not exist at runtime even though the DLL
links.

## 2. Open P0 — video is not visible in the paintbrush window

Evidence from the running app: video plays (transport 00:04 / 00:16,
thumbnails decode) and the canvas area stays empty. `VideoSurface` only reports
geometry to `MainCtx.videoSurfaceProvider`; the native vout composites into the
**main VLC window**, so this window gets a hole with no frames. No combination
of `visible` / `renderingEnabled` / `anchors` can fix that.

**Correct fix (do this, not another surface tweak):**

1. Remove `VideoSurface` from `PaintbrushCanvasView.qml`.
2. On entering the harvest presentation, and on seek/pause, request the current
   frame from the existing `FrameProvider` (`m_editSource`) at the player PTS.
3. Fit it aspect-preserving into the canvas rect and store it as a canvas
   preview image (dedicated field on `PaintbrushCanvasItem`, painted before the
   document tiles, or a dedicated preview layer).
4. Refresh at a modest cadence (5–10 fps) only while harvesting and not
   painting; stop on compose.
5. This is also the correct input for Object Select: the encoded frame is
   exactly the frame the user sees.

Do not reintroduce a native surface.

## 3. Delivered and committed

| Area | Commits / files |
|---|---|
| Adaptive strip (one 64dp surface, RowLayout, flex sliders 5–16 cells, intrinsic icons, clipping) | `PaintbrushStrip.qml`, `PaintbrushToolbar.qml`, `PaintbrushToolOptionsBar.qml`, `PaintbrushWorkspace.qml`; commits `57469a8b71`…`7e8faff59e` |
| Compact colour group: FG/BG chips, 8 recent selected colours, palette popover over all 27 persisted palettes | `PaintbrushStrip.qml`, `b971b9f847`, `7a38f8dd63` |
| Semantic capabilities: `toolUsesColor`, `toolHasPrimaryAction`, `toolIsBusy` (+ `toolCapabilitiesChanged`) | `pb_controller.{hpp,cpp}`, `43a8edc285` |
| Honest history semantics: `recentSelectedColors` | `pb_controller.{hpp,cpp}` |
| Keyboard-operable swatches (Button, Accessible.name/role, StrongFocus, focus ring) | `67cac3e041` |
| Palette store: 27 locked built-ins incl. Art Studio paper/stocks/18 directions, user palettes as JSON, import/export | `pb_palette_store.{hpp,cpp}`, `pb_controller_palettes.cpp`, `palettes/builtin-palettes.json`, `probes/sync-builtin-palettes.ps1` |
| Subject model: `SubjectAsset` / `SubjectInstance` / `DerivedArtwork`, JSON persistence, cut-out PNG | `pb_subject_store.{hpp,cpp}` |
| Object Select: layer-space segmentation (`SourceCanvasLayer`), mask baked to document space, Keep/cut-out use layer pixels | `pb_selection.hpp`, `pb_controller.cpp` |
| SAM: eager preload (`ensureModelLoaded`, worker prologue + per-job), bounded 640px encode with mapped prompt points, encode-keyed cache, phase markers, **row-stride mask fix (stripes)**, device tier ladder `auto/gpu/cpu` with CPU fallback | `pb_segmentation_service.{hpp,cpp}` |
| Progress: service `busyChanged`/`progressChanged` forwarded to QML, 3dp indeterminate rail + phase text; Windows taskbar progress via pimpl `ITaskbarList3`, queued to the GUI thread | `pb_controller.cpp`, `pb_taskbar.{hpp,cpp}`, `PaintbrushCanvasView.qml` |
| Placements: `placeSubject` (cut-out → real layer → persisted instance, rollback on save failure), cascade offset, layer↔instance link, transform sync, restore on project open | `pb_controller_palettes.cpp`, `pb_controller.cpp`, `pb_layer.hpp` |
| Styling: `applySubjectStyle` (Outline/Glow/Ghost on the linked layer), `deriveArtwork` (raster tone derivation, `mode=raster`), `deriveContour` (selection `QPainterPath`, path persisted as `nai-paintbrush/contour/v1`, `mode=vector`) | `pb_controller_palettes.cpp` |
| Transactions: `UndoStack::beginComposite/endComposite` over `CompositeCommand`; used by `deleteInstance` | `pb_history.{hpp,cpp}` |
| Layers: atomic visibility change (forced `update()` + model reload), drag-to-reorder `moveLayerTo` + `DragHandler` | `pb_controller.cpp`, `PaintbrushInspectorDock.qml` |
| Thumbnails: nearest-keyframe acceptance (thumbnailer only returns keyframes), `modelData.*` delegate access | `pb_thumbnail_service.cpp`, `PaintbrushMediaDrawer.qml` |
| Frame placement: centre the **unscaled** raster (layer transform scales about raster centre) | `pb_controller.cpp` |
| Metadata: structured values round-trip (vector path was silently dropped by the scalar-only switch) | `pb_project_store.cpp` |
| Probes/guards: `ProbeModelBinding`, `ProbeCanvasAttach`, `check-qml-contracts`, `check-palettes`, runner | `probes/` |

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
8. QML must consume capability properties, not tool numbers.
9. History is history: recent colours are *selected* colours, not palettes and
   not painted-with.

## 5. Known limitations / unverified

- **Layer show/hide**: implemented (`545746ab4b` — forced repaint + model
  reload). Awaiting runtime confirmation. If it still fails, the `layers` log
  line `setLayerVisible: index=… currentBefore=…` decides between a wrong row
  index from QML and a toggle that never reaches the controller.
- **Strip appearance**: structurally complete; **visual acceptance pending**.
  Parameters are distributed per cell now (sliders flex), but density and
  proportions are unjudged.
- **`sam3_params` has only `use_gpu`** — no adapter index, no VRAM query. NAI
  cannot choose the dGPU by name; a per-application GPU preference must be set
  at driver level. DXGI adapter selection would require a sam3 API change.
- **Taskbar progress** is wired but not runtime-verified.
- **Settings**: magenta was caused by `ColorContext` not resolving inside the
  dialog's own scopes. The uncommitted rewrite replaces it with literal
  VLC-neutral colours; visual confirmation pending.

## 6. Uncommitted work — finish and commit

`modules/gui/qt/paintbrush/qml/PaintbrushSettingsDialog.qml`:

1. `ColorContext` replaced by a literal `QtObject` palette
   (`fg.primary #e8e8e8`, `fg.secondary #a8a8a8`, `accent #4ea1d3`,
   `border #2c2c2c`, `bg.primary #121212`) — VLC-consistent, no magenta.
2. SAM device is a `ComboBox` with `auto / gpu / cpu` bound to
   `draftSamDevice` (was a free-text field).
3. `draftAssistantKey` property added (reset on open) — **input box and
   persistence are NOT yet implemented**.

To finish:

- Add a password `TextField` for the companion API key
  (`echoMode: TextInput.Password`, placeholder "Only if your endpoint requires
  one"), bound to `draftAssistantKey`.
- Add `Q_INVOKABLE void setAssistantApiKey(const QString &key)` to
  `PaintbrushController` and implement it with
  `var_SetString(m_intf, "paintbrush-assistant-api-key", …)`, then call it from
  `apply()` when the field is non-empty.
- Build, run probes, commit.

## 7. Next actions, in order

1. **Video-into-canvas preview** (§2) — the only open P0.
2. Finish and commit Settings (§6).
3. Confirm layer show/hide at runtime; fix from the `layers` log if needed.
4. Visual iteration on the strip (delta from the human, small adjustments only).
5. Remaining ADR ambitions: native `LAYER_VECTOR` for in-canvas node editing
   (measured scope in the previous revision of this file: `LAYER_STICKER` alone
   spans 15 call sites plus render and serialize), and true stylus pressure
   feeding the existing `pressureFactor()`.

## 8. Diagnostics cheat-sheet

`pb_noskip.log` categories:

- `segmentation`: `queued`, `encode START/DONE/FAILED/reused`, `mask ready`,
  `error`, `model: load START/DONE/FAILED (tier=… use_gpu=…)`
- `objectselect`: click route, `layer route`, `compose route`
- `layers`: `setLayerVisible: index=… visible=… currentBefore=…`
- `media`: thumbnail slot latency / nearest-keyframe delta
- `presentation`: `setCanvasItem: attached WxH, harvestMode=…`, mode switches
- `PB_QML`: attachCheck geometry, statusText, place/style/derive errors

## 9. Build/most-recent evidence

```
ninja -C win64-meson            EXIT=0 (full build, plugins.dat regenerated)
modules/libqt_plugin.dll        21.11.2026 08:56
probes                          2/2 passed
HEAD                            67cac3e041
```
