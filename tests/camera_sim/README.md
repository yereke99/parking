# Simulated Hikvision camera LAN (development tooling)

**SIMULATION ONLY.** This directory builds a fake Hikvision camera network in Docker so that
camera mode (`kz_anpr --camera-scan`, `--camera-check`, `--cameras`) can be developed and tested on
a development machine (tested on a Mac, Docker Desktop, arm64). Passing against it shows that our
code follows the protocols as published. It never proves how real DS-TCG406-E firmware, a real
PoE switch or the Jetson's NVIDIA decoder behave. Check those on real hardware.

```text
 Docker bridge "kzcam" 192.168.77.0/24 (gateway .1)
 ├── 192.168.77.5   client: `sim.sh run` (kz-anpr-dev image, repo at /src read-only)
 ├── 192.168.77.21  cam1 ─┬─ kzcam-cam1-rtsp  MediaMTX: RTSP 554, digest, TCP, looping clip
 │                        └─ kzcam-cam1-hik   fake_hikvision.py (same network namespace):
 │                                            SADP 37020, WS-Discovery 3702, HTTP 80, SDK 8000
 ├── 192.168.77.22  cam2  (same pair)
 ├── 192.168.77.23  cam3
 ├── 192.168.77.24  cam4
 └── 192.168.77.25  cam5  (optional: `sim.sh up 5`)
```

| Camera | IP / MAC | Streams 101 / 102 | RTSP port | ONVIF | Login | Scenario |
|---|---|---|---|---|---|---|
| cam1 | .21 / bc:ad:28:77:00:21 | H.264 608x1080 / H.264 304x540 | 554 | off | shared | healthy camera, ONVIF_DISABLED_OR_UNAVAILABLE |
| cam2 | .22 / bc:ad:28:77:00:22 | H.265 608x1080 / H.265 304x540 | 554 | on | shared, separate ONVIF account | H.265 decoding, ONVIF discovery and identification |
| cam3 | .23 / bc:ad:28:77:00:23 | H.264 / H.264 | **8554** | off | shared | RTSP port override (`manual_hosts: ["192.168.77.23:8554"]`), `close-rtsp` |
| cam4 | .24 / bc:ad:28:77:00:24 | H.264 / H.264 | 554 | off | password `other` | RTSP_AUTH_FAILED with the shared credentials |
| cam5 | .25 / bc:ad:28:77:00:25 | H.264 / H.264 | 554 | off | none (not activated) | CAMERA_NOT_ACTIVATED (`Activated=false`, everything 401) |

All cameras report model `DS-TCG406-E`, firmware `V5.7.10 build 231010`, serial
`DS-TCG406-E20260101AAWRSIMnn…`, device name `SIM camN` and realm `IP Camera(SIMnn)`, so nothing
can be mistaken for a real device. The MACs use a Hikvision OUI (bc:ad:28) so OUI-based vendor
detection can be exercised.

## Requirements

- Docker with the `kz-anpr-dev:focal-gcc7` image already built. It provides python3 3.8,
  ffprobe, GStreamer 1.16 with gst-libav, and curl.
- `bluenviron/mediamtx:1.21.1-ffmpeg`, pulled by `sim.sh up` if it is missing. It is pinned and
  has an arm64 variant. The tested image is MediaMTX v1.21.1 on Alpine 3.24 with ffmpeg 8.1.2,
  digest `sha256:00ef3d1a…`, and it is identical to `latest-ffmpeg` on 2026-10-09.
- `video/parking.mp4`, the clip whose expected plate is `152JTA02`.
- Override with `SIM_DEV_IMAGE`, `SIM_MEDIAMTX_IMAGE` and `SIM_BUILD_DIR`.

`sim.sh` runs on macOS's bash 3.2 and on Linux.

## Usage

```sh
tests/camera_sim/sim.sh up            # network, clip cache, cameras 1-4 (idempotent)
tests/camera_sim/sim.sh up 5          # also cam5 (not activated); `up 2` keeps only cam1-2
tests/camera_sim/sim.sh status        # containers, RTSP open/closed, AUTH_FAIL / LOCKED counts
tests/camera_sim/sim.sh verify        # end-to-end checks of the simulator (below), ~2 minutes
tests/camera_sim/sim.sh down          # remove containers and networks (clip cache is kept)
tests/camera_sim/sim.sh clean         # down + delete tests/camera_sim/.cache
```

