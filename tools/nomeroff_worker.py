#!/usr/bin/env python3
"""Persistent Nomeroff Net OCR worker for the native ANPR runtime.

The protocol deliberately uses one ASCII header plus raw BGR bytes. Plate crops are small, and
this avoids JPEG encode/decode latency and quality loss. All diagnostics go to stderr; stdout is
reserved for protocol responses.
"""

import argparse
import contextlib
import json
import math
import os
import platform
import resource
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

import numpy as np


NOMEROFF_VERSION = "4.0.1"
NOMEROFF_COMMIT = "931388550b83f045c0ac951a77daa23df22f962d"
MAX_CROP_BYTES = 32 * 1024 * 1024


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
    # macOS reports bytes; Linux reports KiB.
    return value / (1024.0 * 1024.0) if sys.platform == "darwin" else value / 1024.0


def ctc_character_confidences(logits: Any) -> List[List[float]]:
    """Return confidence for each emitted CTC character, excluding blank/repeats."""
    import torch

    if logits.ndim != 3:
        raise ValueError(f"expected CTC logits [time,batch,classes], got {tuple(logits.shape)}")
    probabilities = torch.softmax(logits.float(), dim=2)
    best_probability, best_token = probabilities.max(dim=2)
    result: List[List[float]] = []
    for batch_index in range(best_token.shape[1]):
        previous = -1
        confidences: List[float] = []
        for token, probability in zip(
            best_token[:, batch_index].detach().cpu().tolist(),
            best_probability[:, batch_index].detach().cpu().tolist(),
        ):
            token = int(token)
            if token != 0 and token != previous:
                confidences.append(float(probability))
            elif token != 0 and confidences:
                # A repeated CTC time step belongs to the current character. Keep its strongest
                # evidence rather than allowing sequence length to inflate the mean.
                confidences[-1] = max(confidences[-1], float(probability))
            previous = token
        result.append(confidences)
    return result


class MockRecognizer:
    """Dependency-free deterministic recognizer used by protocol/unit tests."""

    version = NOMEROFF_VERSION
    commit = NOMEROFF_COMMIT
    device = "cpu"
    model_name = "mock-kz"
    fp16 = False

    def __init__(self, region: str) -> None:
        self.region = region

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> List[Recognition]:
        results = []
        for crop in crops:
            if crop.size == 0:
                results.append(Recognition("", 0.0, 0.0, self.region, 0.0))
            else:
                confidence = min(0.99, 0.80 + float(crop.mean()) / 2550.0)
                results.append(Recognition("123ABC02", confidence, confidence - 0.04,
                                           self.region, 0.05))
        return results


