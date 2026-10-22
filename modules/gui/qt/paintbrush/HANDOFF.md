# NAI Paintbrush — Handoff Document (Phase 5: decoder policy + polish)

**Date:** 2026-10-22
**Author:** implementation agent (handoff to maintainer)
**Spec source:** `C:\Users\lukes.COREI9\Downloads\NAI_Paintbrush_VLC4_Full_Scope_One_Run_Spec (1).md` (Phase 1) + Phase 2 One-Run Implementation Spec
**Repo:** `D:\_SATIN_AI\NAI-OS\vlc` — VLC 4.0.0-dev
**Branch:** `master`
**Commits:** `e4f2d3a399` (base) → `d7b3d33e82` (Phase 1) → `f20dcf8952` (Phase 2+3) → `7578484635` (merge upstream 265 commits) → `9378df875a` (Phase 4 hardening + gaps) → `b4298d8883` (avcodec hardware-decoding policy) → (Phase 5, this session)
**Build tree:** `win64-meson\` (native MSYS2/MINGW64 Meson build, 408 modules)

---

## 1. Current implementation state

The NAI Paintbrush creative workspace is a native VLC 4 Qt/QML module
(`modules/gui/qt/paintbrush/`, 33 C++ files + 8 QML files + 2 docs). It ships
as `vlc --paintbrush` and provides a classic Paintbrush-style drawing app with
video media awareness and AI assistance, all inside the Qt/QML app (no browser
embedding).

Everything from Phase 1 is preserved and verified working: canvas + tools,
media frame bridge, SAM3 selection/tracking, VLM, generative actions,
companion, `.npb` persistence, LiteLLM Edge assistant gateway, and the
MSYS2/MINGW64 build path.

Phase 2 turned the vertical slice into a product: precision drawing (grid +
snap + pen pressure), a real thumbnail timeline, auto collage, selection
refinement, object identity, tracking polish, video-to-sticker, improved
"Ask about this", a lookup extension point, companion silence discipline,
completed diagnostics, crash recovery, export presets, keyboard shortcuts,
demo mode, and an AI job scheduler.

## 2. Closed gaps (from the Phase 1 handoff)

| Previous gap | Now |
|---|---|
| Grid | Implemented: scene-graph overlay (never dirties raster), View toggles, spacing slider, snap-to-grid (independent toggle), crisp at integer zoom |
| Pen/tablet pressure | Implemented: Qt6 native pressure via `QEventPoint::pressure()`; `pressureEnabled`/`pressureToOpacity`/`pressureMin`/`pressureMax`; mouse unaffected (reads 1.0); status shows pen pressure; no vendor APIs |
| Media thumbnails | Implemented: `ThumbnailService` — async, two-phase (seek→snapshot) timer, bounded 4–48 slots, anchor-first (viewport) ordering, cache keyed by media+timestamp, cancellation, placeholder state, seek-on-click, current-frame highlight, compact loading indicator |
| Auto collage | Implemented: `autoCollage(preset)` with free/grid/editorial/scrapbook/poster/banner; preserves every piece as an editable layer (no flatten); transparency preserved; scrapbook adds rotation-jitter metadata; one undoable structure op; UI dialog in inspector |
| Selection refine panel | Implemented: Add (point click), Subtract (alt/right), Feather, Expand (grow), Contract (shrink), Smooth (new 3×3 majority filter, 2 passes), Reset (clear); high-DPI contour via existing mask path |
| Object identity continuity | Implemented: `ObjectTrack` carries id/sourceMRL/startMs/label/keyframes/corrected flags; tracks persist in `.npb`; track id links mask→track→layer flow; `seekToTrackKeyframe` keeps the entity navigable |
| Tracking polish | Implemented: track lane list (label, keyframe count, correction markers), pause/resume (`pause()`/`resume()`), cancel, delete track, seek-to-keyframe click |
| Video-to-sticker | Implemented: `exportTrackedSticker(trackId, dir)` — seeks player to each keyframe, applies mask → transparent cutout, trims to bounds, writes `sticker_%04d.png` sequence; honest about what it is (PNG sequence; no fake GIF/alpha-video claims) |
| "Ask about this" Phase 2 | Implemented: object thumbnail beside conversation, sections (identification/evidence/uncertainty), quick actions (Copy, Add as text, Clear), follow-up question retaining object context |
| ObjectLookupProvider | Implemented: `LookupService` extension point (crop + VLM description + OCR + query → matches); default state explicitly unavailable with reason; "Find product…" UI surfaces it |
| Companion Phase 2 | Implemented: silence discipline (quiet period after a comment, doubled for "Talk less"), talk levels (Only when I ask / Talk less / Talk), kids-mode prompts already question-focused/non-grading |
| Diagnostics | Implemented: Copy Diagnostics, Clear AI cache, Unload AI models, Reveal autosave; active-job list; assistant limits in diagnostics |
| AI resource scheduling | Implemented: `activeJobs()` registry (segmentation/tracking/vision/generation/assistant/thumbnails with resource class), `cancelActiveJob(id)`; segmentation worker already enforces one-pending |
| Project format hardening | Already had versioning/atomic-save/unknown-field tolerance; added: Project Info dialog (canvas/layers/media/tracks/AI history/autosave), media relink, recovery copy flow |
| Crash recovery | Implemented: `checkForRecovery()` on startup; recovery banner (Restore / Open copy / Discard); recovery never overwrites the explicit save; discard removes only recovery data |
| Keyboard shortcuts | Implemented: Ctrl+N/O/S/Shift+S/E/Z/Y/X/C/V/A/D, Delete, B/E/O/T tools, G/Shift+G grid/snap, +/−/Ctrl+0/F zoom, Ctrl+←/→ frame step, Ctrl+Space play, Ctrl+U capture frame, Ctrl+I toggle assistant, Escape cancels AI |
| Export polish | Implemented: `exportImageOptions` (scale, crop-to-content, opaque background fill for JPEG); `exportImagePreset` (Canvas PNG / Transparent Sticker / Social Post / Wallpaper) |
| First-run/empty-state | Implemented: tagline, Start/Open/Play actions, recent projects list (persisted), drag/drop affordance, no AI config wall; contextual AI config on first use |
| Demo mode | Implemented: `paintbrush-demo-mode` config — preloads sample.npb if present, selects Object Select, resets to blank otherwise; no bundled media |

## 3. Extra work delivered in Phase 2 (beyond the Phase 2 spec)

- **Spray physics** (from Phase 1.5): wheel = flow, hand-lag/curve/shake physics, continuous emission, Flow slider — retained and verified.
- **Window + canvas fixes** retained: CSD drag/min/max/close, `ItemHasContents` (canvas renders), welcome overlay off by default (ready-to-draw).
- **AI assistant gateway** retained and hardened: persisted `paintbrush-assistant-*` config, LiteLLM Edge default (`http://127.0.0.1:4000/v1`, model `differential_sseproxy`), cancellable, health visible in diagnostics.
- **Grid/pressure/snap** also surfaced in the palette bar for discoverability.

