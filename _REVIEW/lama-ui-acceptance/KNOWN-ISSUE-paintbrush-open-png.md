# Known issue — `--paintbrush-open=<png>` starts the media player, not the image canvas

Status: **open, not fixed** (recorded 2026-11-24 during the LaMa UI acceptance gate).
Scope: Paintbrush launch path only. This is **not** a LaMa defect and must not be
folded into the LaMa provider or its acceptance evidence.

## Symptom

Launching the real app with an image path:

```powershell
nai-paintbrush.exe --paintbrush --paintbrush-open=C:\...\live_frame_B.png
```

produces a blank 1280x720 canvas and a status line `Playing live_frame_B.png`
instead of opening the PNG as an image layer on a 960x540 canvas.

Observed in the LaMa UI acceptance run (`_REVIEW/lama-ui-acceptance/`): the
launch script passed the PNG through `--paintbrush-open`; the canvas stayed
blank until the test called the controller's `openImageFile()` explicitly. The
gate was completed through `openImageFile` and is not affected by this issue.

## Cause (code pointer, not a fix)

`PaintbrushController` routes every `--paintbrush-open` value through
`openVideoFile()` after a 500 ms timer:

- `modules/gui/qt/paintbrush/pb_controller.cpp` (constructor, the
  `paintbrush-open` block) — unconditionally calls `openVideoFile(url)`.
- `openVideoFile()` calls `enterHarvestPresentation()`, appends the file to the
  playback playlist and sets `Playing <file>`.
- `openImageFile()` (the correct path for a still image) is never consulted and
  there is no extension/MIME branch.

The routing was intentional for video: a bare playlist argument used to create a
separate native VLC video window, so entering Harvest first keeps the vout
embedded. Images were not handled in that decision.

## Expected

A still image passed to `--paintbrush-open` should open through the same path as
**Open picture** (`openImageFile`) and land on the image canvas. Video should
keep the current Harvest/playback behavior.

## Workaround

- Use **Open picture** in the app, or call `openImageFile()` (the acceptance hook
  does this), instead of `--paintbrush-open` with a PNG.
- `--paintbrush-open` remains correct for video files.

## Why it is recorded here

The LaMa acceptance gate depended on loading a still image. The launch-path gap
was observed and worked around, but fixing it would change Harvest/playback
routing and is outside the frozen LaMa scope. It is kept as a separate
known-issue note so the LaMa evidence stays truthful and this defect is not lost.
