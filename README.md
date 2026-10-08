# Kazakhstan Parking ANPR

Native C++ number plate recognition for a parking barrier. A YOLOv8n plate detector and Nomeroff
Net's dedicated Kazakhstan OCR model both run on the GPU through TensorRT; no Python runs at
recognition time. The production target is the NVIDIA Jetson Nano 4 GB on JetPack 4.6.x, in a
pinned Docker image. The same source builds on x86 Linux and macOS for development.

```text
camera (RTSP / USB / video file)
  -> ROI motion check, every frame, no neural network
  -> plate detector (TensorRT FP16) at a state-dependent cadence
  -> tracking and stop detection: is a car at the barrier?
  -> crop quality gate (size, sharpness, exposure)
  -> Nomeroff KZ OCR (TensorRT FP32), about 10 ms per crop on the Nano
  -> Kazakhstan plate-format validation (123ABC02, 123AB02) and region lookup
  -> multi-frame vote: three agreeing readings confirm a plate
  -> one JSON event per vehicle
```

Compute follows the vehicle: an empty scene costs one downscaled frame difference, the detector
speeds up as a car approaches, and OCR runs only on good crops while it is at the barrier and stops
as soon as the plate is confirmed.

## Jetson Nano: quick start

Prerequisites (details in [Jetson deployment](docs/DEPLOYMENT_JETSON.md)): JetPack 4.6.x
(L4T R32.7.x), Docker with `"default-runtime": "nvidia"`, your user in the `docker` group, and the
plate detector at `models/license_plate_detector.onnx` (see [models](models/README.md)).

```sh
make docker-build     # once: builds the C++ binaries, downloads and exports the Nomeroff KZ model
make check            # loads both models on TensorRT; the first run builds the engines (minutes)
make run              # replays video/parking.mp4 once and prints the recognised plates

# Live camera, in the foreground (Ctrl+C stops it)
make camera CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
make camera CAMERA=0  # first USB / V4L2 camera

# The same camera as a service: starts at boot, restarts after any failure
make service CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
```

`make bench` prints FPS and per-stage latency on the test clip. `make shell` opens a shell in the
image.

## Replay all five videos

`tools/video-list.txt` lists `parking.mp4` and `IMG_5666.mp4` through `IMG_5669.mp4`. On the
Jetson, run `make run-videos`: it waits for each clip to finish before starting the next, using
the same Docker image, models and recognition settings as `make run`. The clips are mounted from
the checkout at `/workspace/video`; they are not copied into the image. All five original clips
are tracked in Git, so `git pull` brings the videos along with the code. No image rebuild is needed.

Each run creates a new directory under `var/video-runs/` with events and a log for each clip,
plus `summary.json`. The terminal prints each clip's confirmed plate list and `RECOGNIZED`,
`NO_CONFIRMED_PLATES` or `ERROR`. Only `VALID_HIGH_CONFIDENCE` and `VALID_LOW_CONFIDENCE` events
enter the plate list; this is the recognizer's result, not a ground-truth accuracy comparison.
File errors do not stop the remaining clips. Configuration/model errors stop the batch because
they affect every clip; a failed run returns a nonzero exit code. Completed results are saved
after each clip.

The four iPhone files are 4K, 10-bit HEVC with HLG HDR; the first three also carry a 90-degree
rotation. Decoder support on the Jetson must be checked with its existing OpenCV backend. To
avoid changing any container packages, prepare compatible copies on a machine where FFmpeg is
already available (requires its `libx264` encoder):

```sh
make prepare-videos  # writes video/compatible/; preserves all originals and their resolution
# Copy video/compatible/ into the Jetson checkout, then:
make run-videos VIDEO_ARGS='--input-dir video/compatible'
```

Preparation applies the rotation and encodes H.264, 8-bit `yuv420p`, retaining the HLG/BT.2020
color tags. It does not tone-map HDR or alter the recognition pipeline's brightness handling.
Keeping 4K retains the plate crop sizes used by the current quality gates. It also
copies the original `parking.mp4` into the same directory. No FFmpeg runs during recognition.

For the existing native development build, with the Nomeroff KZ ONNX model present:

