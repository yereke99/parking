# OCR Research Harness

## Decision to make

The benchmark answers two separate questions:

1. Which recognizer reads the Kazakhstan plate correctly?
2. Which compatible recognizer still meets latency, memory and thermal limits with four cameras
   on the original 4 GB Jetson Nano?

Speed does not compensate for a wrong plate. Backends are ranked by exact KZ plate accuracy,
then character error rate (CER), then four-camera OCR p95 latency and peak memory.

## The four backends

| Backend | Execution path | Research role |
| --- | --- | --- |
| `fast_plate_ocr` | Native C++ and ONNX Runtime | Small, fast global-plate baseline; existing implementation is unchanged |
| `nomeroff` | Persistent Python/PyTorch worker, explicit KZ model | Regional production candidate |
| `paddleocr` | Persistent recognition-only worker, `eslav_PP-OCRv5_mobile_rec`, ONNX Runtime | Best general OCR comparison from the earlier crop study |
| `easyocr` | Persistent recognition-only PyTorch worker, English recognizer | General scene-text control/baseline |

PaddleOCR and EasyOCR do not run their text detectors. The C++ YOLO stage already supplies a
tight crop, and adding a second document/scene detector would measure a different pipeline.

The workers receive raw BGR bytes over pipes. They are started once, warmed up before timing,
and shared across all camera pipelines. Startup and peak process-tree memory are still reported.
An OCR inference measurement therefore excludes model download and import time, while the
separate startup field keeps that operational cost visible.

## Reproducible Jetson run

Use the pinned container, power mode, clocks and cooling for every row:

```sh
sudo nvpmodel -m 0
sudo jetson_clocks

make jetson-all
```

This one command builds, validates real TensorRT GPU inference, runs the full matrix, prints the
comparison table, and writes timestamped Markdown and JSON results under `benchmark_results/`.

All research rows use the same shared native TensorRT FP16 detector. EasyOCR then performs OCR on
CUDA; the legacy Fast Plate OCR recognizer uses the documented CPU ORT fallback. To launch the
full project rather than the benchmark, run `make run` (or pass a camera with `RUN_ARGS`).

No model is loaded once per OCR call. Four-camera runs create four C++ processing threads and
four real-time frame pumps. They share one detector. Nomeroff, PaddleOCR and EasyOCR share one
serialized worker/model instance; Fast Plate OCR retains its existing per-pipeline ONNX session.
This represents a memory-bounded edge deployment rather than four unrelated processes.

To test one candidate directly:

```sh
tools/jetson_docker.sh benchmark \
  --video video/parking.mp4 \
  --ocr-backend easyocr \
  --streams 4 \
  --max-frames 300
```

## Sequential OCR benchmark

`make ocr-benchmark` answers the deployment question with one engine in memory at a time:

1. The TensorRT detector runs on every frame of each clip. Every detection that passes the
   pipeline's ROI, size and quality gates is saved as the crop OCR would receive
   (`kz_anpr_benchmark --extract-crops`).
2. Each engine then runs in its own process, one after another with a pause in between:
   - OCR alone on that identical crop set (`--ocr-crops`), after loading and warm-up. This
     gives accuracy, CER, OCR FPS and p50/p95 latency on exactly the same inputs.
   - The full pipeline over every frame of each clip, with no real-time pacing and no dropped
     frames. This gives pipeline FPS and confirmed plates.
3. Process-tree RSS (average and peak) and `tegrastats` are sampled throughout. Startup (model
   load plus warm-up) is reported separately and never counted in FPS or latency. Fallbacks,
   crashes and out-of-memory kills are listed rather than hidden.

The report prints the comparison table, details, the ranking (accuracy, OCR FPS, latency, RAM,
then the best engine for KZ plates by KZ exact accuracy, CER, p95 and peak RAM) and the total
wall-clock time. Crop accuracy uses the manifest: a clip with one label is assumed to show only
that plate, and unlabelled clips are used for speed only.

## Bundled videos and labels

| Video | Use | Label |
| --- | --- | --- |
| `video/parking.mp4` | Kazakhstan plate, primary bundled accuracy smoke test | `152JTA02` |
| `video/car.mp4` | Existing Japanese demo and regression/latency clip | `BR45IL` |
| `video/parking2.mp4` | Longer dashcam load, decode, thermal and missed-frame test | Unlabelled |

`parking2.mp4` must not produce an accuracy percentage. The single labelled KZ plate is also not
a production validation set; it detects obvious incompatibility and regression only. Add a
representative labelled manifest before making an accuracy claim.

## Output

Every run is preserved in JSON. The Markdown summary includes:

- processing threads and per-stream FPS;
- OCR average/p95 and complete stage latency;
- startup/model-load time;
- captured, processed and dropped frames;
- process-tree CPU and peak RSS;
- Jetson GPU use, system RAM, temperature, power and throttling evidence from `tegrastats`;
- exact-plate accuracy, character accuracy, CER and failure category where a label exists;
- explicit unavailable rows rather than skipped libraries.

The report separates `OCR exact` from `Accepted exact`. `OCR exact` counts the best emitted
candidate even when the pipeline marks it low-confidence; `Accepted exact` counts only a result
that would be accepted by the barrier policy. This prevents threshold tuning from being mistaken
for a recognizer error while still exposing unsafe low-confidence behaviour.

The earlier offline crop evaluation supplied with the task found Nomeroff materially more
accurate on CIS/KZ plates than global Fast Plate OCR or general OCR. That result is a selection
prior, not a Jetson benchmark: the final deployment decision must come from the generated report
on the target device.

## Expected deployment outcome

On JetPack 4, Fast Plate OCR and EasyOCR are the runnable candidates in the pinned image.
Nomeroff 4.0.1 and PaddleOCR 3.7 remain named rows but are reported unavailable because their
upstream runtime requirements cannot be satisfied honestly on the supported Nano stack. The
harness intentionally keeps all four rows so a future verified package can be compared without
changing the research schema.
