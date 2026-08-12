/****************************************************************************
 * VelaPaw - keyword spotting
 *
 * A 14-category keyword spotter -- silence, unknown, yes, no and the digits
 * zero..nine -- fed by an MFCC-style frontend that is itself a .tflite graph
 * rather than hand-written DSP.
 *
 * Using the shipped frontend graph is deliberate.  The classic keyword-
 * spotting failure is host-side and device-side feature extraction drifting
 * apart: the model scores 95% in training and 40% on the board, and neither
 * half looks broken.  Invoking the same graph on both sides removes that
 * whole class of bug.
 *
 * The classifier is OURS, trained by host/kws/train_kws.py on Google Speech
 * Commands v0.02 (see host/kws/README.md).  It replaces the 4-label
 * micro_speech model that proved the pipe in build 54.  The digits are what
 * let an owner set a feeding time by voice; yes/no carry the confirm step.
 *
 * The label ORDER is shared with training and must not be reshuffled on one
 * side only -- prepare_words_list() emits [_silence_, _unknown_] + wanted
 * words, which is why silence/unknown/yes/no keep the indices build 54 used
 * and the digits merely extend the enum.
 *
 * C linkage throughout: the UI and the audio capture loop are C.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPAW_VOICE_KWS_H
#define __APPS_EXAMPLES_VELAPAW_VOICE_KWS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Model geometry.  These come from training and are not tunable -- changing
 * any of them requires retraining, not just editing this header.
 */

#define VELAPAW_KWS_SAMPLE_RATE   16000
#define VELAPAW_KWS_FEATURE_SIZE  40    /* mel bins per frame               */
#define VELAPAW_KWS_FEATURE_COUNT 49    /* frames per 1 s window            */
#define VELAPAW_KWS_STRIDE_MS     20
#define VELAPAW_KWS_DURATION_MS   30
#define VELAPAW_KWS_CATEGORIES    14

/* Arena sizes.  In the header, not kws.cc, so a caller can print the capacity
 * next to the high-water mark from velapaw_kws_arena_used().
 *
 * That pairing is the only cheap way to tell which build is on the board.  A
 * change to these is invisible in the flash image -- they are %d ARGUMENTS to
 * a format string, never literals in .rodata -- so grepping nuttx.bin for the
 * number finds nothing whether or not the change landed.  Printing used AND
 * capacity from a command that runs after the console exists makes the arena
 * size directly observable, which velapaw_kws_init()'s own print does not:
 * init runs at boot, before USB CDC has enumerated, and scrolls away unseen.
 *
 * Measured on hardware, build 56: frontend 9840 B, classifier 9060 B.  Both
 * are graph-determined, so if either moves the model changed underneath us.
 */

#define VELAPAW_KWS_FRONTEND_ARENA   (12 * 1024)
#define VELAPAW_KWS_CLASSIFIER_ARENA (12 * 1024)

/* Samples consumed per frame (480) and advanced per frame (320). */

#define VELAPAW_KWS_FRAME_SAMPLES \
  (VELAPAW_KWS_DURATION_MS * VELAPAW_KWS_SAMPLE_RATE / 1000)
#define VELAPAW_KWS_STRIDE_SAMPLES \
  (VELAPAW_KWS_STRIDE_MS * VELAPAW_KWS_SAMPLE_RATE / 1000)

/* Minimum audio for a full 49-frame window: 480 + 48*320 = 15840 samples,
 * i.e. slightly under one second.  A 16000-sample buffer is the natural feed.
 */

#define VELAPAW_KWS_MIN_SAMPLES \
  (VELAPAW_KWS_FRAME_SAMPLES + \
   (VELAPAW_KWS_FEATURE_COUNT - 1) * VELAPAW_KWS_STRIDE_SAMPLES)

