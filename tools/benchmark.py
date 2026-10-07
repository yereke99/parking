#!/usr/bin/env python3
"""Reproducible 1/2/4-stream ANPR benchmark, sequential OCR comparison and telemetry collector."""

import argparse
import copy
import csv
import datetime as dt
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple, Union


ROOT = Path(__file__).resolve().parents[1]
OCR_BACKENDS = ("fast_plate_ocr", "nomeroff", "paddleocr", "easyocr")
RESEARCH_VIDEOS = ("video/car.mp4", "video/parking.mp4", "video/parking2.mp4")
# --ocr-benchmark rows, run strictly one after another. EasyOCR has two: the in-process TensorRT
# export and the PyTorch worker it replaces.
OCR_BENCHMARK_ENGINES = (
    ("nomeroff_onnx", "Nomeroff KZ (ONNX/TensorRT)"),
    ("easyocr_onnx", "EasyOCR (ONNX/TensorRT)"),
    ("easyocr", "EasyOCR (PyTorch worker)"),
    ("nomeroff", "Nomeroff-Net"),
    ("paddleocr", "PaddleOCR"),
    ("fast_plate_ocr", "Fast-Plate-OCR"),
)
CYRILLIC_LOOKALIKES = str.maketrans("АВСЕНКМОРТХУІЈавсенкмортхуіј",
                                    "ABCEHKMOPTXYIJABCEHKMOPTXYIJ")