Fault injection:

```sh
sim.sh stop-camera K    # PoE loss / reboot: both containers stop, the IP disappears
sim.sh start-camera K   # power back (same IP and MAC)
sim.sh close-rtsp K     # camera answers SADP/HTTP/SDK but RTSP connections are refused
sim.sh open-rtsp K
sim.sh trigger-anpr K   # the camera reports its plate on the alertStream now (also SIGUSR1)
sim.sh logs K [rtsp|hik]
```

Running our code on the camera LAN:

```sh
sim.sh build                        # cmake + make kz_anpr into tests/camera_sim/.cache/build-linux
sim.sh run                          # shell at 192.168.77.5; /src = repo (ro), /b = build dir
sim.sh run /b/kz_anpr --camera-scan --camera-config /work/cameras-sim.yaml
SIM_UPLINK=1 sim.sh run …           # Jetson-like routing, see below
```

The `run` container:

- Runs in `/work` (`tests/camera_sim/.cache/work`, writable, so `var/cameras/…` state survives).
  It has symlinks `config`, `models`, `video` and `tools` into `/src`.
- Gets `sim.env` as its env-file and exports `SIM_CAMERA_INTERFACE`, the name of the camera LAN
  interface.
- Has Docker's default capabilities, so CAP_NET_RAW (AF_PACKET, SO_BINDTODEVICE) is available
  and NET_ADMIN is not, the same as the production container.

Routing in the `run` container:

- **Default:** the only interface is `eth0` on kzcam, which also carries Docker's default route
  via 192.168.77.1. Camera mode will warn CAMERA_LAN_HAS_DEFAULT_ROUTE. That is correct for this
  topology and is an artifact of the simulator.
- **`SIM_UPLINK=1`:** also attaches the NAT network `kzcam-uplink` (subnet chosen by Docker) with
  `gw-priority=1`. The default route and Internet then leave through that network (a stand-in for
  the GSM modem), and the camera LAN has no default route, as on the Jetson.
- With the uplink attached, a multicast probe sent without an explicit egress interface leaves
  through the uplink and finds nothing. This was verified:
  `sadp_probe.py sadp` returns 0 devices, while `sadp_probe.py sadp --interface-ip 192.168.77.5`
  returns 4. This is the egress pitfall from the network research, so discovery code must pass
  the camera interface.
- Docker `--internal` networks cannot be used for kzcam. Their isolation rules drop packets
  addressed outside the subnet, and that includes 239.255.255.250. This was verified: 0 SADP
  answers.

For a camera-mode config used inside the simulator:

- Set `network.interface` to `$SIM_CAMERA_INTERFACE` (`eth0` without the uplink). Container
  interfaces are veths without a `device` link, so automatic camera-LAN detection that relies on
  sysfs hardware links will not pick them.
- List `192.168.77.23:8554` under `discovery.manual_hosts`.

## What is simulated

**SADP** (`fake_hikvision.py`):

- Listens on UDP 0.0.0.0:37020 and joins 239.255.255.250 on the camera address.
- An XML `<Probe>` with `<Types>inquiry</Types>` or `inquiry_v32` is answered by unicast to the
  sender's address and port with a `<ProbeMatch>`.
- The ProbeMatch has exactly the fields of research/hikvision.md section 1, in that order: Uuid
  (echoed), Types=inquiry, DeviceType, DeviceDescription (model), DeviceSN, MAC (dashes),
  IPv4Address/SubnetMask/Gateway, IPv6Address/Gateway/MaskLen, DHCP=false, CommandPort=8000,
  HttpPort=80, DSPVersion, BootTime, SoftwareVersion, Activated, PasswordResetModeSecond,
  PasswordResetAbility, SupportSecurityQuestion, SupportHCPlatform, HCPlatformEnable, Encoder,
  OEMInfo=SIMULATED, AnalogChannelNum, DigitalChannelNum, SDKOverTLSPort, SDKServerStatus.
- A binary prefix before the XML is tolerated. Other `Types` values (the mutating SADP commands)
  are ignored and logged.

**WS-Discovery** (cam2 only, `--onvif on`):

