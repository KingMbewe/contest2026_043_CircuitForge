/****************************************************************************
 * VelaPaw - inference backend interface (IMPLEMENTATION_PLAN.md 7.3)
 *
 * A stable boundary so models/runtimes are swappable and the benchmark panel
 * can hold two instances (fast | accurate). Backends:
 *   infer_stub.c      - deterministic pseudo-embedding (no model; emulator dev)
 *   backend_tflm.cc   - TFLite-Micro INT8 inference (real on-device path)
 *
 * velapaw_infer_active() picks the backend (TFLM if CONFIG_VELAPAW_INFER_TFLM,
 * else stub); selection lives in infer.c.
 ****************************************************************************/

#ifndef VELAPAW_INFER_H
#define VELAPAW_INFER_H

#include <stdint.h>
#include <stddef.h>
#include "velapaw.h"
#include "hal/camera.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct velapaw_infer_backend
{
  const char *name;                       /* "stub" | "fast" | "accurate" */
  int  (*init)(void);
  int  (*embed)(const struct velapaw_frame *frame,
                float *out_embedding, int dim);
  void (*deinit)(void);

  /* Benchmarking hooks (panel: latency + memory). */
  uint32_t (*last_latency_us)(void);
  size_t   (*arena_bytes)(void);
};

/* Backend getters (each defined in its own translation unit). */
const struct velapaw_infer_backend *velapaw_infer_stub_backend(void);
const struct velapaw_infer_backend *velapaw_infer_tflm_backend(void);

/* The backend selected for this build. */
const struct velapaw_infer_backend *velapaw_infer_active(void);

/* Body Condition Score (standalone 2nd model, TFLM build w/ bcs.tflite only):
 * *cls = 0 under / 1 ideal / 2 over, *conf = probability. Returns 0 on success.
 */
int velapaw_infer_bcs(const struct velapaw_frame *frame, int *cls, float *conf);

#ifdef __cplusplus
}
#endif

#endif /* VELAPAW_INFER_H */