/* Category indices.  These ARE the training label order -- see WORDS in
 * host/kws/train_kws.py.  Reordering here without retraining makes the board
 * confidently report the wrong word, with nothing in the logs to explain it.
 *
 * The digits are contiguous and in numeric order on purpose: the scheduling
 * dialog converts a spoken hour with `label - VELAPAW_KWS_ZERO`.
 */

enum velapaw_kws_label_e
{
  VELAPAW_KWS_SILENCE = 0,
  VELAPAW_KWS_UNKNOWN = 1,
  VELAPAW_KWS_YES     = 2,
  VELAPAW_KWS_NO      = 3,
  VELAPAW_KWS_ZERO    = 4,
  VELAPAW_KWS_ONE     = 5,
  VELAPAW_KWS_TWO     = 6,
  VELAPAW_KWS_THREE   = 7,
  VELAPAW_KWS_FOUR    = 8,
  VELAPAW_KWS_FIVE    = 9,
  VELAPAW_KWS_SIX     = 10,
  VELAPAW_KWS_SEVEN   = 11,
  VELAPAW_KWS_EIGHT   = 12,
  VELAPAW_KWS_NINE    = 13
};

/* Digit <-> label, so callers never open-code the offset. */

#define VELAPAW_KWS_IS_DIGIT(l) \
  ((l) >= VELAPAW_KWS_ZERO && (l) <= VELAPAW_KWS_NINE)
#define VELAPAW_KWS_DIGIT_OF(l)   ((l) - VELAPAW_KWS_ZERO)
#define VELAPAW_KWS_LABEL_OF(d)   ((d) + VELAPAW_KWS_ZERO)

struct velapaw_kws_result_s
{
  int   label;                              /* enum velapaw_kws_label_e     */
  float score;                              /* winning probability, 0..1    */
  float scores[VELAPAW_KWS_CATEGORIES];     /* dequantised, all categories  */
  uint32_t feature_ms;                      /* frontend time, milliseconds  */
  uint32_t infer_ms;                        /* classifier time              */
};

/****************************************************************************
 * Name: velapaw_kws_init
 *
 * Description:
 *   Allocate both arenas and build both interpreters.  Safe to call more
 *   than once; subsequent calls are no-ops.
 *
 *   Two arenas, not one.  The upstream example shares a single arena because
 *   it builds the frontend interpreter, uses it, destroys it, and only then
 *   builds the classifier.  Both live simultaneously here so they must not
 *   overlap -- sharing would let the classifier's tensor plan scribble on the
 *   frontend's.
 *
 * Returned Value:
 *   0 on success, negated errno on failure.
 ****************************************************************************/

int velapaw_kws_init(void);

/* Clear the frontend's carried noise estimates by rebuilding its interpreter.
 *
 * The MFCC frontend is deliberately stateful: spectral subtraction and PCAN
 * adapt to the background noise floor across frames, which is what makes them
 * work in a real room.  For continuous listening that adaptation is wanted, so
 * velapaw_kws_classify() does NOT reset between calls.
 *
 * Call this when consecutive audio is unrelated -- independent test clips, or
 * a long gap in capture -- so one utterance's noise floor does not bias the
 * next.  It is also what makes results comparable with upstream's, which
 * builds a fresh interpreter per clip.
 *
 * Returns 0, or a negative errno if the rebuild fails.
 */

int velapaw_kws_reset(void);

/****************************************************************************
 * Name: velapaw_kws_classify
 *
 * Description:
 *   Run the full pipeline over one window of 16 kHz mono int16 audio:
 *   49 frontend invocations to build the feature map, then one classifier
 *   invocation.
 *
 * Input Parameters:
 *   audio     - 16 kHz mono int16 samples.
 *   nsamples  - Must be >= VELAPAW_KWS_MIN_SAMPLES.
 *   result    - Filled in on success.
 *
 * Returned Value:
 *   0 on success, negated errno on failure.
 ****************************************************************************/

int velapaw_kws_classify(const int16_t *audio, size_t nsamples,
                         struct velapaw_kws_result_s *result);

