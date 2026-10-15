"""Batch replay photo/report integration, using a fake recognizer and no extra packages."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "run_videos", Path(__file__).resolve().parents[1] / "tools" / "run_videos.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class VideoPhotosTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.project = self.root / "project"
        (self.project / "video").mkdir(parents=True)
        (self.project / "video" / "parking.mp4").write_bytes(b"fixture")
        self.output = self.root / "results"
        self.calls = []
        self.console = io.StringIO()

    def run_batch(self, native=True, accepted=True, save_photo=True, exit_code=0, interrupt=False,
                  event_updates=None, log_level=None):
        def recognize(command, cwd, stdout, stderr):
            self.calls.append(command)
            target = command[command.index("--snapshots-dir") + 1]
            if native:
                staging = Path(target)
            else:
                self.assertTrue(target.startswith("/workspace/var/"))
                staging = self.project / target[len("/workspace/"):]
            photo_name = "152JTA02_8400_t1.jpg"
            event = {"event": "plate_recognition", "normalized_plate": "152JTA02",
                     "status": "VALID_HIGH_CONFIDENCE" if accepted else "LOW_CONFIDENCE"}
            if accepted and save_photo:
                staging.mkdir(parents=True, exist_ok=True)
                (staging / photo_name).write_bytes(b"JPEG fixture")
                event.update(snapshot_path=target + "/" + photo_name, snapshot_timestamp_ms=8400)
            if event_updates:
                event.update(event_updates)
            stdout.write(json.dumps(event) + "\n")
            if interrupt:
                raise KeyboardInterrupt()
            return subprocess.CompletedProcess(command, exit_code)

        argv = ["run_videos.py", "--output-dir", str(self.output)]
        if native:
            argv.append("--native")
        if log_level:
            argv += ["--log-level", log_level]
        with mock.patch.object(runner, "PROJECT", self.project), \
                mock.patch.object(sys, "argv", argv), \
                mock.patch.object(runner.subprocess, "run", side_effect=recognize), \
                contextlib.redirect_stdout(self.console):
            code = runner.main()
        summary = json.loads((self.output / "summary.json").read_text())[0]
        event = json.loads((self.output / summary["events_file"]).read_text())
        self.assertEqual(list((self.project / "var").glob(".snapshots-*")), [])
        return code, summary, event

    def test_native_photo_and_reports_share_a_persistent_relative_path(self):
        code, summary, event = self.run_batch()
        self.assertEqual(code, 0)
        self.assertEqual(summary["status"], "RECOGNIZED")
        self.assertEqual(summary["recognized_plates"], ["152JTA02"])
        snapshot = summary["snapshots"][0]
        self.assertEqual(snapshot["timestamp_ms"], 8400)
        self.assertEqual(snapshot["image"], "01-parking/photos/152JTA02_8400_t1.jpg")
        self.assertEqual(event["snapshot_path"], snapshot["image"])
        self.assertEqual((self.output / snapshot["image"]).read_bytes(), b"JPEG fixture")

    def test_docker_uses_the_existing_writable_mount_with_external_output_dir(self):
        code, summary, event = self.run_batch(native=False)
        self.assertEqual(code, 0)
        self.assertTrue(self.calls[0][0].endswith("tools/jetson_docker.sh"))
        self.assertEqual(self.calls[0][1], "run")
        self.assertEqual(self.calls[0][self.calls[0].index("--source") + 1], "video/parking.mp4")
        self.assertTrue((self.output / event["snapshot_path"]).is_file())
        target = self.calls[0][self.calls[0].index("--snapshots-dir") + 1]
        self.assertTrue(target.startswith("/workspace/var/.snapshots-"))

    def test_docker_saves_directly_in_the_normal_results_directory(self):
        self.output = self.project / "var" / "video-runs" / "fixture"
        code, summary, event = self.run_batch(native=False)
        self.assertEqual(code, 0)
        target = self.calls[0][self.calls[0].index("--snapshots-dir") + 1]
        self.assertEqual(target, "/workspace/var/video-runs/fixture/01-parking/photos")
        self.assertTrue((self.output / event["snapshot_path"]).is_file())

    def test_interrupt_keeps_already_saved_native_photos(self):
        with self.assertRaises(KeyboardInterrupt):
            self.run_batch(interrupt=True)
        self.assertTrue((self.output / "01-parking/photos/152JTA02_8400_t1.jpg").is_file())

    def test_interrupt_keeps_staged_docker_photos(self):
        with self.assertRaises(KeyboardInterrupt):
            self.run_batch(native=False, interrupt=True)
        self.assertTrue((self.output / "01-parking/photos/152JTA02_8400_t1.jpg").is_file())
        self.assertEqual(list((self.project / "var").glob(".snapshots-*")), [])

    def test_unconfirmed_reading_creates_no_photo(self):
        code, summary, event = self.run_batch(accepted=False)
        self.assertEqual(code, 0)
        self.assertEqual(summary["status"], "NO_CONFIRMED_PLATES")
        self.assertEqual(summary["snapshots"], [])
        self.assertFalse((self.output / summary["photos_dir"]).exists())

    def test_unconfirmed_output_shows_evidence_without_accepting_the_candidate(self):
        code, summary, event = self.run_batch(accepted=False, event_updates={
            "confidence": 0.35, "observation_count": 6, "agreeing_observations": 1,
        })
        self.assertEqual(code, 0)
        self.assertEqual(summary["recognized_plates"], [])
        self.assertEqual(summary["snapshots"], [])
        self.assertIn("Event statuses: LOW_CONFIDENCE=1", self.console.getvalue())
        self.assertIn('Unconfirmed candidate: "152JTA02"', self.console.getvalue())
        self.assertIn("confidence=0.350, votes=1/6", self.console.getvalue())
        self.assertIn(str(self.output / summary["events_file"]), self.console.getvalue())
        self.assertIn(str(self.output / summary["log_file"]), self.console.getvalue())

    def test_no_text_output_points_to_existing_diagnostics_without_inventing_a_plate(self):
        code, summary, event = self.run_batch(accepted=False, event_updates={
            "status": "INVALID_FORMAT", "normalized_plate": "", "raw_plate": "",
        })
        self.assertEqual(code, 0)
        self.assertIn("Event statuses: INVALID_FORMAT=1", self.console.getvalue())
        self.assertNotIn("Unconfirmed candidate:", self.console.getvalue())
        self.assertEqual(summary["recognized_plates"], [])

    def test_debug_log_level_is_forwarded_to_the_existing_native_recognizer(self):
        code, summary, event = self.run_batch(log_level="debug")
        self.assertEqual(code, 0)
        command = self.calls[0]
        self.assertEqual(command[command.index("--log-level") + 1], "debug")

    def test_debug_log_level_is_forwarded_to_the_existing_docker_recognizer(self):
        code, summary, event = self.run_batch(native=False, log_level="debug")
        self.assertEqual(code, 0)
        command = self.calls[0]
        self.assertEqual(command[command.index("--log-level") + 1], "debug")

    def test_missing_photo_reports_failure_but_keeps_the_plate(self):
        code, summary, event = self.run_batch(save_photo=False)
        self.assertEqual(code, 1)
        self.assertEqual(summary["status"], "ERROR")
        self.assertEqual(summary["recognized_plates"], ["152JTA02"])
        self.assertEqual(summary["snapshot_errors"], ["no photo saved for 152JTA02"])
        self.assertEqual(event["normalized_plate"], "152JTA02")

    def test_model_failure_stops_the_batch_after_preserving_completed_results(self):
        (self.project / "video" / "second.mp4").write_bytes(b"fixture")
        code, summary, event = self.run_batch(exit_code=4)
        self.assertEqual(code, 1)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(summary["exit_code"], 4)
        self.assertTrue((self.output / event["snapshot_path"]).is_file())


if __name__ == "__main__":
    unittest.main()
