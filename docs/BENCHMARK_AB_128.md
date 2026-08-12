# Benchmark A/B — fast vs accurate (TSFM-Enhanced)

Host CPU, INT8 models, val=1559 imgs / 39 ids, 50 latency runs. Latency is **relative** (host, not ESP32-S3).

| metric | fast | accurate |
|---|---|---|
| tflite size (KB) | 1312 | 1403 |
| ops (count) | 8 | 9 |
| latency mean (ms) | 0.5 | 0.6 |
| latency p95 (ms) | 0.6 | 0.7 |
| intra cosine | 0.346 | 0.347 |
| inter cosine | 0.103 | 0.023 |
| separation margin | 0.243 | 0.324 |
| EER threshold | 0.130 | 0.070 |
| EER | 0.328 | 0.291 |

Ops (TFLite-Micro must support all):
- **fast**: ADD, CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, L2_NORMALIZATION, MEAN, MUL, PAD
- **accurate**: ADD, CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, L2_NORMALIZATION, LOGISTIC, MEAN, MUL, PAD
