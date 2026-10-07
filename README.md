# Kazakhstan Parking ANPR

Native C++ automatic number plate recognition for a parking barrier, with Nomeroff Net running in
one persistent isolated Python worker for regional OCR. Detection, tracking, state, validation,
consensus, event delivery and camera ingestion remain C++.

The reproducible legacy-device research target is an NVIDIA Jetson Nano Developer Kit 4 GB on
JetPack 4.x. Orin remains a separate production target. The same source still builds on x86 and
macOS for development.

```text
camera
  -> cheap ROI motion monitoring, every frame
  -> plate detector at a state-dependent cadence
  -> lightweight tracking and stop detection
  -> crop quality gate
  -> Nomeroff Net KZ/RU/CIS crop OCR
  -> configurable plate-format validation
  -> multi-frame consensus
  -> PlateRecognitionEvent
```

The point is not raw throughput. It is spending compute only when a vehicle is actually there:
on the sample clip the detector runs on 16 percent of frames and OCR three times per vehicle.

## Jetson Nano Docker workflow

Run these commands on the Nano after installing JetPack 4.6.1-4.6.6 (L4T R32.7.x), Docker and
the NVIDIA container runtime. The tested field device reports R32.7.6; the container uses
NVIDIA's latest published R32.7 L4T ML image, `r32.7.1-py3`:

```sh
make jetson-all
```

This single command builds the pinned Docker image, validates GPU/CUDA/TensorRT with a real model
inference, runs all four OCR rows with 1 and 4 streams, prints the final comparison table, and
writes the full timestamped result to `benchmark_results/research_*.md` and `.json`.

To run the ANPR project itself after the check:

```sh
make run
```

`make benchmark-1` and `make benchmark-4` run only the selected stream count. For a short smoke
test, use `make jetson-all MAX_FRAMES=300`. Source, videos, manifests and models are mounted from
the existing checkout; results are written to `benchmark_results/`. The host is not modified by
pip, CMake or CUDA installers.

The image is pinned to NVIDIA's L4T ML R32.7.1 ARM64 image (CUDA 10.2, PyTorch 1.10, OpenCV 4.5)
and builds pinned Python 3.9.25 with one compiler job to stay inside 4 GB RAM. The project detector
runs directly on native TensorRT 8.2/CUDA in FP16 and caches its engine under
`benchmark_results/trt_cache`; preflight performs a real inference and rejects a CPU fallback.
Benchmark control runs on Python 3.9; the CUDA EasyOCR worker remains isolated on JetPack's
Python 3.6 because NVIDIA's PyTorch wheel is ABI-specific. Fast Plate OCR uses pinned ONNX
Runtime 1.11.1 on CPU while the shared detector remains on GPU; EasyOCR 1.6.2 uses the JetPack
CUDA-enabled PyTorch build. Nomeroff 4.0.1 and
PaddleOCR 3.7 are retained in the four-backend report as explicit `unavailable` rows: their
upstream Python/PyTorch/Paddle requirements are incompatible with JetPack 4 and no compatible
official aarch64 package exists. They are never replaced with a misleading differently named OCR.

Python 3.9 reached upstream end-of-life in October 2025; 3.9.25 is intentionally the final pinned
release requested for this legacy deployment, not an unbounded `3.9` tag.

See [Jetson Nano deployment](docs/DEPLOYMENT_JETSON.md) for compatibility details and preflight
checks.

`make run` starts the full project with TensorRT detection and CUDA EasyOCR. If the CUDA OCR worker
fails or is not ready within 240 s, it continues with the same EasyOCR model on CPU and logs
`fallback=<reason>` (see [Jetson Nano deployment](docs/DEPLOYMENT_JETSON.md)). Override the input
without editing configuration, for example:

```sh
make run RUN_ARGS='--source rtsp://user:pass@camera/stream'
```

## Build

Core tests need nothing but a compiler:

```sh
cmake -S . -B build-core -DKZ_ANPR_BUILD_RUNTIME=OFF
cmake --build build-core -j
./build-core/kz_anpr_core_tests
```

The portable C++ runtime uses OpenCV and ONNX Runtime; the Jetson Docker build additionally links
the native TensorRT backend described above. Nomeroff is installed in its own environment and does
not modify system Python:

```sh
# Debian, Ubuntu, JetPack
sudo apt install -y build-essential cmake pkg-config libopencv-dev
# macOS
brew install cmake opencv onnxruntime

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

tools/setup_nomeroff_env.sh
.venv-nomeroff/bin/python tools/nomeroff_worker.py --probe --region kz
```

On newer Jetson releases that satisfy Nomeroff's Python/PyTorch requirements, install NVIDIA's
JetPack-matched PyTorch first and then use `tools/setup_nomeroff_env.sh --jetson`. Jetson Nano /
JetPack 4 must use the pinned Docker workflow above; the setup script refuses that incompatible
combination. See [the Nomeroff integration notes](docs/NOMEROFF_INTEGRATION.md).

Options: `-DKZ_ANPR_BUILD_RUNTIME=OFF`, `-DKZ_ANPR_BUILD_TESTS=OFF`,
`-DKZ_ANPR_WITH_ONNXRUNTIME=OFF`, `-DKZ_ANPR_SANITIZERS=ON`.

## Models

Required model artifacts must be on disk. Nothing is downloaded at runtime.

```text
models/license_plate_detector.onnx    YOLOv8n plate detector
models/nomeroff/                      ignored local cache populated by the setup probe
models/plate_ocr.onnx                 legacy A/B backend only
models/plate_ocr_config.yaml          legacy A/B backend contract
```

Export the detector from the Ultralytics checkpoint:

