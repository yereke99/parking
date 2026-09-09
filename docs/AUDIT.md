# Audit

## Existing Implementation

The `parking` directory initially contained:

- `main.py`: experimental Python script.
- `license_plate_detector.pt`: Ultralytics YOLO detector, 6.1 MB, one class named `license_plate`.
- `video/car.mp4`: local sample clip, 6.9 MB, 296 frames, 1920x1080, 23.976 FPS.
- Python cache and local metadata files.

No C++ runtime, CMake project, configuration file, deployment unit, tests, or documentation were present in this subsystem.

## Python Pipeline

The original script:

- Opens `/Users/yerek/rbt/parking/video/car.mp4` through a hard-coded absolute path.
- Runs YOLO on every frame at `imgsz=640` and confidence `0.5`.
- Runs EasyOCR every fifth frame for every detected plate.
- Resizes plate crops by 3x.
- Crops away the top 38 percent of the detected crop.
- Equalizes histogram and applies a small Gaussian blur.
- Uses an EasyOCR allowlist of `A-Z0-9`.
- Normalizes a six-character pattern of two letters, two digits, two letters.
- Displays frames through `cv2.imshow`.

The crop comment references a Japanese-style plate layout, and the six-character normalizer does not match Kazakhstan ordinary passenger plates.

## Baseline Measurement

Measured on this development Mac with CPU inference, not on Wiren Board 8:

| Metric | Value |
| --- | ---: |
| Frames processed | 296 |
| Frames with detector output | 296 |
| Total runtime | 740.31 s |
| Effective processing FPS | 0.40 |
| Detector average latency | 162.63 ms/frame |
| Detector p95 latency | 169.41 ms/frame |
| Detector max latency | 389.19 ms/frame |
| EasyOCR calls | 59 |
| EasyOCR average latency | 11,723.27 ms/call |
| EasyOCR p95 latency | 24,185.60 ms/call |
| EasyOCR max latency | 31,963.13 ms/call |
| Peak RSS | about 1.98 GB on macOS `ru_maxrss` reporting |

Recognized outputs were mostly `BR45IL`, with occasional `BRA5IL`. Those are not valid current Kazakhstan ordinary plates. The sample clip appears to be useful for plate detector/motion experiments, but it is not sufficient proof of Kazakhstan OCR accuracy.

## Target Compatibility

The Python prototype is not production-compatible with the target:

- Requires Python, PyTorch/Ultralytics, and EasyOCR at runtime.
- Uses GUI display calls.
- Performs neural detection continuously.
- Uses general scene-text OCR.
- Has unbounded latency for barrier behavior.
- Has no explicit vehicle stopped state.
- Has no KZ format parser or confidence-aware temporal consensus.

The local machine has Python OpenCV installed, but no C++ OpenCV package was discoverable by CMake/pkg-config. Runtime compilation must be verified on a machine with `libopencv-dev` or equivalent.

## Major Risks

- No dedicated Kazakhstan OCR model is present.
- Detector training data/license are unknown from the `.pt` file alone.
- Only one local video clip is available.
- No ground truth labels are available.
- Night, glare, dirty plate, rain, and oblique-angle cases are not represented.
- Wiren Board 8 CPU/RAM/thermal measurements are still required.

## Architecture Chosen For This Pass

The new C++ structure keeps the continuous path cheap and deterministic:

- ROI frame differencing at camera FPS.
- Stop-confirmation timer before recognition.
- Recognition burst only after `VEHICLE_STOPPED`.
- OpenCV DNN YOLOv8-style plate detector adapter for ONNX.
- CTC OCR adapter for a future dedicated Kazakhstan ONNX recognizer.
- Kazakhstan parser/validator.
- Candidate-level temporal consensus.
- JSON `PlateRecognitionEvent`.
- Headless default runtime with optional visualization.

The implementation rejects missing OCR/model paths explicitly instead of silently falling back to the Python prototype.


---

# Post-Migration Addendum

Recorded after the C++ rewrite. The sections above describe the state before it.

## What changed since the first pass

The target moved from Wiren Board 8 to Jetson Orin Nano Super, and the OCR question was settled:
the placeholder `CtcPlateOcr`, written against a `kz_plate_ocr.onnx` that never existed, is
replaced by real Fast Plate OCR inference. Alongside that:

- inference moved behind `IInferenceSession` with ONNX Runtime TensorRT, CUDA and CPU providers
  plus an OpenCV DNN fallback, all selected in one place;
- the camera moved behind `CameraSource` with a capture thread, a latest-frame slot and
  reconnect backoff;
- the trigger moved from frame-difference motion alone to plate tracking with velocity, size
  growth and zone membership, with motion kept as the cheap idle gate;
- the detector gained a per-state cadence;
- Kazakhstan plate layouts moved from C++ into configuration as slot patterns;
- consensus became streaming, so OCR stops as soon as the evidence is sufficient;
- the config loader became a real YAML-subset parser that reports unknown keys.

See [the migration plan](MIGRATION_PLAN.md) for the reasoning.

## Two defects the sample clip exposed

Both were found by running the pipeline, not by reading it.

**Mixed clocks.** `stream_ms` fell back to the monotonic wall clock when `CAP_PROP_POS_MSEC`
returned 0, which is the legitimate value for a file's first frame. One wall-clock timestamp
among stream-relative ones put every later frame in the past, and the detector ran exactly once
in a 296-frame clip. File sources now derive their timeline from the container's frame rate and
never mix in the wall clock.

**Frame differencing measured the wrong quantity.** Comparing against the immediately previous
frame measures speed. On the sample clip, a slow camera orbit, that produced motion scores around
1e-7 and the idle gate never opened. The comparison frame now comes from
`motion.reference_interval_ms` ago, which measures displacement instead. A vehicle creeping
toward a barrier has the same problem as a slow orbit, so this matters in production, not just on
this clip.

A third issue was a design gap rather than a bug: the consensus computed confidence from mean OCR
confidence and ignored the weakest character, so a reading containing a 0.15 character could
still score 0.91. The weakest character now caps the result.

## Measured outcome

Full numbers in [benchmarks](../benchmarks/README.md). On the same clip, same models, same
machine:

| | Python `main_fast.py` | C++ pipeline |
| --- | ---: | ---: |
| Detector calls | 296 | 49 |
| OCR calls | 59 | 3 |
| Detector average | 42.3 ms | 38.5 ms |
| OCR average | 25.9 ms | 14.9 ms |
| Wall clock | 14.34 s | 2.54 s |
| Peak RSS | not measured | 277 MB |

Against the original EasyOCR prototype: 0.40 FPS to 116 FPS, 11.7 s per OCR call to 14.9 ms,
about 1.98 GB peak RSS to 277 MB.

Peak RSS grows 18 percent from 592 to 8,880 frames, which is allocator high-water behaviour rather than accumulation. Twenty consecutive recognition cycles all confirmed,
no timeouts. Address and undefined-behaviour sanitizers are clean over 888 frames.

## Risks that remain

Unchanged from the first pass, and none of them are addressable from this repository:

- **Kazakhstan accuracy is unmeasured.** The OCR model's training regions do not include
  Kazakhstan, and there is no labelled Kazakhstan evaluation set here.
- **The only sample clip is a parked Japanese-plate car**, not a barrier approach.
- **The detector's ONNX metadata declares AGPL-3.0.** That needs a decision before release.
- **No Jetson hardware was available.** Every number above is from a development Mac. TensorRT
  FP16 latency, GPU utilisation and thermal behaviour are all unmeasured.
- Night, glare, rain, dirty plate and oblique-angle cases are unrepresented.
