/****************************************************************************
 * VelaPaw - keyword spotting implementation (Phase 1.5)
 *
 * Two interpreters, run back to back:
 *
 *   audio (int16, 480 samples)
 *       -> audio_preprocessor_int8.tflite   -> 40 int8 mel features
 *       (repeated 49x with a 320-sample stride)
 *   features (int8, 49 x 40)
 *       -> micro_speech_quantized.tflite    -> 14 int8 logits
 *
 * Both models are linked as rodata from voice/gen/ (67.6 KB combined), NOT
 * loaded from flash like the 1.4 MB pet model -- they fit the DROM0 window
 * comfortably.
 *
 * printf, not MicroPrintf: the app is built with -DTF_LITE_STRIP_ERROR_STRINGS
 * and this module has no reason to depend on which side of that if/else the
 * vendored library landed on.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <algorithm>

#include <nuttx/kmalloc.h>

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "kws.h"

#include "micro_speech_quantized_model_data.h"
#include "audio_preprocessor_int8_model_data.h"

#ifdef CONFIG_VELAPAW_KWS_SELFTEST
#  include "yes_30ms_audio_data.h"
#  include "no_30ms_audio_data.h"
#  include "yes_1000ms_audio_data.h"
#  include "no_1000ms_audio_data.h"
#  include "silence_1000ms_audio_data.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Generous to begin with, deliberately.  A failed AllocateTensors costs a
 * whole build/flash cycle to discover, whereas over-allocating costs heap we
 * have (MM_REGIONS=2 puts octal PSRAM in the general heap).  Both are printed
 * at init as arena_used_bytes(); shrink these to the measured values once
 * they have been read off the console.
 *
 * Upstream sizes ONE arena at 28584 for whichever model is larger, because it
 * never has both alive at once.  We do, hence two.
 */

/* Measured on hardware (build 36): frontend 9840 B, classifier 6916 B.  Both
 * are fixed by the graph, not the input, so these are exact rather than
 * typical -- sized here with ~20% headroom.  Still printed at init; if either
 * number ever moves, the model changed.
 *
 * The classifier arena went 8 KB -> 16 KB with the 14-label tiny_conv model,
 * sized off an estimate: the conv activation is 25x20x8 = 4000 B and the
 * flattened 4000-B vector feeding final_fc is live alongside the 1960-B input,
 * so the peak had to be around 10 KB against 6916 B before.
 *
 * Build 56 measured it: frontend 9840 B (unchanged, same graph), classifier
 * 9060 B.  12 KB gives the classifier 3324 B of headroom, the same shape of
 * margin the frontend has always had.  These are graph-determined, so if
 * either number moves at boot, the model changed underneath us.
 */

/* Sizes moved to kws.h in build 58 so velapaw_main.c can print capacity
 * beside the high-water mark; these aliases keep the code below unchanged.
 */

#define KWS_FRONTEND_ARENA   VELAPAW_KWS_FRONTEND_ARENA
#define KWS_CLASSIFIER_ARENA VELAPAW_KWS_CLASSIFIER_ARENA

/****************************************************************************
 * Private Types
 ****************************************************************************/

using FrontendOpResolver   = tflite::MicroMutableOpResolver<18>;
using ClassifierOpResolver = tflite::MicroMutableOpResolver<4>;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_ready = false;

static uint8_t *g_frontend_arena   = nullptr;
static uint8_t *g_classifier_arena = nullptr;

static tflite::MicroInterpreter *g_frontend   = nullptr;
static tflite::MicroInterpreter *g_classifier = nullptr;

static size_t g_frontend_used   = 0;
static size_t g_classifier_used = 0;

/* One 49x40 feature map, reused across calls.  1960 bytes -- small enough to
 * keep in .bss rather than juggling ownership with the caller.
 */

static int8_t g_features[VELAPAW_KWS_FEATURE_COUNT][VELAPAW_KWS_FEATURE_SIZE];

/* Order is the training label order (host/kws/train_kws.py WORDS) and is
 * asserted against the enum below, because a table that drifts out of step
 * with the enum mislabels the console output rather than failing.
 */

static const char *g_label_names[VELAPAW_KWS_CATEGORIES] =
{
  "silence", "unknown", "yes", "no",
  "zero", "one", "two", "three", "four",
  "five", "six", "seven", "eight", "nine"
};

