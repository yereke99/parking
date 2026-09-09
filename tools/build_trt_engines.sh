#!/usr/bin/env bash
#
# Builds and caches the TensorRT engines for both models on a Jetson device.
#
# The TensorRT execution provider compiles an engine the first time a model runs, which takes
# minutes and would otherwise happen while a vehicle is waiting at the barrier. Running this once
# after deployment populates the serialised engine cache, so every later start loads instead of
# builds.
#
# Re-run it whenever the models, TensorRT, JetPack, or the GPU change: an engine is only valid for
# the exact combination it was built against, and a stale cache entry is silently rebuilt.
#
# Usage: tools/build_trt_engines.sh [config] [binary]

set -euo pipefail

CONFIG="${1:-config/default.yaml}"
BINARY="${2:-./build/kz_anpr}"

if [[ ! -x "${BINARY}" ]]; then
    echo "error: ${BINARY} not found or not executable. Build the project first." >&2
    exit 1
fi
if [[ ! -f "${CONFIG}" ]]; then
    echo "error: config ${CONFIG} not found." >&2
    exit 1
fi

CACHE_DIR="$(awk '/^[[:space:]]*engine_cache_dir:/ {print $2; exit}' "${CONFIG}")"
CACHE_DIR="${CACHE_DIR:-models/trt_cache}"

echo "Available inference providers:"
"${BINARY}" --print-backends

if ! "${BINARY}" --print-backends | grep -q "TensorrtExecutionProvider"; then
    echo
    echo "error: this ONNX Runtime build has no TensorRT execution provider." >&2
    echo "On Jetson, install the JetPack build of onnxruntime-gpu that matches your" >&2
    echo "TensorRT and CUDA versions, then rebuild kz_anpr against it." >&2
    exit 2
fi

mkdir -p "${CACHE_DIR}"
echo
echo "Building engines into ${CACHE_DIR}. This takes several minutes per model."
echo "Do not interrupt it: a partial cache entry is discarded and rebuilt on the next run."
echo

time "${BINARY}" --config "${CONFIG}" --backend tensorrt --warmup

echo
echo "Cache contents:"
ls -lh "${CACHE_DIR}"
echo
echo "Verify a real start now loads instead of builds:"
echo "  time ${BINARY} --config ${CONFIG} --backend tensorrt --warmup"
echo "The second run should complete in seconds."
