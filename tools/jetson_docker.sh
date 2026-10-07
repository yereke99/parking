#!/usr/bin/env bash
# Builds and runs the Jetson Nano image.
#
#   tools/jetson_docker.sh build              build the image (Docker's default runtime must be nvidia)
#   tools/jetson_docker.sh check              list the backends, load both models on TensorRT and
#                                             cache their engines
#   tools/jetson_docker.sh run [kz_anpr args] run the ANPR with config/jetson-nano.yaml
#   tools/jetson_docker.sh bench [args]       run kz_anpr_benchmark with config/jetson-nano.yaml
#   tools/jetson_docker.sh shell              open a shell in the image
#
# The checkout is mounted read-only at /workspace; var/ (engine cache, events.jsonl) is writable.
# KZ_ANPR_CONTAINER names the container (the systemd service sets it); JETSON_IMAGE and
# KZ_ANPR_CONFIG override the image and the config file inside it.
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

docker_bin="${DOCKER:-docker}"
image="${JETSON_IMAGE:-kz-anpr:jetson-nano-r32.7.1}"
config="${KZ_ANPR_CONFIG:-/workspace/config/jetson-nano.yaml}"
layout="production-1"
action="${1:-}"
shift || true

if [[ "$docker_bin" == *" "* ]]; then
    echo "DOCKER must name one executable. Configure Docker group access instead of embedding sudo." >&2
    exit 2
fi

case "$action" in
    build|check|run|bench|shell)
        ;;
    *)
        echo "usage: $0 build|check|run|bench|shell [arguments]" >&2
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
    # The build compiles against these host files through the NVIDIA runtime's CSV mounts.
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
image_layout="$("$docker_bin" image inspect --format '{{index .Config.Labels "kz-anpr.layout"}}' \
    "$image" 2>/dev/null || true)"
if [[ "$image_layout" != "$layout" ]]; then
    echo "ERROR: image $image is missing or was built from an older checkout." >&2
    echo "Build it first: make docker-build" >&2
    exit 3
fi
if [[ "$action" != "shell" && ! -s models/license_plate_detector.onnx ]]; then
    echo "ERROR: models/license_plate_detector.onnx is missing." >&2
    echo "Export it from license_plate_detector.pt on a machine with ultralytics" >&2
    echo "(tools/export_detector_onnx.py) and copy it into models/ (see models/README.md)." >&2
    exit 3
fi

mkdir -p var
run_args=(
    run --rm --runtime nvidia --network host --ipc host
    -e NVIDIA_VISIBLE_DEVICES=all
    -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,video
    -v "$project_dir:/workspace:ro"
    -v "$project_dir/var:/workspace/var:rw"
)
if [[ -n "${KZ_ANPR_CONTAINER:-}" ]]; then
    run_args+=( --name "$KZ_ANPR_CONTAINER" )
fi
shopt -s nullglob
for camera_device in /dev/video*; do
    run_args+=( --device "$camera_device:$camera_device" )
done
shopt -u nullglob
if [[ -S /tmp/argus_socket ]]; then
    run_args+=( -v /tmp/argus_socket:/tmp/argus_socket )
fi

case "$action" in
    check)
        "$docker_bin" "${run_args[@]}" "$image" /opt/kz-anpr/bin/kz_anpr --print-backends
        # Loads both models exactly as a run does: builds and caches the TensorRT engines on the
        # first call (minutes for the detector), then just loads them.
        output="$("$docker_bin" "${run_args[@]}" "$image" \
            /opt/kz-anpr/bin/kz_anpr --config "$config" --warmup 2>&1)" || {
            echo "$output"
            echo "CHECK FAILED: the models did not load; see the lines above." >&2
            exit 4
        }
        echo "$output"
        if ! grep -q 'event=model_loaded tag=detector .*backend=tensorrt' <<<"$output"; then
            echo "CHECK FAILED: the detector is not on TensorRT." >&2
            exit 4
        fi
        if grep -q 'event=model_loaded tag=ocr .*backend=tensorrt' <<<"$output"; then
            echo "CHECK OK: detector and OCR run on TensorRT (GPU)."
        else
            echo "CHECK WARNING: the OCR fell back from TensorRT; see its model_loaded line." >&2
        fi
        ;;
    run)
        exec "$docker_bin" "${run_args[@]}" "$image" \
            /opt/kz-anpr/bin/kz_anpr --config "$config" "$@"
        ;;
    bench)
        exec "$docker_bin" "${run_args[@]}" "$image" \
            /opt/kz-anpr/bin/kz_anpr_benchmark --config "$config" "$@"
        ;;
    shell)
        exec "$docker_bin" "${run_args[@]}" -it "$image" /bin/bash
        ;;
esac
