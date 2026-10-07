#!/usr/bin/env python3
"""Persistent crop-only PaddleOCR/EasyOCR worker used by the C++ research harness.

Both libraries are intentionally run in recognition-only mode. The native YOLO detector has
already produced a tight plate crop, so running a document/scene text detector again would make
the comparison slower and less accurate. Stdout is reserved for the binary protocol; all library
startup chatter is redirected to stderr.

With --device auto and KZ_ANPR_OCR_ACCELERATOR_TIMEOUT_S set, the accelerator start runs in a
child process. If it fails or exceeds that budget, the child is killed (returning the Jetson's
shared GPU memory) and this process serves the same model on CPU, naming the reason in the
READY line's fallback field.
"""

import argparse
import contextlib
import json
import os
import platform
import re
import resource
import select
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

try:
    from importlib import metadata as importlib_metadata
except ImportError:  # Python 3.6 in JetPack 4.x.
    import importlib_metadata

import numpy as np


MAX_CROP_BYTES = 32 * 1024 * 1024
ACCELERATOR_PROGRESS_INTERVAL_S = 30.0
STARTED = time.monotonic()
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


def _mem_available_mb() -> str:
    try:
        with open("/proc/meminfo") as meminfo:
            for line in meminfo:
                if line.startswith("MemAvailable:"):
                    return str(int(line.split()[1]) // 1024)
    except (OSError, ValueError, IndexError):
        pass
    return "unknown"


def log_stage(stage: str, **fields: Any) -> None:
    """Report startup progress on stderr in the C++ log format.

    On the 4 GB Nano a model load can take minutes; these lines show which stage is slow and how
    much memory is left when it starts.
    """
    parts = [
        f"ts_ms={int(time.monotonic() * 1000)}", "level=info", "event=research_ocr_stage",
        f"stage={stage}", f"pid={os.getpid()}",
        f"elapsed_ms={(time.monotonic() - STARTED) * 1000.0:.0f}",
        f"mem_available_mb={_mem_available_mb()}",
    ]
    parts.extend(f"{key}={_safe_field(value)}" for key, value in fields.items())
    print(" ".join(parts), file=sys.stderr, flush=True)


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

    def __init__(self, engine: str, requested_device: str = "cpu",
                 accelerator: str = "none") -> None:
        self.engine = engine
        if requested_device == "auto" and accelerator == "fail":
            raise RuntimeError("DEVICE_UNAVAILABLE: mock accelerator failure")
        if requested_device == "auto" and accelerator == "hang":
            time.sleep(3600)
        if requested_device == "auto" and accelerator == "ok":
            self.device = "cuda"

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> List[Recognition]:
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
    model_name = "english_g2"
    fp16 = False

    def __init__(self, languages: List[str], requested_device: str, model_cache: Path,
                 allow_download: bool) -> None:
        log_stage("import_torch", engine=self.engine, device=requested_device)
        import torch
        log_stage("import_easyocr", torch=torch.__version__)
        import easyocr

        if requested_device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("DEVICE_UNAVAILABLE: CUDA requested but torch.cuda is unavailable")
        if requested_device == "mps":
            raise RuntimeError("DEVICE_UNAVAILABLE: EasyOCR research worker supports cpu/cuda")
        use_cuda = requested_device == "cuda" or (
            requested_device == "auto" and torch.cuda.is_available()
        )
        self.device = "cuda" if use_cuda else "cpu"
        # cuDNN 8 on JetPack 4.6 loads several hundred MB of kernels on first use. The Nano shares
        # that memory with the TensorRT detector, so its launcher can opt out; PyTorch's native
        # CUDA convolution and LSTM kernels give the same results.
        if use_cuda and os.environ.get("KZ_ANPR_EASYOCR_CUDNN", "1") == "0":
            torch.backends.cudnn.enabled = False
        self.version = importlib_metadata.version("easyocr")
        storage = model_cache / "easyocr"
        storage.mkdir(parents=True, exist_ok=True)
        log_stage("load_model", device=self.device,
                  cudnn=int(use_cuda and torch.backends.cudnn.enabled))
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

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> List[Recognition]:
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


def _paddle_payload(result: Any) -> Dict[str, Any]:
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

        self.version = importlib_metadata.version("paddleocr")
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

    def recognize_batch(self, crops: Iterable[np.ndarray]) -> List[Recognition]:
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


def _build_recognizer(args: argparse.Namespace) -> Tuple[Any, float]:
    started = time.perf_counter()
    if args.mock:
        recognizer: Any = MockRecognizer(args.engine, args.device, args.mock_accelerator)
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
    log_stage("warmup", device=recognizer.device)
    recognizer.recognize_batch([np.full((64, 192, 3), 114, dtype=np.uint8)])
    startup_ms = (time.perf_counter() - started) * 1000.0
    log_stage("ready", device=recognizer.device, startup_ms=f"{startup_ms:.0f}")
    return recognizer, startup_ms


def _accelerator_memory_mb(recognizer: Any) -> float:
    # A CUDA recognizer has already imported torch; never pay for the import just to report 0.
    torch = sys.modules.get("torch")
    if getattr(recognizer, "device", "") != "cuda" or torch is None:
        return 0.0
    return torch.cuda.memory_allocated() / (1024.0 * 1024.0)


def _ready_line(recognizer: Any, startup_ms: float, fallback: str = "") -> str:
    ready = [
        "READY", recognizer.engine, recognizer.version, recognizer.device,
        recognizer.model_name, recognizer.region, "1" if recognizer.fp16 else "0",
        f"{startup_ms:.3f}", f"{_rss_mb():.3f}",
        f"{_accelerator_memory_mb(recognizer):.3f}", fallback,
    ]
    return "\t".join(_safe_field(item) for item in ready)


def _serve(recognizer: Any, startup_ms: float, fallback: str = "") -> int:
    print(_ready_line(recognizer, startup_ms, fallback), flush=True)
    return _serve_requests(recognizer)


def _serve_requests(recognizer: Any) -> int:
    stdin = sys.stdin.buffer
    while True:
        header = stdin.readline()
        if not header:
            return 0
        fields: List[str] = []
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


def _accelerator_timeout_s() -> float:
    value = os.environ.get("KZ_ANPR_OCR_ACCELERATOR_TIMEOUT_S", "").strip()
    if not value:
        return 0.0
    try:
        return max(0.0, float(value))
    except ValueError:
        raise ValueError(f"INVALID_CONFIG: KZ_ANPR_OCR_ACCELERATOR_TIMEOUT_S={value!r}")


def _accelerator_child(args: argparse.Namespace, ready_fd: int) -> int:
    """Load with --device auto, report READY (or FAILED) to the parent, then serve the pipes."""
    try:
        with contextlib.redirect_stdout(sys.stderr):
            recognizer, startup_ms = _build_recognizer(args)
        message = _ready_line(recognizer, startup_ms)
    except Exception as exc:
        os.write(ready_fd, f"FAILED\t{_safe_field(exc)}\n".encode("utf-8", "replace"))
        return 4
    os.write(ready_fd, f"{message}\n".encode("utf-8", "replace"))
    os.close(ready_fd)
    # The parent prints READY only after reading the line above and the client sends nothing
    # before READY, so stdin is still unread and stdout carries nothing of ours yet.
    return _serve_requests(recognizer)


def _reap(child: int) -> str:
    try:
        os.kill(child, signal.SIGKILL)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(child, 0)
    if os.WIFSIGNALED(status):
        return f"signal {os.WTERMSIG(status)}"
    return f"exit {os.WEXITSTATUS(status)}"


def _wait_for_accelerator(child: int, ready_fd: int, timeout_s: float) -> Tuple[str, str]:
    """Return (READY line, "") or ("", fallback reason). The child is reaped on failure."""
    started = time.monotonic()
    next_report = started + ACCELERATOR_PROGRESS_INTERVAL_S
    data = b""
    while b"\n" not in data:
        now = time.monotonic()
        if now - started >= timeout_s:
            status = _reap(child)
            return "", f"accelerator startup exceeded {timeout_s:.0f} s (child {status})"
        if now >= next_report:
            log_stage("waiting_for_accelerator", child=child, waited_s=f"{now - started:.0f}",
                      timeout_s=f"{timeout_s:.0f}")
            next_report = now + ACCELERATOR_PROGRESS_INTERVAL_S
        wait_s = min(started + timeout_s, next_report) - now
        readable, _, _ = select.select([ready_fd], [], [], max(0.0, wait_s))
        if not readable:
            continue
        chunk = os.read(ready_fd, 65536)
        if not chunk:
            status = _reap(child)
            return "", f"accelerator worker died during startup ({status})"
        data += chunk
    line = data.split(b"\n", 1)[0].decode("utf-8", "replace")
    if line.startswith("READY\t"):
        return line, ""
    status = _reap(child)
    reason = line.split("\t", 1)[1] if line.startswith("FAILED\t") else line
    return "", f"{reason} ({status})"


def _serve_with_cpu_fallback(args: argparse.Namespace, timeout_s: float) -> int:
    """Try --device auto in a child process; serve on CPU here if it fails or is too slow.

    A CUDA start that is wedged or swapping cannot be interrupted from inside its own process.
    This parent imports no ML library before the child has finished, so killing the child frees
    every accelerator allocation before the CPU model loads.
    """
    sys.stdout.flush()
    sys.stderr.flush()
    ready_read, ready_write = os.pipe()
    child = os.fork()
    if child == 0:
        status = 4
        try:
            os.close(ready_read)
            status = _accelerator_child(args, ready_write)
        finally:
            os._exit(status)

    os.close(ready_write)

    def forward_sigterm(signum: int, _frame: Any) -> None:
        try:
            os.kill(child, signum)
        except ProcessLookupError:
            pass
        os._exit(128 + signum)

    signal.signal(signal.SIGTERM, forward_sigterm)
    ready, reason = _wait_for_accelerator(child, ready_read, timeout_s)
    os.close(ready_read)
    if ready:
        print(ready, flush=True)
        # Leave the protocol pipes to the child alone, so the client sees EOF when it exits.
        devnull = os.open(os.devnull, os.O_RDWR)
        os.dup2(devnull, 0)
        os.dup2(devnull, 1)
        os.close(devnull)
        _, status = os.waitpid(child, 0)
        if os.WIFSIGNALED(status):
            return 128 + os.WTERMSIG(status)
        return os.WEXITSTATUS(status)

    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    log_stage("cpu_fallback", reason=reason)
    args.device = "cpu"
    with contextlib.redirect_stdout(sys.stderr):
        recognizer, startup_ms = _build_recognizer(args)
    return _serve(recognizer, startup_ms, fallback=reason)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--mock", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--mock-accelerator", choices=("none", "ok", "fail", "hang"),
                        default="none", help=argparse.SUPPRESS)
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
        accelerator_timeout_s = _accelerator_timeout_s()
        if args.serve and args.device == "auto" and accelerator_timeout_s > 0:
            return _serve_with_cpu_fallback(args, accelerator_timeout_s)
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
