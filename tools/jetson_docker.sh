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

if [[ ! -f /etc/nv_tegra_release ]] || ! grep -q 'R32\.7' /etc/nv_tegra_release; then
    echo "ERROR: this pinned image requires Jetson Linux R32.7.x (JetPack 4.6.x)." >&2
    exit 3
fi
if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "ERROR: expected aarch64 host, got $(uname -m)." >&2
    exit 3
fi
if [[ "$action" == "build" ]]; then
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

if [[ "$action" == "run" ]]; then
    exec "$docker_bin" "${run_args[@]}" -e "OCR_BACKEND=${PROJECT_OCR:-easyocr}" "$image" \
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