- Listens on UDP 3702, joined to 239.255.255.250.
- A SOAP 1.2 Probe for `dn:NetworkVideoTransmitter`, for `tds:Device`, or with no type is
  answered by unicast with a ProbeMatch:
  - `RelatesTo` set to the probe's MessageID.
  - EndpointReference `urn:uuid:…<mac>`; Hikvision ends the UUID with the MAC.
  - Types `dn:NetworkVideoTransmitter tds:Device`.
  - Scopes `type/video_encoder`, `Profile/Streaming|G|T`, `hardware/DS-TCG406-E`,
    `name/HIKVISION%20DS-TCG406-E` and `location/city/hangzhou`.
  - XAddrs `http://<ip>/onvif/device_service`.
- Scope filters are honoured. With ONVIF off (the Hikvision default since V5.5.0) the camera does
  not answer at all.

**HTTP** (port 80, `Server: webserver`, HTTP/1.1 keep-alive). Every request line is logged; no
password, Authorization header or digest response is ever logged. Endpoints:

- `GET /`, `/index.asp` and `/doc/page/login.asp`: a small HTML page labelled SIMULATED. No
  authentication is needed.
- `/ISAPI/*` requires RFC 2617 digest. The challenge is
  `WWW-Authenticate: Digest qop="auth", realm="IP Camera(SIMnn)", nonce="…", stale="FALSE"`.
  Only MD5 with `qop=auth`, `nc` and `cnonce` is accepted, the `uri` must equal the request
  target, and Basic is a failed login, as Basic is off by default on the cameras. Nonces live
  300 s; a correct response with an unknown or expired nonce gets `stale="TRUE"` and does not
  count as a failure. 401 bodies are Hikvision's `userCheck` (`lockStatus`, `unlockTime`,
  `retryLoginTime`). ISAPI resources:
  - `GET /ISAPI/System/deviceInfo`: DeviceInfo in namespace
    `http://www.hikvision.com/ver20/XMLSchema` with deviceName, deviceID (a stable UUID),
    model, serialNumber, macAddress, firmwareVersion, firmwareReleasedDate, deviceType=IPCamera
    and more.
  - `GET /ISAPI/Streaming/channels` and `/101`, `/102`: StreamingChannel with
    `Video/videoCodecType` (`H.264`/`H.265`), `videoResolutionWidth/Height`, `maxFrameRate` in
    hundredths of fps (3000 = 30 fps) and `GovLength`. The values are measured from the clip
    each channel streams, so ISAPI and RTSP agree. Any other channel returns 404 `notSupport`.
  - `GET /ISAPI/Traffic/capabilities`: 200 with `isSupportVehicleDetection`,
    `isSupportVehicleDetect`, `isSupportANPR` and `ANPR/isSupport`. The exact tag names vary by
    firmware, so several are offered.
  - `GET /ISAPI/System/time`, `/System/Network/interfaces`, `/System/Network/Integrate` (the
    ONVIF flag) and `/Security/userCheck`.
  - Any other authenticated resource gets 404 `ResponseStatus` (statusCode 4, `notSupport`).
  - Every authenticated non-GET request gets 403 and logs `WRITE_REJECTED`: the simulator is
    read-only, and tests can prove our tool never writes to a camera.
- `GET /ISAPI/Event/notification/alertStream` is a long-lived
  `multipart/mixed; boundary=boundary` response with chunked transfer encoding. It sends:
  - A heartbeat `EventNotificationAlert` (`eventType` videoloss, `eventState` inactive) on
    connect and then every 5 s.
  - An ANPR `EventNotificationAlert` every 20 s and on `trigger-anpr`, shared by all connected
    streams and in the shape of research section 6a:
    - The `<ANPR>` block carries `licensePlate` 152JTA02, `country` 30 (Kazakhstan), `line`,
      `direction`, `confidenceLevel`, `plateType`, `plateColor`, `vehicleType`,
      `plateCharBelieve`, `vehicleInfo`, `pictureInfoList` with `plateRect` 338,338 117x44,
      `originalLicensePlate`, `CRIndex` and `vehicleListName`.
    - Around the block: `<UUID>`, `<picNum>`, and `dateTime` with a +05:00 offset.
    - Two JPEG parts follow: `licensePlatePicture.jpg`, the plate crop from the clip at 7.6 s,
      and `detectionPicture.jpg`, the full frame.
  - The stream ends with `--boundary--` when the camera stops.
