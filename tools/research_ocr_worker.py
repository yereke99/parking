#!/usr/bin/env python3
"""Persistent crop-only PaddleOCR/EasyOCR worker used by the C++ research harness.

Both libraries are intentionally run in recognition-only mode. The native YOLO detector has
already produced a tight plate crop, so running a document/scene text detector again would make
the comparison slower and less accurate. Stdout is reserved for the binary protocol; all library
startup chatter is redirected to stderr.
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.metadata
import json
import os
import platform
import re
import resource
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import numpy as np


MAX_CROP_BYTES = 32 * 1024 * 1024
CYRILLIC_LOOKALIKES = str.maketrans(
    "АВСЕНКМОРТХУІЈавсенкмортхуіј",
    "ABCEHKMOPTXYIJABCEHKMOPTXYIJ",
)


@dataclass(frozen=True)
class Recognition:
    text: str
    confidence: float
    min_char_confidence: float
    region: str
    inference_ms: float


def _safe_field(value: Any) -> str:
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def _rss_mb() -> float:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return value / (1024.0 * 1024.0) if sys.platform == "darwin" else value / 1024.0


def normalize_plate(text: str) -> str:
    """Map plate-font Cyrillic lookalikes and remove general-OCR punctuation."""
    text = str(text).upper().translate(CYRILLIC_LOOKALIKES)
    return re.sub(r"[^0-9A-Z]", "", text)


class MockRecognizer:
    version = "test"
    device = "cpu"
    fp16 = False
    model_name = "mock"
    region = "general"

    def __init__(self, engine: str) -> None:
        self.engine = engine

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> list[Recognition]:
        results = []
        for crop in crops:
            if crop.size == 0:
                results.append(Recognition("", 0.0, 0.0, self.region, 0.0))
            else:
                confidence = min(0.99, 0.8 + float(crop.mean()) / 2550.0)
                results.append(Recognition("152JTA02", confidence, confidence,
                                           self.region, 0.05))
        return results


class EasyRecognizer:
    engine = "easyocr"
    region = "general"
    model_name = "latin_g2"
    fp16 = False

    def __init__(self, languages: list[str], requested_device: str, model_cache: Path,
                 allow_download: bool) -> None:
        import torch
        import easyocr

        if requested_device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("DEVICE_UNAVAILABLE: CUDA requested but torch.cuda is unavailable")
        if requested_device == "mps":
            raise RuntimeError("DEVICE_UNAVAILABLE: EasyOCR research worker supports cpu/cuda")
        use_cuda = requested_device == "cuda" or (
            requested_device == "auto" and torch.cuda.is_available()
        )
        self.device = "cuda" if use_cuda else "cpu"
        self.version = importlib.metadata.version("easyocr")
        storage = model_cache / "easyocr"
        storage.mkdir(parents=True, exist_ok=True)
        self.reader = easyocr.Reader(
            languages,
            gpu=use_cuda,
            model_storage_directory=str(storage),
            user_network_directory=str(storage),
            download_enabled=allow_download,
            detector=False,
            recognizer=True,
            verbose=False,
            quantize=not use_cuda,
        )

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> list[Recognition]:
        results = []
        for crop in crops:
            started = time.perf_counter()
            if crop.size == 0:
                results.append(Recognition("", 0.0, 0.0, self.region, 0.0))
                continue
            height, width = crop.shape[:2]
            rows = self.reader.recognize(
                crop,
                horizontal_list=[[0, width, 0, height]],
                free_list=[],
                decoder="greedy",
                beamWidth=1,
                batch_size=1,
                workers=0,
                detail=1,
                paragraph=False,
                allowlist="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
            )
            if self.device == "cuda":
                import torch
                torch.cuda.synchronize()
            elapsed = (time.perf_counter() - started) * 1000.0
            if rows:
                row = rows[0]
                text = row[1] if isinstance(row, (list, tuple)) and len(row) > 1 else ""
                score = float(row[2]) if isinstance(row, (list, tuple)) and len(row) > 2 else 0.0
                results.append(Recognition(normalize_plate(text), score, score,
                                           self.region, elapsed))
            else:
                results.append(Recognition("", 0.0, 0.0, self.region, elapsed))
        return results


def _paddle_payload(result: Any) -> dict[str, Any]:
    """Accept the dict-like and JSON result variants exposed across PaddleOCR 3.x."""
    candidates = [result]
    for attribute in ("json", "res"):
        value = getattr(result, attribute, None)
        if callable(value):
            try:
                value = value()
            except TypeError:
                continue
        if value is not None:
            candidates.append(value)
    for candidate in candidates:
        if isinstance(candidate, str):
            try:
                candidate = json.loads(candidate)
            except json.JSONDecodeError:
                continue
        if isinstance(candidate, dict):
            nested = candidate.get("res")
            return nested if isinstance(nested, dict) else candidate
    return {}


class PaddleRecognizer:
    engine = "paddleocr"
    region = "general"
    fp16 = False

    def __init__(self, model_name: str, inference_engine: str, requested_device: str,
                 model_cache: Path) -> None:
        os.environ.setdefault("PADDLE_OCR_BASE_DIR", str(model_cache / "paddleocr"))
        os.environ.setdefault("PADDLE_PDX_CACHE_HOME", str(model_cache / "paddleocr"))
        # Runtime is offline after setup. Avoid PaddleX spending up to a minute probing model
        # hosts on every worker start; a missing cached model then fails explicitly.
        os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
        Path(os.environ["PADDLE_OCR_BASE_DIR"]).mkdir(parents=True, exist_ok=True)

        from paddleocr import TextRecognition

        self.version = importlib.metadata.version("paddleocr")
        self.model_name = model_name
        self.inference_engine = inference_engine
        if requested_device == "mps":
            raise RuntimeError("DEVICE_UNAVAILABLE: PaddleOCR worker supports cpu/cuda")
        if requested_device == "cuda":
            self.device = "gpu:0"
        elif requested_device == "cpu":
            self.device = "cpu"
        else:
            self.device = self._auto_device(inference_engine)
        self.model = TextRecognition(
            model_name=model_name,
            engine=inference_engine,
            device=self.device,
        )

    @staticmethod
    def _auto_device(inference_engine: str) -> str:
        if inference_engine == "onnxruntime":
            try:
                import onnxruntime
                if "CUDAExecutionProvider" in onnxruntime.get_available_providers():
                    return "gpu:0"
            except ImportError:
                pass
            return "cpu"
        try:
            import paddle
            return "gpu:0" if paddle.is_compiled_with_cuda() else "cpu"
        except ImportError:
            return "cpu"

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> list[Recognition]:
        crops = list(crops)
        if not crops:
            return []
        started = time.perf_counter()
        outputs = list(self.model.predict(input=crops, batch_size=len(crops)))
        elapsed = (time.perf_counter() - started) * 1000.0
        per_crop_ms = elapsed / len(crops)
        results = []
        for output in outputs:
            payload = _paddle_payload(output)
            text = payload.get("rec_text", payload.get("text", ""))
            score = payload.get("rec_score", payload.get("score", 0.0))
            if isinstance(score, (list, tuple)):
                score = score[0] if score else 0.0
            confidence = float(score)
            results.append(Recognition(normalize_plate(str(text)), confidence, confidence,
                                       self.region, per_crop_ms))
        while len(results) < len(crops):
            results.append(Recognition("", 0.0, 0.0, self.region, per_crop_ms))
        return results


def _build_recognizer(args: argparse.Namespace) -> tuple[Any, float]:
    started = time.perf_counter()
    if args.mock:
        recognizer: Any = MockRecognizer(args.engine)
    elif args.engine == "easyocr":
        recognizer = EasyRecognizer(
            [item.strip() for item in args.languages.split(",") if item.strip()],
            args.device,
            Path(args.model_cache),
            allow_download=args.probe,
        )
    else:
        recognizer = PaddleRecognizer(
            args.paddle_model, args.paddle_engine, args.device, Path(args.model_cache)
        )
    recognizer.recognize_batch([np.full((64, 192, 3), 114, dtype=np.uint8)])
    return recognizer, (time.perf_counter() - started) * 1000.0


def _accelerator_memory_mb(recognizer: Any) -> float:
    if getattr(recognizer, "device", "") != "cuda":
        return 0.0
    try:
        import torch
        return torch.cuda.memory_allocated() / (1024.0 * 1024.0)
    except ImportError:
        return 0.0


def _serve(recognizer: Any, startup_ms: float) -> int:
    ready = [
        "READY", recognizer.engine, recognizer.version, recognizer.device,
        recognizer.model_name, recognizer.region, "1" if recognizer.fp16 else "0",
        f"{startup_ms:.3f}", f"{_rss_mb():.3f}",
        f"{_accelerator_memory_mb(recognizer):.3f}", "",
    ]
    print("\t".join(_safe_field(item) for item in ready), flush=True)

    stdin = sys.stdin.buffer
    while True:
        header = stdin.readline()
        if not header:
            return 0
        fields: list[str] = []
        try:
            fields = header.decode("ascii").rstrip("\r\n").split("\t")
            command = fields[0]
            if command == "STOP":
                print("STOPPED", flush=True)
                return 0
            if command == "PING":
                print("PONG", flush=True)
                continue
            if command != "OCR" or len(fields) != 7:
                raise ValueError("PROTOCOL_ERROR: expected OCR id rows cols channels bytes flags")
            request_id = int(fields[1])
            rows, cols, channels, byte_count = map(int, fields[2:6])
            if int(fields[6]) != 0:
                raise ValueError("PROTOCOL_ERROR: unsupported request flags")
            expected = rows * cols * channels
            if rows <= 0 or cols <= 0 or channels != 3 or expected != byte_count:
                raise ValueError("INVALID_CROP: inconsistent dimensions")
            if byte_count > MAX_CROP_BYTES:
                raise ValueError("INVALID_CROP: crop exceeds size limit")
            payload = stdin.read(byte_count)
            if len(payload) != byte_count:
                raise EOFError("PROTOCOL_ERROR: truncated crop payload")
            crop = np.frombuffer(payload, dtype=np.uint8).reshape(rows, cols, channels)
            result = recognizer.recognize_batch([crop])[0]
            print("\t".join([
                "RESULT", str(request_id), _safe_field(result.text),
                f"{result.confidence:.8f}", f"{result.min_char_confidence:.8f}",
                result.region, f"{result.inference_ms:.4f}",
            ]), flush=True)
        except (BrokenPipeError, EOFError):
            return 1
        except Exception as exc:
            request_id = fields[1] if len(fields) > 1 else "0"
            code = "CUDA_OOM" if "out of memory" in str(exc).lower() else "OCR_ERROR"
            print(f"ERROR\t{request_id}\t{code}\t{_safe_field(exc)}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--mock", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--engine", choices=("paddleocr", "easyocr"), required=True)
    parser.add_argument("--device", choices=("auto", "cpu", "mps", "cuda"), default="auto")
    parser.add_argument("--model-cache", default="models/research")
    parser.add_argument("--paddle-model", default="eslav_PP-OCRv5_mobile_rec")
    parser.add_argument("--paddle-engine", choices=("onnxruntime", "paddle_static"),
                        default="onnxruntime")
    parser.add_argument("--languages", default="en")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        with contextlib.redirect_stdout(sys.stderr):
            recognizer, startup_ms = _build_recognizer(args)
        if args.probe:
            print(json.dumps({
                "engine": recognizer.engine,
                "version": recognizer.version,
                "python": platform.python_version(),
                "device": recognizer.device,
                "model": recognizer.model_name,
                "startup_ms": startup_ms,
                "rss_mb": _rss_mb(),
            }))
            return 0
        if args.serve:
            return _serve(recognizer, startup_ms)
        raise ValueError("choose --probe or --serve")
    except Exception as exc:
        print(f"OCR_BACKEND_UNAVAILABLE: {_safe_field(exc)}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
