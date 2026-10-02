# Data

Do not commit raw customer footage or large external datasets.

Use this structure:

```text
data/
  manifests/
  raw/        # ignored
  processed/  # ignored
```

Each external dataset entry must record:

- source;
- URL;
- license;
- intended usage;
- download date;
- whether redistribution is allowed;
- split group identifier so frames from the same vehicle/event do not leak between train/test.

For benchmark manifests, set `expected_plate` and `plate_region` (`kz`, `ru`, or another CIS
model label). The harness reports overall and per-region exact accuracy, character accuracy, and
CER. `region_code` remains the numeric subdivision printed on the plate and is not a country tag.

The repository currently contains only `video/car.mp4`, which is not enough for production accuracy claims.
