from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("benchmark", ROOT / "tools" / "benchmark.py")
assert SPEC is not None and SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = benchmark
SPEC.loader.exec_module(benchmark)


def truth(*rows: dict) -> dict:
    result: dict = {}
    for row in rows:
        item = {"expected": row["expected"], "plate_region": row.get("region", "kz"),
                "event_id": "", "start_ms": row.get("start_ms"), "stop_ms": row.get("stop_ms")}
        result.setdefault(row["path"], []).append(item)
    return result


class OcrBenchmarkAccuracyTests(unittest.TestCase):
    def write_index(self, directory: Path, rows: list) -> Path:
        index = directory / "index.csv"
        lines = ["file,video,frame,stream_ms,x,y,width,height,detector_confidence,quality_score"]
        lines += ["{0},{1},1,{2},0,0,10,5,0.9,0.8".format(*row) for row in rows]
        index.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return index

    def test_single_label_clip_labels_every_crop_and_unlabelled_clip_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            index = self.write_index(Path(tmp), [
                ("kz/a.png", "video/parking.mp4", 0),
                ("kz/b.png", "video/parking.mp4", 500),
                ("other/c.png", "video/parking2.mp4", 0),
            ])
            labels = benchmark.crop_labels(index, truth(
                {"path": "video/parking.mp4", "expected": "152JTA02"}))
        self.assertEqual(sorted(labels), ["kz/a.png", "kz/b.png"])
        self.assertEqual(labels["kz/a.png"], {"expected": "152JTA02", "region": "kz"})

    def test_time_windows_label_only_crops_inside_them(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            index = self.write_index(Path(tmp), [
                ("a.png", "video/two.mp4", 100),
                ("b.png", "video/two.mp4", 5000),
                ("c.png", "video/two.mp4", 9000),
            ])
            labels = benchmark.crop_labels(index, truth(
                {"path": "video/two.mp4", "expected": "111AAA01", "start_ms": 0, "stop_ms": 1000},
                {"path": "video/two.mp4", "expected": "222BBB02", "start_ms": 4000,
                 "stop_ms": 6000}))
        self.assertEqual(labels["a.png"]["expected"], "111AAA01")
        self.assertEqual(labels["b.png"]["expected"], "222BBB02")
        self.assertNotIn("c.png", labels)

    def test_crop_accuracy_counts_exact_accepted_and_cer_by_region(self) -> None:
        labels = {
            "a.png": {"expected": "152JTA02", "region": "kz"},
            "b.png": {"expected": "152JTA02", "region": "kz"},
            "c.png": {"expected": "BR45IL", "region": "jp"},
        }
        results = [
            {"file": "a.png", "text": "152JTA02", "rejection": "none"},
            # Cyrillic lookalikes and punctuation normalize away; one substitution remains.
            {"file": "b.png", "text": "152-JТА62", "rejection": "low_confidence"},
            {"file": "c.png", "text": "BR45IL", "rejection": "low_confidence"},
            {"file": "unlabelled.png", "text": "X", "rejection": "none"},
        ]
        accuracy = benchmark.crop_accuracy(results, labels)
        self.assertTrue(accuracy["available"])
        self.assertEqual(accuracy["crops"], 3)
        self.assertEqual(accuracy["exact"], 2)
        self.assertEqual(accuracy["accepted_exact"], 1)
        self.assertAlmostEqual(accuracy["cer"], 1 / 22)
        self.assertEqual(accuracy["by_region"]["kz"]["exact"], 1)
        self.assertAlmostEqual(accuracy["by_region"]["kz"]["cer"], 1 / 16)
        self.assertEqual(accuracy["by_region"]["jp"]["exact_accuracy"], 1.0)

    def test_crop_accuracy_without_labels_is_unavailable(self) -> None:
        accuracy = benchmark.crop_accuracy([{"file": "a.png", "text": "1", "rejection": "none"}], {})
        self.assertFalse(accuracy["available"])


if __name__ == "__main__":
    unittest.main()