```sh
make run-videos VIDEO_ARGS='--native'
make run-videos VIDEO_ARGS='--native --input-dir video/compatible'
# Custom list/config/new output directory:
python3 tools/run_videos.py --help
```

## Output

Each vehicle produces one JSON line on stdout. `make camera` and the service also append it to
`var/events.jsonl` (`--events-file PATH`), which another program can follow with `tail -F`:

```json
{"event":"plate_recognition","status":"VALID_HIGH_CONFIDENCE","normalized_plate":"152JTA02","raw_plate":"152JTA02","confidence":0.8309,"timestamp_ms":8533,"time":"2026-10-08T07:12:03.120Z","camera_id":"gate-01","plate_box":{"x":414,"y":340,"width":143,"height":49},"region_code":"02","region_name":"Almaty","format":"current_individual","recognition_latency_ms":4267,"observation_count":30,"agreeing_observations":3,"best_crop_path":null}
```

Open the barrier only for `VALID_HIGH_CONFIDENCE` or `VALID_LOW_CONFIDENCE`. The other statuses
(`LOW_CONFIDENCE`, `INVALID_FORMAT`, `AMBIGUOUS`, `INSUFFICIENT_IMAGE_QUALITY`, `NO_PLATE`,
`TIMEOUT`) report a vehicle whose plate could not be confirmed. `time` is the wall clock in UTC;
`timestamp_ms` is the position in the clip for a file and a monotonic clock for a camera.

Structured logs go to stderr as `key=value` lines. Credentials in camera URLs are masked.

Exit codes: 0 success, 1 unexpected error, 2 configuration error, 3 camera unavailable, 4 model
or backend unavailable.

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
Without writing C++, a separate process can follow `var/events.jsonl` instead.

## Configuration

| File | Use |
| --- | --- |
| `config/jetson-nano.yaml` | production: TensorRT, the thresholds validated on the Nano |
| `config/default.yaml` | development machine: ONNX Runtime on the CPU, the same recognition thresholds, every key documented |

Every operational threshold lives in the YAML, nothing important is a constant in the source, and
an unknown key is reported at startup. Before going live, re-check the ROIs and the stop window
against footage from the real barrier camera: see [camera setup](docs/CAMERA_SETUP.md).

## Development build

```sh
# Debian / Ubuntu
sudo apt install -y build-essential cmake pkg-config libopencv-dev
# macOS
brew install cmake opencv onnxruntime

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Core tests need only a compiler: `-DKZ_ANPR_BUILD_RUNTIME=OFF`. Other options:
`-DKZ_ANPR_BUILD_TESTS=OFF`, `-DKZ_ANPR_WITH_ONNXRUNTIME=OFF`, `-DKZ_ANPR_WITH_TENSORRT=OFF`,
`-DKZ_ANPR_REQUIRE_TENSORRT=ON` (fail without TensorRT, as the Jetson image does),
`-DKZ_ANPR_SANITIZERS=ON`.

The development config expects `models/license_plate_detector.onnx` and
`models/nomeroff-onnx/kz.onnx`; [models/README.md](models/README.md) shows how to produce both.

```sh
./build/kz_anpr --config config/default.yaml --source video/parking.mp4
./build/kz_anpr --config config/default.yaml --source "rtsp://user:pass@camera/stream1"
./build/kz_anpr --print-backends
./build/kz_anpr --config config/default.yaml --warmup
./build/kz_anpr_benchmark --config config/default.yaml --video video/parking.mp4
```

Several `--source` options run several cameras that share the detector and OCR; each keeps its
own tracking and state.

## Documentation

- [Jetson Nano deployment](docs/DEPLOYMENT_JETSON.md)
- [Camera setup](docs/CAMERA_SETUP.md)
- [OCR: Nomeroff Net's Kazakhstan model](docs/OCR.md)
- [Kazakhstan plate formats](docs/KAZAKHSTAN_PLATES.md)
- [Architecture](docs/architecture.md)
- [Models](models/README.md)

The OCR comparison research (Fast Plate OCR, EasyOCR, PaddleOCR, the Nomeroff Python worker) and
its benchmarks are kept on the `research-archive` branch.
