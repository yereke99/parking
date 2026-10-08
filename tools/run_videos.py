#!/usr/bin/env python3
"""Replay every clip in video/ one after another with the existing ANPR executable.

Without options every video file found in video/ is replayed, however many there are: add or
remove clips and the next run follows. --input-dir scans another directory instead (for example
video/compatible from tools/prepare_iphone_videos.sh), --video picks clips by name, --list reads
an explicit clip list. No extra packages are needed.
"""

import argparse
from collections import Counter
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


PROJECT = Path(__file__).resolve().parent.parent
ACCEPTED = {"VALID_HIGH_CONFIDENCE", "VALID_LOW_CONFIDENCE"}
# Containers OpenCV/FFmpeg/GStreamer read; matched case-insensitively (iPhone clips are .MOV).
VIDEO_SUFFIXES = {".mp4", ".mov", ".m4v", ".mkv", ".avi", ".ts", ".webm", ".h264", ".h265", ".hevc"}


def find_videos(directory):
    """Every video file directly inside `directory`, sorted by name. Hidden files are skipped."""
    return sorted((path for path in directory.iterdir()
                   if path.is_file() and not path.name.startswith(".")
                   and path.suffix.lower() in VIDEO_SUFFIXES),
                  key=lambda path: path.name.lower())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", default="video",
                        help="directory whose video files are replayed (default: video)")
    parser.add_argument("--video", action="append", default=[], metavar="NAME",
                        help="replay only this clip (file name or path, repeatable)")
    parser.add_argument("--list", help="replay the clips named in this file, one per line, "
                                       "instead of scanning --input-dir")
    parser.add_argument("--list-videos", action="store_true",
                        help="print the clips that would be replayed and exit")
    parser.add_argument("--output-dir", help="new directory for events, logs and summary.json")
    parser.add_argument("--native", action="store_true", help="use a local binary instead of Jetson Docker")
    parser.add_argument("--binary", default="build/kz_anpr", help="local binary for --native")
    parser.add_argument("--config", help="override the existing Docker/native configuration")
    args = parser.parse_args()

    def project_path(value):
        path = Path(value)
        return path if path.is_absolute() else PROJECT / path

    input_dir = project_path(args.input_dir)
    if args.list:
        candidates = []
        for line in project_path(args.list).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                candidates.append(project_path(line))
    else:
        if not input_dir.is_dir():
            parser.error("video directory not found: %s" % input_dir)
        candidates = find_videos(input_dir)
    if args.video:
        wanted = []
        for name in args.video:
            # os.path.realpath, not Path.resolve: Python 3.6 (the Jetson host) raises on a
            # missing file there.
            target = os.path.realpath(str(project_path(name)))
            matches = [path for path in candidates
                       if path.name == Path(name).name or os.path.realpath(str(path)) == target]
            if not matches:
                # A clip outside the scanned set (another directory, a hidden file) by path.
                path = project_path(name)
                matches = [path] if path.is_file() else []
            if not matches:
                parser.error("video not found: %s (available: %s)" %
                             (name, ", ".join(path.name for path in candidates) or "none"))
            wanted.extend(path for path in matches if path not in wanted)
        candidates = wanted

    clips = []
    for source in candidates:
        if not source.is_file():
            parser.error("video file not found: %s" % source)
        # Docker mounts only the checkout, and relative paths work unchanged in /workspace.
        source = source.resolve()
        if not args.native:
            try:
                source = source.relative_to(PROJECT)
            except ValueError:
                parser.error("Docker video must be inside the checkout: %s" % source)
        clips.append(source)
    if not clips:
        parser.error("no video files found in %s" % (args.list or input_dir))
    if args.list_videos:
        for source in clips:
            print(source)
        return 0

    if args.native:
        command = [str(project_path(args.binary)), "--config", args.config or "config/default.yaml"]
    else:
        command = [str(PROJECT / "tools/jetson_docker.sh"), "run"]
        if args.config:
            command += ["--config", args.config]

    if args.output_dir:
        output = project_path(args.output_dir)
        output.mkdir(parents=True, exist_ok=False)
    else:
        root = PROJECT / "var/video-runs"
        root.mkdir(parents=True, exist_ok=True)
        prefix = datetime.utcnow().strftime("%Y%m%dT%H%M%SZ-")
        output = Path(tempfile.mkdtemp(prefix=prefix, dir=str(root)))

    results = []
    print("Results: %s" % output, flush=True)
    for index, source in enumerate(clips, 1):
        name = "%02d-%s" % (index, source.stem)
        events_path = output / (name + ".events.jsonl")
        log_path = output / (name + ".log")
        print("[%d/%d] %s" % (index, len(clips), source.name), flush=True)
        with events_path.open("w", encoding="utf-8") as events_out, \
                log_path.open("w", encoding="utf-8") as log_out:
            try:
                # Blocking run: a clip finishes before the next process/container starts.
                returncode = subprocess.run(
                    command + ["--source", str(source), "--camera-id", source.stem],
                    cwd=str(PROJECT), stdout=events_out, stderr=log_out).returncode
            except OSError as error:
                log_out.write(str(error) + "\n")
                returncode = 1

        events = []
        parse_error = None
        try:
            for line in events_path.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    event = json.loads(line)
                    if not isinstance(event, dict) or event.get("event") != "plate_recognition":
                        raise ValueError("unexpected event output")
                    events.append(event)
        except (ValueError, UnicodeError) as error:
            parse_error = str(error)

        plates = sorted({event["normalized_plate"] for event in events
                         if event.get("status") in ACCEPTED and event.get("normalized_plate")})
        status = ("ERROR" if returncode != 0 or parse_error else
                  "RECOGNIZED" if plates else "NO_CONFIRMED_PLATES")
        result = {
            "video": str(source), "status": status, "exit_code": returncode,
            "recognized_plates": plates,
            "event_statuses": dict(Counter(event.get("status", "UNKNOWN") for event in events)),
            "events_file": events_path.name, "log_file": log_path.name,
        }
        if parse_error:
            result["output_error"] = parse_error
        results.append(result)
        # Save after every clip so a later failure does not erase earlier results.
        (output / "summary.json").write_text(
            json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print("  %s | plates: %s | events: %d" %
              (status, ", ".join(plates) or "-", len(events)), flush=True)
        if status == "ERROR":
            print("  exit=%d; see %s" % (returncode, log_path), flush=True)
            # A missing model/backend or invalid config affects every clip equally.
            if returncode in (2, 4):
                break
        if returncode < 0 or returncode in (130, 143):
            break

    print("Summary: %s" % (output / "summary.json"), flush=True)
    return 1 if any(result["status"] == "ERROR" for result in results) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, UnicodeError) as error:
        print("run_videos: %s" % error, file=sys.stderr)
        sys.exit(2)
