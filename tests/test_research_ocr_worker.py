from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
WORKER_PATH = ROOT / "tools" / "research_ocr_worker.py"
SPEC = importlib.util.spec_from_file_location("research_ocr_worker", WORKER_PATH)
assert SPEC is not None and SPEC.loader is not None
worker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = worker
SPEC.loader.exec_module(worker)


class ResearchOcrWorkerTests(unittest.TestCase):
    def test_normalization_maps_plate_cyrillic_and_removes_noise(self) -> None:
        self.assertEqual(worker.normalize_plate(" 152 ЈТА 02-"), "152JTA02")
        self.assertEqual(worker.normalize_plate("А123ВС77"), "A123BC77")

    def test_mock_protocol_for_both_engines(self) -> None:
        for engine in ("paddleocr", "easyocr"):
            with self.subTest(engine=engine):
                process = subprocess.Popen(
                    [sys.executable, str(WORKER_PATH), "--serve", "--mock",
                     "--engine", engine],
                    cwd=ROOT,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                assert process.stdin is not None and process.stdout is not None
                ready = process.stdout.readline().decode().rstrip("\n")
                self.assertTrue(ready.startswith(f"READY\t{engine}\ttest\t"), ready)
                crop = np.full((24, 96, 3), 128, dtype=np.uint8)
                process.stdin.write(f"OCR\t3\t24\t96\t3\t{crop.nbytes}\t0\n".encode())
                process.stdin.write(crop.tobytes())
                process.stdin.flush()
                response = process.stdout.readline().decode().rstrip("\n").split("\t")
                self.assertEqual(response[:3], ["RESULT", "3", "152JTA02"])
                process.stdin.write(b"STOP\n")
                process.stdin.flush()
                self.assertEqual(process.stdout.readline().decode().strip(), "STOPPED")
                self.assertEqual(process.wait(timeout=5), 0)
                process.stdin.close()
                process.stdout.close()
                assert process.stderr is not None
                process.stderr.close()

    def test_auto_device_falls_back_to_cpu_or_hands_over_accelerator(self) -> None:
        # accelerator: (expected device, falls back, accelerator timeout in seconds)
        cases = {
            "ok": ("cuda", False, "30"),
            "fail": ("cpu", True, "30"),
            "hang": ("cpu", True, "1"),
        }
        for accelerator, (device, fell_back, timeout_s) in cases.items():
            with self.subTest(accelerator=accelerator):
                process = subprocess.Popen(
                    [sys.executable, str(WORKER_PATH), "--serve", "--mock",
                     "--engine", "easyocr", "--device", "auto",
                     "--mock-accelerator", accelerator],
                    cwd=ROOT,
                    env={**os.environ, "KZ_ANPR_OCR_ACCELERATOR_TIMEOUT_S": timeout_s},
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                )
                assert process.stdin is not None and process.stdout is not None
                ready = process.stdout.readline().decode().rstrip("\n").split("\t")
                self.assertEqual(ready[:4], ["READY", "easyocr", "test", device])
                self.assertEqual(bool(ready[10]), fell_back, ready)
                crop = np.full((24, 96, 3), 128, dtype=np.uint8)
                process.stdin.write(f"OCR\t1\t24\t96\t3\t{crop.nbytes}\t0\n".encode())
                process.stdin.write(crop.tobytes())
                process.stdin.flush()
                response = process.stdout.readline().decode().split("\t")
                self.assertEqual(response[:3], ["RESULT", "1", "152JTA02"])
                process.stdin.write(b"STOP\n")
                process.stdin.flush()
                self.assertEqual(process.stdout.readline().decode().strip(), "STOPPED")
                self.assertEqual(process.wait(timeout=5), 0)
                # The supervising parent must not keep the protocol pipe open.
                self.assertEqual(process.stdout.read(), b"")
                process.stdin.close()
                process.stdout.close()


if __name__ == "__main__":
    unittest.main()
