# Jetson Nano deployment

The production target is the original Jetson Nano Developer Kit 4 GB. Everything runs in one
pinned Docker image; the host only needs JetPack, Docker and the NVIDIA container runtime.

| Component | Pinned contract |
| --- | --- |
| Board | Jetson Nano Developer Kit 4 GB, aarch64 |
| Host | JetPack 4.6.x / L4T R32.7.x (tested on R32.7.6) |
| CUDA / TensorRT | CUDA 10.2 / TensorRT 8.2 of the host, mounted into the container by the NVIDIA runtime |
| Base image | `nvcr.io/nvidia/l4t-ml:r32.7.1-py3`, pinned by digest |
| Build | GCC 7.5, CMake, one compiler job; the unit tests run during the image build |
| Plate detector | YOLOv8n ONNX, TensorRT FP16 engine, no CPU fallback (`strict_backend`) |
| OCR | Nomeroff Net 4.0.1 `kz` checkpoint (SHA-256 pinned), exported to ONNX during the image build, TensorRT FP32 engine |
| Fallback | Microsoft ONNX Runtime 1.11.1 aarch64 CPU package (SHA-256 pinned), used only if TensorRT refuses the OCR model |

CUDA, cuDNN and TensorRT are not installed in the image. On JetPack 4 the NVIDIA container runtime
bind-mounts the host's copies read-only into every container, and into every `docker build` step
when `nvidia` is Docker's default runtime (the lists are in
`/etc/nvidia-container-runtime/host-files-for-container.d/*.csv`). dpkg cannot install those
packages over the mounts, so the image compiles against the host files instead and the host stays
unchanged.

## 1. Host setup, once

Flash JetPack 4.6.x. If CUDA or TensorRT is missing, `sudo apt-get install nvidia-jetpack` adds
them. Make `nvidia` Docker's default runtime in `/etc/docker/daemon.json`:

```json
{
    "runtimes": {
        "nvidia": {
            "path": "nvidia-container-runtime",
            "runtimeArgs": []
        }
    },
    "default-runtime": "nvidia"
}
```

```sh
sudo systemctl restart docker
docker info --format '{{.DefaultRuntime}}'     # must print: nvidia
sudo usermod -aG docker "$USER"                 # then log out and back in
```

For steady performance use the 10 W mode with fixed clocks and keep a fan on the heatsink:

```sh
sudo nvpmodel -m 0
sudo jetson_clocks
```

## 2. The plate detector

The image builds the OCR model itself. The detector, `models/license_plate_detector.onnx`, has to
be exported once on a PC with Python and `ultralytics` and copied into the checkout on the Jetson
([models/README.md](../models/README.md)). Every command below refuses to start without it, except
the camera scan, check and status, which load no model.

## 3. Build the image

```sh
make docker-build
```

