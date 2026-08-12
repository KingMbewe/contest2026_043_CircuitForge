/****************************************************************************
 * VelaPaw - microphone capture (Phase 1.5)
 *
 * Modelled on apps/system/nxlooper, which is the only record path proven to
 * work on this board.  The important difference from a textbook NuttX capture
 * is that the playback device must be opened, primed and started too -- see
 * mic.h for why, and board/patches/README.md patches #6/#7/#16.
 *
 * PERSISTENT SESSION
 * ------------------
 * The audio devices are opened, configured and STARTed once and then left
 * that way.  Each capture only enqueues buffers and collects them again.
 *
 * This is not a performance tweak, it is the fix for the second-capture bug.
 * Builds 41 and 42 tore the whole session down after every capture, and the
 * second "velapaw kws mic" of a boot never received a single RX buffer:
 *
 *   build 41 run 2:  mic: timed out with 4092/32000 bytes
 *                    [rec t/out] state=5 head=5 tail=1
 *   build 42 run 2:  mic: timed out with 0/32000 bytes
 *                    [rec t/out] state=5 head=4 tail=0
 *                    [play t/out] state=5 head=1 tail=1   <- TX is fine
 *
 * `ps` taken at the prompt in that state shows no es8311 threads at all, so
 * nothing was blocked: the workers had been joined and had exited cleanly,
 * and the container pool in esp32s3_i2s.c was not exhausted.  The app-side
 * counters were all nominal.  The damage is in I2S RX state that the teardown
 * leaves behind, and the cheapest correct answer is to stop tearing down.
 *
 * Between captures the session is left running but idle: no record buffer is
 * enqueued, so es8311's worker has nothing to hand to I2S_RECEIVE, RX stops
 * arming on its own and nothing accumulates in the message queue.  BCLK keeps
 * running throughout because patch #16 clears I2S_TX_STOP_EN, so the next
 * capture starts against a clock that never stopped.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <mqueue.h>
#include <time.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>

#include "mic.h"

/* Build 79.  Driver patch #36 (arch/xtensa/src/esp32s3/esp32s3_i2s.c).
 * Resets the I2S0 RX module, its FIFO and the GDMA IN channel, rescues any
 * container the reset orphaned back onto the pending queue, and re-arms.
 *
 * This is the only recovery lever that is safe to pull from here.  The
 * obvious alternatives -- AUDIOIOC_STOP, AUDIOIOC_CONFIGURE, AUDIOIOC_START
 * -- all run through the audio upper half and the codec, which is where
 * this path is already suspected of hanging; a retry built out of them
 * would hang on its first iteration.  The kick touches registers and the
 * driver queues only, under the driver spinlock, and cannot block.
 *
 * Weak on purpose: without patch #36 this resolves to NULL at link time
 * and the retry below degrades to a plain retry rather than failing to
 * build.  Flat build, so the direct call is legitimate -- the same way the
 * UI already externs the driver probe counters.
 */

extern void velapaw_i2s_rx_kick(void) __attribute__((weak));

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
#  error "velapaw voice capture assumes CONFIG_AUDIO_MULTI_SESSION=n"
#endif

#define MIC_RATE      16000
#define MIC_CHANNELS  1
#define MIC_BPSAMP    16
#define MIC_CHMAP     1        /* single front-left slot */

/* ES8311 ADC PGA gain in dB, or negative to leave the codec's own reset
 * default in place.
 *
 * Build 42 set this to 0 dB on the theory that the reset value was too hot,
 * reading a railed peak as clipping.  That was wrong: the railed samples were
 * a start-of-stream transient at a FIXED sample index, not acoustic content,
 * and turning gain down made nothing better.
 *
 * Build 45 measured the signal properly instead of inferring it from a peak.
 * Speech occupies about 300 counts, the noise floor is 2, and a deliberately
 * silent capture came back rms 0 / peak 8:
 *
 *   spoken "no":  profile 16331   191   303   296   200    62    14    28
 *   silence:      profile     8     2     2     2     2     2     2     2
 *
 * So the capture is quiet, not clipped, with plenty of SNR to amplify. 30 dB
 * is x31.6, putting speech near 9500 of 32767 -- a comfortable working level.
 * 42 dB, the driver's ceiling, would be x126 and would genuinely clip.
 *
 * If this changes nothing measurable, the ES8311 gain path is not wired the
 * way the driver believes and the next step is es8311_dump_registers().
 */

#define MIC_GAIN_DB   30

#define MIC_MAX_BUFS  24
#define MIC_PATH_MAX  64

/* The RX stream is TWO slots per frame and only one of them is the mic.
 *
 * esp32s3_i2s.c programs 16-bit slots (i2s_set_datawidth: CHAN_BITS =
 * HALF_SAMPLE_BITS = data_width), so this is not 32-bit padding -- it is the
 * channel map, from our own VelaPaw patch #25:
 *
 *   I2S_RX_TDM_TOT_CHAN_NUM = 1        (i.e. two slots)
 *   rx_chan_mask |= CHAN0_EN | CHAN1_EN
 *   I2S_RX_CONF.I2S_RX_MONO = 1        (slot 1 invalid)
 *
 * I2S_RX_MONO marks the second slot invalid but does NOT remove it from the
 * stream: both slots still reach the RX FIFO and the DMA, with the invalid one
 * zero-filled.  The buffer therefore reads [sample, 0, sample, 0, ...] and the
 * build-50 trace shows exactly that -- s[1] was 0 to the bit on all 24 buffers
 * of the run while s[0] varied.  (Build 49 had it the other way round, which
 * is why the phase is measured below rather than assumed.)
 *
 * The consequence is the one that mattered: fed straight to micro_speech, a
 * 16000-entry window is 8000 frames -- half a second of audio with a zero
 * between every sample.  That is a mirrored spectrum landing inside the mel
 * bins, and it is why every score this project has ever produced was weak
 * (build 46's best live results were "yes" 0.539 and "no" 0.715).
 *
 * So mic_collect() keeps one int16 in two.  Everything above it -- nsamples,
 * velapaw_mic_stats, the energy gate, the model -- still sees plain 16 kHz
 * mono and needs no knowledge of this.
 */

#define MIC_STRIDE 2

/* Record buffers to allocate, overriding the driver's suggestion of 4.
 *
 * GETBUFFERINFO answers 4 x 8192, but the DMA only ever fills 4092 bytes of
 * each -- the descriptor chain's eofnum -- and MIC_STRIDE throws half of that
 * away, so one delivery yields 1023 usable samples.  A 1 s window at 16 kHz
 * therefore needs ceil(16000/1023) = 16 deliveries plus MIC_SKIP_BUFS at the
 * front, and every one of them has to be a DIFFERENT buffer:
 *
 *   build 49 trace, capture 3, after a cold power cycle, four buffers only
 *     seq=0 apb=0x3c510e08 s[1]=1715    seq=4 apb=0x3c510e08 s[1]=1715
 *     seq=1 apb=0x3c512ea8 s[1]=-194    seq=5 apb=0x3c512ea8 s[1]=-194
 *     seq=2 apb=0x3c514f48 s[1]=-856    seq=6 apb=0x3c514f48 s[1]=-856
 *     seq=3 apb=0x3c50ed68 s[1]=851     seq=7 apb=0x3c50ed68 s[1]=851
 *
 * The second delivery of a buffer carried the FIRST delivery's samples, so
 * half of every window was a duplicate of the other half -- which is also why
 * the eight-segment profile came out periodic with a period of four buffers.
 * Those addresses are in the 0x3C... external-memory window, i.e. PSRAM
 * through the data cache (CONFIG_ESP32S3_SPIRAM=y, CONFIG_MM_REGIONS=2), and
 * esp32s3_i2s.c does no cache maintenance at all: no invalidate, no writeback,
 * no barrier.  First delivery misses the cache and reads what the DMA wrote;
 * the copy pulls 4 KB of it into DCache; ~500 ms later the same buffer comes
 * back and the read hits those still-warm lines.
 *
 * Build 50 raised this from 4 to 10 and the duplication vanished -- eight
 * distinct pointers per window, and profiles that stopped mirroring their own
 * first half.  MIC_STRIDE doubles the requirement again.
 *
 * nbuffers from GETBUFFERINFO is a suggestion, not a cap: ALLOCBUFFER is
 * per-call and audio.c only counts what it has already handed out.  If the
 * pool really is limited, mic_alloc_buffer() fails and says which one.
 */

/* Was 18, which covered a 1 s window exactly: 16 deliveries plus the skip, with
 * one spare.  Build 64 widened the capture to 1.25 s for window alignment and
 * needed 20, and going past the pool did not degrade gracefully -- see the
 * VELAPAW_MIC_MAX_SAMPLES block in mic.h for what actually happened.
 *
 * 22 covers 1.25 s (20 deliveries plus the skip) with one spare, the same
 * margin 18 gave 1 s.  It costs 4 more 8192-byte buffers out of the audio
 * driver's PSRAM pool -- 33 KB against several megabytes free -- and pushes
 * the worst-case quiesce drain from 2.3 s to 2.8 s, still inside the 4 s
 * MIC_QUIESCE_TRIES budget, so nothing downstream has to move.
 *
 * MIC_MAX_BUFS is 24 and recbufs[] is sized from it, so this needed no change
 * to the context struct.  If the driver refuses to hand out 22, the allocation
 * loop says so ("pool capped at N buffers") and carries on with what it got --
 * at which point the capture is over the limit again and mic.h's arithmetic is
 * what has to change, not this number alone.
 */

#define MIC_WANT_BUFS VELAPAW_MIC_BUFS

#if MIC_WANT_BUFS > MIC_MAX_BUFS
#  error "VELAPAW_MIC_BUFS exceeds MIC_MAX_BUFS; recbufs[] would overflow"
#endif

/* Below this the session is not worth starting: it is the depth every build
 * up to 49 ran with, so anything less means something is genuinely wrong.
 */

#define MIC_MIN_BUFS  4

