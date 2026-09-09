# Camera Setup

Software cannot recover a plate that was never captured clearly. Use the camera setup as part of the ANPR system.

## Minimum Image Quality

Recommended starting point:

- plate width at stop point: at least 120 pixels, 160+ preferred;
- shutter speed: fast enough to freeze approach motion, especially at night;
- focus: locked on the stop/barrier plane;
- exposure: manual or constrained auto exposure to avoid headlight blowout;
- WDR: enable if strong backlight is common;
- compression: avoid excessive IP-camera bitrate reduction;
- frame rate: 15-25 FPS is enough for the motion monitor if exposure is stable.

## Mounting

- Place the camera where the stopped plate is near frontal.
- Keep horizontal and vertical angle modest; avoid severe perspective skew.
- Avoid pointing directly into headlights or low sun.
- Make the plate-search ROI physically small: the barrier stop area, not the whole driveway.
- Verify day and night framing separately.

## Lighting

- Use supplemental white or IR lighting if night clips are noisy or blurred.
- Check reflective plate overexposure with headlights on.
- Prefer stable illumination over aggressive postprocessing.

## Calibration

Run with visualization during setup:

```sh
./build/kz_anpr --config config/default.yaml --source 0 --visualize --timeline
```

Tune:

- `roi.approach`
- `roi.stop`
- `roi.plate_search`
- `motion.threshold`
- `motion.stop_threshold`
- `motion.stop_confirmation_ms`
- `recognition.minimum_plate_width_px`

Disable visualization in production.