static_assert(VELAPAW_KWS_NINE == VELAPAW_KWS_CATEGORIES - 1,
              "kws enum and VELAPAW_KWS_CATEGORIES disagree");

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t kws_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Both graphs are int8 quantised but take different input types (int16 audio
 * vs int8 features), so shapes are checked rather than assumed.  A silent
 * shape mismatch here would surface much later as "the model is inaccurate",
 * which is a far worse thing to debug.
 */

static int kws_check_tensor(TfLiteTensor *t, const char *what,
                            TfLiteType type, int last_dim)
{
  if (t == nullptr)
    {
      printf("kws: %s tensor is NULL\n", what);
      return -EINVAL;
    }

  if (t->type != type)
    {
      printf("kws: %s tensor type %d, expected %d\n",
             what, (int)t->type, (int)type);
      return -EINVAL;
    }

  if (t->dims->data[t->dims->size - 1] != last_dim)
    {
      printf("kws: %s tensor last dim %d, expected %d\n",
             what, (int)t->dims->data[t->dims->size - 1], last_dim);
      return -EINVAL;
    }

  return 0;
}

/* Registered once and reused across interpreter rebuilds.  The interpreter
 * holds a reference to this, so it must outlive every interpreter built from
 * it -- a function-local static does exactly that.
 *
 * -fno-threadsafe-statics means no __cxa_guard_acquire is emitted here.  That
 * call never returns on this target; it is why the whole signal library has to
 * be built with the same flag (board/patches/tflm-signal.py).
 */

static FrontendOpResolver *kws_frontend_ops(void)
{
  static FrontendOpResolver resolver;
  static bool registered = false;

  if (registered)
    {
      return &resolver;
    }

  /* The nine signal ops below are why the library build needs
   * signal/micro/kernels and signal/src: the vendored tflite-micro Makefile
   * globs only tensorflow/lite/..., so without that these link as undefined.
   */

  if (resolver.AddReshape()                        != kTfLiteOk ||
      resolver.AddCast()                           != kTfLiteOk ||
      resolver.AddStridedSlice()                   != kTfLiteOk ||
      resolver.AddConcatenation()                  != kTfLiteOk ||
      resolver.AddMul()                            != kTfLiteOk ||
      resolver.AddAdd()                            != kTfLiteOk ||
      resolver.AddDiv()                            != kTfLiteOk ||
      resolver.AddMinimum()                        != kTfLiteOk ||
      resolver.AddMaximum()                        != kTfLiteOk ||
      resolver.AddWindow()                         != kTfLiteOk ||
      resolver.AddFftAutoScale()                   != kTfLiteOk ||
      resolver.AddRfft()                           != kTfLiteOk ||
      resolver.AddEnergy()                         != kTfLiteOk ||
      resolver.AddFilterBank()                     != kTfLiteOk ||
      resolver.AddFilterBankSquareRoot()           != kTfLiteOk ||
      resolver.AddFilterBankSpectralSubtraction()  != kTfLiteOk ||
      resolver.AddPCAN()                           != kTfLiteOk ||
      resolver.AddFilterBankLog()                  != kTfLiteOk)
    {
      printf("kws: frontend op registration failed\n");
      return nullptr;
    }

  registered = true;
  return &resolver;
}

/* Build (or REbuild) the frontend interpreter on the existing arena.
 *
 * Rebuilding is the only way to clear the frontend's carried state.  The
 * spectral-subtraction and PCAN noise estimates do NOT live in variable
 * tensors -- they are in each node's user_data, and the sole thing that zeroes
 * them is ResetState() inside the kernel's Prepare(), which runs as part of
 * AllocateTensors().  MicroInterpreter::Reset() resets variable tensors and
 * would silently NOT touch these.  Upstream sidesteps the whole question by
 * constructing a fresh interpreter per utterance; this is that, made explicit.
 *
 * The arena is reused across rebuilds, exactly as upstream reuses its g_arena.
 */

