#!/usr/bin/env python3
"""Prepare a car reference cutout + detail-protection mask for the material
harness.

The supplied photograph has a complex track background. GrabCut isolates the
car silhouette; a conservative HSV heuristic protects dark glass/tyres and
low-saturation highlights (lights, plate, chrome) from the paint shader. This
is a regression fixture, not the production segmentation path.

Usage:
  python scripts/pb-auto-reference-prep.py ^
      --source C:\\Users\\lukes.COREI9\\Downloads\\lamborghini.jpeg ^
      --out D:\\_SATIN_AI\\models\\holo-reference
"""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np


def largest_component(mask: np.ndarray) -> np.ndarray:
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    if count <= 1:
        return mask
    best = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    return np.where(labels == best, 255, 0).astype(np.uint8)


def maskrcnn_car_mask(img: np.ndarray):
    """COCO car instance from Mask R-CNN; the full body, wheels and wing."""
    try:
        import torch
        from torchvision.models.detection import (
            MaskRCNN_ResNet50_FPN_V2_Weights,
            maskrcnn_resnet50_fpn_v2,
        )
    except Exception as exc:  # noqa: BLE001
        print(f"maskrcnn import failed: {exc}")
        return None

    weights = MaskRCNN_ResNet50_FPN_V2_Weights.DEFAULT
    model = maskrcnn_resnet50_fpn_v2(weights=weights,
                                     box_score_thresh=0.5).eval()
    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    tensor = torch.from_numpy(rgb).permute(2, 0, 1).float() / 255.0
    with torch.no_grad():
        out = model([tensor])[0]
    labels = out["labels"].cpu().numpy()
    scores = out["scores"].cpu().numpy()
    masks = out["masks"].cpu().numpy()
    best = None
    best_area = 0
    for index, label in enumerate(labels):
        if int(label) != 3 or float(scores[index]) < 0.5:
            continue
        mask = (masks[index, 0] > 0.5).astype(np.uint8)
        area = int(mask.sum())
        if area > best_area:
            best_area = area
            best = mask
    if best is None:
        return None
    mask = best * 255
    if mask.shape[:2] != img.shape[:2]:
        mask = cv2.resize(mask, (img.shape[1], img.shape[0]),
                          interpolation=cv2.INTER_NEAREST)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((7, 7), np.uint8))
    return largest_component(mask)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--out", default=r"D:\_SATIN_AI\models\holo-reference")
    args = parser.parse_args()

    src = Path(args.source)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    img = cv2.imread(str(src), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"cannot read {src}")
    h, w = img.shape[:2]

    # Prefer a real COCO car instance (complete body, wheels and wing).
    car = maskrcnn_car_mask(img)
    print("SEGMENTER maskrcnn" if car is not None else "SEGMENTER fallback")

    if car is None:
        # Fallback: the only strongly saturated orange object, grown to its
        # hull. This can clip dark extremities; Mask R-CNN avoids that.
        hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
        hue = hsv[..., 0]
        sat = hsv[..., 1]
        value = hsv[..., 2]
        orange = (((hue > 3) & (hue < 32) & (sat > 110)
                   & (value > 70)).astype(np.uint8) * 255)
        orange = largest_component(orange)
        orange = cv2.morphologyEx(orange, cv2.MORPH_CLOSE,
                                  np.ones((15, 15), np.uint8))
        contours, _ = cv2.findContours(orange, cv2.RETR_EXTERNAL,
                                       cv2.CHAIN_APPROX_SIMPLE)
        if not contours:
            raise SystemExit("no car-coloured component found")
        hull = cv2.convexHull(max(contours, key=cv2.contourArea))
        car = np.zeros((h, w), np.uint8)
        cv2.fillPoly(car, [hull], 255)
        car = cv2.dilate(car, np.ones((13, 13), np.uint8), iterations=1)
        car = cv2.erode(car, np.ones((3, 3), np.uint8), iterations=1)
        car = largest_component(car)

    # Protect dark glass/tyres/grille and low-saturation highlights
    # (headlights, plate, chrome trim). The orange paint stays saturated.
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
    value = hsv[..., 2]
    sat = hsv[..., 1]
    protect = (((value < 65) | ((sat < 55) & (value > 205)))).astype(np.uint8)
    protect = cv2.bitwise_and(protect * 255, car)
    protect = cv2.dilate(protect, np.ones((3, 3), np.uint8), iterations=1)
    protect = cv2.bitwise_and(protect, car)

    rgba = cv2.cvtColor(img, cv2.COLOR_BGR2BGRA)
    rgba[..., 3] = car
    cv2.imwrite(str(out / "lamborghini-cutout.png"), rgba)
    cv2.imwrite(str(out / "lamborghini-protection.png"), protect)

    # A quick visual: car over a neutral checker so the cutout is obvious.
    preview = np.full_like(img, 245)
    for y in range(0, h, 24):
        for x in range(0, w, 24):
            if ((x // 24) + (y // 24)) % 2 == 0:
                preview[y:y + 24, x:x + 24] = 205
    alpha = (car.astype(np.float32) / 255.0)[..., None]
    preview = (img.astype(np.float32) * alpha
               + preview.astype(np.float32) * (1.0 - alpha)).astype(np.uint8)
    cv2.imwrite(str(out / "lamborghini-cutout-preview.png"), preview)

    car_px = int((car > 127).sum())
    prot_px = int((protect > 127).sum())
    print(f"SIZE {w}x{h}")
    print(f"CAR_PIXELS {car_px} ({car_px / (w * h):.3%})")
    print(f"PROTECTED {prot_px} ({prot_px / max(1, car_px):.1%} of car)")
    print(f"OUT {out}")
    print("PREP_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
