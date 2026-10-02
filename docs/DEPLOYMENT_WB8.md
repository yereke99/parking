# Wiren Board 8 Deployment

Secondary target. The primary production target is now
[Jetson Orin Nano](DEPLOYMENT_JETSON.md); this page covers the CPU-only ARM64 path.

Target: ARM64/AArch64 Debian on Wiren Board 8, Cortex-A53 CPU only.

Set `inference.backend: onnx_cpu` and expect detector latency well above the Jetson figures. A
Cortex-A53 has no vector performance comparable to the development machine used for the numbers
in [benchmarks](../benchmarks/README.md), so measure on the device before committing to it.

The default Nomeroff/PyTorch OCR path has not been benchmarked on this small CPU-only board and may
not be practical. ONNX Runtime is still required for the detector. If the legacy Fast Plate OCR
comparison backend is explicitly selected, it also requires ONNX Runtime for its uint8 input.
Install the aarch64 CPU build alongside `libopencv-dev` and measure before deployment.

## Packages

Install build and runtime dependencies:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libopencv-dev v4l-utils
```

Optional tools for profiling:

```sh
sudo apt install -y linux-perf htop sysstat
```

## User And Permissions

Run as a dedicated non-root user:

```sh
sudo useradd --system --home /var/lib/kz-anpr --create-home --shell /usr/sbin/nologin kz-anpr
sudo usermod -aG video kz-anpr
```

Use root only if a specific camera driver or GPIO integration later requires it.

## Layout

Suggested production paths:

```text
/opt/kz-anpr/bin/kz_anpr
/etc/kz-anpr/default.yaml
/opt/kz-anpr/models/license_plate_detector.onnx
/opt/kz-anpr/models/plate_ocr.onnx
/opt/kz-anpr/models/plate_ocr_config.yaml
/var/lib/kz-anpr/debug_frames
```

Keep debug storage disabled by default.
If `debug.save_frames` or `debug.save_crops` is enabled, keep `debug.max_files` set to a small positive cap and monitor `/var/lib/kz-anpr`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Use `-j2` initially on the target to reduce memory pressure during compile.

## Install

```sh
sudo install -d /opt/kz-anpr/bin /opt/kz-anpr/models /etc/kz-anpr
sudo install -m 0755 build/kz_anpr /opt/kz-anpr/bin/kz_anpr
sudo install -m 0644 config/default.yaml /etc/kz-anpr/default.yaml
sudo install -m 0644 models/license_plate_detector.onnx /opt/kz-anpr/models/license_plate_detector.onnx
sudo install -m 0644 models/kz_plate_ocr.onnx /opt/kz-anpr/models/plate_ocr.onnx
/opt/kz-anpr/models/plate_ocr_config.yaml
sudo chown -R kz-anpr:kz-anpr /var/lib/kz-anpr
```

Update `/etc/kz-anpr/default.yaml` model paths to the installed `/opt/kz-anpr/models` paths.

## systemd

```sh
sudo install -m 0644 deploy/systemd/kz-anpr.service /etc/systemd/system/kz-anpr.service
sudo systemctl daemon-reload
sudo systemctl enable --now kz-anpr.service
```

Logs:

```sh
journalctl -u kz-anpr.service -f
```

Restart after update:

```sh
sudo systemctl restart kz-anpr.service
```

Rollback is simply replacing the binary/model/config with the previous known-good files and restarting the service.

## Benchmark Mode

```sh
./build/kz_anpr_benchmark \
  --video video/car.mp4 \
  --ground-truth data/manifests/gate_01_events.csv \
  --config config/default.yaml
```

Ground-truth scoring is scaffolded but not complete because no labeled event CSV exists in the repository yet.

## Reliability Checks

Before field use, run a soak test:

```sh
systemd-run --user --scope ./build/kz_anpr --config config/default.yaml --camera 0
```

Monitor:

```sh
pidstat -rud -p "$(pidof kz_anpr)" 5
journalctl -u kz-anpr.service --since "1 hour ago"
```

Verify:

- no steady RSS growth;
- no frame-decoding failure loop;
- camera reconnect logs are bounded;
- CPU stays within thermal limits;
- debug storage remains empty unless explicitly enabled.