- `/onvif/device_service` (cam2):
  - `GetSystemDateAndTime` needs no authentication.
  - `GetDeviceInformation` needs a WS-UsernameToken PasswordDigest,
    `Base64(SHA1(nonce + created + password))`, for the separate ONVIF account `onvifuser`.
    PasswordText, a wrong digest or a Created timestamp more than 300 s off gets 401
    `ter:NotAuthorized` and counts as a failed login. A missing token gets 401 but does not
    count.
  - With ONVIF off the path returns 404.

**Illegal-login lock:**

- 5 failed logins from one client IP within 30 minutes lock that IP for 30 minutes. While
  locked, every ISAPI and ONVIF request from it gets 401 with `lockStatus=lock`, even with the
  right password. The camera logs `AUTH_FAIL ip=… source=isapi|onvif|rtsp failures=n/5`,
  `LOCKED ip=… after 5 failed logins …` and `LOCKED reject …`. Other client IPs are unaffected.
- RTSP failures count towards the same budget. `fake_hikvision.py` tails MediaMTX's log for
  `failed to authenticate` lines; MediaMTX logs one per rejected credential, not for the
  unauthenticated first DESCRIBE.
- MediaMTX itself is not blocked by the lock: a locked IP that sends the right RTSP password
  still gets the stream.
- A correct login does not reset the count, which is stricter than "5 consecutive".
- The lock state is in memory, so restarting the `-hik` container clears it, like a camera
  reboot.
- An un-activated camera rejects everything without counting ISAPI attempts. Its RTSP rejections
  do count, so any `AUTH_FAIL` on cam5 shows the tool tried a camera it should have skipped.
- Tests can assert on `sim.sh logs K hik`:
  - At most one `AUTH_FAIL` per camera and retry interval.
  - Never `LOCKED`.
  - Never `WRITE_REJECTED`.

**SDK port 8000:** accepts TCP so port probes see what a camera shows, reads until the client is
quiet for 10 s, and closes. The proprietary SDK protocol is not spoken.

**RTSP** (MediaMTX, one per camera, generated `.cache/cams/camK/mediamtx.yml`):

- Paths are `Streaming/Channels/101` and `102`. A clip is looped by
  `ffmpeg -re -stream_loop -1 -c copy` and published over localhost only (`authInternalUsers`
  publish is limited to 127.0.0.1/::1).
- Reading needs `admin` with the camera's password. `rtspAuthMethods: [digest]` (MediaMTX 1.21.1
  supports digest), so only Digest MD5 is offered, like Hikvision's default.
- MediaMTX's digest realm is fixed to `ipcam`, not `IP Camera(…)`, and its `Server` header is
  `gortsplib`. Clients must take the realm from the challenge.
- Behaviour, all verified:
  - OPTIONS without credentials: 200.
  - DESCRIBE without or with wrong credentials: 401. MediaMTX then keeps the connection about
    3 s before closing it.
  - Valid credentials on an unknown path: 404 Not Found, which is RTSP_STREAM_PATH_INVALID. A
    catch-all path keeps it from being 400.
  - `rtspTransports: [tcp]`: interleaved TCP only. A UDP SETUP gets `461 Unsupported Transport`,
    so clients must force TCP.
- `close-rtsp` hot-swaps the config so RTSP listens on 127.0.0.1 only. The camera's LAN RTSP
  port then refuses connections while the publishers, HTTP, SADP and the SDK port keep running.
  Existing RTSP sessions are dropped.

**Streams.** `video/parking.mp4` checked with ffprobe in the dev image:

```text
stream|codec_name=h264|profile=High|width=608|height=1080|pix_fmt=yuv420p|r_frame_rate=30/1|bit_rate=1413345|nb_frames=564
stream|codec_name=aac|profile=LC   (dropped by -map 0:v:0)
format|duration=18.924267
```

| File | Used for | Codec | Size | fps | GOP | B-frames |
|---|---|---|---|---|---|---|
| `video/parking.mp4` (copied, not re-encoded) | H.264 cameras, 101 | H.264 High | 608x1080 | 30 | 160 (5.3 s) | yes |
| `.cache/clips/sub_h264.mp4` | H.264 cameras, 102 | H.264 Main, CRF 23 | 304x540 | 30 | 60 | no |
| `.cache/clips/main_h265.mp4` | cam2, 101 | H.265 Main 8-bit, x265 CRF 23 | 608x1080 | 30 | 60 | no, VPS/SPS/PPS repeated in-band |
| `.cache/clips/sub_h265.mp4` | cam2, 102 | H.265 Main, CRF 23 | 304x540 | 30 | 60 | no |