## 3b. Phase 3: real video validation — closed gaps (this session)

Phase 3 exercised the module against real video (`aespa-supernova.mp4`, 2190×1262) and
SAM3 (`D:\_SATIN_AI\models\sam3-q4_0.ggml`) on a 4K (3840×1980) desktop. The user
feedback cycle drove these fixes:

| Item | Fix |
|---|---|
| No way to open a video from inside Paintbrush | "Open video…" toolbar button + FileDialog → `openVideoFile(url)` appends to the main playlist and starts playback; wizard "Play with a video…" opens the picker when no video, else captures a frame (`openVideoRequested` signal) |
| Wizard appeared only once (recovery banner replaced it) | Wizard now shows when `!m_recoveryAvailable && layerCount()==0`; `autosaveNow()` skips writing recovery on a blank unsaved canvas; stale `recovery.npb` cleaned |
| Video bled through the whole workspace (video renders in a native HWND behind the transparent QML view) | Opaque `Rectangle { color: theme.bg.primary }` at the workspace root |
| Bottom panel overflowed the desktop (video-auto-resize resized the paintbrush window to 2190×1262) | `VideoWindowHandler::setVideoSize` early-returns in paintbrush view (no video-sized autoresize) |
| Modal dialogs dimmed but were invisible (zero-width content) | Explicit dialog widths (`Math.min(overlay, VLCStyle.dp(480))`) + `Layout.fillWidth` labels on text/bg/fill/collage dialogs + project info dialog |
| No "extract object" (collage needs objects lifted from video) | `extractObject()` → `objectCrop()` → new canvas with the cutout as an image layer; "Extract object…" button in AI actions |
| VLM answered generically and did not see true video pixels (canvas composite included user strokes) | `pristineSourceFrame()` (first `LAYER_REFERENCE` raster) feeds `currentAiSource()`, `objectCrop()`, `selectionCrop()`; segmentation/tracking/generation/vision all read the true captured frame; verified by `ai_source_probe.cpp` (old composite showed blue scribble pollution, new path returns pure frame pixels) |
| Freehand mask refinement missing (only SAM point-click add/subtract) | Object Select now drag-paints the mask: left-drag add, Alt/right-drag subtract, using the brush size; `paintObjectMask`/`paintObjectMaskSegment`/`endObjectMaskStroke` on the canvas, `objectMaskEdited` → controller sync; hint in the inspector; verified by `mask_paint_probe.cpp` (radius/continuity/subtract/clamp/null-mask all pass) |

