# Third-party notices — NAI PaintBrush

NAI PaintBrush is a derivative work of **VLC media player** and distributes
third-party binaries. This file summarizes the components that ship inside the
application and the MSIX package. It is a notice, not a replacement for the
upstream license texts.

## NAI PaintBrush and the VLC engine

| Component | License | Where |
|---|---|---|
| NAI PaintBrush application layer (`modules/gui/qt/paintbrush/`) | GPL-2.0-or-later | [`COPYING`](COPYING) |
| VLC engine (`libvlc`, `libvlccore`, `modules/`) | LGPL-2.1-or-later / GPL-2.0-or-later | [`COPYING.LIB`](COPYING.LIB), [`COPYING`](COPYING) |
| Inherited VLC README and attribution | — | [`README.VLC-UPSTREAM.md`](README.VLC-UPSTREAM.md), [`AUTHORS`](AUTHORS) |

VLC is a registered trademark of the VideoLAN non-profit organization. NAI
PaintBrush is an independent product; no affiliation with or endorsement by
VideoLAN is implied.

**Corresponding source.** The complete corresponding source for a release is
this repository at the commit named in the release notes. If you received only
a binary, the source is available from the project repository under the same
GPL-2.0-or-later terms.

## Bundled runtime components

| Component | License | Notes |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Quick, Qml, Network, …) | LGPL-3.0 / GPL-3.0 (Qt Company) | Dynamically linked; replaceable DLLs. Source: <https://code.qt.io/> |
| FFmpeg (libavcodec, libavformat, libavutil, libswscale, …) | LGPL-2.1-or-later | Built by VLC contrib **without** `--enable-gpl`; see `contrib/src/ffmpeg/`. |
| ICU | Unicode License | |
| libpng, zlib, bzip2, brotli, freetype, harfbuzz, libxml2, sqlite3, and the remaining VLC contrib libraries | Their upstream licenses | See the individual `contrib/src/*/` rules and upstream projects. |
| MinGW-w64 runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) | GPL-3.0 with the GCC Runtime Library Exception; BSD for MinGW-w64 | Required for the MinGW-built binaries. |
| Windows system libraries | Microsoft | Not redistributed. |

## Optional components (`-IncludeAI` package)

| Component | License | Notes |
|---|---|---|
| NVIDIA CUDA runtime (`cublas`, `cublasLt`, `cudart`, `cudnn`, …) | NVIDIA CUDA Toolkit EULA / cuDNN SLA | Redistributable runtime per NVIDIA's terms; see <https://docs.nvidia.com/cuda/eula/>. |
| ONNX Runtime (`pb_matting_cuda.dll` dependency) | MIT | <https://github.com/microsoft/onnxruntime> |
| ggml / ggml-cuda | MIT | <https://github.com/ggerganov/ggml> |

**AI models are bundled in the `-IncludeAI` package** (they are separate from
the core package, which runs the painting workflow without them):

| Component | License | Notes |
|---|---|---|
| ViTMatte (`matting/models/vitmatte-s-512.onnx`, `vitmatte-s-1024.onnx`) | Apache-2.0 | Weights from `hustvl/vitmatte-small-composition-1k`; full text in `packaging/msix/licenses/Apache-2.0.txt`. |
| SAM 3 (`models/sam3-q4_0.ggml`) | SAM License (Meta) | Redistribution is granted by the SAM License (§1.b.i: provide a copy of the Agreement with the materials). Full text in `packaging/msix/licenses/SAM-License.txt`. |
| ONNX Runtime (matting runtime) | MIT | <https://github.com/microsoft/onnxruntime> |
| CUDA / cuBLAS / cuDNN / cuDART runtime | NVIDIA CUDA Toolkit EULA + cuDNN SLA | Redistributable runtime as shipped inside `matting/`; full texts in `packaging/msix/licenses/NVIDIA-CUDA-EULA.txt` and `NVIDIA-cuDNN-SLA.txt`. |

A user-installed model (any other SAM3/ViTMatte/assistant weights) remains under
its own upstream license.

## No warranty

The software is provided "as is", without warranty of any kind, to the extent
permitted by applicable law. See the GPL-2.0 text in [`COPYING`](COPYING).