/* Record buffers to allocate, overriding the driver's suggestion of 4.
 *
 * GETBUFFERINFO answers 4 x 8192, but the DMA only ever fills 4092 bytes of
 * each -- the descriptor chain's eofnum -- so a 1 s window at 16 kHz mono
 * int16 (32000 bytes) needs ceil(32000/4092) = 8 deliveries, plus
 * MIC_SKIP_BUFS at the front.  Four buffers cannot cover that without being
 * recycled mid-window, and recycling them is what corrupts the capture:
 *
 *   build 49 trace, capture 3, after a cold power cycle
 *     seq=0 apb=0x3c510e08 s[1]=1715    seq=4 apb=0x3c510e08 s[1]=1715
 *     seq=1 apb=0x3c512ea8 s[1]=-194    seq=5 apb=0x3c512ea8 s[1]=-194
 *     seq=2 apb=0x3c514f48 s[1]=-856    seq=6 apb=0x3c514f48 s[1]=-856
 *     seq=3 apb=0x3c50ed68 s[1]=851     seq=7 apb=0x3c50ed68 s[1]=851
 *
 * The second delivery of each buffer carries the FIRST delivery's samples, so
 * half of every window was a duplicate of the other half -- which is also why
 * the 8-segment profile came out periodic with a period of exactly four
 * buffers.  Those addresses sit in the 0x3C... external-memory window, i.e.
 * PSRAM reached through the data cache, and esp32s3_i2s.c does no cache
 * maintenance at all: no invalidate, no writeback, no barrier.  First delivery
 * misses the cache and reads what the DMA wrote; the memcpy pulls 4 KB of it
 * into DCache; ~500 ms later the same buffer comes back and the read hits
 * those still-warm lines.
 *
 * Allocating enough that no buffer is reused inside one window sidesteps it.
 * If it does NOT, the cache reading is wrong and the duplication lives in the
 * driver's descriptor handling instead -- which is worth knowing either way.
 *
 * nbuffers from GETBUFFERINFO is a suggestion, not a cap: ALLOCBUFFER is
 * per-call and audio.c only counts what it has already handed out.  If the
 * pool really is limited, mic_alloc_buffer() fails and says which one.
 */


/* Record buffers to throw away at the start of every capture.
 *
 * Build 44's first buffer held a full-scale sample at index 127 with the rest
 * near silence, and build 42 saw both of its probes peak at the identical
 * index 57 -- a fixed position, which acoustic content does not have.  That is
 * a start-of-stream transient: the ADC settling, plus whatever the codec does
 * on its first frames.
 *
 * One buffer is 8192 B = 4096 samples of headroom in the driver but only 2046
 * samples land per DMA completion, so this costs about 128 ms at the front of
 * the window.  The capture simply runs that much longer.
 */

#define MIC_SKIP_BUFS VELAPAW_MIC_SKIP

/* Record buffers to run through and discard immediately after START.
 *
 * The start-of-stream transient is far worse on the first capture of a session
 * than on later ones, because that is where the codec actually powers up its
 * ADC.  Build 45, same boot, same code path:
 *
 *   capture 1 (just opened):  driver probe peak=32768@43 rail=40, and it
 *                             spilled past the skipped buffer -- the retained
 *                             second still read peak 32701@45
 *   capture 3 (re-armed):     driver probe peak=32768@51 rail=2, and the
 *                             retained second read peak 8
 *
 * One skipped buffer per capture handles the re-arm case. The power-up case
 * needs more, and paying for it on every capture would add 128 ms of latency
 * each time for a transient that is only there once.  So it is charged to
 * session open instead, where nobody is waiting.
 */

#define MIC_WARMUP_BUFS 3

/* Bring-up chatter: device paths, buffer geometry, per-capture head/tail
 * counters, quiesce and warm-up accounting.
 *
 * Off by default.  These are per-capture, and the product listens
 * continuously from the UI task, so leaving them on turns the console into a
 * scrolling wall and starves LVGL.  They are kept rather than deleted because
 * every one of them was the thing that identified a real bug, and the next
 * codec problem will want them back -- set this to 1 and rebuild.
 *
 * Genuine failures print unconditionally.  Only the happy path is gated.
 */

#ifndef MIC_VERBOSE
#  define MIC_VERBOSE 0
#endif

#if MIC_VERBOSE
#  define micinfo(...) printf(__VA_ARGS__)
#else
#  define micinfo(...) do { } while (0)
#endif

/* Per-buffer capture trace.  Separate from MIC_VERBOSE because it is loud
 * (one line per 128 ms) and exists to answer one specific question -- see the
 * block in mic_collect().  Turn off once build 48's periodic profile is
 * explained.
 */

#ifndef MIC_TRACE
#  define MIC_TRACE 0
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mic_ctx_s
{
  bool   open;                 /* session is live: devices open and RUNNING */
  pid_t  owner;                /* task that owns recfd/playfd */
  int    recfd;
  int    playfd;
  mqd_t  mq;
  char   mqname[24];
  bool   mq_open;
  bool   rec_reserved;
  bool   play_reserved;
  bool   rec_started;
  bool   play_started;

  struct ap_buffer_s *recbufs[MIC_MAX_BUFS];
  int    nrecbufs;
  struct ap_buffer_s *playbuf;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The session outlives any one capture.  Single-threaded by contract: the
 * caller is velapaw's own worker thread, one capture at a time.
 */

static struct mic_ctx_s g_mic =
{
  .recfd  = -1,
  .playfd = -1,
};

/* Capture progress, published for velapaw_mic_stage().  See mic.h for why.
 *
 * The names are not decoration.  A bare number would have to be matched
 * against this file by eye, and -- more usefully -- a number is invisible in
 * the flash image, while these strings land in .rodata where `grep -a` can
 * confirm the build on the board is the one that was just compiled.  That
 * check has no other cheap form: the arena sizes taught us that a constant
 * passed as a %d argument leaves no trace to grep for.
 *
 * MIC_STAGE is a statement macro rather than an inline function so a stage
 * marker reads as one line at the call site and does not disturb the
 * surrounding code's shape.
 */

static const char * const g_mic_stage_names[] =
{
  "idle",              /*  0 */
  "enter",             /*  1 */
  "find-devices",      /*  2 */
  "open-rec",          /*  3 */
  "open-play",         /*  4 */
  "reserve-rec",       /*  5 */
  "reserve-play",      /*  6 */
  "configure-rec",     /*  7 */
  "status-rec-cfg",    /*  8 */
  "configure-play",    /*  9 */
  "status-play-cfg",   /* 10 */
  "set-gain",          /* 11 */
  "bufinfo-rec",       /* 12 */
  "bufinfo-play",      /* 13 */
  "alloc-rec-bufs",    /* 14 */
  "alloc-play-buf",    /* 15 */
  "mq-open",           /* 16 */
  "register-mq",       /* 17 */
  "arm",               /* 18 */
  "status-enq",        /* 19 */
  "start-play",        /* 20 */
  "status-play-start", /* 21 */
  "start-rec",         /* 22 */
  "status-rec-start",  /* 23 */
  "warmup",            /* 24 */
  "session-open-done", /* 25 */
  "flush-mq",          /* 26 */
  "rearm",             /* 27 */
  "status-rearm",      /* 28 */
  "collect",           /* 29 */
  "quiesce",           /* 30 */
  "dcblock",           /* 31 */
  "session-close",     /* 32 */
  "done",              /* 33 */

  /* Build 79.  Stage 32 used to cover the whole of mic_session_close(),
   * which is precisely the function this path hangs in -- so "stage 32"
   * said no more than "somewhere in the teardown", and the teardown is
   * eleven ioctls long.  Split it.
   */

  "kick",              /* 34 */
  "kick-flush",        /* 35 */
  "kick-rearm",        /* 36 */
  "kick-collect",      /* 37 */
  "close-stop",        /* 38 */
  "close-quiesce",     /* 39 */
  "close-unreg-mq",    /* 40 */
  "close-free-bufs",   /* 41 */
  "close-release",     /* 42 */
  "close-mq",          /* 43 */
  "close-fds"          /* 44 */
};

#define MIC_STAGE_IDLE       0
#define MIC_STAGE_ENTER      1
#define MIC_STAGE_FIND       2
#define MIC_STAGE_OPEN_REC   3
#define MIC_STAGE_OPEN_PLAY  4
#define MIC_STAGE_RSV_REC    5
#define MIC_STAGE_RSV_PLAY   6
#define MIC_STAGE_CFG_REC    7
#define MIC_STAGE_ST_REC_CFG 8
#define MIC_STAGE_CFG_PLAY   9
#define MIC_STAGE_ST_PLY_CFG 10
#define MIC_STAGE_GAIN       11
#define MIC_STAGE_BUFI_REC   12
#define MIC_STAGE_BUFI_PLAY  13
#define MIC_STAGE_ALLOC_REC  14
#define MIC_STAGE_ALLOC_PLAY 15
#define MIC_STAGE_MQ_OPEN    16
#define MIC_STAGE_REG_MQ     17
#define MIC_STAGE_ARM        18
#define MIC_STAGE_ST_ENQ     19
#define MIC_STAGE_START_PLAY 20
#define MIC_STAGE_ST_PLY_STA 21
#define MIC_STAGE_START_REC  22
#define MIC_STAGE_ST_REC_STA 23
#define MIC_STAGE_WARMUP     24
#define MIC_STAGE_OPENED     25
#define MIC_STAGE_FLUSH      26
#define MIC_STAGE_REARM      27
#define MIC_STAGE_ST_REARM   28
#define MIC_STAGE_COLLECT    29
#define MIC_STAGE_QUIESCE    30
#define MIC_STAGE_DCBLOCK    31
#define MIC_STAGE_CLOSE      32
#define MIC_STAGE_DONE       33
#define MIC_STAGE_KICK       34
#define MIC_STAGE_KICK_FLSH  35
#define MIC_STAGE_KICK_ARM   36
#define MIC_STAGE_KICK_COLL  37
#define MIC_STAGE_CL_STOP    38
#define MIC_STAGE_CL_QUIESCE 39
#define MIC_STAGE_CL_UNREG   40
#define MIC_STAGE_CL_FREE    41
#define MIC_STAGE_CL_RELEASE 42
#define MIC_STAGE_CL_MQ      43
#define MIC_STAGE_CL_FDS     44

/* How many times a failed collect is retried in-session before falling
 * back to the teardown.  Each try costs one capture timeout, and the
 * caller (the voice worker) is already waiting on this, so keep it small
 * enough that a hopeless case still gives up inside the UI's own 8 s
 * readiness window rather than long after the user gave up.
 */

#define MIC_KICK_TRIES       2

static volatile int      g_mic_stage;
static volatile unsigned g_mic_stage_seq;

#define MIC_STAGE(s) \
  do { g_mic_stage = (s); g_mic_stage_seq++; } while (0)

/* Set for the duration of velapaw_mic_capture().  See the guard at the top of
 * that function for why a flag and not a mutex.
 */

static volatile bool  g_mic_busy;
static volatile pid_t g_mic_busy_pid;

/* Last capture failure errno; see velapaw_mic_last_error() in mic.h. */

static volatile int   g_mic_last_error;

/* Build 79 recovery census, read by the voice probe.
 *
 *   kicks 0             no capture ever needed recovery
 *   kicks n, ok n       the stall happens and the kick clears it
 *   kicks n, ok 0       the stall happens and the kick does not touch it,
 *                       i.e. the fault really is below the DMA
 *
 * The third reading is the one worth having: it would be the first hard
 * evidence separating `the DMA got wedged` from `no samples are arriving`,
 * and it costs one boot to obtain.
 */

uint32_t g_mic_kicks;
uint32_t g_mic_kick_ok;

/* Set when a kick has been issued and no capture has succeeded since, so
 * that the success is credited to the kick exactly once.
 */

static volatile bool g_mic_kick_armed;

/* Driver counters at the moment of that failure; see velapaw_mic_last_fail().
 * Not volatile as a struct -- it is written once by the failing capture and
 * read afterwards by a task, never concurrently, because the capture that
 * wrote it is over by the time anything can ask. */

static struct velapaw_mic_fail_s g_mic_fail;

/* The last two session opens; see velapaw_mic_open_log() in mic.h.  Written
 * progressively as the open proceeds, so a session that never finishes still
 * leaves everything it managed to observe. */

static struct velapaw_mic_open_s g_mic_open[2];
static unsigned                  g_mic_opens;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void mic_dcblock(int16_t *audio, size_t nsamples);
static void mic_session_close(struct mic_ctx_s *ctx);

/* Find a record-capable and a playback-capable node under /dev/audio.
 *
 * Done by capability rather than by name: the device names come from the
 * board's audio registration, which lives in the VM board tree, and a wrong
 * guess would present as ENOENT at run time for no good reason.
 */

static int mic_find_devices(char *inpath, char *outpath, size_t len)
{
  struct audio_caps_s caps;
  struct dirent *entry;
  char path[MIC_PATH_MAX];
  DIR *dirp;
  int found = 0;
  int fd;

  inpath[0]  = '\0';
  outpath[0] = '\0';

  dirp = opendir("/dev/audio");
  if (dirp == NULL)
    {
      printf("mic: cannot open /dev/audio: %d\n", errno);
      return -errno;
    }

  while ((entry = readdir(dirp)) != NULL)
    {
      snprintf(path, sizeof(path), "/dev/audio/%s", entry->d_name);

      fd = open(path, O_RDWR | O_CLOEXEC);
      if (fd < 0)
        {
          continue;
        }

      memset(&caps, 0, sizeof(caps));
      caps.ac_len     = sizeof(caps);
      caps.ac_type    = AUDIO_TYPE_QUERY;
      caps.ac_subtype = AUDIO_TYPE_QUERY;

      if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps) == caps.ac_len)
        {
          /* ONE ROLE PER NODE -- an else-if chain, exactly as nxlooper does
           * in nxlooper_opendevice().  That is not incidental style.
           *
           * This board exposes two nodes: /dev/audio/pcm0, which reports BOTH
           * directions, and /dev/audio/pcm_in0, which is the record side.
           * Treating "reports both" as "usable for both" looks reasonable and
           * is wrong -- it claims pcm0 twice, so the capture opens and
           * reserves one device through two fds, pcm_in0 is never touched,
           * and RX never delivers a single buffer (timed out with 0/32000).
           *
           * The chain is order-independent here: whichever of the two is
           * enumerated first, pcm0 ends up on playback and pcm_in0 on record.
           */

          if (outpath[0] == '\0' &&
              (caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT) != 0)
            {
              strlcpy(outpath, path, len);
              found++;
            }
          else if (inpath[0] == '\0' &&
                   (caps.ac_controls.b[0] & AUDIO_TYPE_INPUT) != 0)
            {
              strlcpy(inpath, path, len);
              found++;
            }
        }

      close(fd);

      if (inpath[0] != '\0' && outpath[0] != '\0')
        {
          break;
        }
    }

  closedir(dirp);

  if (inpath[0] == '\0' || outpath[0] == '\0')
    {
      printf("mic: need both a record and a playback node "
             "(in='%s' out='%s')\n", inpath, outpath);
      return -ENODEV;
    }

  /* Cannot happen with the chain above, but say so loudly rather than hang
   * for five seconds if the board ever registers a single full-duplex node.
   */

  if (strcmp(inpath, outpath) == 0)
    {
      printf("mic: record and playback resolved to the same node (%s)\n",
             inpath);
      return -ENODEV;
    }

  micinfo("mic: record=%s playback=%s\n", inpath, outpath);
  return found;
}

