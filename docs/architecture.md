# Architecture

## The idea

A parking barrier is idle most of the time. The system is built around that: compute is spent in
proportion to how likely the next frame is to contain a plate worth reading.

```text
no vehicle          one downscaled ROI difference per frame, no neural network at all
vehicle approaching detector at 10 FPS, lightweight tracking between calls
vehicle near        detector at 15 FPS
vehicle stopped     short OCR burst on quality-filtered crops
plate confirmed     OCR stops immediately
vehicle leaves      back to idle
```

Measured on the bundled clip, this runs the detector on 16 percent of frames and OCR three times
per vehicle, against the Python prototype's every frame and 59 OCR calls.

## Data flow

```text
CameraSource (file | USB | RTSP | GStreamer)
  -> FramePump              capture thread, one-slot latest-frame handover
  -> RoiMotionDetector      every frame, downscaled ROI difference
  -> PlateDetector          state-dependent cadence, ONNX Runtime or OpenCV DNN
  -> PlateTracker           IoU plus centroid association between detector calls
  -> StopDetector           rolling displacement, size and speed window
  -> VehicleStateMachine    decides what the next frame deserves
  -> QualityAssessor        size, blur, exposure and clipping gates
  -> NomeroffRecognizer     dedicated configured regional CTC model
  -> PlateValidator         configurable slot patterns and confusion repair
  -> PlateConsensus         streaming multi-frame vote, stops early when satisfied
  -> PlateSink              PlateRecognitionEvent
```

## Threading and multiple streams

Each source owns one capture thread and one latest-frame slot. A single coordinator visits the
streams round-robin and runs their independent state machines. Detector weights and the persistent
Nomeroff worker are shared; tracking, stop detection, consensus and event identity remain per
camera.

**Capture thread.** Owns the camera handle, reads frames, publishes into a one-slot buffer that
overwrites. It also owns reconnection with exponential backoff. It never waits for inference,
because an RTSP stream that is not drained backs up in the driver and everything the pipeline
later sees is stale.

**Processing thread.** Everything else, in order. Shared detector and OCR access is serialized,
which bounds CUDA memory and is the safe reference path for the 4 GB target. If a later batch
benchmark proves beneficial, the OCR abstraction already exposes batch recognition.

A frame that the processing thread did not collect before the next one arrived is dropped and
counted. At a barrier the newest view of the vehicle is what matters; the one from 400 ms ago is
worth nothing. There is no queue that can grow.

## Inference backends

`IInferenceSession` hides the runtime. Backend selection happens once, in
`createInferenceSession`, and nowhere else: no business-logic file contains a hardware branch.

| Backend | Used for |
| --- | --- |
| `tensorrt` | Jetson. FP16, serialised engine cache, falls through to CUDA then CPU per subgraph |
| `onnx_cuda` | Any CUDA GPU without TensorRT |
| `onnx_cpu` | Portable fallback, and the development default on x86 and macOS |
| `opencv_dnn` | Last resort when ONNX Runtime is not linked. Detector only |

`auto` walks that list and takes the first that loads. `strict_backend: true` turns a fallback
into a startup failure instead, which is what a production Jetson should run with.

TensorRT is reached through ONNX Runtime's execution provider rather than hand-written TensorRT
code. That gives FP16 engines and an engine cache while keeping one code path for every target;
a hand-written builder would duplicate memory management and turn the x86 development path into
a second implementation.

Detector buffers are allocated once. The session owns its input and output host buffers, the detector
holds `cv::Mat` headers directly over the input tensor so `cv::split` writes the planar NCHW
layout in place. Nomeroff owns its PyTorch tensors in the persistent worker and receives only the
quality-selected crop; a full frame never crosses the process boundary.

## State machine

```text
IDLE -> VEHICLE_APPROACHING -> VEHICLE_NEAR -> VEHICLE_STOPPED
     -> PLATE_RECOGNITION -> PLATE_CONFIRMED -> COOLDOWN -> IDLE
```

The machine consumes a motion score and a track summary. It never touches an image, so it is
fully deterministic and unit tested without a GPU.

- **IDLE to APPROACHING**: a tracked plate inside the detection zone promotes immediately;
  otherwise motion must exceed `motion.threshold` for `motion.min_motion_ms`.
