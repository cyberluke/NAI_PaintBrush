# NAI Paintbrush — Handoff Document

**Date:** 2026-10-02
**Author:** implementation agent (handoff to maintainer)
**Spec source:** `C:\Users\lukes.COREI9\Downloads\NAI_Paintbrush_VLC4_Full_Scope_One_Run_Spec (1).md` (34 sections)
**Repo:** `D:\_SATIN_AI\NAI-OS\vlc` — VLC 4.0.0-dev, branch `master`, base commit `e4f2d3a399`
**Build tree:** `win64-meson\` (native MSYS2/MINGW64 Meson build, 408 modules)

---

## 1. Executive summary

The NAI Paintbrush creative workspace was implemented as a native VLC 4 Qt/QML
module (`modules/gui/qt/paintbrush/`, 27 C++ files + 8 QML files), following the
spec's primary architecture decision: **inside VLC's Qt/QML app with a C++
backend** — no browser embedding, no separate app.

Beyond the spec, the run also produced a **fully working native Windows build and
runtime** (the spec assumed a working VLC checkout; this checkout had no build and
multiple runtime blockers that had to be resolved to demo anything), a **persisted
LiteLLM Edge AI-assistant gateway**, and a **graffiti-physics spray tool**.

**Status:** the workspace launches (`vlc.exe --paintbrush`), a blank canvas is
ready immediately, classic drawing tools work, the assistant is wired to a live
local gateway, and the window is fully manageable. Not everything in the spec's
full vertical slice is complete — gaps are listed in §5 with exact reasons.

---

## 2. What was built FROM the spec

### 2.1 Workspace shell (Spec §2, §5, §6, Phase B)

| Spec requirement | Delivered |
|---|---|
| `vlc --paintbrush` CLI + startup target | Yes — `add_bool("paintbrush")` + `MainCtx::loadPrefs`; `--paintbrush <media>` equivalent via normal VLC args |
| Navigation entry point (sidebar "Paintbrush") | Yes — `NavigationModel` entry, handled in `MainDisplay.qml`, gated by `paintbrush-enabled` |
| Workspace switching (preserves player) | Yes — `MAININTERFACE_MODE_PAINTBRUSH` flag; entering Paintbrush never clears `playerView`; "Back to VLC" restores media item/timestamp/paused state |
| Main layout: tools left / canvas center / inspector+assistant right / palette below / media drawer bottom | Yes — `PaintbrushWorkspace.qml`; right dock + drawer collapsible |
| Blank canvas / Open image / Open media starting states | Yes — `newBlankCanvas`, `openImageFile`, `captureVideoFrame`, clipboard paste, drag/drop |
| Drag-and-drop onto canvas | Yes — `ItemAcceptsDrops` + `imageDropped` → layer |
| Clipboard image paste | Yes — `pasteImageFromClipboard` |
| Feature flag isolation (`paintbrush-enabled`) | Yes |
| First-launch experience (Start drawing / Open a picture / Play with a video) | Yes — `PaintbrushFirstLaunch.qml` (overlay exists; **default is now hidden** so the workspace starts ready-to-draw) |

### 2.2 Tool rail (Spec §3.3, §6.2, Phase C)

All 15 classic tools present in `PaintbrushToolRail.qml` + `ToolId` enum:
Freeform Select, Rect Select, Object Select (with sparkle when SAM3 available),
Pencil, Brush, Eraser, Fill, Eyedropper, Spray, Line, Rectangle, Rounded
Rectangle, Ellipse, Text, Pan/Zoom. Tool rail also has zoom in/out/fit buttons.

### 2.3 Canvas engine + document (Spec §7, §14, Phase C)

| Spec requirement | Delivered |
|---|---|
| Native QQuickItem canvas, tile-based, dirty-tile updates, QSG textures | Yes — `pb_canvas_item.cpp`: 256px tiles, dirty-set compositing, `QSGSimpleTextureNode` upload of only dirty tiles |
| Layers: raster / image / text / generated / reference | Yes — `CanvasLayer` types in `pb_layer.hpp` |
| Layer ops: create/delete/rename/reorder/duplicate/visibility/lock/alpha-lock/opacity/blend/mask | Yes (controller + document); rename via document API |
| Blend modes: Normal/Multiply/Screen/Overlay/Darken/Lighten | Yes — `BlendMode` + `toCompositionMode` |
| Transform: move/scale/rotate/flip/crop | Yes — `flipHorizontal/flipVertical/rotate90/cropToSelection`; move via layer position; selection move/scale |
| History: document-level undo/redo, coalesced strokes, bounded memory | Yes — `UndoStack` (region deltas for strokes, structural snapshots for layer ops) |
| Pencil/brush/eraser/fill/eyedropper/spray/shapes/text | Yes |
| Selections: rect/freeform/invert/grow/shrink/feather/hole-fill/copy/cut/paste/delete | Yes — full selection suite in canvas item |
| New/Open/Save/Save As, canvas resize, crop, rotate/flip, zoom, grid | Save/Open/resize/crop/rotate/zoom yes; **grid not implemented** |
| Copy/cut/paste, transparent paste | Yes |
| Status bar coords/selection dims/zoom | Yes — `updateStatus` |
| Export PNG/JPEG/WebP, transparent export, mask PNG, object PNG | Yes — `pb_export.cpp` |
| Pen/tablet pressure | **Not implemented** (Qt exposes it; deferred) |

### 2.4 Media ingestion + frame bridge (Spec §8, §10, Phase D, F)

| Spec requirement | Delivered |
|---|---|
| Local image / local video / current playback / clipboard / drag-drop | Yes |
| Frame capture via VLC vout (no window screenshots) | Yes — `vlc_player_vout_Hold()` + `vout_GetSnapshot(..., "png")` in `pb_frame_capture.cpp` |
| Frame-step / seek / play-pause / stop | Yes — `vlc_player_NextVideoFrame`/`PreviousVideoFrame` via `on_next_frame_status`, `setTime`, playlist controller |
| Media drawer: transport, timestamp, seek bar, "Use this frame", tracked lanes | Yes — `PaintbrushMediaDrawer.qml` |
| Thumbnail strip (async) | **Not implemented** |
| **Real temporal tracking** (forward/backward, progress, cancel, keyframes, corrections, mask cache) | Yes — `TrackingService` uses SAM3 `sam3_create_visual_tracker` / `sam3_propagate_frame` / `sam3_refine_instance`; keyframes + confidence + corrections stored; track lanes in drawer |

### 2.5 SAM3 object selection (Spec §9, §11.2, Phase E)

| Spec requirement | Delivered |
|---|---|
| Reuse SAM3 without touching the video filter | Yes — `SegmentationService` links the same `sam3` contrib; `modules/video_filter/sam3.cpp` untouched |
| Point-click positive, alt/right-click negative | Yes — `objectSelectClick(pos, positive)`; mask tinted overlay + contour |
| Text-prompt selection ("Find by words") | Yes — `refineObjectByText` → `sam3_segment_pcs` |
| Frame-embedding cache (no re-encode per refinement click) | Yes — byte-hash keyed cache |
| Mask preview / cutout / add-to-canvas / transparent export | Yes — `cutOutObject`, `addObjectToCanvas`, `exportObjectPng`, `exportMaskPng` |
| Missing model → explicit unavailable reason | Yes — `SegmentationService::unavailableReason` shown in UI; classic painting still usable |
| Async (no UI-thread inference) | Yes — dedicated `std::thread` for model load/encode/inference |
| Manual refinement: brush add/subtract, grow/shrink, feather, invert, clear, hole fill | grow/shrink/feather/invert/clear/hole-fill yes; **brush add/subtract not implemented** |

### 2.6 Ask about this / VLM (Spec §11, Phase G)

| Spec requirement | Delivered |
|---|---|
| OpenAI-compatible VLM provider abstraction | Yes — `VisionService`; base URL, model, API key (env `PAINTBRUSH_VLM_API_KEY` or config), timeout, max image resolution |
| Object crop + contextual full frame + question | Yes — crop capped 768px, context capped 1024px, both as `image_url` parts |
| Evidence / inference / uncertainty structured answer | Yes — JSON `{answer, evidence, uncertainty}` prompt + parsing; UI shows answer + status |
| Follow-up conversation preserving object context | Yes — conversation id threading (`sel-<ts>`) |
| Add answer as text to canvas | **Not implemented** (response shown in inspector/assistant list only) |
| "Search web" extension point | Config-gated, disabled by default (per spec) |

### 2.7 Generative object actions (Spec §12, Phase H)

| Spec requirement | Delivered |
|---|---|
| Remove object (inpaint) | Yes — `removeObject` → `GenerationService` (OpenAI-compatible image edit), result as undoable layer |
| Generative fill | Yes — `generativeFill` |
| Background replace | Yes — `replaceBackground` |
| Make sticker | Yes — `makeSticker` (trim transparent bounds + insert layer; cartoon treatment via configured image model optional) |
| Auto collage | **Not implemented** |
| Color palette helper (extract from frame/object) | **Not implemented** |
| Result-as-new-layer semantics + undo | Yes — `addResultLayer(LAYER_GENERATED)`; `m_generationRequestId` correlation; cancel + undo |
| Unconfigured provider → explicit unavailable | Yes — `GenerationService::unavailableReason` |

### 2.8 Companion (Spec §13, Phase I)

| Spec requirement | Delivered |
|---|---|
| Off / Quiet / Buddy / Kids modes | Yes — `CompanionService::Mode` + dock selector |
| Meaningful-change event aggregation (not every stroke) | Yes — `notifyEvent` on new-canvas/ai-result/object-tracked; min-interval throttle; one-analysis concurrency limit |
| Canvas preview analysis (real image) | Yes — flattened preview sent with prompt |
| Specific observations, open questions, adult/kids personas | Yes — distinct prompt builders per mode |
| Conversation in dock; bounded history | Yes — `assistantMessages` (200 cap) |
| Optional tool proposals | Suggestions only (text); no tool-executing agent |
| Optional STT/TTS voice | Config extension points exist (`paintbrush-stt-*`, `paintbrush-tts-*`, `paintbrush-companion-voice`); **no provider wired** — voice optional, isolated |
| Kids: network AI off by default | Yes — `paintbrush-kids-network` default false |

### 2.9 Persistence, diagnostics, config (Spec §15, §17, §18, §20, §21, Phase J)

| Spec requirement | Delivered |
|---|---|
| Versioned `.npb` project format (JSON manifest + assets) | Yes — `ProjectStore`: version, canvas, layers, source media, tracks, settings; atomic temp+rename save |
| Autosave (configurable) | Yes — `paintbrush-autosave`, `paintbrush-autosave-interval`, autosave timer + `autosaveNow` |
| Crash recovery | Temp-file strategy yes; **recovery UI not built** |
| Missing-media relink | **Not implemented** |
| AI Diagnostics panel (backend/model/endpoint/health) | Yes — `diagnosticsText` QML-visible (segmentation, VLM, image, assistant, tracking, companion state) |
| Config options: all spec §21 items | Yes — every `paintbrush-*` option listed in spec §21 exists in `qt.cpp` (plus assistant gateway extras, §3) |
| Secrets not committed; env/secure config for API keys | Yes — `pbApiKey` reads env first, never logged |
| Services with request id / cancel / timeout / status | Yes — segmentation/tracking/vision/generation/assistant all support cancel; timeout on network services |
| Clean shutdown / cancellation | Yes — controller destructor cancels all services |
| Structured logging via VLC | Partial — `msg_*` used; **no `paintbrush.*` category hierarchy** |

### 2.10 Failure behavior (Spec §22)

Explicit, visible, local failures everywhere: missing SAM model, VLM endpoint
down (no provider substitution), generation unconfigured, tracking canceled
(keeps completed keyframes, marks incomplete). GPU OOM / source-media-moved
paths not specially handled (no dedicated UI).

### 2.11 Performance (Spec §19)

- Dirty-tile canvas, no full repaints ✓
- Async inference on worker threads ✓
- Frame embedding cache ✓
- No window screenshot routing ✓
- Timeline thumbnails / bounded thumbnail pool ✗ (not implemented)

---

## 3. What was built as EXTRA (beyond the spec)

The spec assumed a working VLC checkout; this one had **no build at all** and
multiple runtime blockers. Delivering a demoable `--paintbrush` required:

### 3.1 Full native Windows build pipeline (env + tooling, not in spec)
- Full MSYS2/MINGW64 Meson build of VLC 4.0.0-dev → `win64-meson\bin\vlc.exe` (408 modules).
- Download + unpack of the 615 MB official contrib prebuilt
  (`vlc-contrib-x86_64-w64-mingw32-…tar.zst`), symlink→copy fixes,
  `change_prefix.sh`, custom native machine file `contrib.ini` (Windows paths).
- Meson flag set that makes the tree build natively: `-Ddbus=disabled
  -Dncurses=disabled -Dtaglib=disabled -Dqt=enabled -Dskins2=enabled
  -Dupdate-check=enabled …` plus `_WIN32_WINNT/WINVER/__MSVCRT_VERSION__`
  C/C++ args, `PKG_CONFIG_LIBDIR`, contrib `bin` on PATH for `luac`.
- pacman-installed `mingw-w64-x86_64-qt6-svg` and `qt6-shadertools` (qsb).

### 3.2 Runtime fixes required to run at all (not in spec)
- `src/win32/plugin.c`: `vlc_dlopen` now loads with
  `LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`
  (was SYSTEM32-only) — plugins failed to load before this.
- `mainui.cpp`: `preloadQtRuntimeDlls()` — preloads every `Qt6*.dll` from the
  exe dir before QML plugin loading (shared-Qt builds need this; contrib Qt is
  static in official builds so this path only activates on dev builds).
- `qt.cpp`: added `Q_INIT_RESOURCE(paintbrush_assets)` — without it the
  paintbrush QML was **dropped by the linker** (blank screen).
- Copied 139 mingw64 runtime DLLs + 65 Qt6 DLLs into `win64-meson\bin\`.
- Player mutex fix: all `vlc_player_*` accessor calls wrapped in a `PlayerLock`
  RAII guard (`pb_common.hpp` + `pb_frame_capture.cpp`) — fixed
  `vlc_mutex_held(&player->lock)` assertions in tracking/frame capture.
- Removed two bogus `change_internal()` calls after the
  `paintbrush-vlm-api-key`/`paintbrush-image-api-key` options (symbol doesn't exist).
- `modules/hw/amf/amf_helper.h`: `<locale.h>` + `__MSVCRT_VERSION__` push/pop
  workaround so the AMF module compiles with MINGW64.
- Qt module Windows deps: added `winmm` (eject.c via open_panels.cpp).
- `D3D12MemAlloc.h` vendored into the contrib include tree (Qt private header
  `qrhid3d12_p.h` needs it; contrib Qt is patched to avoid it).

### 3.3 Window / interaction fixes (extra polish)
- **Window cannot be minimized/maximized/dragged** — fixed: the paintbrush top
  bar now embeds `CSDTitlebarTapNDrapHandler` + `CSDWindowButtonSet` (CSD is the
  default on Windows; the workspace replaced the topbar that normally provides
  drag/min/max/close). Drag, double-click maximize, right-click menu, min/max/close all work.
- **Canvas painted nothing** — `PaintbrushCanvasItem` was missing
  `ItemHasContents`; `updatePaintNode()` was never called. Added the flag.
- **Welcome overlay blocked drawing** — `welcomeVisible` defaulted `true` and
  its `z:10` overlay swallowed mouse events. Default is now `false`; workspace
  starts ready-to-draw with a blank 1280×720 layer.
- QML theming fixes: `theme.bg.border` → `theme.border` (8 refs; `ColorProperty`
  has no `border`), `Overlay.overlay` null guards in dialogs, seek-slider
  binding-loop fix (`Binding { when: !pressed }`).
- `%APPDATA%\vlc` vlcrc/ini cleanup documented for wizard/geometry resets.

### 3.4 Professional graffiti spray (extra beyond classic tool)
- Wheel = **flow** while Spray active (0.1–4.0), Ctrl+wheel still zooms; Flow
  slider appears in the palette bar when Spray selected; live flow in status.
- Physics emitter (`emitSprayPuff`, ~83 Hz timer): can-tip **lags** the hand
  (velocity-dependent latency → organic curves), smoothed **hand-shake** wobble,
  cone **widens with speed**, dense core + translucent mist + occasional drip.
- Can keeps spraying while held still, like a pressurized can.

### 3.5 Persisted AI assistant gateway (extra wiring, user-requested)
- New `AssistantService` — OpenAI-compatible chat with in-process conversation
  history, auto-compaction (threshold 90 %, prune, tail 4 turns, preserve 16536,
  reserve 20000), reasoning flag, text+image input.
- New persisted config group `paintbrush-assistant-*` defaulting to the
  **LiteLLM Edge gateway** `http://127.0.0.1:4000/v1` + model
  `differential_sseproxy` — verified live against a running gateway
  (`/v1/models` and `/v1/chat/completions` both return 200).