static int mic_configure(int fd, int type)
{
  struct audio_caps_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len            = sizeof(struct audio_caps_s);
  desc.caps.ac_type           = type;
  desc.caps.ac_channels       = MIC_CHANNELS;
  desc.caps.ac_chmap          = MIC_CHMAP;
  desc.caps.ac_subtype        = AUDIO_FMT_PCM;
  desc.caps.ac_controls.hw[0] = MIC_RATE;
  desc.caps.ac_controls.b[3]  = MIC_RATE >> 16;
  desc.caps.ac_controls.b[2]  = MIC_BPSAMP;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
    {
      printf("mic: AUDIOIOC_CONFIGURE(type %d) failed: %d\n", type, errno);
      return -errno;
    }

  return 0;
}

/* Set the ES8311 ADC PGA gain, in dB.  The driver clamps to 0..42 and
 * quantises to 6 dB steps (es8311_setmicgain -> ES8311_ADC_REG16).
 *
 * This MUST be issued AFTER the AUDIO_TYPE_INPUT configure: es8311_configure
 * calls es8311_reset(), which hard-writes REG16 = 0x20 and would wipe any
 * gain set beforehand.  es8311_start touches REG17 (digital volume) but not
 * REG16, so a gain set here survives START.
 */

static int mic_set_gain(int fd, int db)
{
  struct audio_caps_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len            = sizeof(struct audio_caps_s);
  desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
  desc.caps.ac_format.hw      = AUDIO_FU_INP_GAIN;
  desc.caps.ac_controls.hw[0] = (uint16_t)db;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
    {
      printf("mic: set gain %d dB failed: %d\n", db, errno);
      return -errno;
    }

  micinfo("mic: ADC gain %d dB\n", db);
  return 0;
}

/* Read the DEVICE-level state out of the audio upper half.
 *
 * AUDIOIOC_GETSTATUS memcpys upper->status, and upper->status->state is the
 * exact field that both audio_configure() and audio_start() branch on -- and
 * both of them have a silent-success path:
 *
 *   audio_configure() calls the lower half ONLY when the state is OPEN(0)
 *   (for INPUT/OUTPUT types).  Any other state skips the body, leaves ret at
 *   OK and returns 0 -- es8311_configure() never runs, so audio_mode keeps
 *   whatever es8311_initialize() left it at, which is ES_MODULE_DAC.
 *
 *   audio_start() calls lower->ops->start ONLY when the state is PREPARED(1)
 *   or XRUN(3).  It returns -EPERM for OPEN(0), but for anything else it
 *   falls through to "return OK" WITHOUT starting the codec.
 *
 * Either way the ioctl returns 0 and nothing happens, which is
 * indistinguishable from success at the application level.  That is why this
 * is measured rather than assumed.
 *
 * head is the decisive counter: audio_enqueuebuffer() increments
 * upper->status->head only AFTER lower->ops->enqueuebuffer() has returned OK,
 * so head == nrecbufs proves every record buffer really did land in the
 * es8311 pending queue.
 *
 * With a persistent session these counters are cumulative for the life of the
 * session rather than for one capture, so read them as "in flight = head -
 * tail", not as absolute numbers.
 *
 * States: 0 OPEN, 1 PREPARED, 2 PAUSED, 3 XRUN, 4 DRAINING, 5 RUNNING.
 */

static void mic_status(int fd, const char *tag)
{
#if MIC_VERBOSE
  struct audio_status_s st;

  memset(&st, 0, sizeof(st));
  if (ioctl(fd, AUDIOIOC_GETSTATUS, (unsigned long)&st) < 0)
    {
      printf("mic: [%-10s] GETSTATUS failed: %d\n", tag, errno);
      return;
    }

  printf("mic: [%-10s] state=%d head=%lu tail=%lu\n",
         tag, st.state, (unsigned long)st.head, (unsigned long)st.tail);
#else
  (void)fd;
  (void)tag;
#endif
}

/* The device state from AUDIOIOC_GETSTATUS, or a negative errno.
 *
 * This is the same read mic_status() does, minus the printing -- which is the
 * point.  mic_status() is compiled out at MIC_VERBOSE 0 and prints to a console
 * that drops writes when it is not, so the states the comments above have been
 * insisting on since build 45 have never actually been observed.  This one is
 * always compiled and goes to memory.
 */

static int mic_state(int fd)
{
  struct audio_status_s st;

  if (fd < 0)
    {
      return -EBADF;
    }

  memset(&st, 0, sizeof(st));
  if (ioctl(fd, AUDIOIOC_GETSTATUS, (unsigned long)&st) < 0)
    {
      return -errno;
    }

  return (int)st.state;
}