static int kws_build_frontend(void)
{
  const tflite::Model *model =
    tflite::GetModel(g_audio_preprocessor_int8_model_data);

  FrontendOpResolver *ops = kws_frontend_ops();
  if (ops == nullptr)
    {
      return -EINVAL;
    }

  delete g_frontend;
  g_frontend = nullptr;

  tflite::MicroInterpreter *interp =
    new tflite::MicroInterpreter(model, *ops, g_frontend_arena,
                                 KWS_FRONTEND_ARENA);

  if (interp == nullptr)
    {
      printf("kws: frontend interpreter alloc failed\n");
      return -ENOMEM;
    }

  if (interp->AllocateTensors() != kTfLiteOk)
    {
      printf("kws: frontend AllocateTensors FAILED (arena %d B too small?)\n",
             KWS_FRONTEND_ARENA);
      delete interp;
      return -ENOMEM;
    }

  g_frontend      = interp;
  g_frontend_used = interp->arena_used_bytes();
  return 0;
}

static int kws_init_frontend(void)
{
  const tflite::Model *model =
    tflite::GetModel(g_audio_preprocessor_int8_model_data);
  int ret;

  if (model->version() != TFLITE_SCHEMA_VERSION)
    {
      printf("kws: frontend schema %u, expected %u\n",
             (unsigned)model->version(), (unsigned)TFLITE_SCHEMA_VERSION);
      return -EINVAL;
    }

  g_frontend_arena = (uint8_t *)kmm_memalign(16, KWS_FRONTEND_ARENA);
  if (g_frontend_arena == nullptr)
    {
      printf("kws: frontend arena alloc %d B failed\n", KWS_FRONTEND_ARENA);
      return -ENOMEM;
    }

  ret = kws_build_frontend();
  if (ret < 0)
    {
      return ret;
    }

  if (kws_check_tensor(g_frontend->input(0), "frontend in", kTfLiteInt16,
                       VELAPAW_KWS_FRAME_SAMPLES) < 0 ||
      kws_check_tensor(g_frontend->output(0), "frontend out", kTfLiteInt8,
                       VELAPAW_KWS_FEATURE_SIZE) < 0)
    {
      return -EINVAL;
    }

  printf("kws: frontend ready, arena used %u/%d B\n",
         (unsigned)g_frontend_used, KWS_FRONTEND_ARENA);
  return 0;
}

static int kws_init_classifier(void)
{
  const tflite::Model *model =
    tflite::GetModel(g_micro_speech_quantized_model_data);

  if (model->version() != TFLITE_SCHEMA_VERSION)
    {
      printf("kws: classifier schema %u, expected %u\n",
             (unsigned)model->version(), (unsigned)TFLITE_SCHEMA_VERSION);
      return -EINVAL;
    }

  g_classifier_arena = (uint8_t *)kmm_memalign(16, KWS_CLASSIFIER_ARENA);
  if (g_classifier_arena == nullptr)
    {
      printf("kws: classifier arena alloc %d B failed\n",
             KWS_CLASSIFIER_ARENA);
      return -ENOMEM;
    }

  static ClassifierOpResolver resolver;

  /* Conv2D, NOT DepthwiseConv2D.  The shipped micro_speech model used a
   * depthwise conv; our tiny_conv does a plain CONV_2D (verified against the
   * op list in kws_int8.tflite).  Same slot count, different op -- and a
   * missing op is not a link error, it is AllocateTensors returning
   * kTfLiteError at boot with "Didn't find op for builtin opcode".
   */

  if (resolver.AddReshape()         != kTfLiteOk ||
      resolver.AddFullyConnected()  != kTfLiteOk ||
      resolver.AddConv2D()          != kTfLiteOk ||
      resolver.AddSoftmax()         != kTfLiteOk)
    {
      printf("kws: classifier op registration failed\n");
      return -EINVAL;
    }

  static tflite::MicroInterpreter interp(model, resolver, g_classifier_arena,
                                         KWS_CLASSIFIER_ARENA);

  if (interp.AllocateTensors() != kTfLiteOk)
    {
      printf("kws: classifier AllocateTensors FAILED (arena %d B too small?)\n",
             KWS_CLASSIFIER_ARENA);
      return -ENOMEM;
    }

  g_classifier      = &interp;
  g_classifier_used = interp.arena_used_bytes();

  if (kws_check_tensor(interp.input(0), "classifier in", kTfLiteInt8,
                       VELAPAW_KWS_FEATURE_COUNT *
                       VELAPAW_KWS_FEATURE_SIZE) < 0 ||
      kws_check_tensor(interp.output(0), "classifier out", kTfLiteInt8,
                       VELAPAW_KWS_CATEGORIES) < 0)
    {
      return -EINVAL;
    }

  printf("kws: classifier ready, arena used %u/%d B\n",
         (unsigned)g_classifier_used, KWS_CLASSIFIER_ARENA);
  return 0;
}

