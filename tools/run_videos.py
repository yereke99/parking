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
import shutil
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


def run_clip(command, source, photos_dir, native, events_out, log_out):
    """Save directly into the results whenever the recognizer can write there.

    External Docker --output-dir locations use staging in the existing writable mount;
    completed photos are kept even when the run is interrupted.
    """
    def recognize(target):
        try:
            return subprocess.run(
                command + ["--source", str(source), "--camera-id", source.stem,
                           "--snapshots-dir", target],
                cwd=str(PROJECT), stdout=events_out, stderr=log_out).returncode
        except OSError as error:
            log_out.write(str(error) + "\n")
            return 1

    if native:
        return recognize(str(photos_dir)), []
    staging_root = PROJECT / "var"
    staging_root.mkdir(parents=True, exist_ok=True)
    try:
        relative = Path(os.path.realpath(str(photos_dir))).relative_to(
            Path(os.path.realpath(str(staging_root))))
    except ValueError:
        pass
    else:
        return recognize("/workspace/var/" + relative.as_posix()), []

    snapshot_errors = []
    with tempfile.TemporaryDirectory(prefix=".snapshots-", dir=str(staging_root)) as temporary:
        staging = Path(temporary)
        target = "/workspace/" + staging.relative_to(PROJECT).as_posix()
        try:
            returncode = recognize(target)
        finally:
            for image in sorted(staging.glob("*.jpg")):
                try:
                    photos_dir.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(image), str(photos_dir / image.name))
                except OSError as error:
                    snapshot_errors.append("%s: %s" % (image.name, error))
    return returncode, snapshot_errors


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
    parser.add_argument("--output-dir", help="new directory for photos, events, logs and summary.json")
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
        photos_dir = output / name / "photos"
        print("[%d/%d] %s" % (index, len(clips), source.name), flush=True)
        with events_path.open("w", encoding="utf-8") as events_out, \
                log_path.open("w", encoding="utf-8") as log_out:
            # Blocking run: a clip finishes before the next process/container starts.
            returncode, snapshot_errors = run_clip(
                command, source, photos_dir, args.native, events_out, log_out)

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
        snapshots = []
        if not parse_error:
            for event in events:
                if event.get("status") not in ACCEPTED:
                    continue
                snapshot_path = event.get("snapshot_path")
                image = photos_dir / Path(snapshot_path).name if snapshot_path else None
                if image is not None and image.is_file():
                    # Paths in the saved report are relative to this run, on the host; they
                    # never point at a temporary directory or a container-only /workspace.
                    event["snapshot_path"] = image.relative_to(output).as_posix()
                    snapshots.append({
                        "plate": event.get("normalized_plate"),
                        "timestamp_ms": event.get("snapshot_timestamp_ms"),
                        "image": event["snapshot_path"],
                    })
                else:
                    snapshot_errors.append("no photo saved for %s" % event.get("normalized_plate"))
                    event.pop("snapshot_path", None)
                    event.pop("snapshot_timestamp_ms", None)
            events_path.write_text(
                "".join(json.dumps(event, ensure_ascii=False) + "\n" for event in events),
                encoding="utf-8")
        status = ("ERROR" if returncode != 0 or parse_error or snapshot_errors else
                  "RECOGNIZED" if plates else "NO_CONFIRMED_PLATES")
        result = {
            "video": str(source), "status": status, "exit_code": returncode,
            "recognized_plates": plates,
            "event_statuses": dict(Counter(event.get("status", "UNKNOWN") for event in events)),
            "events_file": events_path.name, "log_file": log_path.name,
            "photos_dir": photos_dir.relative_to(output).as_posix(),
            "snapshots": snapshots,
        }
        if parse_error:
            result["output_error"] = parse_error
        if snapshot_errors:
            result["snapshot_errors"] = snapshot_errors
        results.append(result)
        # Save after every clip so a later failure does not erase earlier results.
        (output / "summary.json").write_text(
            json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print("  %s | plates: %s | events: %d" %
              (status, ", ".join(plates) or "-", len(events)), flush=True)
        if snapshots:
            print("  Photos: %s (%d)" % (photos_dir, len(snapshots)), flush=True)
        for error in snapshot_errors:
            print("  Photo error: %s" % error, flush=True)
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
