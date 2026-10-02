from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
WORKER_PATH = ROOT / "tools" / "nomeroff_worker.py"
SPEC = importlib.util.spec_from_file_location("nomeroff_worker", WORKER_PATH)
assert SPEC is not None and SPEC.loader is not None
worker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = worker
SPEC.loader.exec_module(worker)


class NomeroffWorkerTests(unittest.TestCase):
    def test_mock_batch_preserves_order_and_raw_text(self) -> None:
        recognizer = worker.MockRecognizer("kz")
        dark = np.zeros((24, 90, 3), dtype=np.uint8)
        bright = np.full((24, 90, 3), 255, dtype=np.uint8)
        results = recognizer.recognize_batch([dark, bright])
        self.assertEqual([result.text for result in results], ["123ABC02", "123ABC02"])
        self.assertLess(results[0].confidence, results[1].confidence)
        self.assertEqual(results[0].region, "kz")

    def test_empty_crop_is_not_fabricated(self) -> None:
        result = worker.MockRecognizer("kz").recognize_batch(
            [np.empty((0, 0, 3), dtype=np.uint8)]
        )[0]
        self.assertEqual(result.text, "")
        self.assertEqual(result.confidence, 0.0)

    def test_ctc_confidence_ignores_blank_and_collapses_repeats(self) -> None:
        import torch

        # Tokens by timestep: blank, A, A, blank, B. Class zero is CTC blank.
        logits = torch.tensor(
            [
                [[8.0, 0.0, 0.0]],
                [[0.0, 5.0, 0.0]],
                [[0.0, 7.0, 0.0]],
                [[8.0, 0.0, 0.0]],
                [[0.0, 0.0, 6.0]],
            ]
        )
        confidences = worker.ctc_character_confidences(logits)[0]
        self.assertEqual(len(confidences), 2)
        self.assertGreater(confidences[0], 0.99)
        self.assertGreater(confidences[1], 0.99)

    def test_binary_protocol_recognizes_one_crop_and_stops(self) -> None:
        process = subprocess.Popen(
            [sys.executable, str(WORKER_PATH), "--serve", "--mock", "--region", "kz"],
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert process.stdin is not None and process.stdout is not None
        ready = process.stdout.readline().decode().rstrip("\n")
        self.assertTrue(ready.startswith("READY\t4.0.1\t"), ready)

        crop = np.full((25, 100, 3), 128, dtype=np.uint8)
        process.stdin.write(f"OCR\t7\t25\t100\t3\t{crop.nbytes}\t0\n".encode("ascii"))
        process.stdin.write(crop.tobytes())
        process.stdin.flush()
        response = process.stdout.readline().decode().rstrip("\n").split("\t")
        self.assertEqual(response[:3], ["RESULT", "7", "123ABC02"])
        self.assertEqual(response[5], "kz")

        process.stdin.write(b"STOP\n")
        process.stdin.flush()
        self.assertEqual(process.stdout.readline().decode().strip(), "STOPPED")
        self.assertEqual(process.wait(timeout=5), 0)
        process.stdin.close()
        process.stdout.close()
        assert process.stderr is not None
        process.stderr.close()

    def test_binary_protocol_rejects_corrupt_dimensions(self) -> None:
        process = subprocess.Popen(
            [sys.executable, str(WORKER_PATH), "--serve", "--mock"],
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert process.stdin is not None and process.stdout is not None
        process.stdout.readline()
        process.stdin.write(b"OCR\t8\t10\t10\t3\t1\t0\n")
        process.stdin.flush()
        response = process.stdout.readline().decode()
        self.assertTrue(response.startswith("ERROR\t8\tOCR_ERROR\tINVALID_CROP"), response)
        process.stdin.write(b"STOP\n")
        process.stdin.flush()
        process.wait(timeout=5)
        process.stdin.close()
        process.stdout.close()
        assert process.stderr is not None
        process.stderr.close()


if __name__ == "__main__":
    unittest.main()