/* Build the 49x40 feature map.  One interpreter invocation per frame -- the
 * frontend graph is stateful across calls (spectral subtraction and PCAN both
 * carry noise estimates forward), so frames MUST be fed in time order and the
 * interpreter must not be rebuilt between them.
 */

static int kws_generate_features(const int16_t *audio, size_t nsamples)
{
  TfLiteTensor *input  = g_frontend->input(0);
  TfLiteTensor *output = g_frontend->output(0);
  size_t remaining     = nsamples;
  int frame            = 0;

  while (remaining >= VELAPAW_KWS_FRAME_SAMPLES &&
         frame < VELAPAW_KWS_FEATURE_COUNT)
    {
      std::copy_n(audio, VELAPAW_KWS_FRAME_SAMPLES,
                  tflite::GetTensorData<int16_t>(input));

      if (g_frontend->Invoke() != kTfLiteOk)
        {
          printf("kws: frontend Invoke failed at frame %d\n", frame);
          return -EIO;
        }

      std::copy_n(tflite::GetTensorData<int8_t>(output),
                  VELAPAW_KWS_FEATURE_SIZE, g_features[frame]);

      frame     += 1;
      audio     += VELAPAW_KWS_STRIDE_SAMPLES;
      remaining -= VELAPAW_KWS_STRIDE_SAMPLES;
    }

  if (frame != VELAPAW_KWS_FEATURE_COUNT)
    {
      printf("kws: only %d/%d frames from %u samples\n",
             frame, VELAPAW_KWS_FEATURE_COUNT, (unsigned)nsamples);
      return -EINVAL;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapaw_kws_init(void)
{
  int ret;

  if (g_ready)
    {
      return 0;
    }

  printf("kws: init (frontend %d B + classifier %d B arenas)\n",
         KWS_FRONTEND_ARENA, KWS_CLASSIFIER_ARENA);
  fflush(stdout);

  ret = kws_init_frontend();
  if (ret < 0)
    {
      return ret;
    }

  ret = kws_init_classifier();
  if (ret < 0)
    {
      return ret;
    }

  g_ready = true;
  printf("kws: ready\n");
  fflush(stdout);
  return 0;
}

int velapaw_kws_reset(void)
{
  if (!g_ready)
    {
      return 0;   /* nothing built yet; init() starts clean anyway */
    }

  /* Frontend only.  The classifier graph (Reshape / Conv2D / FullyConnected /
   * Softmax) carries nothing between invocations, so rebuilding it would cost
   * time and change no result.
   */

  return kws_build_frontend();
}

int velapaw_kws_classify(const int16_t *audio, size_t nsamples,
                         struct velapaw_kws_result_s *result)
{
  uint32_t t0;
  uint32_t t1;
  uint32_t t2;
  int ret;
  int i;
  int best;

  if (audio == nullptr || result == nullptr)
    {
      return -EINVAL;
    }

  if (!g_ready)
    {
      ret = velapaw_kws_init();
      if (ret < 0)
        {
          return ret;
        }
    }

  if (nsamples < (size_t)VELAPAW_KWS_MIN_SAMPLES)
    {
      printf("kws: %u samples, need at least %d\n",
             (unsigned)nsamples, VELAPAW_KWS_MIN_SAMPLES);
      return -EINVAL;
    }

  memset(result, 0, sizeof(*result));

  t0  = kws_now_ms();
  ret = kws_generate_features(audio, nsamples);
  if (ret < 0)
    {
      return ret;
    }

  t1 = kws_now_ms();

  TfLiteTensor *input  = g_classifier->input(0);
  TfLiteTensor *output = g_classifier->output(0);

  std::copy_n(&g_features[0][0],
              VELAPAW_KWS_FEATURE_COUNT * VELAPAW_KWS_FEATURE_SIZE,
              tflite::GetTensorData<int8_t>(input));

  if (g_classifier->Invoke() != kTfLiteOk)
    {
      printf("kws: classifier Invoke failed\n");
      return -EIO;
    }

  t2 = kws_now_ms();

  /* Dequantise: the graph ends in Softmax, so these sum to ~1.0. */

  best = 0;
  for (i = 0; i < VELAPAW_KWS_CATEGORIES; i++)
    {
      result->scores[i] =
        (tflite::GetTensorData<int8_t>(output)[i] - output->params.zero_point) *
        output->params.scale;

      if (result->scores[i] > result->scores[best])
        {
          best = i;
        }
    }

  result->label      = best;
  result->score      = result->scores[best];
  result->feature_ms = t1 - t0;
  result->infer_ms   = t2 - t1;

  return 0;
}

int velapaw_kws_best_of(const struct velapaw_kws_result_s *result,
                        const int *labels, int n, float *score)
{
  int best = -1;
  int i;

  if (result == nullptr || labels == nullptr || n <= 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < n; i++)
    {
      if (labels[i] < 0 || labels[i] >= VELAPAW_KWS_CATEGORIES)
        {
          return -EINVAL;
        }

      if (best < 0 || result->scores[labels[i]] > result->scores[best])
        {
          best = labels[i];
        }
    }

  /* Raw softmax probability, not renormalised over the subset -- see the
   * header.  The mass this keeps in the denominator is the mass on
   * _unknown_/_silence_, which is the whole rejection signal.
   */

  if (score != nullptr)
    {
      *score = result->scores[best];
    }

  return best;
}

const char *velapaw_kws_label_name(int label)
{
  if (label < 0 || label >= VELAPAW_KWS_CATEGORIES)
    {
      return "?";
    }

  return g_label_names[label];
}

void velapaw_kws_arena_used(size_t *frontend, size_t *classifier)
{
  if (frontend != nullptr)
    {
      *frontend = g_frontend_used;
    }

  if (classifier != nullptr)
    {
      *classifier = g_classifier_used;
    }
}

#ifdef CONFIG_VELAPAW_KWS_SELFTEST

/* Expected first-frame features, copied verbatim from the upstream
 * micro_speech_test.cc.  These are the ground truth for the frontend on any
 * target: if this passes, windowing/FFT/mel/PCAN/log are all bit-correct here.
 */

static const int8_t g_expected_yes_feature[VELAPAW_KWS_FEATURE_SIZE] =
{
  124, 105, 126, 103, 125, 101, 123, 100, 116, 98,  115, 97,  113, 90,
  91,  82,  104, 96,  117, 97,  121, 103, 126, 101, 125, 104, 126, 104,
  125, 101, 116, 90,  81,  74,  80,  71,  83,  76,  82,  71,
};

static const int8_t g_expected_no_feature[VELAPAW_KWS_FEATURE_SIZE] =
{
  126, 103, 124, 102, 124, 102, 123, 100, 118, 97, 118, 100, 118, 98,
  121, 100, 121, 98,  117, 91,  96,  74,  54,  87, 100, 87,  109, 92,
  91,  80,  64,  55,  83,  74,  74,  78,  114, 95, 101, 81,
};

/* One frame only, so kws_generate_features() cannot be reused (it insists on
 * a full 49).  This deliberately drives the frontend directly.
 */

static int kws_selftest_feature(const char *what, const int16_t *audio,
                                unsigned int nsamples,
                                const int8_t *expected)
{
  int mismatches = 0;
  int i;
  int ret;

  if (nsamples != (unsigned int)VELAPAW_KWS_FRAME_SAMPLES)
    {
      printf("kws/selftest: %s has %u samples, expected %d\n",
             what, nsamples, VELAPAW_KWS_FRAME_SAMPLES);
      return -EINVAL;
    }

  /* The expected vectors are for a frontend that has processed nothing at all
   * beforehand, so each check needs a clean one -- otherwise the first check
   * poisons the second with its noise estimate.  Tensor pointers move when the
   * interpreter is rebuilt, so they are fetched after this, not before.
   */

  ret = velapaw_kws_reset();
  if (ret < 0)
    {
      return ret;
    }

  TfLiteTensor *input  = g_frontend->input(0);
  TfLiteTensor *output = g_frontend->output(0);

  std::copy_n(audio, VELAPAW_KWS_FRAME_SAMPLES,
              tflite::GetTensorData<int16_t>(input));

  if (g_frontend->Invoke() != kTfLiteOk)
    {
      printf("kws/selftest: %s frontend Invoke failed\n", what);
      return -EIO;
    }

  const int8_t *got = tflite::GetTensorData<int8_t>(output);

  for (i = 0; i < VELAPAW_KWS_FEATURE_SIZE; i++)
    {
      if (got[i] != expected[i])
        {
          if (mismatches < 4)
            {
              printf("kws/selftest: %s feature[%d] = %d, expected %d\n",
                     what, i, (int)got[i], (int)expected[i]);
            }

          mismatches++;
        }
    }

  if (mismatches != 0)
    {
      printf("kws/selftest: %s FRONTEND MISMATCH, %d/%d bytes differ\n",
             what, mismatches, VELAPAW_KWS_FEATURE_SIZE);
      return -EIO;
    }

  printf("kws/selftest: %s frontend bit-exact (%d/%d)\n",
         what, VELAPAW_KWS_FEATURE_SIZE, VELAPAW_KWS_FEATURE_SIZE);
  return 0;
}

static int kws_selftest_classify(const char *what, const int16_t *audio,
                                 unsigned int nsamples, int expected)
{
  struct velapaw_kws_result_s r;
  int ret;
  int i;

  /* Each clip is independent, and upstream builds a fresh interpreter per
   * clip; reset so these numbers are comparable with its published ones.
   */

  ret = velapaw_kws_reset();
  if (ret < 0)
    {
      return ret;
    }

  ret = velapaw_kws_classify(audio, nsamples, &r);
  if (ret < 0)
    {
      printf("kws/selftest: %s classify failed: %d\n", what, ret);
      return ret;
    }

  printf("kws/selftest: %-8s -> %-8s %.3f  (feat %ums, infer %ums)\n",
         what, velapaw_kws_label_name(r.label), (double)r.score,
         (unsigned)r.feature_ms, (unsigned)r.infer_ms);

  for (i = 0; i < VELAPAW_KWS_CATEGORIES; i++)
    {
      printf("kws/selftest:     %-8s %.4f\n",
             velapaw_kws_label_name(i), (double)r.scores[i]);
    }

  if (r.label != expected)
    {
      printf("kws/selftest: %s MISCLASSIFIED (expected %s)\n",
             what, velapaw_kws_label_name(expected));
      return -EIO;
    }

  return 0;
}

int velapaw_kws_selftest(void)
{
  int failures = 0;
  int ret;

  ret = velapaw_kws_init();
  if (ret < 0)
    {
      return ret;
    }

  printf("kws/selftest: ---- frontend bit-exactness ----\n");
  fflush(stdout);

  /* Every check below resets the frontend first.  It carries a noise estimate
   * forward, and the upstream expected vectors assume one that has seen
   * nothing else -- without the reset, yes_30ms poisons no_30ms and the second
   * check fails on all 40 bytes while the first passes.  That is exactly what
   * happened on build 35.
   */

  if (kws_selftest_feature("yes_30ms", g_yes_30ms_audio_data,
                           g_yes_30ms_audio_data_size,
                           g_expected_yes_feature) < 0)
    {
      failures++;
    }

  if (kws_selftest_feature("no_30ms", g_no_30ms_audio_data,
                           g_no_30ms_audio_data_size,
                           g_expected_no_feature) < 0)
    {
      failures++;
    }

  printf("kws/selftest: ---- end-to-end classification ----\n");
  fflush(stdout);

  if (kws_selftest_classify("yes", g_yes_1000ms_audio_data,
                            g_yes_1000ms_audio_data_size,
                            VELAPAW_KWS_YES) < 0)
    {
      failures++;
    }

  if (kws_selftest_classify("no", g_no_1000ms_audio_data,
                            g_no_1000ms_audio_data_size,
                            VELAPAW_KWS_NO) < 0)
    {
      failures++;
    }

  if (kws_selftest_classify("silence", g_silence_1000ms_audio_data,
                            g_silence_1000ms_audio_data_size,
                            VELAPAW_KWS_SILENCE) < 0)
    {
      failures++;
    }

  if (failures != 0)
    {
      printf("kws/selftest: %d CHECK(S) FAILED\n", failures);
      return -EIO;
    }

  printf("kws/selftest: ALL PASS -- the model path is proven, "
         "any live-audio failure is capture-side\n");
  fflush(stdout);
  return 0;
}

#endif /* CONFIG_VELAPAW_KWS_SELFTEST */
