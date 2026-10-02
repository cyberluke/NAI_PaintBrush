# NAI Paintbrush — Implementation Notes

Phase A reconnaissance record and architecture decisions for the NAI
Paintbrush creative workspace integrated into the VLC 4 Qt/QML interface.

## Repository state (at implementation time)

- Branch: `master` (VLC 4.0.0-dev)
- Build system: Meson (primary) + autotools (`Makefile.am`) both updated.
- Qt module: `modules/gui/qt` — native Qt 6 / QML interface.
- QML modules are registered in `modules/gui/qt/meson.build` (`qml_modules`
  list, `VLC.*` namespaces, generated `qmldir`/`res.qrc` via
  `scripts/genqrc.py` / `scripts/genqmldir.py`) and mirrored in
  `modules/gui/qt/Makefile.am`.

## Key VLC facilities reused

- **Interface modes**: `MainCtx::MainInterfaceMode` flags
  (`MINIMAL > PLAYER > PAINTBRUSH > MAINDISPLAY` priority). A new
  `MAININTERFACE_MODE_PAINTBRUSH = 8` flag + `MainCtx.paintbrushView`
  property drives the `MainViewLoader` in `MainInterface.qml`.
- **Navigation**: `NavigationModel` sidebar entry (`paintbrush` URI) is
  handled specially in `MainDisplay.qml` (switches interface mode instead
  of navigating); gated by the `paintbrush-enabled` feature flag.
- **Player state**: entering Paintbrush never clears `playerView`, so
  returning to VLC restores the same media item, timestamp and paused
  state. The workspace "Back to VLC" button returns to the player view
  when a video is loaded.
- **Frame capture**: `vlc_player_vout_Hold()` + `vout_GetSnapshot(..., "png")`
  (`pb_frame_capture.cpp`) — real vout snapshot, no window screenshots.
- **Player stepping**: `vlc_player_NextVideoFrame` / `PreviousVideoFrame`
  with the `on_next_frame_status` listener callback (tracking service).
- **Playlist control**: `MainPlaylistController` for play/pause/stop.
- **Config**: new `paintbrush-*` options declared in `modules/gui/qt/qt.cpp`
  through the standard `add_bool/add_string/add_integer_with_range`
  machinery (including the `--paintbrush` startup option read in
  `MainCtx::loadPrefs`).
- **Logging/theme**: VLC's existing infrastructure; the workspace uses the
  standard `ColorContext`/`VLCStyle` theming.

## SAM3 integration (segmentation + tracking)

- The vendored SAM3 API (from the `sam3` contrib, `sam3.h` v1.0.0):
  - `sam3_load_model` / `sam3_create_state` / `sam3_encode_image`
  - `sam3_segment_pvs` (point clicks), `sam3_segment_pcs` (text prompt)
  - **Real temporal tracking**: `sam3_create_visual_tracker`,
    `sam3_tracker_add_instance`, `sam3_propagate_frame`,
    `sam3_refine_instance` (corrections).
- `SegmentationService` runs model load / encode / inference on a dedicated
  `std::thread`; the frame embedding is cached by a byte hash so refinement
  clicks on the same paused frame do not re-encode.
- `TrackingService` drives the player frame-by-frame on the GUI thread and
  runs propagation on a worker thread; masks are keyframed per frame with
  confidence and correction flags.
- The existing `modules/video_filter/sam3.cpp` filter is untouched.
- Compile gating: `dependency('sam3', required: false)` in the qt module;
  `-DHAVE_SAM3` is added only when the contrib is present. Without it the
  services report an explicit "not compiled in" unavailable state.

## Directory layout

```
modules/gui/qt/paintbrush/
├─ pb_common.hpp/.cpp            shared helpers, tools enums, mask/path utils
├─ pb_layer.hpp/.cpp             CanvasLayer data
├─ pb_document.hpp/.cpp          PaintbrushDocument model + JSON snapshots
├─ pb_history.hpp/.cpp           UndoStack (region deltas + structure ops)
├─ pb_canvas_item.hpp/.cpp       tile-based QQuickItem raster canvas
├─ pb_controller.hpp/.cpp        QML singleton controller (PaintbrushCtx)
├─ pb_frame_capture.hpp/.cpp     vout snapshot frame bridge
├─ pb_segmentation_service.hpp/.cpp  SAM3 point/text segmentation (async)
├─ pb_tracking_service.hpp/.cpp  SAM3 temporal tracking
├─ pb_vision_service.hpp/.cpp    OpenAI-compatible VLM ("Ask about this")
├─ pb_generation_service.hpp/.cpp OpenAI-compatible image edits provider
├─ pb_companion_service.hpp/.cpp event-driven creative companion
├─ pb_project_store.hpp/.cpp     versioned .npb JSON project format
├─ pb_export.hpp/.cpp            PNG/JPEG/WebP/mask export
└─ qml/                          Paintbrush workspace UI (8 files)
```

## Verification performed

- `g++ -fsyntax-only` on all 14 paintbrush `.cpp` files against the real
  in-tree VLC headers + Qt 6.11 headers, with a faithful `sam3.h` API stub
  and `-DHAVE_SAM3` (both backend-on and backend-off paths).
- `moc` + compile of the generated meta-object code for all 9 Q_OBJECT
  headers.
- Modified VLC integration files (`mainctx.cpp`, `navigationmodel.cpp`,
  `mainui.cpp`) compile; `qt.cpp` additions follow the existing option
  patterns (its full verification is blocked by stub config limitations).
- `qmllint` on all Paintbrush QML files (no syntax/real errors) and the
  modified `MainInterface.qml`/`MainDisplay.qml`.
- `meson setup` parses the whole tree including the qt module changes
  (fails only at missing contrib dependencies, as expected on a machine
  without the VLC contrib build).

## Explicitly unavailable / deferred (by design, never faked)

- STT/TTS voice: config extension points exist (`paintbrush-stt-*`,
  `paintbrush-tts-*`), no provider wired — voice is optional.
- Video export via transcode: not implemented; masks/tracks export as PNG.
- Print: excluded per spec (no new print stack).
- Menu/hotkey entry points: sidebar + CLI + drag/drop + clipboard are
  implemented; menubar/hotkey integration deferred.
- Web search extension point: disabled unless configured.