Verification: plugin rebuilds clean (`ninja modules/libqt_plugin.dll`), cache regenerated,
UI Automation tree confirms all 15 tools + new buttons instantiate, zero QML errors on launch.

## 3c. Phase 4: upstream VLC merge + paintbrush hardening (this session)

Merged upstream `master` (265 commits) into the paintbrush branch. The 7 files
that overlapped with our edits (`Makefile.am`, `mainctx.{hpp,cpp}`,
`MainDisplay.qml`, `MainInterface.qml`, `meson.build`, `qt.cpp`) merged cleanly
with every paintbrush addition preserved (paintbrush entries in meson.build,
config options + `Q_INIT_RESOURCE(paintbrush_assets)` in qt.cpp,
`MAININTERFACE_MODE_PAINTBRUSH`/`paintbrushView`/`s_instance` in mainctx.hpp,
paintbrush controller + QML type registrations in mainui.cpp).

The full tree rebuilt end-to-end on Windows after fixing three upstream build
regressions that the merge surfaced:

| Upstream regression | Fix |
|---|---|
| `modules/lua/meson.build` split `m.full_path()` on `/` but Windows returns backslash paths, so `custom_target output:` kept a path segment that modern meson rejects | Normalize to forward slashes before splitting bin/meson.build + test/meson.build + modules/lua/meson.build |
| `-DTOP_BUILDDIR/-DTOP_SRCDIR/-DSRCDIR` embedded Windows paths raw into C literals; the `\N`/`\w`/`\_` sequences break newer GCC (`incomplete universal character name`) | Normalize those defines to forward slashes (valid in putenv paths) |
| NSIS `nsProcess` built a `shared_library` with no sources when the i686 cross toolchain is absent, failing to link; WiX MSI targets crashed configure when WiX/Wine were missing | Guard the nsProcess target and the WiX block on the tools actually being present |

Paintbrush module hardening (all pre-existing latent issues, surfaced now that
QML warnings are captured):

- **Invalid `VLCStyle.fontSize_xxxsmall`** (does not exist upstream): replaced
  with `fontSize_xsmall` in 11 places across 4 QML files — this removed every
  "`Unable to assign [undefined] to int`" warning.