```sh
python3 tools/export_detector_onnx.py --weights license_plate_detector.pt --imgsz 640
mv license_plate_detector.onnx models/
```

Fetch and verify the pinned Nomeroff KZ model once, on a machine with network access:

```sh
tools/setup_nomeroff_env.sh
```

The setup pins Nomeroff Net 4.0.1 to commit `931388550b83f045c0ac951a77daa23df22f962d`.
Production starts with the cache already populated; normal runtime must not depend on a download.

## Run

```sh
./build/kz_anpr --config config/default.yaml --source video/car.mp4
./build/kz_anpr --config config/default.yaml --source 0 --camera-id gate-01
./build/kz_anpr --config config/default.yaml --source "rtsp://user:pass@camera/stream1"

# Multiple sources share detector and OCR weights; each keeps independent tracking/state.
./build/kz_anpr --config config/default.yaml \
  --source cam1.mp4 --source cam2.mp4 --source cam3.mp4 --source cam4.mp4

./build/kz_anpr --print-backends              # what this machine can actually run
./build/kz_anpr --config config/default.yaml --warmup   # load models, build TensorRT engines
./build/kz_anpr --config config/default.yaml --source video/car.mp4 --timeline --log-level debug
```

Exit codes: 0 success, 1 unexpected error, 2 configuration error, 3 camera unavailable, 4 model
or backend unavailable.

Each finished recognition prints one JSON event on stdout. Structured logs go to stdout and
stderr as `key=value` lines.

## Wiring up a barrier

`PlateSink` is the whole integration surface. Implement it to call a database, post to an HTTP
API, drive a GPIO relay or hand the plate to an access-control service:

```cpp
class BarrierSink final : public anpr::PlateSink {
public:
    void onRecognition(const anpr::PlateRecognitionEvent& event) override {
        if (!anpr::isAccepted(event.status)) {
            return;
        }
        openBarrierFor(event.normalized_plate);
    }
};
```

It is called from the processing thread. Anything that can block belongs on its own queue.

## Benchmark

```sh
python3 tools/benchmark.py --streams 1
python3 tools/benchmark.py --streams 2
python3 tools/benchmark.py --streams 4
python3 tools/benchmark.py --matrix

# Full research matrix: 4 OCRs x 3 videos x 1/4 processing threads.
python3 tools/benchmark.py --research

# Lower-level deterministic single-file benchmark remains available.
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml --detector-only
```

Measured results and the comparison against the Python prototype are in
[benchmarks/README.md](benchmarks/README.md).

The four OCR implementations available to the benchmark are `fast_plate_ocr`, `nomeroff`,
`paddleocr`, and `easyocr`. PaddleOCR and EasyOCR are persistent recognition-only workers: the
native detector has already produced a plate crop, so their scene/document text detectors are
not run. On local development machines, install the two optional research environments with
`tools/setup_research_ocr_envs.sh --all`. See [OCR research](docs/OCR_RESEARCH.md) for the exact
Nano Docker procedure, fairness rules, output columns, and hardware caveats.

### Manual/non-container: one video, one processing thread

The collector also records `tegrastats`, RAM, CPU, GPU, temperature and power:

```sh
python3 tools/benchmark.py \
  --config config/research.yaml \
  --video video/parking.mp4 \
  --ocr-backend nomeroff \
  --streams 1 \
  --manifest data/manifests/video_research.csv
```

Replace `nomeroff` with `fast_plate_ocr`, `paddleocr`, or `easyocr` to test one of the other
recognizers under identical conditions.

### Manual/non-container: the same video as four cameras / four processing threads

```sh
python3 tools/benchmark.py \
  --config config/research.yaml \
  --video video/parking.mp4 \
  --ocr-backend nomeroff \
  --streams 4 \
  --manifest data/manifests/video_research.csv
```

The lower-level binary equivalents are:

```sh
./build/kz_anpr_benchmark --config config/research.yaml --video video/parking.mp4 \
  --ocr-backend nomeroff --streams 1 --json
./build/kz_anpr_benchmark --config config/research.yaml --video video/parking.mp4 \
  --ocr-backend nomeroff --streams 4 --json
```

## Configuration

Every operational threshold lives in `config/default.yaml`, documented in place. Nothing
important is a constant in the source. An unrecognised key is reported at startup rather than
silently ignored, so a typo in a threshold name is visible.

Categories: `camera`, `inference`, `roi`, `motion`, `detector`, `ocr`, `quality`, `tracking`,
`stop_detection`, `consensus`, `recognition`, `validation`, `debug`, `performance`, `logging`.

The shipped values were tuned against the bundled development clip. Re-tune the ROIs, motion
thresholds and stop window against footage from the actual barrier.

## The Python prototype

`legacy_python/` keeps the original `main.py` and `main_fast.py` for output comparison during
migration. They are not part of the build, are never invoked by the runtime, and can be deleted
once the C++ implementation is validated on real Kazakhstan footage.

## Documentation

- [Migration plan](docs/MIGRATION_PLAN.md)
- [Architecture](docs/architecture.md)
- [Audit and baseline](docs/AUDIT.md)
- [Kazakhstan plate formats](docs/KAZAKHSTAN_PLATES.md)
- [Model evaluation](docs/MODEL_EVALUATION.md)
- [Nomeroff integration and licensing](docs/NOMEROFF_INTEGRATION.md)
- [Jetson deployment](docs/DEPLOYMENT_JETSON.md)
- [Wiren Board 8 deployment, CPU only](docs/DEPLOYMENT_WB8.md)
- [Camera setup](docs/CAMERA_SETUP.md)
- [Benchmarks](benchmarks/README.md)
