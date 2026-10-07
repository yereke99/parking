#!/usr/bin/env python3
"""Fail-fast compatibility check executed inside the Jetson Nano container."""

import json
import os
import platform
import subprocess
import sys
from pathlib import Path


ROOT = Path(os.environ.get("KZ_ANPR_ROOT", "/workspace"))
EXPECTED = {
    "machine": "aarch64",
    "python": "3.9.25",
    "worker_python_prefix": "3.6.",
    "numpy": "1.19.5",
    "scipy": "1.5.4",
    "skimage": "0.17.2",
    "pillow": "8.4.0",
    "pyyaml": "5.4.1",
    "torch_prefix": "1.10.0",
    "torchvision_prefix": "0.11.0",
    "easyocr": "1.6.2",
}


def check(label, ok, detail, failures):
    state = "PASS" if ok else "FAIL"
    print("[{0}] {1}: {2}".format(state, label, detail))
    if not ok:
        failures.append("{0}: {1}".format(label, detail))


def main():
    failures = []
    machine = platform.machine()
    check("architecture", machine == EXPECTED["machine"], machine, failures)
    check("orchestrator Python", platform.python_version() == EXPECTED["python"],
          platform.python_version(), failures)

    release = Path("/etc/nv_tegra_release")
    release_text = release.read_text().strip() if release.is_file() else "missing"
    check("Jetson Linux", "R32 (release), REVISION: 7" in release_text,
          release_text, failures)

    mem_total_mb = 0
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemTotal:"):
            mem_total_mb = int(line.split()[1]) // 1024
            break
    check("memory", 3000 <= mem_total_mb <= 5000,
          "{0} MB total (Nano 4 GB expected)".format(mem_total_mb), failures)

    worker_probe = r'''
import json, platform
import cv2, easyocr, numpy, scipy, skimage, torch, torchvision, tensorrt, yaml
from PIL import __version__ as pillow_version
print(json.dumps({
    "python": platform.python_version(),
    "numpy": numpy.__version__, "scipy": scipy.__version__, "skimage": skimage.__version__,
    "pillow": pillow_version, "pyyaml": yaml.__version__,
    "opencv": cv2.__version__,
    "opencv_cuda_devices": cv2.cuda.getCudaEnabledDeviceCount() if hasattr(cv2, "cuda") else 0,
    "torch": torch.__version__, "torchvision": torchvision.__version__,
    "torch_cuda": torch.cuda.is_available(), "torch_cuda_build": torch.version.cuda,
    "tensorrt": tensorrt.__version__, "easyocr": easyocr.__version__,
}, sort_keys=True))
'''
    try:
        process = subprocess.Popen(
            ["/usr/bin/python3", "-c", worker_probe], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, universal_newlines=True,
        )
        stdout, stderr = process.communicate()
        if process.returncode != 0:
            raise RuntimeError(stderr.strip() or stdout.strip())
        worker = json.loads(stdout.splitlines()[-1])
        worker_ok = (
            worker["python"].startswith(EXPECTED["worker_python_prefix"])
            and worker["numpy"] == EXPECTED["numpy"]
            and worker["scipy"] == EXPECTED["scipy"]
            and worker["skimage"] == EXPECTED["skimage"]
            and worker["pillow"] == EXPECTED["pillow"]
            and worker["pyyaml"] == EXPECTED["pyyaml"]
            and worker["opencv"].startswith("4.5.")
            and worker["torch"].startswith(EXPECTED["torch_prefix"])
            and worker["torchvision"].startswith(EXPECTED["torchvision_prefix"])
            and worker["tensorrt"].startswith("8.2.")
            and worker["easyocr"] == EXPECTED["easyocr"]
        )
        check("JetPack Python worker stack", worker_ok, json.dumps(worker, sort_keys=True),
              failures)
        check("CUDA through PyTorch", worker["torch_cuda"],
              "available={0}, build={1}".format(
                  worker["torch_cuda"], worker["torch_cuda_build"]), failures)
    except Exception as exc:
        check("JetPack Python worker stack", False, str(exc), failures)

    required = [
        ROOT / "models/license_plate_detector.onnx",
        Path("/opt/kz-anpr/models/fast-plate-ocr/cct_s_v2_global.onnx"),
        Path("/opt/kz-anpr/models/fast-plate-ocr/cct_s_v2_global_float.onnx"),
        Path("/opt/kz-anpr/models/nomeroff-onnx/kz.onnx"),
        ROOT / "models/plate_ocr_config.yaml",
        ROOT / "data/manifests/video_research.csv",
        ROOT / "video/car.mp4",
        ROOT / "video/parking.mp4",
        ROOT / "video/parking2.mp4",
        Path("/opt/kz-anpr/models/research/easyocr/english_g2.pth"),
    ] + [
        Path("/opt/kz-anpr/models/easyocr-onnx/english_g2_{0}.onnx".format(width))
        for width in (64, 128, 192, 256, 320, 384)
    ]
    missing = [str(path) for path in required if not path.is_file() or path.stat().st_size == 0]
    check("models/videos/manifests", not missing,
          "all present" if not missing else "missing: " + ", ".join(missing), failures)

    try:
        process = subprocess.Popen(
            ["/opt/kz-anpr/bin/kz_anpr", "--print-backends"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True,
        )
        output = process.communicate()[0].strip()
        check("native TensorRT link", process.returncode == 0 and
              "provider=NativeTensorRTExecutionProvider" in output, output, failures)
        check("native ONNX Runtime CPU fallback", process.returncode == 0 and
              "provider=CPUExecutionProvider" in output, output, failures)
    except Exception as exc:
        check("native runtime", False, str(exc), failures)

    # This is deliberately an actual model inference, not an import-only CUDA check. It also
    # creates the FP16 engine cache used by the subsequent project and research runs.
    try:
        process = subprocess.Popen(
            [
                "/opt/kz-anpr/bin/kz_anpr_benchmark",
                "--video", str(ROOT / "video/car.mp4"),
                "--config", str(ROOT / "config/jetson-nano-research.yaml"),
                "--backend", "tensorrt",
                "--detector-only",
                "--max-frames", "1",
                "--warmup-frames", "0",
            ],
            cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        stdout, stderr = process.communicate()
        gpu_ok = process.returncode == 0 and "backend=tensorrt" in stdout
        detail = stdout.strip() if gpu_ok else (stderr.strip() or stdout.strip())
        check("project detector GPU inference", gpu_ok, detail, failures)
    except Exception as exc:
        check("project detector GPU inference", False, str(exc), failures)

    compatibility = {
        "fast_plate_ocr": "TensorRT detector + TensorRT FP32 OCR (float32-input model copy; pinned ONNX Runtime 1.11.1 CPU fallback)",
        "easyocr": "TensorRT detector + pinned EasyOCR 1.6.2 NVIDIA PyTorch CUDA OCR",
        "nomeroff": "unavailable: v4.0.1 needs torch >=1.12; JetPack 4 CUDA worker is torch 1.10",
        "nomeroff_onnx": "TensorRT detector + TensorRT FP32 OCR (Nomeroff 4.0.1 kz model exported with PyTorch 1.10; pinned ONNX Runtime 1.11.1 CPU fallback)",
        "paddleocr": "unavailable: v3.7 requires newer Python; PaddlePaddle has no official JetPack 4 aarch64 wheel",
    }
    print("OCR compatibility: " + json.dumps(compatibility, sort_keys=True))
    if failures:
        print("Preflight failed with {0} hard error(s).".format(len(failures)), file=sys.stderr)
        return 1
    print("Jetson Nano preflight passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