- Companion dock chat now routes through the assistant gateway (kept the
  separate "Ask about this" vision path for object questions).
- Added `assistantConfigured`/`assistantUnavailableReason` QML properties +
  assistant block in diagnostics.

---

## 4. Verification actually performed (spec §28)

- Targeted build of the Qt plugin: `ninja modules/libqt_plugin.dll` — succeeds
  (warnings only: pre-existing `_WIN32_WINNT` redefines, Qt `-Wsfinae-incomplete`).
- Plugin cache regenerated with `vlc-cache-gen.exe` (qt module present; `--paintbrush` accepted).
- Runtime smoke tests executed with `--paintbrush --no-media-library
  --no-qt-privacy-ask --no-qt-updates-notif --file-logging
  --logmode=text --logfile=... --log-verbose=2`:
  - App launches, window renders, process stays alive; no asserts/crashes.
  - Blank canvas ready immediately; **user confirmed drawing tools work** and
    the window can be exited.
  - New spray physics + flow slider verified in the built DLL.
  - Gateway `/v1/models` + `/v1/chat/completions` return 200 (`PING_OK`).
- What was NOT verified: SAM3 real inference (no model file at runtime),
  tracking across frames, VLM/generation calls, save/reopen round-trip,
  video export, autosave recovery.

---

