/****************************************************************************
 * VelaPaw - TFLite-Micro inference backend (real on-device INT8 path)
 *
 * Joins the proven TFLM integration with the VelaPaw app: loads an embedded
 * INT8 CNN, preprocesses the 96x96 grayscale frame, runs Invoke(), and exposes
 * the result through the velapaw_infer_backend interface with measured latency
 * and tensor-arena usage (feeds the benchmark panel).
 *
 * NOTE: the embedded model is `person_detect` (a 2-class classifier) used as a
 * stand-in. It demonstrates a *real* inference path + real latency/arena, but
 * its 2-value output is not a discriminative pet embedding -- swap in a host-
 * trained embedding model (Phase 1) for real identity. Until then keep the stub
 * backend (CONFIG_VELAPAW_INFER_TFLM=n) for the recognition demo.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/kmalloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "velapaw_model_data.h"   /* generated from infer/model/velapaw.tflite: g_velapaw_model_data[] */
/* BCS model is flash-loaded from 0x760000 (not embedded) -- see tflm_init. */

#include "infer/infer.h"

/* Full-model build: the 1.4MB model is flashed to raw offset 0x600000 and read
 * into a PSRAM buffer at runtime via the NuttX software spi_flash_read
 * (CONFIG_ESP32S3_SPI_FLASH_DONT_USE_ROM_CODE=y), run on a thread whose stack is
 * internal RAM (the driver's SPI buffer lives on the caller stack; the velapaw
 * task stack is PSRAM, unusable while the read suspends the cache). */
extern "C" int spi_flash_read(uint32_t addr, void *dest, uint32_t size);
#define VELAPAW_MODEL_FLASH_OFFSET 0x600000u
static uint8_t g_flash_rd_stack[8192];
struct velapaw_flash_rd { uint32_t off; void *dst; uint32_t len; int rc; };
static void *velapaw_flash_rd_thread(void *arg)
{
  struct velapaw_flash_rd *r = (struct velapaw_flash_rd *)arg;
  r->rc = spi_flash_read(r->off, r->dst, r->len);
  return NULL;
}

#define VELAPAW_MODEL_SIZE 1436344u   /* infer/model/velapaw.tflite */
#define VELAPAW_BCS_FLASH_OFFSET 0x760000u  /* past the 1.4MB identity model */
#define VELAPAW_BCS_SIZE 626512u            /* infer/model/bcs.tflite */

/* Read a model from raw flash into a PSRAM buffer, on a pthread with an
 * internal-RAM stack (spi_flash_read's on-stack SPI buffer can't live in PSRAM
 * while the read suspends the cache). Reused for the identity + BCS models
 * (called sequentially, so the shared read stack is safe). */
static const tflite::Model *velapaw_load_model_from_flash(uint32_t off,
                                                          uint32_t size,
                                                          const char *tag)
{
  uint8_t *buf = (uint8_t *)kmm_memalign(16, size);
  if (buf == nullptr)
    {
      printf("tflm[%s]: PSRAM buf alloc %u B FAILED\n",
             tag, (unsigned)size); fflush(stdout);
      return nullptr;
    }
  struct velapaw_flash_rd r = { off, buf, size, -1 };
  pthread_attr_t attr;
  pthread_t th;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, g_flash_rd_stack, sizeof(g_flash_rd_stack));
  printf("tflm[%s]: flash-read %u B from 0x%06x -> %p ...\n",
         tag, (unsigned)size, (unsigned)off, (void *)buf); fflush(stdout);
  if (pthread_create(&th, &attr, velapaw_flash_rd_thread, &r) != 0)
    {
      printf("tflm[%s]: flash-read thread create FAILED\n", tag); fflush(stdout);
      pthread_attr_destroy(&attr);
      kmm_free(buf);
      return nullptr;
    }
  pthread_join(th, nullptr);
  pthread_attr_destroy(&attr);
  printf("tflm[%s]: flash-read rc=%d, model[0..3]=%02x %02x %02x %02x\n",
         tag, r.rc, buf[0], buf[1], buf[2], buf[3]); fflush(stdout);
  if (r.rc < 0)
    {
      kmm_free(buf);
      return nullptr;
    }
  return tflite::GetModel(buf);
}

#ifndef CONFIG_VELAPAW_INFER_TFLM_ARENA_SIZE
#  define CONFIG_VELAPAW_INFER_TFLM_ARENA_SIZE (512 * 1024)
#endif