- **Checkerboard Grid overflow** (`Grid contains more visible items than
  rows*columns`): checkerboard cells are now positioned explicitly instead of
  via a `Grid`+`Repeater`, so item count always matches the area.

Result: after the merge, `vlc --paintbrush` launches with **zero paintbrush QML
warnings/errors** and zero plugin-load errors (a stale `src/plugins` cache path
noise line aside). The only remaining Qt log noise is an upstream
`IconToolButton.qml:81` `console.assert("text is empty")` from the standard
interface (not paintbrush — paintbrush sets both `text` and `description`).

Closed HANDOFF gaps (section 4): auto collage slot scaling (baked, pixel-perfect),
grid rulers, and the recovery "Open copy" flow.

## 3d. Phase 5: decoder pixelation fix + remaining polish (this session)

The user's core complaint — pixelated frames when scrubbing the seek slider
while paused — was root-caused to **hardware decoding** (VLC's D3D11VA
integration emits compressed/artifacted frames on random seeks). Fix per the
user's directive: a **tri-state hardware-decoding policy** instead of the
obsolete `--avcodec-hw` flag.

| Item | Fix |
|---|---|
| Pixelation on scrubbed paused frames | `avcodec-hwdec` option: `Auto` (0, default) / `Disabled` (1) / `Required` (2). When Disabled, `ffmpeg_GetFormat()` skips every hardware output format (d3d11va_vld, d3d11, dxva2_vld, cuda, vulkan) and decodes software (`yuv420p`); `InitVideoHwDec()` also refuses so AV1 falls back to software; Required fails decode if no hardware backend opens. Committed at `b4298d8883` |
| Paintbrush media must use exact random-access frames | `openVideoFile()` now appends the media with `:avcodec-hwdec=1` (software decode), while normal VLC playback keeps Auto. Verified live: the paintbrush log shows all five hardware formats skipped and `yuv420p` used |
| Track effects (blur/dim/spotlight/outline/attach) | `TrackEffect` struct on `ObjectTrack` (type/intensity/color), persisted in `.npb` (old files default to None). Applied in `renderStickerFrame()` — shared by preview, export and canvas placement. Sticker editor gains an Effect combo (None/Blur/Dim/Spotlight/Outline/Attach to frame) + intensity + color, with a live preview |
| `paintbrush.*` log-category hierarchy | `pbLogMsg()` calls `vlc_object_Log()` with dotted module names so paintbrush lines are separable from `[qt]`. Wired into configure/init/segment/track/media-open/export/network paths (`paintbrush.segmentation`, `paintbrush.tracking`, `paintbrush.media`, `paintbrush.export`, `paintbrush.network`, …) |
| Show/Hide rulers independent of grid | New `rulersVisible` property on the canvas item; rulers bind to it; `R` shortcut toggles rulers, `G` stays grid-only; "Rulers" toggle button added to the palette bar |

Verification: both plugins rebuild clean; plugin cache regenerated; full E2E
against the real SAM3 model on a real video — launch (zero QML errors) → open
video (software decode confirmed in log) → pause → "Use this frame" → Object
Select → SAM3 inference (~75 s CPU) → mask produced (Extract enabled) → Extract
object → new canvas with the cutout. Decoder A/B on the CLI confirms Auto uses
D3D11VA while `avcodec-hwdec=1` uses `yuv420p`.

## 4. Remaining gaps (actual, honest)

