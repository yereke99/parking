#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

docker_bin="${DOCKER:-docker}"
image="${JETSON_IMAGE:-kz-anpr:jetson-nano-r32.7.1}"
action="${1:-}"
shift || true

if [[ "$docker_bin" == *" "* ]]; then
    echo "DOCKER must name one executable. Configure Docker group access instead of embedding sudo." >&2
    exit 2
fi

case "$action" in
    build|check|benchmark|run)
        ;;
    *)
        echo "usage: $0 build|check|benchmark|run [arguments]" >&2
        exit 2
        ;;
esac

if [[ ! -f /etc/nv_tegra_release ]] || \
   ! grep -Eq '^# R32 \(release\), REVISION: 7\.[0-9]+' /etc/nv_tegra_release; then
    detected_release="$(sed -n '1p' /etc/nv_tegra_release 2>/dev/null || true)"
    echo "ERROR: this pinned image requires Jetson Linux R32.7.x (JetPack 4.6.x)." >&2
    echo "Detected: ${detected_release:-/etc/nv_tegra_release is missing}" >&2
    exit 3
fi
host_l4t="$(sed -n \
    's/^# R\([0-9][0-9]*\) (release), REVISION: \([0-9][0-9.]*\).*/R\1.\2/p' \
    /etc/nv_tegra_release)"
echo "Jetson host detected: ${host_l4t} ($(uname -m))"
if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "ERROR: expected aarch64 host, got $(uname -m)." >&2
    exit 3
fi
if [[ "$action" == "build" ]]; then
    default_runtime="$("$docker_bin" info --format '{{.DefaultRuntime}}' 2>/dev/null || true)"
    if [[ "$default_runtime" != "nvidia" ]]; then
        echo "ERROR: Docker's default runtime must be nvidia while building on JetPack 4." >&2
        echo "CUDA, TensorRT and the Jetson driver libraries are mounted from the host into" >&2
        echo "build steps only by the nvidia runtime; the image does not contain them." >&2
        echo "Set \"default-runtime\": \"nvidia\" in /etc/docker/daemon.json, restart Docker," >&2
        echo "and verify: docker info --format '{{.DefaultRuntime}}'" >&2
        exit 3
    fi
    # The build compiles against these host files through the NVIDIA runtime's CSV mounts. Check
    # them here instead of failing after the long Python build.
    csv_dir=/etc/nvidia-container-runtime/host-files-for-container.d
    missing=()
    for required in \
        "$csv_dir/cuda.csv" \
        "$csv_dir/cudnn.csv" \
        "$csv_dir/tensorrt.csv" \
        /usr/local/cuda-10.2/bin/nvcc \
        /usr/local/cuda-10.2/include/cuda_runtime_api.h \
        /usr/local/cuda-10.2/lib64/libcudart.so \
        /usr/include/aarch64-linux-gnu/NvInfer.h \
        /usr/include/aarch64-linux-gnu/NvOnnxParser.h \
        /usr/lib/aarch64-linux-gnu/libnvinfer.so \
        /usr/lib/aarch64-linux-gnu/libnvonnxparser.so; do
        [[ -e "$required" ]] || missing+=("$required")
    done
    if (( ${#missing[@]} > 0 )); then
        echo "ERROR: JetPack CUDA/TensorRT files that the build mounts from this host are missing:" >&2
        printf '  %s\n' "${missing[@]}" >&2
        echo "Install the JetPack components on the Jetson, then rebuild:" >&2
        echo "  sudo apt-get update && sudo apt-get install nvidia-jetpack" >&2
        exit 3
    fi
    # BuildKit does not run build steps through Docker's default runtime, so the mounts above
    # would be missing. Use the classic builder.
    export DOCKER_BUILDKIT=0
    exec "$docker_bin" build --file Dockerfile.jetson-nano --tag "$image" .
fi
if ! "$docker_bin" info --format '{{json .Runtimes}}' | grep -q 'nvidia'; then
    echo "ERROR: Docker NVIDIA runtime is not registered." >&2
    exit 3
fi

mkdir -p benchmark_results

run_args=(
    run --rm --runtime nvidia --network host --ipc host
    -e NVIDIA_VISIBLE_DEVICES=all
    -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,video
    -e KZ_ANPR_ROOT=/workspace
    # Skip cuDNN in the EasyOCR worker: its kernels cost several hundred MB of the memory the GPU
    # shares with the TensorRT detector. See tools/research_ocr_worker.py.
    -e KZ_ANPR_EASYOCR_CUDNN=0
    -v "$project_dir:/workspace:ro"
    -v "$project_dir/benchmark_results:/workspace/benchmark_results:rw"
    -v "$project_dir/models:/workspace/models:ro"
    -v /etc/nv_tegra_release:/etc/nv_tegra_release:ro
)
if command -v nvpmodel >/dev/null 2>&1; then
    power_mode="$(nvpmodel -q 2>&1 || true)"
    run_args+=( -e "KZ_ANPR_POWER_MODE=$power_mode" )
fi
if command -v jetson_clocks >/dev/null 2>&1; then
    clock_state="$(jetson_clocks --show 2>&1 || true)"
    run_args+=( -e "KZ_ANPR_JETSON_CLOCKS=$clock_state" )
fi
if [[ -x /usr/bin/tegrastats ]]; then
    run_args+=( -v /usr/bin/tegrastats:/usr/bin/tegrastats:ro )
fi
shopt -s nullglob
for camera_device in /dev/video*; do
    run_args+=( --device "$camera_device:$camera_device" )
done
shopt -u nullglob
if [[ -S /tmp/argus_socket ]]; then
    run_args+=( -v /tmp/argus_socket:/tmp/argus_socket )
fi

if [[ "$action" == "check" ]]; then
    exec "$docker_bin" "${run_args[@]}" "$image" \
        /opt/python3.9/bin/python3.9 /opt/kz-anpr/tools/jetson_preflight.py
fi

if [[ "$action" == "run" && "${PROJECT_OCR:-easyocr}" == "easyocr_onnx" ]] &&
   ! "$docker_bin" run --rm --entrypoint /bin/sh "$image" \
       -c 'test -f /opt/kz-anpr/models/easyocr-onnx/english_g2_320.onnx' >/dev/null 2>&1; then
    echo "ERROR: image $image predates the easyocr_onnx OCR in this checkout." >&2
    echo "Rebuild it first: make docker-build" >&2
    exit 3
fi

if [[ "$action" == "run" ]]; then
    # The project must start even when CUDA OCR cannot: give the CUDA worker this long, then serve
    # the same EasyOCR model on CPU and log the reason (research_ocr_ready fallback=...).
    # Benchmarks stay strict so a CPU result is never reported as a CUDA row.
    exec "$docker_bin" "${run_args[@]}" -e "OCR_BACKEND=${PROJECT_OCR:-easyocr}" \
        -e "KZ_ANPR_OCR_ACCELERATOR_TIMEOUT_S=${OCR_CUDA_TIMEOUT_S:-240}" "$image" \
        /opt/kz-anpr/bin/kz_anpr \
        --config /workspace/config/jetson-nano-research.yaml \
        "$@"
fi

exec "$docker_bin" "${run_args[@]}" "$image" \
    /opt/python3.9/bin/python3.9 /workspace/tools/benchmark.py \
        --binary /opt/kz-anpr/bin/kz_anpr_benchmark \
        --config /workspace/config/jetson-nano-research.yaml \
        --manifest /workspace/data/manifests/video_research.csv \
        --output-dir /workspace/benchmark_results \
        "$@"