Notes on the clips:

- The H.264 main stream keeps the original clip's long GOP and B-frames. A client that joins
  mid-GOP can wait up to 5.3 s for the first key frame and may log decoder warnings until then.
  That is a fair worst case for a camera with H.264+ / long I-frame intervals.
- The transcodes are made by `sim.sh up` with the MediaMTX image's ffmpeg 8.1.2 (libx265 4.x).
  The dev image's ffmpeg 4.2 / x265 3.2 also works but prints `Failed to genrate CPU mask` on
  arm64 and took 95 s instead of 32 s.
- The transcodes are cached in `tests/camera_sim/.cache/clips` (gitignored) and keyed on the
  clip's checksum and the recipe.
- Is the plate still readable after transcoding? The HEAD build (69746d7) of
  `kz_anpr --backend onnx_cpu` was run on the files:
  - `parking.mp4`: `VALID_HIGH_CONFIDENCE 152JTA02`.
  - `main_h265.mp4`: `VALID_HIGH_CONFIDENCE 152JTA02`, confidence 0.858.

## What it cannot simulate

- **NVIDIA hardware decoding** (nvv4l2decoder / NVDEC). Containers here have no NVIDIA runtime,
  so camera mode will report HARDWARE_DECODER_UNAVAILABLE and decode in software. Decoder plans,
  latency and memory have to be checked on the Jetson.
- **Real Hikvision firmware.** That includes:
  - Its RTSP server: realm, header order, session timeouts, RTCP and keep-alive behaviour,
    `Server` header.
  - Exact ISAPI capability tag names.
  - Whether RTSP failures really feed the lock.
  - Smart-codec streams, SEI and metadata tracks.
  - Activation over SADP.
  - Camera-side ANPR accuracy.
  - The httpHosts push mode.
- **PoE electrical link loss.** `stop-camera` removes the camera from the bridge (connections
  hang, then fail with "host unreachable"). It approximates power loss. It cannot reproduce an
  Ethernet link going down on the Jetson's own port (CAMERA_LAN_LINK_DOWN): the client's link
  stays up.
- **The GSM modem.** `SIM_UPLINK=1` gives a NAT uplink with a default route, but no modem
  interface types (ppp/wwan/cdc_ether), metrics, outages or captive APNs.
- Duplicate IPs (Docker refuses them on one network), cameras on a foreign subnet such as the
  factory 192.168.1.64, DHCP, rp_filter on the host, Jetson-specific sysfs (r8168 `eth0`,
  `l4tbr0`, `rndis0`, `usb0`), RAM pressure and real network loss or jitter.

## Producing the diagnostics

| Diagnostic | How |
|---|---|
| RTSP_AUTH_FAILED | cam4 with the shared credentials; `sim.sh logs 4 hik` shows the AUTH_FAIL count |
| RTSP_CREDENTIALS_MISSING | `sim.sh run env -u HIKVISION_USERNAME -u HIKVISION_PASSWORD /b/kz_anpr …` |
| RTSP_STREAM_PATH_INVALID | a stream path that is not 101/102, e.g. `rtsp.main_path: /Streaming/Channels/999` |
| RTSP_PORT_CLOSED | `sim.sh close-rtsp K` |
| CAMERA_UNREACHABLE, STREAM_TIMEOUT | `sim.sh stop-camera K` (before / while streaming) |
| STREAM_ENDED | `sim.sh close-rtsp K` while streaming (verified: a running rtspsrc gets EOS) |
| CAMERA_NOT_ACTIVATED | `sim.sh up 5` (cam5) |
| ONVIF_DISABLED_OR_UNAVAILABLE | cam1, cam3, cam4 |
| CAMERA_LIMIT_REACHED | `sim.sh up 5` with `runtime.max_active_cameras: 4` |
| NO_CAMERAS_DISCOVERED | `sim.sh up 1`, then `sim.sh stop-camera 1` |
| HARDWARE_DECODER_UNAVAILABLE | always (no NVIDIA runtime) |
| CAMERA_LAN_HAS_DEFAULT_ROUTE | `sim.sh run` without `SIM_UPLINK=1` |
| NO_INTERNET_ROUTE / INTERNET_OFFLINE | not reproducible: kzcam always has Docker's gateway, which reaches the Internet |

