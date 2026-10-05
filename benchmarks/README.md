# Benchmarks

## How to run

Current reproducible multi-stream harness:

```sh
python3 tools/benchmark.py --streams 1
python3 tools/benchmark.py --streams 2
python3 tools/benchmark.py --streams 4
python3 tools/benchmark.py --matrix
```

For the four-OCR research suite over all three bundled videos:

```sh
python3 tools/benchmark.py --research
```

That command runs `fast_plate_ocr`, `nomeroff`, `paddleocr`, and `easyocr` with one and four
independent C++ processing threads. A four-thread run repeats the selected clip as four cameras;
the detector is shared and serialized, and persistent Python workers are shared so their model
weights are not duplicated. Each result records the requested OCR, actual loaded backend/model,
startup time, per-stage latency, per-stream FPS, dropped frames, CPU, process-tree RSS, GPU load,
GPU memory, temperature, power, recognition events and labelled accuracy when available.

The generated Markdown never silently drops an unavailable backend. Missing environments or an
unsupported Jetson package appear as `unavailable` rows in both Markdown and JSON.

It writes timestamped JSON and Markdown under `benchmark_results/`, samples the full process tree
(including the shared Nomeroff worker), and records GPU/Jetson telemetry when those tools exist.
The harness defaults to `config/default.yaml`, including the production KZ validation profile.
Pass `--config config/benchmark.yaml` only when exercising the bundled Japanese demo clip's event
path; that profile must not be used for a KZ accuracy result.
The historical commands and numbers below predate the Nomeroff migration and remain useful only
as the Fast Plate OCR baseline.

```sh
# Full pipeline, including the state machine, tracking, OCR and consensus.
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml

# Detector on every frame, ignoring the state machine. Comparable to the Python prototype.
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml --detector-only

# Longer sample and memory stability.
/usr/bin/time -l ./build/kz_anpr_benchmark --video video/car.mp4 \
    --config config/benchmark.yaml --repeat 20

# Machine-readable.
./build/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml --json
```

`--repeat` offsets each pass's timeline so the state machine sees monotonic time and the clip
produces one full recognition cycle per pass.

## What the sample clip can and cannot measure

`video/car.mp4` is a slow camera orbit around a parked Nissan Skyline carrying a Japanese plate
reading `BR45IL`. It is not a Kazakhstan plate and not a barrier approach.

It **does** measure detector and OCR latency, memory behaviour, the state machine, tracking, stop
detection, quality filtering, consensus and the event path. It **does not** measure Kazakhstan
recognition accuracy. `config/benchmark.yaml` differs from `config/default.yaml` only in the plate
layout, so every timing and threshold is the production one.

## Results

All numbers measured on the development machine: Apple M-series, 8 cores (6 performance), macOS,
ONNX Runtime 1.29 CPU provider, `intra_op_threads: 4`. **Not Jetson numbers.** Re-measure on the
device before accepting anything here as a deployment figure.

### End to end, one vehicle

| Metric | Value |
| --- | ---: |
| Frames in clip | 296 |
| Frames processed | 296 |
| Wall clock | 2.54 s |
| Effective FPS | 116.5 |
| Detector calls | 49 |
| Detector calls as share of frames | 16.6 % |
| OCR calls | 3 |
| Recognition sessions | 1 |
| Plates confirmed | 1 |
| Recognition timeouts | 0 |
| Stop-to-result latency | 125 ms |
| Result | `BR45IL`, confidence 0.902, 3 of 3 observations agreeing |

### Per-stage latency

| Stage | Average | p95 |
| --- | ---: | ---: |
| Motion difference, every frame | 1.29 ms | |
| Detector preprocess | 0.56 ms | |
| Detector inference | 38.0 ms | |
| Detector total | 38.5 ms | 39.8 ms |
| Quality assessment | 0.40 ms | |
| OCR preprocess | 0.09 ms | |
| OCR inference | 14.8 ms | |
| OCR total | 14.9 ms | 14.7 ms |
| Whole frame tick | 7.8 ms | 40.0 ms |

The frame tick is bimodal by design: a frame with no detector call costs about 1.3 ms, a frame
with one costs about 40 ms. The average sits near 7.8 ms because 83 percent of frames take the
cheap path.

### Against the Python prototype

Same clip, same two ONNX models, same machine.

| | Python `main_fast.py` | C++ pipeline | C++ detector-only |
| --- | ---: | ---: | ---: |
| Detector calls | 296 | 49 | 296 |
| Detector average | 42.3 ms | 38.5 ms | 38.3 ms |
| OCR calls | 59 | 3 | 0 |
| OCR average | 25.9 ms | 14.9 ms | |
| Wall clock | 14.34 s | 2.54 s | 11.48 s |
| Effective FPS | 20.6 | 116.5 | 25.8 |

The original EasyOCR prototype, `legacy_python/main.py`, is a different order of magnitude:
162 ms per detector call, **11.7 s** per OCR call, 0.40 FPS, about 1.98 GB peak RSS.

Where the two are directly comparable, per detector call, the C++ path is about 10 percent
faster. The end-to-end 5.6-fold win comes almost entirely from calling the detector 6 times less
often and OCR 20 times less often, not from a faster kernel.

### Thread sensitivity

Detector latency, `--detector-only`, same clip:

| Configuration | Average |
| --- | ---: |
| ONNX Runtime CPU, 2 intra-op threads | 64.5 ms |
| ONNX Runtime CPU, 4 intra-op threads | 38.3 ms |
| ONNX Runtime CPU, 8 intra-op threads | 44.7 ms |
| OpenCV DNN | 71.6 ms |

Oversubscribing costs about as much as undersubscribing. `intra_op_threads: 4` is the shipped
default because of this measurement; re-measure on the target, where the core count and the
performance/efficiency split differ.

### Memory

Peak resident set, full pipeline:

| Frames processed | Peak RSS |
| --- | ---: |
| 592 | 234 MB |
| 2 960 | 244 MB |
| 8 880 | 277 MB |

Broadly flat across a 15-fold increase in work: 18 percent more resident memory for 15 times the
frames, which is allocator high-water behaviour rather than accumulation. 20 consecutive
recognition cycles all confirmed, 942 detector calls, 60 OCR calls, no timeouts.

### Sanitizers

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKZ_ANPR_SANITIZERS=ON
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=0:detect_container_overflow=0 \
    ./build-asan/kz_anpr_benchmark --video video/car.mp4 --config config/benchmark.yaml --repeat 3
```

Clean over 888 frames and 3 recognition cycles, with all tests passing under the sanitizer build.

`detect_container_overflow=0` is required and is not covering up a defect in this project.
OpenCV is not built with sanitizer instrumentation, so when `cv::dnn::NMSBoxes` resizes a
`std::vector` the container annotations set by our instrumented translation unit go stale. A
twelve-line program that does nothing but call `NMSBoxes` with a `reserve()`d output vector
reproduces the identical report.

## What a production release still needs

The numbers above validate the implementation, not the deployment. Before release, measure on the
Jetson with real barrier footage:

- total vehicle events, exact plate accuracy, character accuracy;
- falsely accepted plates and rejected valid ones;
- missed vehicles and duplicate recognitions;
- stop-to-result latency at the barrier;
- detector and OCR latency with TensorRT FP16;
- average and peak CPU, GPU utilisation, peak RSS, thermal behaviour under sustained load;
- night, rain, glare, dirty plate and oblique angle cases.

Record for every number whether it came from a development machine or from the device.