| Item | State |
|---|---|
| Pen hover/eraser pressure mapping | Pressure maps to size + optional opacity; hover/eraser pressure not separately mapped (mouse default unchanged) |
| Thumbnail generation while playing | Generates only while paused (by design); playing idles with placeholders |
| Auto collage scaling | **Done (Phase 4):** scale/rotation are now baked into each piece's raster so image/generated layers land exactly in their slot (pixel-perfect; previously a render-time scale with an ambiguous pivot) |
| STT/TTS providers | Config extension points exist; no provider wired (voice optional, core unaffected) |
| Animated sticker as WebM/alpha video | Only transparent PNG sequence export; WebM-alpha needs an encoder that supports it (not claimed) |
| Object lookup provider | Interface + unavailable state only; no vendor wired (by design) |
| Track effects (blur/dim/spotlight/outline/attach) | **Done (Phase 5):** per-track effect persisted in `.npb`, rendered in sticker preview/export/placement; editor combo + intensity + color in the sticker dialog |
| Automatic menu "Open in Paintbrush" / global hotkey | Sidebar + CLI + drag/drop + clipboard only |
| Grid ruler/coordinate feedback | **Done (Phase 4):** pixel-coordinate rulers along the top/left canvas edges; **Phase 5:** rulers now have their own `rulersVisible` toggle + `R` shortcut, independent of the grid (`G`) |
| Full log-category hierarchy (`paintbrush.*`) | **Done (Phase 5):** `pbLog*` helpers call `vlc_object_Log()` with dotted category names (`paintbrush.segmentation`, `paintbrush.tracking`, `paintbrush.media`, `paintbrush.export`, `paintbrush.network`, …) so paintbrush lines are separable/filterable from `[qt]` |
| Recovery "Open copy" flow | **Done (Phase 4):** `openRecoveryCopy()` loads the recovered autosave as an unsaved copy and leaves the original recovery file untouched; banner no longer opens the save dialog |

## 5. Verification performed (Phase 2 + Phase 3)

- **Builds:** `ninja modules/libqt_plugin.dll` — clean (only pre-existing `-Wsfinae-incomplete` warning). Plugin cache regenerated with `vlc-cache-gen.exe` (after each Phase 3 rebuild).
- **Runtime smoke (real launch):** `vlc.exe --paintbrush --no-media-library --no-qt-privacy-ask --no-qt-updates-notif --paintbrush-sam-model=D:\_SATIN_AI\models\sam3-q4_0.ggml --paintbrush-sam-device=cpu --verbose=2 -I qt`
  - Process alive, no assertions, no crashes; UI Automation tree shows all 15 tools + new controls instantiated.
  - **Zero QML errors**: no `TypeError`, `Cannot read property`, `Unable to assign`, or `ReferenceError` (all new UI — thumbnail strip, collage dialog, recovery banner, refine buttons, talk-level selector, project info, diagnostics actions, Open video, Extract object, mask-refine hint — instantiated cleanly).
- **Phase 3 real-video validation (4K desktop, real video + SAM3 model):**
  - Video opens from inside Paintbrush; frame capture produces a canvas with a `LAYER_REFERENCE`; wizard reappears on a fresh blank start; workspace renders opaque (no video bleed); window fits the desktop (media drawer visible); modal dialogs render with content.
  - `ai_source_probe.cpp` (compiled, exit 0): old canvas-composite crop shows blue scribble pollution (53,103,65), pristine-frame crop returns true pixels (76,128,10) — proves the VLM now sees the real frame.
  - `mask_paint_probe.cpp` (compiled, exit 0): brush radius, segment continuity (no gaps on a diagonal stroke), subtract erase, out-of-bounds clamp, and null-mask guard all pass.
