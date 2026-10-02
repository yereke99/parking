#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

if ! command -v uv >/dev/null 2>&1; then
    echo "uv is required (https://docs.astral.sh/uv/). No system Python changes were made." >&2
    exit 2
fi

mode="local"
if [[ "${1:-}" == "--jetson" ]] || [[ -f /etc/nv_tegra_release ]]; then
    mode="jetson"
fi

if [[ "$mode" == "jetson" ]]; then
    python_command="${PYTHON:-python3}"
    "$python_command" - <<'PY'
import cv2, torch, torchvision
print(f"Using JetPack PyTorch {torch.__version__} from {torch.__file__}")
print(f"Using torchvision {torchvision.__version__}")
if not torch.cuda.is_available():
    raise SystemExit("Jetson setup requires a CUDA-enabled NVIDIA PyTorch installation")
PY
    torch_before="$($python_command -c 'import torch; print(torch.__version__)')"
    uv venv --clear --system-site-packages --python "$python_command" .venv-nomeroff
    uv pip install --python .venv-nomeroff/bin/python -r requirements/nomeroff-jetson.txt
    uv pip install --python .venv-nomeroff/bin/python --no-deps \
        'https://github.com/ria-com/nomeroff-net/archive/931388550b83f045c0ac951a77daa23df22f962d.tar.gz'
    torch_after="$(.venv-nomeroff/bin/python -c 'import torch; print(torch.__version__)')"
    if [[ "$torch_before" != "$torch_after" ]]; then
        echo "ERROR: Nomeroff setup shadowed JetPack PyTorch ($torch_before -> $torch_after)." >&2
        echo "Remove .venv-nomeroff and review the NVIDIA wheel/dependency set." >&2
        exit 4
    fi
else
    uv venv --python 3.11 .venv-nomeroff
    uv pip sync --python .venv-nomeroff/bin/python requirements/nomeroff-local.lock
fi

.venv-nomeroff/bin/python tools/nomeroff_worker.py \
    --probe --region "${PLATE_REGION_MODE:-kz}" --device "${DEVICE:-auto}"