class NomeroffRecognizer:
    """Crop-only Nomeroff v4 adapter using one explicit regional model."""

    def __init__(self, region: str, lines: int, requested_device: str, fp16: bool,
                 model_cache: str) -> None:
        os.environ["LOCAL_STORAGE"] = str(Path(model_cache).resolve())
        Path(os.environ["LOCAL_STORAGE"]).mkdir(parents=True, exist_ok=True)

        import torch
        import nomeroff_net
        import nomeroff_net.pipes.number_plate_text_readers.base.ocr as ocr_module
        import nomeroff_net.pipes.number_plate_text_readers.text_detector as detector_module
        from nomeroff_net.pipes.number_plate_text_readers.text_detector import TextDetector

        installed = getattr(nomeroff_net, "__version__", "unknown")
        if installed != NOMEROFF_VERSION:
            raise RuntimeError(
                f"Nomeroff version mismatch: expected {NOMEROFF_VERSION}, imported {installed}"
            )

        self.torch = torch
        self.region = region
        self.lines = lines
        self.requested_device = requested_device
        self.device = self._select_device(requested_device)
        self.fp16 = bool(fp16 and self.device == "cuda")
        self.version = installed
        self.commit = NOMEROFF_COMMIT
        self.model_name = self._model_name(region, lines)

        # Nomeroff v4 stores the selected device in module globals. Set both import locations
        # before model construction so loading and forward use the same device.
        ocr_module.device_torch = self.device
        detector_module.device_torch = self.device
        preset = {
            self.model_name: {
                "for_regions": [region],
                "for_count_lines": [lines],
                "model_path": "latest",
            }
        }
        self.detector = TextDetector(
            presets=preset,
            default_label=region,
            default_lines_count=lines,
            off_number_plate_classification=True,
        )
        if self.fp16:
            for detector in self.detector.detectors:
                detector.model.half()

    @staticmethod
    def _model_name(region: str, lines: int) -> str:
        if lines == 2:
            return "su_2lines_efficientnet_b2" if region == "su" else "eu_2lines_efficientnet_b2"
        return {
            "kz": "kz",
            "ru": "ru",
            "by": "by",
            "kg": "kg",
            "su": "su_efficientnet_b2",
        }[region]

    def _select_device(self, requested: str) -> str:
        torch = self.torch
        if requested == "cuda":
            if not torch.cuda.is_available():
                raise RuntimeError("DEVICE_UNAVAILABLE: CUDA requested but torch.cuda is unavailable")
            return "cuda"
        if requested == "mps":
            if not hasattr(torch.backends, "mps") or not torch.backends.mps.is_available():
                raise RuntimeError("DEVICE_UNAVAILABLE: MPS requested but unavailable")
            return "mps"
        if requested == "cpu":
            return "cpu"
        if torch.cuda.is_available():
            return "cuda"
        if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            return "mps"
        return "cpu"

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> List[Recognition]:
        crops = list(crops)
        if not crops:
            return []
        if any(crop.ndim != 3 or crop.shape[2] != 3 or crop.size == 0 for crop in crops):
            raise ValueError("INVALID_CROP: every crop must be a non-empty BGR image")

        started = time.perf_counter()
        labels = [self.region] * len(crops)
        lines = [self.lines] * len(crops)
        predicted = self.detector.define_order_detector(crops, labels, lines)
        confidence_by_order: Dict[int, List[float]] = {}

        with self.torch.inference_mode():
            for key, group in predicted.items():
                tensor = self.torch.as_tensor(np.asarray(group["xs"]), device=self.device)
                tensor = tensor.half() if self.fp16 else tensor.float()
                logits = self.detector.detectors[int(key)].forward(tensor)
                group["ys"] = logits
                confidence_rows = ctc_character_confidences(logits)
                for order, confidences in zip(group["order"], confidence_rows):
                    confidence_by_order.setdefault(int(order), []).extend(confidences)

            texts = self.detector.postprocess(predicted)
            if self.device == "cuda":
                self.torch.cuda.synchronize()

        elapsed_ms = (time.perf_counter() - started) * 1000.0
        per_crop_ms = elapsed_ms / len(crops)
        results = []
        for index, text in enumerate(texts):
            confidences = confidence_by_order.get(index, [])
            mean = sum(confidences) / len(confidences) if confidences else 0.0
            minimum = min(confidences) if confidences else 0.0
            results.append(Recognition(str(text).upper(), mean, minimum, self.region, per_crop_ms))
        while len(results) < len(crops):
            results.append(Recognition("", 0.0, 0.0, self.region, per_crop_ms))
        return results


def _build_recognizer(args: argparse.Namespace) -> Tuple[Any, float, Optional[str]]:
    started = time.perf_counter()
    fallback = None  # type: Optional[str]
    if args.mock or os.environ.get("NOMEROFF_MOCK") == "1":
        recognizer: Any = MockRecognizer(args.region)
    else:
        try:
            recognizer = NomeroffRecognizer(args.region, args.lines, args.device, args.fp16,
                                            args.model_cache)
            recognizer.recognize_batch([np.full((50, 200, 3), 114, dtype=np.uint8)])
        except Exception as exc:
            # MPS support varies by PyTorch/Nomeroff operation. Auto may try it, but only a real
            # warm-up earns the right to keep it. Explicit MPS remains a hard failure.
            import torch
            auto_selected_mps = (
                args.device == "auto"
                and not torch.cuda.is_available()
                and hasattr(torch.backends, "mps")
                and torch.backends.mps.is_available()
            )
            if auto_selected_mps:
                fallback = f"mps_to_cpu: {_safe_field(exc)}"
                recognizer = NomeroffRecognizer(args.region, args.lines, "cpu", False,
                                                args.model_cache)
                recognizer.recognize_batch([np.full((50, 200, 3), 114, dtype=np.uint8)])
            else:
                raise
    return recognizer, (time.perf_counter() - started) * 1000.0, fallback