namespace
{
/* Real 1.4MB MobileNetV3: 768KB PSRAM arena (AllocateTensors uses ~206KB). */
constexpr int kArenaSize = 786432;   /* 768 KB */
uint8_t *g_arena = nullptr;

const tflite::Model     *g_model = nullptr;
tflite::MicroInterpreter *g_interp = nullptr;
TfLiteTensor            *g_input = nullptr;
uint32_t                 g_last_us = 0;
size_t                   g_arena_used = 0;

#ifdef VELAPAW_HAVE_BCS
constexpr int kBcsArena = 640 * 1024;   /* PSRAM (too big for internal .bss) */
uint8_t                  *g_bcs_arena = nullptr;
const tflite::Model      *g_bcs_model = nullptr;
tflite::MicroInterpreter *g_bcs_interp = nullptr;
TfLiteTensor             *g_bcs_input = nullptr;
#endif

uint32_t elapsed_us(const struct timespec *a, const struct timespec *b)
{
  return (uint32_t)((b->tv_sec - a->tv_sec) * 1000000ULL +
                    (b->tv_nsec - a->tv_nsec) / 1000);
}
}  // namespace

static int tflm_init(void)
{
  tflite::InitializeTarget();
  printf("tflm: arena alloc %d B (PSRAM)...\n", (int)kArenaSize); fflush(stdout);
  if (g_arena == nullptr)
    {
      g_arena = (uint8_t *)kmm_memalign(16, kArenaSize);
      printf("tflm: arena=%p\n", (void *)g_arena); fflush(stdout);
      if (g_arena == nullptr)
        {
          MicroPrintf("velapaw/tflm: arena alloc %d B failed", kArenaSize);
          return -1;
        }
    }
  usleep(50000);

  /* Load the real 1.4MB MobileNetV3 embedding model from raw flash into PSRAM
   * (too big for flash-mapped rodata -- DROM window bus-stalls past ~256KB). */
  g_model = velapaw_load_model_from_flash(VELAPAW_MODEL_FLASH_OFFSET,
                                          VELAPAW_MODEL_SIZE, "id");
  if (g_model == nullptr)
    {
      printf("tflm: flash model load FAILED\n"); fflush(stdout);
      return -1;
    }
  printf("tflm: model version=%u (flash 0x%06x, %u B)\n",
         (unsigned)g_model->version(), VELAPAW_MODEL_FLASH_OFFSET,
         (unsigned)VELAPAW_MODEL_SIZE); fflush(stdout);
  if (g_model->version() != TFLITE_SCHEMA_VERSION)
    {
      MicroPrintf("velapaw/tflm: model schema %d != supported %d",
                  g_model->version(), TFLITE_SCHEMA_VERSION);
      return -1;
    }

  /* Generous op set: covers the green-path test model (micro_speech: Conv/
   * DepthwiseConv/FullyConnected/Softmax/Reshape) and the future compact
   * depthwise-separable embedding CNN. */
  static tflite::MicroMutableOpResolver<16> resolver;
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddFullyConnected();
  resolver.AddL2Normalization();
  resolver.AddLogistic();
  resolver.AddMean();
  resolver.AddMul();
  resolver.AddAdd();
  resolver.AddPad();
  resolver.AddSoftmax();
  resolver.AddReshape();
  resolver.AddAveragePool2D();
  resolver.AddMaxPool2D();
  resolver.AddQuantize();
  resolver.AddDequantize();

  printf("tflm: build interpreter...\n"); fflush(stdout); usleep(100000);
  static tflite::MicroInterpreter interp(g_model, resolver, g_arena, kArenaSize);
  g_interp = &interp;

  printf("tflm: AllocateTensors...\n"); fflush(stdout); usleep(100000);
  if (g_interp->AllocateTensors() != kTfLiteOk)
    {
      MicroPrintf("velapaw/tflm: AllocateTensors failed (arena too small?)");
      printf("tflm: AllocateTensors FAILED\n"); fflush(stdout);
      return -1;
    }
  printf("tflm: AllocateTensors OK, used %u B\n",
         (unsigned)g_interp->arena_used_bytes()); fflush(stdout);

  g_input      = g_interp->input(0);
  g_arena_used = g_interp->arena_used_bytes();

#ifdef VELAPAW_HAVE_BCS
  /* Second, standalone network: 3-class body condition (under/ideal/over).
   * Flash-loaded into PSRAM like the identity model (too big for rodata), with
   * its own PSRAM arena. */
  g_bcs_arena = (uint8_t *)kmm_memalign(16, kBcsArena);
  g_bcs_model = velapaw_load_model_from_flash(VELAPAW_BCS_FLASH_OFFSET,
                                              VELAPAW_BCS_SIZE, "bcs");
  if (g_bcs_arena != nullptr && g_bcs_model != nullptr &&
      g_bcs_model->version() == TFLITE_SCHEMA_VERSION)
    {
      static tflite::MicroMutableOpResolver<10> bcs_res;
      bcs_res.AddAdd();
      bcs_res.AddConv2D();
      bcs_res.AddDepthwiseConv2D();
      bcs_res.AddFullyConnected();
      bcs_res.AddMean();
      bcs_res.AddMul();
      bcs_res.AddPad();
      bcs_res.AddQuantize();
      bcs_res.AddSoftmax();
      bcs_res.AddSub();
      static tflite::MicroInterpreter bcs_interp(g_bcs_model, bcs_res,
                                                 g_bcs_arena, kBcsArena);
      if (bcs_interp.AllocateTensors() == kTfLiteOk)
        {
          g_bcs_interp = &bcs_interp;
          g_bcs_input  = g_bcs_interp->input(0);
          printf("tflm[bcs]: ready, arena used %u B\n",
                 (unsigned)g_bcs_interp->arena_used_bytes()); fflush(stdout);
        }
      else
        {
          printf("tflm[bcs]: AllocateTensors FAILED (arena small?)\n");
          fflush(stdout);
        }
    }
  else
    {
      printf("tflm[bcs]: load/schema FAILED (arena=%p model=%p)\n",
             (void *)g_bcs_arena, (void *)g_bcs_model); fflush(stdout);
    }
#endif

  return 0;
}

