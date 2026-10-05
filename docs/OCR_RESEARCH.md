# OCR Research Harness

## Decision to make

The benchmark answers two separate questions:

1. Which recognizer reads the Kazakhstan plate correctly?
2. Which correct recognizer still meets latency, memory and thermal limits with four cameras on
   a 4 GB Jetson Orin Nano?

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

Use the same JetPack image, power mode, clocks and cooling for every row:

```sh
sudo nvpmodel -m 0
sudo jetson_clocks

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

tools/setup_nomeroff_env.sh --jetson
tools/setup_research_ocr_envs.sh --all

python3 tools/benchmark.py --research
```

No model is loaded once per OCR call. Four-camera runs create four C++ processing threads and
four real-time frame pumps. They share one detector. Nomeroff, PaddleOCR and EasyOCR share one
serialized worker/model instance; Fast Plate OCR retains its existing per-pipeline ONNX session.
This represents a memory-bounded edge deployment rather than four unrelated processes.

To test one candidate directly:

```sh
python3 tools/benchmark.py \
  --config config/research.yaml \
  --video video/parking.mp4 \
  --ocr-backend nomeroff \
  --streams 4 \
  --manifest data/manifests/video_research.csv
```

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
- OCR p50/p95 and complete stage latency;
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

Fast Plate OCR is expected to win raw speed and footprint. Nomeroff is expected to win KZ exact
accuracy and is therefore the production default. PaddleOCR is useful as a general-recognition
control but its arm64 packaging is less reliable. EasyOCR is expected to be both heavier and less
accurate on tight plate crops. The harness intentionally measures all four so these expectations
can be accepted or rejected on the actual Orin Nano instead of being treated as benchmark data.