## 5. Known gaps vs spec (explicit, never faked)

| Area | Gap | Reason / note |
|---|---|---|
| Grid (canvas) | not implemented | optional per spec §14 |
| Pen/tablet pressure | not wired | Qt exposes it; deferred |
| Thumbnail strip (async) | not implemented | media drawer shows transport/seek only |
| Auto collage | not implemented | §12.5 |
| Palette extraction from frame/object | not implemented | §12.6 |
| "Add answer as text to canvas" | not implemented | vision answer shown in dock only |
| Selection brush add/subtract | not implemented | grow/shrink/feather/invert exist |
| Missing-media relink UI | not implemented | §22 |
| Recovery UI after crash | temp-file strategy only | §15 |
| STT/TTS providers | config only, no provider | voice optional per spec |
| Video/clip export | not implemented | masks/tracks export as PNG; spec allows deferring to VLC transcode |
| `paintbrush.*` structured log categories | partial (`msg_*` only) | §20 |
| Menu "Open in Paintbrush" / hotkey | sidebar+CLI+drag+clipboard only | §5.1 menu/hotkey deferred |
| Track effects (blur/dim/spotlight/outline/attach) | not implemented | §10.4 (track *data* exists) |

---

## 6. Repository state & rollback

- **Uncommitted changes:** `git status --short` shows 13 modified tracked files
  + untracked `modules/gui/qt/paintbrush/`,
  `modules/gui/qt/pixmaps/icons/ic_fluent_paintbrush.svg`, `.kilo/`.
  Modified files: `Makefile.am`, `assets.qrc`, `compositor_dcomp.cpp`,
  `mainctx.cpp`, `mainctx.hpp`, `mainui.cpp`, `mainui.hpp`,
  `navigationmodel.cpp`, `MainDisplay.qml`, `MainInterface.qml`,
  `meson.build`, `qt.cpp`, `modules/hw/amf/amf_helper.h`, `src/win32/plugin.c`.