static void tflm_deinit(void)
{
}

static uint32_t tflm_last_latency_us(void)
{
  return g_last_us;
}

static size_t tflm_arena_bytes(void)
{
  return g_arena_used;
}

static int tflm_embed(const struct velapaw_frame *frame, float *out, int dim)
{
  if (g_interp == nullptr || frame == nullptr || out == nullptr || dim <= 0)
    {
      return -1;
    }

  /* Preprocess: RGB uint8 -> normalized [-1,1] -> int8 per model input quant
   * (matches host normalization mean/std = 127.5). */
  float in_scale = g_input->params.scale;
  int   in_zp    = g_input->params.zero_point;
  if (in_scale == 0.0f)
    {
      in_scale = 1.0f / 128.0f;   /* symmetric fallback */
    }
  int n    = frame->width * frame->height * frame->channels;
  int in_n = g_input->bytes;
  for (int i = 0; i < n && i < in_n; i++)
    {
      float norm = ((float)frame->data[i] - 127.5f) / 127.5f;
      int q = (int)lroundf(norm / in_scale) + in_zp;
      if (q < -128) q = -128;
      if (q > 127)  q = 127;
      g_input->data.int8[i] = (int8_t)q;
    }

  struct timespec t0;
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  TfLiteStatus status = g_interp->Invoke();
  clock_gettime(CLOCK_MONOTONIC, &t1);
  g_last_us = elapsed_us(&t0, &t1);

  if (status != kTfLiteOk)
    {
      MicroPrintf("velapaw/tflm: Invoke failed");
      return -1;
    }

  /* Dequantize the output tensor into the embedding, zero-pad, L2-normalize.
   * (person_detect output is 2 values; a real embedding model fills `dim`.) */
  TfLiteTensor *o   = g_interp->output(0);
  float        scale = o->params.scale;
  int          zp    = o->params.zero_point;
  int          olen  = o->bytes;   /* int8 element count */

  for (int i = 0; i < dim; i++)
    {
      out[i] = 0.0f;
    }

  for (int i = 0; i < olen && i < dim; i++)
    {
      out[i] = ((int)o->data.int8[i] - zp) * scale;
    }

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

static const struct velapaw_infer_backend g_tflm_backend =
{
  "tflm",
  tflm_init,
  tflm_embed,
  tflm_deinit,
  tflm_last_latency_us,
  tflm_arena_bytes,
};

extern "C" const struct velapaw_infer_backend *velapaw_infer_tflm_backend(void)
{
  return &g_tflm_backend;
}

#ifdef VELAPAW_HAVE_BCS
/* Run the BCS network on a frame: *cls = 0 under / 1 ideal / 2 over,
 * *conf = softmax probability of that class. Returns 0 on success. */
extern "C" int velapaw_infer_bcs(const struct velapaw_frame *frame,
                                 int *cls, float *conf)
{
  if (g_bcs_interp == nullptr || g_bcs_input == nullptr || frame == nullptr)
    {
      return -1;
    }

  int n    = frame->width * frame->height * frame->channels;
  int in_n = g_bcs_input->bytes;   /* uint8 input, 0-255 RGB (prep is in-model) */
  for (int i = 0; i < n && i < in_n; i++)
    {
      g_bcs_input->data.uint8[i] = frame->data[i];
    }

  if (g_bcs_interp->Invoke() != kTfLiteOk)
    {
      return -1;
    }

  TfLiteTensor *o = g_bcs_interp->output(0);
  float scale = o->params.scale;
  int   zp    = o->params.zero_point;
  int   best  = 0;
  float bestp = -1.0f;
  for (int i = 0; i < o->bytes; i++)
    {
      float p = ((int)o->data.uint8[i] - zp) * scale;
      if (p > bestp)
        {
          bestp = p;
          best  = i;
        }
    }
  if (cls)
    {
      *cls = best;
    }
  if (conf)
    {
      *conf = bestp;
    }
  return 0;
}
#endif
