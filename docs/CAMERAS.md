# Hikvision cameras (camera mode)

Camera mode runs the ANPR on every Hikvision IP camera on the PoE switch at once. `kz_anpr` finds
the cameras by itself, gives each a stable id, checks login, stream and decoder for each one, and
then runs one recognition pipeline per camera on one shared plate detector and one shared OCR. A
camera that is down, misconfigured or rejects the password is reported and retried; the others
keep working. Every event carries the `camera_id` of the camera that saw the plate.

The reference camera is the DS-TCG406-E (4 MP ANPR entrance camera); any Hikvision camera with
RTSP works, and plain RTSP cameras from other makers can be listed by address. The video-file
commands (`make run`, `make run-videos`, `make bench`) and the single-camera `make camera` are
unchanged.

## Topology

```text
  DS-TCG406-E     DS-TCG406-E     DS-TCG406-E     DS-TCG406-E     static addresses,
  192.168.10.21   192.168.10.22   192.168.10.23   192.168.10.24   powered over PoE
        |               |               |               |
  +-----+---------------+---------------+---------------+-----+
  |            unmanaged PoE switch (no DHCP server)          |
  +-----------------------------+-----------------------------+
                                | uplink port
                     eth0  192.168.10.5/24   NetworkManager profile kz-camera-lan:
                                |            no gateway, never the default route
                  +-------------+-------------+
                  |      Jetson Nano 4 GB     |
                  |   Docker --network host   |
                  +-------------+-------------+
                                | USB
                     GSM modem (wwan0, ppp0 or eth1)  ->  Internet: the only default route
```

- The switch hands out no addresses: the Jetson and every camera need a static address in one
  subnet. 192.168.10.0/24 is used throughout this guide; any private subnet that no other
  interface uses works.
- Camera traffic stays on eth0. Internet traffic uses the modem. Nothing routes between them, and
  nothing here ever changes a route.
- Recognition needs no Internet. A modem that is offline is reported and ignored.

## Quick start

1. **Credentials.** One login for all cameras, kept out of Git:

   ```sh
   cp config/cameras.env.example config/cameras.env
   chmod 600 config/cameras.env
   nano config/cameras.env          # HIKVISION_USERNAME=admin, HIKVISION_PASSWORD=...
   ```

2. **Image and models**, once and after every `git pull`:

   ```sh
   make docker-build
   make check
   ```

3. **Camera LAN address**, once, when the Jetson has no address in the cameras' subnet yet
   (`make camera-scan` then reports `CAMERA_SUBNET_UNCONFIGURED`):

   ```sh
   make camera-lan-setup ADDRESS=192.168.10.5/24                    # shows what it would do
   make camera-lan-setup ADDRESS=192.168.10.5/24 LAN_ARGS=--apply   # does it
   ```

4. **Find and check the cameras:**

   ```sh
   make camera-scan     # network, Internet, every camera found; sends no password
   make camera-check    # per camera: login, stream, codec, decoder, frames; READY or not
   ```

5. **Run**, in the foreground first, then as the boot service:

   ```sh
   make run-cameras     # Ctrl+C stops it; events also go to var/events.jsonl
   make camera-service  # kz-anpr systemd service, replaces a single-camera `make service`
   make camera-status   # status table, any time
   ```

`config/cameras.yaml` holds every camera-mode setting, each documented in place; it holds no
secrets. Another file: `make camera-check CAMERA_CONFIG=config/site.yaml`. Extra `kz_anpr`
options go in `RUN_ARGS`, for example `make run-cameras RUN_ARGS='--log-level debug'`.

## What each command prints

The layouts below are abridged examples.

**`make camera-scan`** prints the camera LAN and the Internet uplink, then every device that looks
like a camera, then the problems found. It never sends a password.

```text
CAMERA NETWORK
  interface: eth0 (r8168, pci)
  link: UP
  ip: 192.168.10.5/24
  subnet: 192.168.10.0/24
INTERNET
  interface: wwan0 (cellular, qmi_wwan)
  default route: GSM via wwan0 (gateway 10.64.64.64, metric 700)
  status: ONLINE (1.1.1.1:53 in 212 ms)

DISCOVERED CAMERAS
  camera-01  192.168.10.21  44:19:b6:3a:10:21  Hikvision DS-TCG406-E  sadp,arp
  camera-02  192.168.10.22  44:19:b6:3a:10:22  Hikvision DS-TCG406-E  sadp,arp
  camera-03  192.168.1.64   44:19:b6:3a:10:23  Hikvision DS-TCG406-E  sadp

INFO ONVIF_DISABLED_OR_UNAVAILABLE  camera-01 192.168.10.21
  The camera is reachable but did not answer ONVIF (disabled by default on Hikvision). Not needed: RTSP is used.
  action: Optional: enable ONVIF and create an ONVIF user in the camera web UI (Network > Advanced > Integration Protocol).
ERROR CAMERA_ON_OTHER_SUBNET  camera-03 192.168.1.64
  The camera answered discovery but its IP is outside the Jetson's camera subnet, so it cannot be reached over the camera LAN.
  action: Change the camera IP into the camera subnet, or add an address in its subnet to the Jetson (make camera-lan-setup).
```

