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
| OpenCV / NumPy / scikit-image | OpenCV 4.5.0 / NumPy 1.19.5 from L4T; scikit-image 0.17.2 built against that NumPy |
| Project detector | Native TensorRT 8.2, CUDA, FP16, serialized engine cache |
| Native ONNX Runtime | Microsoft aarch64 CPU package 1.11.1, SHA-256 checked |
| EasyOCR | 1.6.2, recognition-only, English G2 model, SHA-256 checked; also exported to ONNX for TensorRT |

The container intentionally does not upgrade CUDA, TensorRT, PyTorch, torchvision, OpenCV,
NumPy or SciPy. Those packages are ABI-coupled to the old JetPack image. EasyOCR's required
`skimage` module uses 0.17.2, the last Python 3.6 release. An isolated Docker builder compiles
its wheel against the same L4T NumPy, with pinned build tools and one Cython/compiler job.
Runtime installs that wheel and its pinned dependencies with `--no-deps`. Bionic's 0.13.1
package is incompatible with NumPy 1.19.5 (`_validate_lengths` was removed). The image build
checks imports, array cropping, and offline EasyOCR model loading/inference on CPU; preflight
checks GPU access. Python 3.9.25 controls the benchmark; GPU OCR stays in a separate Python 3.6
process so NVIDIA's JetPack wheel remains usable.

pip never downloads anything during the image build; its 15-second read timeout failed on the
Nano's connection. `tools/docker/fetch-wheels.sh` fetches every wheel listed in
`requirements/jetson-nano-wheels.txt` and `requirements/jetson-nano-build-wheels.txt` with
retries and SHA-256 checks, and pip installs them with `--no-index`. After changing a lock file,
regenerate its list:

```sh
tools/docker/wheel_manifest.py requirements/jetson-nano.lock > requirements/jetson-nano-wheels.txt
tools/docker/wheel_manifest.py requirements/jetson-nano-build.lock > requirements/jetson-nano-build-wheels.txt
```

CUDA, cuDNN and TensorRT are not installed in the image. On JetPack 4 the NVIDIA container runtime
mounts the host's copies read-only into every container (libraries, headers and `nvcc`, as listed
in `/etc/nvidia-container-runtime/host-files-for-container.d/*.csv`), and into every
`docker build` step when `nvidia` is Docker's default runtime. The builder compiles against those
files and `docker run --runtime nvidia` supplies the same libraries at run time. Installing these
packages with apt inside the image cannot work: dpkg fails on the mounted paths with
`Read-only file system` or `Invalid cross-device link`. The Jetson host remains unchanged.

Python 3.9 reached upstream end-of-life on October 31, 2025. Version 3.9.25 is the final release
and is pinned here because this deployment explicitly requires 3.9; it should not be interpreted
as a currently supported general-purpose Python baseline.

## Host prerequisite

Flash JetPack 4.6.x with L4T R32.7.x, including its CUDA and TensorRT components
(`sudo apt-get install nvidia-jetpack` adds them if they are missing), and install Docker plus
NVIDIA Container Runtime. The host release format is `R32 (release), REVISION: 7.x`; the launcher
parses that NVIDIA format rather than looking for a nonexistent literal `R32.7` substring. NVIDIA
published the L4T ML image for R32.7.1, and it is used as the pinned R32.7 user-space baseline on
R32.7.x hosts. CUDA, TensorRT and JetPack driver libraries such as `libnvmedia` and
`libnvdla_compiler` are injected by NVIDIA Container Runtime; make `nvidia` Docker's default
runtime in `/etc/docker/daemon.json` so they are also available during `docker build`:

```json
{
    "runtimes": {
        "nvidia": {
            "path": "nvidia-container-runtime",
            "runtimeArgs": []
        }
    },
    "default-runtime": "nvidia"
}
```

Restart Docker with `sudo systemctl restart docker`, then confirm both the registered runtimes and
the default:

