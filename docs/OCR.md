# OCR: Nomeroff Net's Kazakhstan model

The plate reader is Nomeroff Net 4.0.1's dedicated Kazakhstan model (`kz`), run inside the C++
process by TensorRT. It was chosen over Fast Plate OCR, EasyOCR and PaddleOCR after a comparison
on the same plate crops (kept on the `research-archive` branch): on the bundled clip it reads the
correct `152JTA02` where Fast Plate OCR's global model settles on `152JTA10`, and it is small
enough for the Nano's GPU.

## The model

| Field | Value |
| --- | --- |
| Source | Nomeroff Net 4.0.1, model card `nomeroff-net-ocr-kz/model-4.json` |
| Checkpoint | `anpr_ocr_kz_2022_11_14.ckpt`, SHA-256 `b8d09e77dc0d212cf4a9f266e2ba2b18bd655dfcb45582195d64d647ca37ac28` |
| Network | ResNet-18 trunk up to layer3, linear 1024 to 512, two bidirectional LSTMs (hidden 32), linear CTC head |
| Input | 1x3x50x200 float32, RGB, CHW |
| Output | 13 time steps x 37 classes: the CTC blank, then `0-9A-Z` |
| Cost | about 0.5 GFLOP per crop |
| On the Jetson Nano | TensorRT FP32 engine, about 10 ms per crop; the engine builds in about 30 s on first start |

Nomeroff itself needs Python 3.9 and PyTorch 1.12 or newer, which JetPack 4 cannot provide. The
image build therefore rebuilds the network with the image's PyTorch 1.10
(`tools/export_nomeroff_onnx.py`), loads the published checkpoint and writes a batch-1 ONNX model
(opset 11, static shapes, explicit zero LSTM states). Before writing, the script compares the
export graph with a line-for-line copy of Nomeroff's own forward pass. ONNX Runtime's output on
580 real plate crops matched PyTorch within 2.2e-5, and the C++ readings equalled the PyTorch
model's on all 579 crops of the development clips.

The engine is built in FP32: the network is small, so FP16 would save little, and the LSTMs are
where reduced precision is most likely to change a reading.

## One crop, step by step

`NomeroffOnnx` (`src/ocr/nomeroff_onnx.cpp`) reproduces Nomeroff's preprocessing exactly:

1. BGR to RGB (Nomeroff swaps the channels of the BGR crops it is given);
2. resize to 200x50, bilinear;
3. min-max scaling of the whole crop to 0..1 (`cv::normalize`, `NORM_MINMAX`);
4. HWC to the CHW input tensor, written in place through fixed `cv::Mat` headers.

Greedy CTC decoding (`src/ocr/nomeroff_decoding.cpp`) takes the softmax argmax per step, collapses
repeats and drops blanks. A character's confidence is the highest probability among the steps that
produced it; the reading carries the mean and the weakest character.

## Gates

| Rejection | When |
| --- | --- |
| `no_text` | nothing was decoded |
| `weak_character` | any character below `ocr.min_char_confidence` |
| `low_confidence` | mean below `ocr.min_confidence` |

A reading that passes goes to the Kazakhstan format validator and then to the multi-frame vote; a
plate is confirmed only when `consensus.required_votes` readings agree (three in the Jetson
profile). `ocr.max_attempts` caps the OCR calls per vehicle. The Jetson profile uses
`min_confidence: 0.40`, `min_char_confidence: 0.20` and `max_attempts: 60`.

Crops below `quality.min_plate_width_px` x `quality.min_plate_height_px` (90 x 22 on the Jetson)
never reach the OCR: smaller characters are only 6-8 px wide, and a 66x20 crop of `152JTA02` was
once confirmed as `152JT02`.

## Limits

- There is no labelled Kazakhstan evaluation set in this repository. The model's accuracy at a
  real barrier depends on distance, angle, shutter speed, glare and the plate mix: measure it on
  footage from the actual camera before relying on it.
- When the detector's box cuts into the KZ emblem or the region box, readings can gain a leading
  letter or misread the region. The size gate and the three-vote consensus absorb most of this;
  a vehicle whose readings never agree ends as `LOW_CONFIDENCE` instead of being confirmed.
- Only the ordinary current layouts are validated (`123ABC02`, `123AB02`); see
  [Kazakhstan plate formats](KAZAKHSTAN_PLATES.md). Foreign plates are not read.

## Licence

Nomeroff Net is published under GPL-3.0. The runtime does not import Nomeroff's code, but it runs
the network and weights of its release. Obtain a licensing review before distributing a
proprietary product; this document is not a legal conclusion.
