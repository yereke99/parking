#!/usr/bin/env python3
"""Offline helper that places a Fast Plate OCR model where the C++ runtime expects it.

This is a development-time tool. The deployed application never downloads anything and never
runs Python: it reads `models/plate_ocr.onnx` and `models/plate_ocr_config.yaml` from disk.

Run it once on a machine with network access, then ship the two files with the build.

    python3 tools/fetch_ocr_model.py --model cct-s-v2-global-model

Requires `pip install fast-plate-ocr[onnx]` in a throwaway virtual environment. Nothing from that
environment is needed at runtime.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

# Every model in the Fast Plate OCR hub whose ONNX export the C++ runtime can consume. The
# runtime reads the slot count, alphabet, image size and colour mode from the model's own YAML,
# so any of these works without a code change.
KNOWN_MODELS = [
    "cct-s-v2-global-model",
    "cct-xs-v2-global-model",
    "cct-s-v1-global-model",
    "cct-xs-v1-global-model",
    "global-plates-mobile-vit-v2-model",
    "european-plates-mobile-vit-v2-model",
    "argentinian-plates-cnn-model",
    "argentinian-plates-cnn-synth-model",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model",
        default="cct-s-v2-global-model",
        help=f"hub model name. Known: {', '.join(KNOWN_MODELS)}",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "models",
        help="destination directory (default: the project's models/)",
    )
    args = parser.parse_args()

    if args.model not in KNOWN_MODELS:
        print(f"warning: '{args.model}' is not in the known list; continuing anyway", file=sys.stderr)

    try:
        from fast_plate_ocr.inference import hub
    except ModuleNotFoundError:
        print(
            "error: fast-plate-ocr is not installed.\n"
            "  python3 -m venv .venv-fast\n"
            "  .venv-fast/bin/pip install 'fast-plate-ocr[onnx]'\n"
            "  .venv-fast/bin/python tools/fetch_ocr_model.py",
            file=sys.stderr,
        )
        return 1

    onnx_path, config_path = hub.download_model(model_name=args.model)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    destinations = {
        Path(onnx_path): args.output_dir / "plate_ocr.onnx",
        Path(config_path): args.output_dir / "plate_ocr_config.yaml",
    }
    for source, destination in destinations.items():
        shutil.copyfile(source, destination)
        print(f"{destination}  ({destination.stat().st_size / 1_048_576:.2f} MB)")

    print(
        "\nBoth files are in place. Verify the C++ side reads them with:\n"
        "  ./build/kz_anpr --config config/default.yaml --warmup"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
