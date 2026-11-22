# NAI Paintbrush

**An AI-native paint and video workstation, built on the VLC 4 engine.**

NAI Paintbrush is one editor and one document system for digital painting,
editable vector illustration, non-destructive compositing, and turning
real-world subjects extracted from video into editable artwork. It runs as a
single native Windows application (`nai-paintbrush.exe`) on top of libVLC's
playback, decode and filter core.

> This repository is a derivative of [VLC](https://www.videolan.org/) focused on
> the Paintbrush application. The VLC engine remains under its original
> licenses — see [License and upstream](#license-and-upstream).

---

## What it is

One document, one toolset, no mode switches:

- **Painting** — pressure-aware brushes, layers, non-destructive edits, undo
  across the whole document.
- **Vector + raster** — editable vector paths live inside the same layer stack
  as raster paint (`LAYER_VECTOR`, in-canvas node editing).
- **Compositing** — layer transforms, effects (blur, shadow, glow), paper and
  material surfaces.
- **Glitter powder material** — a real powder simulation rendered by the GPU
  powder shader (mirror flakes, holographic sheen, gravity, blow). Live preview
  runs at display rate.
- **Video subjects** — reference monitor, frame capture into the canvas, object
  selection (SAM3), neural alpha matting (ViTMatte via ONNX Runtime + CUDA), and
  temporal tracking for rotoscoping.
- **Adaptive UI** — the tool strip and options bar are driven by semantic tool
  capabilities, not by fixed cells.

Product contract: [`modules/gui/qt/paintbrush/docs/ADAPTIVE_UI_AND_SUBJECTS.md`](modules/gui/qt/paintbrush/docs/ADAPTIVE_UI_AND_SUBJECTS.md).

## Status

Active development, Windows-first (MSYS2 / MinGW64, Qt 6.8, D3D11 RHI).
The product contract is partially implemented; its status line is the honest
list of what is real and what is pending. Operational notes, build history and
known gaps live in
[`modules/gui/qt/paintbrush/docs/HANDOFF.md`](modules/gui/qt/paintbrush/docs/HANDOFF.md).
Performance work and its measured baseline live in
[`modules/gui/qt/paintbrush/docs/PERFORMANCE_TODO.md`](modules/gui/qt/paintbrush/docs/PERFORMANCE_TODO.md).

## Build (Windows)

Prerequisites: MSYS2 with the `mingw64` toolchain, Meson, Ninja, and the Qt 6
runtime that ships in `contrib/x86_64-w64-mingw32`. The build directory is
`win64-meson`.

Fast loop while working on the application layer (rebuilds the Qt plugin and
regenerates the plugin cache):

```powershell
pwsh scripts/pb-build-plugin.ps1
```

Full build (also relinks the application):

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
ninja -C win64-meson
```

Output:

```
win64-meson\bin\nai-paintbrush.exe
```

The executable is the VLC engine binary renamed at link time
(`bin/meson.build`), so the process and taskbar never clash with a media
player. No wrapper, no copy step.

## Run

```powershell
pwsh scripts/launch-pb-noskip.ps1
```

The launcher sets the plugin/QML paths and starts the app with the no-skip
decoder profile used for frame-accurate painting. Useful switches:

```powershell
# open a video in the workspace (never pass media as a bare argument)
pwsh scripts/launch-pb-noskip.ps1 -Media D:\clips\dance.mp4

# neural matte self-test
pwsh scripts/launch-pb-noskip.ps1 -MatteSelfTest

# run the binary directly
win64-meson\bin\nai-paintbrush.exe --paintbrush
```

`--paintbrush` selects the NAI Paintbrush workspace. Without it the same binary
starts the standard VLC interface.

## Repository layout

```
bin/                              application entry point (the renamed VLC main)
lib/  src/  include/              libVLC engine core
modules/                          VLC plugins (codecs, video output, interfaces)
modules/gui/qt/paintbrush/        THE PRODUCT: Qt Quick UI, canvas, document,
                                  materials, GPU renderers, matting, tracking
scripts/                          build, launch, capture and verification tools
docs (paintbrush)/                product contract, handoff lamp, performance
```

## Verification and diagnostics

```powershell
# focus the app, select Glitter powder (Shift+L), drag a stroke, capture
pwsh scripts/pb-powder-verify.ps1 -NoBlow -StepMs 60

# powder cost split on stderr (bake/pack/upload/tick)
$env:PB_POWDER_PERF='1'; pwsh scripts/launch-pb-noskip.ps1
```

Stroke tracing is on by default and appends to `pb_stroke_trace.log`
(16 MB rotation, `PB_POWDER_TRACE=0` disables it).

## License and upstream

NAI Paintbrush is a derivative work of **VLC media player** by the
[VideoLAN project](https://www.videolan.org/), used under the GNU General
Public License.

- The combined application is distributed under **GPLv2 or later**. See
  [`COPYING`](COPYING).
- libVLC, the engine, is under **LGPLv2 or later**. See
  [`COPYING.LIB`](COPYING.LIB).
- Upstream source: <https://code.videolan.org/videolan/vlc>
- Upstream authors and contributors: [`AUTHORS`](AUTHORS).

Changes in this repository are the NAI Paintbrush application layer under
`modules/gui/qt/paintbrush/`, the link-time rename of the main executable, and
the build/verification scripts under `scripts/`. They are published under the
same GPLv2-or-later terms.