- **APPROACHING to NEAR**: the tracked plate's centre enters `roi.near_barrier`.
- **NEAR to STOPPED**: `StopDetector` reports stationary for `stop_detection.stop_duration_ms`
  while the plate is inside `roi.stop`.
- **STOPPED to RECOGNITION**: the pipeline opens a session and calls `markRecognitionActive`.
- **RECOGNITION to CONFIRMED**: the consensus is satisfied. Otherwise the recognition timeout
  ends the session and the machine goes straight to cooldown.
- **COOLDOWN to IDLE**: `recognition.cooldown_ms` has passed, the plate track is gone, and
  motion has been quiet for `recognition.leave_confirmation_ms`.

A vehicle that stops and stays put cannot retrigger: its track persists, so cooldown holds. A
*different* track id at the barrier after the cooldown releases immediately, which is what a
queue of cars at a parking entrance looks like.

## Stop detection

No single frame decides anything. Three signals are measured over a rolling window:

| Signal | Why |
| --- | --- |
| centre displacement, relative to plate width | scale-independent, so it works at any distance |
| relative plate width change | catches a vehicle still closing while its centre holds |
| centre speed in pixels per second | absolute floor against slow drift |

All three must stay under their thresholds continuously for `stop_detection.stop_duration_ms`.

## Motion detection

The comparison frame comes from `motion.reference_interval_ms` ago, not from the previous frame.
Differencing adjacent frames measures speed, and a vehicle creeping toward a barrier moves too
few pixels between two frames to clear any threshold that also rejects sensor noise. Comparing
across a fixed interval measures displacement, which is the quantity that separates an
approaching vehicle from an empty scene. The bundled clip made this concrete: with adjacent-frame
differencing its motion score sat at 1e-7, and the detector never woke up.

## Consensus

Observations are added as they arrive rather than batched at the end, so OCR stops the moment the
evidence is sufficient. Each observation's weight combines detector confidence, mean OCR
confidence, the weakest character in the reading, crop quality, and the validator's correction
penalty. Frequency then picks between candidates.

The weakest character is carried separately from the mean and caps the final confidence. A plate
string is only as certain as its least certain character, and a single 0.15 character inside an
otherwise strong reading is exactly what multi-frame voting exists to catch.

Acceptance needs `min_samples` valid readings, `required_votes` agreeing on the winning string,
`min_agreement` share of the total weight, `min_avg_confidence` mean confidence, and
`min_final_confidence` combined. One frame is never enough unless `allow_single_frame` is turned
on explicitly.

## Validation

Nothing about Kazakhstan is compiled into the pipeline. Layouts are slot patterns in
configuration:

```text
D  digit
L  letter from validation.letters
R  region-code digit, checked against validation.regions
```

`DDDLLLRR` is `123ABC02`. Adding a layout is a config change.

Corrections are position-aware and conservative. A digit slot only accepts a digit-shaped
substitution, a letter slot only a letter-shaped one, and a character with no configured
confusion partner is never rewritten. A reading needing more than `max_corrections` is reported
ambiguous rather than repaired. Formats compete: the layout needing the fewest repairs wins.

## Output

One `PlateRecognitionEvent` per finished session, accepted or not, delivered through `PlateSink`.
That is the whole public surface. `JsonStdoutSink` is the default; a database writer, an HTTP
client, a GPIO relay or an access-control call implements the same interface. Check
`event.status` before opening a barrier.

```json
{
  "event": "plate_recognition",
  "status": "VALID_HIGH_CONFIDENCE",
  "normalized_plate": "123ABC02",
  "raw_plate": "123A8C02",
  "confidence": 0.9024,
  "timestamp_ms": 1292,
  "camera_id": "gate-01",
  "plate_box": {"x": 842, "y": 378, "width": 545, "height": 347},
  "region_code": "02",
  "region_name": "Almaty",
  "format": "current_individual",
  "recognition_latency_ms": 125,
  "observation_count": 3,
  "agreeing_observations": 3,
  "best_crop_path": null
}
```

## Failure handling

Nothing recoverable is fatal. Camera loss, a read timeout, a bad crop, a malformed model output,
an inference failure and an invalid bounding box are all handled in place and logged. Only
startup problems exit: configuration errors return 2, an unavailable camera 3, a model or backend
failure 4.

Debug crop storage is off by default and capped by `debug.max_files` when enabled, so a process
left running for weeks cannot fill the device.
