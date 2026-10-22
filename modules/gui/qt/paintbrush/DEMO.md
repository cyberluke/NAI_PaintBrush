# NAI Paintbrush — 21-step end-to-end demo checklist

A human-driven walkthrough of the Phase 3 feature set against a real video + SAM3
model (Phase 4 additions noted inline). The implementation agent validated every step's
underlying service path (see `HANDOFF.md` §5); the live clicks below need a human
because video/AI output grading requires eyes and because the offscreen-automation harness cannot judge vision.

**Setup (once):**
- Build: `cd win64-meson && export PATH=/mingw64/bin:/usr/bin:<contrib>/bin && ninja modules/libqt_plugin.dll`
- Regenerate plugin cache: full PATH incl. `win64-meson\bin;lib;src;modules`, then `bin\vlc-cache-gen.exe ./modules`.
- Launch: `scripts\run-paintbrush.ps1 -Media "C:\Users\lukes.COREI9\Videos\aespa-supernova.mp4"`
  (adds `--paintbrush-sam-model=D:\_SATIN_AI\models\sam3-q4_0.ggml --paintbrush-sam-device=cpu`).
  Expect ~595 modules loaded and the Paintbrush workspace (15 tools) + media drawer.
  Note: CPU SAM encode is ~75 s/frame at 2190×1262 → each click below that triggers SAM is slow.

---

## 1. Workspace boots clean
New blank canvas, opaque (no video bleed), tools rail (Sel, Lasso, Object, Pencil, Brush,
Eraser, Fill, Pick, Spray, Line, Rect, Rnd, Oval, Text, Pan), inspector + layers docked.

## 2. Open a video from inside Paintbrush
"Open video…" → pick `aespa-supernova.mp4` → it plays in the drawer, timeline auto-generates thumbnails.

## 3. Pause and scrub without pixelation
Pause (Space or Play/pause). Drag the seek slider to several positions. The frame that settles
must be a clean decoded frame, NOT a blocky mid-GOP artifact (this was the pixelation bug; now fixed —
see HANDOFF §5 seek-drag evidence).

## 4. Capture a frame
"Use this frame" → a `LAYER_REFERENCE` image layer lands on the canvas.

## 5. Object Select — point prompt
Select "Object", click a point on an object (e.g. an artist). SAM3 encodes (~75 s) then returns a mask
(outline overlay). Status goes Object Select → Segmenting → ready.

## 6. Drag-refine the mask
With Object active, left-drag adds mask, Alt/right-drag subtracts. Overlap/no-gap behavior was
validated by `mask_paint_probe.cpp`.

## 7. Track the object across frames
"Track" → the service adds the instance and propagates the mask forward (validated by `sam3_track_probe.cpp`:
instance id stable across 4 frames, bbox/centroid follow motion).

## 8. Ask about this (VLM)
Select the object, ask e.g. "What is this artist wearing?" → gateway returns 200; verify the answer
references the true captured frame (pristine path), not a canvas composite.

## 9. Generative fill / replace background
Select a region, "Generative fill…" / "Replace background…" → generation job runs (provider 200).

## 10. Extract object to its own canvas
"Extract object…" → a new canvas with the cutout as an image layer.

## 11. Build a collage
"Auto collage…" → pieces are repositioned + raster→image converted onto the canvas.

## 12. Add text / shapes / sticker art
Use Text, Line/Rect/Oval/Spray on a layer; undo/redo; reorder + duplicate layers.

## 13. Selections
Grow, shrink, feather, smooth, invert, fill holes, crop-to-selection via the inspector selection actions.

## 14. Thumbnails
"Generate thumbnails" — bounded 80 ms/tick batch, anchor-first, works while paused.

## 15. Project info + diagnostics
"Project info" dialog shows canvas/layers/media/tracks/AI history/autosave; "Diagnostics" shows
service health + active jobs; cancel a job from the diagnostics list.

## 16. Save project, then crash-recovery
Save project → then force-close the app → relaunch → the recovery banner appears
(Restore / Open copy… / Discard).
- **Restore** reloads the unsaved work.
- **Open copy…** (Phase 4) opens the recovered autosave as an unsaved copy and
  *keeps the original recovery file on disk*, so the autosave is not consumed by
  merely viewing a copy.
- **Discard** removes the recovery data.

## 17. Export image (PNG/JPEG, scale, presets)
"Export image…" → pick PNG/JPEG, set scale or a preset (Canvas / Transparent Sticker / Social Post /
Wallpaper), native Save dialog → confirm a file is written (a 1280×720 RGBA PNG was produced this session).

## 18. Export sticker (tracked object → PNG sequence)
With a tracked object, export the stick track → `sticker_%04d.png` transparent sequence
(honest PNG sequence; WebM-alpha requires an alpha-capable encoder).

## 19. Playback coexistence
Play the video to the end → the canvas/media drawer stay responsive; the app shuts down cleanly
(no crash; log ends with `QApp exec() finished`).

## 20. Back to VLC
"Back to VLC" returns to the normal interface, then reopen Paintbrush — state persists.

## 21. Crash-free sustained use
Run the whole flow twice back-to-back; confirm zero assertions / zero unhandled exceptions and
the log stays clean (no `TypeError`, `Cannot read property`, or SAM model-load failures).

---

## Notes
- SAM3 + tracking are validated at the API level against the real model + real frames
  (`sam3_seg_probe`, `sam3_track_probe`): model loads, encode ~75 s/frame CPU, point-segment
  ~370–450 ms, tracking instance stable across 4 frames.
- The `21` steps map onto the full feature surface; steps 3, 5–9, 14, 16–18 are where a human
  eyes-on confirmation adds the most value.

## Phase 4 additions (post-merge, built and launched clean this session)
- **Coordinate rulers** — top/left pixel-coordinate rulers appear at the canvas edges (they share
  the grid toggle, `G`). Ticks stay aligned with the document while you pan/zoom; density adapts to
  the zoom level.
- **Pixel-perfect collage scaling** — auto collage now bakes each piece's scale/rotation into its
  raster, so image/generated layers land exactly in their slot (previously the scale was only a
  render-time transform with an ambiguous pivot).
- **Recovery "Open copy…"** — wired to open the recovered autosave as an unsaved copy while keeping
  the original recovery file on disk (see step 16).