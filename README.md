# Kazakhstan Parking ANPR

Native C++ automatic number plate recognition for a parking barrier, with Nomeroff Net running in
one persistent isolated Python worker for regional OCR. Detection, tracking, state, validation,
consensus, event delivery and camera ingestion remain C++.

The first hardware validation target is an NVIDIA Jetson Orin Nano 4 GB; the production target is
an Orin Nano / Orin Nano Super 8 GB. The same binary runs on x86 or macOS for development.

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

## Build

Core tests need nothing but a compiler:

```sh
cmake -S . -B build-core -DKZ_ANPR_BUILD_RUNTIME=OFF
cmake --build build-core -j
./build-core/kz_anpr_core_tests
```

The C++ runtime needs OpenCV and ONNX Runtime for the existing detector. Nomeroff is installed in
its own environment and does not modify system Python:

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

On Jetson, install NVIDIA's JetPack-matched PyTorch first, then use
`tools/setup_nomeroff_env.sh --jetson`. The setup verifies that the NVIDIA PyTorch version was not
replaced. See [the Nomeroff integration notes](docs/NOMEROFF_INTEGRATION.md).

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

# Lower-level deterministic single-file benchmark remains available.
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml --detector-only
```

Measured results and the comparison against the Python prototype are in
[benchmarks/README.md](benchmarks/README.md).

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