/****************************************************************************
 * Name: velapaw_kws_best_of
 *
 * Description:
 *   Pick the highest-scoring label from a SUBSET of the vocabulary, and
 *   report its score.
 *
 *   This is how the scheduling dialog reads the model, and it is worth a
 *   paragraph.  The product never asks "which of 14 words was that?" -- each
 *   dialog step already knows its legal answers (3 for the meal slot, 10 for
 *   the hour, 2 for the confirm), and restricting the argmax to them is worth
 *   a lot: measured on the held-out test split the 14-way accuracy is 80.9%
 *   while the same model is 95.7% / 85.0% / 97.5% on those three subsets.
 *
 *   The score returned is the RAW softmax probability, deliberately NOT
 *   renormalised over the subset.  Renormalising (p_best / sum p over the
 *   subset) looks tempting -- it is the "probability given that it must be
 *   one of these" -- but it throws away the probability mass sitting on
 *   _unknown_ and _silence_, which is precisely the mass that means "that
 *   was not one of my words".  Measured: at a comparable accept rate the
 *   renormalised score lets 86% of silence/out-of-vocabulary clips through
 *   the confirm step, the raw score 4.9%.  So callers threshold on this
 *   number directly.
 *
 *   Thresholds live with the consumer (VSCHED_*_SCORE in ui/ui_lvgl.c), which
 *   is where the reasoning for each one is written down.  As of build 56,
 *   re-tuned on real ES8311 capture:
 *
 *       meal slot  0.40     hour  0.55     confirm  0.55
 *
 *   They came down from 0.45/0.73/0.59 because words through this microphone
 *   score around 0.68-0.71 when correctly heard, not the ~0.99 the test split
 *   suggested.  Expect to move them again if the gain chain changes.
 *
 * Input Parameters:
 *   result  - a completed velapaw_kws_classify() result.
 *   labels  - the labels legal at this step.
 *   n       - how many.
 *   score   - if non-NULL, receives the raw probability of the winner.
 *
 * Returned Value:
 *   The winning label, or -EINVAL if the subset is empty or malformed.
 ****************************************************************************/

int velapaw_kws_best_of(const struct velapaw_kws_result_s *result,
                        const int *labels, int n, float *score);

/****************************************************************************
 * Name: velapaw_kws_label_name
 *
 * Description:
 *   Human-readable name for a category index.  Never returns NULL.
 ****************************************************************************/

const char *velapaw_kws_label_name(int label);

/****************************************************************************
 * Name: velapaw_kws_arena_used
 *
 * Description:
 *   Actual arena high-water marks reported by AllocateTensors, for shrinking
 *   the compile-time sizes once measured on hardware.  Zero before init.
 ****************************************************************************/

void velapaw_kws_arena_used(size_t *frontend, size_t *classifier);

#ifdef CONFIG_VELAPAW_KWS_SELFTEST

/****************************************************************************
 * Name: velapaw_kws_selftest
 *
 * Description:
 *   Run the pipeline against audio embedded at build time, with no
 *   microphone involved.  Two levels:
 *
 *   1. Bit-exact frontend check.  The upstream test ships the exact 40-byte
 *      feature vector the frontend must produce for the first 30 ms frame of
 *      "yes" and "no".  Matching it proves the entire DSP chain -- windowing,
 *      FFT, mel filterbank, PCAN, log -- is numerically correct on this
 *      target.  A mismatch here is a build or kernel problem, never a
 *      microphone problem.
 *
 *   2. End-to-end classification of the 1 s "yes", "no" and "silence" clips.
 *
 *   Run this BEFORE trusting any live-microphone result.  If it passes and
 *   live audio fails, the fault is in capture or levels, which is a much
 *   smaller search.
 *
 * Returned Value:
 *   0 if every check passed, negated errno otherwise.
 ****************************************************************************/

int velapaw_kws_selftest(void);

#endif /* CONFIG_VELAPAW_KWS_SELFTEST */

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_VELAPAW_VOICE_KWS_H */
