#!/usr/bin/env python3
"""Run the original Python prototype headlessly and report baseline timings.

Offline development tool, kept only to reproduce the pre-migration numbers for comparison. The
prototype it drives lives in legacy_python/ and is not part of the deployed runtime.
"""

from __future__ import annotations

import argparse
import json
import platform
import resource
import statistics
import sys
import time
from pathlib import Path

import cv2

# The prototype was moved under legacy_python/ during the C++ migration.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "legacy_python"))

import main as prototype  # noqa: E402


def timing(values: list[float]) -> dict[str, float | int] | None:
    if not values:
        return None
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "avg_ms": round(statistics.mean(ordered) * 1000.0, 2),
        "p50_ms": round(ordered[len(ordered) // 2] * 1000.0, 2),
        "p95_ms": round(ordered[max(0, int(len(ordered) * 0.95) - 1)] * 1000.0, 2),
        "max_ms": round(ordered[-1] * 1000.0, 2),
    }


def rss_report() -> dict[str, float | str]:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if platform.system() == "Darwin":
        return {"peak_rss_bytes": value, "peak_rss_mb": round(value / (1024 * 1024), 2)}
    return {"peak_rss_kb": value, "peak_rss_mb": round(value / 1024, 2)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", default="video/car.mp4")
    parser.add_argument("--ocr-every", type=int, default=5)
    args = parser.parse_args()

    video = Path(args.video)
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise SystemExit(f"cannot open video: {video}")

    frame_number = 0
    frames_with_plates = 0
    detector_times: list[float] = []
    ocr_times: list[float] = []
    outputs: list[dict[str, object]] = []

    started = time.perf_counter()
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        frame_number += 1

        detector_started = time.perf_counter()
        plates = prototype.detect_license_plate(frame)
        detector_times.append(time.perf_counter() - detector_started)

        if plates:
            frames_with_plates += 1

        for x1, y1, x2, y2, detector_confidence in plates:
            if frame_number % args.ocr_every != 0:
                continue
            crop = frame[y1:y2, x1:x2]
            ocr_started = time.perf_counter()
            number, ocr_confidence = prototype.recognize_plate(crop)
            ocr_times.append(time.perf_counter() - ocr_started)
            if number:
                outputs.append(
                    {
                        "frame": frame_number,
                        "plate": number,
                        "detector_confidence": float(detector_confidence),
                        "ocr_confidence": float(ocr_confidence),
                        "bbox": [int(x1), int(y1), int(x2), int(y2)],
                    }
                )

    elapsed = time.perf_counter() - started
    capture.release()

    report = {
        "video": str(video),
        "frames": frame_number,
        "frames_with_plates": frames_with_plates,
        "total_seconds": round(elapsed, 2),
        "effective_fps": round(frame_number / elapsed, 2) if elapsed > 0 else 0.0,
        "detector": timing(detector_times),
        "ocr": timing(ocr_times),
        "recognized_outputs": outputs,
    }
    report.update(rss_report())
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