def command_output(command: List[str]) -> Optional[str]:
    try:
        result = subprocess.run(command, universal_newlines=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = (result.stdout + result.stderr).strip()
    return output or None


def descendants(root_pid: int) -> Set[int]:
    output = command_output(["ps", "-axo", "ppid=,pid="])
    if not output:
        return {root_pid}
    children: Dict[int, List[int]] = {}
    for line in output.splitlines():
        try:
            parent, child = map(int, line.split())
        except ValueError:
            continue
        children.setdefault(parent, []).append(child)
    result = {root_pid}
    frontier = [root_pid]
    while frontier:
        child_list = children.get(frontier.pop(), [])
        for child in child_list:
            if child not in result:
                result.add(child)
                frontier.append(child)
    return result


def process_usage(root_pid: int) -> Tuple[float, float]:
    pids = descendants(root_pid)
    output = command_output(["ps", "-axo", "pid=,%cpu=,rss="])
    cpu = 0.0
    rss_kib = 0.0
    if output:
        for line in output.splitlines():
            fields = line.split()
            if len(fields) != 3:
                continue
            try:
                pid = int(fields[0])
                if pid in pids:
                    cpu += float(fields[1])
                    rss_kib += float(fields[2])
            except ValueError:
                continue
    return cpu, rss_kib / 1024.0


def nvidia_sample() -> Optional[Dict[str, float]]:
    if shutil.which("nvidia-smi") is None:
        return None
    output = command_output([
        "nvidia-smi", "--query-gpu=utilization.gpu,memory.used,temperature.gpu,power.draw",
        "--format=csv,noheader,nounits",
    ])
    if not output:
        return None
    try:
        gpu, memory, temperature, power = [float(item.strip()) for item in output.splitlines()[0].split(",")]
    except (ValueError, IndexError):
        return None
    return {"gpu_util_percent": gpu, "gpu_memory_mb": memory,
            "temperature_c": temperature, "power_w": power}


class ResourceSampler:
    def __init__(self, pid: int) -> None:
        self.pid = pid
        self.samples: List[Dict[str, float]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._tegrastats = None  # type: Optional[subprocess.Popen]
        self._tegrastats_thread = None  # type: Optional[threading.Thread]
        self._tegrastats_lines: List[str] = []

    def start(self) -> None:
        tegrastats = shutil.which("tegrastats")
        if tegrastats:
            self._tegrastats = subprocess.Popen(
                [tegrastats, "--interval", "500"], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, universal_newlines=True,
            )
            self._tegrastats_thread = threading.Thread(target=self._read_tegrastats, daemon=True)
            self._tegrastats_thread.start()
        self._thread.start()

    def stop(self) -> Dict[str, Any]:
        self._stop.set()
        self._thread.join(timeout=2)
        if self._tegrastats is not None:
            self._tegrastats.terminate()
            try:
                self._tegrastats.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._tegrastats.kill()
                self._tegrastats.wait(timeout=2)
        if self._tegrastats_thread is not None:
            self._tegrastats_thread.join(timeout=2)
        if not self.samples:
            return {"samples": 0}

        def summary(key: str) -> Optional[Dict[str, float]]:
            values = [sample[key] for sample in self.samples if key in sample]
            if not values:
                return None
            return {"avg": sum(values) / len(values), "peak": max(values)}

        result = {
            "samples": len(self.samples),
            "cpu_percent": summary("cpu_percent"),
            "rss_mb": summary("rss_mb"),
            "system_ram_used_mb": summary("system_ram_used_mb"),
            "gpu_util_percent": summary("gpu_util_percent"),
            "gpu_memory_mb": summary("gpu_memory_mb"),
            "temperature_c": summary("temperature_c"),
            "power_w": summary("power_w"),
        }
        if self._tegrastats_lines:
            result["tegrastats_samples"] = len(self._tegrastats_lines)
            result["tegrastats_last"] = self._tegrastats_lines[-1]
            result["throttling_observed"] = any(
                "throt" in line.lower() for line in self._tegrastats_lines
            )
        return result

    def _read_tegrastats(self) -> None:
        assert self._tegrastats is not None and self._tegrastats.stdout is not None
        for line in self._tegrastats.stdout:
            line = line.strip()
            if not line:
                continue
            self._tegrastats_lines.append(line)
            sample: Dict[str, float] = {}
            gpu = re.search(r"GR3D_FREQ\s+(\d+(?:\.\d+)?)%", line)
            ram = re.search(r"RAM\s+(\d+(?:\.\d+)?)/(\d+(?:\.\d+)?)MB", line)
            power = re.search(r"VDD_IN\s+(\d+(?:\.\d+)?)mW", line)
            temperatures = [float(value) for value in re.findall(r"\w+@([\d.]+)C", line)]
            if gpu:
                sample["gpu_util_percent"] = float(gpu.group(1))
            if ram:
                sample["system_ram_used_mb"] = float(ram.group(1))
            if power:
                sample["power_w"] = float(power.group(1)) / 1000.0
            if temperatures:
                sample["temperature_c"] = max(temperatures)
            if sample:
                self.samples.append(sample)

    def _run(self) -> None:
        while not self._stop.wait(0.5):
            cpu, rss = process_usage(self.pid)
            sample = {"cpu_percent": cpu, "rss_mb": rss}
            gpu = nvidia_sample()
            if gpu:
                sample.update(gpu)
            self.samples.append(sample)


def levenshtein(left: str, right: str) -> int:
    previous = list(range(len(right) + 1))
    for left_index, left_char in enumerate(left, 1):
        current = [left_index]
        for right_index, right_char in enumerate(right, 1):
            current.append(min(
                current[-1] + 1,
                previous[right_index] + 1,
                previous[right_index - 1] + (left_char != right_char),
            ))
        previous = current
    return previous[-1]


def load_ground_truth(path: Optional[Path]) -> Dict[str, List[Dict[str, Any]]]:
    if path is None:
        return {}
    truth: Dict[str, List[Dict[str, Any]]] = {}

    def optional_int(value: Optional[str]) -> Optional[int]:
        value = (value or "").strip()
        return int(value) if value else None

    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            expected = (row.get("expected_plate") or "").strip().upper()
            source = (row.get("path") or "").strip()
            if expected and source:
                item = {
                    "expected": expected,
                    "plate_region": (row.get("plate_region") or "unspecified").strip().lower(),
                    "event_id": (row.get("event_id") or "").strip(),
                    "start_ms": optional_int(row.get("start_ms")),
                    "stop_ms": optional_int(row.get("stop_ms")),
                }
                truth.setdefault(source, []).append(item)
                basename = Path(source).name
                if basename != source:
                    truth.setdefault(basename, []).append(item)
    return truth


def accuracy_report(run: Dict[str, Any], sources: List[str],
                    truth: Dict[str, List[Dict[str, Any]]],
                    failure_root: Path) -> Dict[str, Any]:
    cases: List[Dict[str, Any]] = []
    for index, stream in enumerate(run.get("per_stream", [])):
        source = sources[index] if index < len(sources) else sources[0]
        labels = truth.get(source) or truth.get(Path(source).name)
        if not labels:
            continue
        stream_events = stream.get("events", [])
        metrics = stream.get("metrics", {})
        for label_index, label in enumerate(labels):
            expected = label["expected"]
            start_ms, stop_ms = label["start_ms"], label["stop_ms"]
            if start_ms is not None or stop_ms is not None:
                events = [
                    event for event in stream_events
                    if (start_ms is None or int(event.get("timestamp_ms", -1)) >= start_ms)
                    and (stop_ms is None or int(event.get("timestamp_ms", -1)) <= stop_ms)
                ]
            elif len(labels) > 1:
                events = stream_events[label_index:label_index + 1]
            else:
                events = stream_events
            accepted = [
                event for event in events
                if event.get("status") in ("VALID_HIGH_CONFIDENCE", "VALID_LOW_CONFIDENCE")
            ]
            predicted = accepted[0].get("normalized_plate", "") if accepted else ""
            candidates = [event for event in events if event.get("normalized_plate")]
            candidate_predicted = candidates[0].get("normalized_plate", "") if candidates else ""
            detected = len(labels) == 1 and int(metrics.get("detections", 0)) > 0
            ocr_attempted = len(labels) == 1 and int(metrics.get("ocr_calls", 0)) > 0
            status = (accepted[0].get("status") if accepted else
                      (events[0].get("status") if events else
                       ("PLATE_DETECTED_OCR_FAILED" if ocr_attempted else
                        ("PLATE_DETECTED_NO_OCR_ATTEMPT" if detected else
                         "PLATE_COMPLETELY_MISSED"))))
            distance = levenshtein(expected, predicted)
            case = {
                "camera_id": stream.get("camera_id"), "source": source,
                "event_id": label["event_id"], "expected": expected,
                "predicted": predicted, "exact": predicted == expected,
                "ocr_candidate": candidate_predicted,
                "ocr_exact": candidate_predicted == expected,
                "ocr_edit_distance": levenshtein(expected, candidate_predicted),
                "edit_distance": distance, "status": status,
                "plate_region": label["plate_region"],
            }
            cases.append(case)
            if predicted != expected:
                statuses = {event.get("status") for event in events}
                if "LOW_CONFIDENCE" in statuses or status == "LOW_CONFIDENCE":
                    category = "low_confidence"
                else:
                    category = "failures" if not predicted else "false_ocr"
                destination = failure_root / category
                destination.mkdir(parents=True, exist_ok=True)
                for event in events:
                    crop = event.get("best_crop_path")
                    if crop and Path(crop).is_file():
                        shutil.copy2(crop, destination / Path(crop).name)
                # A failed OCR may not produce a best consensus crop. The C++ pipeline still
                # saves every attempted crop in benchmark/debug mode, named with its camera id.
                debug_crops = failure_root / "debug" / "crops"
                copied = {path.name for path in destination.glob("*")}
                for crop in debug_crops.glob(f"{stream.get('camera_id')}_*.jpg"):
                    if crop.name not in copied:
                        shutil.copy2(crop, destination / crop.name)

    if not cases:
        return {"available": False, "reason": "manifest has no labels for benchmark sources"}
    total_chars = sum(len(case["expected"]) for case in cases)
    total_errors = sum(case["edit_distance"] for case in cases)
    total_ocr_errors = sum(case["ocr_edit_distance"] for case in cases)
    exact = sum(case["exact"] for case in cases)
    ocr_exact = sum(case["ocr_exact"] for case in cases)
    failure_modes = {
        "plate_completely_missed": sum(not case["predicted"] and case["status"] == "PLATE_COMPLETELY_MISSED"
                                        for case in cases),
        "plate_detected_ocr_failed": sum(not case["predicted"] and case["status"] != "PLATE_COMPLETELY_MISSED"
                                          and case["status"] != "PLATE_DETECTED_NO_OCR_ATTEMPT"
                                          for case in cases),
        "plate_detected_no_ocr_attempt": sum(
            case["status"] == "PLATE_DETECTED_NO_OCR_ATTEMPT" for case in cases
        ),
        "wrong_character_count": sum(bool(case["predicted"]) and
                                     len(case["predicted"]) != len(case["expected"])
                                     for case in cases),
        "invalid_format": sum(case["status"] in ("INVALID_FORMAT", "AMBIGUOUS")
                              for case in cases),
        "invalid_kz_format": sum(case["plate_region"] == "kz" and
                                 case["status"] in ("INVALID_FORMAT", "AMBIGUOUS")
                                 for case in cases),
        "low_confidence": sum(case["status"] == "LOW_CONFIDENCE" for case in cases),
        "false_ocr": sum(bool(case["predicted"]) and not case["exact"] for case in cases),
    }
    by_region: Dict[str, Dict[str, Union[float, int]]] = {}
    for region in sorted({case["plate_region"] for case in cases}):
        regional = [case for case in cases if case["plate_region"] == region]
        regional_chars = sum(len(case["expected"]) for case in regional)
        regional_errors = sum(case["edit_distance"] for case in regional)
        regional_ocr_errors = sum(case["ocr_edit_distance"] for case in regional)
        regional_exact = sum(case["exact"] for case in regional)
        regional_ocr_exact = sum(case["ocr_exact"] for case in regional)
        by_region[region] = {
            "cases": len(regional),
            "exact_plate_accuracy": regional_exact / len(regional),
            "ocr_exact_plate_accuracy": regional_ocr_exact / len(regional),
            "character_accuracy": max(0.0, 1.0 - regional_errors / max(1, regional_chars)),
            "cer": regional_errors / max(1, regional_chars),
            "ocr_cer": regional_ocr_errors / max(1, regional_chars),
        }
    return {
        "available": True,
        "cases": len(cases),
        "exact_plate_accuracy": exact / len(cases),
        "ocr_exact_plate_accuracy": ocr_exact / len(cases),
        "character_accuracy": max(0.0, 1.0 - total_errors / max(1, total_chars)),
        "cer": total_errors / max(1, total_chars),
        "ocr_character_accuracy": max(0.0, 1.0 - total_ocr_errors / max(1, total_chars)),
        "ocr_cer": total_ocr_errors / max(1, total_chars),
        "failures": len(cases) - exact,
        "failure_modes": failure_modes,
        "by_region": by_region,
        "details": cases,
    }


def ocr_environment_metadata() -> Dict[str, Any]:
    environments = {
        "nomeroff": (Path(os.environ.get("KZ_ANPR_NOMEROFF_PYTHON",
                                         ROOT / ".venv-nomeroff" / "bin" / "python")),
                     "nomeroff_net"),
        "paddleocr": (Path(os.environ.get("KZ_ANPR_PADDLEOCR_PYTHON",
                                          ROOT / ".venv-paddleocr" / "bin" / "python")),
                      "paddleocr"),
        "easyocr": (Path(os.environ.get("KZ_ANPR_EASYOCR_PYTHON",
                                        ROOT / ".venv-easyocr" / "bin" / "python")),
                    "easyocr"),
    }
    result: Dict[str, Any] = {}
    for name, (python, package) in environments.items():
        if not python.is_file():
            result[name] = {"available": False, "reason": "environment not installed"}
            continue
        script = (
            "import json,platform,pkg_resources;"
            f"print(json.dumps({{'python':platform.python_version(),"
            f"'version':pkg_resources.get_distribution('{package}').version}}))"
        )
        output = command_output([str(python), "-c", script])
        if not output:
            result[name] = {"available": False, "reason": "metadata probe failed"}
            continue
        try:
            result[name] = {"available": True, **json.loads(output.splitlines()[-1])}
        except json.JSONDecodeError:
            result[name] = {"available": False, "reason": output}
    return result


def platform_metadata() -> Dict[str, Any]:
    git_commit = command_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"])
    jetpack = command_output(["bash", "-lc", "cat /etc/nv_tegra_release 2>/dev/null"])
    return {
        "timestamp": dt.datetime.now().astimezone().isoformat(),
        "git_commit": git_commit,
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "ocr_environment": ocr_environment_metadata(),
        "jetpack": jetpack,
        "power_mode": (command_output(["nvpmodel", "-q"]) if shutil.which("nvpmodel")
                       else os.environ.get("KZ_ANPR_POWER_MODE")),
        "jetson_clocks": (command_output(["jetson_clocks", "--show"])
                          if shutil.which("jetson_clocks")
                          else os.environ.get("KZ_ANPR_JETSON_CLOCKS")),
    }


def parse_json_output(stdout: str) -> Dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise RuntimeError("benchmark binary did not emit a JSON report")


def run_once(args: argparse.Namespace, stream_count: int, run_root: Path,
             truth: Dict[str, List[Dict[str, Any]]]) -> Dict[str, Any]:
    sources = args.source[:] or [args.video]
    if len(sources) == 1:
        expanded_sources = sources * stream_count
    else:
        expanded_sources = sources
        if len(expanded_sources) != stream_count:
            raise ValueError("number of --source values must equal --streams")

    crop_root = run_root / f"streams_{stream_count}" / "debug"
    command = [
        str(args.binary), "--config", str(args.config), "--streams", str(stream_count),
        "--warmup-frames", str(args.warmup_frames), "--json", "--save-crops", str(crop_root),
    ]
    if args.max_frames:
        command.extend(["--max-frames", str(args.max_frames)])
    if args.backend:
        command.extend(["--backend", args.backend])
    if args.ocr_backend:
        command.extend(["--ocr-backend", args.ocr_backend])
    if args.source:
        for source in args.source:
            command.extend(["--source", source])
    else:
        command.extend(["--video", args.video])

    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True)
    sampler = ResourceSampler(process.pid)
    sampler.start()
    stdout, stderr = process.communicate()
    resources = sampler.stop()
    if process.returncode != 0:
        raise RuntimeError(
            f"benchmark failed with exit {process.returncode}\nstdout:\n{stdout}\nstderr:\n{stderr}"
        )
    report = parse_json_output(stdout)
    report["resources"] = resources
    report["accuracy"] = accuracy_report(report, expanded_sources, truth,
                                          run_root / f"streams_{stream_count}")
    report["command"] = command
    report["stderr"] = stderr
    report["ocr_backend_requested"] = args.ocr_backend
    report["video"] = expanded_sources[0] if len(set(expanded_sources)) == 1 else "multiple"
    return report


def write_markdown(path: Path, report: Dict[str, Any]) -> None:
    runs = report["runs"]
    lines = [
        "# ANPR Benchmark",
        "",
        f"Timestamp: `{report['metadata']['timestamp']}`",
        "",
        "| Streams | Total FPS | FPS/stream | OCR P50 ms | OCR P95 ms | Peak RAM MB | GPU Mem MB | GPU Util % |",
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for run in runs:
        aggregate = run["aggregate"]
        ocr = aggregate["latency"]["ocr"]
        resources = run["resources"]
        ram = (resources.get("rss_mb") or {}).get("peak")
        gpu_memory = (resources.get("gpu_memory_mb") or {}).get("peak")
        gpu_util = (resources.get("gpu_util_percent") or {}).get("avg")
        fmt = lambda value: "n/a" if value is None else f"{value:.2f}"
        total_fps = aggregate["processed_fps"]
        lines.append(
            f"| {run['streams']} | {total_fps:.2f} | {total_fps / run['streams']:.2f} | "
            f"{ocr['p50_ms']:.2f} | {ocr['p95_ms']:.2f} | {fmt(ram)} | "
            f"{fmt(gpu_memory)} | {fmt(gpu_util)} |"
        )
    lines.extend(["", "## Notes", ""])
    if report["metadata"].get("jetpack") is None:
        lines.append("- This was not a Jetson run; no Jetson performance claim is made.")
    for run in runs:
        accuracy = run["accuracy"]
        if not accuracy.get("available"):
            lines.append(f"- {run['streams']} stream(s): accuracy unavailable ({accuracy['reason']}).")
        else:
            lines.append(
                f"- {run['streams']} stream(s): exact={accuracy['exact_plate_accuracy']:.3f}, "
                f"character={accuracy['character_accuracy']:.3f}, CER={accuracy['cer']:.3f}."
            )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_research_markdown(path: Path, report: Dict[str, Any]) -> None:
    lines = [
        "# Four-OCR Video Research",
        "",
        f"Timestamp: `{report['metadata']['timestamp']}`",
        "",
        "Each stream uses an independent C++ processing thread. Detector and persistent worker "
        "models are shared where the backend supports it.",
        "",
        "| OCR | Video | Streams | Status | FPS/stream | OCR avg ms | OCR p95 ms | Peak RSS MB | Dropped | Timeouts | Accuracy failures | OCR exact | Accepted exact | OCR CER |",
        "| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    def fmt(value: Any) -> str:
        return "n/a" if value is None else f"{float(value):.2f}"

    for run in report["runs"]:
        if run.get("status") != "ok":
            reason = str(run.get("reason", "unavailable")).splitlines()[-1][:100]
            lines.append(
                f"| {run['ocr_backend_requested']} | {Path(run['video']).name} | "
                f"{run['streams']} | unavailable: {reason} | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |"
            )
            continue
        aggregate = run["aggregate"]
        ocr = aggregate["latency"]["ocr"]
        resources = run["resources"]
        accuracy = run["accuracy"]
        exact = accuracy.get("exact_plate_accuracy") if accuracy.get("available") else None
        ocr_exact = accuracy.get("ocr_exact_plate_accuracy") if accuracy.get("available") else None
        ocr_cer = accuracy.get("ocr_cer") if accuracy.get("available") else None
        failures = accuracy.get("failures") if accuracy.get("available") else None
        lines.append(
            f"| {run['ocr_backend_requested']} | {Path(run['video']).name} | "
            f"{run.get('processing_threads', run['streams'])} | ok | "
            f"{aggregate['processed_fps'] / run['streams']:.2f} | {ocr['avg_ms']:.2f} | "
            f"{ocr['p95_ms']:.2f} | {fmt((resources.get('rss_mb') or {}).get('peak'))} | "
            f"{aggregate.get('frames_dropped', 0)} | {aggregate.get('recognition_timeouts', 0)} | "
            f"{'n/a' if failures is None else int(failures)} | {fmt(ocr_exact)} | "
            f"{fmt(exact)} | {fmt(ocr_cer)} |"
        )

    kz_runs = [
        run for run in report["runs"]
        if run.get("status") == "ok" and Path(run["video"]).name == "parking.mp4"
        and run["streams"] == 1 and run["accuracy"].get("available")
    ]
    lines.extend(["", "## Selection rule", ""])
    if kz_runs:
        ranked = sorted(
            kz_runs,
            key=lambda run: (
                -run["accuracy"]["ocr_exact_plate_accuracy"],
                -run["accuracy"]["exact_plate_accuracy"],
                run["accuracy"]["ocr_cer"],
                run["aggregate"]["latency"]["ocr"]["p95_ms"],
            ),
        )
        winner = ranked[0]
        lines.append(
            f"On this device and the single labelled KZ clip, `{winner['ocr_backend_requested']}` "
            f"ranks first by OCR exact accuracy, accepted exact accuracy, CER, then OCR p95. "
            "A correct low-confidence candidate is visible but is not counted as barrier-accepted. "
            "This is a research result, "
            "not a production accuracy claim; one plate is not a representative validation set."
        )
    else:
        lines.append(
            "No backend completed the labelled KZ run, so the harness does not declare a winner."
        )
    lines.extend([
        "",
        "The unlabelled `parking2.mp4` row is useful for throughput, thermal and missed-frame "
        "testing only. It must not be used as an accuracy score.",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def normalize_plate(text: str) -> str:
    """The workers' normalization: plate-font Cyrillic lookalikes to Latin, alphanumerics only."""
    return re.sub(r"[^0-9A-Z]", "", str(text).upper().translate(CYRILLIC_LOOKALIKES))


def mem_available_mb() -> Optional[float]:
    try:
        with open("/proc/meminfo") as meminfo:
            for line in meminfo:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 1024.0
    except (OSError, ValueError, IndexError):
        pass
    return None


def run_measured(command: List[str], label: str) -> Dict[str, Any]:
    """Runs one benchmark process to completion while sampling its process tree."""
    available_before = mem_available_mb()
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True)
    sampler = ResourceSampler(process.pid)
    sampler.start()
    stdout, stderr = process.communicate()
    resources = sampler.stop()
    wall_seconds = time.monotonic() - started
    if process.returncode != 0:
        lines = (stderr or stdout).strip().splitlines()
        # Lead with the line that names the failure; worker progress lines come first otherwise.
        errors = [line for line in lines if re.search(
            r"UNAVAILABLE|NOT_FOUND|INVALID|Error|error|failed|Traceback", line)]
        tail = "\n".join(errors[-2:] + lines[-6:])
        if process.returncode < 0:
            signal_number = -process.returncode
            cause = "killed by signal {0}{1}".format(
                signal_number, " (likely out of memory)" if signal_number == 9 else "")
        else:
            cause = "exit {0}".format(process.returncode)
        raise RuntimeError("{0} failed: {1}\n{2}".format(label, cause, tail))
    report = parse_json_output(stdout)
    report["resources"] = resources
    report["wall_seconds"] = wall_seconds
    report["mem_available_before_mb"] = available_before
    report["command"] = command
    report["log_findings"] = log_findings(stderr)
    return report


def log_findings(stderr: str) -> List[str]:
    """Fallbacks, unavailable providers and worker failures worth reporting from a run's log."""
    findings = []
    for line in stderr.splitlines():
        if ("backend_unavailable" in line or "level=error" in line or "level=warn" in line or
                ("fallback=" in line and "fallback=none" not in line) or "cpu_fallback" in line or
                "OCR_BACKEND_UNAVAILABLE" in line or "out of memory" in line.lower()):
            findings.append(line.strip()[:300])
    return findings[:20]


def crop_labels(index_path: Path, truth: Dict[str, List[Dict[str, Any]]]) -> Dict[str, Dict[str, str]]:
    """Ground truth per crop file. A clip with one label and no time window shows one plate; a
    clip with time windows labels only the crops inside a window; other clips stay unlabelled."""
    labels: Dict[str, Dict[str, str]] = {}
    with index_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            video = row["video"]
            entries = truth.get(video) or truth.get(Path(video).name) or []
            stream_ms = int(float(row.get("stream_ms") or 0))
            windowed = [entry for entry in entries
                        if entry["start_ms"] is not None or entry["stop_ms"] is not None]
            match = None
            if windowed:
                for entry in windowed:
                    if ((entry["start_ms"] is None or stream_ms >= entry["start_ms"]) and
                            (entry["stop_ms"] is None or stream_ms <= entry["stop_ms"])):
                        match = entry
                        break
            elif len(entries) == 1:
                match = entries[0]
            if match is not None:
                labels[row["file"]] = {"expected": match["expected"],
                                       "region": match["plate_region"]}
    return labels


def crop_accuracy(results: List[Dict[str, Any]], labels: Dict[str, Dict[str, str]]) -> Dict[str, Any]:
    def score(items: List[Tuple[Dict[str, Any], Dict[str, str]]]) -> Dict[str, Any]:
        chars = sum(len(label["expected"]) for _, label in items)
        errors = sum(levenshtein(label["expected"], normalize_plate(result["text"]))
                     for result, label in items)
        exact = sum(normalize_plate(result["text"]) == label["expected"] for result, label in items)
        accepted_exact = sum(result["rejection"] == "none" and
                             normalize_plate(result["text"]) == label["expected"]
                             for result, label in items)
        return {
            "crops": len(items),
            "exact": exact,
            "exact_accuracy": exact / len(items),
            "accepted_exact": accepted_exact,
            "accepted_exact_accuracy": accepted_exact / len(items),
            "cer": errors / max(1, chars),
            "character_accuracy": max(0.0, 1.0 - errors / max(1, chars)),
        }

    labelled = [(result, labels[result["file"]]) for result in results if result["file"] in labels]
    if not labelled:
        return {"available": False, "reason": "no crop comes from a labelled clip"}
    by_region = {}
    for region in sorted({label["region"] for _, label in labelled}):
        by_region[region] = score([item for item in labelled if item[1]["region"] == region])
    return {"available": True, **score(labelled), "by_region": by_region}


def pipeline_accuracy(report: Dict[str, Any], video: str,
                      truth: Dict[str, List[Dict[str, Any]]]) -> Dict[str, Any]:
    entries = truth.get(video) or truth.get(Path(video).name) or []
    if len(entries) != 1:
        return {"available": False,
                "reason": "clip is unlabelled" if not entries else "clip has several labels"}
    expected = entries[0]["expected"]
    events = report.get("events", [])
    accepted = [event for event in events
                if event.get("status") in ("VALID_HIGH_CONFIDENCE", "VALID_LOW_CONFIDENCE")]
    candidates = [event for event in events if event.get("normalized_plate")]
    predicted = normalize_plate(accepted[0].get("normalized_plate", "")) if accepted else ""
    candidate = normalize_plate(candidates[0].get("normalized_plate", "")) if candidates else ""
    return {"available": True, "expected": expected, "region": entries[0]["plate_region"],
            "accepted": predicted, "exact": predicted == expected,
            "candidate": candidate, "candidate_exact": candidate == expected}


def merge_resources(runs: List[Dict[str, Any]], key: str) -> Dict[str, Optional[float]]:
    """Sample-weighted average and overall peak of one resource series across runs."""
    total, weight, peak = 0.0, 0, None
    for run in runs:
        resources = run.get("resources") or {}
        series = resources.get(key)
        if not series:
            continue
        samples = max(1, int(resources.get("samples", 1)))
        total += series["avg"] * samples
        weight += samples
        peak = series["peak"] if peak is None else max(peak, series["peak"])
    return {"avg": total / weight if weight else None, "peak": peak}


def ocr_benchmark(args: argparse.Namespace, run_root: Path,
                  truth: Dict[str, List[Dict[str, Any]]]) -> Dict[str, Any]:
    """Every OCR engine on the same crops and the same clips, one engine at a time."""
    total_started = time.monotonic()
    videos = args.research_video or list(RESEARCH_VIDEOS)
    common = ["--config", str(args.config), "--warmup-frames", str(max(1, args.warmup_frames)),
              "--json"]
    if args.backend:
        common.extend(["--backend", args.backend])
    frame_limit = ["--max-frames", str(args.max_frames)] if args.max_frames else []

    crops_root = run_root / "crops"
    extraction = []
    for video in videos:
        print("[ocr-benchmark] extracting plate crops from {0}".format(video), flush=True)
        extraction.append(run_measured(
            [str(args.binary), "--video", video, "--extract-crops", str(crops_root)] + common +
            frame_limit, "crop extraction for " + video))
    index_path = crops_root / "index.csv"
    if not index_path.is_file():
        raise RuntimeError("the detector produced no plate crops; nothing to compare")
    labels = crop_labels(index_path, truth)

    selected = args.ocr_engine or [engine for engine, _ in OCR_BENCHMARK_ENGINES]
    names = dict(OCR_BENCHMARK_ENGINES)
    rows = []
    for position, engine in enumerate(selected):
        if position:
            # Let the previous engine's memory return to the system before the next one starts.
            time.sleep(args.cooldown)
        row: Dict[str, Any] = {"engine": engine, "label": names.get(engine, engine)}
        engine_started = time.monotonic()
        print("[ocr-benchmark] {0}: OCR on {1} identical crops".format(
            row["label"], sum(item["crops"] for item in extraction)), flush=True)
        try:
            crops = run_measured([str(args.binary), "--ocr-backend", engine,
                                  "--ocr-crops", str(index_path)] + common,
                                 row["label"] + " crop OCR")
        except RuntimeError as exc:
            row.update(status="unavailable", reason=str(exc),
                       wall_seconds=time.monotonic() - engine_started)
            rows.append(row)
            continue
        crops["accuracy"] = crop_accuracy(crops.pop("results"), labels)
        row["crops"] = crops
        pipelines = []
        for video in videos:
            print("[ocr-benchmark] {0}: full pipeline on {1}, every frame".format(
                row["label"], video), flush=True)
            try:
                run = run_measured([str(args.binary), "--video", video, "--ocr-backend", engine] +
                                   common + frame_limit, row["label"] + " pipeline on " + video)
                run["accuracy"] = pipeline_accuracy(run, video, truth)
                run["status"] = "ok"
            except RuntimeError as exc:
                run = {"status": "failed", "video": video, "reason": str(exc)}
            pipelines.append(run)
        row["pipelines"] = pipelines
        row["status"] = "ok" if all(run["status"] == "ok" for run in pipelines) else "partial"
        row["wall_seconds"] = time.monotonic() - engine_started
        rows.append(row)

    return {
        "mode": "ocr_benchmark",
        "metadata": platform_metadata(),
        "videos": videos,
        "extraction": extraction,
        "labelled_crops": len(labels),
        "engines": rows,
        "total_seconds": time.monotonic() - total_started,
    }


def ocr_engine_summary(row: Dict[str, Any]) -> Dict[str, Any]:
    """Flat per-engine figures for the report and the ranking."""
    crops = row["crops"]
    accuracy = crops["accuracy"]
    pipelines = [run for run in row["pipelines"] if run["status"] == "ok"]
    frames = sum(run["frames"] for run in pipelines)
    pipeline_seconds = sum(run["seconds"] for run in pipelines)
    runs = [crops] + pipelines
    kz = (accuracy.get("by_region") or {}).get("kz") if accuracy.get("available") else None
    return {
        "device": crops["backend"],
        "model": crops["model"],
        "crops": crops["crops"],
        "accepted": crops["accepted"],
        "empty": crops["empty"],
        "rejected": crops["crops"] - crops["accepted"],
        "exact": accuracy.get("exact_accuracy") if accuracy.get("available") else None,
        "cer": accuracy.get("cer") if accuracy.get("available") else None,
        "kz_exact": kz["exact_accuracy"] if kz else None,
        "kz_cer": kz["cer"] if kz else None,
        "ocr_fps": crops["ocr_fps"],
        "ocr_avg_ms": crops["latency"]["ocr"]["avg_ms"],
        "ocr_p50_ms": crops["latency"]["ocr"]["p50_ms"],
        "ocr_p95_ms": crops["latency"]["ocr"]["p95_ms"],
        "startup_s": (crops["load_ms"] + crops["warmup_ms"]) / 1000.0,
        "frames": frames,
        "pipeline_fps": frames / pipeline_seconds if pipeline_seconds > 0 else None,
        "detections": sum(run.get("metrics", {}).get("detections", 0) for run in pipelines),
        "pipeline_ocr_calls": sum(run.get("ocr_calls", 0) for run in pipelines),
        "plates_confirmed": sum(run.get("plates_confirmed", 0) for run in pipelines),
        "pipeline_exact": [run["accuracy"] for run in pipelines
                           if run.get("accuracy", {}).get("available")],
        "rss_mb": merge_resources(runs, "rss_mb"),
        "system_ram_mb": merge_resources(runs, "system_ram_used_mb"),
        "gpu_memory_mb": merge_resources(runs, "gpu_memory_mb"),
        "wall_seconds": row["wall_seconds"],
        "findings": [line for run in runs for line in run.get("log_findings", [])],
        "failed_pipelines": [run for run in row["pipelines"] if run["status"] != "ok"],
    }


def write_ocr_benchmark_markdown(path: Path, report: Dict[str, Any]) -> None:
    def fmt(value: Any, digits: int = 1, suffix: str = "") -> str:
        return "n/a" if value is None else "{0:.{1}f}{2}".format(float(value), digits, suffix)

    def pct(value: Any) -> str:
        return "n/a" if value is None else "{0:.1f}%".format(100.0 * float(value))

    summaries = {row["engine"]: ocr_engine_summary(row)
                 for row in report["engines"] if row["status"] in ("ok", "partial")}
    crops = sum(item["crops"] for item in report["extraction"])
    lines = [
        "# Sequential OCR Benchmark",
        "",
        "Timestamp: `{0}`; git `{1}`".format(report["metadata"]["timestamp"],
                                             (report["metadata"].get("git_commit") or "?")[:12]),
        "",
        "Engines ran one at a time, each in its own process. All used the same TensorRT detector "
        "settings, clips and configuration. Accuracy, OCR FPS and latency come from the same "
        "{0} plate crops (detector on every frame, then the pipeline's ROI, size and quality "
        "gates); {1} of them come from labelled clips. Pipeline FPS comes from full runs over "
        "every frame of {2}. Model loading and warm-up are excluded from FPS and latency and "
        "reported as startup.".format(crops, report["labelled_crops"],
                                       ", ".join(Path(v).name for v in report["videos"])),
        "",
        "| OCR | Device | Accuracy (exact) | KZ exact | CER | OCR FPS | Pipeline FPS | Avg latency | Peak RAM | Startup | Total time |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in report["engines"]:
        summary = summaries.get(row["engine"])
        if summary is None:
            reason = str(row.get("reason", "unavailable")).splitlines()
            lines.append("| {0} | unavailable | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | {1} |".format(
                row["label"], fmt(row.get("wall_seconds"), 0, " s")))
            continue
        lines.append("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |".format(
            row["label"], summary["device"], pct(summary["exact"]), pct(summary["kz_exact"]),
            fmt(summary["cer"], 3), fmt(summary["ocr_fps"]), fmt(summary["pipeline_fps"]),
            fmt(summary["ocr_avg_ms"], 1, " ms"), fmt(summary["rss_mb"]["peak"], 0, " MB"),
            fmt(summary["startup_s"], 1, " s"), fmt(summary["wall_seconds"], 0, " s")))

    lines.extend([
        "",
        "## Details",
        "",
        "| OCR | Model | Frames | Crops | Recognized | Empty | P50 | P95 | Avg RAM | System RAM peak | Plates confirmed | Pipeline exact |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ])
    for row in report["engines"]:
        summary = summaries.get(row["engine"])
        if summary is None:
            continue
        pipeline_exact = ", ".join(
            "{0} {1}".format(item["expected"], "yes" if item["exact"] else
                             "no (" + (item["accepted"] or item["candidate"] or "none") + ")")
            for item in summary["pipeline_exact"]) or "n/a"
        lines.append("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} |".format(
            row["label"], summary["model"], summary["frames"], summary["crops"],
            summary["accepted"], summary["empty"], fmt(summary["ocr_p50_ms"], 1, " ms"),
            fmt(summary["ocr_p95_ms"], 1, " ms"), fmt(summary["rss_mb"]["avg"], 0, " MB"),
            fmt(summary["system_ram_mb"]["peak"], 0, " MB"), summary["plates_confirmed"],
            pipeline_exact))

    lines.extend(["", "## Ranking", ""])
    ranked = list(summaries.items())
    names = {row["engine"]: row["label"] for row in report["engines"]}

    def best(key, reverse, label, unit_format):
        candidates = [(engine, summary) for engine, summary in ranked if summary[key] is not None]
        if not candidates:
            return "{0}: not measurable on this run.".format(label)
        candidates.sort(key=lambda item: item[1][key], reverse=reverse)
        engine, summary = candidates[0]
        return "{0}: **{1}** ({2}).".format(label, names[engine], unit_format(summary[key]))

    lines.append("1. " + best("exact", True, "Best accuracy", pct))
    lines.append("2. " + best("ocr_fps", True, "Best OCR FPS", lambda v: fmt(v) + " crops/s"))
    lines.append("3. " + best("ocr_avg_ms", False, "Lowest latency", lambda v: fmt(v, 1, " ms")))
    peak_ram = [(engine, summary) for engine, summary in ranked if summary["rss_mb"]["peak"] is not None]
    if peak_ram:
        engine, summary = min(peak_ram, key=lambda item: item[1]["rss_mb"]["peak"])
        lines.append("4. Lowest RAM: **{0}** (peak {1}).".format(
            names[engine], fmt(summary["rss_mb"]["peak"], 0, " MB")))
    else:
        lines.append("4. Lowest RAM: not measurable on this run.")
    kz = [(engine, summary) for engine, summary in ranked if summary["kz_exact"] is not None]
    if kz:
        kz.sort(key=lambda item: (-item[1]["kz_exact"], item[1]["kz_cer"], item[1]["ocr_p95_ms"],
                                  item[1]["rss_mb"]["peak"] or 0.0))
        engine, summary = kz[0]
        if summary["kz_exact"] > 0.0:
            lines.append(
                "5. Best overall for Kazakhstan plates on this device: **{0}** — ranked by KZ "
                "exact accuracy ({1}), then KZ CER ({2}), then OCR p95 ({3}), then peak "
                "RAM.".format(names[engine], pct(summary["kz_exact"]), fmt(summary["kz_cer"], 3),
                              fmt(summary["ocr_p95_ms"], 1, " ms")))
        else:
            lines.append(
                "5. Best overall for Kazakhstan plates: **none** — no engine read a labelled KZ "
                "crop exactly. Closest by character error rate: {0} (KZ CER {1}).".format(
                    names[engine], fmt(summary["kz_cer"], 3)))
    else:
        lines.append("5. Best overall for Kazakhstan plates: cannot be decided — no engine "
                     "produced readings on labelled KZ crops.")

    lines.extend(["", "Total wall-clock time for the whole benchmark: **{0}** ({1:.0f} s).".format(
        str(dt.timedelta(seconds=int(report["total_seconds"]))), report["total_seconds"]), ""])

    lines.extend(["## Unavailable engines, failures and fallbacks", ""])
    noted = False
    for row in report["engines"]:
        if row["status"] == "unavailable":
            lines.append("- {0}: {1}".format(row["label"], " ".join(
                str(row.get("reason", "")).split())[:400]))
            noted = True
            continue
        summary = summaries[row["engine"]]
        for run in summary["failed_pipelines"]:
            lines.append("- {0}: pipeline on {1} failed: {2}".format(
                row["label"], run["video"], " ".join(str(run["reason"]).split())[:300]))
            noted = True
        for finding in summary["findings"][:5]:
            lines.append("- {0}: `{1}`".format(row["label"], finding))
            noted = True
    if not noted:
        lines.append("- None: every engine loaded and ran without a fallback or crash.")

    lines.extend([
        "",
        "## Notes",
        "",
        "- Crop accuracy assumes each single-label clip shows only its labelled plate "
        "(`data/manifests/video_research.csv`); unlabelled clips count for speed only. Two "
        "labelled clips are a smoke test, not a production validation set.",
        "- `Recognized` counts crops whose reading passed the OCR confidence thresholds.",
        "- RAM is the peak resident memory of the benchmark process and its OCR worker. On the "
        "Jetson, CPU and GPU share memory: `System RAM peak` (tegrastats) includes GPU "
        "allocations; there is no separate GPU memory counter.",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / "kz_anpr_benchmark")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--video", default="video/car.mp4")
    parser.add_argument("--source", action="append", default=[])
    parser.add_argument("--streams", type=int, choices=(1, 2, 4), default=1)
    parser.add_argument("--matrix", action="store_true", help="run 1, 2 and 4 streams")
    parser.add_argument(
        "--research", action="store_true",
        help="run all four OCR backends on all three bundled videos with 1 and 4 streams",
    )
    parser.add_argument("--ocr-backend", choices=OCR_BACKENDS + ("easyocr_onnx", "nomeroff_onnx"),
                        default="")
    parser.add_argument(
        "--ocr-benchmark", action="store_true",
        help="sequential OCR comparison: every engine on the same crops and clips, one at a time",
    )
    parser.add_argument("--ocr-engine", action="append",
                        choices=[engine for engine, _ in OCR_BENCHMARK_ENGINES],
                        help="limit --ocr-benchmark to these engines; repeat for several")
    parser.add_argument("--cooldown", type=float, default=5.0,
                        help="seconds to wait between OCR engines in --ocr-benchmark")
    parser.add_argument("--research-video", action="append", default=[],
                        help="override the research video suite; repeat for multiple clips")
    parser.add_argument("--research-streams", action="append", type=int, choices=(1, 4),
                        help="research stream count; repeat to select both (default: 1 and 4)")
    parser.add_argument("--warmup-frames", type=int, default=3)
    parser.add_argument("--max-frames", type=int, default=0)
    parser.add_argument("--backend", default="")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "benchmark_results")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.config is None:
        args.config = ROOT / "config" / (
            "research.yaml" if args.research or args.ocr_benchmark else "default.yaml")
    if args.research and args.matrix:
        print("--research already runs 1 and 4 streams; do not combine it with --matrix",
              file=sys.stderr)
        return 2
    if args.research_streams and not args.research:
        print("--research-streams requires --research", file=sys.stderr)
        return 2
    if not args.binary.is_file():
        print(f"benchmark binary not found: {args.binary}; build the project first", file=sys.stderr)
        return 2
    stream_counts = [1, 2, 4] if args.matrix else [args.streams]
    timestamp = dt.datetime.now().astimezone().strftime("%Y%m%d_%H%M%S")
    run_root = args.output_dir / f"run_{timestamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    manifest = args.manifest
    if args.research and manifest is None:
        manifest = ROOT / "data" / "manifests" / "video_research.csv"
    if args.ocr_benchmark and manifest is None:
        manifest = ROOT / "data" / "manifests" / "video_research.csv"
    truth = load_ground_truth(manifest)
    if args.ocr_benchmark:
        try:
            report = ocr_benchmark(args, run_root, truth)
        except RuntimeError as exc:
            print(str(exc), file=sys.stderr)
            return 1
        json_path = args.output_dir / f"ocr_benchmark_{timestamp}.json"
        markdown_path = args.output_dir / f"ocr_benchmark_{timestamp}.md"
        json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        write_ocr_benchmark_markdown(markdown_path, report)
        print(markdown_path.read_text(encoding="utf-8"))
        print(f"json={json_path}")
        print(f"markdown={markdown_path}")
        return 0
    if args.research:
        runs: List[Dict[str, Any]] = []
        videos = args.research_video or list(RESEARCH_VIDEOS)
        research_streams = list(dict.fromkeys(args.research_streams or (1, 4)))
        for ocr_backend in OCR_BACKENDS:
            for video in videos:
                for stream_count in research_streams:
                    case_args = copy.copy(args)
                    case_args.ocr_backend = ocr_backend
                    case_args.video = video
                    case_args.source = []
                    case_root = run_root / ocr_backend / Path(video).stem
                    try:
                        run = run_once(case_args, stream_count, case_root, truth)
                        run["status"] = "ok"
                    except (RuntimeError, ValueError) as exc:
                        run = {
                            "status": "unavailable",
                            "reason": str(exc),
                            "ocr_backend_requested": ocr_backend,
                            "video": video,
                            "streams": stream_count,
                        }
                    runs.append(run)
        report = {"mode": "four_ocr_research", "metadata": platform_metadata(), "runs": runs}
        json_path = args.output_dir / f"research_{timestamp}.json"
        markdown_path = args.output_dir / f"research_{timestamp}.md"
        json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        write_research_markdown(markdown_path, report)
        print(markdown_path.read_text(encoding="utf-8"))
        print(f"json={json_path}")
        print(f"markdown={markdown_path}")
        return 0
    try:
        runs = [run_once(args, count, run_root, truth) for count in stream_counts]
    except (RuntimeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    report = {"metadata": platform_metadata(), "runs": runs}
    json_path = args.output_dir / f"run_{timestamp}.json"
    markdown_path = args.output_dir / f"run_{timestamp}.md"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_markdown(markdown_path, report)
    print(markdown_path.read_text(encoding="utf-8"))
    print(f"json={json_path}")
    print(f"markdown={markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
