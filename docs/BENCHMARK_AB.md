# Benchmark A/B — fast vs accurate (TSFM-Enhanced)

Host CPU, INT8 models, val=1559 imgs / 39 ids, 50 latency runs. Latency is **relative** (host, not ESP32-S3).

| metric | fast | accurate |
|---|---|---|
| tflite size (KB) | 1312 | 1403 |
| ops (count) | 8 | 9 |
| latency mean (ms) | 0.3 | 0.3 |
| latency p95 (ms) | 0.4 | 0.5 |
| intra cosine | 0.306 | 0.425 |
| inter cosine | 0.113 | 0.260 |
| separation margin | 0.192 | 0.165 |
| EER threshold | 0.140 | 0.290 |
| EER | 0.356 | 0.362 |

Ops (TFLite-Micro must support all):
- **fast**: ADD, CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, L2_NORMALIZATION, MEAN, MUL, PAD
- **accurate**: ADD, CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, L2_NORMALIZATION, LOGISTIC, MEAN, MUL, PAD
