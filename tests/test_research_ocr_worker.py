from __future__ import annotations

import importlib.util
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


if __name__ == "__main__":
    unittest.main()
