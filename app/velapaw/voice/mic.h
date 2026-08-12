/****************************************************************************
 * VelaPaw - microphone capture for keyword spotting
 *
 * Captures a fixed number of 16 kHz mono int16 samples from the onboard
 * ES8311 and returns them in a caller-supplied buffer, ready to hand to
 * velapaw_kws_classify().
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPAW_VOICE_MIC_H
#define __APPS_EXAMPLES_VELAPAW_VOICE_MIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Capture nsamples of 16 kHz mono 16-bit audio into dest.
 *
 * Blocks until the buffer is full or timeout_ms elapses.  Returns the number
 * of samples actually captured, or a negative errno.
 *
 * The first call opens the audio devices and starts them; every later call
 * reuses that session, and only a failure tears it down.  Tearing the session
 * down after each capture is what broke the second "velapaw kws mic" of a
 * boot -- RX never armed again.  See the header comment in mic.c.
 *
 * IMPORTANT -- this opens the PLAYBACK device as well as the record device,
 * and that is not optional.  esp32s3_i2s.c sets I2S_SIG_LOOPBACK whenever the
 * board config has both tx_en and rx_en (i2s_configure(), the
 * "Share BCLK and WS if in full-duplex mode" branch), which is a static board
 * property rather than anything to do with which device node you opened.  RX
 * therefore has NO bit clock of its own -- it borrows BCLK/WS from TX.  A
 * record-only capture gets no clock and blocks forever.
 *
 * So TX is primed with one buffer of silence and started first.  Submitting a
 * buffer is what actually sets I2S_TX_START; AUDIOIOC_START alone only writes
 * codec registers and spawns the worker thread.  One buffer is enough because
 * VelaPaw patch #16 clears I2S_TX_STOP_EN, so BCLK/WS keep running after the
 * TX FIFO drains instead of gating off and stalling RX.
 *
 * (This is the same deadlock nxlooper hit; see board/patches/README.md,
 * patches #6, #7 and #16.)
 *
 * dest receives plain 16 kHz mono, but that is NOT what the driver delivers.
 * The RX link runs two 16-bit slots per frame with the second one zero-filled
 * (VelaPaw patch #25 sets I2S_RX_MONO, which marks that slot invalid without
 * removing it from the stream), so mic.c keeps one int16 in two on the way
 * out.  Capturing nsamples therefore consumes 2 * nsamples from the driver and
 * takes twice as long as the raw byte count suggests.  Everything above this
 * function -- the stats, the energy gate, the model -- sees only the result.
 */

/* The largest nsamples velapaw_mic_capture() can deliver CLEANLY, and the pool
 * geometry it falls out of.  Kept here rather than in mic.c because it is a
 * limit on the caller, and build 64 proved that a caller which exceeds it does
 * not get a degraded capture -- it gets a wedged one.
 *
 * The reasoning behind each number is in mic.c next to MIC_WANT_BUFS and
 * MIC_SKIP_BUFS; the short version is that the driver delivers 4092 bytes per
 * DMA completion, MIC_STRIDE discards every second int16 of that, so one
 * delivery yields 1023 usable samples, and no buffer may be reused inside one
 * window (the PSRAM stale-cache bug from build 49).
 *
 * mic_collect() has a fallback that starts recycling buffers once the pool
 * runs dry, and on paper that makes overrunning this limit merely lossy.  It
 * does not.  Build 64 asked for 20000 samples against the 18-buffer pool that
 * caps at 17391, and RX stalled rather than recycling: the capture blocked to
 * its 8 s timeout, took the failure path, and hung inside mic_session_close()
 * with the calling thread never returning -- so the UI's voice worker was gone
 * for the rest of the boot and reported "mic unavailable" to every subsequent
 * dialog.  Treat this as a hard limit, and check it at COMPILE time: there is
 * no runtime error to catch, because the call does not come back.
 */

/* 18, not the 22 build 65 tried.  22 was allocated successfully by the driver
 * ("mic: 22 buffers x 8192 B") and did NOT fix the 1.25 s capture, so it bought
 * nothing and cost 33 KB of PSRAM plus half a second of worst-case quiesce.
 * Back to the depth every working build has used. */

#define VELAPAW_MIC_BUFS         18   /* MIC_WANT_BUFS in mic.c            */
#define VELAPAW_MIC_SKIP         1    /* MIC_SKIP_BUFS in mic.c            */
#define VELAPAW_MIC_SAMP_PER_BUF 1023 /* 4092 B / 2 slots / 2 B per sample */

