#!/usr/bin/env python3
"""Export the FASHN Human Parser (SegFormer-B4) to ONNX for NAI Paintbrush.

Why: the creative recolour currently uses the whole subject silhouette plus a
heuristic skin guard. Garment/hair/skin/accessory materials need independent
regions. FASHN is an 18-class fashion parser; this script pins its revision,
exports a reproducible ONNX graph and writes a provenance manifest so the
native inference service can consume exactly one artifact.

Model card: https://huggingface.co/fashn-ai/fashn-human-parser
License:   inherits the NVIDIA Source Code License for SegFormer - review before
           shipping the weights. This script only produces a local artifact.

Contract (must stay in sync with the C++ service):
  input   pixel_values  float32  [batch, 3, 576, 384]  RGB, ImageNet norm
  output  logits        float32  [batch, 18, 144, 96]  /4 resolution
  classes 0 background, 1 face, 2 hair, 3 top, 4 dress, 5 skirt, 6 pants,
          7 belt, 8 bag, 9 hat, 10 scarf, 11 glasses, 12 arms, 13 hands,
          14 legs, 15 feet, 16 torso, 17 jewelry

Usage:
  python scripts/pb-fashn-export.py
  python scripts/pb-fashn-export.py --out D:\\models\\fashn-human-parser
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
import transformers
import huggingface_hub as hfhub
from huggingface_hub import snapshot_download
from PIL import Image, __version__ as PIL_VERSION
import cv2
from transformers import SegformerForSemanticSegmentation

MODEL_ID = "fashn-ai/fashn-human-parser"
# Pinned from the Hugging Face model API on 2026-11-24. Do not float this.
REVISION = "1f80c34dbab321c5730dda5c3fea279fd3e97498"
INPUT_SIZE = (576, 384)  # height, width - the model's preprocessor contract
CLASSES = [
    "background", "face", "hair", "top", "dress", "skirt", "pants", "belt",
    "bag", "hat", "scarf", "glasses", "arms", "hands", "legs", "feet",
    "torso", "jewelry",
]
# Distinct, readable class colours for the validation map.
PALETTE = [
    (18, 18, 22), (255, 214, 189), (92, 58, 36), (255, 86, 120),
    (255, 170, 60), (255, 220, 60), (90, 120, 255), (180, 120, 60),
    (230, 80, 180), (120, 220, 160), (200, 160, 255), (120, 200, 255),
    (255, 190, 160), (255, 230, 200), (140, 90, 60), (90, 60, 40),
    (200, 120, 90), (255, 240, 120),
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def default_test_image() -> Path | None:
    candidates = [
        Path(r"D:\_SATIN_AI\NAI-OS\vlc\paintbrush\docs\creative-synthesis-assets\selftest-holo-fashion.png"),
        Path(r"D:\_SATIN_AI\NAI-OS\vlc\paintbrush\docs\creative-synthesis-assets\reference-01-neon-editorial.png"),
    ]
    for path in candidates:
        if path.is_file():
            return path
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=r"D:\_SATIN_AI\models\fashn-human-parser")
    parser.add_argument("--test-image", default="")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    hf_dir = out_dir / "hf"
    onnx_path = out_dir / "fashn-human-parser.onnx"

    print(f"[1/5] snapshot {MODEL_ID} @ {REVISION}", flush=True)
    snapshot_download(
        repo_id=MODEL_ID,
        revision=REVISION,
        local_dir=str(hf_dir),
        allow_patterns=["config.json", "preprocessor_config.json",
                        "model.safetensors"],
    )
    for name in ("config.json", "preprocessor_config.json",
                 "model.safetensors"):
        path = hf_dir / name
        if not path.is_file():
            print(f"missing {path}", file=sys.stderr)
            return 2
        print(f"  {name}: {path.stat().st_size} bytes", flush=True)

    print("[2/5] load SegFormer-B4", flush=True)
    model = SegformerForSemanticSegmentation.from_pretrained(
        str(hf_dir), local_files_only=True
    ).eval()
    class Wrapper(torch.nn.Module):
        def __init__(self, inner: torch.nn.Module):
            super().__init__()
            self.inner = inner

        def forward(self, pixel_values: torch.Tensor) -> torch.Tensor:
            return self.inner(pixel_values=pixel_values).logits

    dummy = torch.randn(1, 3, INPUT_SIZE[0], INPUT_SIZE[1])
    print(f"[3/5] export ONNX (opset 17) -> {onnx_path}", flush=True)
    torch.onnx.export(
        Wrapper(model),
        dummy,
        str(onnx_path),
        input_names=["pixel_values"],
        output_names=["logits"],
        opset_version=18,
        do_constant_folding=True,
        dynamo=True,
        external_data=False,
    )

    print("[4/5] validate with onnxruntime (CPU)", flush=True)
    session = ort.InferenceSession(str(onnx_path),
                                   providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name

    test_path = Path(args.test_image) if args.test_image else default_test_image()
    counts: dict[str, int] = {}
    test_note = "synthetic gradient (no test image found)"
    if test_path and test_path.is_file():
        image = Image.open(test_path).convert("RGB")
        test_note = str(test_path)
        rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
        resized = cv2.resize(rgb, (INPUT_SIZE[1], INPUT_SIZE[0]),
                             interpolation=cv2.INTER_AREA)
        pixel_values = resized.astype(np.float32) / 255.0
        normalised = (pixel_values
                      - np.array([0.485, 0.456, 0.406], np.float32))
        normalised /= np.array([0.229, 0.224, 0.225], np.float32)
        pixel_values = np.transpose(normalised, (2, 0, 1))[None]
    else:
        # Deterministic contract check even without a subject photo.
        ramp = np.linspace(0.0, 1.0, INPUT_SIZE[1], dtype=np.float32)
        pixel_values = np.repeat(ramp[None, None, :, None], 3, axis=1)
        pixel_values = np.repeat(pixel_values, INPUT_SIZE[0], axis=2)
        pixel_values = (pixel_values - 0.5) / 0.25

    logits = session.run([output_name], {input_name: pixel_values})[0]
    print(f"  output shape: {logits.shape}", flush=True)

    # Numerical parity with the reference PyTorch model on the same input.
    with torch.no_grad():
        reference = model(pixel_values=torch.from_numpy(pixel_values)).logits.numpy()
    parity = {
        "reference": "PyTorch SegformerForSemanticSegmentation",
        "maxAbs": float(np.max(np.abs(reference - logits))),
        "meanAbs": float(np.mean(np.abs(reference - logits))),
        "classAgreement": float(
            (reference.argmax(axis=1) == logits.argmax(axis=1)).mean()
        ),
    }
    print(f"  parity: max={parity['maxAbs']:.6g} "
          f"mean={parity['meanAbs']:.6g} "
          f"agree={parity['classAgreement']:.4f}", flush=True)

    labels = logits.argmax(axis=1)[0].astype(np.uint8)
    for index, name in enumerate(CLASSES):
        counts[name] = int((labels == index).sum())
    class_map = Image.fromarray(labels, mode="L").resize(
        (INPUT_SIZE[1], INPUT_SIZE[0]), Image.Resampling.NEAREST
    )
    palette_bytes = [c for entry in PALETTE for c in entry]
    palette_bytes += [0] * (768 - len(palette_bytes))
    class_map.putpalette(palette_bytes)
    map_path = out_dir / "validation-classes.png"
    class_map.convert("RGB").save(map_path)

    manifest = {
        "model": MODEL_ID,
        "revision": REVISION,
        "license": "NVIDIA Source Code License for SegFormer (inherited; review before distribution)",
        "exportedAt": datetime.now(timezone.utc).isoformat(),
        "exportCommand": "python scripts/pb-fashn-export.py",
        "onnx": {
            "file": onnx_path.name,
            "bytes": onnx_path.stat().st_size,
            "sha256": sha256(onnx_path),
            "input": {"name": input_name, "shape": [1, 3, 576, 384],
                      "dtype": "float32", "color": "RGB",
                      "mean": [0.485, 0.456, 0.406],
                      "std": [0.229, 0.224, 0.225],
                      "resize": "cv2.INTER_AREA to 384x576 (training preprocessing)"},
            "output": {"name": output_name, "shape": list(logits.shape),
                       "layout": "logits at 1/4 resolution"},
        },
        "exporter": {
            "torch": torch.__version__,
            "transformers": transformers.__version__,
            "onnx": onnx.__version__,
            "onnxruntime": ort.__version__,
            "opencv": cv2.__version__,
            "numpy": np.__version__,
            "pillow": PIL_VERSION,
            "huggingface_hub": hfhub.__version__,
        },
        "parity": parity,
        "classes": {str(i): name for i, name in enumerate(CLASSES)},
        "validation": {
            "image": test_note,
            "classPixels": counts,
            "classMap": map_path.name,
        },
    }
    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print("[5/5] done", flush=True)
    print(f"  onnx: {onnx_path} ({manifest['onnx']['bytes']} bytes)", flush=True)
    print(f"  sha256: {manifest['onnx']['sha256']}", flush=True)
    print(f"  manifest: {manifest_path}", flush=True)
    print(f"  classes: {counts}", flush=True)
    print("EXPORT_OK", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
