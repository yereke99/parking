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

An idle barrier therefore costs almost nothing, and the GPU is busy only while a vehicle is
there.

## Data flow

```text
CameraSource (file | USB | RTSP | GStreamer)
  -> FramePump              capture thread, one-slot latest-frame handover
  -> RoiMotionDetector      every frame, downscaled ROI difference
  -> PlateDetector          YOLOv8n at a state-dependent cadence (TensorRT FP16 on the Jetson)
  -> PlateTracker           IoU plus centroid association between detector calls
  -> StopDetector           rolling displacement, size and speed window
  -> VehicleStateMachine    decides what the next frame deserves
  -> QualityAssessor        size, blur, exposure and clipping gates
  -> NomeroffOnnx           Nomeroff Net's Kazakhstan CTC model (TensorRT FP32 on the Jetson)
  -> PlateValidator         Kazakhstan slot patterns, region codes, confusion repair
  -> PlateConsensus         streaming multi-frame vote, stops early when satisfied
  -> PlateSink              PlateRecognitionEvent: JSON on stdout and in the events file
```

Camera mode (`kz_anpr --cameras`, [Hikvision cameras](CAMERAS.md)) puts the same pipeline behind
camera discovery and runs it once per camera:

```text
NetworkSnapshot             sysfs, /proc/net/route: camera LAN, GSM uplink; never changes a route
  -> discovery              SADP, ONVIF WS-Discovery, ARP table, manual and registry hosts,
                            subnet scan when needed; never sends credentials
  -> CameraRegistry         stable ids: MAC > serial > device ID > ONVIF UUID > IP
  -> RTSP preflight         one DESCRIBE with credentials per camera; a 401 is final
  -> decoder plan           nvv4l2decoder through a native GStreamer appsink, CPU fallback
  -> per camera             capture thread -> processing thread (AnprPipeline as above)
  -> shared models          one TensorRT detector, one Nomeroff OCR, calls serialized
  -> SynchronizedSink       events tagged with camera_id; var/cameras/status.json every 5 s
```

The network, discovery, RTSP, ISAPI, registry and status code (`src/net`, `src/hikvision`,
`src/cameras` except capture and the runner) uses plain POSIX sockets and procfs/sysfs, no
OpenCV, so all of it is unit tested on any machine.

## Threading and multiple streams

**One source** (`make run`, `make camera`, `--source`): one capture thread and one processing
thread.

**Capture thread.** Owns the camera handle, reads frames, publishes into a one-slot buffer that
overwrites. It also owns reconnection with exponential backoff. It never waits for inference,
because an RTSP stream that is not drained backs up in the driver and everything the pipeline
later sees is stale. A video file is the exception: there the capture thread waits for the slot
to be collected, so every frame of the clip is processed in order.

**Processing thread.** Everything else, in order: motion, detection, tracking, the state machine,
OCR and the vote.

A frame that the processing thread did not collect before the next one arrived is dropped and
counted. At a barrier the newest view of the vehicle is what matters; the one from 400 ms ago is
worth nothing. There is no queue that can grow.

**Several cameras** (camera mode, or several `--source` options) run in one process,
`MultiCameraRunner`, with one capture thread and one processing thread per camera:

```text
camera-01  capture thread -> latest-frames queue -> processing thread --+
camera-02  capture thread -> latest-frames queue -> processing thread --+--> shared detector (mutex)
camera-03  capture thread -> latest-frames queue -> processing thread --+--> shared OCR (mutex)
                                                                         \--> SynchronizedSink
```

- The capture thread of a camera drives its GStreamer pipeline (`GstCapture`: reads with a
  timeout, the bus error text, I420 frames from the hardware converter), keeps a bounded queue of
  the newest frames (`capture.queue_size`, the oldest is dropped and counted) and owns reconnection
  through `ReconnectPolicy`: exponential backoff for network failures, one attempt every 15
  minutes and at most two retries for a rejected password, because Hikvision locks an address out
  after a few failed logins.