```sh
docker info --format '{{json .Runtimes}}'
docker info --format '{{.DefaultRuntime}}'
```

The first output must contain `nvidia`, and the second must be `nvidia`. `make docker-build`
verifies this and the host CUDA/TensorRT files before it starts, and uses Docker's classic builder
because BuildKit does not run build steps through the default runtime. Do not install Python
packages, ONNX Runtime, CMake or OCR libraries on the host.

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
- native ONNX Runtime CPU fallback, used by any OCR model TensorRT refuses;
- detector, Fast Plate OCR, EasyOCR, videos and research manifest files.

It also prints the compatibility state of every OCR backend.

## Four-backend compatibility policy

The research harness still executes the same four named candidates and never silently drops a
row.

| Backend | Nano result | Reason |
| --- | --- | --- |
| `fast_plate_ocr` | TensorRT detector, TensorRT OCR | The image gives the published uint8-input model a float32 input (`tools/convert_fast_plate_ocr.py`), which TensorRT 8.2 accepts; FP32 engine, ONNX Runtime 1.11.1 CPU fallback |
| `easyocr` | TensorRT detector, CUDA OCR | EasyOCR 1.6.2 reuses NVIDIA PyTorch 1.10; the second text detector is disabled |
| `nomeroff_onnx` | TensorRT detector, TensorRT OCR | Nomeroff 4.0.1's `kz` model rebuilt and exported with the image's PyTorch 1.10 (`tools/export_nomeroff_onnx.py`), run in-process; FP32 engine, ONNX Runtime CPU fallback. `make run` default |
| `easyocr_onnx` | TensorRT detector, TensorRT OCR | The same EasyOCR recognizer exported to ONNX at image build, run in-process |
| `nomeroff` | Explicitly unavailable | Control Python is 3.9, but Nomeroff 4.0.1 also needs PyTorch >=1.12; Nano's CUDA stack is fixed at NVIDIA PyTorch 1.10 under Python 3.6 |
| `paddleocr` | Explicitly unavailable | PaddleOCR 3.7 requires a newer Python stack and PaddlePaddle does not publish a compatible JetPack 4 aarch64 wheel |

Using EasyOCR or Tesseract while labelling the row “Nomeroff” or “PaddleOCR” would invalidate the
research comparison, so no such substitution is made. The benchmark catches each unavailable
startup, continues, and writes the reason into the final table. A future verified, prebuilt
JetPack 4 package can be added without changing the C++ benchmark protocol.

## Runtime and results

The container limits BLAS/OpenMP worker counts to one. The Nano profile uses a 256 MiB TensorRT
builder workspace, FP16, two CPU threads for the ONNX Runtime fallback, and one OpenCV thread.
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

The published Fast Plate OCR network takes a uint8 input, which TensorRT 8.2 cannot bind. The
image build therefore writes `cct_s_v2_global_float.onnx` beside it with
`tools/convert_fast_plate_ocr.py`: the one graph input is retyped to float32, a one-byte change,
and the model's first node, already a Cast to float32, becomes a no-op. The C++ recognizer feeds
the same 0-255 pixel values as float32, so its readings match the original model (identical on the
bundled clips under ONNX Runtime). `PROJECT_OCR=fast_plate_ocr make run` builds an FP32 engine
for it, about 2 GFLOP per crop, so FP16 would save little; the first run logs
`event=tensorrt_engine_build tag=ocr` and caches the engine next to the detector's. If TensorRT
rejects the model, the recognizer runs on the pinned Microsoft aarch64 ONNX Runtime CPU package as
before and `model_loaded` says so; the uint8 original always runs there.