**`make camera-check`** does the scan, then logs in to each camera once over RTSP, opens the
stream with the decoder this container would use and reads frames for
`capture.check_duration_ms` (3 s). One block per camera; exit code 0 only when every enabled
camera is READY, 3 otherwise.

```text
CAMERA camera-01
  IP: 192.168.10.21   MAC: 44:19:b6:3a:10:21   Vendor: Hikvision   Model: DS-TCG406-E
  RTSP: OK   Stream: main   URL: rtsp://<redacted>@192.168.10.21:554/Streaming/Channels/101
  Codec: H.264   Resolution: 2688x1520   FPS: 25.0   First frame: 430 ms
  Decoder: nvidia_hardware
  STATUS: READY

CAMERA camera-02
  IP: 192.168.10.22   MAC: 44:19:b6:3a:10:22   Vendor: Hikvision   Model: DS-TCG406-E
  RTSP: AUTH   Stream: main   URL: rtsp://<redacted>@192.168.10.22:554/Streaming/Channels/101
  ERROR RTSP_AUTH_FAILED  camera-02 192.168.10.22
    The camera rejected the configured username/password.
    action: Verify HIKVISION_USERNAME / HIKVISION_PASSWORD (or the per-camera variables); repeated failures lock the camera for 30 minutes.
  STATUS: NOT READY
```

**`make run-cameras`** logs the network block and the cameras it starts on stderr, then prints one
JSON event per vehicle on stdout (and appends it to `var/events.jsonl`), exactly as for a video
file, with the camera's id:

```json
{"event":"plate_recognition","status":"VALID_HIGH_CONFIDENCE","normalized_plate":"152JTA02","camera_id":"camera-01","confidence":0.8309,"time":"2026-10-09T07:12:03.120Z","...":"..."}
```

Logs go to stderr as `key=value` lines; every line about a camera carries `camera_id=`, and every
problem carries `error=<CODE>` and `action="..."` from the table below.

**`make camera-status`** reads `var/cameras/status.json`, which a running `run-cameras` or the
service rewrites every `runtime.status_interval_ms` (5 s). With nothing running it probes the
registered cameras instead (reachability, RTSP, login) and shows ANPR as `STOPPED`. It loads no
model, so it is safe to run next to the service.

```text
updated 2026-10-09T10:15:05Z  camera LAN: eth0 192.168.10.5/24  Internet: wwan0 ONLINE
CAMERA     IP             LINK  RTSP  VIDEO  ANPR          ERROR
camera-01  192.168.10.21  OK    OK    H264   RUNNING
camera-02  192.168.10.22  OK    AUTH  -      ERROR         RTSP_AUTH_FAILED
camera-03  192.168.10.23  OK    OK    H265   RUNNING
camera-04  192.168.10.24  DOWN  DOWN  -      RECONNECTING  CAMERA_UNREACHABLE
```

