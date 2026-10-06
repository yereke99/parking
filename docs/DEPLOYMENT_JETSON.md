# Jetson Nano 4 GB Docker deployment

This path targets the original NVIDIA Jetson Nano Developer Kit, not Orin:

| Component | Pinned contract |
| --- | --- |
| Board | Jetson Nano Developer Kit, 4 GB, aarch64 |
| Host JetPack / L4T | JetPack 4.6.1-4.6.6 / L4T R32.7.x (including R32.7.6) |
| CUDA / TensorRT | CUDA 10.2 / TensorRT 8.2.1 from JetPack |
| Container | `nvcr.io/nvidia/l4t-ml:r32.7.1-py3` pinned by manifest digest |
| Control Python | CPython 3.9.25, source SHA-256 pinned; benchmark/preflight only |
| CUDA worker Python | JetPack Python 3.6 / NVIDIA PyTorch 1.10.0 / torchvision 0.11.0 |
| OpenCV / NumPy | OpenCV 4.5.0 / NumPy 1.19.5 from the L4T image |
| Project detector | Native TensorRT 8.2, CUDA, FP16, serialized engine cache |
| Native ONNX Runtime | Microsoft aarch64 CPU package 1.11.1, SHA-256 checked |
| EasyOCR | 1.6.2, recognition-only, English G2 model, SHA-256 checked |

The container intentionally does not upgrade CUDA, TensorRT, PyTorch, torchvision, OpenCV,
NumPy, SciPy or scikit-image. Those packages are ABI-coupled to the old JetPack image. Python
3.9.25 controls the benchmark; GPU OCR stays in a separate Python 3.6 process so NVIDIA's
JetPack wheel remains usable.

Python 3.9 reached upstream end-of-life on October 31, 2025. Version 3.9.25 is the final release
and is pinned here because this deployment explicitly requires 3.9; it should not be interpreted
as a currently supported general-purpose Python baseline.

## Host prerequisite

Flash JetPack 4.6.x with L4T R32.7.x and install Docker plus NVIDIA Container Runtime. The host
release format is `R32 (release), REVISION: 7.x`; the launcher parses that NVIDIA format rather
than looking for a nonexistent literal `R32.7` substring. NVIDIA published the L4T ML image for
R32.7.1, and it is used as the pinned R32.7 user-space baseline on R32.7.x hosts. Confirm Docker can see
the runtime:

```sh
docker info --format '{{json .Runtimes}}'
```

The output must contain `nvidia`. Do not install Python packages, ONNX Runtime, CMake or OCR
libraries on the host.

## One-command workflow

From the repository root on the Nano:

```sh
make jetson-all
```

`jetson-all` builds the image, runs the hardware preflight and real TensorRT inference, executes
the complete 1/4-stream four-OCR research matrix, prints its comparison table, and reports the
timestamped Markdown/JSON paths. For a bounded smoke run use
`make jetson-all MAX_FRAMES=300`.

Optional focused runs:

```sh
make docker-build
make jetson-check
make benchmark-1
make benchmark-4
make research MAX_FRAMES=300
make run
make run RUN_ARGS='--source rtsp://user:pass@camera/stream'
```

The build uses one compiler process. The run mounts the checkout and `models/` read-only; only
`benchmark_results/` is writable. Existing videos, datasets, manifests, model files and prior
results are never deleted or overwritten. Available `/dev/video*` devices and the Argus socket are
forwarded automatically for the full-project target. Every report has a timestamped JSON and
Markdown file.

## What preflight checks

`make jetson-check` fails before the benchmark when any hard contract is wrong:

- host architecture and L4T R32 release;
- registered Docker NVIDIA runtime and container GPU access;
- control Python 3.9.25 plus the isolated Python 3.6 CUDA worker stack;
- CUDA-enabled NVIDIA PyTorch, torchvision, OpenCV and NumPy versions;
- TensorRT import and version supplied by JetPack;
- native TensorRT/CUDA linkage and one real detector inference on the GPU;
- 4 GB memory profile;
- native ONNX Runtime CPU fallback for the legacy uint8 OCR model;
- detector, Fast Plate OCR, EasyOCR, videos and research manifest files.