- The processing thread of a camera owns that camera's `AnprPipeline`: motion, tracking, stop
  detection, the state machine, consensus and event identity are per camera and share nothing.
  Frames older than `capture.max_frame_age_ms` at pick-up are dropped as stale; I420 frames are
  converted to BGR only here, so dropped frames cost no conversion.
- The detector and the OCR exist once. Each is one TensorRT engine whose calls are serialized by
  a mutex, which bounds CUDA memory on the 4 GB board; the threads take turns, so a slow or dead
  camera never blocks the others. If a later batch benchmark proves beneficial, the OCR
  abstraction already exposes batch recognition.
- Events from all cameras go through `SynchronizedSink`, so JSON lines never interleave. The
  runner writes `var/cameras/status.json`, logs per-camera and system summaries, watches the
  available RAM, and adds cameras that appear later through periodic rediscovery.

## Inference backends

`IInferenceSession` hides the runtime. Backend selection happens once, in
`createInferenceSession`, and nowhere else: no business-logic file contains a hardware branch.

| Backend | Used for |
| --- | --- |
| `tensorrt` | Jetson. Native TensorRT 8.2 with a serialized engine cache in `inference.engine_cache_dir`: the detector in FP16, the OCR in FP32 |
| `onnx_cuda` | Any CUDA GPU without TensorRT |
| `onnx_cpu` | Portable fallback, and the development default on x86 and macOS |
| `opencv_dnn` | Development last resort when ONNX Runtime is not linked; depends on the OpenCV build's ONNX importer |

`auto` walks that list and takes the first that loads; `tensorrt` falls back to ONNX Runtime.
`strict_backend: true` turns a detector fallback into a startup failure, which is what the Jetson
profile runs with. The OCR may still fall back to the ONNX Runtime CPU package if TensorRT refuses
its model, so a refused engine never stops the barrier; `model_loaded` logs which backend each
model got.

The Jetson image links the compact native TensorRT session instead of compiling ONNX Runtime on a
4 GB board; Microsoft's prebuilt aarch64 CPU package provides the fallback. Both implement the same
`IInferenceSession` contract.

Buffers are allocated once. Each session owns its input and output host buffers, and the detector
and the OCR hold `cv::Mat` headers directly over their input tensors, so `cv::split` writes the
planar NCHW layout in place. The OCR receives only the quality-selected plate crop.

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
That is the whole public surface. `JsonStdoutSink` writes every event to stdout, and
`JsonLinesFileSink` (`--events-file`) appends it to a file; a database writer, an HTTP client, a
GPIO relay or an access-control call implements the same interface. Check `event.status` before
opening a barrier. Logs go to stderr, so stdout carries nothing but events.

```json
{
  "event": "plate_recognition",
  "status": "VALID_HIGH_CONFIDENCE",
  "normalized_plate": "152JTA02",
  "raw_plate": "152JTA02",
  "confidence": 0.8309,
  "timestamp_ms": 8533,
  "time": "2026-10-08T07:12:03.120Z",
  "camera_id": "gate-01",
  "plate_box": {"x": 414, "y": 340, "width": 143, "height": 49},
  "region_code": "02",
  "region_name": "Almaty",
  "format": "current_individual",
  "recognition_latency_ms": 4267,
  "observation_count": 30,
  "agreeing_observations": 3,
  "best_crop_path": null
}
```

`time` is the wall clock in UTC. `timestamp_ms` is pipeline time: the position in the clip for a
file, a monotonic clock for a camera.

## Failure handling

Nothing recoverable is fatal. Camera loss, a read timeout, a bad crop, a malformed model output,
an inference failure and an invalid bounding box are all handled in place and logged. Only
startup problems exit: configuration errors return 2, an unavailable camera 3, a model or backend
failure 4. In camera mode a failing camera is never fatal to the others: each problem is logged
once with a stable code (`error=RTSP_AUTH_FAILED`), the camera's id and address, and one suggested
action, and the camera is retried on its own schedule.

Debug crop storage is off by default and capped by `debug.max_files` when enabled, so a process
left running for weeks cannot fill the device.
