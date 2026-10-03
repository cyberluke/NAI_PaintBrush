# NAI Paintbrush — Handoff Document (Phase 3: real video validation)

**Date:** 2026-10-02
**Author:** implementation agent (handoff to maintainer)
**Spec source:** `C:\Users\lukes.COREI9\Downloads\NAI_Paintbrush_VLC4_Full_Scope_One_Run_Spec (1).md` (Phase 1) + Phase 2 One-Run Implementation Spec
**Repo:** `D:\_SATIN_AI\NAI-OS\vlc` — VLC 4.0.0-dev
**Branch:** `master`
**Commits:** `e4f2d3a399` (base, before Paintbrush) → `d7b3d33e82` (Phase 1 checkpoint) → `*pending*` (Phase 2 + Phase 3, uncommitted at writing time)
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
| Paused + drag frame slider always landed on pixelated/blocky frames (compression artifacts) | Thumbnail auto-batch (seeking slots every ~80 ms) raced the user's seek and yanked the playhead back mid-GOP. Fix: `ThumbnailService` gained `suspend()/resume()/isSuspended()/noteUserSeek()` + `m_suspended`/`m_userSought`; `cancel()`/`onTick()` only restore if `!m_userSought`; `onTick()` returns early when suspended; `startBatch()` clears the flags. `PbController::playerSeekBegin()/playerSeekEnd()` suspend/resume around the drag; `playerSeek()`/`playerFrameNext/Prev()` call `noteUserSeek()`; `onMediaTimer()` resumes a suspended batch only on PLAYING and gates auto-gen on `!isSuspended()`. `PaintbrushMediaDrawer.qml` slider: `onPressedChanged` → begin/end + final `playerSeek(value)` on release; `onMoved` → 80 ms `seekThrottle` debounce. Verified in a running build: the auto-batch ran once (25 discrete slots), then every user seek appeared as an isolated single seek to its exact target with no interleaved thumbnail race and no yank-back (see §5). |

Verification: plugin rebuilds clean (`ninja modules/libqt_plugin.dll`), cache regenerated,
UI Automation tree confirms all 15 tools + new buttons instantiate, zero QML errors on launch.

## 4. Remaining gaps (actual, honest)

| Item | State |
|---|---|
| Pen hover/eraser pressure mapping | Pressure maps to size + optional opacity; hover/eraser pressure not separately mapped (mouse default unchanged) |
| Thumbnail generation while playing | Generates only while paused (by design); playing idles with placeholders |
| Auto collage scaling | Pieces are repositioned and raster→image-converted; pixel-perfect slot *scaling* of image layers is limited (positions + content cut; rescaling to slot is partial for image layers) |
| STT/TTS providers | Config extension points exist; no provider wired (voice optional, core unaffected) |
| Animated sticker as WebM/alpha video | Only transparent PNG sequence export; WebM-alpha needs an encoder that supports it (not claimed) |
| Object lookup provider | Interface + unavailable state only; no vendor wired (by design) |
| Track effects (blur/dim/spotlight/outline/attach) | Not implemented (track data model supports metadata) |
| Automatic menu "Open in Paintbrush" / global hotkey | Sidebar + CLI + drag/drop + clipboard only |
| Grid ruler/coordinate feedback | Status bar shows cursor/selection/zoom/snap/grid; ruler widgets not added |
| Full log-category hierarchy (`paintbrush.*`) | `msg_*` logging only |
| Recovery "Open copy" flow | Banner offers Restore/Discard; "Open copy" saves via Save dialog (manual) |

## 5. Verification performed (Phase 2 + Phase 3)

- **Builds:** `ninja modules/libqt_plugin.dll` — clean (only pre-existing `-Wsfinae-incomplete` warning). Plugin cache regenerated with `vlc-cache-gen.exe` (after each Phase 3 rebuild).
  - **Build-system caveat (this session):** a `meson` regeneration under meson 1.12.1 fails on `modules/lua/meson.build` (`custom_target output` with a path segment). Workaround: back up `win64-meson/build.ninja` (`build.ninja.regbak`), comment out the `build build.ninja: REGENERATE_BUILD` rule (lines `#build build.ninja:` + `# pool = console`) and rebuild with the existing ninja file. Do NOT run `meson setup --reconfigure` (it will fail and leave generated qrc/qmldir stale).
  - **QML type-resolution regression (root-caused + fixed this session):** after a meson regen, all 12 VLC QML modules failed to load (`Widgets.ToolTipExt is not a type`, `Widgets.PointingTooltip unavailable`) because `genqmldir.py` built the module `prefer` path with `os.path.join()` → Windows backslashes (`prefer :/qt/qml\VLC\Widgets/`) which broke qrc resource lookup. Fixed in `scripts/genqmldir.py` (force `/` on the prefer path; flatten `nargs='*'` append lists) and `scripts/genqrc.py` (same). Regenerated all `res.qrc`/`qmldir`/`qmldir.qrc` from `meson.build` authoritative source lists (includes singletons, e.g. `PlayerControlbarControls` is a singleton of `VLC.Player`). `mainui.cpp` now always adds `:/qt/qml` import path. Verified: app launches, all 15 tools render, zero QML errors.
  - **Decoder note:** `--avcodec-hw=none` does NOT disable the D3D11VA decoder in VLC 4 (option was removed; `d3d11va: Using D3D11VA` still appears). Software decode test launcher kept in `scripts/launch-pb-software-decode.ps1` (converter falls back to swscale but HW decoder still selected).
