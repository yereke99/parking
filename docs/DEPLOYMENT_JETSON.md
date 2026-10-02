# Jetson Orin Nano Deployment

The first benchmark target is an Orin Nano 4 GB; the later production target is an Orin Nano or
Orin Nano Super 8 GB. Everything below assumes JetPack 6 with CUDA and TensorRT installed from
the NVIDIA repositories. Validate the exact JetPack/PyTorch combination on both memory sizes.

The detector continues to use ONNX Runtime/TensorRT. OCR uses pinned Nomeroff/PyTorch and is
installed separately; do not interpret the detector TensorRT setup as OCR acceleration.

For a CPU-only ARM board, see [Wiren Board 8 deployment](DEPLOYMENT_WB8.md); the application is
the same binary with `inference.backend: onnx_cpu`.

## 1. Base packages

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
    libopencv-dev v4l-utils \
    gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-libav
```

Confirm CUDA and TensorRT came with JetPack:

```sh
dpkg -l | grep -E 'nvidia-tensorrt|cuda-toolkit'
python3 -c "import tensorrt; print(tensorrt.__version__)"
```

## 2. ONNX Runtime with the TensorRT provider

The stock `onnxruntime` package has no TensorRT provider. Use NVIDIA's Jetson build, matched to
your JetPack version, from the Jetson Zoo. It must be the C and C++ package, not just the Python
wheel:

```sh
# Adjust the version to your JetPack release.
wget https://nvidia.box.com/shared/static/<onnxruntime-linux-aarch64-gpu>.tgz
tar xzf onnxruntime-linux-aarch64-gpu-*.tgz
sudo cp -r onnxruntime-linux-aarch64-gpu-*/include /usr/local/include/onnxruntime
sudo cp -r onnxruntime-linux-aarch64-gpu-*/lib/* /usr/local/lib/
sudo ldconfig
```

CMake finds it automatically. If it is installed somewhere unusual, point at it:

```sh
cmake -S . -B build -DONNXRUNTIME_ROOT=/opt/onnxruntime
```

Verify the provider is present before going further:

```sh
./build/kz_anpr --print-backends
```

`TensorrtExecutionProvider` must appear. If it does not, the ONNX Runtime build is wrong and the
application will silently fall back to CPU.

## 3. Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Install the isolated OCR environment without replacing NVIDIA PyTorch:

```sh
# Install NVIDIA's PyTorch/torchvision wheels matching this exact JetPack first.
sudo apt install -y python3-opencv
tools/setup_nomeroff_env.sh --jetson
```

## 4. Models

The detector file and pre-populated Nomeroff cache must be on the device. Normal runtime should
not download anything.

```text
models/license_plate_detector.onnx
models/nomeroff/
```

See [model evaluation](MODEL_EVALUATION.md) for how each was produced.

## 5. Build the detector TensorRT engine once

The TensorRT provider compiles an engine the first time a model runs. That takes minutes and must
not happen while a vehicle waits at the barrier.

```sh
tools/build_trt_engines.sh config/default.yaml ./build/kz_anpr
```

Then confirm the second run loads instead of builds:

```sh
time ./build/kz_anpr --config config/default.yaml --backend tensorrt --warmup
```

The engine cache is tied to the exact model, TensorRT version, JetPack version and GPU. Rebuild
it after changing any of them. Cache files are generated artifacts and are not committed.

## 6. Pin the performance mode

Jetson clocks default to a power-saving profile. For a barrier that must respond quickly:

```sh
sudo nvpmodel -m 0     # MAXN
sudo jetson_clocks     # pin clocks to maximum
```

`nvpmodel` persists across reboots; `jetson_clocks` does not, so run it from a boot unit if you
depend on it. Check thermals under sustained load before committing to MAXN in an enclosure:

```sh
sudo tegrastats
```

## 7. Camera

RTSP through the config file:

```yaml
camera:
  source: "rtsp://user:password@192.168.1.64:554/Streaming/Channels/101"
  kind: rtsp
  rtsp_tcp: true
```

For a hardware-decoded GStreamer pipeline, which keeps decode off the CPU:

```yaml
camera:
  kind: gstreamer
  source: "rtspsrc location=rtsp://user:password@192.168.1.64:554/Streaming/Channels/101 latency=100 protocols=tcp ! rtph265depay ! h265parse ! nvv4l2decoder ! nvvidconv ! video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! appsink drop=true max-buffers=1 sync=false"
```

Use `rtph264depay` and `h264parse` for an H.264 camera. `drop=true max-buffers=1` matters: it
makes GStreamer discard stale frames rather than queue them.

Verify the pipeline outside the application first:

```sh
gst-launch-1.0 rtspsrc location=... ! rtph265depay ! h265parse ! nvv4l2decoder ! fakesink
```

## 8. Configuration

Start from `config/default.yaml` and set:

```yaml
inference:
  backend: tensorrt
  fp16: true
  strict_backend: true    # fail loudly rather than falling back to CPU
  engine_cache_dir: /var/lib/kz-anpr/trt_cache

camera:
  camera_id: gate-01

logging:
  level: info
```

`strict_backend: true` is the important one in production. Without it, a broken engine cache
turns into a silent drop to CPU inference and a barrier that responds seconds late.

Then re-tune the ROIs and the stop window against real footage from the barrier. The shipped
values were tuned on a development clip and are starting points, not deployment values.

## 9. Install

```text
/opt/kz-anpr/bin/kz_anpr
/opt/kz-anpr/models/
/etc/kz-anpr/default.yaml
/var/lib/kz-anpr/trt_cache
```

```sh
sudo useradd --system --home /var/lib/kz-anpr --create-home --shell /usr/sbin/nologin kz-anpr
sudo usermod -aG video kz-anpr
sudo install -D -m 0755 build/kz_anpr /opt/kz-anpr/bin/kz_anpr
sudo install -D -m 0644 config/default.yaml /etc/kz-anpr/default.yaml
sudo install -D -m 0755 tools/setup_nomeroff_env.sh /opt/kz-anpr/tools/setup_nomeroff_env.sh
sudo install -D -m 0755 tools/nomeroff_worker.py /opt/kz-anpr/tools/nomeroff_worker.py
sudo mkdir -p /opt/kz-anpr/requirements
sudo cp requirements/nomeroff-jetson.txt /opt/kz-anpr/requirements/
sudo mkdir -p /opt/kz-anpr/models && sudo cp models/*.onnx models/*.yaml /opt/kz-anpr/models/
sudo /opt/kz-anpr/tools/setup_nomeroff_env.sh --jetson
sudo chown -R kz-anpr:kz-anpr /var/lib/kz-anpr
sudo install -D -m 0644 deploy/systemd/kz-anpr.service /etc/systemd/system/kz-anpr.service
sudo systemctl daemon-reload
sudo systemctl enable --now kz-anpr
```

Build the engine cache as the service user, or the service will rebuild it on first start:

```sh
sudo -u kz-anpr /opt/kz-anpr/bin/kz_anpr --config /etc/kz-anpr/default.yaml --warmup
```

## 10. Verify

```sh
journalctl -u kz-anpr -f
```

Expect detector `backend=tensorrt`, `event=nomeroff_ready` with the intended model/device, then
`event=camera_connected`. Drive a car up to the barrier and watch the state transitions through
to `event=plate_confirmed`.

Run the required matrix without changing the system power mode:

```sh
python3 tools/benchmark.py --config config/default.yaml --streams 1 \
    --manifest data/manifests/kz_eval.csv
python3 tools/benchmark.py --config config/default.yaml --streams 2 \
    --manifest data/manifests/kz_eval.csv
python3 tools/benchmark.py --config config/default.yaml --streams 4 \
    --manifest data/manifests/kz_eval.csv
```

The collector reads `nvpmodel -q`, `jetson_clocks --show`, and GPU metrics when available, and
samples the full process tree for CPU/RAM. It never changes the power mode or clock settings.

The periodic `event=metrics` line carries per-stage latencies and counters. Watch
`detector_avg_ms`, `ocr_avg_ms` and `frames_dropped` for the first day.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `backend_unavailable ... TensorrtExecutionProvider is not present` | ONNX Runtime build has no TensorRT provider; reinstall the Jetson build |
| First recognition takes minutes | engine cache empty or invalidated; run the warm-up tool |
| `event=camera_stream_lost` repeatedly | switch the camera to TCP, check `read_timeout_ms`, verify the pipeline with `gst-launch-1.0` |
| High `frames_dropped` | processing is slower than capture; check `detector_avg_ms` and confirm TensorRT is actually in use |
| Never leaves `IDLE` | motion ROI wrong, or `motion.quiet_threshold` too high. Run with `--timeline --log-level debug` and read the motion scores |
| Reaches `VEHICLE_NEAR` but never `VEHICLE_STOPPED` | `stop_detection` thresholds too tight for the framing; check `stationary_ms` in the state logs |
| `status=INVALID_FORMAT` on valid plates | plate layout missing from `validation.formats` |