## Verification of the simulator itself

These checks use existing tools only, because kz_anpr camera mode is not finished yet. `sim.sh
verify` starts two helper containers on kzcam, at 192.168.77.6 (checks) and 192.168.77.7
(lockout), runs `verify_sim.sh` checks in them, and drives fault injection from the host. The
helpers are removed afterwards. The individual commands:

```sh
# RTSP codec/size (credentials percent-encoded into the URL; URLs are redacted in all output)
ffprobe -v error -rtsp_transport tcp -show_entries stream=codec_name,profile,width,height,r_frame_rate \
    -of compact=p=0 'rtsp://admin:Sim-pass_123@192.168.77.22:554/Streaming/Channels/101'
# frames through GStreamer, software decoding, counted for 12 s
timeout 12 gst-launch-1.0 -v rtspsrc location=rtsp://192.168.77.22:554/Streaming/Channels/101 \
    protocols=tcp latency=200 user-id=admin user-pw=Sim-pass_123 ! rtph265depay ! h265parse \
    ! avdec_h265 ! fakesink sync=false silent=false | grep -c 'last-message = chain'
python3 /sim/sadp_probe.py sadp --expect 192.168.77.21 192.168.77.22 192.168.77.23 192.168.77.24
python3 /sim/sadp_probe.py wsd --json                      # only 192.168.77.22 answers
python3 /sim/sadp_probe.py onvif-info 192.168.77.22        # ONVIF_USERNAME/PASSWORD from sim.env
curl --digest -u admin:Sim-pass_123 http://192.168.77.21/ISAPI/System/deviceInfo      # 200
curl --digest -u admin:wrong http://192.168.77.21/ISAPI/System/deviceInfo             # 401
for i in 1 2 3 4 5; do curl -s -o /dev/null -w '%{http_code} ' --digest -u admin:wrong \
    http://192.168.77.21/ISAPI/System/deviceInfo; done      # from .7: 401 x5, then locked
curl -sN --digest -u admin:Sim-pass_123 http://192.168.77.21/ISAPI/Event/notification/alertStream
```

Result of `tests/camera_sim/sim.sh up 5 && tests/camera_sim/sim.sh verify` on 2026-10-09. The
machine was a Mac with Docker Desktop (Engine 29.6.1) on arm64, using the final configuration.
The run took 1 min 50 s and exited 0. SADP detail lines are trimmed here.

