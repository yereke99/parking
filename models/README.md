# Models

Do not commit converted or downloaded model binaries, or generated TensorRT engines, unless
explicitly approved. `.gitignore` excludes `*.onnx`, `*.engine`, `*.plan` and `models/trt_cache/`.

## Production files

| File | What it is |
| --- | --- |
| `license_plate_detector.onnx` | YOLOv8n plate detector, exported from `../license_plate_detector.pt` |
| `plate_ocr.onnx` | Fast Plate OCR `cct-s-v2-global` |
| `plate_ocr_config.yaml` | the OCR model's contract, read at startup |
| `trt_cache/` | serialised TensorRT engines, generated on the device |
| `easyocr-onnx/english_g2_<width>.onnx` | EasyOCR recognizer per input width, exported by `tools/export_easyocr_onnx.py` (inside the Jetson image at `/opt/kz-anpr/models/easyocr-onnx`) |

`../license_plate_detector.pt` is the Ultralytics checkpoint kept from the prototype. The C++
runtime never loads `.pt` files and never invokes Python.

## Producing them

```sh
python3 tools/export_detector_onnx.py --weights license_plate_detector.pt --imgsz 640
mv license_plate_detector.onnx models/

.venv-fast/bin/python tools/fetch_ocr_model.py --model cct-s-v2-global-model
```

On the Jetson, build the engine cache once after deployment:

```sh
tools/build_trt_engines.sh config/default.yaml ./build/kz_anpr
```

## Verifying a swap

```sh
./build/kz_anpr --config config/default.yaml --warmup
```

The OCR loader cross-checks `plate_ocr_config.yaml` against the ONNX input signature and the
plate head size, so a mismatched pair fails at startup rather than producing quiet nonsense.

See [model evaluation](../docs/MODEL_EVALUATION.md) for the full contract of each model, the
licence question on the detector, and what still needs measuring.
