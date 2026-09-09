# Model Evaluation

## Detector

| Field | Value |
| --- | --- |
| Production file | `models/license_plate_detector.onnx`, 11.7 MB |
| Source checkpoint | `license_plate_detector.pt`, 6.1 MB, kept from the prototype |
| Architecture | Ultralytics YOLOv8n, per the ONNX metadata |
| Input | `images`, float32, `1x3x640x640`, NCHW, RGB, letterboxed with pad 114 |
| Output | `output0`, float32, `1x5x8400` |
| Classes | `{0: "license_plate"}` |
| Training data | not recorded in the checkpoint, unknown |
| Licence | ONNX metadata declares **AGPL-3.0**, Ultralytics |
| Export | `python3 tools/export_detector_onnx.py --weights license_plate_detector.pt --imgsz 640` |

**The AGPL-3.0 marker needs a decision before release.** It is embedded in the exported model's
metadata by Ultralytics tooling. Whether it binds this deployment depends on how the weights were
obtained and how the system is distributed. Resolve it with whoever owns the weights, or retrain
on a permissively licensed base.

The detector found a plate in all 296 frames of the only local clip. That is not an accuracy
number: there is no ground truth and no negative footage.

## OCR

EasyOCR is gone from the production path. It required Python and PyTorch at runtime, was general
scene-text rather than plate-specific, took **11.7 seconds** per call on this machine, and
produced strings that were not valid Kazakhstan plates.

| Field | Value |
| --- | --- |
| Production file | `models/plate_ocr.onnx`, 5.0 MB |
| Model | Fast Plate OCR `cct-s-v2-global` |
| Architecture | Compact Convolutional Transformer |
| Input | `input`, **uint8**, `Nx64x128x3`, NHWC, RGB |
| Outputs | `plate` float32 `Nx10x37`, `region` float32 `Nx66` |
| Normalisation | inside the model; preprocessing must not scale pixels |
| Alphabet | `0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_`, 37 classes |
| Padding character | `_`, index 36 |
| Slots | 10 |
| Resize | stretch to 128x64, `keep_aspect_ratio: false`, `INTER_LINEAR` |
| Source | https://github.com/ankandrew/fast-plate-ocr |
| Fetch | `.venv-fast/bin/python tools/fetch_ocr_model.py --model cct-s-v2-global-model` |

Every one of those values is read at startup from `models/plate_ocr_config.yaml`, the file the
project ships with the model. None of it is assumed in code, and the loader refuses to start if
the YAML and the ONNX signature disagree. The `plate` head is already softmaxed, verified by
checking that each slot's 37 values sum to 1, so no softmax is applied on the C++ side.

### Decoding

Reshape to `(slots, vocabulary)`, argmax per slot, map through the alphabet, strip trailing
padding. This matches the reference implementation with two deliberate differences:

**Confidence excludes padding slots.** The reference averages all ten slots, including padding,
which the model predicts with near certainty. A six-character plate would be flattered by four
near-1.0 padding predictions. Confidence here is the mean over the characters actually returned,
and the weakest of those is carried separately into the consensus.

**Interior padding is rejected.** The reference strips only trailing padding, so a prediction of
`12_ABC02` keeps the underscore. Removing it would splice two halves into a plausible
seven-character plate; keeping it would fail validation for the wrong reason. The reading is
discarded with reason `interior_padding`. The Python prototype produced exactly this on the
sample clip: `BR451L__IL`.

### Measured latency

Fast Plate OCR on the development machine, ONNX Runtime CPU:

| | Average | p95 |
| --- | ---: | ---: |
| Preprocess | 0.07 ms | |
| Inference | 17.5 ms | |
| Total | 17.6 ms | 17.6 ms |

Against EasyOCR's 11,723 ms average on the same clip, that is roughly a 660-fold reduction.

### Kazakhstan accuracy: unmeasured

The model's own region list contains 65 countries plus `Unknown`. **Kazakhstan is not among
them.** Kazakhstan plates use Latin characters in layouts close to several post-Soviet states
that *are* in the list, so they are broadly in distribution character-wise, but no claim about
Kazakhstan accuracy can be made from anything measured here.

On the bundled clip the model read the Japanese plate `BR45IL` at 0.92 to 0.99 confidence,
alternating between `I` and `1` in position five across frames. That single confusion is handled
by the validator's letter-slot repair and settled by the multi-frame vote, which is encouraging
for the mechanism but says nothing about Kazakhstan glyphs.

Before release, evaluate on labelled Kazakhstan plates and record exact-plate accuracy, character
accuracy, and the confusion matrix. If accuracy is short, the options in order of cost are: add
a Kazakhstan-specific format weighting, fine-tune Fast Plate OCR on Kazakhstan data using its
training CLI, or train a dedicated model. Fast Plate OCR's training path produces the same ONNX
plus YAML pair, so a fine-tuned model is a file swap with no code change.

## Sample data

`video/car.mp4` is 296 frames, 1920x1080, 23.976 FPS. It shows a slow camera orbit around a
parked Nissan Skyline with the Japanese vanity plate `BR45IL`. It is useful for latency, memory,
tracking, stop detection and the event path. It is useless for Kazakhstan accuracy and it is not
a barrier approach.

No labelled Kazakhstan evaluation set exists in this repository. Night, glare, rain, dirty plate
and oblique-angle cases are entirely unrepresented.

## Required before a production release

For every model, record: architecture, input resolution, precision, file size, training dataset,
licence, export command, benchmark on the Jetson, and accuracy against a labelled Kazakhstan
evaluation manifest. Do not commit converted or downloaded model binaries without approval.