#define VELAPAW_MIC_MAX_SAMPLES \
  ((VELAPAW_MIC_BUFS - VELAPAW_MIC_SKIP) * VELAPAW_MIC_SAMP_PER_BUF)

/* Optional.  Open and start the session early so the caller's timeout does
 * not have to cover it -- on this board that is 5350 ms, against dialog
 * budgets of 8000 ms.  Call from the thread that will capture: the session
 * is owned by pid, and a mismatch triggers the -110 reopen path.
 *
 * Ignoring the return value is fine.  On failure the first capture opens
 * the session inline exactly as it does without this call.
 */

int velapaw_mic_preopen(void);

int velapaw_mic_capture(int16_t *dest, size_t nsamples,
                        unsigned int timeout_ms);

/* Close the persistent capture session, releasing the audio devices.
 *
 * Optional: the session is harmless when idle and costs two open fds plus the
 * buffer pool.  Call it if something else on the board needs the codec, or on
 * a clean application exit.  Safe to call when no session is open.
 */

void velapaw_mic_shutdown(void);

/* Print peak (with its sample index), RMS, DC and rail count for a captured
 * buffer, followed by an 8-segment peak profile across the window.
 *
 * The index and the profile are the point.  A peak on its own cannot tell a
 * start-of-stream transient from speech, and reading one as the other cost
 * four builds: the transient is a full-scale sample at a FIXED index with a
 * flat noise floor behind it, while speech spreads energy across several
 * segments.  Always look at the profile before concluding anything about
 * gain.
 *
 * Reference levels at MIC_GAIN_DB 30, build 52, on de-interleaved audio --
 * i.e. a full second of real samples, which is what the model finally sees:
 *   silence  rms  26  profile    87    82    87    68    55    67    56    60
 *   "yes"    rms 645  profile   447    90   107   100  4085  5019  1426  1724
 *   "no"     rms 791  profile  1949  5402  3534   596    74    66    87    94
 * with rail 0 and dc 0 throughout.  Those two utterances scored yes 0.996 and
 * no 0.750, so this is what a healthy window looks like.
 *
 * A railed peak at a repeatable index means the warm-up is not covering the
 * start-of-stream transient, NOT that the gain is too high.
 */

void velapaw_mic_stats(const int16_t *audio, size_t nsamples);

/* True if the window holds sustained sound rather than silence or a single
 * artifact.  Call this BEFORE velapaw_kws_classify().
 *
 * micro_speech does not reject silence for you -- given a near-zero window it
 * returns a confident wrong label rather than "silence", so a continuously
 * listening feeder would act on an empty room.  The test is duration-based
 * (how many eighths of the window carry level), because peak and RMS both
 * pass a lone start-of-stream transient.  See mic.c for the measurements.
 */

bool velapaw_mic_speech_present(const int16_t *audio, size_t nsamples);

/* Where capture has got to, for reading from ANOTHER task while a capture is
 * in progress.
 *
 * This exists because build 60 proved the UI's voice worker enters
 * velapaw_mic_capture() and never comes back, while `velapaw kws mic 1` in the
 * same boot captures normally.  Everything that distinguishes those two paths
 * is inside this file, and none of it can be seen from outside: the failure is
 * a block, not a return value, so there is no errno to inspect and the worker
 * never reaches a printf.  MIC_VERBOSE would show it, but it prints from the
 * blocked thread on a console the UI has set O_NONBLOCK, and it floods badly
 * enough to starve LVGL -- so the state is published to memory instead and
 * read out later, from a task, over a working console.
 *
 * velapaw_mic_stage() names the last step STARTED.  If it is still the same
 * step seconds later, that step is where the block is.
 *
 * velapaw_mic_stage_seq() counts stage transitions since boot.  Two probes
 * with the same stage but a rising seq mean capture is looping, not stuck --
 * the distinction a single reading cannot make.
 *
 * Both are plain volatile ints written by one thread and read by another: no
 * lock, because a torn read of an int does not happen on this target and a
 * missed update only costs one stale probe.
 */

int velapaw_mic_stage(void);
unsigned velapaw_mic_stage_seq(void);
const char *velapaw_mic_stage_name(int stage);