def _metadata(recognizer: Any, startup_ms: float,
              fallback: Optional[str]) -> Dict[str, Any]:
    torch_version = "not-loaded"
    cuda_version = None
    cuda_memory_mb = 0.0
    try:
        import torch
        torch_version = torch.__version__
        cuda_version = torch.version.cuda
        if getattr(recognizer, "device", "") == "cuda":
            cuda_memory_mb = torch.cuda.memory_allocated() / (1024.0 * 1024.0)
    except ImportError:
        pass
    return {
        "nomeroff_version": recognizer.version,
        "nomeroff_commit": recognizer.commit,
        "python": platform.python_version(),
        "torch": torch_version,
        "cuda": cuda_version,
        "device": recognizer.device,
        "fp16": recognizer.fp16,
        "model": recognizer.model_name,
        "region": recognizer.region,
        "startup_ms": startup_ms,
        "rss_mb": _rss_mb(),
        "cuda_memory_mb": cuda_memory_mb,
        "fallback": fallback,
    }


def _serve(recognizer: Any, startup_ms: float, fallback: Optional[str]) -> int:
    meta = _metadata(recognizer, startup_ms, fallback)
    ready = [
        "READY", meta["nomeroff_version"], meta["nomeroff_commit"], meta["device"],
        meta["model"], meta["region"], "1" if meta["fp16"] else "0",
        f"{meta['startup_ms']:.3f}", f"{meta['rss_mb']:.3f}",
        f"{meta['cuda_memory_mb']:.3f}", meta["fallback"] or "",
    ]
    print("\t".join(_safe_field(item) for item in ready), flush=True)

    stdin = sys.stdin.buffer
    while True:
        header = stdin.readline()
        if not header:
            return 0
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
                raise ValueError("PROTOCOL_ERROR: expected OCR id rows cols channels bytes")
            request_id = int(fields[1])
            rows, cols, channels, byte_count = map(int, fields[2:6])
            # Field 6 is reserved for future flags and must currently be zero.
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
            print(
                "\t".join([
                    "RESULT", str(request_id), _safe_field(result.text),
                    f"{result.confidence:.8f}", f"{result.min_char_confidence:.8f}",
                    _safe_field(result.region), f"{result.inference_ms:.4f}",
                ]),
                flush=True,
            )
        except (BrokenPipeError, EOFError):
            return 1
        except RuntimeError as exc:
            code = "CUDA_OOM" if "out of memory" in str(exc).lower() else "OCR_ERROR"
            request_id = fields[1] if "fields" in locals() and len(fields) > 1 else "0"
            print(f"ERROR\t{request_id}\t{code}\t{_safe_field(exc)}", flush=True)
        except Exception as exc:
            request_id = fields[1] if "fields" in locals() and len(fields) > 1 else "0"
            print(f"ERROR\t{request_id}\tOCR_ERROR\t{_safe_field(exc)}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Nomeroff Net v4 crop OCR worker")
    parser.add_argument("--serve", action="store_true", help="run the binary stdin/stdout protocol")
    parser.add_argument("--probe", action="store_true", help="load, warm up, and print metadata JSON")
    parser.add_argument("--mock", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--region", choices=("kz", "ru", "by", "kg", "su"), default="kz")
    parser.add_argument("--lines", choices=(1, 2), type=int, default=1)
    parser.add_argument("--device", choices=("auto", "cpu", "mps", "cuda"), default="auto")
    precision = parser.add_mutually_exclusive_group()
    precision.add_argument("--fp16", dest="fp16", action="store_true")
    precision.add_argument("--no-fp16", dest="fp16", action="store_false")
    parser.set_defaults(fp16=True)
    parser.add_argument("--model-cache", default="models/nomeroff")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        # Several upstream loaders print download/status messages to stdout. Stdout is our binary
        # protocol channel, so redirect all construction chatter to stderr even in probe mode.
        with contextlib.redirect_stdout(sys.stderr):
            recognizer, startup_ms, fallback = _build_recognizer(args)
    except RuntimeError as exc:
        code = "CUDA_OOM" if "out of memory" in str(exc).lower() else "MODEL_LOAD_FAILED"
        print(f"{code}: {_safe_field(exc)}", file=sys.stderr)
        return 4
    except Exception as exc:
        print(f"MODEL_LOAD_FAILED: {_safe_field(exc)}", file=sys.stderr)
        return 4

    if args.probe:
        print(json.dumps(_metadata(recognizer, startup_ms, fallback), sort_keys=True))
        return 0
    if args.serve:
        return _serve(recognizer, startup_ms, fallback)
    print("Specify --probe or --serve", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