- **Runtime smoke (real launch):** `vlc.exe --paintbrush --no-media-library --no-qt-privacy-ask --no-qt-updates-notif --paintbrush-sam-model=D:\_SATIN_AI\models\sam3-q4_0.ggml --paintbrush-sam-device=cpu --verbose=2 -I qt`
  - Process alive, no assertions, no crashes; UI Automation tree shows all 15 tools + new controls instantiated.
  - **Zero QML errors**: no `TypeError`, `Cannot read property`, `Unable to assign`, or `ReferenceError` (all new UI — thumbnail strip, collage dialog, recovery banner, refine buttons, talk-level selector, project info, diagnostics actions, Open video, Extract object, mask-refine hint — instantiated cleanly).
- **Phase 3 real-video validation (4K desktop, real video + SAM3 model):**
  - Video opens from inside Paintbrush; frame capture produces a canvas with a `LAYER_REFERENCE`; wizard reappears on a fresh blank start; workspace renders opaque (no video bleed); window fits the desktop (media drawer visible); modal dialogs render with content.
  - `ai_source_probe.cpp` (compiled, exit 0): old canvas-composite crop shows blue scribble pollution (53,103,65), pristine-frame crop returns true pixels (76,128,10) — proves the VLM now sees the real frame.
  - `mask_paint_probe.cpp` (compiled, exit 0): brush radius, segment continuity (no gaps on a diagonal stroke), subtract erase, out-of-bounds clamp, and null-mask guard all pass.
  - **Seek-drag pixelation fix (real run, `pb_run6.log`):** video opened and paused; full seek log shows the thumbnail auto-batch ran once as 25 discrete slot seeks (1 ms → 36 000 ms at 1.5 s), then each user seek (8935 ms, 27 074 ms, 10 143 ms, 10 057 ms, 11 067 ms, 10 143 ms) appeared as an **isolated single seek to its exact target with no interleaved thumbnail-race seeking and no playhead yank-back**. Every seek produced a clean `Received first picture` decode; screenshot analysis of the paused frame at ~10.3 s showed high detail (535–943 distinct colors, R range 4–255) — no blocky compression artifacts. App then ran to EOF and shut down cleanly (`QApp exec() finished`), proving the rebuilt plugin is stable under real media playback.
