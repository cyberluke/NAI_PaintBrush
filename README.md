# NAI PaintBrush

**Paint with video. Harvest real moments. Compose living artwork.**

NAI PaintBrush is a native multimedia painting and composition studio. Start
from a blank canvas or an exact video frame, isolate subjects, build layered
artwork with physical-looking materials, and export a high-resolution image or a
short **Living Poster**.

This is not a themed media player. It is a dedicated C++/Qt 6/QML creative
workspace developed inside a fork of the **VLC 4.0.0-dev** source tree: it
reuses VLC's native media pipeline and implements its own document model,
compositing, tools, undo system, material rendering, video-object workflow and
export path.

**Status:** active development, Windows-first source build. The list below
describes implementations in this repository, with recorded acceptance evidence
linked under [Engineering evidence](#engineering-evidence-and-limitations).

---

## From video to artwork

1. **Open and navigate video** in the integrated media drawer — seek, inspect
   thumbnails and capture an exact source frame.
2. **Select the subject** — manual mask editing or configured **SAM3**
   segmentation and tracking, then refine edges and extract transparent
   cutouts.
3. **Harvest** — keep chosen subjects or poses in a document-scoped tray with
   source-frame / presentation-timestamp provenance.
4. **Compose** — arrange editable image, text and effect layers; choose paper,
   color moods, brush marks, stickers and glitter.
5. **Boost** — analyze a same-source pose sequence, suggest hierarchy and depth,
   optionally bridge a gap with a newly decoded and segmented frame, and apply
   the composition as one undoable transaction.
6. **Bring a poster to life** — a time-aware renderer animates a harvested
   figure with source-informed, photo-limited motion. Preview it on the canvas
   and export a Living Poster as PNG frames or, when the required encoders are
   present, MP4 / transparent WebM.

The creative workflow remains usable without AI models. SAM3, visual
understanding, generative edits and assistant functions require their
respective configured model/runtime or compatible provider.

## What makes PaintBrush different

| System | Implementation |
| --- | --- |
| **Native canvas and editing** | Qt Quick scene graph; brushes, spray, shapes, selection/mask refinement, pressure-aware pen input, layers, transforms, thumbnails, autosave/recovery and typed undo/redo. |
| **Video-to-art bridge** | VLC-backed decode and frame capture; original-frame provenance, subject extraction, object tracking and a frame-aware Harvest tray. |
| **Art materials runtime** | Six procedural paper stocks; bristle brush; physical-state glitter with glue, bonded/loose grains and document lighting; deterministic material state. |
| **Composition engine** | Layer roles; non-destructive shadow, glow, motion blur, outline and ghost effects; six visual moods, editable typography and procedural badges. |
| **Boost + Living Poster** | Deterministic composition/bridge planning, photo-constrained puppet motion, time-aware compositing and reversible edits. |
| **Rendering and delivery** | GPU preview path via Qt **QRhi/D3D11** for paper, glitter and finish effects; CPU reference renderer retained for deterministic acceptance/export; scale-aware image export and encoder-gated video export. |
| **AI-assisted tools** | SAM3 segmentation/tracking, optional VLM object questions, generative actions and a configurable OpenAI-compatible assistant gateway. |

**Boundaries.** QRhi GPU acceleration is a preview path, not a claim that the
whole editor/export pipeline runs on the GPU; the core renderer and exports have
separate verification paths. A real SAM3 CPU session can be slow at high
resolution. Living Poster output depends on the available FFmpeg binary and its
H.264/VP9 encoders. The powder material renders live at display rate; the
release bake is still one synchronous pass.

## Architecture: product code, not a theme

```text
modules/gui/qt/paintbrush/
  qml/                 native creative workspace and controls
  pb_document.*        document model and canonical renderer
  pb_layer.*           editable layers / identity / material state
  pb_history.*         typed commands and transactional undo
  pb_frame_capture.*   decoded source frames
  pb_segmentation_*    SAM3 subject selection
  pb_tracking_*        object tracking
  pb_harvest.*         exact-frame asset tray
  pb_boost.*           composition / bridge planning
  pb_export.*          image and Living Poster delivery
  art/                 paper, brushes, glitter, finish, moods, GPU
  motion/              source-informed puppet / Living Poster runtime
```

The Meson project identifies **VLC 4.0.0-dev**, and `contrib/src/ffmpeg/rules.mak`
pins **FFmpeg 9.0** for dependency builds. That pin alone does not mean every
developer build contains or exposes every FFmpeg encoder — the Living Poster
controls probe the local encoder availability.

Projects are stored in the versioned **`.npb`** format. The document/render
layer preserves media references and edits independently of the current
playback position.

## Build (Windows)

The documented and exercised target is **Windows + MSYS2/MINGW64 + Qt 6 +
Meson/Ninja**. The VLC parent tree is cross-platform, but this README does not
assert equivalent PaintBrush build or runtime validation on macOS or Linux.

Prerequisites: MSYS2 with the `mingw64` toolchain, Meson, Ninja, and the Qt 6
runtime that ships in `contrib/x86_64-w64-mingw32`. The build directory is
`win64-meson`. Read
[`modules/gui/qt/paintbrush/BUILDING.md`](modules/gui/qt/paintbrush/BUILDING.md)
first — it covers the toolchain, contrib setup, the Qt plugin loop, the plugin
cache and known Meson regeneration issues.

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

Installable package (signed with the product leaf, see [`SECURITY.md`](SECURITY.md)):

```powershell
pwsh scripts/pb-build-msix.ps1                # core package (~267 MB)
pwsh scripts/pb-build-msix.ps1 -IncludeAI     # + CUDA / matting models
```

Output: `win64-meson\msix\msix\NAI.PaintBrush_<version>_x64.msix`.

Install it (self-elevates through UAC, provisions the NAI trust chain, then
sideloads the package):

```powershell
pwsh scripts/pb-install-msix.ps1
```

## Run

```powershell
pwsh scripts/launch-pb-noskip.ps1
```

The canonical launcher sets the plugin/QML paths and starts the app with the
no-skip decoder profile used for frame-accurate painting. Common switches:

```powershell
# open a video in the workspace (media is never a bare argument)
pwsh scripts/launch-pb-noskip.ps1 -Media D:\clips\dance.mp4

# neural matte self-test
pwsh scripts/launch-pb-noskip.ps1 -MatteSelfTest

# alternative launcher with SAM control
pwsh scripts/run-paintbrush.ps1 -NoSam
pwsh scripts/run-paintbrush.ps1 -Media "D:\media\clip.mp4" -SamModel "D:\models\sam3-q4_0.ggml"

# run the binary directly
win64-meson\bin\nai-paintbrush.exe --paintbrush
```

`--paintbrush` selects the NAI PaintBrush workspace; without it the same binary
starts the standard VLC interface. `Run Paintbrush (latest).cmd` is a developer
convenience launcher that expects a valid local build — do not point users at a
generic upstream VLC release and imply it includes PaintBrush.

## Verification and diagnostics

```powershell
# focus the app, select Glitter powder (Shift+L), drag a stroke, capture
pwsh scripts/pb-powder-verify.ps1 -NoBlow -StepMs 60

# powder cost split on stderr (bake/pack/upload/tick)
$env:PB_POWDER_PERF='1'; pwsh scripts/launch-pb-noskip.ps1
```

Stroke tracing is on by default and appends to `pb_stroke_trace.log`
(16 MB rotation, `PB_POWDER_TRACE=0` disables it). The measured performance
baseline and the remaining performance TODO live in
[`modules/gui/qt/paintbrush/docs/PERFORMANCE_TODO.md`](modules/gui/qt/paintbrush/docs/PERFORMANCE_TODO.md).

Releases are Authenticode-signed with the product leaf issued by the NAI Code
Signing CA (`CN=NAI PaintBrush, O=NANOTRIK.AI, C=US`). See
[`SECURITY.md`](SECURITY.md) for the thumbprint, the verification chain and the
operator signing flow (`pwsh scripts/pb-sign.ps1`).

## Engineering evidence and limitations

Recorded project checks, not tests rerun by this README change:

- [`_REVIEW/PAINTBRUSH_2_STAGE_REPORT.md`](_REVIEW/PAINTBRUSH_2_STAGE_REPORT.md)
  — Art/Harvest/Boost, the QRhi GPU first port, editable typography,
  real-video/SAM3 acceptance and scale-aware image export.
- [`_REVIEW/PAINTBRUSH_LIVING_SHOWROOM.md`](_REVIEW/PAINTBRUSH_LIVING_SHOWROOM.md)
  — time-aware canvas preview and real MP4/WebM mux acceptance with encoder
  checks.
- [`_REVIEW/PAINTBRUSH_LIVING_HARVEST.md`](_REVIEW/PAINTBRUSH_LIVING_HARVEST.md)
  — deterministic motion, photo safety limits, provenance, restoration and
  serialization.
- [`modules/gui/qt/paintbrush/docs/HANDOFF.md`](modules/gui/qt/paintbrush/docs/HANDOFF.md)
  — implementation handoff and integration caveats;
  [`DEMO.md`](modules/gui/qt/paintbrush/DEMO.md) — human demo checklist.

Some reports describe earlier stages and may contain historical limitations
superseded by later work. GPU output remains an accelerator, not the
authoritative golden export path. The Windows developer environment and the
configured AI dependencies are the best-evidenced setup; reproducible public
installer delivery is not established here.

## Repository layout

```
bin/                              application entry point (renamed VLC main)
lib/  src/  include/              libVLC engine core
modules/                          VLC plugins (codecs, video output, interfaces)
modules/gui/qt/paintbrush/        THE PRODUCT: document, canvas, tools,
                                  materials, GPU renderers, matting, tracking
scripts/                          build, launch, capture, verification tools
_REVIEW/                          recorded stage reports and render harnesses
```

## Product family and upstream credit

NAI PaintBrush is one of several independent NAI applications.
[NAI OS](https://github.com/cyberluke/NAI-OS) provides the Windows desktop shell
and integration surface, not the PaintBrush source code or its rendering
runtime.

This repository includes the original
[VideoLAN VLC](https://code.videolan.org/videolan/vlc) codebase. VLC and its
contributors retain attribution for upstream work; the inherited introduction
is preserved verbatim as
[`README.VLC-UPSTREAM.md`](README.VLC-UPSTREAM.md).

## License

- The combined application is distributed under **GPLv2 or later**. See
  [`COPYING`](COPYING).
- libVLC, the engine, is under **LGPLv2.1 or later**. See
  [`COPYING.LIB`](COPYING.LIB).
- Upstream source: <https://code.videolan.org/videolan/vlc>
- Upstream authors and contributors: [`AUTHORS`](AUTHORS), [`THANKS`](THANKS).
- Individual bundled components may carry their own notices.

Changes in this repository are the NAI PaintBrush application layer under
`modules/gui/qt/paintbrush/`, the link-time rename of the main executable, and
the build/verification scripts under `scripts/`. They are published under the
same GPLv2-or-later terms.

**NAI PaintBrush is a separately developed product built on that foundation. No
affiliation with or endorsement by VideoLAN is implied.**