```text
== simulator verification from 192.168.77.6 (and 192.168.77.7 for the lockout)
-- RTSP: codec and size of every stream (ffprobe, TCP, digest)
PASS rtsp cam1 Streaming/Channels/101: codec_name=h264|profile=High|width=608|height=1080|r_frame_rate=30/1
PASS rtsp cam1 Streaming/Channels/102: codec_name=h264|profile=Main|width=304|height=540|r_frame_rate=30/1
PASS rtsp cam2 Streaming/Channels/101: codec_name=hevc|profile=Main|width=608|height=1080|r_frame_rate=30/1
PASS rtsp cam2 Streaming/Channels/102: codec_name=hevc|profile=Main|width=304|height=540|r_frame_rate=30/1
PASS rtsp cam3 Streaming/Channels/101: codec_name=h264|profile=High|width=608|height=1080|r_frame_rate=30/1
PASS rtsp cam3 Streaming/Channels/102: codec_name=h264|profile=Main|width=304|height=540|r_frame_rate=30/1
PASS rtsp cam4 Streaming/Channels/101: codec_name=h264|profile=High|width=608|height=1080|r_frame_rate=30/1
PASS rtsp cam4 Streaming/Channels/102: codec_name=h264|profile=Main|width=304|height=540|r_frame_rate=30/1
PASS rtsp cam5: 401 Unauthorized (CAMERA_NOT_ACTIVATED scenario)
-- RTSP: rejected logins and a wrong stream path
PASS rtsp cam1-wrong-password: 401 Unauthorized (RTSP_AUTH_FAILED scenario)
PASS cam1 counted that wrong RTSP password once (AUTH_FAIL source=rtsp)
PASS rtsp cam4-shared-credentials: 401 Unauthorized (RTSP_AUTH_FAILED scenario)
PASS rtsp cam1 Streaming/Channels/999: 404 Not Found (RTSP_STREAM_PATH_INVALID)
-- RTSP: frames through GStreamer (rtspsrc, software decoders)
PASS gstreamer cam1 Streaming/Channels/101: 324 frames decoded by avdec_h264 in 12s (rtspsrc protocols=tcp)
PASS gstreamer cam2 Streaming/Channels/101: 329 frames decoded by avdec_h265 in 12s (rtspsrc protocols=tcp)
PASS gstreamer cam3 Streaming/Channels/102: 329 frames decoded by avdec_h264 in 12s (rtspsrc protocols=tcp)
-- Discovery
PASS sadp: ProbeMatch from 192.168.77.21 192.168.77.22 192.168.77.23 192.168.77.24 192.168.77.25
       192.168.77.21   mac=bc-ad-28-77-00-21 model=DS-TCG406-E serial=DS-TCG406-E20260101AAWRSIM217798817 activated=true http=80 sdk=8000 sw='V5.7.10build 231010' from=192.168.77.21:37020
       (… .22, .23, .24 alike …)
       192.168.77.25   mac=bc-ad-28-77-00-25 model=DS-TCG406-E serial=DS-TCG406-E20260101AAWRSIM257798821 activated=false http=80 sdk=8000 sw='V5.7.10build 231010' from=192.168.77.25:37020
PASS sadp 192.168.77.21: Activated=true
PASS sadp 192.168.77.25: Activated=false
PASS http cam5-not-activated: GET / 401
PASS isapi cam5-not-activated deviceInfo: 401 lockStatus=unlock retryLoginTime=0
PASS ws-discovery: only 192.168.77.22 answered (ONVIF off elsewhere)
PASS onvif cam2: ONVIF 192.168.77.22: GetDeviceInformation OK manufacturer=HIKVISION model=DS-TCG406-E firmware='V5.7.10 build 231010' serial=DS-TCG406-E20260101AAWRSIM227798818
PASS onvif cam1: /onvif/device_service 404 (ONVIF disabled, as shipped)
PASS tcp cam1-sdk: 192.168.77.21:8000 open
-- ISAPI (HTTP digest)
PASS isapi cam1 deviceInfo: 200 model=DS-TCG406-E serialNumber=DS-TCG406-E20260101AAWRSIM217798817 macAddress=bc:ad:28:77:00:21 firmwareVersion=V5.7.10 deviceID=a61c05e8-c9ce-592e-a06c-b98d8d1f323a
PASS isapi cam1-wrong-password deviceInfo: 401 lockStatus=unlock retryLoginTime=3
PASS isapi cam1 channel 101: videoCodecType=H.264 videoResolutionWidth=608 videoResolutionHeight=1080 maxFrameRate=3000 GovLength=160
PASS isapi cam2 channel 101: videoCodecType=H.265 videoResolutionWidth=608 videoResolutionHeight=1080 maxFrameRate=3000 GovLength=60
PASS isapi cam2 Traffic/capabilities: 200 isSupportANPR=true isSupportVehicleDetection=true
-- Illegal-login lock (cam1, from 192.168.77.7)
PASS lockout cam1 wrong password x5: HTTP 401 401 401 401 401
PASS lockout cam1 right password while locked: 401 lockStatus=lock unlockTime=1799
PASS cam1 logged LOCKED once for 192.168.77.7
PASS isapi cam1-other-client-unaffected deviceInfo: 200 model=DS-TCG406-E serialNumber=… deviceID=a61c05e8-c9ce-592e-a06c-b98d8d1f323a
PASS cam1 never locked the verification client 192.168.77.6
PASS isapi cam1-unlocked-after-reboot deviceInfo: 200 model=DS-TCG406-E serialNumber=… deviceID=a61c05e8-c9ce-592e-a06c-b98d8d1f323a
-- alertStream (heartbeat + ANPR event, every 20 s)
PASS alertStream cam1: multipart/mixed chunked: heartbeats=4 licensePlate=152JTA02 country=30 pictures=licensePlatePicture.jpg(2518B),detectionPicture.jpg(103148B)
-- close-rtsp / open-rtsp (RTSP_PORT_CLOSED)
PASS tcp cam3-rtsp-closed: 192.168.77.23:8554 closed
PASS http cam3-web-still-up: GET / 200
PASS sadp: ProbeMatch from 192.168.77.23
PASS rtsp cam3-reopened Streaming/Channels/101: codec_name=h264|profile=High|width=608|height=1080|r_frame_rate=30/1
-- stop-camera / start-camera (PoE loss, approximated)
PASS tcp cam1-rtsp-gone: 192.168.77.21:554 unreachable
PASS tcp cam1-http-gone: 192.168.77.21:80 unreachable
PASS rtsp cam1-back Streaming/Channels/101: codec_name=h264|profile=High|width=608|height=1080|r_frame_rate=30/1
PASS http cam1-back: GET / 200
== verification: 45 passed, 0 failed
```