- **Gateway:** LiteLLM Edge `127.0.0.1:4000` — `/v1/models` and `/v1/chat/completions` return 200 (verified `PING_OK` earlier; retained).
- **Crash recovery Restore (real run, `pb_run7.log`):** at launch the app surfaced a valid `recovery.npb` (`%APPDATA%\vlc\paintbrush\recovery.npb`, 5538 B) with a banner showing all three actions (**Restore / Open copy… / Discard**). Invoking **Restore** deleted the recovery file and loaded the document — the UI then showed the restored canvas with `Layer 1`, the banner text was gone, and all AI services reported ready (`Object Select ready`, `Ask about this ready`, `Image generation ready`). Exit: recovery file consumed, restore succeeded.
- **Export (real run, same session):** "Export image…" opens the export dialog (format selectors `PNG image (*.png)` / `JPEG image (*.jpg)`, `Scale` option), then a native Save dialog (`File name` field, `Save`/`Cancel`). A completed export wrote a **1280×720 RGBA PNG, 3.69 MB** (verified via `System.Drawing`: width/height/pixel format). The PNG encoder write path is independently confirmed by the thumbnail tiles and `sam3_track_probe` mask outputs (all logged as `using image encoder module "png"`).
- **SAM3 runtime inference (verified with `sam3_seg_probe`, real `sam3-q4_0.ggml`, real video frame):** model loads cleanly (FORMAT v3, 1464 tensors, 712.55 MB buffer, 49408 vocab entries), encoder runs (~74.7 s/frame on 8 CPU threads for the 2190×1262 frame), and a center-point PVS segment returns `MASK 2190x1262 iou=0.751 obj=1.000` saved to PNG (exit 0). This is the exact `pb_segmentation_service` API path (`sam3_load_model` → `sam3_create_state` → `sam3_encode_image` → `sam3_segment_pvs`). Point-segment cost after one encode is ~370–450 ms (embedding reuse works). The `HAVE_SAM3` branch is confirmed compiled into the running plugin (343 `-DHAVE_SAM3` flags in `build.ninja`; "Object Select ready" reported in the UI).
- **Tracking (verified, `sam3_track_probe`):** tracker creation, instance add, and per-frame propagation all run against the real model and a real 4-frame clip (frames extracted from `aespa-supernova.mp4`). Result `TRACKING_OK frames=4 instance=1`: model + visual tracker created, frame 1 encoded, a 6-point scan found an object-sized mask (cov=3.82%, obj=1.0) at (766,820), `sam3_tracker_add_instance` → id=1, then `sam3_propagate_frame` across frames 2–4 each returned a detection for instance 1 with the bbox/centroid tracking the moving object (centroid 218→746→769 across frames; per-frame mask PNGs `track_f2/f3/f4` saved). Per-frame encode dominates runtime (~71–102 s on 8 CPU threads at 2190×1262), point-segment is ~370–450 ms after one encode. This is the exact `pb_tracking_service` API path.
- **Not verifiable in this environment:** VLM/generation *answer quality* (providers return 200 but the specific answers need the user's eyes), animated sticker decode timing (needs a real tracked object), tablet pressure (no tablet hardware).

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

- Branch `master`; base `e4f2d3a399`; Phase 1 checkpoint `d7b3d33e82`; Phase 2 checkpoint `bf1c6b3dcb`.
- **Phase 2 + Phase 3 are committed** as logical commits (all on `master`):
  - `d4257d11bf` **paintbrush: Phase 3 core services — real-video AI path and freehand mask refinement** (25 files)
  - `268dc265bd` **paintbrush: Phase 3 QML — opaque workspace, Open video, dialogs, debounced seek** (7 files)
  - `1e7ea118b2` **paintbrush: Phase 3 handoff docs and launch scripts** (docs + `scripts/`)
  - `7bbf8bdae9` **qt: fix qml module qrc path generation on Windows (qmldir prefer + qrc prefixes)**
  - `f185f7e935` **paintbrush: fit interface window to desktop and fix qml import path**
  - `61c919ed39` **paintbrush: add software-decode launch variant for testing (avcodec-hw=none)**
- The one remaining untracked incidental file is `vlc-help.txt` (a captured `vlc --help` dump; not meant for commit).
- No secrets, no binaries, no build artifacts staged (`.gitignore` covers `.kilo/`, win64-meson build dir, autosave temp files). SAM3 model file and test media are outside the repo.

## 9. Rollback

1. Discard the untracked incidental file: `vlc-help.txt` (CLI help dump).
2. To drop all Phase 3 work: `git reset --hard bf1c6b3dcb` (the Phase 2 checkpoint) — this discards the
   three Phase 3 commits `d4257d11bf`/`268dc265bd`/`1e7ea118b2` while keeping the whole feature from Phase 1 `d7b3d33e82` onward.
3. To disable the feature entirely: `--no-paintbrush` / unset `paintbrush` config; the module is gated by `paintbrush-enabled`.

## 10. Suggested next work (maintainer)

Phase 2 + Phase 3 are now committed as three logical commits on `master`:
`d4257d11bf` (core services), `268dc265bd` (QML), `1e7ea118b2` (handoff docs + launch scripts).
SAM3 inference and tracking are verified *at the API level* (seg + track probes, real model, real frames);
the remaining end-to-end work needs a human driving the live UI (model can't grade vision output):

1. Drive the live end-to-end demo with the model + video on disk: open video → capture frame →
   object select (point click) → drag-refine mask → track across frames → sticker export → collage.
   Note: on this CPU-only build each SAM frame encode is ~75 s (8 threads), so interactive clicks
   are slow but do work; a GPU build (`--paintbrush-sam-device=...` with a CUDA build) would make it fast.
2. Wire STT/TTS providers and the optional lookup provider (config extension points exist).
3. Add slot-scaling for collage image layers (positions + content cut work; pixel-perfect rescale is partial).
4. Consider WebM-alpha sticker export when an alpha-capable encoder is confirmed
   (currently an honest transparent PNG sequence).
5. Optional: `vlc-help.txt` at the repo root is an incidental CLI help dump (left untracked).

## 11. Remaining items (honest, what is NOT done / NOT verified)

- **Not verified:** VLM/generation *answer quality* (provider returns 200; grading needs the user's eyes),
  animated sticker *decode timing* in a real tracked clip (PNG sequence export path is proven by
  the track probe; the in-UI sticker flow needs a human), tablet pressure (no tablet hardware).
- **Not implemented / by design:** track effects (blur/dim/spotlight/outline/attach), object lookup
  vendor, STT/TTS voices, WebM-alpha export, global hotkey/menu item, full `paintbrush.*` log hierarchy,
  thumbnails-while-playing (paused-only by design), pen hover/eraser pressure mapping, pixel-perfect
  collage slot scaling.
- **Explicitly kept honest:** `activeJobs()` (segmentation/tracking/vision/generation/assistant/thumbnails
  with resource class) and `cancelActiveJob()` route to each service's cancel — code-reviewed, not yet
  exercised through the live diagnostics UI.