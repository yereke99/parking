#!/usr/bin/env python3
"""Headless ANPR prototype using YOLO detection and fast-plate-ocr recognition."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import time
from collections import Counter
from pathlib import Path
from typing import Any

import cv2
import numpy as np


DEFAULT_VIDEO = Path("video/car.mp4")
DEFAULT_DETECTOR = Path("models/license_plate_detector.onnx")
ALLOWED_CHARS_RE = re.compile(r"[^A-Z0-9_]")


class OnnxYoloPlateDetector:
    def __init__(self, model_path: Path, input_size: int, confidence: float, nms: float) -> None:
        if not model_path.exists():
            raise SystemExit(f"detector model not found: {model_path}")
        self.net = cv2.dnn.readNetFromONNX(str(model_path))
        self.input_size = input_size
        self.confidence = confidence
        self.nms = nms

    def detect(self, frame: np.ndarray) -> list[tuple[int, int, int, int, float]]:
        input_image, scale, pad_x, pad_y = letterbox(frame, self.input_size)
        blob = cv2.dnn.blobFromImage(
            input_image,
            scalefactor=1.0 / 255.0,
            size=(self.input_size, self.input_size),
            mean=(0.0, 0.0, 0.0),
            swapRB=True,
            crop=False,
        )
        self.net.setInput(blob)
        output = self.net.forward()
        if output.ndim != 3:
            return []

        predictions = output[0]
        if predictions.shape[0] < predictions.shape[1]:
            predictions = predictions.T

        boxes: list[list[int]] = []
        scores: list[float] = []
        for row in predictions:
            score = float(np.max(row[4:]))
            if score < self.confidence:
                continue

            cx, cy, width, height = map(float, row[:4])
            x1 = int(round((cx - width * 0.5 - pad_x) / scale))
            y1 = int(round((cy - height * 0.5 - pad_y) / scale))
            box_w = int(round(width / scale))
            box_h = int(round(height / scale))
            x1, y1, box_w, box_h = clamp_box(x1, y1, box_w, box_h, frame.shape[1], frame.shape[0])
            if box_w <= 0 or box_h <= 0:
                continue
            boxes.append([x1, y1, box_w, box_h])
            scores.append(score)

        keep = cv2.dnn.NMSBoxes(boxes, scores, self.confidence, self.nms)
        detections: list[tuple[int, int, int, int, float]] = []
        for index in np.asarray(keep).reshape(-1):
            x, y, width, height = boxes[int(index)]
            detections.append((x, y, x + width, y + height, scores[int(index)]))
        return detections


def normalize_plate(text: str) -> str:
    return ALLOWED_CHARS_RE.sub("", text.upper()).strip("_")


def letterbox(frame: np.ndarray, input_size: int) -> tuple[np.ndarray, float, int, int]:
    height, width = frame.shape[:2]
    scale = min(input_size / width, input_size / height)
    resized_width = int(round(width * scale))
    resized_height = int(round(height * scale))
    resized = cv2.resize(frame, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR)
    padded = np.full((input_size, input_size, 3), 114, dtype=frame.dtype)
    pad_x = (input_size - resized_width) // 2
    pad_y = (input_size - resized_height) // 2
    padded[pad_y : pad_y + resized_height, pad_x : pad_x + resized_width] = resized
    return padded, scale, pad_x, pad_y


def clamp_box(x: int, y: int, width: int, height: int, frame_width: int, frame_height: int) -> tuple[int, int, int, int]:
    x2 = min(frame_width, max(0, x + width))
    y2 = min(frame_height, max(0, y + height))
    x1 = min(frame_width, max(0, x))
    y1 = min(frame_height, max(0, y))
    return x1, y1, max(0, x2 - x1), max(0, y2 - y1)


def timing(values: list[float]) -> dict[str, float | int] | None:
    if not values:
        return None
    ordered = sorted(value * 1000.0 for value in values)
    p95_index = min(len(ordered) - 1, max(0, math.ceil(len(ordered) * 0.95) - 1))
    return {
        "count": len(ordered),
        "avg_ms": round(statistics.mean(ordered), 3),
        "p50_ms": round(ordered[len(ordered) // 2], 3),
        "p95_ms": round(ordered[p95_index], 3),
        "max_ms": round(ordered[-1], 3),
    }


def prediction_confidence(prediction: Any) -> float | None:
    direct = getattr(prediction, "plate_confidence", None)
    if direct is not None:
        return float(direct)

    char_probs = getattr(prediction, "char_probs", None)
    if char_probs is None:
        return None

    probs = np.asarray(char_probs, dtype=float).reshape(-1)
    return float(probs.mean()) if probs.size else None


def run_ocr(recognizer: Any, crop_bgr: np.ndarray, crop_top: float) -> tuple[str, float | None]:
    if crop_bgr.size == 0:
        return "", None

    if crop_top > 0.0:
        height = crop_bgr.shape[0]
        crop_bgr = crop_bgr[int(height * crop_top) :, :]
        if crop_bgr.size == 0:
            return "", None

    crop_rgb = cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2RGB)
    prediction = recognizer.run(crop_rgb, return_confidence=True)[0]
    plate = normalize_plate(getattr(prediction, "plate", str(prediction)))
    return plate, prediction_confidence(prediction)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Test fast-plate-ocr on a video.")
    parser.add_argument("--video", type=Path, default=DEFAULT_VIDEO)
    parser.add_argument("--detector", type=Path, default=DEFAULT_DETECTOR)
    parser.add_argument("--ocr-model", default="cct-s-v2-global-model")
    parser.add_argument("--conf", type=float, default=0.5)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--nms", type=float, default=0.45)
    parser.add_argument("--ocr-every", type=int, default=5)
    parser.add_argument("--crop-top", type=float, default=0.0)
    parser.add_argument("--max-frames", type=int, default=0)
    parser.add_argument("--json", action="store_true", help="Print only the final JSON report.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.ocr_every <= 0:
        raise SystemExit("--ocr-every must be positive")
    if not 0.0 <= args.crop_top < 1.0:
        raise SystemExit("--crop-top must be in [0.0, 1.0)")
    if not args.video.exists():
        raise SystemExit(f"video not found: {args.video}")

    from fast_plate_ocr import LicensePlateRecognizer

    detector = OnnxYoloPlateDetector(args.detector, args.imgsz, args.conf, args.nms)
    recognizer = LicensePlateRecognizer(args.ocr_model)

    capture = cv2.VideoCapture(str(args.video))
    if not capture.isOpened():
        raise SystemExit(f"cannot open video: {args.video}")

    frame_count = 0
    frames_with_plates = 0
    detections_total = 0
    ocr_calls = 0
    outputs: list[dict[str, Any]] = []
    detector_times: list[float] = []
    ocr_times: list[float] = []

    started = time.perf_counter()
    while True:
        if args.max_frames and frame_count >= args.max_frames:
            break

        ok, frame = capture.read()
        if not ok:
            break

        frame_count += 1

        detector_started = time.perf_counter()
        boxes = detector.detect(frame)
        detector_times.append(time.perf_counter() - detector_started)

        if boxes:
            frames_with_plates += 1
            detections_total += len(boxes)

        for x1, y1, x2, y2, detector_confidence in boxes:
            if frame_count % args.ocr_every != 0:
                continue

            crop = frame[y1:y2, x1:x2]

            ocr_started = time.perf_counter()
            plate, ocr_confidence = run_ocr(recognizer, crop, args.crop_top)
            ocr_times.append(time.perf_counter() - ocr_started)
            ocr_calls += 1

            if not plate:
                continue

            row = {
                "frame": frame_count,
                "plate": plate,
                "detector_confidence": round(detector_confidence, 4),
                "ocr_confidence": round(ocr_confidence, 4) if ocr_confidence is not None else None,
                "bbox": [x1, y1, x2, y2],
            }
            outputs.append(row)

            if not args.json:
                print(
                    f"frame={frame_count} plate={plate} "
                    f"detector={row['detector_confidence']:.2f} "
                    f"ocr={row['ocr_confidence'] if row['ocr_confidence'] is not None else 'n/a'}"
                )

    elapsed = time.perf_counter() - started
    capture.release()

    report = {
        "video": str(args.video),
        "detector": str(args.detector),
        "ocr_model": args.ocr_model,
        "frames": frame_count,
        "frames_with_plates": frames_with_plates,
        "detections_total": detections_total,
        "ocr_calls": ocr_calls,
        "total_seconds": round(elapsed, 3),
        "effective_fps": round(frame_count / elapsed, 3) if elapsed > 0.0 else 0.0,
        "detector_timing": timing(detector_times),
        "ocr": timing(ocr_times),
        "top_outputs": Counter(row["plate"] for row in outputs).most_common(10),
        "recognized_outputs": outputs,
    }
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