/* The errno of the last capture that took the failure path, or 0.
 *
 * Builds 64 and 65 both died with the caller's own return value still unset,
 * because velapaw_mic_capture() never returned: it failed in mic_collect(),
 * went to the teardown path, and blocked inside mic_session_close().  The
 * failure printf on that path is real but goes to a console that drops it, so
 * three builds established WHERE it fails and none of them established WHY.
 *
 * Published before the teardown that hangs, so it survives the hang and can be
 * read by any other task afterwards.  0 means no capture has failed. */

int velapaw_mic_last_error(void);

/* What the driver's counters read at the moment a capture gave up.
 *
 * -110 says the collect timed out; it does not say whether RX never started,
 * started and stalled, or ran normally into a buffer pool that could not cover
 * the request.  Those want completely different fixes and the counters
 * separate them outright:
 *
 *   rhead   should equal the pool depth -- proof every buffer was accepted
 *           into es8311's pendq.  Short means arming failed, not receiving.
 *   rtail   completions.  0 means RX never delivered ONE buffer, i.e. no bit
 *           clock or no worker thread; the codec never ran at all.
 *   seq     buffers accepted into this window (after MIC_SKIP_BUFS).  Nonzero
 *           with a short got means delivery is working but too slow -- a
 *           timeout that a longer budget would have survived.
 *   phase   -1 means every buffer that did arrive was zero to the bit, so RX
 *           is running and the microphone side is dead.  See mic_data_phase().
 *
 * Read this together with velapaw_mic_stage(): the stage says where the code
 * stopped, these say what the hardware was doing when it did.  Published from
 * inside mic_collect() before the teardown, for the same reason the errno is.
 */

struct velapaw_mic_fail_s
{
  unsigned long got;    /* output bytes collected when the capture gave up */
  unsigned long want;   /* output bytes it was asked for                   */
  unsigned long rhead;  /* record enqueues the driver accepted             */
  unsigned long rtail;  /* record buffers the driver handed back           */
  unsigned long phead;  /* playback, same pair                             */
  unsigned long ptail;
  int           seq;    /* record buffers accepted into this window        */
  int           phase;  /* live slot, or -1 if no buffer ever had content  */
};

void velapaw_mic_last_fail(struct velapaw_mic_fail_s *out);

/* The device states observed while opening a session, for the last two
 * sessions opened.
 *
 * Two, not one, because the interesting comparison is between a session that
 * worked and one that did not, and the cheapest way to get both is one boot
 * where `velapaw kws mic 1` runs before the UI's worker does.  A single
 * reading would only say what the failing session looked like; a pair says
 * what it looked like DIFFERENTLY.
 *
 * The numbers to read, from audio.c:
 *
 *   rec_cfg    MUST be 1 (PREPARED).  0 (OPEN) would mean the ioctl succeeded
 *              without doing anything -- audio_configure() only calls the
 *              lower half from state OPEN and otherwise falls through with
 *              ret = OK -- leaving the record node in ES_MODULE_DAC, where it
 *              sends record buffers to I2S_SEND and RX is never armed.  That
 *              produces head = pool depth with tail = 0, which is exactly what
 *              build 67 measured.
 *   rec_start  MUST be 5 (RUNNING).  Anything else means audio_start() took
 *              the same silent fall-through and es8311_start() never ran, so
 *              there is no worker thread to drain pendq into I2S_RECEIVE.
 *   play_start MUST be 5 as well; TX is what generates the clock RX borrows.
 *
 * States: 0 OPEN, 1 PREPARED, 2 PAUSED, 3 XRUN, 4 DRAINING, 5 RUNNING.
 * A negative value means the GETSTATUS ioctl itself failed.
 */

struct velapaw_mic_open_s
{
  int      pid;         /* task that opened this session                    */
  int      rec_cfg;     /* record state after AUDIOIOC_CONFIGURE            */
  int      rec_start;   /* record state after AUDIOIOC_START                */
  int      play_start;  /* playback state after AUDIOIOC_START              */
  int      nbufs;       /* record buffers actually granted                  */
  int      ret;         /* 0 if the open completed, else where it stopped   */
  unsigned seq;         /* stage sequence at entry, to order the sessions   */
};

/* Fills up to max entries, oldest first, and returns how many were written. */

int velapaw_mic_open_log(struct velapaw_mic_open_s *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_VELAPAW_VOICE_MIC_H */