`make run` uses `nomeroff_onnx`, Nomeroff Net's dedicated Kazakhstan model. Nomeroff 4.0.1
itself cannot be installed on JetPack 4, but its `kz` text reader is a small network: a ResNet-18
trunk up to layer3, a linear layer, two bidirectional LSTMs and a CTC head. The image build
downloads the published checkpoint (`anpr_ocr_kz_2022_11_14.ckpt`, SHA-256 checked), rebuilds
the network with the image's PyTorch 1.10 and exports a batch-1 ONNX model to
`/opt/kz-anpr/models/nomeroff-onnx/kz.onnx` (`tools/export_nomeroff_onnx.py`, which first checks
the export against Nomeroff's own forward pass). The C++ backend reproduces the `nomeroff`
worker's preprocessing (channel swap, 200x50 bilinear resize, min-max scaling) and its greedy CTC
decoding and confidences; on all 579 plate crops of the bundled clips its readings equal the
PyTorch model's. It runs as an FP32 TensorRT engine of about 0.5 GFLOP per crop; the first run
logs `event=tensorrt_engine_build tag=ocr`, and if TensorRT rejects the model it runs on ONNX
Runtime CPU and `model_loaded` says so. On `parking.mp4` its most frequent reading is the correct
152JTA02 (50 of 233 crops), where Fast Plate OCR's global model settles on 152JTA10. Once the car
stops, the detector's crops cut into the plate's KZ emblem, so many readings gain a leading letter
or misread the 02 region box, and the 60-attempt consensus can still end LOW_CONFIDENCE. The model
does not read the Japanese demo plate in `car.mp4`, which is outside its training formats.

nomeroff.net.ua often resets the download to the Nano; it resumes where it stopped, up to 40
attempts. To skip it, copy the file to `models/nomeroff/anpr_ocr_kz_2022_11_14.ckpt` in the
checkout before `make docker-build` (for example with `scp` from a machine that has it); a copy
with the right SHA-256 is used instead of the download.

`PROJECT_OCR=easyocr_onnx make run` uses EasyOCR. The image build exports EasyOCR's recognizer with its own
PyTorch 1.10 (`tools/export_easyocr_onnx.py`) once per input width EasyOCR can use, 64 to 384 px,
into `/opt/kz-anpr/models/easyocr-onnx`. The C++ backend picks the width EasyOCR would pad the
crop to and reproduces its preprocessing, 0-9A-Z allowlist, contrast retry and score; on the
bundled clips its readings match the PyTorch worker crop for crop. It shares the detector's CUDA
context, so the roughly 2 GB PyTorch worker and its 75 s start disappear. The first run logs
`event=tensorrt_engine_build` for each width and caches the engines next to the detector's. If
TensorRT rejects an OCR engine, that width runs on ONNX Runtime CPU and `model_loaded` says so.

Both `kz_anpr` models warm up before the camera opens, so TensorRT's lazy first-inference setup
no longer stalls the first frames. With `camera.process_every_file_frame: true` (the Jetson
profile) a video file is processed frame by frame; cameras and RTSP still keep only the newest
frame.

The PyTorch worker (`PROJECT_OCR=easyocr make run`) remains available. A cold CUDA EasyOCR start
shares the Nano's 4 GB with the TensorRT detector and can take minutes.
The worker prints one `event=research_ocr_stage` line per stage (`import_torch`, `load_model`,
`warmup`, `ready`) with `mem_available_mb`, so a slow start shows where it waits. On the Jetson it
runs without cuDNN (`KZ_ANPR_EASYOCR_CUDNN=0`), whose kernels would cost several hundred MB more.
`make run` gives the CUDA attempt 240 s in a child process; if it fails or is still loading, the
child is killed and the same model is served on CPU. The run then logs
`event=research_ocr_ready device=cpu fallback=<reason>` instead of stopping. Raise the limit with
`OCR_CUDA_TIMEOUT_S=420 make run`; `ocr.startup_timeout_ms` (600 s) must exceed it plus the CPU
load. Benchmarks never fall back, so a CPU result cannot appear as a CUDA row.

## Power mode

The benchmark records state but never changes it. Set the same mode before every comparison if
your deployment policy allows it:

```sh
sudo nvpmodel -m 0
sudo jetson_clocks
```

Keep active cooling attached and compare sustained temperature/throttling, not only first-run FPS.
