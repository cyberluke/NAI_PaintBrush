# NAI PaintBrush

**Paint with video. Harvest real moments. Compose living artwork.**

NAI PaintBrush is a **native multimedia painting and composition studio** in the [NANOTRIK NAI software ecosystem](https://github.com/cyberluke/NAI-OS). Start with a blank canvas or an actual video frame, isolate subjects, build layered artwork with physical-looking materials, and export either a high-resolution image or a short **Living Poster**.

This is **not a renamed VLC player or a browser-based drawing wrapper**. It is a dedicated C++/Qt 6/QML creative workspace developed inside a fork of the **VLC 4.0.0-dev** source tree, reusing VLC's native media pipeline while implementing its own document model, compositing, tools, undo system, material rendering, video-object workflow and export path.

**Status:** active development; Windows-first source build, not a verified public binary release. Features below describe implementations in this repository, with the recorded acceptance evidence linked below.

## From video to artwork

1. **Open and navigate video** in the integrated media drawer. Seek, inspect thumbnails and capture an exact source frame.
2. **Select the subject.** Use manual mask editing or configured **SAM3** segmentation and tracking, then refine edges and extract transparent cutouts.
3. **Harvest.** Keep chosen subjects or poses in a document-scoped tray with source-frame / presentation-timestamp provenance.
4. **Compose.** Arrange editable image, text and effect layers; choose paper, color moods, brush marks, stickers and glitter.
5. **Boost.** Analyze a same-source pose sequence, suggest hierarchy and depth, optionally bridge a gap with a newly decoded and segmented frame, and apply the composition as one undoable transaction.
6. **Bring a poster to life.** A time-aware renderer can animate a harvested figure with source-informed, photo-limited motion. Preview it on the canvas and export a Living Poster as PNG frames or, when the required encoders are present, MP4 / transparent WebM.

The creative workflow remains usable without AI models. SAM3, visual understanding, generative edits and assistant functions require their respective configured model/runtime or compatible provider.

## What makes PaintBrush different

| System | Implementation |
| --- | --- |
| **Native canvas and editing** | Qt Quick scene graph; brushes, spray, shapes, selection/mask refinement, pressure-aware pen input, layers, transforms, thumbnails, autosave/recovery and typed undo/redo. |
| **Video-to-art bridge** | VLC-backed decode and frame capture; original-frame provenance, subject extraction, object tracking and a frame-aware Harvest tray. |
| **Art Materials Runtime** | Six procedural paper stocks; bristle brush; physical-state glitter with glue, bonded/loose grains and document lighting; deterministic material state. |
| **Composition engine** | Layer roles; non-destructive shadow, glow, motion blur, outline and ghost effects; six visual moods, editable typography and procedural badges. |
| **Boost + Living Poster** | Deterministic composition/bridge planning, photo-constrained puppet motion, time-aware compositing and reversible edits. |
| **Rendering / delivery** | GPU preview port via Qt **QRhi/D3D11** for paper, glitter and finish effects; CPU reference renderer retained for deterministic acceptance/export; scale-aware 4K image export and encoder-gated video export. |
| **AI-assisted tools** | SAM3 segmentation/tracking, optional VLM object questions, generative actions and a configurable OpenAI-compatible assistant gateway. |

**Important boundaries:** QRhi GPU acceleration is a first-port preview path, not a claim that the whole editor/export pipeline runs on the GPU. The core renderer and exports have separate verification paths. A real SAM3 CPU session can be slow at high resolution. Living Poster output depends on the available FFmpeg binary and its H.264/VP9 encoders.

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

The fork's Meson project identifies **VLC 4.0.0-dev**. Its `contrib/src/ffmpeg/rules.mak` selects **FFmpeg 9.0** for dependency builds. That configuration alone does **not** mean every developer build contains or exposes all FFmpeg encoders: Living Poster MP4/WebM controls explicitly probe the local encoder availability.

Projects are stored in the versioned **`.npb`** format. The document/render layer preserves media references and edits independently of the current playback position.

## Build and run

The documented and exercised PaintBrush target is **Windows + MSYS2/MINGW64 + Qt 6 + Meson/Ninja**. The VLC parent tree is cross-platform, but this README does not assert equivalent PaintBrush build or runtime validation on macOS and Linux.

Read **[the Windows build instructions](modules/gui/qt/paintbrush/BUILDING.md)** before building. They cover the native toolchain, VLC contrib setup, Qt plugin build, plugin cache and known Meson regeneration issues.

After producing the documented `win64-meson` build, launch from the repository root:

```powershell
.\scripts\run-paintbrush.ps1 -NoSam
# With a video and your own configured SAM3 model:
.\scripts\run-paintbrush.ps1 -Media "D:\media\clip.mp4" -SamModel "D:\models\sam3-q4_0.ggml"
```

The root **`Run Paintbrush (latest).cmd`** is a developer convenience launcher that expects a valid local build, not an installer. Do not point users to a generic upstream VLC release and imply it includes PaintBrush.

## Engineering evidence and current limitations

These are **recorded project checks**, not tests rerun by this README change:

- **[PaintBrush 2.0 stage report](_REVIEW/PAINTBRUSH_2_STAGE_REPORT.md):** Art/Harvest/Boost, QRhi GPU first port, editable typography, real-video/SAM3 acceptance and scale-aware image export.
- **[Living Showroom](_REVIEW/PAINTBRUSH_LIVING_SHOWROOM.md):** time-aware canvas preview and real MP4/WebM mux acceptance, with encoder checks.
- **[Living Harvest](_REVIEW/PAINTBRUSH_LIVING_HARVEST.md):** deterministic motion, photo safety limits, provenance, restoration and serialization.
- **[Implementation handoff](HANDOFF.md)** and **[human demo checklist](modules/gui/qt/paintbrush/DEMO.md):** reproduction details and integration caveats.

Some recorded reports describe earlier stages and may contain historical limitations later superseded by the next stage. In particular, GPU output remains an accelerator rather than the authoritative golden export path. The Windows developer environment and configured AI dependencies are currently the best-evidenced setup; reproducible public installer delivery is not established here.

## Product family and upstream credit

NAI PaintBrush is one of several independent NAI applications. **[NAI OS](https://github.com/cyberluke/NAI-OS)** provides the Windows desktop shell and integration surface, not the PaintBrush source code or its rendering runtime.

This repository includes the original **[VideoLAN VLC](https://code.videolan.org/videolan/vlc)** codebase. VLC and its contributors retain attribution for upstream work. The original inherited introduction is preserved as **[README.VLC-UPSTREAM.md](README.VLC-UPSTREAM.md)**. See `AUTHORS`, `THANKS`, `COPYING`, `COPYING.LIB` and third-party license notices for the applicable source and dependency licenses. VLC is licensed under GPL-2.0-or-later and libVLC under LGPL-2.1-or-later; individual bundled components may carry their own notices.

**NAI PaintBrush is a separately developed product built on that foundation.** No affiliation with or endorsement by VideoLAN is implied.
