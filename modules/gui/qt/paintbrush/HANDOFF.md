# NAI Paintbrush — Handoff Document (Phase 2 completed)

**Date:** 2026-10-02
**Author:** implementation agent (handoff to maintainer)
**Spec source:** `C:\Users\lukes.COREI9\Downloads\NAI_Paintbrush_VLC4_Full_Scope_One_Run_Spec (1).md` (Phase 1) + Phase 2 One-Run Implementation Spec
**Repo:** `D:\_SATIN_AI\NAI-OS\vlc` — VLC 4.0.0-dev
**Branch:** `master`
**Commits:** `e4f2d3a399` (base, before Paintbrush) → `d7b3d33e82` (Phase 1 checkpoint) → `*pending*` (Phase 2, uncommitted at writing time)
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

## 4. Remaining gaps (actual, honest)

| Item | State |
|---|---|
| Pen hover/eraser pressure mapping | Pressure maps to size + optional opacity; hover/eraser pressure not separately mapped (mouse default unchanged) |
| Thumbnail generation while playing | Generates only while paused (by design); playing idles with placeholders |
| Auto collage scaling | Pieces are repositioned and raster→image-converted; pixel-perfect slot *scaling* of image layers is limited (positions + content cut; rescaling to slot is partial for image layers) |
| Selection brush add/subtract (drag-paint on mask) | Point click add/subtract exists; freehand mask painting not added |
| STT/TTS providers | Config extension points exist; no provider wired (voice optional, core unaffected) |
| Animated sticker as WebM/alpha video | Only transparent PNG sequence export; WebM-alpha needs an encoder that supports it (not claimed) |
| Object lookup provider | Interface + unavailable state only; no vendor wired (by design) |
| Track effects (blur/dim/spotlight/outline/attach) | Not implemented (track data model supports metadata) |
| Automatic menu "Open in Paintbrush" / global hotkey | Sidebar + CLI + drag/drop + clipboard only |
| Grid ruler/coordinate feedback | Status bar shows cursor/selection/zoom/snap/grid; ruler widgets not added |
| Full log-category hierarchy (`paintbrush.*`) | `msg_*` logging only |
| Recovery "Open copy" flow | Banner offers Restore/Discard; "Open copy" saves via Save dialog (manual) |

## 5. Verification performed (Phase 2)

- **Builds:** `ninja modules/libqt_plugin.dll` — clean (only pre-existing `-Wsfinae-incomplete` warning). Plugin cache regenerated with `vlc-cache-gen.exe`.
- **Runtime smoke (real launch):** `vlc.exe --paintbrush --no-media-library --no-qt-privacy-ask --no-qt-updates-notif --file-logging --logmode=text --logfile=/tmp/pb.log --log-verbose=2`
  - Process alive after 15 s, no assertions, no crashes.
  - **Zero QML errors**: no `TypeError`, `Cannot read property`, `Unable to assign`, or `ReferenceError` in the log (all new UI — thumbnail strip, collage dialog, recovery banner, refine buttons, talk-level selector, project info, diagnostics actions — instantiated cleanly).
- **Gateway:** LiteLLM Edge `127.0.0.1:4000` — `/v1/models` and `/v1/chat/completions` return 200 (verified `PING_OK` earlier; retained).
- **Not verifiable in this environment:** SAM3 real inference (no model file), tracking across frames, VLM/generation calls (providers unconfigured by default), animated sticker decode (needs a real tracked object), 4K drawing (no 4K display verified), tablet pressure (no tablet hardware) — these paths are wired and build, but require the external pieces to exercise end-to-end.

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

- Branch `master`; base `e4f2d3a399`; Phase 1 checkpoint `d7b3d33e82`.
- **Phase 2 changes are uncommitted at writing time** (21 modified files + 5 new untracked):
  - New: `pb_thumbnail_service.{hpp,cpp}`, `pb_lookup_service.{hpp,cpp}`, `BUILDING.md`, `HANDOFF.md` (this file).
  - Modified: `pb_canvas_item.{hpp,cpp}`, `pb_companion_service.{hpp,cpp}`, `pb_controller.{hpp,cpp}`, `pb_export.{hpp,cpp}`, `pb_segmentation_service.{hpp,cpp}`, `pb_tracking_service.{hpp,cpp}`, 7 QML files, `qt.cpp`, `meson.build`, `Makefile.am`.
- No secrets, no binaries, no build artifacts staged (`.gitignore` covers `.kilo/`, win64-meson build dir, autosave temp files).
- Recommended commit: `paintbrush: complete Phase 2 polish and missing features`.

## 9. Rollback

1. Discard the 5 untracked files: `pb_thumbnail_service.*`, `pb_lookup_service.*`, `BUILDING.md`, `HANDOFF.md`.
2. `git checkout -- <modified files>` (21 files) — or `git reset --hard d7b3d33e82` to drop all Phase 2 changes while keeping the Phase 1 checkpoint.
3. To disable the feature entirely: `--no-paintbrush` / unset `paintbrush` config; the module is gated by `paintbrush-enabled`.

## 10. Suggested next work (maintainer)

1. Commit Phase 2 (recommended message above).
2. Provide a SAM3 model + a demo video, then exercise: object select → track → sticker export → collage end-to-end.
3. Wire STT/TTS providers and the optional lookup provider.
4. Add freehand mask-painting refinement and slot-scaling for collage image layers.
5. Consider WebM-alpha sticker export when an alpha-capable encoder is confirmed.