`LINK` is OK, DOWN or `-`; `RTSP` is OK, AUTH, NOCRED, PORT, PATH, DOWN or ERROR; `ANPR` is
RUNNING, STARTING, RECONNECTING, STALLED, ERROR, DISABLED, OFFLINE, STANDBY (over the camera
limit) or STOPPED. The status file also holds the per-camera numbers of
[performance monitoring](#performance-monitoring).

## Diagnostic codes

Every problem is reported with one of these codes: in the terminal reports, in the log
(`error=`), in the status file and in the status table.

| Code | Severity | Meaning | Action |
| --- | --- | --- | --- |
| `CAMERA_LAN_NOT_FOUND` | error | No wired Ethernet interface has link: the Jetson is not connected to the PoE switch. | Check the cable from the Jetson Ethernet port to the switch uplink port and that the switch is powered. |
| `CAMERA_SUBNET_UNCONFIGURED` | error | The Ethernet link to the PoE switch is up but the Jetson has no usable IPv4 address on it (the switch has no DHCP server). | Give the Jetson a static address in the cameras' subnet: make camera-lan-setup ADDRESS=<ip>/24. |
| `CAMERA_LAN_LINK_DOWN` | error | The camera LAN interface lost its link while running. | Check the Jetson-to-switch cable and the switch power; cameras reconnect automatically. |
| `CAMERA_LAN_HAS_DEFAULT_ROUTE` | warning | The camera LAN interface carries the default route, so Internet traffic goes to the PoE switch instead of the GSM modem. | Set ipv4.never-default on the camera LAN connection (make camera-lan-setup) and remove its gateway. |
| `SUBNET_CONFLICT` | warning | Another interface (a Docker bridge, the USB gadget or the modem) uses a subnet that overlaps the camera LAN. | Move the cameras to a non-overlapping subnet or set Docker's default-address-pools. |
| `NO_INTERNET_ROUTE` | info | There is no default route, so no Internet uplink is configured. Camera processing does not need it. | Connect the USB GSM modem if events must be sent upstream. |
| `INTERNET_OFFLINE` | warning | A default route exists but the Internet probe failed. Camera processing continues. | Check the GSM modem signal, SIM card and APN. |
| `NO_CAMERAS_DISCOVERED` | error | The camera LAN is up, but no Hikvision/ONVIF/RTSP device answered. | Check camera PoE power and cabling; list known camera IPs under discovery.manual_hosts. |
| `DUPLICATE_IP_DETECTED` | error | More than one device answers for the same IP address. | Give each camera a unique IP (Hikvision SADP tool or web UI); connect factory-reset cameras one at a time. |
| `CAMERA_ON_OTHER_SUBNET` | error | The camera answered discovery but its IP is outside the Jetson's camera subnet, so it cannot be reached over the camera LAN. | Change the camera IP into the camera subnet, or add an address in its subnet to the Jetson (make camera-lan-setup). |
| `CAMERA_NOT_ACTIVATED` | error | The Hikvision camera is not activated (factory state, no admin password) and will not stream. | Activate it with Hikvision SADP or its web page and set a strong admin password. |
| `ONVIF_DISABLED_OR_UNAVAILABLE` | info | The camera is reachable but did not answer ONVIF (disabled by default on Hikvision). Not needed: RTSP is used. | Optional: enable ONVIF and create an ONVIF user in the camera web UI (Network > Advanced > Integration Protocol). |
| `CAMERA_LIMIT_REACHED` | warning | More cameras were found than runtime.max_active_cameras; the extra cameras are not processed. | Raise runtime.max_active_cameras if the Jetson has headroom, or disable cameras in config/cameras.yaml. |
| `CAMERA_DISABLED` | info | The camera is disabled in config/cameras.yaml. | Set enabled: true for this camera to process it. |
| `CAMERA_UNREACHABLE` | error | The camera was discovered but does not answer on the network now. | Check its PoE port, cable and power; it may be rebooting. |
| `RTSP_PORT_CLOSED` | error | The camera responds on the network, but nothing listens on its RTSP port. | Enable RTSP or check the RTSP port in the camera (Network > Advanced > Port), or set rtsp_port. |
| `RTSP_CREDENTIALS_MISSING` | error | The camera requires a login but no credentials are configured. | Set HIKVISION_USERNAME and HIKVISION_PASSWORD (config/cameras.env). |
| `RTSP_AUTH_FAILED` | error | The camera rejected the configured username/password. | Verify HIKVISION_USERNAME / HIKVISION_PASSWORD (or the per-camera variables); repeated failures lock the camera for 30 minutes. |
| `RTSP_STREAM_PATH_INVALID` | error | Authentication works but the requested stream does not exist. | Check rtsp.stream / rtsp_path (Hikvision main stream 101, sub stream 102). |
| `RTSP_PROTOCOL_ERROR` | error | The RTSP port answered with something that is not a valid RTSP response. | Check that the configured port is the camera's RTSP port. |
| `ISAPI_UNAVAILABLE` | info | The camera's ISAPI web service did not answer, so model/serial details may be missing. | Optional: check the HTTP port; streaming does not need ISAPI. |
| `UNSUPPORTED_CODEC` | error | The stream codec cannot be decoded with this Jetson setup. | Set the camera stream to H.264 or H.265 in its Video settings. |
| `STREAM_OPEN_FAILED` | error | RTSP works, but the decoder could not open the video. | Run make camera-check to see each decoder's error; try decode.decoder: software. |
| `NO_FRAMES_RECEIVED` | error | The stream opened but no video frames arrived within the timeout. | Check the camera's stream settings and bitrate, or reboot the camera. |
| `STREAM_TIMEOUT` | warning | The stream stopped delivering frames (camera reboot, PoE loss or network stall); reconnecting. | Nothing if it recovers; otherwise check the camera power and cable. |
| `STREAM_ENDED` | warning | The camera closed the stream (EOF or connection reset); reconnecting. | Nothing if it recovers; check the camera log if it repeats. |
| `HARDWARE_DECODER_UNAVAILABLE` | warning | NVIDIA hardware decoding is not available in this container; decoding falls back to the CPU. | Run the image with --runtime nvidia on JetPack 4.6 (keep nvidia-container-toolkit 1.7.0). |
| `TENSORRT_NOT_AVAILABLE` | error | Video works but the AI inference backend (TensorRT) could not start. | Run make check; rebuild the image with make docker-build if TensorRT is missing. |
| `OUT_OF_MEMORY_RISK` | warning | Available Jetson RAM is below the safe threshold. | Stop other processes, use the sub-stream, or reduce the number of active cameras. |
| `NATIVE_ANPR_UNAVAILABLE` | info | The camera's own ANPR events are not available (not supported, not enabled, or rejected). | Optional: enable ANPR on the camera, or set native_anpr.enabled: false. |

Exit codes of the camera commands: 0 success, 1 unexpected error, 2 configuration error, 3 camera
or camera LAN problem, 4 model or inference backend unavailable.

## Credentials

Passwords never go into `config/cameras.yaml`, a command line or a log. They come from
environment variables:

| Variables | Used for |
| --- | --- |
| `HIKVISION_USERNAME`, `HIKVISION_PASSWORD` | every camera |
| `HIKVISION_USERNAME_CAMERA_02`, `HIKVISION_PASSWORD_CAMERA_02` | camera `camera-02` only: the id upper-cased, `-` as `_` |
| the names in a camera's `username_env` / `password_env` | that camera, under names you choose |
| `ONVIF_USERNAME`, `ONVIF_PASSWORD` | optional, ONVIF device details only |

Put them in `config/cameras.env` (ignored by Git and by the Docker build context). Docker reads
that file with `--env-file`, which takes each line literally: write `KEY=value` with no quotes, no
spaces around `=` and no `export`. `tools/jetson_docker.sh` warns about quoted values and about a
file other users can read (`chmod 600 config/cameras.env`). Exported shell variables with these
names work too and win over the file; `tools/jetson_docker.sh` passes them to the container by
name, so their values never appear in `ps`. `CAMERA_ENV_FILE=...` points at another file.

`make camera-service` takes the credentials from the environment, else from
`config/cameras.env`, else asks for them, runs the camera check with exactly those values, and
writes them to `/etc/default/kz-anpr` (root only, mode 0600). The service reads only that file:
after changing a password, run `make camera-service` again.

**Lockout.** Hikvision blocks an address for 30 minutes after about 5 failed logins (Illegal Login
Lock, on by default), and RTSP, the web page and ISAPI are treated as one budget. So every login is
tried once: discovery never sends a password, `camera-check` logs in once per camera, a rejected
login is never retried at once, and a running process retries it only every 15 minutes, at most
twice, then gives that camera up until it is restarted (`camera-status` shows ERROR /
`RTSP_AUTH_FAILED`). ISAPI is only called with credentials after RTSP accepted them. Fix the
password before restarting anything; each restart of a process with a wrong password spends up to
three attempts. `make camera-service` refuses to install while a camera rejects the login. A
locked camera unlocks after 30 minutes or a reboot (cut its PoE).

## Camera ids

Each physical camera gets a stable id the first time it is seen: `camera-01`, `camera-02`, ...,
the lowest free number. The ids live in `var/cameras/registry.yaml` (no secrets), which every scan
updates:

```yaml
cameras:
  - id: "camera-01"
    mac: "44:19:b6:3a:10:21"
    serial: "DS-TCG406-E20250101AAWRK12345678"
    device_id: "6aff4000-a6a7-11b2-8f5a-4419b63a1021"
    onvif_uuid: ""
    model: "DS-TCG406-E"
    last_ip: "192.168.10.21"
    first_seen: "2026-10-09T09:58:12Z"
    last_seen: "2026-10-09T10:15:00Z"
```

A camera is recognised by the strongest identifier available: MAC address, then serial number,
then Hikvision device ID, then ONVIF UUID, and the IP address only for an entry with nothing
stronger. A camera that moves to another IP keeps its id; a different camera that takes over an
old address does not inherit the old id (its MAC or serial differs). Ids of cameras that are
offline stay reserved.

To rename a camera (`gate-entry` instead of `camera-01`): stop the service, edit the `id:` in
`var/cameras/registry.yaml` (the container writes it as root: `sudo nano ...`), rename that id in
`config/cameras.yaml` overrides and in per-camera variables (`HIKVISION_PASSWORD_GATE_ENTRY`),
then start again. Ids use letters, digits, `-` and `_`. Deleting the file renumbers every camera
on the next scan.

## Per-camera settings

`cameras:` in `config/cameras.yaml` matches a camera by `id`, `mac`, `serial` or `ip` and
overrides any of: `enabled`, `rtsp_port`, `stream` (main or sub), `rtsp_path`, `http_port`,
`username_env` / `password_env`, `decoder`, `anpr_config` (its own ROIs and thresholds; models
stay shared) and `native_anpr`. Cameras that discovery cannot find (another maker, ONVIF and SADP
off) go into `discovery.manual_hosts` as `192.168.10.30` or `192.168.10.30:8554`.

## ONVIF

ONVIF is optional. Hikvision firmware 5.5 and later ships with ONVIF disabled, and ONVIF uses its
own user accounts, separate from the camera's admin login. Discovery does not depend on it: SADP
(Hikvision's own discovery, UDP 37020), the ARP table, manual hosts and, when those find nothing,
a TCP sweep of the subnet find the cameras; video comes over RTSP. A camera without ONVIF is
reported as `ONVIF_DISABLED_OR_UNAVAILABLE` (info) and works normally.

To use it: in the camera web UI enable ONVIF (Configuration > Network > Advanced Settings >
Integration Protocol), add an ONVIF user there, and set `ONVIF_USERNAME` / `ONVIF_PASSWORD`. It
then also answers WS-Discovery (UDP 3702) and reports its model and serial over ONVIF.

## Decoding

`decode.decoder` in `config/cameras.yaml`:

| Value | Meaning |
| --- | --- |
| `auto` (default) | NVIDIA hardware decoding when this container can do it, else the CPU |
| `nvidia_hardware` | hardware only; a camera it cannot open is reported, not degraded |
| `software` | CPU decoding (GStreamer's libav decoders or FFmpeg) |

The hardware path is GStreamer in the image, driven directly by `kz_anpr`:
`rtspsrc` (RTSP over TCP) -> `rtph264depay`/`rtph265depay` -> `h264parse`/`h265parse` ->
`nvv4l2decoder` (NVDEC) -> `nvvidconv` (scales on the VIC) -> I420 frames -> `appsink`. The frames
arrive as I420 and only the frames the pipeline actually processes are converted to BGR on the
CPU; frames that are dropped as stale cost no conversion. Reads have a timeout, so a camera that
goes silent (PoE cable pulled) becomes `STREAM_TIMEOUT` within `capture.read_timeout_ms` instead of
hanging. `camera-check` prints the decoder each camera got and why any earlier choice failed.
H.264 and H.265 can be mixed; each camera's codec comes from its stream description.

**Capacity.** NVDEC on the Nano decodes about 500 megapixels per second. Four DS-TCG406-E main
streams at 2688x1520 and 25 fps are 4 x 2688 x 1520 x 25 = 409 MP/s, about 82 % of it: possible
with `sudo nvpmodel -m 0` and `sudo jetson_clocks`, but with little headroom. The CPU (four
Cortex-A57 cores) is the tighter limit: colour conversion, motion check and tracking of 4 MP frames
for four cameras. For four cameras the sub-stream (`rtsp.stream: sub`, up to 1920x1080 on the
DS-TCG406-E, 4 x 1080p25 = 207 MP/s) is usually the better choice, as long as the plate at the
stop line stays at least 120 px wide (see [camera setup](CAMERA_SETUP.md)). `decode.max_width`
scales wider streams down on the VIC before conversion; `decode.max_fps` caps the frame rate
before conversion.

Camera settings that help: H.264 or H.265 (not MJPEG), 25 fps, H.264+/H.265+ off (they stretch
the key-frame interval, which delays the first frame after every reconnect), an I-frame interval
of 25-50.

## Live cameras versus video files

A file is processed frame by frame: the capture thread waits for the pipeline, nothing is
skipped, and `timestamp_ms` is the position in the clip. A camera does not wait: its capture
thread keeps only the newest frames in a bounded queue (`capture.queue_size`, default 1). When the
queue is full the oldest frame is dropped and counted (`live_dropped`), and a frame older than
`capture.max_frame_age_ms` (1.5 s) when processing picks it up is dropped as stale
(`stale_dropped`). There is no queue that can grow, so latency stays bounded however slow the
pipeline gets; at a barrier the newest view of the car is what matters. `timestamp_ms` is a
monotonic clock for cameras.

## Several cameras in one process

```text
camera-01  capture thread -> processing thread --+
camera-02  capture thread -> processing thread --+--> one TensorRT detector (calls serialized)
camera-03  capture thread -> processing thread --+--> one Nomeroff KZ OCR (calls serialized)
camera-04  capture thread -> processing thread --+--> events tagged with camera_id

capture thread:     RTSP, decoding, latest-frame queue, reconnecting
processing thread:  motion, tracking, stop detection, state machine, OCR vote
```

- **One copy of each model.** The detector and the OCR are loaded once and shared; their calls
  are serialized, which bounds GPU memory on the 4 GB board. Each extra camera costs its decoder
  buffers, its frame queue and its per-camera state, not another model.
- **Isolated cameras.** Motion, tracking, stop detection, the state machine and the vote are per
  camera. A camera that disconnects, stalls or fails only affects its own threads; the others
  keep their turns on the GPU. Events from different cameras never interleave in the output.
- **Limits.** At most `runtime.max_active_cameras` (4) are processed; extra cameras are listed as
  STANDBY (`CAMERA_LIMIT_REACHED`). Below `runtime.min_available_ram_mb` (300 MB) of available RAM
  the run logs `OUT_OF_MEMORY_RISK`.
- **Late cameras.** Discovery runs again every `discovery.rescan_interval_ms` (5 min), so a camera
  powered up later is added without a restart.

## Reconnecting

| Failure | Retry |
| --- | --- |
| Network and stream problems: `CAMERA_UNREACHABLE`, `STREAM_TIMEOUT`, `STREAM_ENDED`, `NO_FRAMES_RECEIVED`, `RTSP_PORT_CLOSED`, a lost LAN link | after 1 s, 2 s, 4 s ... up to 30 s, forever |
| Login: `RTSP_AUTH_FAILED`, `RTSP_CREDENTIALS_MISSING` | after 15 min, at most twice, then the camera is given up until the process restarts |
| Camera configuration: `RTSP_STREAM_PATH_INVALID`, `UNSUPPORTED_CODEC`, `CAMERA_NOT_ACTIVATED`, `CAMERA_ON_OTHER_SUBNET`, `DUPLICATE_IP_DETECTED` | every 2 min, forever |

A successful connection resets the backoff. The times are `capture.*` settings. A camera that is
down is logged when it fails and when the error changes, not on every attempt.

## Network edge cases

| Situation | What happens | What to do |
| --- | --- | --- |
| No DHCP on the PoE switch | The Jetson's eth0 gets no address: `CAMERA_SUBNET_UNCONFIGURED`. New Hikvision cameras look for DHCP, then fall back to 192.168.1.64 | `make camera-lan-setup ADDRESS=192.168.10.5/24`; give every camera a static address |
| Static addresses | Recommended: Jetson 192.168.10.5/24, cameras 192.168.10.21 and up, mask 255.255.255.0 | Camera gateway: leave empty or any unused address; cameras never need the Internet |
| Camera in another subnet | SADP works at layer 2, so its reply can still arrive (see rp_filter below): `CAMERA_ON_OTHER_SUBNET`. The camera is never contacted, because its address would route out through the modem | Re-address the camera, or temporarily add an address in its subnet: `LAN_ARGS='--address 192.168.10.5/24 --address 192.168.1.10/24 --apply'` |
| Two factory-reset cameras at 192.168.1.64 | Two MACs answer ARP for one IP: `DUPLICATE_IP_DETECTED`; RTSP to that address would flip between them | Connect new cameras one at a time, activate and re-address each before the next |
| Link up, no IPv4 on eth0 | `CAMERA_SUBNET_UNCONFIGURED`. An address added with `ip addr add` is wiped by NetworkManager at its next DHCP attempt or cable replug | `make camera-lan-setup` (a NetworkManager profile survives both) |
| GSM modem under another name | Interfaces are classified from sysfs (driver, USB vendor), not by name: a HiLink modem as `eth1` or a modem renumbered after a replug is still the uplink, never the camera LAN | Nothing |
| Docker networks | Docker takes 172.17-31.x and then 192.168.0.0/20 (covering 192.168.1.x and 192.168.10.x) for its networks; an overlap gives `SUBNET_CONFLICT` | Keep cameras out of 172.16.0.0/12; pin Docker's pools (below) |
| Modem subnet equals the camera subnet | Some modems use 192.168.1.0/24 (Alcatel), 192.168.8.0/24 (Huawei HiLink): `SUBNET_CONFLICT`; `camera-lan-setup` refuses an overlapping address | Use another camera subnet |
| Internet up, camera LAN down | `CAMERA_LAN_NOT_FOUND` / `CAMERA_LAN_LINK_DOWN`; cameras are retried; Internet is unaffected | Check the Jetson-to-switch cable and switch power |
| Camera LAN up, GSM down | `NO_INTERNET_ROUTE` (info) or `INTERNET_OFFLINE` (warning); recognition continues normally | Check SIM, signal and APN when events must go upstream |
| eth0 carries the default route | `CAMERA_LAN_HAS_DEFAULT_ROUTE`: Internet traffic goes to the switch | `make camera-lan-setup` (never-default, no gateway) and remove the gateway from any other eth0 profile |
| Multicast and Docker | SADP and WS-Discovery are multicast on the camera LAN, and ARP probes need raw sockets: they work only in the host network namespace | Always run through `tools/jetson_docker.sh` (it uses `--network host`) |
| Mixed H.264 and H.265 | Each camera's codec is read from its stream; each gets its own decoder pipeline | Nothing; MJPEG is not supported (`UNSUPPORTED_CODEC`) |
| Different RTSP ports | Port 554 by default | `rtsp_port` in a camera override, or `IP:PORT` in `discovery.manual_hosts` |
| rp_filter drops replies | Ubuntu's strict reverse-path filter (`rp_filter=1`) drops replies from a camera outside eth0's subnets, so a camera in a foreign subnet stays invisible even to SADP | Add an address in its subnet as above, or `sudo sysctl -w net.ipv4.conf.all.rp_filter=2` (loose; persist in `/etc/sysctl.d/`) |

Docker's address pools, in `/etc/docker/daemon.json` next to the runtime settings, then
`sudo systemctl restart docker`:

```json
"default-address-pools": [{"base": "172.17.0.0/16", "size": 24}]
```

`make camera-lan-setup` (`tools/camera_lan_setup.sh`, run on the Jetson itself, not in Docker)
prints the interfaces, NetworkManager's connections, the default routes and the reverse-path
filter, and the exact `nmcli` commands, without changing anything. With `LAN_ARGS=--apply` it
asks for confirmation, adds the profile `kz-camera-lan` (manual address, `ipv4.never-default`,
no gateway, no DNS, autoconnect priority 100) and activates it; if the default route changed, it
takes the profile down and deletes it again. It refuses the modem, Wi-Fi, bridges and the USB
gadget, an interface that carries the default route, and an address that overlaps another
interface's subnet. `LAN_ARGS=--show` only prints; `LAN_ARGS=--remove` deletes the profile. An SSH
session over eth0 may drop while the profile is applied.

### Preparing new cameras

New Hikvision cameras are not activated: they have no password and do not stream
(`CAMERA_NOT_ACTIVATED`). Connect one new camera at a time. Give the Jetson a second address in
the factory subnet, then reach the camera's web page from a laptop through an SSH tunnel:

```sh
make camera-lan-setup ADDRESS=192.168.10.5/24,192.168.1.10/24 LAN_ARGS=--apply
ssh -L 8080:192.168.1.64:80 user@jetson     # on the laptop, then open http://localhost:8080
```

Set the admin password (activation), then in Configuration > Network > Basic Settings > TCP/IP
turn DHCP off and set the address (192.168.10.21, 255.255.255.0). Hikvision's SADP tool on a
Windows laptop plugged into the switch does the same. When every camera is re-addressed, go back
to one address: `LAN_ARGS=--remove`, then the setup with `ADDRESS=192.168.10.5/24` only.

## Hikvision's own ANPR (optional)

The DS-TCG406-E reads plates itself. With `native_anpr.enabled: true` (or `native_anpr: true` on
one camera), `kz_anpr` also follows the camera's ISAPI event stream
(`/ISAPI/Event/notification/alertStream`) and writes each plate the camera reports as a
`hikvision_anpr` event next to the pipeline's own `plate_recognition` events, with the same
`camera_id`. It is off by default and meant for comparison only: barrier logic must keep using
`plate_recognition`. The camera's country code 30 is Kazakhstan (select Kazakhstan in the camera's
ANPR region settings). It needs ANPR enabled on the camera and the same login as RTSP; it connects
only after RTSP accepted the credentials. `NATIVE_ANPR_UNAVAILABLE` (info) means the camera does
not offer it or rejected it; recognition is unaffected.

## Performance monitoring

Every `runtime.metrics_interval_ms` (60 s) the log gets one summary line per camera and one for
the Jetson. `var/cameras/status.json` holds the same numbers, refreshed every 5 s; `make
camera-status` prints its short table.

| Per camera | |
| --- | --- |
| decoder, codec, resolution | what the stream is decoded with |
| input FPS / processed FPS | frames from the camera / frames the pipeline handled |
| detector FPS and average ms | plate detector calls for this camera |
| OCR calls and average ms | |
| live dropped / stale dropped | frames replaced in the queue / discarded as too old |
| capture-to-process ms | age of a frame when processing picks it up |
| reconnects, first-frame ms | stream stability |
| plates confirmed, last plate | |

| Jetson | |
| --- | --- |
| RAM total / available, process RSS, container memory | `OUT_OF_MEMORY_RISK` below the threshold |
| load average, CPU % | |
| GPU % | from `/sys/devices/gpu.0/load` |
| detector and OCR backends | `tensorrt` expected for both |

A processed FPS far below the input FPS with many stale drops means the Jetson is saturated: use
the sub-stream, `decode.max_fps`, or fewer cameras. `sudo tegrastats` on the host shows the
`NVDEC` clock while hardware decoding runs.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `HARDWARE_DECODER_UNAVAILABLE`; `camera-check` shows `It isn't a v4l2 driver` or `Inappropriate ioctl for device` | nvidia-container-toolkit was upgraded. JetPack 4.6 needs `nvidia-container-toolkit 1.7.0-1`, `nvidia-docker2 2.8.0-1`, `nvidia-container-runtime 3.7.0-1`: downgrade them and `sudo apt-mark hold` them |
| `no element "nvv4l2decoder"` although the plugin file exists | GStreamer blacklists a plugin that failed to load once. The container rebuilds its registry on every start; on the host (testing with `gst-launch-1.0`) run `rm -rf ~/.cache/gstreamer-1.0`. If it stays missing, `gst-inspect-1.0 nvv4l2decoder` in `make shell` prints why (usually the toolkit version, row above) |
| Memory grows after many reconnects | Reported for nvv4l2decoder on R32.7 when decoding pipelines are recreated many times. `OUT_OF_MEMORY_RISK` warns first; `sudo systemctl restart kz-anpr` starts clean |
| `RTSP_AUTH_FAILED` although the password is right | Quotes in `config/cameras.env` (see the warning), a per-camera variable overriding the global one, or the camera is locked after earlier failures: wait 30 min or power-cycle it |
| A camera is found but never READY | Run `make camera-check` and read its block: the code and the decoder errors say which stage failed |
| `NO_CAMERAS_DISCOVERED` | `make camera-lan-setup LAN_ARGS=--show` shows eth0's link and address; cameras in another subnet need the steps above; non-Hikvision cameras go into `discovery.manual_hosts` |
| Events stop but the service runs | `make camera-status`: STALLED or RECONNECTING names the camera and the code |
| Out of memory, very slow start | Only one ANPR process fits on the 4 GB Nano: stop the service before `make run-cameras`, `make check` or `make run` |

## Verification status

Development verified, without camera hardware:

- unit tests of every camera-mode module, run on macOS and in a Linux GCC 7 container: network
  classification against recorded sysfs/procfs trees of a Jetson with modems and the USB gadget,
  route and ARP parsing, SADP / ONVIF / ISAPI replies taken from real Hikvision devices, RTSP
  digest authentication and SDP parsing against simulated cameras (local RTSP and HTTP servers on
  127.0.0.1), the camera registry, the reconnect policy, the status file and the camera-mode
  configuration;
- the multi-camera runner with simulated sources and stand-in models, and GStreamer capture with
  software decoders (GStreamer 1.16 in the test container);
- `tools/jetson_docker.sh`, `tools/install_service.sh`, `tools/camera_lan_setup.sh` and the
  Makefile targets against stand-ins for docker, sudo, systemctl, nmcli and sysfs; the
  credential file `make camera-service` writes, through systemd's own parser (systemd 245) and
  checked against the parser source of systemd 237 (JetPack 4.6).

Not verified yet, needs the real hardware:

- a real DS-TCG406-E: its SADP reply, activation state, RTSP realm and lockout behaviour, ISAPI
  device info, and the native ANPR event stream with Kazakhstan plates;
- NVDEC on the Nano: `nvv4l2decoder` through the native GStreamer capture in this image, four
  streams at once, decode latency, CPU load and memory across reconnects;
- the image build with the GStreamer development packages on the Jetson;
- the GSM modem: its classification, the Internet probe, and that nothing disturbs its route;
- NetworkManager on JetPack 4.6 applying, and if needed rolling back, the `kz-camera-lan` profile;
- the service on systemd 237 end to end, including a reboot.