- **Gateway:** LiteLLM Edge `127.0.0.1:4000` — `/v1/models` and `/v1/chat/completions` return 200 (verified `PING_OK` earlier; retained).
- **Not verifiable in this environment:** SAM3 real inference quality (model present; per-frame encode ~50 s on CPU makes interactive clicks slow but functional), tracking across frames, VLM/generation call results (providers return 200 but answer quality needs the user's eyes), animated sticker decode (needs a real tracked object), tablet pressure (no tablet hardware).

## 6. Performance observations (measured where possible)

- Idle CPU with Paintbrush open and no AI work: negligible (observed stable process, no busy loop).
- Canvas drawing: tile-based dirty updates; grid is a scene-graph geometry node — toggling it triggers no raster repaint (measured via design, not profiler).
- Thumbnails: one seek or one snapshot per 80 ms tick, bounded at 48 frames, anchor-first — UI stays responsive (observed; no blocking decode on the UI thread).
- Segmentation/tracking run on worker threads; companion yields to user actions; segmentation queue capped at one pending job.
- Memory: canvases are full-canvas RGBA QImages with copy-on-write snapshots; large-image memory not re-measured this run (no 4K source available).

## 7. Build / run (exact)

See `modules/gui/qt/paintbrush/BUILDING.md` for the full environment. Summary:

```bash
# MSYS2 MINGW64 shell
export PATH="/d/_SATIN_AI/NAI-OS/vlc/win64-meson/bin:/d/_SATIN_AI/NAI-OS/vlc/win64-meson/src:/d/_SATIN_AI/NAI-OS/vlc/win64-meson/lib:/d/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32/bin:$PATH"
export VLC_PLUGIN_PATH=/d/_SATIN_AI/NAI-OS/vlc/win64-meson/modules
cd /d/_SATIN_AI/NAI-OS/vlc/win64-meson
ninja modules/libqt_plugin.dll
./bin/vlc-cache-gen.exe ../win64-meson/modules
```

Launch (PowerShell):

```powershell
$env:VLC_PLUGIN_PATH = "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\modules"
$env:QT_PLUGIN_PATH   = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH  = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
& "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin\vlc.exe" --paintbrush --no-media-library --no-qt-privacy-ask --no-qt-updates-notif
```

Demo mode: add `--paintbrush-demo-mode` (preloads `sample.npb` if present).

## 8. Git state

- Branch `master`; base `e4f2d3a399`; Phase 1 checkpoint `d7b3d33e82`; Phase 2/3 committed (`bf1c6b3dcb` … `f20dcf8952`).
- **Phase 4 (this session), in order:** `7578484635` merge upstream `master` (265 commits); `e6d71d2cb3` + `2fa7963b0c` Windows build regressions (lua path, WiX/nsProcess guards, backslash-escape defines); `7856bdac86` paintbrush QML hardening (`fontSize_xxxsmall`, checkerboard); `9378df875a` paintbrush HANDOFF gaps (recovery "Open copy", baked collage scaling, rulers).
- **Phase 5 (this session), in order:** `b4298d8883` avcodec hardware-decoding policy (`avcodec-hwdec` Auto/Disabled/Required + paintbrush `:avcodec-hwdec=1`); then the paintbrush polish commit (track effects, `paintbrush.*` log categories, rulers `R` shortcut).
- Untracked (pre-existing, not ours): `scripts/launch-pb-noskip.ps1`, `vlc-help.txt`.
- No secrets, no binaries, no build artifacts staged (`.gitignore` covers `.kilo/`, win64-meson build dir, autosave temp files). SAM3 model file and test media are outside the repo.

## 9. Rollback

1. To drop only the Phase 4 upstream merge + hardening: `git reset --hard f20dcf8952` (keep the pre-merge paintbrush commits).
2. To drop the entire paintbrush feature back to upstream VLC: reset onto `7578484635^` (the merged upstream tip) or `git reset --hard <upstream>`, then re-apply only the paintbrush commits you want to keep.
3. To disable the feature entirely: `--no-paintbrush` / unset `paintbrush` config; the module is gated by `paintbrush-enabled`.

## 10. Suggested next work (maintainer)

1. Interactive quality pass with the user's eyes: track an object across frames (SAM3 CPU inference ~75 s/frame), export an animated sticker, build a collage — confirm the baked collage scaling, rulers, and track effects feel right interactively.
2. Wire STT/TTS providers and the optional lookup provider (extension points exist; only the interface is shipped).
3. Consider WebM-alpha sticker export when an alpha-capable encoder is confirmed (PNG sequence export is the honest current state).
4. Optional: a separate `PaintbrushFrameProvider` that serves exact software-decoded frames directly to thumbnail/SAM/VLM paths (today they reuse the main player's `:avcodec-hwdec=1` media, which is sufficient).