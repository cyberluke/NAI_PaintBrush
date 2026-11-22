# NAI PaintBrush

![NAI PaintBrush](docs/assets/screenshot.png)

### Create like nobody's watching.<br>Catch the frame everyone missed.<br>Glitter like it's couture.<br>**Make it sliving.** ✨

NAI PaintBrush is an AI-assisted art studio for people who want to make
something unforgettable: paint, layer premium glitter powder, cut yourself out
of a video, and turn it all into a poster that moves.

**Paint with video. Catch real moments. Compose sliving artwork.**

> Sampling built the best records. The Prodigy pulled moments off the radio and
> made a hit; you pull a frame out of a video, cut out the part that matters,
> and flip it into something unmistakably yours.

[Download](#download) · [The loop](#the-loop) · [What it does](#what-it-does) · [The AI part](#the-ai-part-honestly) · [For the nerds](#for-the-nerds)

---

## The loop

1. **Start with a sketch, a photo, or a frame you caught from video.**
2. **Add premium glitter powder.** Real GPU glitter: pour it, glue it, tilt it,
   blow it. This is the feature. You will not stop.
3. **Style it up.** Paper, moods, stickers, text, shapes, layers that don't
   wreck each other.
4. **Steal yourself from a video.** Pick a frame, select yourself, cut it out,
   now you're a sticker.
5. **Make it move.** Living Poster turns the composition into a short animated
   poster.
6. **Export and post it.** PNG, MP4, or transparent WebM.

*Mommy energy: everything you make is beautiful, and then you add glitter.*

## What it does

| | |
|---|---|
| ✨ **Premium glitter powder** | A real powder simulation on the GPU: grains, glue, gravity, blow, holographic mirror flakes. Live at display rate. |
| 🖌️ **Draw** | Pressure-aware brushes, spray, shapes, text, layers, transforms, and an undo that actually undoes. |
| 🧷 **Video to sticker** | Open a video, grab a frame, select the subject with SAM3, cut it out with neural matting (ViTMatte), track it across frames. |
| 🎞️ **Living Poster** | A time-aware renderer animates a harvested figure; preview it on the canvas, export a short poster. |
| 🖼️ **Export** | Scale-aware PNG, plus MP4 / transparent WebM when the local encoders exist. |
| 🔒 **Local-first** | No account, no cloud, no "sign in to draw". Files on your PC. |

## Download

> **The signed MSIX is hosted on our R2 bucket — link in the release post.**
<!-- RELEASE: put the R2 URL here -->

- **Package:** `NAI.PaintBrush_1.0.0.0_x64.msix`
- **Size:** 2.4 GB (core app + SAM3 + ViTMatte + CUDA/cuDNN runtime)
- **SHA256:** `2FEE5A832079B0FD1C2D715E7FE535561DB9DBB4A951C37145A9902EFFCCCF1B`
- **Signed by:** `CN=NAI Paint Brush, O=NANOTRIK.AI, C=US` (`49F660D8…`)

Windows 10 1809+ / Windows 11, x64.

```powershell
# one command: UAC once, provisions the NAI trust chain, installs the MSIX
pwsh scripts/pb-install-msix.ps1
```

Verify what you downloaded before installing:

```powershell
Get-FileHash .\NAI.PaintBrush_1.0.0.0_x64.msix -Algorithm SHA256
Get-AuthenticodeSignature .\NAI.PaintBrush_1.0.0.0_x64.msix |
  Format-List Status, SignerCertificate
```

Object Select, matting and tracking work out of the box: the package ships
SAM3 (q4) and ViTMatte plus the CUDA/cuDNN runtime. A recent NVIDIA driver is
enough — no CUDA Toolkit or cuDNN install. No GPU? Everything still runs on the
CPU, just slower.

## The AI part (honestly)

- **Bundled:** SAM3 weights (SAM License), ViTMatte (Apache-2.0), ONNX Runtime,
  CUDA/cuDNN redistributables.
- **Bring your own:** the assistant / generative actions need an
  OpenAI-compatible endpoint and your key. Drawing, glitter and stickers need
  no key.

## For the nerds

A native C++/Qt 6/QML creative workspace, not a themed media player and not a
web wrapper. It reuses VLC's media pipeline and adds its own document model,
compositing, tools, undo, material rendering, video-object workflow and export
path.

**Engine base:** VLC 4.0.0-dev at commit `e4f2d3a399` (2026-09-28), the parent
of the first PaintBrush commit. This is a snapshot fork: the engine is **not**
continuously merged with upstream yet; the upstream merge is a roadmap item
before the next major version. Upstream: <https://code.videolan.org/videolan/vlc>.

### Build (Windows)

MSYS2 `mingw64`, Meson, Ninja, Qt 6 from the contrib/MSYS tree. The build
directory is `win64-meson`. See
[`modules/gui/qt/paintbrush/BUILDING.md`](modules/gui/qt/paintbrush/BUILDING.md).

```powershell
# fast loop while working on the UI
pwsh scripts/pb-build-plugin.ps1

# full build (relinks the app)
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
ninja -C win64-meson
```

Output: `win64-meson\bin\nai-paintbrush.exe` (the VLC engine binary renamed at
link time). Run it with `pwsh scripts/launch-pb-noskip.ps1`.

```powershell
# signed MSIX (core)
pwsh scripts/pb-build-msix.ps1
# signed MSIX with AI models + CUDA runtime
pwsh scripts/pb-build-msix.ps1 -IncludeAI -SamModel D:\models\sam3-q4_0.ggml
```

### Layout

```text
bin/                      application entry point (renamed VLC main)
lib/ src/ include/        libVLC engine core
modules/                  VLC plugins and the Qt interface
modules/gui/qt/paintbrush  THE PRODUCT: document, canvas, tools, materials,
                          GPU renderers (powder, paper), matting, tracking
scripts/                  build, launch, capture, verification, packaging
packaging/msix/           manifest, assets, license texts
```

Performance ledger: [`docs/PERFORMANCE_TODO.md`](modules/gui/qt/paintbrush/docs/PERFORMANCE_TODO.md)
(powder bake 3.7 ms, whole input tick 2.6 ms, 60 fps live preview).

### Evidence

- [`_REVIEW/PAINTBRUSH_2_STAGE_REPORT.md`](_REVIEW/PAINTBRUSH_2_STAGE_REPORT.md)
- [`_REVIEW/PAINTBRUSH_LIVING_SHOWROOM.md`](_REVIEW/PAINTBRUSH_LIVING_SHOWROOM.md)
- [`_REVIEW/PAINTBRUSH_LIVING_HARVEST.md`](_REVIEW/PAINTBRUSH_LIVING_HARVEST.md)
- [`modules/gui/qt/paintbrush/docs/HANDOFF.md`](modules/gui/qt/paintbrush/docs/HANDOFF.md)

## License

- Combined application: **GPL-2.0-or-later** — [`COPYING`](COPYING).
- libVLC engine: **LGPL-2.1-or-later** — [`COPYING.LIB`](COPYING.LIB).
- Third-party binaries and bundled AI models: [`LICENSE-THIRD-PARTY.md`](LICENSE-THIRD-PARTY.md),
  with the full texts in [`packaging/msix/licenses/`](packaging/msix/licenses/).
- Inherited VLC README: [`README.VLC-UPSTREAM.md`](README.VLC-UPSTREAM.md).

NAI PaintBrush is an independent product built on the VLC engine. No affiliation
with or endorsement by VideoLAN is implied.
