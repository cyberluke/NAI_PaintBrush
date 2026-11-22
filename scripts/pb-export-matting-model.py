#!/usr/bin/env python3
r"""pb-export-matting-model.py — reproducible ViTMatte -> ONNX export for NAI Paintbrush.

This is a BUILD-TIME tool. Python, PyTorch and transformers are NOT runtime
dependencies of the distributed Paintbrush application: their only job is to
produce the ONNX artifact and the manifest that `pb_matting_cuda.dll` reads.

Frozen facts (verified against the ViTMatte reference code and the published
Hugging Face checkpoint; do not change without re-validating parity):

  * Input tensor      : one float32 NCHW tensor, 4 channels, fixed square
                        sizes produced here (default 512 and 1024).
  * Channel order     : R,G,B normalized with ImageNet mean/std
                        (123.675,116.280,103.530)/255 and
                        (58.395,57.120,57.375)/255; channel 3 is the trimap.
  * Trimap encoding   : single channel in [0,1], 0 = background,
                        0.5 = unknown, 1 = foreground (the model's own
                        training convention; the 128/255 value used by the
                        original dataset is equivalent for inference).
  * Padding           : caller pads H/W up to a multiple of 32 with ZEROS, then
                        crops the output back to the requested H/W. This script
                        exports a graph whose spatial size is FIXED, so the
                        runtime always letterboxes the subject region to that
                        fixed square and never relies on dynamic shapes.
  * Output            : `alpha`, float32, shape (1,1,S,S), range [0,1].

The manifest written next to the ONNX files is the single source of truth for
the native runtime: it pins the graph hashes, the preprocessing constants, the
supported sizes and the exact provenance of the weights.

Usage (development machine only):

  python scripts/pb-export-matting-model.py \
      --output-dir D:\_SATIN_AI\NAI-OS\gpu-matting\models\vitmatte \
      --sizes 512,1024

The script is idempotent: an existing ONNX file with a matching hash in the
manifest is not re-exported unless --force is given.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import sys
from datetime import datetime, timezone

MODEL_ID = "hustvl/vitmatte-small-composition-1k"
# Pinned revision (HF commit) of the published Apache-2.0 checkpoint.
MODEL_REVISION = "6a58ad7646403c1df626fbd746900aec7361ea1d"
MODEL_WEIGHTS_SHA256 = (
    "bda9289db1bb6762d978b42d1c62ae3f34daf7497171a347a1d09657efd788cb"
)
MODEL_LICENSE = "Apache-2.0 (model card); ViTMatte source is MIT"

IMAGENET_MEAN = [123.675 / 255.0, 116.280 / 255.0, 103.530 / 255.0]
IMAGENET_STD = [58.395 / 255.0, 57.120 / 255.0, 57.375 / 255.0]
SIZE_DIVISOR = 32
TRIMAP_BACKGROUND = 0.0
TRIMAP_UNKNOWN = 0.5
TRIMAP_FOREGROUND = 1.0
OPSET = 17


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class AlphaOnlyWrapper:
    """Exports just the alpha plane so the native runtime never sees a dict."""

    def __new__(cls, model):
        import torch

        class _Wrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, pixel_values):
                return self.inner(pixel_values=pixel_values).alphas

        return _Wrapper(model)


def build_manifest(args, exports, weights_path, weights_sha):
    import onnx
    import onnxruntime
    import torch
    import transformers

    return {
        "schema": "nai-paintbrush.matting-model/v1",
        "createdUtc": datetime.now(timezone.utc).isoformat(),
        "model": {
            "family": "ViTMatte",
            "variant": "vitmatte-small-composition-1k",
            "sourceRepo": "https://github.com/hustvl/ViTMatte",
            "sourceLicense": "MIT",
            "weightsRepo": MODEL_ID,
            "weightsRevision": MODEL_REVISION,
            "weightsFile": os.path.basename(weights_path),
            "weightsSha256": weights_sha,
            "weightsLicense": MODEL_LICENSE,
            "parameterCount": 25826178,
            "precision": "fp32",
        },
        "preprocess": {
            "inputName": "pixel_values",
            "inputLayout": "NCHW",
            "channels": 4,
            "colorOrder": "RGB",
            "normalization": "image/255 then (x-mean)/std",
            "mean": IMAGENET_MEAN,
            "std": IMAGENET_STD,
            "trimapChannel": 3,
            "trimapEncoding": {
                "background": TRIMAP_BACKGROUND,
                "unknown": TRIMAP_UNKNOWN,
                "foreground": TRIMAP_FOREGROUND,
            },
            "sizeDivisor": SIZE_DIVISOR,
            "padding": "zero",
            "outputCrop": "caller crops alpha back to the requested H/W",
        },
        "output": {
            "name": "alpha",
            "layout": "NCHW",
            "channels": 1,
            "activation": "sigmoid (inside the graph)",
            "range": [0.0, 1.0],
        },
        "runtime": {
            "opset": OPSET,
            "exporter": "torch.onnx.export (TorchScript)",
            "torch": torch.__version__,
            "transformers": transformers.__version__,
            "onnx": onnx.__version__,
            "onnxruntime": onnxruntime.__version__,
            "python": platform.python_version(),
        },
        "graphs": exports,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True,
                        help="directory that will hold the ONNX graphs + manifest")
    parser.add_argument("--sizes", default="512,1024",
                        help="comma separated fixed square sizes")
    parser.add_argument("--force", action="store_true",
                        help="re-export even when a matching graph exists")
    parser.add_argument("--cache-dir", default=None,
                        help="optional Hugging Face cache directory")
    args = parser.parse_args()

    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from huggingface_hub import hf_hub_download
    from transformers import VitMatteForImageMatting

    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    for size in sizes:
        if size % SIZE_DIVISOR != 0:
            raise SystemExit(f"size {size} is not divisible by {SIZE_DIVISOR}")

    os.makedirs(args.output_dir, exist_ok=True)

    print(f"downloading {MODEL_ID}@{MODEL_REVISION}")
    weights_path = hf_hub_download(
        repo_id=MODEL_ID,
        filename="model.safetensors",
        revision=MODEL_REVISION,
        cache_dir=args.cache_dir,
    )
    weights_sha = sha256_file(weights_path)
    if MODEL_WEIGHTS_SHA256 and weights_sha != MODEL_WEIGHTS_SHA256:
        raise SystemExit(
            f"weights hash mismatch: {weights_sha} != {MODEL_WEIGHTS_SHA256}")
    print(f"weights sha256 {weights_sha}")

    manifest_path = os.path.join(args.output_dir, "manifest.json")
    previous = {}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                previous = json.load(handle)
        except (OSError, ValueError):
            previous = {}
    previous_graphs = {
        int(g["size"]): g for g in previous.get("graphs", []) if "size" in g}

    model = VitMatteForImageMatting.from_pretrained(
        MODEL_ID, revision=MODEL_REVISION, cache_dir=args.cache_dir)
    model.eval()
    wrapper = AlphaOnlyWrapper(model)
    wrapper.eval()

    torch.manual_seed(1234)
    np.random.seed(1234)

    exports = []
    for size in sizes:
        name = f"vitmatte-s-{size}.onnx"
        path = os.path.join(args.output_dir, name)

        # Deterministic, structured probe input: a smooth gradient + a disc so
        # the comparison is not dominated by noise.
        yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
        base = np.stack([
            (xx / max(1, size - 1)),
            (yy / max(1, size - 1)),
            (((xx - size / 2) ** 2 + (yy - size / 2) ** 2) <
             (size * 0.3) ** 2).astype(np.float32),
        ], axis=0)
        rgb = base * 255.0
        normalized = (rgb / 255.0 - np.array(IMAGENET_MEAN, dtype=np.float32)[:, None, None]) \
            / np.array(IMAGENET_STD, dtype=np.float32)[:, None, None]
        trimap = np.full((1, size, size), TRIMAP_UNKNOWN, dtype=np.float32)
        trimap[:, : size // 8, :] = TRIMAP_BACKGROUND
        trimap[:, -size // 8 :, :] = TRIMAP_FOREGROUND
        probe = np.concatenate([normalized[None], trimap[None]], axis=1).astype(np.float32)
        probe_t = torch.from_numpy(probe)

        with torch.no_grad():
            reference = wrapper(probe_t).numpy()

        digest = previous_graphs.get(size, {}).get("sha256", "")
        if (not args.force and os.path.exists(path)
                and digest == sha256_file(path)):
            print(f"{name}: existing graph hash matches, skipping export")
        else:
            print(f"{name}: exporting fixed {size}x{size} graph")
            with torch.no_grad():
                torch.onnx.export(
                    wrapper,
                    (probe_t,),
                    path,
                    input_names=["pixel_values"],
                    output_names=["alpha"],
                    opset_version=OPSET,
                    do_constant_folding=True,
                    dynamo=False,
                )
            onnx.checker.check_model(onnx.load(path))
            digest = sha256_file(path)
            print(f"{name}: sha256 {digest}")

        session = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
        produced = session.run(None, {"pixel_values": probe})[0]
        if produced.shape != (1, 1, size, size):
            raise SystemExit(f"{name}: unexpected output shape {produced.shape}")
        diff = float(np.max(np.abs(produced - reference)))
        mean_diff = float(np.mean(np.abs(produced - reference)))
        # Float32 kernel-reordering differences of ~1.5e-4 are expected
        # (1e-4 of an 8-bit alpha level is 0.025). Anything above 5e-4 means a
        # real semantic export error, not rounding.
        if diff > 5e-4:
            raise SystemExit(
                f"{name}: ONNX/PyTorch parity failed (max {diff:.3e})")
        print(f"{name}: parity max={diff:.3e} mean={mean_diff:.3e}")

        exports.append({
            "size": size,
            "file": name,
            "sha256": digest,
            "inputShape": [1, 4, size, size],
            "outputShape": [1, 1, size, size],
            "parity": {"maxAbsDiff": diff, "meanAbsDiff": mean_diff,
                       "provider": "CPUExecutionProvider",
                       "probe": "synthetic gradient+disc, trimap bands"},
        })

    manifest = build_manifest(args, exports, weights_path, weights_sha)
    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
    print(f"manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
