# VelaPaw host pipeline — train & export the on-device embedding model

Off-device Python (TensorFlow) that produces the INT8 `.tflite` embedding model
the device app runs via `infer/backend_tflm.cc`. Run on a GPU machine, **not**
in the openVela build.

## Approach (IMPLEMENTATION_PLAN.md §4)

Open-set identity by **embeddings**, not N-way softmax: a backbone produces a
fixed-length L2-normalized embedding; identity = cosine similarity vs. enrolled
pets. Training uses a margin head (ArcFace) over known classes; **only the
backbone→embedding sub-model is exported** (the head is training-only). This lets
new pets be enrolled on-device without retraining.

Two interchangeable models for the benchmark panel:
- **fast** — MobileNetV3-Small backbone + embedding head.
- **accurate** — same + an attention module (the operator's TSFM thesis block;
  a placeholder insertion point is provided — replace it).

## Pipeline

```bash
pip install -r requirements.txt

# 1. Train (fp32 checkpoint). See data/README.md for dataset layout.
python train_embedding.py --variant fast     --data data/train --epochs 30
python train_embedding.py --variant accurate --data data/train --epochs 30

# 2. INT8 quantize + export -> .tflite + C array + meta.json
python quantize_export.py --variant fast     --repr data/repr
python quantize_export.py --variant accurate --repr data/repr

# 3. Check the embedding separates identities (and pick THRESHOLD)
python eval_separability.py --variant accurate --data data/val
```

Outputs land in `export/<variant>/`:
`<variant>.tflite`, `<variant>_model_data.{cc,h}` (C array), `meta.json`
(input shape, normalization, embed dim, suggested threshold).

## Wiring the model into the device

`backend_tflm.cc` currently embeds `person_detect` (placeholder). To use a real
model: point the model-gen at `export/<variant>/<variant>.tflite`, set
`VELAPAW_EMBED_DIM` to `meta.json:embed_dim`, match the device preprocessing to
`meta.json` (input size / channels / normalization), and extend the TFLM
`MicroMutableOpResolver` in `backend_tflm.cc` to cover the model's ops.

> **TFLM op-support caveat (plan risk #2):** MobileNetV3 uses hard-swish and
> squeeze-excite (→ HARD_SWISH, MUL, ADD, LOGISTIC, MEAN ops). Verify every op
> is in TFLite-Micro before committing the accurate model; `quantize_export.py`
> prints the op list. If unsupported, use `--no-hardswish --no-se` (relu, no SE)
> for a TFLM-safe variant.

> **Latency caveat:** absolute inference time must be measured on the ESP32-S3
> (LX7), not the emulator. See docs/TFLM_SPIKE_REPORT.md.