Further checks run by hand on the same simulator:

- The existing RTSP-source mode of the HEAD build (69746d7) reads the plate from the simulated
  streams. That build uses OpenCV/FFmpeg with forced TCP. The command was:

  ```sh
  SIM_BUILD_DIR=<headbuild> sim.sh run /b/kz_anpr --config config/default.yaml --backend onnx_cpu \
      --events-file /work/events.jsonl --source 'rtsp://…@192.168.77.2N:554/Streaming/Channels/101'
  ```

  Each camera ran for 60 s. cam1 (H.264) gave `LOW_CONFIDENCE 152JTA02`. cam2 (H.265) gave
  `VALID_HIGH_CONFIDENCE 152JTA02`, plus one `TIMEOUT` on another pass of the loop. ONNX on the
  CPU processes about 4-5 of the 30 fps, so the status per pass varies.
- No container log and no MediaMTX log contains `Sim-pass_123`, `Sim-onvif_456` or the wrong
  test password. The scan was `docker logs` of all 10 containers plus `.cache/cams/*/mediamtx.log`.
- With `SIM_UPLINK=1` the client's routes were `default via 172.18.0.1 dev eth1` and
  `192.168.77.0/24 dev eth0`, and Internet was reachable. SADP found 4 cameras with
  `--interface-ip 192.168.77.5` and 0 without it; WS-Discovery found 1 (cam2).
- `sim.sh up` on running cameras takes about 1 s and leaves them untouched.
- Changing `fake_hikvision.py` or a camera's settings recreates that camera on the next `up`; the
  config hash is stored in a container label.

## Unit tests

`test_fake_hikvision.py` holds the unit tests for `fake_hikvision.py` and `sadp_probe.py`. They
use the standard library only, bind only to 127.0.0.1 with ephemeral ports, need no root and run
in about 8 s. They cover:

- Digest and the lock, with a fake clock.
- WS-UsernameToken rules.
- Exact SADP field names.
- WS-Discovery matching.
- ISAPI and alert documents.
- A live HTTP server checked with `urllib`'s own digest client.
- The chunked multipart alertStream, ONVIF over HTTP, SADP and WS-Discovery over UDP, the SDK port
  and the MediaMTX log watcher.

```sh
python3 tests/camera_sim/test_fake_hikvision.py -v      # macOS python 3.9: 43 tests OK
docker run --rm -v "$PWD/tests/camera_sim:/sim:ro" kz-anpr-dev:focal-gcc7 \
    python3 /sim/test_fake_hikvision.py                 # python 3.8: 43 tests OK
docker run --rm -v "$PWD/tests/camera_sim:/sim:ro" python:3.6-slim \
    python3 /sim/test_fake_hikvision.py                 # python 3.6 (Jetson's Ubuntu 18.04): 43 OK
```

## Files

| File | Purpose |
|---|---|
| `sim.sh` | create, control, verify and remove the simulated LAN |
| `fake_hikvision.py` | one camera's SADP, WS-Discovery, ISAPI, ONVIF, alertStream and SDK port |
| `sadp_probe.py` | SADP / WS-Discovery / ONVIF client used by `verify`, also usable on real cameras |
| `verify_sim.sh` | the checks `sim.sh verify` runs inside the helper containers |
| `sim.env` | **test fixture**: the simulator's logins. Committed, fake, never real credentials |
| `test_fake_hikvision.py` | unit tests |
| `.cache/` | generated clips, per-camera MediaMTX configs and logs, `run` state and build dir (gitignored) |
