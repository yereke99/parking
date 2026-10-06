#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

if ! command -v uv >/dev/null 2>&1; then
    echo "uv is required (https://docs.astral.sh/uv/). No system Python changes were made." >&2
    exit 2
fi

selection="${1:---all}"
if [[ "$selection" != "--all" && "$selection" != "--easyocr" && "$selection" != "--paddleocr" ]]; then
    echo "usage: $0 [--all|--easyocr|--paddleocr]" >&2
    exit 2
fi

python_command="${PYTHON:-python3}"
jetson=0
if [[ -f /etc/nv_tegra_release ]]; then
    jetson=1
fi
if [[ "$jetson" == "1" && "$selection" != "--easyocr" ]]; then
    "$python_command" - <<'PY'
import sys
if sys.version_info < (3, 8):
    raise SystemExit(
        "The current PaddleOCR 3.x setup is incompatible with Python 3.6 on JetPack 4. "
        "Use Dockerfile.jetson-nano for the pinned Nano research environment."
    )
PY
fi

setup_easyocr() {
    if [[ "$jetson" == "1" ]]; then
        "$python_command" - <<'PY'
import torch, torchvision
print(f"Reusing JetPack PyTorch {torch.__version__} from {torch.__file__}")
if not torch.cuda.is_available():
    raise SystemExit("EasyOCR Jetson setup requires NVIDIA's CUDA-enabled PyTorch")
PY
        torch_before="$($python_command -c 'import torch; print(torch.__version__)')"
        uv venv --clear --system-site-packages --python "$python_command" .venv-easyocr
        uv pip install --python .venv-easyocr/bin/python easyocr==1.7.2
        torch_after="$(.venv-easyocr/bin/python -c 'import torch; print(torch.__version__)')"
        if [[ "$torch_before" != "$torch_after" ]]; then
            echo "ERROR: EasyOCR setup shadowed JetPack PyTorch ($torch_before -> $torch_after)." >&2
            exit 4
        fi
    else
        uv venv --clear --python 3.11 .venv-easyocr
        uv pip install --python .venv-easyocr/bin/python easyocr==1.7.2
    fi
    .venv-easyocr/bin/python tools/research_ocr_worker.py \
        --probe --engine easyocr --device auto --model-cache models/research
}

setup_paddleocr() {
    # PaddlePaddle itself has no official arm64 wheel. The worker therefore uses PaddleOCR's
    # ONNX Runtime engine. On Jetson, expose a JetPack-compatible Python ONNX Runtime package
    # before running this setup; never install an arbitrary x86/CUDA wheel over it.
    if [[ "$jetson" == "1" ]]; then
        "$python_command" - <<'PY'
import onnxruntime
print("Reusing Jetson ONNX Runtime providers:", onnxruntime.get_available_providers())
PY
        ort_before="$($python_command -c 'import onnxruntime; print(onnxruntime.__version__)')"
        uv venv --clear --system-site-packages --python "$python_command" .venv-paddleocr
        uv pip install --python .venv-paddleocr/bin/python paddleocr==3.7.0
        ort_after="$(.venv-paddleocr/bin/python -c 'import onnxruntime; print(onnxruntime.__version__)')"
        if [[ "$ort_before" != "$ort_after" ]]; then
            echo "ERROR: PaddleOCR setup shadowed Jetson ONNX Runtime ($ort_before -> $ort_after)." >&2
            exit 4
        fi
    else
        uv venv --clear --python 3.11 .venv-paddleocr
        uv pip install --python .venv-paddleocr/bin/python \
            paddleocr==3.7.0 onnxruntime
    fi
    .venv-paddleocr/bin/python tools/research_ocr_worker.py \
        --probe --engine paddleocr --device auto --paddle-engine onnxruntime \
        --paddle-model eslav_PP-OCRv5_mobile_rec --model-cache models/research
}

if [[ "$selection" == "--all" || "$selection" == "--easyocr" ]]; then
    setup_easyocr
fi
if [[ "$selection" == "--all" || "$selection" == "--paddleocr" ]]; then
    setup_paddleocr
fi