It also prints the compatibility state of every OCR backend.

## Four-backend compatibility policy

The research harness still executes the same four named candidates and never silently drops a
row.

| Backend | Nano result | Reason |
| --- | --- | --- |
| `fast_plate_ocr` | TensorRT detector, CPU OCR | Official aarch64 ONNX Runtime 1.11.1 C/C++ package handles only this legacy uint8 recognizer |
| `easyocr` | TensorRT detector, CUDA OCR | EasyOCR 1.6.2 reuses NVIDIA PyTorch 1.10; the second text detector is disabled |
| `nomeroff` | Explicitly unavailable | Control Python is 3.9, but Nomeroff 4.0.1 also needs PyTorch >=1.12; Nano's CUDA stack is fixed at NVIDIA PyTorch 1.10 under Python 3.6 |
| `paddleocr` | Explicitly unavailable | PaddleOCR 3.7 requires a newer Python stack and PaddlePaddle does not publish a compatible JetPack 4 aarch64 wheel |

Using EasyOCR or Tesseract while labelling the row “Nomeroff” or “PaddleOCR” would invalidate the
research comparison, so no such substitution is made. The benchmark catches each unavailable
startup, continues, and writes the reason into the final table. A future verified, prebuilt
JetPack 4 package can be added without changing the C++ benchmark protocol.

## Runtime and results

The container limits BLAS/OpenMP worker counts to one. The Nano profile uses a 256 MiB TensorRT
builder workspace, FP16, two CPU threads for the Fast Plate OCR fallback, and one OpenCV thread.
Four video streams share the TensorRT detector and the active OCR worker as the existing benchmark
already defines; model weights are not multiplied four times.

The Markdown table reports every backend/video/stream combination with status, FPS per stream,
OCR average and p95 latency, peak process-tree RSS, dropped frames, recognition timeouts,
accuracy failures, exact accuracy and CER. The JSON retains full telemetry and failure modes.
`tegrastats` is mounted when present so RAM, GPU load, temperature, power and throttling evidence
are sampled during each run.

The bundled manifest contains only a very small smoke-test label set. It catches regressions but
cannot establish production accuracy; use the same mounted-manifest mechanism for a representative
labelled barrier dataset.

## GPU execution policy

JetPack supplies TensorRT 8.2.1 and CUDA 10.2. The C++ detector links those native libraries
directly, builds the bundled ONNX detector into an FP16 engine once, and reuses the serialized
engine from `benchmark_results/trt_cache`. This avoids compiling ONNX Runtime on the 4 GB board.
`strict_backend: true` prevents the detector from silently moving to CPU, and `make jetson-check`
executes one real frame and requires `backend=tensorrt`.

The detector graph is opset 12, but its newer exporter stamped ONNX IR 10. At load time the native
session normalizes that metadata byte to IR 8 in memory, which TensorRT 8.2 accepts; the mounted
ONNX file is never rewritten.

The legacy Fast Plate OCR network has a uint8 input that this TensorRT 8.2 path does not accept,
so that recognizer alone uses the pinned Microsoft aarch64 ONNX Runtime CPU package. EasyOCR uses
CUDA through NVIDIA PyTorch. `make run` selects EasyOCR by default, so both the detector and OCR
of the normal project path use the Nano GPU. `PROJECT_OCR=fast_plate_ocr make run` selects the
explicit mixed GPU-detector/CPU-OCR fallback.

## Power mode

The benchmark records state but never changes it. Set the same mode before every comparison if
your deployment policy allows it:

```sh
sudo nvpmodel -m 0
sudo jetson_clocks
```

Keep active cooling attached and compare sustained temperature/throttling, not only first-run FPS.
