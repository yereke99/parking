#!/usr/bin/env python3
"""Reproducible 1/2/4-stream ANPR benchmark and telemetry collector."""

from __future__ import annotations

import argparse
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
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def command_output(command: list[str]) -> str | None:
    try:
        result = subprocess.run(command, text=True, capture_output=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = (result.stdout + result.stderr).strip()
    return output or None


def descendants(root_pid: int) -> set[int]:
    output = command_output(["ps", "-axo", "ppid=,pid="])
    if not output:
        return {root_pid}
    children: dict[int, list[int]] = {}
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


def process_usage(root_pid: int) -> tuple[float, float]:
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


def nvidia_sample() -> dict[str, float] | None:
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
        self.samples: list[dict[str, float]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._tegrastats: subprocess.Popen[str] | None = None
        self._tegrastats_thread: threading.Thread | None = None
        self._tegrastats_lines: list[str] = []

    def start(self) -> None:
        tegrastats = shutil.which("tegrastats")
        if tegrastats:
            self._tegrastats = subprocess.Popen(
                [tegrastats, "--interval", "500"], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True,
            )
            self._tegrastats_thread = threading.Thread(target=self._read_tegrastats, daemon=True)
            self._tegrastats_thread.start()
        self._thread.start()

    def stop(self) -> dict[str, Any]:
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

        def summary(key: str) -> dict[str, float] | None:
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
            sample: dict[str, float] = {}
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


def load_ground_truth(path: Path | None) -> dict[str, list[dict[str, Any]]]:
    if path is None:
        return {}
    truth: dict[str, list[dict[str, Any]]] = {}

    def optional_int(value: str | None) -> int | None:
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


def accuracy_report(run: dict[str, Any], sources: list[str],
                    truth: dict[str, list[dict[str, Any]]],
                    failure_root: Path) -> dict[str, Any]:
    cases: list[dict[str, Any]] = []
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
    exact = sum(case["exact"] for case in cases)
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
    by_region: dict[str, dict[str, float | int]] = {}
    for region in sorted({case["plate_region"] for case in cases}):
        regional = [case for case in cases if case["plate_region"] == region]
        regional_chars = sum(len(case["expected"]) for case in regional)
        regional_errors = sum(case["edit_distance"] for case in regional)
        regional_exact = sum(case["exact"] for case in regional)
        by_region[region] = {
            "cases": len(regional),
            "exact_plate_accuracy": regional_exact / len(regional),
            "character_accuracy": max(0.0, 1.0 - regional_errors / max(1, regional_chars)),
            "cer": regional_errors / max(1, regional_chars),
        }
    return {
        "available": True,
        "cases": len(cases),
        "exact_plate_accuracy": exact / len(cases),
        "character_accuracy": max(0.0, 1.0 - total_errors / max(1, total_chars)),
        "cer": total_errors / max(1, total_chars),
        "failures": len(cases) - exact,
        "failure_modes": failure_modes,
        "by_region": by_region,
        "details": cases,
    }


def ocr_environment_metadata() -> dict[str, Any] | None:
    python = ROOT / ".venv-nomeroff" / "bin" / "python"
    if not python.is_file():
        return None
    script = (
        "import json,platform,nomeroff_net,torch;"
        "print(json.dumps({'python':platform.python_version(),"
        "'nomeroff':getattr(nomeroff_net,'__version__','unknown'),"
        "'torch':torch.__version__,'cuda':torch.version.cuda,"
        "'cuda_available':torch.cuda.is_available()}))"
    )
    output = command_output([str(python), "-c", script])
    if not output:
        return None
    try:
        return json.loads(output.splitlines()[-1])
    except json.JSONDecodeError:
        return {"error": output}


def platform_metadata() -> dict[str, Any]:
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
        "power_mode": command_output(["nvpmodel", "-q"]) if shutil.which("nvpmodel") else None,
        "jetson_clocks": command_output(["jetson_clocks", "--show"])
        if shutil.which("jetson_clocks") else None,
    }


def parse_json_output(stdout: str) -> dict[str, Any]:
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise RuntimeError("benchmark binary did not emit a JSON report")


def run_once(args: argparse.Namespace, stream_count: int, run_root: Path,
             truth: dict[str, str]) -> dict[str, Any]:
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
    if args.source:
        for source in args.source:
            command.extend(["--source", source])
    else:
        command.extend(["--video", args.video])

    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True)
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
    return report


def write_markdown(path: Path, report: dict[str, Any]) -> None:
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / "kz_anpr_benchmark")
    parser.add_argument("--config", type=Path, default=ROOT / "config" / "default.yaml")
    parser.add_argument("--video", default="video/car.mp4")
    parser.add_argument("--source", action="append", default=[])
    parser.add_argument("--streams", type=int, choices=(1, 2, 4), default=1)
    parser.add_argument("--matrix", action="store_true", help="run 1, 2 and 4 streams")
    parser.add_argument("--warmup-frames", type=int, default=3)
    parser.add_argument("--max-frames", type=int, default=0)
    parser.add_argument("--backend", default="")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "benchmark_results")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.binary.is_file():
        print(f"benchmark binary not found: {args.binary}; build the project first", file=sys.stderr)
        return 2
    stream_counts = [1, 2, 4] if args.matrix else [args.streams]
    timestamp = dt.datetime.now().astimezone().strftime("%Y%m%d_%H%M%S")
    run_root = args.output_dir / f"run_{timestamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    truth = load_ground_truth(args.manifest)
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