static int mic_alloc_buffer(int fd, struct ap_buffer_s **out, int numbytes)
{
  struct audio_buf_desc_s desc;
  int ret;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes  = numbytes;
  desc.u.pbuffer = out;

  ret = ioctl(fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
  if (ret != sizeof(desc))
    {
      printf("mic: AUDIOIOC_ALLOCBUFFER failed: %d\n", ret);
      return -ENOMEM;
    }

  return 0;
}

/* Submit a buffer.  The caller owns apb->nbytes and must set it first: on the
 * record side it means "empty, fill me" (0) and on the playback side it means
 * "this much data to send" (nmaxbytes).  It is read by the driver during the
 * ioctl, so it cannot be fixed up afterwards.
 */

static int mic_enqueue(int fd, struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes  = apb->nmaxbytes;
  desc.u.buffer  = apb;

  if (ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc) < 0)
    {
      return -errno;
    }

  return 0;
}

/* Wait until nothing is in flight on either device.
 *
 * A capture stops requeueing the moment it has the samples it asked for, so a
 * couple of record buffers are usually still down in the driver at that point.
 * They have to come home before the next capture re-enqueues them, otherwise
 * that capture would hand the driver a buffer it is already holding.
 *
 * tail counts completions and head counts accepted enqueues, so tail == head
 * on both devices means nothing is in flight.  Bounded, because a wedged
 * driver must not hang the shell.
 */

/* One buffer is 2046 frames = 128 ms of audio, so a full pool takes
 * MIC_WANT_BUFS x 128 ms to drain -- 2.3 s at 18 buffers.  The old budget was
 * 1 s, sized when the pool was 4, and build 51 blew straight through it: the
 * quiesce gave up with most of the pool still held by the driver, capture 2
 * armed with only six free buffers, and those six recycled three times over
 * inside one window.  The first of them came back with both slots nonzero
 * (s=5374 24678 143 14002) and put 26465 into the profile three times.
 *
 * Sized for the whole pool with headroom.  It is a ceiling, not a cost --
 * mic_collect() no longer requeues while fresh buffers remain, so the normal
 * case has one buffer in flight and exits on the first pass.
 */

#define MIC_QUIESCE_TRIES  80      /* x 50 ms = 4 s worst case */

static void mic_quiesce(struct mic_ctx_s *ctx)
{
  struct audio_status_s rst;
  struct audio_status_s pst;
  struct audio_msg_s msg;
  struct timespec deadline;
  unsigned int prio;
  ssize_t size;
  int i;

  memset(&rst, 0, sizeof(rst));
  memset(&pst, 0, sizeof(pst));

  if (!ctx->mq_open)
    {
      return;
    }

  for (i = 0; i < MIC_QUIESCE_TRIES; i++)
    {
      if (ctx->recfd >= 0)
        {
          ioctl(ctx->recfd, AUDIOIOC_GETSTATUS, (unsigned long)&rst);
        }

      if (ctx->playfd >= 0)
        {
          ioctl(ctx->playfd, AUDIOIOC_GETSTATUS, (unsigned long)&pst);
        }

      if (rst.head == rst.tail && pst.head == pst.tail)
        {
          break;
        }

      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 50 * 1000000;
      if (deadline.tv_nsec >= 1000000000)
        {
          deadline.tv_sec  += 1;
          deadline.tv_nsec -= 1000000000;
        }

      /* A timeout here is not an error -- it just means no buffer came back
       * during this slice, so re-read the counters and keep waiting.
       */

      size = mq_timedreceive(ctx->mq, (char *)&msg, sizeof(msg), &prio,
                             &deadline);
      (void)size;
    }

  micinfo("mic: quiesce rec %lu/%lu play %lu/%lu after %d\n",
         (unsigned long)rst.tail, (unsigned long)rst.head,
         (unsigned long)pst.tail, (unsigned long)pst.head, i);
}

/* Discard anything still sitting in the message queue.
 *
 * mic_quiesce() stops as soon as the counters balance, which can leave the
 * final DEQUEUE notifications unread.  Those messages carry buffer pointers,
 * and a stale one picked up by the NEXT capture would be both wrong data and
 * a double enqueue of a buffer we are about to submit ourselves.  So the queue
 * is emptied before every capture, using a deadline that has already passed so
 * mq_timedreceive returns immediately once it is dry.
 */

static void mic_flush_mq(struct mic_ctx_s *ctx)
{
  struct audio_msg_s msg;
  struct timespec deadline;
  unsigned int prio;
  int dropped = 0;

  if (!ctx->mq_open)
    {
      return;
    }

  for (; ; )
    {
      clock_gettime(CLOCK_REALTIME, &deadline);

      if (mq_timedreceive(ctx->mq, (char *)&msg, sizeof(msg), &prio,
                          &deadline) != sizeof(msg))
        {
          break;
        }

      dropped++;
    }

  if (dropped > 0)
    {
      printf("mic: flushed %d stale message(s)\n", dropped);
    }
}

/* Submit every record buffer, plus one buffer of TX silence.
 *
 * Enqueueing is what actually arms the hardware.  On the record side es8311's
 * worker only calls I2S_RECEIVE when it has a buffer, and i2s_rxdma_start()
 * only loads a DMA descriptor when one is pending; on the transmit side
 * submitting a buffer is what sets I2S_TX_START.  AUDIOIOC_START, already
 * issued once at session open, does neither.
 *
 * TX is re-primed on every capture even though the clock should still be free
 * running from the last one -- patch #16 clears I2S_TX_STOP_EN so BCLK/WS do
 * not gate off when the FIFO drains.  One 8192-byte buffer is only 256 ms of
 * audio and captures are a full second, so the clock demonstrably outlives the
 * data; the re-prime costs nothing and removes the assumption.
 */

static int mic_arm(struct mic_ctx_s *ctx)
{
  int ret;
  int i;

  for (i = 0; i < ctx->nrecbufs; i++)
    {
      ctx->recbufs[i]->nbytes  = 0;
      ctx->recbufs[i]->curbyte = 0;

      ret = mic_enqueue(ctx->recfd, ctx->recbufs[i]);
      if (ret < 0)
        {
          printf("mic: enqueue record buffer %d failed: %d\n", i, ret);
          return ret;
        }
    }

  memset(ctx->playbuf->samp, 0, ctx->playbuf->nmaxbytes);
  ctx->playbuf->nbytes  = ctx->playbuf->nmaxbytes;
  ctx->playbuf->curbyte = 0;

  ret = mic_enqueue(ctx->playfd, ctx->playbuf);
  if (ret < 0)
    {
      printf("mic: priming TX failed: %d\n", ret);
      return ret;
    }

  return 0;
}

/* Run the ADC power-up transient through and throw it away.  See
 * MIC_WARMUP_BUFS.  Failure here is not fatal: a warm-up that times out costs
 * a dirty first capture, not a broken session.
 */

