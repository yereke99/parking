# Nomeroff Net Integration

## Pinned upstream

| Field | Value |
| --- | --- |
| Repository | `https://github.com/ria-com/nomeroff-net` |
| Version | 4.0.1 |
| Commit | `931388550b83f045c0ac951a77daa23df22f962d` |
| Python (validated locally) | 3.11.15 |
| Local PyTorch (validated) | 2.14.1 CPU/MPS build |
| Local platform | macOS ARM64 |

The v4.0.0 release tag was evaluated first. Its installed package omits
`number_plate_keypoints_detectors` because that directory has no `__init__.py`, and the text
reader then fails to import. The pinned 4.0.1 commit fixes packaging, uses lazy top-level imports,
and retains the same crop OCR API/model definitions. Production must not track `master`.

The local dependency closure is frozen in `requirements/nomeroff-local.lock`. Jetson deliberately
uses a separate setup path because PyTorch must match JetPack/CUDA; the setup refuses to accept a
changed PyTorch version.

Record the following on every Jetson result: JetPack, Python, NVIDIA PyTorch, CUDA, power mode and
the Nomeroff commit. `tools/benchmark.py` records the fields it can inspect automatically.

## Architecture decision

Architecture A is implemented:

```text
bounded camera pumps
  -> existing YOLO plate detector (one shared model)
  -> independent per-camera tracker and stop state
  -> quality-selected crop
  -> Nomeroff crop OCR (one shared persistent worker/model)
  -> configured KZ validation and position-aware correction
  -> weighted temporal consensus
  -> unchanged PlateRecognitionEvent / PlateSink
```

The existing detector is retained because it already detects the bundled clip, has state-based
sampling, and avoids loading Nomeroff's second detector/keypoint/classifier stack. Architecture B
(full Nomeroff detection and geometry) is not presented as faster or more accurate: no labelled
KZ barrier set exists here, and loading the extra models is especially risky on a 4 GB Jetson.
Benchmark it later only if difficult perspective crops demonstrate a measurable accuracy gap.

## Current API usage

Nomeroff v4 defines dedicated `kz`, `ru`, `by`, `kg` and ex-USSR OCR models in
`NumberPlateTextReading.DEFAULT_PRESETS`. The adapter uses the same `TextDetector` crop path with
classification disabled and an explicit configured label. `ocr.region_mode: kz` is the default.
This is intentional: upstream documents that its options classifier is mainly configured for
Ukrainian plates, so it is not trusted to route Kazakhstan parking traffic.

`validation.profile: auto` selects the built-in position-aware KZ or RU ordinary-plate rules.
The RU rules cover one leading legal plate letter, three digits, two legal letters, and a two- or
three-digit region suffix. BY/KG/SU OCR models are available, but starting those modes with the
automatic validation profile is rejected: the deployment must provide audited `custom` layouts
instead of silently reusing Kazakhstan rules.

Two-line input is available with `ocr.lines_count: 2`. Upstream routes KZ/RU/BY/KG two-line plates
through its generic `eu_2lines_efficientnet_b2` model and splitter; it is not a dedicated KZ box
model. Treat it as a separate measured camera-lane setting, not an automatic fallback.

The worker derives mean and weakest-character confidence from softmaxed CTC time-step logits,
excluding blank/repeated steps. Raw Nomeroff text remains `raw_plate`; format-aware validation
creates `normalized_plate`. A low mean or weak character is rejected before consensus.

## Process and memory model

The C++ adapter starts one worker and speaks a framed stdin/stdout protocol containing raw BGR
crops. It does not JPEG-encode crops and does not start Python per request. Identical OCR configs
share the worker across every stream. Access is serialized because the upstream model is not
thread-safe; batching is exposed by the recognizer abstraction and can be enabled later only if
the benchmark shows a gain at acceptable latency.

The validated local KZ worker used about 599 MB peak RSS after model load/warm-up. This is a local
measurement, not a Jetson result. CUDA allocations, unified-memory pressure and 1/2/4-stream
headroom still require the Orin Nano 4 GB benchmark.

## Device behavior

- `cuda`: must be available or startup fails.
- `mps`: must be available or startup fails.
- `cpu`: explicit CPU path.
- `auto`: CUDA first; otherwise tries MPS and performs real inference. An unsupported MPS
  operation causes a logged CPU fallback. CUDA OOM is reported as `gpu_oom`, not hidden by cache
  clearing or a silent backend change.
- FP16 is enabled only on CUDA. CPU and MPS remain FP32.

## Installation

Local development:

```sh
tools/setup_nomeroff_env.sh
```

Jetson after installing NVIDIA's matching PyTorch/torchvision and `python3-opencv`:

```sh
tools/setup_nomeroff_env.sh --jetson
```

The final probe downloads the configured regional model to `models/nomeroff/` and executes a
warm-up. Copy/populate that ignored cache before deploying to an offline site.

## Jetson Nano / JetPack 4: `nomeroff_onnx`

Nomeroff 4.0.1 needs Python >= 3.9 and PyTorch >= 1.12; JetPack 4's CUDA stack is Python 3.6 with
NVIDIA PyTorch 1.10, so the worker above cannot run on the Nano. The `kz` text reader alone runs
there through ONNX instead:

- `Dockerfile.jetson-nano` downloads the model card's checkpoint
  (`https://nomeroff.net.ua/models/ocr/kz/torch/model_v3.3/anpr_ocr_kz_2022_11_14.ckpt`,
  SHA-256 `b8d09e77dc0d212cf4a9f266e2ba2b18bd655dfcb45582195d64d647ca37ac28`).
- `tools/export_nomeroff_onnx.py` rebuilds NPOcrNet (resnet18 trunk to layer3, linear 512, two
  bidirectional LSTMs with hidden 32, 37-class CTC head) with PyTorch 1.10, checks the export
  graph against a copy of Nomeroff's forward pass and writes a batch-1 ONNX model.
- `ocr.backend: nomeroff_onnx` runs it in the C++ process (TensorRT FP32 on the Nano, ONNX Runtime
  elsewhere) with the worker's preprocessing, greedy CTC decoding and confidence gates. Only the
  one-line `kz` model is exported, so it requires `ocr.region_mode: kz` and `ocr.lines_count: 1`.

On the 579 plate crops of the bundled clips the C++ readings equal the PyTorch model's, with
confidences within 5e-5.

## License flag

Nomeroff Net declares GNU GPL-3.0. This implementation imports and executes Nomeroff directly in
a persistent child process and distributes configuration/code intended to invoke it; it is not a
remote third-party API. That integration may have implications for distribution of a proprietary
parking application. This document does not make a legal conclusion—obtain qualified licensing
review before production distribution. The existing detector's Ultralytics/AGPL provenance is a
separate unresolved licensing item documented in `MODEL_EVALUATION.md`.
