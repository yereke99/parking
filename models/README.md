# Models

Model files are exported or downloaded, never committed: `.gitignore` excludes `*.onnx`,
`*.engine`, `*.plan` and `models/nomeroff/`. Nothing is downloaded at run time.

| File | What it is | Where it comes from |
| --- | --- | --- |
| `models/license_plate_detector.onnx` | YOLOv8n plate detector, 1x3x640x640 input, one class | exported from `../license_plate_detector.pt`, see below |
| `/opt/kz-anpr/models/nomeroff-onnx/kz.onnx` (in the image) | Nomeroff Net's Kazakhstan OCR | made by `make docker-build` ([OCR](../docs/OCR.md)) |
| `models/nomeroff-onnx/kz.onnx` | the same OCR model, for a development machine | `tools/export_nomeroff_onnx.py`, see below |
| `models/nomeroff/anpr_ocr_kz_2022_11_14.ckpt` | optional local copy of the Nomeroff checkpoint | used by the image build instead of the download |
| `var/trt_cache/` | TensorRT engines | built on the Jetson on the first start |

## The plate detector

`../license_plate_detector.pt` is the Ultralytics checkpoint the project started from. Export it
on any PC with Python 3.8 or newer, then copy the result to `models/` on the Jetson:

```sh
python3 -m pip install ultralytics      # installs what the ONNX export needs on first use
python3 tools/export_detector_onnx.py --weights license_plate_detector.pt --imgsz 640
mv license_plate_detector.onnx models/
scp models/license_plate_detector.onnx jetson:~/parking/models/
```

The graph is opset 12. Newer exporters stamp ONNX IR version 10, which TensorRT 8.2 rejects; the
TensorRT session rewrites that version byte to 8 in memory when it loads the model, and the file
on disk is never changed.

## The OCR model on a development machine

The Jetson image makes its own copy. For `config/default.yaml` on a PC:

```sh
curl -fLO https://nomeroff.net.ua/models/ocr/kz/torch/model_v3.3/anpr_ocr_kz_2022_11_14.ckpt
echo "b8d09e77dc0d212cf4a9f266e2ba2b18bd655dfcb45582195d64d647ca37ac28  anpr_ocr_kz_2022_11_14.ckpt" \
  | sha256sum -c -                        # macOS: shasum -a 256 -c -
python3 -m pip install torch
python3 tools/export_nomeroff_onnx.py --checkpoint anpr_ocr_kz_2022_11_14.ckpt \
  --output models/nomeroff-onnx/kz.onnx
```

The same checkpoint, placed at `models/nomeroff/anpr_ocr_kz_2022_11_14.ckpt`, spares the Jetson
image build its download.

## Verifying a swap

```sh
./build/kz_anpr --config config/default.yaml --warmup   # development machine
make check                                             # Jetson
```

Both load the models and run one inference each; a missing or mismatched file fails here with
exit code 4 instead of during a run.

## Licences

- The detector's ONNX metadata declares Ultralytics' **AGPL-3.0**; how the weights were trained is
  not recorded. Resolve this with whoever owns the weights, or retrain on a permissively licensed
  base, before distributing the system.
- Nomeroff Net is GPL-3.0 ([OCR](../docs/OCR.md#licence)).