static void mic_warmup(struct mic_ctx_s *ctx)
{
  struct audio_msg_s msg;
  struct timespec deadline;
  unsigned int prio;
  int discarded = 0;
  int guard = 0;

  while (discarded < MIC_WARMUP_BUFS && guard++ < MIC_WARMUP_BUFS * 4)
    {
      struct ap_buffer_s *apb;

      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 1;

      if (mq_timedreceive(ctx->mq, (char *)&msg, sizeof(msg), &prio,
                          &deadline) != sizeof(msg))
        {
          break;
        }

      if (msg.msg_id != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      apb = (struct ap_buffer_s *)msg.u.ptr;
      if (apb == NULL || apb == ctx->playbuf)
        {
          continue;
        }

      discarded++;
      apb->nbytes  = 0;
      apb->curbyte = 0;

      if (mic_enqueue(ctx->recfd, apb) < 0)
        {
          break;
        }
    }

  micinfo("mic: warmup discarded %d buffer(s)\n", discarded);
}

/* Open the devices and leave them RUNNING.  Called once per boot. */

static int mic_session_open(struct mic_ctx_s *ctx)
{
  struct ap_buffer_info_s bufinfo;
  struct ap_buffer_info_s playbufinfo;
  struct velapaw_mic_open_s *slot;
  char inpath[MIC_PATH_MAX];
  char outpath[MIC_PATH_MAX];
  int ret;
  int i;

  memset(ctx, 0, sizeof(*ctx));
  ctx->recfd  = -1;
  ctx->playfd = -1;

  /* Claim the slot on the way IN and fill it as we go, so that an open which
   * returns early -- or never returns at all -- still leaves behind everything
   * it had observed by then.  ret stays at its sentinel unless the open runs
   * to completion, which makes "how far did it get" readable without a
   * separate stage. */

  slot = &g_mic_open[g_mic_opens++ % 2];
  memset(slot, 0, sizeof(*slot));
  slot->pid        = (int)getpid();
  slot->seq        = g_mic_stage_seq;
  slot->rec_cfg    = -999;
  slot->rec_start  = -999;
  slot->play_start = -999;
  slot->ret        = -999;

  MIC_STAGE(MIC_STAGE_FIND);
  ret = mic_find_devices(inpath, outpath, sizeof(inpath));
  if (ret < 0)
    {
      return ret;
    }

  MIC_STAGE(MIC_STAGE_OPEN_REC);
  ctx->recfd = open(inpath, O_RDWR | O_CLOEXEC);
  if (ctx->recfd < 0)
    {
      printf("mic: open %s failed: %d\n", inpath, errno);
      return -errno;
    }

  MIC_STAGE(MIC_STAGE_OPEN_PLAY);
  ctx->playfd = open(outpath, O_RDWR | O_CLOEXEC);
  if (ctx->playfd < 0)
    {
      printf("mic: open %s failed: %d\n", outpath, errno);
      return -errno;
    }

  MIC_STAGE(MIC_STAGE_RSV_REC);
  if (ioctl(ctx->recfd, AUDIOIOC_RESERVE, 0) < 0)
    {
      printf("mic: reserve record failed: %d\n", errno);
      return -errno;
    }

  ctx->rec_reserved = true;

  MIC_STAGE(MIC_STAGE_RSV_PLAY);
  if (ioctl(ctx->playfd, AUDIOIOC_RESERVE, 0) < 0)
    {
      printf("mic: reserve playback failed: %d\n", errno);
      return -errno;
    }

  ctx->play_reserved = true;

  MIC_STAGE(MIC_STAGE_CFG_REC);
  ret = mic_configure(ctx->recfd, AUDIO_TYPE_INPUT);
  if (ret < 0)
    {
      return ret;
    }

  /* Must read state=1 (PREPARED).  If it reads 0 (OPEN), the CONFIGURE was a
   * silent no-op and the record node is still in ES_MODULE_DAC -- which sends
   * its buffers to I2S_SEND and starves RX exactly as observed.
   */

  MIC_STAGE(MIC_STAGE_ST_REC_CFG);
  slot->rec_cfg = mic_state(ctx->recfd);
  mic_status(ctx->recfd, "rec cfg");

  /* Configured to match the record side.  TX is only ever fed silence here,
   * but it generates the clock RX runs on, so its frame geometry has to be
   * the same or RX samples at the wrong rate.
   */

  MIC_STAGE(MIC_STAGE_CFG_PLAY);
  ret = mic_configure(ctx->playfd, AUDIO_TYPE_OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  MIC_STAGE(MIC_STAGE_ST_PLY_CFG);
  mic_status(ctx->playfd, "play cfg");

  /* Gain goes here, after BOTH configures, deliberately.
   *
   * pcm0 and pcm_in0 are two independent es8311 priv structs driving ONE
   * physical codec, and es8311_configure calls es8311_reset() on whichever
   * priv it was handed.  So configuring playback re-resets the chip and
   * would wipe a gain applied straight after the record configure.
   *
   * Disabled by default -- see MIC_GAIN_DB.
   */

  if (MIC_GAIN_DB >= 0)
    {
      MIC_STAGE(MIC_STAGE_GAIN);
      mic_set_gain(ctx->recfd, MIC_GAIN_DB);
    }

  /* GETBUFFERINFO is NOT just a query -- it is what arms allocation.
   *
   * audio.c's AUDIOIOC_GETBUFFERINFO handler copies the driver's reply into
   * upper->nbuffers as a side effect, and audio_allocbuffer() opens with
   * "if (upper->periods >= upper->nbuffers) return 0;".  Skip the query on a
   * device and its nbuffers stays 0, so the very first ALLOCBUFFER on it
   * returns 0 -- not a negative errno, literally zero, which reads like
   * success to anything checking for < 0.
   *
   * So it has to be issued on BOTH devices: the counter lives in the
   * per-device upper half, and answering for the record node does nothing for
   * the playback node.  nxlooper does the same, once per device.
   *
   * The reply is reported rather than assumed, because 4 x 8192 happens to be
   * both the es8311 driver's answer and the CONFIG_AUDIO_* fallback below --
   * identical numbers, opposite meanings.
   */

  MIC_STAGE(MIC_STAGE_BUFI_REC);
  ret = ioctl(ctx->recfd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&bufinfo);
  micinfo("mic: record GETBUFFERINFO ret=%d\n", ret);
  if (ret != OK)
    {
      bufinfo.buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
      bufinfo.nbuffers    = CONFIG_AUDIO_NUM_BUFFERS;
    }

  MIC_STAGE(MIC_STAGE_BUFI_PLAY);
  ret = ioctl(ctx->playfd, AUDIOIOC_GETBUFFERINFO,
              (unsigned long)&playbufinfo);
  micinfo("mic: playback GETBUFFERINFO ret=%d\n", ret);
  if (ret != OK)
    {
      playbufinfo.buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
      playbufinfo.nbuffers    = CONFIG_AUDIO_NUM_BUFFERS;
    }

  /* Deliberately ask for more than the driver suggested -- see MIC_WANT_BUFS.
   * This also widens the message queue below, since mq_maxmsg is derived from
   * this count, which is what a window's worth of un-drained DEQUEUEs needs.
   */

  if (bufinfo.nbuffers < MIC_WANT_BUFS)
    {
      bufinfo.nbuffers = MIC_WANT_BUFS;
    }

  if (bufinfo.nbuffers > MIC_MAX_BUFS)
    {
      bufinfo.nbuffers = MIC_MAX_BUFS;
    }

  printf("mic: %d buffers x %d B\n",
         (int)bufinfo.nbuffers, (int)bufinfo.buffer_size);

  for (i = 0; i < bufinfo.nbuffers; i++)
    {
      /* Re-stamped every iteration.  The stage does not change, but the
       * sequence number does, so a probe can tell an allocation that is
       * grinding through 18 buffers from one that is wedged on a single
       * ALLOCBUFFER -- which the stage alone cannot.
       */

      MIC_STAGE(MIC_STAGE_ALLOC_REC);
      ret = mic_alloc_buffer(ctx->recfd, &ctx->recbufs[i],
                             bufinfo.buffer_size);
      if (ret < 0)
        {
          /* Asking past the driver's suggested depth is speculative, so a
           * refusal partway through is a legitimate answer rather than a
           * failure -- carry on with what was granted.  Below the suggested
           * depth it is a real error and still aborts.
           */

          if (i >= MIC_MIN_BUFS)
            {
              printf("mic: pool capped at %d buffers (wanted %d, ret %d)\n",
                     i, (int)bufinfo.nbuffers, ret);
              break;
            }

          return ret;
        }

      ctx->nrecbufs = i + 1;
    }

  MIC_STAGE(MIC_STAGE_ALLOC_PLAY);
  ret = mic_alloc_buffer(ctx->playfd, &ctx->playbuf, playbufinfo.buffer_size);
  if (ret < 0)
    {
      return ret;
    }

  snprintf(ctx->mqname, sizeof(ctx->mqname), "/vpmic%d", (int)getpid());

  MIC_STAGE(MIC_STAGE_MQ_OPEN);

  {
    struct mq_attr attr;

    attr.mq_maxmsg  = bufinfo.nbuffers + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags   = 0;

    ctx->mq = mq_open(ctx->mqname, O_RDWR | O_CREAT, 0644, &attr);
  }

  if (ctx->mq == (mqd_t)-1)
    {
      printf("mic: mq_open failed: %d\n", errno);
      return -errno;
    }

  ctx->mq_open = true;

  MIC_STAGE(MIC_STAGE_REG_MQ);
  if (ioctl(ctx->recfd, AUDIOIOC_REGISTERMQ, (unsigned long)ctx->mq) < 0 ||
      ioctl(ctx->playfd, AUDIOIOC_REGISTERMQ, (unsigned long)ctx->mq) < 0)
    {
      printf("mic: REGISTERMQ failed: %d\n", errno);
      return -errno;
    }

  /* Buffers must be in the driver's hands BEFORE START on this codec: START
   * is what spawns es8311's worker thread, and the worker looks for work in
   * pendq as soon as it runs.  This is the order nxlooper uses and the order
   * every working capture on this board has used.
   */

  MIC_STAGE(MIC_STAGE_ARM);
  ret = mic_arm(ctx);
  if (ret < 0)
    {
      return ret;
    }

  /* head must equal nrecbufs here.  That is what proves the buffers got past
   * audio_enqueuebuffer() into es8311's pendq.
   */

  MIC_STAGE(MIC_STAGE_ST_ENQ);
  mic_status(ctx->recfd, "rec enq");
  mic_status(ctx->playfd, "play enq");

  /* Build 81: TX STARTS FIRST.  This reverts build 80.
   *
   *   "TX starts FIRST.  Without a running TX, RX never sees a bit clock
   *    and the dequeue loop blocks until the timeout with zero buffers
   *    returned -- RX borrows BCLK/WS from TX in full-duplex mode."
   *
   * b80 started RX 200 us early to test whether RX had to be listening
   * before the first clock edge landed.  It did not help: b80 reproduced
   * b78's and b79's signature exactly -- isr 0 / eof 0 / start 2,
   * desc 80000ff0, intraw 000000a0 -- which is what the slave-framing
   * argument predicted, since a slave receiver frames on every WS edge
   * and so has no one-shot sync to miss.
   *
   * Reverted because b80's order is not merely unhelpful, it is UNPROVEN.
   * No b80 boot ever captured audio, so that order has never been shown
   * safe for the path that works.  This is the order every successful
   * capture on this board has used.  Do not reintroduce b80's.
   */

  MIC_STAGE(MIC_STAGE_START_PLAY);
  if (ioctl(ctx->playfd, AUDIOIOC_START, 0) < 0)
    {
      printf("mic: start playback failed: %d\n", errno);
      return -errno;
    }

  ctx->play_started = true;

  MIC_STAGE(MIC_STAGE_ST_PLY_STA);
  slot->play_start = mic_state(ctx->playfd);
  mic_status(ctx->playfd, "play start");

  /* Must read state=5 (RUNNING).  A state that is neither PREPARED(1) nor
   * XRUN(3) here means audio_start() took its silent fall-through and
   * es8311_start() was never called -- no worker thread, so nothing ever
   * drains pendq into I2S_RECEIVE, which is what "act=0 pend=0" reports.
   */

  MIC_STAGE(MIC_STAGE_START_REC);
  if (ioctl(ctx->recfd, AUDIOIOC_START, 0) < 0)
    {
      printf("mic: start record failed: %d\n", errno);
      return -errno;
    }

  ctx->rec_started = true;

  MIC_STAGE(MIC_STAGE_ST_REC_STA);
  slot->rec_start = mic_state(ctx->recfd);
  slot->nbufs     = ctx->nrecbufs;
  mic_status(ctx->recfd, "rec start");

  ctx->open  = true;
  ctx->owner = getpid();

  MIC_STAGE(MIC_STAGE_WARMUP);
  mic_warmup(ctx);

  slot->ret = 0;

  MIC_STAGE(MIC_STAGE_OPENED);
  return 0;
}

/* Full teardown.  Only on shutdown, or to recover from a failed capture --
 * NOT between successful captures, which is the bug this file exists to fix.
 */

static void mic_session_close(struct mic_ctx_s *ctx)
{
  struct audio_buf_desc_s desc;
  int i;

  /* Build 79: sub-staged.  This whole function used to report as stage 32,
   * and it is the function the failing path hangs in, so "stage 32" named
   * eleven different ioctls at once.  Now the stage names the call.
   */

  MIC_STAGE(MIC_STAGE_CL_STOP);

  if (ctx->rec_started)
    {
      ioctl(ctx->recfd, AUDIOIOC_STOP, 0);
    }

  if (ctx->play_started)
    {
      ioctl(ctx->playfd, AUDIOIOC_STOP, 0);
    }

  /* Must run after STOP and before FREEBUFFER: freeing a buffer the driver is
   * still holding leaves it pointing at released memory.
   */

  MIC_STAGE(MIC_STAGE_CL_QUIESCE);
  mic_quiesce(ctx);

  MIC_STAGE(MIC_STAGE_CL_UNREG);

  if (ctx->mq_open)
    {
      if (ctx->recfd >= 0)
        {
          ioctl(ctx->recfd, AUDIOIOC_UNREGISTERMQ, (unsigned long)ctx->mq);
        }

      if (ctx->playfd >= 0)
        {
          ioctl(ctx->playfd, AUDIOIOC_UNREGISTERMQ, (unsigned long)ctx->mq);
        }
    }

  MIC_STAGE(MIC_STAGE_CL_FREE);

  for (i = 0; i < ctx->nrecbufs; i++)
    {
      if (ctx->recbufs[i] != NULL)
        {
          memset(&desc, 0, sizeof(desc));
          desc.u.buffer = ctx->recbufs[i];
          ioctl(ctx->recfd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          ctx->recbufs[i] = NULL;
        }
    }

  if (ctx->playbuf != NULL)
    {
      memset(&desc, 0, sizeof(desc));
      desc.u.buffer = ctx->playbuf;
      ioctl(ctx->playfd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
      ctx->playbuf = NULL;
    }

  MIC_STAGE(MIC_STAGE_CL_RELEASE);

  if (ctx->rec_reserved)
    {
      ioctl(ctx->recfd, AUDIOIOC_RELEASE, 0);
    }

  if (ctx->play_reserved)
    {
      ioctl(ctx->playfd, AUDIOIOC_RELEASE, 0);
    }

  MIC_STAGE(MIC_STAGE_CL_MQ);

  if (ctx->mq_open)
    {
      mq_close(ctx->mq);
      mq_unlink(ctx->mqname);
    }

  MIC_STAGE(MIC_STAGE_CL_FDS);

  if (ctx->recfd >= 0)
    {
      close(ctx->recfd);
    }

  if (ctx->playfd >= 0)
    {
      close(ctx->playfd);
    }

  memset(ctx, 0, sizeof(*ctx));
  ctx->recfd  = -1;
  ctx->playfd = -1;
}

/* Which int16 of each pair carries the microphone -- 0 or 1.
 *
 * One slot of the two is always dead (see MIC_STRIDE), but WHICH one is not
 * fixed, and this is the single most important thing in this file to not
 * "simplify" later.  It does not merely vary between builds -- build 52 saw it
 * flip BETWEEN CAPTURES INSIDE ONE BOOT:
 *
 *   [1/3] live slot 0 of 2   ->  yes 0.996
 *   [2/3] live slot 0 of 2   ->  no  0.750
 *   [3/3] live slot 1 of 2   ->  silence, correctly rejected
 *
 * Every capture re-arms RX, and the first sample lands on whichever slot the
 * frame happens to be at when it does.  Hardcoding slot 0 -- the obvious
 * reading of the first two captures -- would have made capture 3 come back all
 * zeros and look exactly like a dead microphone.
 *
 * So it is measured, per capture, off the first buffer that carries anything.
 *
 * Exact zeros are the discriminator, not level.  The dead slot is zero to the
 * bit, while the live slot carries an ADC noise floor even in a silent room --
 * a silent build-50 capture read -1, -1, -2, 1, 5, -1, 57, -16.  A ceiling on
 * amplitude would confuse the two during silence; counting exact zeros does
 * not.  Ties go to slot 0, which is where es8311_start() puts the ADC
 * (GPIO_REG44 = 0x50, ADC left / DAC right).
 */

static int mic_data_phase(const int16_t *src, size_t nsrc)
{
  size_t live[2] =
    {
      0, 0
    };

  size_t i;

  for (i = 0; i < nsrc; i++)
    {
      if (src[i] != 0)
        {
          live[i & 1]++;
        }
    }

  /* An all-zero buffer names no slot.  RX delivers a few of these while it
   * warms up -- build 51's first capture opened with three of them -- and
   * deciding on one would be a coin flip dressed up as a measurement.  The
   * caller keeps asking until a buffer has something in it; until then the
   * data is zeros whichever slot it is read from, so nothing is lost.
   */

  if (live[0] == 0 && live[1] == 0)
    {
      return -1;
    }

  return live[0] >= live[1] ? 0 : 1;
}

/* Collect `want` bytes of audio.  Returns bytes captured, or a negative
 * errno on timeout.
 *
 * `want` counts OUTPUT bytes, i.e. real mono samples.  Because of MIC_STRIDE
 * this consumes twice that much from the driver.
 */

static ssize_t mic_collect(struct mic_ctx_s *ctx, uint8_t *out, size_t want,
                           unsigned int timeout_ms)
{
  struct audio_msg_s msg;
  struct timespec deadline;
  size_t got = 0;
  int skipped = 0;
  int phase = -1;
  int seq = 0;
  unsigned int prio;
  ssize_t size;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec  += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
  if (deadline.tv_nsec >= 1000000000)
    {
      deadline.tv_sec  += 1;
      deadline.tv_nsec -= 1000000000;
    }

  while (got < want)
    {
      size = mq_timedreceive(ctx->mq, (char *)&msg, sizeof(msg), &prio,
                             &deadline);
      if (size != sizeof(msg))
        {
          /* tail counts completions.  Comparing these against head separates
           * "RX never started" from "RX ran and returned empty buffers".
           *
           * Captured to memory as well as printed.  The printf is the older
           * half and is no longer the reliable one: the UI puts fd 1 in
           * O_NONBLOCK and a full TX buffer drops the write outright, which is
           * how build 66 managed to time out without leaving a single line
           * behind.  A struct written here can be read from the prompt later
           * by any task, and survives the teardown below hanging.
           */

          {
            struct audio_status_s rst;
            struct audio_status_s pst;

            memset(&rst, 0, sizeof(rst));
            memset(&pst, 0, sizeof(pst));

            if (ctx->recfd >= 0)
              {
                ioctl(ctx->recfd, AUDIOIOC_GETSTATUS, (unsigned long)&rst);
              }

            if (ctx->playfd >= 0)
              {
                ioctl(ctx->playfd, AUDIOIOC_GETSTATUS, (unsigned long)&pst);
              }

            g_mic_fail.got   = (unsigned long)got;
            g_mic_fail.want  = (unsigned long)want;
            g_mic_fail.rhead = (unsigned long)rst.head;
            g_mic_fail.rtail = (unsigned long)rst.tail;
            g_mic_fail.phead = (unsigned long)pst.head;
            g_mic_fail.ptail = (unsigned long)pst.tail;
            g_mic_fail.seq   = seq;
            g_mic_fail.phase = phase;
          }

          printf("mic: timed out with %u/%u bytes\n",
                 (unsigned)got, (unsigned)want);

          mic_status(ctx->recfd, "rec t/out");
          mic_status(ctx->playfd, "play t/out");

          return -ETIMEDOUT;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE)
        {
          struct ap_buffer_s *apb = (struct ap_buffer_s *)msg.u.ptr;
          size_t copy;

          if (apb == NULL)
            {
              continue;
            }

          /* A playback buffer coming back is the primed silence completing.
           * Do not treat it as captured audio -- and do not requeue it, so
           * TX stays quiet while RX keeps the clock it already has.
           */

          if (apb == ctx->playbuf)
            {
              continue;
            }

          /* Throw away the start-of-stream transient.  See MIC_SKIP_BUFS. */

          if (skipped < MIC_SKIP_BUFS)
            {
              skipped++;
              apb->nbytes  = 0;
              apb->curbyte = 0;

              if (mic_enqueue(ctx->recfd, apb) < 0)
                {
                  break;
                }

              continue;
            }

#if MIC_TRACE
          /* One line per accepted buffer, BEFORE the copy.
           *
           * This started as a way to tell two causes of a periodic capture
           * apart -- a buffer redelivered without a refill, versus descriptors
           * sharing one region -- and it settled that question in build 49 by
           * showing the same four pointers coming round twice with identical
           * samples.  See MIC_WANT_BUFS.
           *
           * It earns its keep now as the check on both fixes at once, which is
           * why it prints four samples rather than two:
           *
           *   apb  -- must be distinct for every seq in a window.  A repeat
           *           means the pool is too small again and half the capture
           *           is a stale cached copy of the other half.
           *   s    -- must alternate live, 0, live, 0.  Which of the pair is
           *           live can flip between builds; that is expected and is
           *           what mic_data_phase() measures.  What must NOT happen is
           *           both being nonzero (the dead slot is carrying playback
           *           feedback) or all four zero (RX is delivering nothing).
           *
           * Set MIC_TRACE to 0 once a build has confirmed both.
           */

          {
            const int16_t *s16 = (const int16_t *)apb->samp;

            printf("mic: trace seq=%d apb=%p nbytes=%u "
                   "s=%d %d %d %d\n",
                   seq, apb, (unsigned)apb->nbytes,
                   (int)s16[0], (int)s16[1], (int)s16[2], (int)s16[3]);
          }
#endif

          seq++;

          /* De-interleave into the caller's buffer: one int16 in MIC_STRIDE,
           * starting at the live slot.  The phase is settled once per capture,
           * off the first buffer that actually carries something, and held for
           * the rest of it -- a buffer is an even number of int16 (4092 B =
           * 2046), so the phase cannot drift across a buffer boundary.
           */

          {
            const int16_t *src = (const int16_t *)apb->samp;
            size_t nsrc = apb->nbytes / sizeof(int16_t);
            int16_t *dst = (int16_t *)(out + got);
            size_t room = (want - got) / sizeof(int16_t);
            size_t start;
            size_t n = 0;
            size_t i;

            if (phase < 0)
              {
                phase = mic_data_phase(src, nsrc);
                if (phase >= 0)
                  {
                    printf("mic: live slot %d of %d (seq %d)\n",
                           phase, MIC_STRIDE, seq - 1);
                  }
              }

            /* Slot 0 until a buffer with content settles it -- see above,
             * the choice cannot matter while every sample is zero.
             */

            start = phase < 0 ? 0 : (size_t)phase;

            for (i = start; i < nsrc && n < room; i += MIC_STRIDE)
              {
                dst[n++] = src[i];
              }

            copy = n * sizeof(int16_t);
          }

          got += copy;

          /* Requeue only when the pool is about to run dry.
           *
           * The pool is now deep enough to cover a whole window outright
           * (MIC_WANT_BUFS), so handing buffers back is not just unnecessary,
           * it is actively harmful twice over: a recycled buffer is the stale-
           * cache bug, and the backlog it leaves in flight is what overran the
           * quiesce budget in build 51 and left the NEXT capture with six
           * buffers to work with.
           *
           * The guard is self-adjusting rather than a constant, so a capped
           * pool still completes -- it just falls back to the old recycling
           * behaviour, with the old caveats, instead of coming up short.
           *
           * Everything still in flight comes home in mic_quiesce(), and
           * leaving the record queue empty is what lets RX idle harmlessly
           * between captures instead of filling the message queue with audio
           * nobody is reading.
           */

          if (got < want && skipped + seq >= ctx->nrecbufs - 1)
            {
              apb->nbytes  = 0;
              apb->curbyte = 0;

              if (mic_enqueue(ctx->recfd, apb) < 0)
                {
                  break;
                }
            }
        }
      else if (msg.msg_id == AUDIO_MSG_STOP ||
               msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          break;
        }
    }

  return (ssize_t)got;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Build 81.  Set by velapaw_mic_preopen(), cleared by the first capture that
 * consumes the session.
 *
 * mic_session_open() leaves buffers ARMED and in flight -- which is why the
 * inline-open path below goes straight to collect while the already-open
 * path does flush + arm.  Without this flag a pre-opened session would take
 * the second path and enqueue on top of buffers the driver still holds.
 */

static volatile bool g_mic_fresh;

/****************************************************************************
 * Name: velapaw_mic_preopen
 *
 * Description:
 *   Open and start the capture session AHEAD of the first capture, so that
 *   its cost (measured at 5350 ms on this board) is paid at boot instead of
 *   inside the caller's timeout budget.
 *
 *   Optional.  If this is never called, or fails, velapaw_mic_capture()
 *   opens the session inline exactly as it always has -- so no caller has
 *   to handle the error.  It is reported only so a probe can see it.
 *
 *   MUST be called from the thread that will capture.  The session records
 *   its owning pid, and a mismatch sends the next capture down the
 *   drop-and-reopen path, which is the -110 reopen bug.
 *
 ****************************************************************************/

int velapaw_mic_preopen(void)
{
  int ret;

  if (g_mic_busy)
    {
      return -EBUSY;
    }

  if (g_mic.open && g_mic.owner == getpid())
    {
      return 0;
    }

  g_mic_busy     = true;
  g_mic_busy_pid = getpid();

  if (g_mic.open && g_mic.owner != getpid())
    {
      printf("mic: session belonged to pid %d, reopening\n",
             (int)g_mic.owner);
      memset(&g_mic, 0, sizeof(g_mic));
      g_mic.recfd  = -1;
      g_mic.playfd = -1;
    }

  MIC_STAGE(MIC_STAGE_ENTER);

  ret = mic_session_open(&g_mic);
  if (ret < 0)
    {
      printf("mic: pre-open failed (%d); the first capture will retry "
             "inline\n", ret);
      mic_session_close(&g_mic);
    }
  else
    {
      g_mic_fresh = true;
      printf("mic: session pre-opened\n");
    }

  MIC_STAGE(MIC_STAGE_DONE);
  g_mic_busy = false;
  return ret;
}

int velapaw_mic_capture(int16_t *dest, size_t nsamples,
                        unsigned int timeout_ms)
{
  ssize_t got;
  size_t want;
  int ret;

  if (dest == NULL || nsamples == 0)
    {
      return -EINVAL;
    }

  /* One capture at a time, across the whole system.
   *
   * mic.h has always said "single-threaded by contract", and until build 60
   * that contract held by luck: only one thing captured.  It does not hold
   * now.  The UI's voice worker is blocked somewhere inside this function,
   * and diagnosing that means running `velapaw kws mic` from the console
   * while it is -- at which point the second caller sees a session it does
   * not own, memsets g_mic, and rips the descriptors and buffer pointers out
   * from under the thread still using them.  That destroys the evidence and
   * would eventually crash.
   *
   * So a second caller is refused rather than served.  -EBUSY here is also a
   * useful answer in its own right: it says "the worker really is inside",
   * which is the fact under investigation.
   *
   * Not a mutex.  A mutex would make the console command WAIT on the very
   * block being diagnosed, which is the opposite of what a diagnostic should
   * do; and the flag is written by one thread while any competing writer is,
   * by definition, blocked out.
   */

  if (g_mic_busy)
    {
      printf("mic: capture already in progress (pid %d, stage %d %s)\n",
             (int)g_mic_busy_pid, g_mic_stage,
             velapaw_mic_stage_name(g_mic_stage));
      return -EBUSY;
    }

  g_mic_busy     = true;
  g_mic_busy_pid = getpid();

  MIC_STAGE(MIC_STAGE_ENTER);

  want = nsamples * sizeof(int16_t);

  /* A session left behind by a task that has since exited.
   *
   * This is a flat build, so g_mic lives in one shared .data for the whole
   * system and survives the task that filled it in -- but file descriptors do
   * not.  NuttX closes them at task exit, so recfd/playfd are now either
   * invalid or, worse, reused by somebody else.  Build 43 hit exactly this:
   * the second `velapaw kws mic` of a boot found open == true and got -EBADF
   * from the first enqueue.
   *
   * Drop the record without touching those descriptors -- closing a number we
   * no longer own would be closing whatever now holds it -- and open fresh.
   */

  if (g_mic.open && g_mic.owner != getpid())
    {
      printf("mic: session belonged to pid %d, reopening\n", (int)g_mic.owner);
      memset(&g_mic, 0, sizeof(g_mic));
      g_mic.recfd  = -1;
      g_mic.playfd = -1;
    }

  if (!g_mic.open)
    {
      ret = mic_session_open(&g_mic);
      if (ret < 0)
        {
          mic_session_close(&g_mic);
          MIC_STAGE(MIC_STAGE_DONE);
          g_mic_busy = false;
          return ret;
        }

      g_mic_fresh = false;
    }
  else if (g_mic_fresh)
    {
      /* Build 81: pre-opened by velapaw_mic_preopen() and not yet captured
       * from.  mic_session_open() already armed the buffers and they are
       * still in flight, so this must behave like the branch above --
       * collect directly, do NOT flush and re-arm.  Doing so would enqueue
       * on top of buffers the driver still holds.
       */

      g_mic_fresh = false;

      MIC_STAGE(MIC_STAGE_ST_REARM);
      mic_status(g_mic.recfd, "rec fresh");
    }
  else
    {
      /* Second and later captures: the devices are still open, configured and
       * RUNNING, so all that is needed is fresh buffers.
       */

      MIC_STAGE(MIC_STAGE_FLUSH);
      mic_flush_mq(&g_mic);

      MIC_STAGE(MIC_STAGE_REARM);
      ret = mic_arm(&g_mic);
      if (ret < 0)
        {
          goto failed;
        }

      MIC_STAGE(MIC_STAGE_ST_REARM);
      mic_status(g_mic.recfd, "rec rearm");
    }

  MIC_STAGE(MIC_STAGE_COLLECT);
  got = mic_collect(&g_mic, (uint8_t *)dest, want, timeout_ms);

  /* Bring the stragglers home either way -- on the failure path too, so that
   * the teardown below does not free buffers the driver still holds.
   */

  MIC_STAGE(MIC_STAGE_QUIESCE);
  mic_quiesce(&g_mic);

  /* Build 80: the retry moved OUT of this function.
   *
   * b79 retried here and hung at stage 37 (kick-collect), identically on a
   * cold cycle and a warm reset, both at seq 48.  Two separate faults:
   *
   *   a) velapaw_i2s_rx_kick() re-arms the container it rescued, so the
   *      driver still owns that buffer.  mic_arm() then enqueued all 18
   *      recbufs on top of it -- two driver containers pointing at one apb,
   *      and the mq accounting stops adding up.
   *
   *   b) dropping mic_arm() would not have been enough on its own.  The
   *      kick re-arms exactly the rescued container: one buffer, 4096
   *      bytes, 2048 samples, against a 32000-sample window.  A retry with
   *      no re-arm starves at 6% of a window and times out anyway.
   *
   * So the retry belongs one level up, where the worker already sleeps
   * 200 ms and calls back in -- and that second call takes this function's
   * `else` branch, which flushes the mq and re-arms all 18 buffers by the
   * normal, proven path with no kick interaction at all.
   */

  if (got < 0)
    {
      ret = (int)got;
      goto failed;
    }

  if (g_mic_kick_armed)
    {
      g_mic_kick_armed = false;
      g_mic_kick_ok++;
      printf("mic: recovered after a kick\n");
    }

  ret = (int)(got / (ssize_t)sizeof(int16_t));

  MIC_STAGE(MIC_STAGE_DCBLOCK);
  mic_dcblock(dest, (size_t)ret);

  MIC_STAGE(MIC_STAGE_DONE);
  g_mic_busy = false;
  return ret;

failed:

  /* Publish the errno BEFORE the teardown, because the teardown is where this
   * path has twice failed to come back from -- see velapaw_mic_last_error() in
   * mic.h.  A value written here survives a hang in mic_session_close(); a
   * return value does not, and neither does the printf below. */

  g_mic_last_error = ret;

  /* Build 80: kick, then RETURN.  Do not close the session.
   *
   * The old comment here said a failed capture leaves a state we cannot
   * reason about and a full rebuild is the only recovery.  That was a fair
   * reading at the time, but mic_session_close() is the function this path
   * hung in on every run before b79, so "the only recovery there is" was
   * in practice "the thread dies here".  Leaving the session open is
   * strictly better than not returning.
   *
   * The kick still earns its place even though it recovers nothing by
   * itself: it empties rx.act, and while rx.act is non-empty every
   * subsequent i2s_rxdma_start() bails at its !sq_empty() test -- so
   * without this the NEXT capture could not arm either, and one stall
   * would still be permanent.
   */

  g_mic_kicks++;
  g_mic_kick_armed = true;

  printf("mic: capture failed (%d), kicking RX and holding the session\n",
         ret);

  MIC_STAGE(MIC_STAGE_KICK);
  if (velapaw_i2s_rx_kick != NULL)
    {
      velapaw_i2s_rx_kick();
    }

  MIC_STAGE(MIC_STAGE_KICK_FLSH);
  mic_flush_mq(&g_mic);

  MIC_STAGE(MIC_STAGE_DONE);
  g_mic_busy = false;
  return ret;
}

int velapaw_mic_stage(void)
{
  return g_mic_stage;
}

unsigned velapaw_mic_stage_seq(void)
{
  return g_mic_stage_seq;
}

int velapaw_mic_last_error(void)
{
  return g_mic_last_error;
}

void velapaw_mic_last_fail(struct velapaw_mic_fail_s *out)
{
  if (out != NULL)
    {
      *out = g_mic_fail;
    }
}

int velapaw_mic_open_log(struct velapaw_mic_open_s *out, int max)
{
  unsigned have;
  unsigned first;
  unsigned i;
  int n = 0;

  if (out == NULL || max <= 0)
    {
      return 0;
    }

  have  = g_mic_opens < 2 ? g_mic_opens : 2;
  first = g_mic_opens - have;

  for (i = first; i < g_mic_opens && n < max; i++)
    {
      out[n++] = g_mic_open[i % 2];
    }

  return n;
}

const char *velapaw_mic_stage_name(int stage)
{
  if (stage < 0 ||
      (unsigned)stage >= sizeof(g_mic_stage_names) /
                         sizeof(g_mic_stage_names[0]))
    {
      return "?";
    }

  return g_mic_stage_names[stage];
}

void velapaw_mic_shutdown(void)
{
  /* Same reason as the guard in velapaw_mic_capture, and the more urgent half
   * of it: `velapaw kws mic` calls this on its way out even when its capture
   * failed, so without this a -EBUSY refusal would be immediately followed by
   * a teardown of the session the refusal was protecting.
   */

  if (g_mic_busy)
    {
      printf("mic: capture in progress (pid %d), not shutting down\n",
             (int)g_mic_busy_pid);
      return;
    }

  if (!g_mic.open)
    {
      return;
    }

  /* Same descriptor-ownership rule as in velapaw_mic_capture: only the task
   * that opened these fds may close them.
   */

  if (g_mic.owner != getpid())
    {
      memset(&g_mic, 0, sizeof(g_mic));
      g_mic.recfd  = -1;
      g_mic.playfd = -1;
      return;
    }

  mic_session_close(&g_mic);
}

/* Subtract the mean.  Build 41 measured dc = -8585 on a 32767 scale -- a
 * quarter of full range spent on a constant offset, which both eats headroom
 * on one side (asymmetric clipping) and shows up as a large DC bin the
 * feature front end never expects.  Cheap to fix here and it keeps the driver
 * untouched.
 */

static void mic_dcblock(int16_t *audio, size_t nsamples)
{
  long sum = 0;
  long dc;
  size_t i;

  if (nsamples == 0)
    {
      return;
    }

  for (i = 0; i < nsamples; i++)
    {
      sum += audio[i];
    }

  dc = sum / (long)nsamples;
  if (dc == 0)
    {
      return;
    }

  for (i = 0; i < nsamples; i++)
    {
      long v = (long)audio[i] - dc;

      audio[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }

  micinfo("mic: removed dc %ld\n", dc);
}

/* Integer square root, so this costs no libm and no float formatting. */

static unsigned long mic_isqrt(unsigned long long v)
{
  unsigned long long rem = 0;
  unsigned long long root = 0;
  int i;

  for (i = 0; i < 32; i++)
    {
      root <<= 1;
      rem = (rem << 2) | (v >> 62);
      v <<= 2;

      if (root < rem)
        {
          rem -= root | 1;
          root += 2;
        }
    }

  return (unsigned long)(root >> 1);
}

/* Report where the energy actually is.
 *
 * A single peak number cannot tell a spoken word from a click, and that
 * ambiguity cost several builds: the driver probe only fires on 2 of the 11
 * buffers in a capture, and build 44 showed a full-scale sample at index 127
 * of the first buffer with near silence in the others -- yet the assembled
 * second still reported peak 32767.
 *
 * So: RMS distinguishes loud-overall from one-loud-sample, the rail count
 * says whether anything is actually pinned at full scale, and the eight-way
 * profile says WHERE.  Speech spreads across several segments; a start-of-
 * stream transient puts everything in segment 0.
 */

void velapaw_mic_stats(const int16_t *audio, size_t nsamples)
{
  unsigned long long sq = 0;
  long sum = 0;
  int peak = 0;
  size_t peakidx = 0;
  int rail = 0;
  int seg[8];
  size_t seglen;
  size_t i;
  int s;

  if (nsamples == 0)
    {
      printf("mic: no samples\n");
      return;
    }

  memset(seg, 0, sizeof(seg));
  seglen = (nsamples + 7) / 8;

  for (i = 0; i < nsamples; i++)
    {
      int v = audio[i];
      int a = v < 0 ? -v : v;

      if (a > peak)
        {
          peak    = a;
          peakidx = i;
        }

      if (a >= 32700)
        {
          rail++;
        }

      s = (int)(i / seglen);
      if (s < 8 && a > seg[s])
        {
          seg[s] = a;
        }

      sq  += (unsigned long long)((long)v * v);
      sum += v;
    }

  printf("mic: %u samples, peak %d@%u, rms %lu, dc %ld, rail %d\n",
         (unsigned)nsamples, peak, (unsigned)peakidx,
         mic_isqrt(sq / nsamples), sum / (long)nsamples, rail);

  printf("mic: profile");
  for (s = 0; s < 8; s++)
    {
      printf(" %5d", seg[s]);
    }

  printf("\n");
}

/* Is there sustained sound in this window, or only silence and artifacts?
 *
 * This exists because micro_speech has no opinion about it.  Handed a window
 * of near-zero samples it does not answer "silence" -- the log-mel features
 * are then dominated by quantisation noise and the classifier returns a
 * confident nonsense label.  Build 47, two captures of an empty room:
 *
 *   rms  6  profile     1    1    1    3    4   13   25   71  -> yes 0.602
 *   rms 23  profile   232   80   83   70   67   93   66  104  -> no  0.496
 *
 * A feeder that listens continuously would act on those.
 *
 * Peak is useless as a gate: the start-of-stream transient is a single
 * full-scale sample.  RMS is not much better -- one 25220 impulse in a 16000
 * sample window is rms 199 on its own, which is above anything a silence
 * threshold could sensibly reject without also rejecting quiet speech.
 *
 * What separates them is DURATION.  Speech puts energy in most of the window;
 * an impulse puts it in one segment.  So: count the eighths that carry real
 * level and require several of them.  Checked against every capture on record,
 * including the two above, a deliberate silence, and a pure transient
 * (32701 67 67 67 67 67 68 68 -> one segment, correctly rejected).
 *
 * Thresholds are deliberately far from both populations: real speech at
 * MIC_GAIN_DB 30 runs 2500-6900 per segment and the loudest silence seen is
 * 232, so MIC_SPEECH_FLOOR sits between them by more than a factor of 10 in
 * one direction and about 4 in the other.
 */

/* SEGS was 3 until build 55, when the 14-label model put short words in the
 * vocabulary and hardware capture showed 3 rejecting real speech:
 *
 *   profile   81   75   48   61  1549 1254  108   64  -> 2 segments, DROPPED
 *
 * That is a word spoken across the middle two eighths, 250 ms, which is what a
 * digit sounds like.  "yes" and "no" are long enough to light most of the
 * window (the same session: 4189 2164 1409 1211 1251 714 1217 2253), so the
 * old value was never tested against anything shorter.
 *
 * Two is still clear of every false-positive population on record -- the two
 * build 47 empty rooms score 0 and 1 segments, a deliberate silence 0, and a
 * pure start-of-stream transient 1 -- so nothing that was rejected before is
 * accepted now.  And the consequence of letting a marginal window through is
 * no longer what it was in build 47: it is one 100 ms classification whose
 * result still has to clear a per-step probability threshold.  That gate did
 * not exist when this constant was chosen.
 *
 * Do not lower it to 1.  One segment is exactly the signature of the transient.
 */

#define MIC_SPEECH_FLOOR 200   /* a segment counts if its peak reaches this */
#define MIC_SPEECH_SEGS  2     /* and this many segments must count         */

bool velapaw_mic_speech_present(const int16_t *audio, size_t nsamples)
{
  int seg[8];
  size_t seglen;
  size_t i;
  int loud = 0;
  int s;

  if (audio == NULL || nsamples < 8)
    {
      return false;
    }

  memset(seg, 0, sizeof(seg));
  seglen = (nsamples + 7) / 8;

  for (i = 0; i < nsamples; i++)
    {
      int v = audio[i];
      int a = v < 0 ? -v : v;

      s = (int)(i / seglen);
      if (s < 8 && a > seg[s])
        {
          seg[s] = a;
        }
    }

  for (s = 0; s < 8; s++)
    {
      if (seg[s] >= MIC_SPEECH_FLOOR)
        {
          loud++;
        }
    }

  return loud >= MIC_SPEECH_SEGS;
}
