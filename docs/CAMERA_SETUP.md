# Camera setup

Software cannot recover a plate that was never captured clearly. Treat the camera as part of the
ANPR system.

This page covers image quality, mounting and calibration, which apply to every camera. For
Hikvision cameras on the PoE switch (addresses, discovery, logins, streams, decoding), see
[Hikvision cameras](CAMERAS.md).

## Image quality

- Plate width at the stop line: at least 120 px, 160 px or more preferred. Crops smaller than
  `quality.min_plate_width_px` x `quality.min_plate_height_px` (90 x 22) are never read.
- Shutter fast enough to freeze a moving car, especially at night: 1/500 s or faster.
- Focus locked on the stop line, not on auto.
- Exposure: manual or limited auto exposure, so headlights do not blow out the plate. Enable WDR if
  there is strong backlight.
- Do not starve the stream of bitrate; compression smears the characters first.
- 15-25 FPS is plenty. Pick the smallest resolution that keeps the plate at 120 px: a single
  `make camera` stream is decoded on the Nano's CPU, and in camera mode four cameras share one
  hardware decoder ([decoding](CAMERAS.md#decoding)).

## Mounting

- Place the camera where the stopped car's plate faces it almost head-on; keep the horizontal and
  vertical angle under about 30 degrees.
- Avoid pointing into headlights or the low sun.
- Frame the stop area, not the whole driveway: the detector only looks inside `roi.detection`.
- Check day and night framing separately.
- Add white or IR light if night frames are noisy or blurred, and check reflective plates with
  headlights on.

## Calibration

All ROIs are `[x, y, width, height]` fractions of the frame, set in `config/jetson-nano.yaml`
(copy the keys from `config/default.yaml`, where every one is documented). In camera mode that
profile applies to every camera; a camera framed differently gets its own copy, named by
`anpr_config` in its entry of `config/cameras.yaml`.

| Key | What it does |
| --- | --- |
| `roi.motion` | area of the cheap frame difference that wakes the pipeline |
| `roi.detection` | the only area the plate detector sees |
| `roi.near_barrier` | a tracked plate centred here counts as near the barrier |
| `roi.stop` | a stop is only accepted inside this area |
| `roi.recognition` | OCR runs only on plates centred here |
| `motion.threshold`, `motion.quiet_threshold` | changed-pixel share for vehicle motion and for a quiet scene |
| `stop_detection.*` | how still a plate must be, and for how long, to count as stopped |
| `quality.min_plate_width_px` | smallest plate crop worth reading |

The shipped stop detection is permissive: recognition starts as soon as a tracked plate is near
the barrier, even while the car still rolls, and the three-vote consensus decides. Tighten
`stop_detection` (the strict values are in `config/default.yaml`) only if cars passing by without
stopping produce events.

A practical loop on the Jetson:

```sh
# Record a minute of the real camera into the checkout (ffmpeg on the host: sudo apt install ffmpeg)
ffmpeg -rtsp_transport tcp -i 'rtsp://user:password@192.168.1.64:554/Streaming/Channels/101' \
  -t 60 -c copy video/gate.mp4

# Replay it with one log line per frame: state, motion, track, detections
make run VIDEO=video/gate.mp4 RUN_ARGS='--timeline --log-level debug'
```

`--log-level debug` also logs every OCR reading and why a crop or a reading was rejected
(`ocr_crop_rejected`, `ocr_rejected`). To look at what the OCR actually receives, set
`debug.save_crops: true`: crops go to `var/debug/crops/`, at most `debug.max_files`. Turn both off
again for production.

Git ignores recorded clips in `video/`; only the test clip `video/parking.mp4` is tracked.