The build installs the compiler, builds `kz_anpr` and `kz_anpr_benchmark` with one job, runs the
unit tests, downloads the Nomeroff checkpoint and exports it to
`/opt/kz-anpr/models/nomeroff-onnx/kz.onnx` (`tools/export_nomeroff_onnx.py`, which first checks
the export against Nomeroff's own forward pass). It uses Docker's classic builder because BuildKit
does not run build steps through the default runtime.

nomeroff.net.ua often resets long downloads to the Nano; the download resumes where it stopped, up
to 40 attempts. To skip it, copy the checkpoint into the checkout before building, for example from
a PC: `scp anpr_ocr_kz_2022_11_14.ckpt jetson:~/parking/models/nomeroff/`. A copy with the right
SHA-256 is used instead of the download.

## 4. Check

```sh
make check
```

`make check` lists the available inference backends, then loads both models exactly as a run does
and runs one inference each. It ends with one of:

- `CHECK OK: detector and OCR run on TensorRT (GPU).`
- `CHECK WARNING: the OCR fell back from TensorRT` — the OCR runs on the ONNX Runtime CPU package.
  Recognition still works, more slowly; the `backend_unavailable` line above says why.
- `CHECK FAILED` — the detector is not on TensorRT or a model did not load.

The first check builds the TensorRT engines: a few minutes for the detector, about 30 s for the
OCR. They are cached in `var/trt_cache/` under a name that includes the model's content hash and
the TensorRT version, so a changed model is rebuilt automatically and later starts take seconds.
`rm -rf var/trt_cache` removes old engines; they are rebuilt on the next start.

## 5. Test clip

```sh
make run
```

Replays `video/parking.mp4` once, frame by frame, and prints one JSON event per vehicle. The
expected plate is `152JTA02` (Almaty). `make run VIDEO=video/other.mp4` replays another clip
inside the checkout; `make bench` prints FPS and per-stage latency.

## 6. Live camera

```sh
make camera CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
make camera CAMERA=0                       # first USB / V4L2 camera
make camera CAMERA='...' CAMERA_ID=gate-02 # the camera_id written into every event
```

Typical RTSP addresses (check the camera's manual):

| Camera | Main stream |
| --- | --- |
| Hikvision | `rtsp://user:password@IP:554/Streaming/Channels/101` (`102` is the sub-stream) |
| Dahua | `rtsp://user:password@IP:554/cam/realmonitor?channel=1&subtype=0` (`subtype=1` is the sub-stream) |

Keep the URL in single quotes: `&` and `?` mean something to the shell. RTSP is read over TCP
(`camera.rtsp_tcp`), only the newest frame is processed, and a lost stream is reconnected with
backoff (`camera_stream_lost`, `camera_reconnected` in the log) without restarting the process.

The Nano decodes the stream on its CPU. Pick the smallest stream on which the plate at the stop
line is still at least 120 px wide; aimed at the stop line, 1280x720 usually is, and it costs far
less to decode than 1920x1080. [Camera setup](CAMERA_SETUP.md) covers mounting, exposure and the
ROIs.

Events are printed and appended to `var/events.jsonl`. Ctrl+C stops the run cleanly.

## 7. Run as a service

```sh
make service CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
```

`tools/install_service.sh` runs `make check`, then installs and starts the `kz-anpr` systemd
service (it asks for sudo). The service starts at boot and is restarted 10 s after any exit;
`--events-file var/events.jsonl` is always on. The camera address is stored in
`/etc/default/kz-anpr`, readable by root only because it may contain a password; logs mask it.

```sh
journalctl -u kz-anpr -f              # logs
tail -F var/events.jsonl              # recognised plates
sudo systemctl stop kz-anpr           # stop it before make run, make check or make bench
sudo systemctl start kz-anpr
sudo systemctl disable --now kz-anpr  # remove it from boot
```

Running `make service` again with another camera replaces the settings. `var/events.jsonl` is
rotated weekly by logrotate, twelve compressed weeks are kept (`/etc/logrotate.d/kz-anpr`).

A password containing `$` is expanded by make; call the script directly instead:
`tools/install_service.sh 'rtsp://user:pa$$word@...' gate-01`.

## 8. Hikvision cameras (camera mode)

Instead of one camera address, camera mode finds every Hikvision camera on the PoE switch and runs
them all in one process with one shared detector and OCR. [Hikvision cameras](CAMERAS.md) is the
full guide; on the host it needs, besides section 1:

- A permanent static address on the camera LAN (the PoE switch has no DHCP). An `ip addr add` is
  wiped by NetworkManager; `tools/camera_lan_setup.sh` adds a NetworkManager profile that never
  takes the default route, so the GSM modem keeps Internet traffic.
- JetPack 4.6's own `nvidia-container-toolkit 1.7.0`: a newer toolkit breaks hardware decoding in
  containers (troubleshooting below).

```sh
cp config/cameras.env.example config/cameras.env && chmod 600 config/cameras.env  # camera login
make camera-lan-setup ADDRESS=192.168.10.5/24                   # dry run: the nmcli commands
make camera-lan-setup ADDRESS=192.168.10.5/24 LAN_ARGS=--apply  # once
make camera-scan                                                # what is on the switch
make camera-check                                               # every camera READY?
make run-cameras                                                # foreground, Ctrl+C stops it
make camera-service                                             # at boot
make camera-status                                              # any time
```

`make camera-service` runs `make check` and the camera check first, refuses to install while a
camera rejects the login (Hikvision locks an address out after a few failed logins), and installs
`deploy/systemd/kz-anpr-cameras.service` under the same `kz-anpr` name as `make service`: only one
ANPR process fits on the 4 GB Nano, so either mode replaces the other. The camera credentials go to
`/etc/default/kz-anpr`, readable by root only; the service uses only that file, so run
`make camera-service` again after changing a password. Events go to `var/events.jsonl` as before,
each with its `camera_id`; logs are in `journalctl -u kz-anpr -f`.

The camera image adds the GStreamer development files to the build stage, so `kz_anpr` drives the
NVIDIA decoder directly. An image built before camera mode is refused with
`image ... was built from an older checkout`: run `make docker-build`.

## Updating

```sh
git pull
make docker-build
make check
sudo systemctl restart kz-anpr   # when the service is installed
```

## Troubleshooting

| Message | Meaning and fix |
| --- | --- |
| `Docker's default runtime must be nvidia` | Set `"default-runtime": "nvidia"` (section 1) and restart Docker |
| `JetPack CUDA/TensorRT files ... are missing` | `sudo apt-get install nvidia-jetpack` |
| `image ... is missing or was built from an older checkout` | `make docker-build` |
| `models/license_plate_detector.onnx is missing` | Copy the detector into `models/` (section 2) |
| `fetch-resumable: ... failed` during the build | Copy the checkpoint into `models/nomeroff/` (section 3) |
| `CHECK WARNING: the OCR fell back from TensorRT` | See the `backend_unavailable` line; recognition works on the CPU, slower |
| `model_load_failed`, exit code 4 | A model file is missing or unreadable; the `reason` field names it |
| `camera_unavailable`, exit code 3 | A video file that cannot be opened. Cameras and RTSP streams are retried instead (`camera_reconnect_failed`): check the address in VLC from another machine |
| `permission denied ... docker.sock` | Your user is not in the `docker` group yet (section 1) |
| Out of memory, very slow start | Only one ANPR process fits on the 4 GB Nano: stop the service before running `make` targets |
| `CAMERA_SUBNET_UNCONFIGURED`, `CAMERA_LAN_NOT_FOUND` | eth0 has no address or no link: `make camera-lan-setup LAN_ARGS=--show`, then section 8 |
| `RTSP_AUTH_FAILED` | Check the login in `config/cameras.env` (no quotes); wait 30 min if the camera locked itself ([Hikvision cameras](CAMERAS.md#credentials)) |
| `HARDWARE_DECODER_UNAVAILABLE`, `It isn't a v4l2 driver` | nvidia-container-toolkit is newer than JetPack 4.6's 1.7.0: downgrade it and `apt-mark hold` it |
| `config/cameras.env ... every user can read it` | `chmod 600 config/cameras.env` |
| Any other upper-case camera code | Its meaning and action are in the [diagnostic table](CAMERAS.md#diagnostic-codes) |