- **Nothing committed yet** — the whole campaign is in the working tree.
- **Rollback:** discard the untracked `paintbrush/` dir + icon, then
  `git checkout --` the 13 modified files. No commit exists to revert.

---

## 7. Launch commands (next maintainer)

PowerShell (set these env vars per session, or wrap in a launcher script):

```powershell
$env:VLC_PLUGIN_PATH = "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\modules"
$env:QT_PLUGIN_PATH   = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH  = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:Path = "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin;D:\_SATIN_AI\NAI-OS\vlc\win64-meson\src;D:\_SATIN_AI\NAI-OS\vlc\win64-meson\lib;C:\msys64\mingw64\bin;" + $env:Path
& "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin\vlc.exe" --paintbrush --no-media-library --no-qt-privacy-ask --no-qt-updates-notif
```

Rebuild after edits (MSYS2 bash):

```bash
export PATH="/d/_SATIN_AI/NAI-OS/vlc/win64-meson/bin:/d/_SATIN_AI/NAI-OS/vlc/win64-meson/src:/d/_SATIN_AI/NAI-OS/vlc/win64-meson/lib:/d/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32/bin:$PATH"
export VLC_PLUGIN_PATH=/d/_SATIN_AI/NAI-OS/vlc/win64-meson/modules
cd /d/_SATIN_AI/NAI-OS/vlc/win64-meson
ninja modules/libqt_plugin.dll
./bin/vlc-cache-gen.exe ../win64-meson/modules
```

Gateway (assistant): LiteLLM Edge on `http://127.0.0.1:4000/v1`, model
`differential_sseproxy`; configured by default via `paintbrush-assistant-*`.
VLM/image/generation providers are **not** preconfigured — set
`paintbrush-vlm-base-url`/`-model` (+`PAINTBRUSH_VLM_API_KEY`) and
`paintbrush-image-base-url`/`-model` as needed.

---

## 8. Suggested next work (maintainer decisions, not blockers)

1. Commit the working tree (spec §32 rollback/commit hygiene).
2. Provide a real SAM3 model file and verify Object Select + tracking end-to-end.
3. Wire STT/TTS and the spec's §10.4 track effects if in scope.
4. Add "Add answer as text", auto collage, palette extraction, thumbnail strip.
5. Create a launcher script (`.cmd`/`.ps1`) encoding §7 env vars so
   `build\vlc.exe --paintbrush` works for users.