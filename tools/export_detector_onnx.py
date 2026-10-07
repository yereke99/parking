#!/usr/bin/env python3
"""Export the YOLOv8n plate detector (license_plate_detector.pt) to ONNX for the C++ runtime.

Needs `ultralytics` on a PC; JetPack 4's Python is too old for it. See models/README.md.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from ultralytics import YOLO


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--weights", default="license_plate_detector.pt")
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--opset", type=int, default=12)
    parser.add_argument("--dynamic", action="store_true")
    args = parser.parse_args()

    weights = Path(args.weights)
    if not weights.exists():
        raise SystemExit(f"weights not found: {weights}")

    model = YOLO(str(weights))
    exported = model.export(
        format="onnx",
        imgsz=args.imgsz,
        opset=args.opset,
        dynamic=args.dynamic,
        simplify=True,
    )
    print(exported)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

