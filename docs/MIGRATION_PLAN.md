# Migration Plan

Status: historical Fast Plate OCR migration record. The current default is the pinned Nomeroff
integration documented in [NOMEROFF_INTEGRATION.md](NOMEROFF_INTEGRATION.md); the material below
records the earlier C++ rewrite and remains as baseline context.

## 1. What the Python prototype does

`main.py` (EasyOCR prototype):

- Hard-coded absolute video path.
- Ultralytics YOLO on **every** frame, `imgsz=640`, `conf=0.5`.
- EasyOCR on every fifth frame, for every detected box.
- 3x upscale, crop away the top 38 percent (a Japanese-plate assumption, wrong for Kazakhstan),
  histogram equalisation, 3x3 Gaussian blur.
- Six-character normaliser expecting two letters, two digits, two letters. That is not a
  Kazakhstan format.
- `cv2.imshow` GUI output, no event interface.

`main_fast.py` (later prototype):

- Same YOLO detector exported to ONNX, run through OpenCV DNN.
- `fast-plate-ocr` `cct-s-v2-global-model`, OCR every fifth frame.
- Reports timing JSON. No state machine, no ROI, no consensus, no validation.

Measured baseline for `main.py` on this development Mac, CPU only: 0.40 FPS end to end,
162 ms average detector latency, 11.7 s average EasyOCR call, about 2 GB peak RSS.

## 2. What the existing C++ subsystem already had

A first C++ pass existed: CMake project, mini-YAML config, motion-based state machine,
Kazakhstan parser, weighted temporal consensus, quality assessor, JSON event, deterministic
tests, benchmark stub, docs. It targeted Wiren Board 8 and OpenCV DNN on CPU.

Gaps against the current requirements:

| Area | Before | After |
| --- | --- | --- |
| OCR | `CtcPlateOcr`, a CTC decoder for a `kz_plate_ocr.onnx` that does not exist | `FastPlateOcr`, the real Fast Plate OCR CCT model, fixed-slot decode |
| Inference | OpenCV DNN, CPU only | `IInferenceSession` with ONNX Runtime (TensorRT / CUDA / CPU EP) and an OpenCV DNN fallback |
| Camera | `cv::VideoCapture` used inline in the pipeline | `ICameraSource` plus a capture thread with a latest-frame slot and reconnect backoff |
| Trigger | frame-difference motion only | plate tracking, velocity, size growth, zone membership, then motion as the cheap idle gate |
| Scheduling | detector only inside a burst | per-state detector intervals, motion-gated in idle |
| Validation | Kazakhstan formats hard-coded in C++ | slot-pattern profiles and confusion maps loaded from config |
| Consensus | one batch call at the end | streaming, with early stop once the criteria are met |
| Config | two-level flat parser | real YAML subset parser, nested maps and sequences |
| Metrics | none | per-stage latency, counters, rejection reasons |
| Target | Wiren Board 8 | Orin Nano 4 GB validation, Orin Nano/Super 8 GB production |

## 3. Design decisions

**ONNX Runtime, not hand-written TensorRT.** The TensorRT execution provider inside ONNX
Runtime gives FP16 TensorRT engines on Jetson with a serialised engine cache, while the same
code path falls back to CUDA or CPU elsewhere. Hand-written TensorRT would duplicate the
builder, the plugin handling, and the memory management for no measured gain, and would make
the x86 development path a second implementation. Backend choice is a config string, resolved
once at startup behind `IInferenceSession`.

**Fast Plate OCR requires ONNX Runtime.** The `cct-s-v2-global` model takes a `uint8`
NHWC tensor and normalises internally. OpenCV DNN's `blobFromImage` path is float NCHW, so the
OCR stage refuses to load without ONNX Runtime rather than silently mangling the input. The
detector still works on OpenCV DNN when ONNX Runtime is absent.

**Two threads.** Capture must never wait for inference on an RTSP source, so capture runs in
its own thread and publishes into a one-slot buffer that overwrites. Everything else is one
sequential processing thread. There is exactly one vehicle of interest, the detector and OCR
never run concurrently on the same frame, and a worker pool would add latency and lock traffic
without shortening the critical path. This is revisited only if profiling shows the processing
thread saturated.

**Tracking without a framework.** Fixed camera, one important vehicle, predictable approach
direction. Greedy IoU-plus-centroid association with an exponential-moving-average box and a
rolling displacement window covers it. No Kalman filter, no external tracker.

## 4. Order of work

1. Audit and baseline. Done, see `AUDIT.md`.
2. Config, logging, metrics, YAML parser.
3. Validation profiles from config.
4. Tracker and stop detector, no OpenCV, unit tested.
5. State machine rewritten around zones and velocity.
6. Streaming consensus.
7. Inference session abstraction, ONNX Runtime backend.
8. Detector on the new abstraction.
9. Fast Plate OCR.
10. Camera source and capture thread.
11. Pipeline integration.
12. Tests, benchmark, docs.

## 5. Python retained, offline only

`legacy_python/` keeps `main.py` and `main_fast.py` for output comparison during migration.
`tools/` keeps the export and baseline scripts. Nothing in the deployed C++ runtime links,
embeds, or spawns Python.

## 6. Risks carried into this pass

- The Fast Plate OCR global model's region list does **not** include Kazakhstan. It is a
  Latin-alphabet global model, so Kazakhstan plates are in-distribution character-wise but not
  region-wise. Accuracy on Kazakhstan plates is unmeasured against ground truth.
- The only sample clip shows a non-Kazakhstan plate, so it validates the pipeline and the
  timings, not Kazakhstan accuracy.
- The detector `.pt` carries an Ultralytics AGPL-3.0 licence marker in its ONNX metadata.
  Confirm licensing before shipping.
- No Jetson hardware is available in this environment, so all Jetson numbers must be
  re-measured on the device.
