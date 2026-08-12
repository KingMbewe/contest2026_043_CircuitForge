/****************************************************************************
 * VelaPaw - stub inference backend
 *
 * Produces a deterministic, L2-normalized pseudo-embedding from the frame so
 * the rest of the pipeline (enroll, cosine match, feed) is exercisable in the
 * emulator before a trained model exists. Same frame -> same vector; different
 * scenes -> different vectors. Swap for backend_tflm.cc to run a real model.
 ****************************************************************************/

#include <math.h>
#include "infer/infer.h"

static int stub_init(void)
{
  return 0;
}

static void stub_deinit(void)
{
}

static uint32_t stub_last_latency_us(void)
{
  return 0;   /* real backend reports measured inference time */
}

static size_t stub_arena_bytes(void)
{
  return 0;   /* real backend reports tensor-arena usage */
}

static int stub_embed(const struct velapaw_frame *frame,
                      float *out, int dim)
{
  if (frame == NULL || out == NULL || dim <= 0)
    {
      return -1;
    }

  for (int i = 0; i < dim; i++)
    {
      out[i] = 0.0f;
    }

  /* Bucket pixel intensities into `dim` features. */
  int n = frame->width * frame->height * frame->channels;
  for (int i = 0; i < n; i++)
    {
      out[i % dim] += (float)frame->data[i];
    }

  /* L2-normalize so cosine similarity is a plain dot product. */
  float norm = 0.0f;
  for (int i = 0; i < dim; i++)
    {
      norm += out[i] * out[i];
    }

  norm = sqrtf(norm);
  if (norm < 1e-6f)
    {
      norm = 1.0f;
    }

  for (int i = 0; i < dim; i++)
    {
      out[i] /= norm;
    }

  return 0;
}

static const struct velapaw_infer_backend g_stub_backend =
{
  .name            = "stub",
  .init            = stub_init,
  .embed           = stub_embed,
  .deinit          = stub_deinit,
  .last_latency_us = stub_last_latency_us,
  .arena_bytes     = stub_arena_bytes,
};

const struct velapaw_infer_backend *velapaw_infer_stub_backend(void)
{
  return &g_stub_backend;
}
