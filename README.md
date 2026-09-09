# Kazakhstan Parking ANPR

Native C++ automatic number plate recognition for a parking barrier. No Python in the deployed
runtime.

Primary target is an NVIDIA Jetson Orin Nano Super running TensorRT FP16. The same binary runs on
x86 or macOS against the ONNX Runtime CPU provider for development.

```text
camera
  -> cheap ROI motion monitoring, every frame
  -> plate detector at a state-dependent cadence
  -> lightweight tracking and stop detection
  -> crop quality gate
  -> Fast Plate OCR
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

The runtime needs OpenCV and, for OCR, ONNX Runtime:

```sh
# Debian, Ubuntu, JetPack
sudo apt install -y build-essential cmake pkg-config libopencv-dev
# macOS
brew install cmake opencv onnxruntime

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Fast Plate OCR needs a uint8 input tensor, which OpenCV DNN cannot supply, so ONNX Runtime is
required for recognition. Without it the detector still builds and the OCR stage refuses to start
with a clear message rather than feeding the model the wrong thing.

Options: `-DKZ_ANPR_BUILD_RUNTIME=OFF`, `-DKZ_ANPR_BUILD_TESTS=OFF`,
`-DKZ_ANPR_WITH_ONNXRUNTIME=OFF`, `-DKZ_ANPR_SANITIZERS=ON`.

## Models

Both models must be on disk. Nothing is downloaded at runtime.

```text
models/license_plate_detector.onnx    YOLOv8n plate detector
models/plate_ocr.onnx                 Fast Plate OCR, cct-s-v2-global
models/plate_ocr_config.yaml          the model's own contract, read at startup
```

Export the detector from the Ultralytics checkpoint:

```sh
python3 tools/export_detector_onnx.py --weights license_plate_detector.pt --imgsz 640
mv license_plate_detector.onnx models/
```

Fetch the OCR model once, on a machine with network access:

```sh
python3 -m venv .venv-fast
.venv-fast/bin/pip install 'fast-plate-ocr[onnx]'
.venv-fast/bin/python tools/fetch_ocr_model.py --model cct-s-v2-global-model
```

Both scripts are offline development tools. Neither is needed, or present, at runtime.

Image size, colour mode, interpolation, alphabet, padding character and slot count all come from
`plate_ocr_config.yaml`, so switching to another Fast Plate OCR model is a file swap. The loader
cross-checks that file against the ONNX signature and refuses a mismatched pair at startup.

## Run

```sh
./build/kz_anpr --config config/default.yaml --source video/car.mp4
./build/kz_anpr --config config/default.yaml --source 0 --camera-id gate-01
./build/kz_anpr --config config/default.yaml --source "rtsp://user:pass@camera/stream1"

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
- [Jetson deployment](docs/DEPLOYMENT_JETSON.md)
- [Wiren Board 8 deployment, CPU only](docs/DEPLOYMENT_WB8.md)
- [Camera setup](docs/CAMERA_SETUP.md)
- [Benchmarks](benchmarks/README.md)
