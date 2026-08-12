/****************************************************************************
 * VelaPaw - interactive LVGL UI (tabbed: Enroll/Recognize/My Pets/Trends)
 *
 * HARDWARE layout: the real panel is landscape 480x320 (ST7796). The UI fills
 * the whole screen: a slim header (breadcrumb + bell) over a 4-tab view whose
 * pages scroll vertically when content is taller than the short screen.
 *   Enroll    - live camera, capture 5 samples, name + portion, Save Pet
 *   Recognize - live recognition, feed the matched pet, cooldown
 *   My Pets   - list of enrolled pets (name, portion, fed-today, BCS, sparkline)
 *   Trends    - per-pet 7/30-day intake chart
 *
 * (Originally authored for the 1280x800 emulator as a 600px portrait column;
 * re-laid-out for 480x320. All recognition/feeding/store logic is unchanged -
 * only geometry and font sizes differ.)
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <lvgl/lvgl.h>

#include "velapaw.h"
#include "hal/camera.h"
#include "hal/feeder.h"
#include "infer/infer.h"
#include "identity/identity.h"
#include "store/store.h"
#include "ui/ui.h"
#include "ui/ui_i18n.h"

#ifdef CONFIG_VELAPAW_VOICE
#include "voice/kws.h"
#include "voice/mic.h"
#endif

#ifdef CONFIG_VELAPAW_INFER_TFLM
#include "velapaw_assets.h"
#define NUM_VIEWS VELAPAW_NUM_PETS
#else
#define NUM_VIEWS 2
#endif

#define UI_DEMO_COOLDOWN_S 10
#define MAX_SHOTS          5
#define IMG_SCALE_X        360    /* 256=100%; 360 -> 128px becomes ~180px WIDE */
#define IMG_SCALE_Y        300    /* 300 -> 128px becomes ~150px tall (1.2:1, less squish) */
#define FRAME_INTERVAL_MS  150    /* live camera preview refresh cadence */
/* Idle policy. Streaming the camera forever into an empty room is both what
 * preceded the wedge AND wrong for an appliance, so:
 *   Enroll    - a human must be present to enroll, so stop the preview after
 *               ENROLL_IDLE_MS with no touch; a tap resumes it.
 *   Recognize - CANNOT be touch-gated: the whole point is that a cat walks up
 *               and is fed with nobody at the screen. So instead we just slow
 *               the preview down while no pet is present (a cat lingers at the
 *               bowl for seconds, so 2 fps still catches it), and go back to
 *               full rate once one is on camera. ~4x fewer idle captures. */
#define FRAME_IDLE_MS      500    /* Recognize: preview rate while no pet seen */
#define ENROLL_IDLE_MS     30000  /* Enroll: pause preview after 30s untouched */
#define RECOG_INTERVAL_MS  1200
#define RECOG_STABLE_N     1      /* consecutive same-ID frames before acting */
#define BCS_VOTES          5      /* frames majority-voted per visit for robust BCS */

#define SCR_W              480    /* real panel: landscape 480x320 */
#define SCR_H              320
#define COL_W              460    /* usable content width (screen minus margins) */

/* Font hierarchy for the small screen. These route through velapaw_font() so a
 * single CJK font (CONFIG_VELAPAW_FONT_CJK) swaps in everywhere at once; the
 * result is re-read every time a label is (re)built, i.e. on a language switch. */
#define F_TITLE  velapaw_font(VP_FONT_TITLE)
#define F_HEAD   velapaw_font(VP_FONT_HEAD)
#define F_BODY   velapaw_font(VP_FONT_BODY)
#define F_SMALL  velapaw_font(VP_FONT_SMALL)

#define COL_BG     0x0b1020
#define COL_CARD   0x161c2e
#define COL_CARD2  0x1e2740
#define COL_ACCENT 0x66ccff
#define COL_FED    0x57d977
#define COL_COOL   0xf0b24a
#define COL_UNK    0xe06464
#define COL_DIM    0x8893a6
#define COL_TEXT   0xffffff
#define COL_BTN    0x2a3550

typedef enum { EV_NONE, EV_VIEW, EV_CAPTURE, EV_SAVE, EV_NEXTDAY,
               EV_TREND_PREV, EV_TREND_NEXT, EV_TREND_WEEK,
               EV_TREND_MONTH,
               EV_CLK_HUP, EV_CLK_HDN,      /* set current clock +/- 1h */
               EV_CLK_MUP, EV_CLK_MDN,      /* set current clock +/- 5m */
               EV_MEAL_HUP, EV_MEAL_HDN,    /* set pet meal time +/- 1h */
               EV_MEAL_MUP, EV_MEAL_MDN,    /* set pet meal time +/- 5m */
               EV_MEAL_SEL,                 /* cycle which meal is being edited */
               EV_MEAL_VOICE,               /* start/cancel the voice dialog */
               EV_EDIT_SAVE, EV_EDIT_CANCEL, /* edit-an-enrolled-pet dialog */
               EV_EDIT_DELETE,              /* delete pet (two-tap confirm) */
               EV_LANG                      /* toggle UI language EN <-> 中 */
             } ui_event_t;

static volatile ui_event_t g_evt = EV_NONE;

/* shared live frame */
static uint16_t g_img565[VELAPAW_FRAME_W * VELAPAW_FRAME_H];
static lv_image_dsc_t g_dsc;
static lv_obj_t *g_img;      /* preview in Enroll tab */
static lv_obj_t *g_img_r;    /* preview in Recognize tab */

/* enroll widgets */
static lv_obj_t *g_sample_lbl;
static lv_obj_t *g_progress;
static lv_obj_t *g_name_ta;
static lv_obj_t *g_name_ok;
static lv_obj_t *g_kb;
static lv_obj_t *g_portion_slider;
static lv_obj_t *g_portion_lbl;
static lv_obj_t *g_limit_slider;
static lv_obj_t *g_limit_lbl;
static lv_obj_t *g_thumb[MAX_SHOTS];
/* Enrollment thumbnails are only ever drawn ~44px wide, so storing them at the
 * full 128x128 was wasting 160 KB of internal RAM (5 x 128x128x2). At 64x64 they
 * look identical on screen and cost 40 KB -- freeing 120 KB of DRAM. */
#define THUMB_W 64
#define THUMB_H 64
static uint16_t  g_thumb565[MAX_SHOTS][THUMB_W * THUMB_H];
static lv_image_dsc_t g_thumb_dsc[MAX_SHOTS];

/* recognize widgets */
static lv_obj_t *g_status;
static lv_obj_t *g_sub;
static lv_obj_t *g_metrics;
static lv_obj_t *g_count;
#ifdef CONFIG_VELAPAW_VOICE
static lv_obj_t *g_voice_lbl;     /* Recognize: voice prompt / last verdict */
#endif

/* my-pets widgets */
static lv_obj_t *g_pets_box;
/* Per-pet card icon descriptors -- must outlive refresh_pets (LVGL keeps the
 * pointer), so they are static and point straight at the identity icon store. */
static lv_image_dsc_t g_petcard_dsc[VELAPAW_MAX_PETS];

/* trends widgets */
static lv_obj_t *g_trend_chart;
static lv_chart_series_t *g_trend_ser;
static lv_obj_t *g_trend_title;
static lv_obj_t *g_trend_val;
static lv_obj_t *g_trend_stats;
static lv_obj_t *g_btn_week;
static lv_obj_t *g_btn_month;
static int g_trend_pet;
static int g_trend_month;               /* 0 = 7-day, 1 = 30-day */
static int32_t g_trend_vals[VELAPAW_HIST_DAYS];
static int32_t g_trend_avg;             /* mean of the shown window (bar colour split) */
/* appetite line chart (Trends): daily intake as % of the pet's allowance */
static lv_obj_t *g_appet_chart;
static lv_chart_series_t *g_appet_ser;
/* BCS gauge (Trends): a 3-zone dial (under/ideal/over) with a needle. */
static lv_obj_t *g_bcs_needle;
static lv_obj_t *g_bcs_lbl;
static lv_point_precise_t g_bcs_pts[2]; /* needle endpoints -- lv_line keeps this ptr */
static void refresh_bcs_gauge(void);    /* defined below refresh_trends, called by it */

/* tabs + header */
static lv_obj_t *g_scr;        /* active screen: the whole UI tree is rebuilt
                               * onto this when the language is toggled */
static lv_obj_t *g_tv;
static lv_obj_t *g_crumb;
static lv_obj_t *g_bell;
static lv_obj_t *g_langlbl;    /* header EN/中 language toggle */

static const struct velapaw_infer_backend *g_be;
static float g_samples[MAX_SHOTS][VELAPAW_EMBED_DIM];
static int   g_nsamp;
static int   g_petn;
static int   g_view;

/* scheduled feeding (Model B: a pet is fed only when it is RECOGNIZED at the
 * bowl AND one of its meal times is due and not yet fed today). */
#define MEAL_NONE      (-1)
/* meal times being edited in Enroll (min-of-day); g_meal_sel = which one the
 * +/- buttons act on. Defaults: 08:00, 12:30, 18:00. */
static int   g_meal_min[VELAPAW_MAX_MEALS] = { 8 * 60, 12 * 60 + 30, 18 * 60 };
static int   g_meal_sel;               /* 0..VELAPAW_MAX_MEALS-1 */
static lv_obj_t *g_meal_lbl;           /* "Meals: 08:00 12:30 18:00" */
static lv_obj_t *g_clock_lbl;          /* "Clock now: HH:MM" */

/* Edit-an-enrolled-pet dialog (My Pets -> Edit). The owner changes meal times /
 * portion / daily limit WITHOUT re-enrolling -- the pet's embedding is never
 * touched, so recognition is unaffected. Times are edited in g_edit_meal (a
 * scratch copy) so a cancelled edit leaves both the pet AND the Enroll tab's
 * defaults alone; the +/- buttons act on whichever of the two is active. */
/* idle gating (see FRAME_IDLE_MS / ENROLL_IDLE_MS) */
static long  g_last_touch;
static int   g_enroll_paused;

static int   g_edit_pet = -1;          /* pet being edited, -1 = dialog closed */
static int   g_edit_meal[VELAPAW_MAX_MEALS];
static lv_obj_t *g_edit_modal;
static lv_obj_t *g_edit_title;
static lv_obj_t *g_edit_meal_lbl;
static lv_obj_t *g_edit_portion_sl;
static lv_obj_t *g_edit_portion_lbl;
static lv_obj_t *g_edit_limit_sl;
static lv_obj_t *g_edit_limit_lbl;
static lv_obj_t *g_del_btn;            /* "Delete" -> "Sure?" -> gone */
static int       g_del_armed;          /* deleting a pet is destructive: 2 taps */
/* day-number each (pet, meal-slot) was last fed -> one feed per slot per day */
static long  g_meal_fed_day[VELAPAW_MAX_PETS][VELAPAW_MAX_MEALS];
/* When/how much each scheduled meal was actually eaten, so My Pets can show the
 * schedule-vs-reality timeline ("08:00  ate 08:02  15g"). ~192 bytes total. */
static int   g_meal_fed_min[VELAPAW_MAX_PETS][VELAPAW_MAX_MEALS];
static int   g_meal_fed_g[VELAPAW_MAX_PETS][VELAPAW_MAX_MEALS];
/* owner tapped this (pet, meal-slot) to skip it for today, e.g. skip meal 2
 * but still feed 1 and 3. Cleared wherever g_meal_fed_day resets to -1. */
static uint8_t g_meal_skipped[VELAPAW_MAX_PETS][VELAPAW_MAX_MEALS];
static long  g_last_fed[VELAPAW_MAX_PETS];
static velapaw_pet_id_t g_fed_visit = VELAPAW_PET_UNKNOWN;  /* pet already fed this visit */
static velapaw_pet_id_t g_bcs_for = VELAPAW_PET_UNKNOWN;    /* pet already BCS-scored this visit */
static velapaw_pet_id_t g_cand = VELAPAW_PET_UNKNOWN;       /* current candidate id */
static int g_stable;          /* consecutive frames the candidate has held */
static int g_bcs_votes[3];    /* per-visit BCS class votes (majority) */
static int g_bcs_nvotes;      /* votes cast this visit */
static int g_absent;          /* consecutive Unknown frames (debounce departure) */

static long mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- wall clock -----------------------------------------------------------
 * Software CLOCK_REALTIME (no hardware RTC: enabling it killed the USB console,
 * so the clock is set by the user each session via Set Clock and starts at a
 * sane default). LOCALTIME is off, so derive HH:MM straight from epoch seconds
 * and treat that as local time. */
static int wall_min(void)          /* minute-of-day, 0..1439 */
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int)((ts.tv_sec % 86400) / 60);
}
static long wall_day(void)         /* day number (for per-day "fed" resets) */
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long)(ts.tv_sec / 86400);
}
/* Board hook: persist the clock into the on-board PCF85063 RTC chip (direct
 * I2C in the board file). Weak-ish: returns <0 if the chip isn't there. */
extern int board_rtc_settime(time_t t);

static void wall_set_min(int minofday)   /* set clock to today at HH:MM */
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  if (minofday < 0) minofday += 1440;
  minofday %= 1440;
  ts.tv_sec  = (ts.tv_sec / 86400) * 86400 + (long)minofday * 60;
  ts.tv_nsec = 0;
  clock_settime(CLOCK_REALTIME, &ts);
  board_rtc_settime(ts.tv_sec);   /* survive reset / power-off */
}


/****************************************************************************
 * small widget helpers
 ****************************************************************************/

static lv_obj_t *make_label(lv_obj_t *p, const lv_font_t *f, uint32_t c)
{
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
  lv_obj_set_style_text_font(l, f, 0);
  lv_label_set_text(l, "");
  return l;
}

/* a rounded "card" container with no scrolling */
static lv_obj_t *make_card(lv_obj_t *p, int w, int h, uint32_t bg)
{
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 8, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void btn_cb(lv_event_t *e)
{
  g_evt = (ui_event_t)(intptr_t)lv_event_get_user_data(e);
}

/* pill button; returns the button (label centered) */
static lv_obj_t *make_btn(lv_obj_t *p, const char *txt, int w, uint32_t bg,
                          ui_event_t ev)
{
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_size(b, w, 38);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ev);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, F_HEAD, 0);
  lv_obj_center(l);
  return b;
}

/****************************************************************************
 * frame display
 ****************************************************************************/

static void frame_to_565(const struct velapaw_frame *f, uint16_t *dst)
{
  /* Rotate 90 deg counter-clockwise while converting (the sensor is mounted
   * rotated). Square frame (128x128), dims unchanged. dst(dr,dc)=src(dc, W-1-dr). */
  int W = f->width, H = f->height;
  for (int dr = 0; dr < H; dr++)
    for (int dc = 0; dc < W; dc++)
      {
        int si = (dc * W + (W - 1 - dr)) * 3;
        uint8_t r = f->data[si + 0];
        uint8_t g = f->data[si + 1];
        uint8_t b = f->data[si + 2];
        dst[dr * W + dc] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
}

/* Thumbnail version: same 90-deg CCW rotation, decimated 2x (128x128 -> 64x64). */
static void frame_to_thumb565(const struct velapaw_frame *f, uint16_t *dst)
{
  int W = f->width;                     /* 128 (square) */
  for (int dr = 0; dr < THUMB_H; dr++)
    for (int dc = 0; dc < THUMB_W; dc++)
      {
        int si = ((dc * 2) * W + (W - 1 - dr * 2)) * 3;
        uint8_t r = f->data[si + 0];
        uint8_t g = f->data[si + 1];
        uint8_t b = f->data[si + 2];
        dst[dr * THUMB_W + dc] =
            (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
}

static void show_frame(const struct velapaw_frame *f)
{
  frame_to_565(f, g_img565);
  if (g_img)
    {
      lv_obj_invalidate(g_img);
    }
  if (g_img_r)
    {
      lv_obj_invalidate(g_img_r);
    }
}

/****************************************************************************
 * camera capture, counted
 *
 * The board wedged (console "semaphore timeout") after a long idle on the
 * Enroll tab -- the preview grabs a frame every FRAME_INTERVAL_MS forever, so
 * minutes of idling = many thousands of captures, and the next Capture died.
 * Every capture goes through here so the log tells us the frame number at the
 * wedge: a repeatable count => something leaks per-capture; no pattern => it's
 * time/temperature/electrical. Do not guess this one again.
 ****************************************************************************/

static unsigned long g_cap_n;

/* ---- background inference worker: shared state -----------------------------
 * Declared up here because BOTH the recognize loop and Enroll's Capture hand
 * work to the worker. Inference MUST NOT run on the UI task: that task's stack
 * is in PSRAM, and running the ESP-NN/TFLM Invoke on a PSRAM stack wedges the
 * SoC (console goes dead, "semaphore timeout") -- the same trap the flash model
 * loader hit. A pthread's stack is heap/internal RAM, where it runs fine.
 * Protocol: 0 idle -> 1 request -> 2 busy -> 3 done -> 0. */
static struct velapaw_frame   g_inf_frame;
static float                  g_inf_emb[VELAPAW_EMBED_DIM];
static struct velapaw_match   g_inf_m;
static uint32_t               g_inf_us;
static volatile int           g_inf_state;  /* 0 idle,1 request,2 busy,3 done */
static volatile int           g_inf_mode;   /* 0 = recognize, 1 = enroll capture */
static int                    g_cap_pending;/* an enroll capture is in flight */

/* Serialises the two TFLM interpreters (vision embed / KWS classify), which
 * now live on two different threads and would otherwise Invoke concurrently.
 * Their arenas are separate, so this is not about the arenas -- it is the
 * ESP-NN kernels underneath them, which VelaPaw builds with the optimised
 * path and which are not documented to be reentrant.  Serialising costs
 * nothing we can measure (vision runs at most every RECOG_INTERVAL_MS, voice
 * classify is ~90 ms) whereas the failure mode for getting it wrong on this
 * board is a dead console with no backtrace.  Capture is deliberately OUTSIDE
 * the lock: the 2 s mic read must overlap with vision, not block it. */
static pthread_mutex_t        g_tflm_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef CONFIG_VELAPAW_VOICE
/* ---- voice (keyword spotting): shared state ------------------------------
 * A second worker, same shape as the inference one but simpler, because voice
 * has no input to hand over: the worker owns the microphone end to end and the
 * UI only reads the verdict.
 *
 * Why a persistent thread and not a capture-on-demand call: velapaw_mic_capture
 * opens the audio devices on first use and keeps the session for the life of
 * the process.  Tearing it down and reopening leaves I2S RX unable to arm again
 * -- a second session times out with -110 and only a cold power cycle clears it
 * (see voice/mic.c).  Holding one session in a thread that never exits is not a
 * workaround for that bug, it is the design the bug forced us to prove out, and
 * it is exactly what a feeder wants anyway.
 *
 * It also must not run on the UI task, for the same reason the vision worker
 * does not: that task's stack is in PSRAM and TFLM Invoke on a PSRAM stack
 * wedges the SoC.
 *
 * Handover is one flag, g_voice_evt, written only by the worker and cleared
 * only by the UI loop, so there is no lock and no lost-update window: the
 * worker refuses to post while a verdict is still pending, which also means
 * g_voice_res is stable for as long as the UI needs to read it. */

static volatile int   g_voice_run;    /* UI -> worker: 1 = listen, 0 = idle   */
static volatile int   g_voice_ready;  /* worker -> UI: a capture has succeeded*/
static volatile int   g_voice_fail;   /* worker -> UI: mic/model unusable     */
static volatile int   g_voice_evt;    /* worker -> UI: 1 = a result is waiting*/
static float          g_voice_score;  /* winning score, for the Recognize tab */

/* Probe counters, read by "velapaw voice" (see velapaw_ui_voice_probe).
 *
 * Added in build 60 because the worker's own printfs are unreadable: it starts
 * at boot, and everything it says before USB CDC enumerates is gone.  Twice now
 * a cause has been inferred from symptoms and been wrong -- first -110
 * poisoning, then a wedged capture -- when the state that would have settled it
 * was sitting in memory the whole time.  This is a flat build, so a CLI task
 * shares .data with the UI and can just read it.
 *
 * Sixteen bytes, and they answer the only question that matters: how far does
 * the worker get, and what is it stuck on.
 */

static volatile int   g_voice_init_rc = -999;  /* velapaw_kws_init() return   */
static volatile int   g_voice_loops;           /* worker loop iterations      */
static volatile int   g_voice_caps;            /* captures ATTEMPTED          */
static volatile int   g_voice_lastret = -999;  /* last capture return value   */

/* Build 79.  A failed capture no longer kills the worker, so the failures
 * have to be counted somewhere instead of being implied by `fail 1`.
 *
 * capfail is the running total; consec is the current unbroken run and is
 * cleared by any success.  Only consec can disable voice -- a total is the
 * wrong thing to latch on, because a long session that drops one window an
 * hour is working fine.
 */

/* Hoisted so velapaw_ui_voice_probe() can print it -- the dialog's own
 * #define sits far below the probe.  See VOICE_CAPTURE_MS: these two must
 * never be equal again.
 */

#define VSCHED_MIC_WAIT_MS_VAL 8000

/* Build 81: velapaw_mic_preopen()'s return, for the probe.  0 = the session
 * was open and running before any dialog could ask for it.  Anything else
 * means the first capture had to open it inline, and the good-boot margin
 * is back to what it was.
 */

static volatile int   g_voice_preopen_rc = -999;

static volatile int   g_voice_capfail;         /* captures that failed        */
static volatile int   g_voice_consec;          /* ... consecutively, right now */

/* Consecutive failures tolerated before voice is declared dead.  mic.c
 * already retries MIC_KICK_TRIES times inside each call, so this is a
 * multiplier on that, not the whole budget.
 */

#define VOICE_MAX_CAPFAIL 3

/* ---- voice event log -----------------------------------------------------
 *
 * A ring of the last VLOG_N things the voice path did, each stamped with the
 * millisecond it happened, dumped by "velapaw voice".
 *
 * This exists because printf to the console is not reliable here, which build
 * 61 finally established rather than suspected: the dialog ran three steps and
 * printed a `velapaw/voice: step ...` line for each, and not one of them
 * reached the terminal -- only the es8311 driver's syslog spam did.  Raising
 * CONFIG_CDCACM_TXBUFSIZE to 2048 in build 58 did not fix it.  The likely
 * mechanism is the O_NONBLOCK this file puts on fd 1 (see velapaw_ui_run):
 * NuttX hands a task's stdio down as dup'd descriptors sharing one struct
 * file, so that flag plausibly applies to nsh's console too, and a full TX
 * buffer then DROPS instead of waiting -- for every writer, not just this one.
 *
 * Removing the O_NONBLOCK is not an option: it is what stops the UI and the
 * camera freezing solid when no PC is attached to the USB CDC.  So the record
 * is kept in RAM, where nothing can drop it, and read out later on demand.
 *
 * The timestamps are the other half of the point.  Eleven captures across one
 * dialog is either fine or terrible depending on how much of that wall time
 * the microphone was actually listening, and that gap is invisible without
 * them.  A capture is ~2 s of stream; anything much beyond that per cycle is
 * time the feeder is deaf, and words spoken into it are not misheard, they are
 * never heard at all.
 *
 * Written from two threads -- the worker classifies, the UI task steps the
 * dialog -- with no lock.  A collision costs one garbled line and cannot
 * corrupt anything else: each slot is a fixed-size array written by snprintf,
 * and the head only ever increments.  Worth strictly less than the cost of
 * taking a mutex on the audio path.
 */

#define VLOG_N   28
#define VLOG_LEN 76

static char              g_vlog[VLOG_N][VLOG_LEN];
static volatile unsigned g_vlog_head;   /* total lines ever written */

static void vlog(const char *fmt, ...)
{
  unsigned slot = g_vlog_head % VLOG_N;
  va_list  ap;
  int      n;

  n = snprintf(g_vlog[slot], VLOG_LEN, "%6ld ", mono_ms());
  if (n < 0 || n >= VLOG_LEN)
    {
      return;
    }

  va_start(ap, fmt);
  vsnprintf(g_vlog[slot] + n, VLOG_LEN - n, fmt, ap);
  va_end(ap);

  g_vlog_head++;
}

/* The FULL classifier result, not just the winning label.
 *
 * Build 54 posted `label + 1` because the only consumer wanted yes/no.  The
 * scheduling dialog needs the whole score vector: each of its steps decides
 * among a different subset of the vocabulary (see velapaw_kws_best_of), and
 * the 14-way winner is frequently not the answer to the question that was
 * actually asked -- "_unknown_" outscoring a correctly-heard "eight" is the
 * normal case, not a failure.  Filtering in the worker would throw away the
 * information before the step that needs it ever sees it. */
static struct velapaw_kws_result_s g_voice_res;

/* UI-side only.  g_voice_dismissed is what "no" actually DOES: it stops the
 * listening for this visit, so the owner can shut the feeder up without
 * leaving the tab, and it clears when they leave or the pet does.  Without it
 * "no" would print a message and change nothing, which is not an answer. */
static int            g_voice_dismissed;
static long           g_voice_hold;   /* keep a verdict on screen until this  */
static int            g_voice_shown;  /* 0 blank, 1 prompt, 2 off, 3 verdict  */

#define VOICE_MSG_HOLD_MS 5000

/* The microphone is captured 1.25 s at a time and the model is given the best
 * 1 s inside that -- see voice_align().  40 KB, touched only by the worker,
 * static rather than automatic because it does not belong on a thread stack.
 *
 * Build 62 measured why the extra half second is needed.  Captures repeated
 * every 1440 ms and each retained 1000 ms of audio, so 31% of wall time was
 * never recorded (MIC_SKIP_BUFS discards the first buffer inside every
 * capture), and the 1 s that WAS kept started wherever the buffer boundary
 * happened to fall.  A word straddling either edge reached the model with its
 * onset or its tail cut off, which is what produced a correct label at a weak
 * score: "eight" was heard three times running at 0.441, 0.176 and 0.316
 * against a 0.55 gate.  Capturing wider and choosing the window afterwards
 * costs one extra buffer of latency and fixes both.
 *
 * AND IT IS CURRENTLY 1.0 s, i.e. no slack at all.  Four builds were spent on
 * the wider window and none of them captured:
 *
 *   b63  1.5 s   overflowed dram0_0_seg by 2804 bytes at link time
 *   b64  1.25 s  linked, but the FIRST capture never returned: it failed and
 *                hung inside mic_session_close(), taking the voice worker
 *                with it for the rest of the boot
 *   b65  1.25 s  same failure with the record pool raised 18 -> 22 buffers
 *   b66  1.0 s   SAME FAILURE AGAIN, at a width b62 had captured happily
 *
 * b66 is the one that settles it.  The width was back to what worked, the pool
 * was back to 18, and it still died -- so the wider window was never the cause
 * and neither was the pool ceiling.  b66 also carried velapaw_mic_last_error()
 * and it answered: -110, ETIMEDOUT, returned by mic_collect().  The stage probe
 * agreed to the digit (seq 45 lands exactly on MIC_STAGE_CLOSE for an
 * 18-buffer session open), so the session opens correctly every time and it is
 * the collect that comes up empty.
 *
 * The alignment machinery below is kept and is inert at this width, because
 * the analysis behind it stands -- the 31% coverage gap and the edge
 * truncation are both real and measured -- and the #error guard makes
 * re-widening safe once the collect failure is understood.
 *
 * What is NOT blocked on any of that is the vote rule at VSCHED_VOTES, which
 * addresses the same rejected utterances without needing one extra sample.
 */

#define VOICE_CAPTURE_SAMPLES VELAPAW_KWS_SAMPLE_RATE

/* How long to let one capture run before giving up.
 *
 * 8000, restored from the 5000 build 66 shipped.  That reduction was tidying,
 * not a measurement: 8000 is the budget every capture that has ever worked on
 * this board ran under, and b66 is the only build to have failed at a width
 * b62 succeeded at.  A tightened timeout is the ONLY functional difference
 * between those two builds in the whole capture path, so it goes back first
 * and on its own.
 *
 * The number is not arbitrary either way.  Collecting 16000 samples needs 16
 * DMA deliveries plus MIC_SKIP_BUFS, which is about 1.1 s of audio time, so 5 s
 * is nearly five times the steady-state cost -- ample for any capture EXCEPT
 * the first of a session, which is also the one that carries the codec's ADC
 * power-up.  That first capture is why MIC_WARMUP_BUFS exists at all, and it is
 * precisely the capture that failed.
 *
 * If a first capture still times out at 8 s, the delay is not settling and the
 * next place to look is mic.c, not this file. */

/* Build 81: 8000 -> 3500.
 *
 * This was the SAME NUMBER as VSCHED_MIC_WAIT_MS below, which meant the
 * dialog gave the capture exactly as long as the capture gave itself --
 * while the capture's clock started later, after ~5350 ms of session open.
 * The dialog could never outlast a failing capture, so b79's retry loop
 * has never once executed (b80: `caps 1 capfail 1 consec 1`).
 *
 * Safe only because velapaw_mic_preopen() now pays the session-open cost at
 * boot.  Without that this would just fire retries into a window still
 * half-consumed by setup.  Two full attempts now fit inside the dialog's
 * 8000 ms: 3500 + 200 sleep + 3500 = 7200.
 *
 * 1.75x margin on a 2000 ms capture.  b66 failed at 5000 and b67 restored
 * 8000, but that was three lucky boots -- the tightening was never the
 * cause, so do not read this number as load-bearing either.
 */

#define VOICE_CAPTURE_MS 3500

/* Checked here rather than trusted, because build 64 showed what asking for
 * too much costs: not a short capture and not an error code, but a capture
 * that never returns and a voice worker that is gone until the next power
 * cycle.  There is no runtime check that can catch that -- the call does not
 * come back to be checked -- so it has to fail at build time instead. */

#if VOICE_CAPTURE_SAMPLES > VELAPAW_MIC_MAX_SAMPLES
#  error "VOICE_CAPTURE_SAMPLES exceeds what the mic buffer pool can deliver; "
#  error "raise VELAPAW_MIC_BUFS in voice/mic.h or shorten the window"
#endif

static int16_t        g_voice_audio[VOICE_CAPTURE_SAMPLES];

/* Acceptance thresholds, deliberately asymmetric.  Acting on a false "yes"
 * puts food in the bowl for a pet that should not eat; missing a real one
 * costs the owner one repeat.  So "yes" is held to well above the worst score
 * we have measured for a real utterance and "no" is let through easily.
 * Measured on hardware, build 52/53: yes 0.980 / 0.996, no 0.750 / 0.789. */
#define VOICE_YES_SCORE  0.80f
#define VOICE_NO_SCORE   0.60f

/* ---- voice scheduling dialog ---------------------------------------------
 * Sets a meal time by voice, from the edit-pet modal.  Three steps, each a
 * closed question:
 *
 *   MEAL     "which meal?"        one / two / three
 *   HOUR     "hour?"              zero .. nine
 *   CONFIRM  "08:00 - yes/no?"    yes / no      (a "no" offers HH+12)
 *
 * There is no wake word.  "Hi VelaPaw" is not in Speech Commands and would
 * have to be recorded and trained from scratch, and listening continuously
 * costs ~40% of the single core we have.  A button the owner already has to
 * reach the schedule screen with is strictly better than both.
 *
 * The pet is never spoken: the camera already knows which animal this is, and
 * asking the owner to say a name the model was never trained on would be
 * worse than the sensor on the board.
 *
 * One digit plus the AM/PM confirm covers 00:00-09:00 and 12:00-21:00, which
 * is every hour a feeder is plausibly scheduled for.  10 and 11 need the
 * +/- buttons -- an honest gap, not an oversight.
 *
 * Thresholds are per step and are RAW softmax probabilities of the best legal
 * word (velapaw_kws_best_of does not renormalise -- see voice/kws.h for why
 * that matters).  They were re-tuned against real ES8311 capture in build 56;
 * the block below records what moved and why. */

/* RETUNED ON HARDWARE, build 56.  The first set came off the test split and
 * priced a wrong digit as expensive.  Two things were wrong with that.
 *
 * First, the microphone.  Correctly-heard words through the ES8311 at normal
 * speaking distance measured:
 *
 *   one    0.680      eight  0.707      yes  0.668
 *
 * against a test-split median near 0.99.  HOUR was 0.73, so a correctly-heard
 * "eight" was BELOW the gate: the dialog would have spent all three tries on
 * good speech and cancelled.
 *
 * Second, the pricing.  A wrong hour is not expensive -- VS_CONFIRM reads it
 * back on screen and the owner says "no".  A rejected hour is: it burns a try
 * and ends with nothing set.  Measured on the test split, moving HOUR 0.73 ->
 * 0.55 takes accept 64.6% -> 78.9% and correct-given-accepted 96.9% -> 94.2%,
 * i.e. it trades 2.7 points of confirm work for 14 points of the dialog
 * actually completing.  That is the right side of the trade for a step that
 * has a confirm behind it.
 *
 * The junk false-accept column that drove the first pass overstates the risk
 * anyway: it counts _silence_ clips, and velapaw_mic_speech_present() drops
 * silence before the classifier ever runs.
 */

#define VSCHED_MEAL_SCORE    0.40f
#define VSCHED_HOUR_SCORE    0.55f
#define VSCHED_CONFIRM_SCORE 0.55f

/* Agreement: a step also accepts a word it has heard twice, below threshold.
 *
 * Build 62 ended with the hour step rejecting three captures in a row that
 * were all the SAME correct word -- eight at 0.441, 0.176 and 0.316 against a
 * 0.55 gate -- and cancelling.  A single score says how confident the model is
 * in one window; repetition across INDEPENDENT windows is separate evidence
 * that the per-window threshold cannot see, and throwing it away is what made
 * a correctly-heard answer unreachable.
 *
 * Votes, not a consecutive run.  The same log rules out a run: the three
 * "eight" readings were interrupted by the 0.176 one, which any floor worth
 * having excludes, so a run counter would have reset on it and never fired.
 * Captures under the floor are ignored rather than treated as disagreement.
 *
 * The floor is what keeps this honest, and the same run sets it.  The failing
 * MEAL step landed on "two" twice -- at 0.003 and 0.211 -- so votes alone
 * would have accepted a meal slot from noise.  0.25 excludes both of those
 * while admitting both usable "eight" readings; there is roughly a factor of
 * two of daylight either side of it in the only data we have.
 *
 * MEAL and HOUR only.  Both have the confirm step behind them, where a wrong
 * answer costs one spoken "no" and is read back on screen first.  CONFIRM
 * itself is the last gate and is a two-way choice, where agreeing twice is
 * only as surprising as a coin landing the same way twice -- it keeps the
 * threshold and nothing else.
 */

#define VSCHED_VOTES       2
#define VSCHED_VOTE_FLOOR  0.25f

/* A step gives up after this many unusable captures.
 *
 * Was three, which is where measured accept rates made a fourth try nearly
 * free of new information -- true of a rule that only ever looks at one
 * capture.  Votes make later captures informative precisely BECAUSE earlier
 * ones exist, so the count has to leave room for a second vote to arrive: at
 * ~1.4 s per capture five still fits inside VSCHED_TIMEOUT_MS with margin.
 */
#define VSCHED_TRIES 5

/* And gives up on silence after this long -- someone walked away mid-dialog.
 *
 * Build 62 timed the cycle rather than guessing it: a 1 s capture returned in
 * 1230 ms and the worker came back round every 1440 ms.  At 1.25 s of audio
 * that is roughly 1.75 s a turn, so this is about eight attempts' worth of
 * patience and comfortably more than VSCHED_TRIES can spend. */
#define VSCHED_TIMEOUT_MS 15000

enum vsched_state_e
{
  VS_IDLE = 0,
  VS_MEAL,          /* which of the pet's meal slots                       */
  VS_HOUR,          /* the hour digit                                      */
  VS_CONFIRM,       /* HH:00 -- yes commits, no offers the PM reading      */
  VS_CONFIRM_PM     /* (HH+12):00 -- yes commits, no re-asks the hour      */
};

static struct
{
  int  state;
  int  slot;        /* 0..VELAPAW_MAX_MEALS-1, chosen in VS_MEAL           */
  int  hour;        /* 0..9, chosen in VS_HOUR                             */
  int  tries;       /* unusable captures at the current step               */
  long deadline;    /* mono_ms() by which this step must produce something */

  /* Below-threshold captures at the current step that still cleared
   * VSCHED_VOTE_FLOOR, counted per word of the step's subset and indexed by
   * position in it.  Ten is the largest subset (the hour digits).  Cleared by
   * vsched_enter, so votes never carry across a step or a dialog. */

  int  votes[10];
} g_vs;

static lv_obj_t *g_vc_lbl;      /* dialog prompt / verdict, in the modal    */
static lv_obj_t *g_vc_btn;      /* the mic button (doubles as cancel)       */
#endif /* CONFIG_VELAPAW_VOICE */

static void cam_grab(struct velapaw_frame *f)
{
  velapaw_camera_get_frame(f);
  if (++g_cap_n % 200 == 0)
    {
      printf("velapaw/cam: capture #%lu\n", g_cap_n);
    }
}

/****************************************************************************
 * appetite / alert  (relative to each pet's DAILY LIMIT = the ration the
 * owner set, not a rolling average)
 ****************************************************************************/

/* % of today's ration eaten: today / daily_limit (0..100, since feeding stops
 * at the limit). 100% = ate its full daily allowance. */
static int pet_appetite_pct(velapaw_pet_id_t id, const struct velapaw_pet *p)
{
  int today = velapaw_store_today_total(id);
  int pct = (p->daily_limit_g > 0) ? today * 100 / p->daily_limit_g : 100;
  return (pct > 100) ? 100 : pct;   /* limit is a cap: never over 100% */
}

/* appetite-drop alert: the last completed day was under half the daily limit. */
static int pet_alert_drop(velapaw_pet_id_t id, const struct velapaw_pet *p)
{
  int yesterday = velapaw_store_day_total(id, 1);
  return (p->daily_limit_g > 0 && yesterday * 2 < p->daily_limit_g) ? 1 : 0;
}

/****************************************************************************
 * enroll
 ****************************************************************************/

static void refresh_enroll(void)
{
  if (g_meal_lbl)
    {
      /* show all 3 meals; mark the one the +/- buttons are editing with [ ] */
      char buf[64]; int n = 0;
      n += snprintf(buf + n, sizeof(buf) - n, "%s", T(S_MEALS));
      for (int k = 0; k < VELAPAW_MAX_MEALS && n < (int)sizeof(buf) - 12; k++)
        {
          n += snprintf(buf + n, sizeof(buf) - n,
                        (k == g_meal_sel) ? " [%02d:%02d]" : " %02d:%02d",
                        g_meal_min[k] / 60, g_meal_min[k] % 60);
        }
      lv_label_set_text(g_meal_lbl, buf);
    }
  if (g_clock_lbl)
    {
      int c = wall_min();
      lv_label_set_text_fmt(g_clock_lbl, T(S_CLOCK_NOW_FMT), c / 60, c % 60);
    }
  lv_label_set_text_fmt(g_sample_lbl, T(S_SAMPLE_FMT), g_nsamp, MAX_SHOTS);
  lv_bar_set_value(g_progress, g_nsamp * 100 / MAX_SHOTS, LV_ANIM_OFF);
  for (int i = 0; i < MAX_SHOTS; i++)
    {
      if (i < g_nsamp)
        {
          lv_obj_clear_flag(g_thumb[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_thumb[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void do_capture(void)
{
  if (g_nsamp >= MAX_SHOTS)
    {
      return;
    }
  if (g_inf_state != 0 || g_cap_pending)
    {
      return;                       /* an inference is already in flight */
    }

  /* Grab the frame here (cheap, ~ms), but hand the INFERENCE to the worker.
   * Running embed() inline on this task wedged the SoC -- see the note on the
   * worker state above. The loop picks the result up when g_inf_state == 3. */
  cam_grab(&g_inf_frame);
  g_inf_mode    = 1;                /* enroll capture: embed, don't match */
  g_cap_pending = 1;
  g_inf_state   = 1;                /* -> worker */
  lv_label_set_text(g_sample_lbl, T(S_CAPTURING));
}

/* Called from the UI loop once the worker has embedded an enroll capture. */
static void finish_capture(void)
{
  memcpy(g_samples[g_nsamp], g_inf_emb, sizeof(g_inf_emb));
  frame_to_thumb565(&g_inf_frame, g_thumb565[g_nsamp]);
  lv_image_set_src(g_thumb[g_nsamp], &g_thumb_dsc[g_nsamp]);
  g_nsamp++;
  printf("velapaw/ui: captured sample %d/%d\n", g_nsamp, MAX_SHOTS);

  g_cap_pending = 0;
  g_inf_mode    = 0;
  g_inf_state   = 0;                /* worker free; preview resumes */
  refresh_enroll();
}

static void refresh_pets(void);

static void popup_ok_cb(lv_event_t *e)
{
  /* delete the whole modal overlay (async: safe from within the callback) */
  lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

/* Centered modal over everything: green tick + message + OK. */
static void show_enroll_popup(const char *msg)
{
  lv_obj_t *ov = lv_obj_create(lv_layer_top());
  lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_50, 0);
  lv_obj_set_style_border_width(ov, 0, 0);
  lv_obj_set_style_radius(ov, 0, 0);
  lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *card = make_card(ov, 400, 200, COL_CARD);
  lv_obj_center(card);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card, 10, 0);

  lv_obj_t *title = make_label(card, F_TITLE, COL_FED);
  lv_label_set_text(title, T(S_ENROLLED));

  lv_obj_t *t = make_label(card, F_BODY, COL_TEXT);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t, 340);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(t, msg);

  lv_obj_t *ok = lv_button_create(card);
  lv_obj_set_size(ok, 140, 40);
  lv_obj_set_style_bg_color(ok, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_radius(ok, 10, 0);
  lv_obj_add_event_cb(ok, popup_ok_cb, LV_EVENT_CLICKED, ov);
  lv_obj_t *okl = lv_label_create(ok);
  lv_label_set_text(okl, T(S_OK));
  lv_obj_set_style_text_font(okl, F_HEAD, 0);
  lv_obj_center(okl);
}

/* Tap the header bell -> list current health notifications. */
static void bell_cb(lv_event_t *e)
{
  (void)e;
  lv_obj_t *ov = lv_obj_create(lv_layer_top());
  lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_50, 0);
  lv_obj_set_style_border_width(ov, 0, 0);
  lv_obj_set_style_radius(ov, 0, 0);
  lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *card = make_card(ov, 420, 250, COL_CARD);
  lv_obj_center(card);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 8, 0);

  lv_obj_t *title = make_label(card, F_TITLE, COL_ACCENT);
  lv_label_set_text(title, T(S_NOTIFICATIONS));

  int alerts = 0;
  int n = velapaw_identity_count();
  for (int i = 0; i < n; i++)
    {
      const struct velapaw_pet *p = velapaw_identity_get(i);
      if (pet_alert_drop(i, p))
        {
          lv_obj_t *l = make_label(card, F_BODY, COL_UNK);
          lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
          lv_obj_set_width(l, 380);
          lv_label_set_text_fmt(l, T(S_ALERT_APPETITE_FMT), p->name);
          alerts++;
        }
      int bcs = velapaw_store_get_bcs(i);
      if (bcs == 0 || bcs == 2)
        {
          lv_obj_t *l = make_label(card, F_BODY, COL_COOL);
          lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
          lv_obj_set_width(l, 380);
          lv_label_set_text_fmt(l, T(S_ALERT_BCS_FMT),
              p->name, (bcs == 0) ? T(S_UNDERWEIGHT) : T(S_OVERWEIGHT));
          alerts++;
        }
    }
  if (alerts == 0)
    {
      lv_obj_t *l = make_label(card, F_BODY, COL_FED);
      lv_label_set_text(l, T(S_ALL_NORMAL));
    }

  lv_obj_t *ok = lv_button_create(card);
  lv_obj_set_size(ok, 140, 38);
  lv_obj_set_style_bg_color(ok, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_radius(ok, 10, 0);
  lv_obj_add_event_cb(ok, popup_ok_cb, LV_EVENT_CLICKED, ov);
  lv_obj_t *okl = lv_label_create(ok);
  lv_label_set_text(okl, T(S_CLOSE));
  lv_obj_set_style_text_font(okl, F_HEAD, 0);
  lv_obj_center(okl);
}

static void do_save(void)
{
  if (g_nsamp == 0)
    {
      lv_label_set_text(g_sample_lbl, T(S_CAP_AT_LEAST));
      return;
    }
  char name[VELAPAW_NAME_MAX];
  const char *typed = lv_textarea_get_text(g_name_ta);
  if (typed && typed[0] != '\0')
    {
      strncpy(name, typed, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
    }
  else
    {
      snprintf(name, sizeof(name), "Pet %d", g_petn + 1);
    }
  int portion = (int)lv_slider_get_value(g_portion_slider);
  int limit   = (int)lv_slider_get_value(g_limit_slider);
  if (limit < portion)
    {
      limit = portion;   /* never below one serving */
    }
  velapaw_pet_id_t id = velapaw_identity_enroll(name, portion,
                                                UI_DEMO_COOLDOWN_S, limit,
                                                g_meal_min,   /* 3 meal times */
                                                g_samples, g_nsamp);
  printf("velapaw/ui: saved %s (id %d) %dg from %d samples\n",
         name, id, portion, g_nsamp);

  /* Card icon: down-sample a middle capture's 64x64 thumbnail to the icon size
   * (nearest-neighbour) and persist it. Reuses the enroll thumbnail, which is
   * already rotated to display orientation. */
  if (id != VELAPAW_PET_UNKNOWN && g_nsamp > 0)
    {
      static uint16_t icon[VELAPAW_ICON_W * VELAPAW_ICON_H];
      const uint16_t *src = g_thumb565[g_nsamp / 2];   /* a middle shot */
      for (int y = 0; y < VELAPAW_ICON_H; y++)
        for (int x = 0; x < VELAPAW_ICON_W; x++)
          icon[y * VELAPAW_ICON_W + x] =
              src[(y * THUMB_H / VELAPAW_ICON_H) * THUMB_W
                  + (x * THUMB_W / VELAPAW_ICON_W)];
      velapaw_identity_set_icon(id, icon);
      velapaw_identity_save_icons();
    }

  velapaw_identity_save();               /* persist to flash: survives reset */
  velapaw_store_seed(id, portion * 3);   /* baseline ~3 meals/day for trends */
  g_petn++;
  g_nsamp = 0;
  lv_textarea_set_text(g_name_ta, "");
  refresh_enroll();
  refresh_pets();
  lv_label_set_text_fmt(g_count, T(S_ENROLLED_COUNT_FMT), velapaw_identity_count());

  char msg[96];
  snprintf(msg, sizeof(msg), T(S_ENROLLED_MSG_FMT), name);
  show_enroll_popup(msg);
}

static void name_ta_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED)
    {
      lv_keyboard_set_textarea(g_kb, g_name_ta);
      lv_obj_remove_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    }
  else if (code == LV_EVENT_VALUE_CHANGED)
    {
      const char *t = lv_textarea_get_text(g_name_ta);
      if (t && t[0] != '\0')
        {
          lv_obj_clear_flag(g_name_ok, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_name_ok, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void kb_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
      lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void portion_cb(lv_event_t *e)
{
  (void)e;
  lv_label_set_text_fmt(g_portion_lbl, T(S_PORTION_FMT),
                        (int)lv_slider_get_value(g_portion_slider));
}

static void limit_cb(lv_event_t *e)
{
  (void)e;
  lv_label_set_text_fmt(g_limit_lbl, T(S_DAILY_LIMIT_FMT),
                        (int)lv_slider_get_value(g_limit_slider));
}

static void build_enroll(lv_obj_t *tab)
{
  lv_obj_set_style_pad_all(tab, 8, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tab, 6, 0);

  /* camera preview card -- wider than tall so it reads like a real viewfinder */
  lv_obj_t *cam = make_card(tab, COL_W, 168, COL_CARD);
  g_img = lv_image_create(cam);
  lv_image_set_src(g_img, &g_dsc);
  lv_image_set_scale_x(g_img, IMG_SCALE_X);
  lv_image_set_scale_y(g_img, IMG_SCALE_Y);
  lv_obj_center(g_img);

  /* Capture button directly under the camera so you can aim and grab a shot
   * without scrolling; the sample filmstrip sits below it. */
  { lv_obj_t *caprow = lv_obj_create(tab);
    lv_obj_set_size(caprow, COL_W, 44);
    lv_obj_set_style_bg_opa(caprow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(caprow, 0, 0);
    lv_obj_set_style_pad_all(caprow, 0, 0);
    lv_obj_clear_flag(caprow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap = make_btn(caprow, T(S_CAPTURE), COL_W - 8,
                             COL_BTN, EV_CAPTURE);
    lv_obj_center(cap); }

  /* sample progress + filmstrip */
  g_sample_lbl = make_label(tab, F_BODY, COL_DIM);
  g_progress = lv_bar_create(tab);
  lv_obj_set_size(g_progress, COL_W - 20, 8);
  lv_obj_set_style_bg_color(g_progress, lv_color_hex(COL_CARD2), 0);
  lv_obj_set_style_bg_color(g_progress, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_bar_set_range(g_progress, 0, 100);

  lv_obj_t *strip = lv_obj_create(tab);
  lv_obj_set_size(strip, COL_W, 56);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(strip, 0, 0);
  lv_obj_set_style_pad_all(strip, 0, 0);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < MAX_SHOTS; i++)
    {
      g_thumb_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
      g_thumb_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
      g_thumb_dsc[i].header.w = THUMB_W;
      g_thumb_dsc[i].header.h = THUMB_H;
      g_thumb_dsc[i].header.stride = THUMB_W * 2;
      g_thumb_dsc[i].data = (const uint8_t *)g_thumb565[i];
      g_thumb_dsc[i].data_size = sizeof(g_thumb565[i]);
      g_thumb[i] = lv_image_create(strip);
      lv_image_set_src(g_thumb[i], &g_thumb_dsc[i]);
      lv_image_set_scale(g_thumb[i], 176);  /* 64px -> ~44px thumb */
      lv_obj_add_flag(g_thumb[i], LV_OBJ_FLAG_HIDDEN);
    }

  /* details card: name + portion + limit + meal time + clock */
  lv_obj_t *det = make_card(tab, COL_W, 340, COL_CARD);
  lv_obj_set_flex_flow(det, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(det, 5, 0);

  lv_obj_t *nrow = lv_obj_create(det);
  lv_obj_set_size(nrow, lv_pct(100), 40);
  lv_obj_set_style_bg_opa(nrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(nrow, 0, 0);
  lv_obj_set_style_pad_all(nrow, 0, 0);
  lv_obj_clear_flag(nrow, LV_OBJ_FLAG_SCROLLABLE);
  g_name_ta = lv_textarea_create(nrow);
  lv_textarea_set_one_line(g_name_ta, true);
  lv_textarea_set_placeholder_text(g_name_ta, T(S_PET_NAME));
  lv_obj_set_width(g_name_ta, COL_W - 90);
  lv_obj_align(g_name_ta, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_add_event_cb(g_name_ta, name_ta_cb, LV_EVENT_ALL, NULL);
  g_name_ok = make_label(nrow, F_HEAD, COL_FED);
  lv_label_set_text(g_name_ok, LV_SYMBOL_OK);
  lv_obj_align(g_name_ok, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_flag(g_name_ok, LV_OBJ_FLAG_HIDDEN);

  g_portion_lbl = make_label(det, F_BODY, COL_TEXT);
  lv_label_set_text_fmt(g_portion_lbl, T(S_PORTION_FMT), 15);
  g_portion_slider = lv_slider_create(det);
  lv_slider_set_range(g_portion_slider, 5, 50);
  lv_slider_set_value(g_portion_slider, 15, LV_ANIM_OFF);
  lv_obj_set_width(g_portion_slider, COL_W - 40);
  lv_obj_add_event_cb(g_portion_slider, portion_cb, LV_EVENT_VALUE_CHANGED, NULL);

  g_limit_lbl = make_label(det, F_BODY, COL_TEXT);
  lv_label_set_text_fmt(g_limit_lbl, T(S_DAILY_LIMIT_FMT), 60);
  g_limit_slider = lv_slider_create(det);
  lv_slider_set_range(g_limit_slider, 20, 300);
  lv_slider_set_value(g_limit_slider, 60, LV_ANIM_OFF);
  lv_obj_set_width(g_limit_slider, COL_W - 40);
  lv_obj_add_event_cb(g_limit_slider, limit_cb, LV_EVENT_VALUE_CHANGED, NULL);

  /* --- scheduled meal time: the pet is fed only when RECOGNIZED at the bowl
   *     AND this meal is due (and not already eaten today). --- */
  g_meal_lbl = make_label(det, F_BODY, COL_TEXT);
  { lv_obj_t *mrow = lv_obj_create(det);
    lv_obj_set_size(mrow, lv_pct(100), 42);
    lv_obj_set_style_bg_opa(mrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mrow, 0, 0);
    lv_obj_set_style_pad_all(mrow, 0, 0);
    lv_obj_clear_flag(mrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(mrow, LV_SYMBOL_REFRESH, 76, COL_ACCENT, EV_MEAL_SEL); /* pick meal */
    make_btn(mrow, "-1h", 86, COL_BTN, EV_MEAL_HDN);
    make_btn(mrow, "+1h", 86, COL_BTN, EV_MEAL_HUP);
    make_btn(mrow, "-5m", 86, COL_BTN, EV_MEAL_MDN);
    make_btn(mrow, "+5m", 86, COL_BTN, EV_MEAL_MUP); }

  /* --- current time: no hardware RTC on this board, so set it once here --- */
  g_clock_lbl = make_label(det, F_BODY, COL_DIM);
  { lv_obj_t *crow = lv_obj_create(det);
    lv_obj_set_size(crow, lv_pct(100), 42);
    lv_obj_set_style_bg_opa(crow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(crow, 0, 0);
    lv_obj_set_style_pad_all(crow, 0, 0);
    lv_obj_clear_flag(crow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(crow, "-1h", 98, COL_CARD2, EV_CLK_HDN);
    make_btn(crow, "+1h", 98, COL_CARD2, EV_CLK_HUP);
    make_btn(crow, "-5m", 98, COL_CARD2, EV_CLK_MDN);
    make_btn(crow, "+5m", 98, COL_CARD2, EV_CLK_MUP); }

  /* actions */
  lv_obj_t *arow = lv_obj_create(tab);
  lv_obj_set_size(arow, COL_W, 48);
  lv_obj_set_style_bg_opa(arow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(arow, 0, 0);
  lv_obj_set_style_pad_all(arow, 0, 0);
  lv_obj_clear_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *sv = make_btn(arow, T(S_SAVE_PET), COL_W - 8,
                          COL_ACCENT, EV_SAVE);
  lv_obj_center(sv);
}

/****************************************************************************
 * recognize
 ****************************************************************************/

static void build_recognize(lv_obj_t *tab)
{
  lv_obj_set_style_pad_all(tab, 8, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tab, 6, 0);

  lv_obj_t *cam = make_card(tab, COL_W, 172, COL_CARD);
  g_img_r = lv_image_create(cam);
  lv_image_set_src(g_img_r, &g_dsc);
  lv_image_set_scale_x(g_img_r, IMG_SCALE_X);
  lv_image_set_scale_y(g_img_r, IMG_SCALE_Y);
  lv_obj_center(g_img_r);

  g_status = make_label(tab, F_TITLE, COL_TEXT);
  lv_label_set_text(g_status, T(S_POINT_AT_PET));
  g_sub = make_label(tab, F_BODY, COL_DIM);
  g_count = make_label(tab, F_BODY, COL_FED);
#ifdef CONFIG_VELAPAW_VOICE
  /* Above the metrics line, not below it: this one is an instruction to the
   * owner, the metrics line is diagnostics.  Left empty until the worker says
   * the microphone actually works -- promising "say yes" on a board where the
   * mic failed to open would be worse than saying nothing. */
  g_voice_lbl = make_label(tab, F_SMALL, COL_COOL);
  g_voice_shown = 0;    /* a language switch rebuilds this label empty */
#endif
  g_metrics = make_label(tab, F_SMALL, COL_DIM);
}

/****************************************************************************
 * my pets
 ****************************************************************************/

/* red bell when any pet shows a sudden appetite drop */
static void update_bell(void)
{
  int alert = 0;
  int n = velapaw_identity_count();
  for (int i = 0; i < n; i++)
    {
      const struct velapaw_pet *p = velapaw_identity_get(i);
      if (p && pet_alert_drop(i, p))
        {
          alert = 1;
          break;
        }
    }
  if (g_bell)
    {
      lv_obj_set_style_text_color(g_bell,
                                  lv_color_hex(alert ? COL_UNK : COL_DIM), 0);
    }
}

/****************************************************************************
 * edit an enrolled pet  (schedule / portion / limit -- never the embedding)
 ****************************************************************************/

static void refresh_pets(void);

/* Which meal-time array the +/- buttons act on: the edit dialog's scratch copy
 * while it is open, otherwise the Enroll tab's defaults. */
static int *meal_target(void)
{
  return (g_edit_pet >= 0) ? g_edit_meal : g_meal_min;
}

/* Put the Delete button back to its safe, unarmed state. */
static void del_disarm(void)
{
  g_del_armed = 0;
  if (g_del_btn)
    {
      lv_label_set_text(lv_obj_get_child(g_del_btn, 0), T(S_DELETE));
      lv_obj_set_style_bg_color(g_del_btn, lv_color_hex(COL_UNK), 0);
    }
}

static void refresh_edit(void)
{
  if (g_edit_modal == NULL || g_edit_pet < 0)
    {
      return;
    }

  char buf[64]; int n = 0;
  n += snprintf(buf + n, sizeof(buf) - n, "%s", T(S_MEALS));
  for (int k = 0; k < VELAPAW_MAX_MEALS && n < (int)sizeof(buf) - 12; k++)
    {
      n += snprintf(buf + n, sizeof(buf) - n,
                    (k == g_meal_sel) ? " [%02d:%02d]" : " %02d:%02d",
                    g_edit_meal[k] / 60, g_edit_meal[k] % 60);
    }
  lv_label_set_text(g_edit_meal_lbl, buf);

  lv_label_set_text_fmt(g_edit_portion_lbl, T(S_PORTION_FMT),
                        (int)lv_slider_get_value(g_edit_portion_sl));
  lv_label_set_text_fmt(g_edit_limit_lbl, T(S_DAILY_LIMIT_FMT),
                        (int)lv_slider_get_value(g_edit_limit_sl));
}

static void edit_open(velapaw_pet_id_t id)
{
  const struct velapaw_pet *p = velapaw_identity_get(id);
  if (p == NULL || g_edit_modal == NULL)
    {
      return;
    }

  g_edit_pet = id;
  g_meal_sel = 0;
  for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
    {
      g_edit_meal[k] = p->meal_min[k];
    }

  lv_slider_set_value(g_edit_portion_sl, p->portion_g, LV_ANIM_OFF);
  lv_slider_set_value(g_edit_limit_sl, p->daily_limit_g, LV_ANIM_OFF);
  lv_label_set_text_fmt(g_edit_title, T(S_EDIT_TITLE_FMT), p->name);
  lv_obj_clear_flag(g_edit_modal, LV_OBJ_FLAG_HIDDEN);
  del_disarm();                 /* never open the dialog already armed */

#ifdef CONFIG_VELAPAW_VOICE
  /* Blank, not the last pet's result -- "meal 2 at 08:00" left over from the
   * previous animal would read as a statement about this one. */

  if (g_vc_lbl != NULL)
    {
      lv_label_set_text(g_vc_lbl, "");
    }
#endif

  refresh_edit();
}

static void edit_close(void)
{
  g_edit_pet = -1;
  del_disarm();

#ifdef CONFIG_VELAPAW_VOICE
  /* A dialog left running against a closed modal would keep the microphone
   * open and, worse, would eventually write a meal time into a scratch array
   * nothing is looking at any more.  Closing the screen ends the conversation
   * -- the same thing walking away from a person does. */

  g_vs.state = VS_IDLE;
#endif

  if (g_edit_modal)
    {
      lv_obj_add_flag(g_edit_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void edit_save(void)
{
  if (g_edit_pet < 0)
    {
      return;
    }

  int id      = g_edit_pet;
  int portion = (int)lv_slider_get_value(g_edit_portion_sl);
  int limit   = (int)lv_slider_get_value(g_edit_limit_sl);

  velapaw_identity_update(id, portion, limit, g_edit_meal);
  velapaw_identity_save();          /* persist: the new times survive a reset */

  printf("velapaw/ui: pet %d updated - %dg, limit %dg, meals %02d:%02d "
         "%02d:%02d %02d:%02d\n", id, portion, limit,
         g_edit_meal[0] / 60, g_edit_meal[0] % 60,
         g_edit_meal[1] / 60, g_edit_meal[1] % 60,
         g_edit_meal[2] / 60, g_edit_meal[2] % 60);

  edit_close();
  refresh_pets();
}

static void edit_portion_cb(lv_event_t *e)
{
  (void)e;
  refresh_edit();
}

/* Delete a pet, keeping every per-pet table in step.
 *
 * Pet ids ARE array indices, so velapaw_identity_delete() renumbers every pet
 * after the deleted one. Anything keyed by id must shift the same way or the
 * data ends up on the wrong animal: the feeding history (store), the meal
 * timeline tables here, and any id we're currently holding. */
static void do_delete_pet(velapaw_pet_id_t id)
{
  int n = velapaw_identity_count();
  int p, k;

  if (id < 0 || id >= n)
    {
      return;
    }

  /* shift this file's per-pet tables down over the deleted slot */
  for (p = id; p < VELAPAW_MAX_PETS - 1; p++)
    {
      for (k = 0; k < VELAPAW_MAX_MEALS; k++)
        {
          g_meal_fed_day[p][k] = g_meal_fed_day[p + 1][k];
          g_meal_fed_min[p][k] = g_meal_fed_min[p + 1][k];
          g_meal_fed_g[p][k]   = g_meal_fed_g[p + 1][k];
          g_meal_skipped[p][k] = g_meal_skipped[p + 1][k];
        }
      g_last_fed[p] = g_last_fed[p + 1];
    }
  for (k = 0; k < VELAPAW_MAX_MEALS; k++)
    {
      g_meal_fed_day[VELAPAW_MAX_PETS - 1][k] = 0;   /* 0 = never fed */
      g_meal_fed_min[VELAPAW_MAX_PETS - 1][k] = 0;
      g_meal_fed_g[VELAPAW_MAX_PETS - 1][k]   = 0;
      g_meal_skipped[VELAPAW_MAX_PETS - 1][k] = 0;
    }
  g_last_fed[VELAPAW_MAX_PETS - 1] = 0;

  velapaw_store_delete(id);            /* history + BCS, shifted to match */
  velapaw_identity_delete(id);         /* the pet itself + icon, renumbers rest */
  velapaw_identity_save();             /* persist: stays deleted after reset */
  velapaw_identity_save_icons();       /* icons were shifted too */

  /* every id we were holding may now point at a different pet */
  g_fed_visit = VELAPAW_PET_UNKNOWN;
  g_bcs_for   = VELAPAW_PET_UNKNOWN;
  g_cand      = VELAPAW_PET_UNKNOWN;
  g_stable    = 0;

  n = velapaw_identity_count();
  if (g_trend_pet >= n) g_trend_pet = (n > 0) ? n - 1 : 0;
  g_petn = n;

  printf("velapaw/ui: deleted pet %d (%d left)\n", (int)id, n);
}

static void edit_btn_cb(lv_event_t *e)
{
  edit_open((velapaw_pet_id_t)(intptr_t)lv_event_get_user_data(e));
}

/* tap a not-yet-eaten meal row on My Pets to skip/un-skip just that slot */
static void meal_skip_toggle_cb(lv_event_t *e)
{
  int enc  = (int)(intptr_t)lv_event_get_user_data(e);
  int pet  = enc / VELAPAW_MAX_MEALS;
  int slot = enc % VELAPAW_MAX_MEALS;
  g_meal_skipped[pet][slot] = !g_meal_skipped[pet][slot];
  refresh_pets();
}

static void refresh_pets(void)
{
  if (g_pets_box == NULL)
    {
      return;
    }
  lv_obj_clean(g_pets_box);
  int n = velapaw_identity_count();
  if (n == 0)
    {
      lv_obj_t *l = make_label(g_pets_box, F_BODY, COL_DIM);
      lv_label_set_text(l, T(S_NO_PETS));
      update_bell();
      return;
    }
  for (int i = 0; i < n; i++)
    {
      const struct velapaw_pet *p = velapaw_identity_get(i);
      int today = velapaw_store_today_total(i);
      int meals = velapaw_store_today_meals(i);
      int avg   = velapaw_store_baseline_g(i);
      int week  = velapaw_store_week_total(i);
      int pct   = pet_appetite_pct(i, p);
      int alert = pet_alert_drop(i, p);

      lv_obj_t *row = make_card(g_pets_box, COL_W - 8, LV_SIZE_CONTENT, COL_CARD2);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(row, 2, 0);

      /* header: [icon | name]. Icon is the enrolled thumbnail, or a coloured
       * disc with the pet's initial if this pet predates the icon feature. */
      lv_obj_t *hdr = lv_obj_create(row);
      lv_obj_set_size(hdr, lv_pct(100), 52);
      lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(hdr, 0, 0);
      lv_obj_set_style_pad_all(hdr, 0, 0);
      lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(hdr, 12, 0);

      const uint16_t *ic = velapaw_identity_get_icon(i);
      if (ic)
        {
          g_petcard_dsc[i].header.cf   = LV_COLOR_FORMAT_RGB565;
          g_petcard_dsc[i].header.w    = VELAPAW_ICON_W;
          g_petcard_dsc[i].header.h    = VELAPAW_ICON_H;
          g_petcard_dsc[i].data_size   = VELAPAW_ICON_W * VELAPAW_ICON_H * 2;
          g_petcard_dsc[i].data        = (const uint8_t *)ic;
          /* circular frame that clips the photo to a disc, matching the "K"
           * placeholder's shape */
          lv_obj_t *frame = lv_obj_create(hdr);
          lv_obj_set_size(frame, VELAPAW_ICON_W, VELAPAW_ICON_H);
          lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
          lv_obj_set_style_clip_corner(frame, true, 0);
          lv_obj_set_style_border_width(frame, 0, 0);
          lv_obj_set_style_pad_all(frame, 0, 0);
          lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_t *img = lv_image_create(frame);
          lv_image_set_src(img, &g_petcard_dsc[i]);
          lv_obj_center(img);
        }
      else
        {
          lv_obj_t *disc = lv_obj_create(hdr);
          lv_obj_set_size(disc, VELAPAW_ICON_W, VELAPAW_ICON_H);
          lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
          lv_obj_set_style_bg_color(disc, lv_color_hex(COL_BTN), 0);
          lv_obj_set_style_border_width(disc, 0, 0);
          lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_t *ini = lv_label_create(disc);
          char c[2] = { p->name[0] ? p->name[0] : '?', 0 };
          lv_label_set_text(ini, c);
          lv_obj_set_style_text_font(ini, F_HEAD, 0);
          lv_obj_set_style_text_color(ini, lv_color_hex(COL_ACCENT), 0);
          lv_obj_center(ini);
        }

      lv_obj_t *nm = make_label(hdr, F_HEAD, COL_TEXT);
      lv_label_set_text(nm, p->name);

      lv_obj_t *l1 = make_label(row, F_SMALL,
                                (today >= p->daily_limit_g) ? COL_FED : COL_DIM);
      lv_label_set_text_fmt(l1, T(S_TODAY_FMT),
                            today, p->daily_limit_g, meals);
      if (today >= p->daily_limit_g)
        {
          lv_obj_t *lim = make_label(row, F_SMALL, COL_FED);
          lv_label_set_text(lim, T(S_LIMIT_REACHED));
        }

      /* ---- meal timeline: what was scheduled vs what actually happened ----
       * "08:00  ate 08:02  15g"  = eaten (recognised at/after the meal)
       * "12:30  waiting for pet" = meal is due but the pet hasn't shown up
       * "18:00  pending"         = still ahead today
       * "18:00  skipped"         = owner tapped it off; won't dispense today
       * Not-yet-eaten rows are tappable to toggle skipped, e.g. skip meal 2
       * but still feed 1 and 3. Resets automatically at the next Next Day. */
      { long today_d = wall_day();
        int  nowm    = wall_min();
        int  any     = 0;
        for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
          {
            int mm = p->meal_min[k];
            if (mm < 0) continue;
            any = 1;
            lv_obj_t *ml = make_label(row, F_SMALL, COL_DIM);
            if (g_meal_fed_day[i][k] == today_d)
              {
                lv_obj_set_style_text_color(ml, lv_color_hex(COL_FED), 0);
                lv_label_set_text_fmt(ml, T(S_ATE_FMT),
                    mm / 60, mm % 60,
                    g_meal_fed_min[i][k] / 60, g_meal_fed_min[i][k] % 60,
                    g_meal_fed_g[i][k]);
              }
            else
              {
                lv_obj_add_flag(ml, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_ext_click_area(ml, 10);
                lv_obj_add_event_cb(ml, meal_skip_toggle_cb, LV_EVENT_CLICKED,
                    (void *)(intptr_t)(i * VELAPAW_MAX_MEALS + k));
                if (g_meal_skipped[i][k])
                  {
                    lv_label_set_text_fmt(ml, T(S_SKIPPED), mm / 60, mm % 60);
                  }
                else if (nowm >= mm)
                  {
                    lv_obj_set_style_text_color(ml, lv_color_hex(COL_COOL), 0);
                    lv_label_set_text_fmt(ml, T(S_WAITING), mm / 60, mm % 60);
                  }
                else
                  {
                    lv_label_set_text_fmt(ml, T(S_PENDING), mm / 60, mm % 60);
                  }
              }
          }
        if (!any)
          {
            lv_obj_t *ml = make_label(row, F_SMALL, COL_DIM);
            lv_label_set_text(ml, T(S_NO_MEALS_SCHED));
          } }

      lv_obj_t *l2 = make_label(row, F_SMALL, COL_DIM);
      lv_label_set_text_fmt(l2, T(S_AVG_WEEK_FMT), avg, week);

      uint32_t pcol = (pct >= 70) ? COL_FED : (pct >= 40) ? COL_COOL : COL_UNK;
      lv_obj_t *l3 = make_label(row, F_BODY, pcol);
      if (alert)
        {
          lv_label_set_text_fmt(l3, T(S_APPETITE_DROP_FMT), pct);
        }
      else
        {
          lv_label_set_text_fmt(l3, T(S_APPETITE_FMT), pct);
        }

      /* Body Condition Score from the 2nd on-device model */
      int bcs = velapaw_store_get_bcs(i);
      const char *bnm = (bcs == 0) ? T(S_BCS_UNDER) : (bcs == 1) ? T(S_BCS_IDEAL)
                        : (bcs == 2) ? T(S_BCS_OVER) : T(S_BCS_NA);
      uint32_t bcol = (bcs == 1) ? COL_FED : (bcs < 0) ? COL_DIM : COL_COOL;
      lv_obj_t *l4 = make_label(row, F_BODY, bcol);
      if (bcs == 0 || bcs == 2)   /* under/over: nudge to a vet */
        {
          lv_label_set_text_fmt(l4, T(S_BCS_VET_FMT), bnm);
        }
      else
        {
          lv_label_set_text_fmt(l4, T(S_BCS_FMT), bnm);
        }

      /* 7-day intake sparkline (oldest -> today) */
      lv_obj_t *spark = lv_obj_create(row);
      lv_obj_set_size(spark, COL_W - 60, 36);
      lv_obj_set_style_bg_opa(spark, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(spark, 0, 0);
      lv_obj_set_style_pad_all(spark, 0, 0);
      lv_obj_clear_flag(spark, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(spark, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(spark, LV_FLEX_ALIGN_SPACE_EVENLY,
                            LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
      int maxg = 1;
      for (int d = 0; d < VELAPAW_WEEK_DAYS; d++)
        {
          int g = velapaw_store_day_total(i, d);
          if (g > maxg)
            {
              maxg = g;
            }
        }
      for (int d = VELAPAW_WEEK_DAYS - 1; d >= 0; d--)
        {
          int g = velapaw_store_day_total(i, d);
          lv_obj_t *bar = lv_obj_create(spark);
          lv_obj_set_size(bar, 12, 4 + g * 28 / maxg);
          lv_obj_set_style_radius(bar, 2, 0);
          lv_obj_set_style_border_width(bar, 0, 0);
          lv_obj_set_style_pad_all(bar, 0, 0);
          lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_set_style_bg_color(bar,
              lv_color_hex(d == 0 ? COL_ACCENT : COL_BTN), 0);
        }

      /* Change this pet's meal times / portion without re-enrolling it. */
      lv_obj_t *eb = lv_button_create(row);
      lv_obj_set_size(eb, 180, 34);
      lv_obj_set_style_bg_color(eb, lv_color_hex(COL_BTN), 0);
      lv_obj_set_style_radius(eb, 10, 0);
      lv_obj_add_event_cb(eb, edit_btn_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      lv_obj_t *ebl = lv_label_create(eb);
      lv_label_set_text(ebl, T(S_EDIT_SCHEDULE));
      lv_obj_set_style_text_font(ebl, F_SMALL, 0);
      lv_obj_center(ebl);
    }
  update_bell();
}

static void build_pets(lv_obj_t *tab)
{
  lv_obj_set_style_pad_all(tab, 8, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tab, 6, 0);

  lv_obj_t *t = make_label(tab, F_TITLE, COL_ACCENT);
  lv_label_set_text(t, T(S_ENROLLED_PETS));

  g_pets_box = lv_obj_create(tab);
  lv_obj_set_width(g_pets_box, COL_W);
  lv_obj_set_height(g_pets_box, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(g_pets_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_pets_box, 0, 0);
  lv_obj_set_style_pad_all(g_pets_box, 0, 0);
  lv_obj_set_flex_flow(g_pets_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_pets_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(g_pets_box, 6, 0);
  lv_obj_clear_flag(g_pets_box, LV_OBJ_FLAG_SCROLLABLE);
  refresh_pets();

  /* health disclaimer banner */
  lv_obj_t *dcard = make_card(tab, COL_W, LV_SIZE_CONTENT, COL_CARD2);
  lv_obj_set_style_border_width(dcard, 2, 0);
  lv_obj_set_style_border_color(dcard, lv_color_hex(COL_COOL), 0);
  lv_obj_t *disc = make_label(dcard, F_SMALL, COL_COOL);
  lv_label_set_long_mode(disc, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(disc, COL_W - 30);
  lv_obj_set_style_text_align(disc, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(disc, T(S_BCS_DISCLAIMER));

  make_btn(tab, T(S_NEXT_DAY), 240, COL_BTN, EV_NEXTDAY);
}

#ifdef CONFIG_VELAPAW_VOICE

/****************************************************************************
 * microphone glyph
 *
 * A one-glyph LVGL font, because no font this build ships has a microphone in
 * it: LV_SYMBOL_* has none, lv_font_montserrat_16's FontAwesome codepoint list
 * skips f130/f131, and the CJK subsets carry no icon range at all (which is
 * why ui_i18n.c falls them back to Montserrat).  The voice button used
 * LV_SYMBOL_AUDIO -- a music note -- for want of anything better.
 *
 * 11x14 box, 4 bpp, drawn by scratchpad/mkmic.py.  Sits at 0xF130, FontAwesome's
 * microphone slot, so the codepoint reads conventionally even though the bitmap
 * is ours.  Applied to this one label only; every other icon still comes from
 * Montserrat.
 ****************************************************************************/

#define VP_ICON_MIC "\xEF\x84\xB0"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t mic_glyph_bitmap[] = {
    0x00, 0x00, 0x48, 0x40, 0x00, 0x00, 0x00, 0x5f, 0xff, 0x50, 0x00, 0x00,
    0x0a, 0xff, 0xfa, 0x00, 0x00, 0x00, 0xbf, 0xff, 0xb0, 0x00, 0x00, 0x0b,
    0xff, 0xfb, 0x00, 0x00, 0x00, 0xbf, 0xff, 0xb0, 0x00, 0x6c, 0x0a, 0xff,
    0xfa, 0x0c, 0x62, 0xf2, 0x5f, 0xff, 0x52, 0xf2, 0x0b, 0xc1, 0x48, 0x41,
    0xcb, 0x00, 0x1c, 0xd7, 0x47, 0xdc, 0x10, 0x00, 0x17, 0xcf, 0xc7, 0x10,
    0x00, 0x00, 0x04, 0xf4, 0x00, 0x00, 0x00, 0x28, 0x9f, 0x98, 0x20, 0x00,
    0x02, 0x88, 0x88, 0x82, 0x00
};

static const lv_font_fmt_txt_glyph_dsc_t mic_glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0,   .box_w = 0,  .box_h = 0,
     .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 192, .box_w = 11, .box_h = 14,
     .ofs_x = 1, .ofs_y = 0}
};

static const lv_font_fmt_txt_cmap_t mic_cmaps[] =
{
    {
        .range_start = 0xF130, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

/* kern_dsc NULL / kern_classes 0 is what the generator emits for --no-kerning;
 * one glyph has nothing to kern against. */

static const lv_font_fmt_txt_dsc_t mic_font_dsc = {
    .glyph_bitmap  = mic_glyph_bitmap,
    .glyph_dsc     = mic_glyph_dsc,
    .cmaps         = mic_cmaps,
    .kern_dsc      = NULL,
    .kern_scale    = 0,
    .cmap_num      = 1,
    .bpp           = 4,
    .kern_classes  = 0,
    .bitmap_format = 0,
};

/* line_height 16 / base_line 1 puts the 14 px box in the middle of the line;
 * the button centers the label anyway, so this only has to avoid clipping. */

static const lv_font_t velapaw_font_mic = {
    .get_glyph_dsc       = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap    = lv_font_get_bitmap_fmt_txt,
    .line_height         = 16,
    .base_line           = 1,
    .subpx               = LV_FONT_SUBPX_NONE,
    .underline_position  = -2,
    .underline_thickness = 1,
    .dsc                 = &mic_font_dsc,
    .fallback            = NULL,
    .user_data           = NULL,
};

#endif /* CONFIG_VELAPAW_VOICE */

/* Modal editor, floating above the tabs (like the keyboard). Reuses the same
 * EV_MEAL_* buttons as Enroll -- they act on g_edit_meal while this is open. */
static void build_edit_modal(void)
{
  g_edit_modal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_edit_modal, COL_W, 320);
  lv_obj_center(g_edit_modal);
  lv_obj_set_style_bg_color(g_edit_modal, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_color(g_edit_modal, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_border_width(g_edit_modal, 2, 0);
  lv_obj_set_style_radius(g_edit_modal, 12, 0);
  lv_obj_set_style_pad_all(g_edit_modal, 10, 0);
  lv_obj_clear_flag(g_edit_modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(g_edit_modal, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_edit_modal, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  /* 5, not 6.  This modal is 320 px tall on a 320 px screen and is NOT
   * scrollable, so the column has to fit exactly: the voice row below adds
   * ~46 px to a stack that previously left ~57 px of slack, and if it ever
   * overflowed, the row that fell off the bottom would be Cancel/Delete/Save
   * -- an owner trapped in a modal with no way out.  One pixel per gap buys
   * back 8 px and costs nothing visible. */

  lv_obj_set_style_pad_row(g_edit_modal, 5, 0);

  g_edit_title = make_label(g_edit_modal, F_TITLE, COL_ACCENT);
  lv_label_set_text(g_edit_title, T(S_EDIT_PET));

  g_edit_meal_lbl = make_label(g_edit_modal, F_BODY, COL_TEXT);
  { lv_obj_t *mrow = lv_obj_create(g_edit_modal);
    lv_obj_set_size(mrow, lv_pct(100), 42);
    lv_obj_set_style_bg_opa(mrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mrow, 0, 0);
    lv_obj_set_style_pad_all(mrow, 0, 0);
    lv_obj_clear_flag(mrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(mrow, LV_SYMBOL_REFRESH, 76, COL_ACCENT, EV_MEAL_SEL);
    make_btn(mrow, "-1h", 86, COL_BTN, EV_MEAL_HDN);
    make_btn(mrow, "+1h", 86, COL_BTN, EV_MEAL_HUP);
    make_btn(mrow, "-5m", 86, COL_BTN, EV_MEAL_MDN);
    make_btn(mrow, "+5m", 86, COL_BTN, EV_MEAL_MUP); }

#ifdef CONFIG_VELAPAW_VOICE
  /* Voice entry point.  A button, not a wake word: "Hi VelaPaw" is not in the
   * training vocabulary, and listening continuously would cost ~40% of the
   * one core this board has.  The owner is already here to change a schedule.
   *
   * The label below it is the whole conversation -- prompt, "say again",
   * result.  A 3.5" screen is a better feedback channel than a beep, and it
   * keeps working if the speaker path lags.
   *
   * Symbol-only in both languages: "语音" needs two glyphs the shipped font
   * subset does not carry, and an unshipped glyph renders blank silently. */

  { lv_obj_t *vrow = lv_obj_create(g_edit_modal);
    lv_obj_set_size(vrow, lv_pct(100), 40);   /* 38 px button + 1 px either side */
    lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vrow, 0, 0);
    lv_obj_set_style_pad_all(vrow, 0, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vrow, 10, 0);
    g_vc_btn = make_btn(vrow, VP_ICON_MIC, 76, COL_BTN, EV_MEAL_VOICE);

    /* make_btn() set the label to F_HEAD, which resolves to Montserrat (or
     * to the CJK subset that falls back to it) -- neither has 0xF130.
     * Override the font on this one label; child 0 is the label make_btn
     * created and centered. */

    lv_obj_set_style_text_font(lv_obj_get_child(g_vc_btn, 0),
                               &velapaw_font_mic, 0);
    g_vc_lbl = make_label(vrow, F_SMALL, COL_DIM);

    /* Fixed width and DOT, not WRAP: a wrapped second line would grow the row
     * inside a column that has no spare height (see pad_row above), and this
     * label carries translated text whose length is not under our control. */

    lv_obj_set_width(g_vc_lbl, 340);
    lv_label_set_long_mode(g_vc_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_vc_lbl, ""); }
#endif

  g_edit_portion_lbl = make_label(g_edit_modal, F_BODY, COL_TEXT);
  lv_label_set_text_fmt(g_edit_portion_lbl, T(S_PORTION_FMT), 15);
  g_edit_portion_sl = lv_slider_create(g_edit_modal);
  lv_slider_set_range(g_edit_portion_sl, 5, 50);
  lv_obj_set_width(g_edit_portion_sl, COL_W - 50);
  lv_obj_add_event_cb(g_edit_portion_sl, edit_portion_cb,
                      LV_EVENT_VALUE_CHANGED, NULL);

  g_edit_limit_lbl = make_label(g_edit_modal, F_BODY, COL_TEXT);
  lv_label_set_text_fmt(g_edit_limit_lbl, T(S_DAILY_LIMIT_FMT), 60);
  g_edit_limit_sl = lv_slider_create(g_edit_modal);
  lv_slider_set_range(g_edit_limit_sl, 20, 300);
  lv_obj_set_width(g_edit_limit_sl, COL_W - 50);
  lv_obj_add_event_cb(g_edit_limit_sl, edit_portion_cb,
                      LV_EVENT_VALUE_CHANGED, NULL);

  { lv_obj_t *arow = lv_obj_create(g_edit_modal);
    lv_obj_set_size(arow, lv_pct(100), 48);
    lv_obj_set_style_bg_opa(arow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arow, 0, 0);
    lv_obj_set_style_pad_all(arow, 0, 0);
    lv_obj_clear_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(arow, T(S_CANCEL), 110, COL_BTN, EV_EDIT_CANCEL);
    g_del_btn = make_btn(arow, T(S_DELETE), 150, COL_UNK,
                         EV_EDIT_DELETE);
    make_btn(arow, T(S_SAVE), 130, COL_ACCENT, EV_EDIT_SAVE); }

  lv_obj_add_flag(g_edit_modal, LV_OBJ_FLAG_HIDDEN);
}

/****************************************************************************
 * trends (interactive chart)
 ****************************************************************************/

static void chart_press_cb(lv_event_t *e)
{
  (void)e;
  int32_t id = lv_chart_get_pressed_point(g_trend_chart);
  if (id == LV_CHART_POINT_NONE)
    {
      return;
    }
  int n = g_trend_month ? VELAPAW_HIST_DAYS : VELAPAW_WEEK_DAYS;
  int day_back = (n - 1) - (int)id;
  if (day_back == 0)
    {
      lv_label_set_text_fmt(g_trend_val, T(S_TODAY_VAL_FMT), (int)g_trend_vals[id]);
    }
  else if (day_back == 1)
    {
      lv_label_set_text_fmt(g_trend_val, T(S_YDAY_VAL_FMT),
                            (int)g_trend_vals[id]);
    }
  else
    {
      lv_label_set_text_fmt(g_trend_val, T(S_NDAYS_VAL_FMT), day_back,
                            (int)g_trend_vals[id]);
    }
}

static void refresh_trends(void)
{
  if (g_trend_chart == NULL)
    {
      return;
    }
  int cnt = velapaw_identity_count();
  if (cnt == 0)
    {
      lv_label_set_text(g_trend_title, T(S_TRENDS_ENROLL_FIRST));
      lv_label_set_text(g_trend_stats, "");
      lv_label_set_text(g_trend_val, "");
      return;
    }
  if (g_trend_pet >= cnt)
    {
      g_trend_pet = 0;
    }
  const struct velapaw_pet *pet = velapaw_identity_get(g_trend_pet);
  int n = g_trend_month ? VELAPAW_HIST_DAYS : VELAPAW_WEEK_DAYS;
  lv_chart_set_point_count(g_trend_chart, n);
  int maxg = 1;
  long sum = 0;
  for (int i = 0; i < n; i++)
    {
      int db = (n - 1) - i;              /* i=0 oldest ... i=n-1 today */
      int v = velapaw_store_day_total(g_trend_pet, db);
      g_trend_vals[i] = v;
      sum += v;
      if (v > maxg)
        {
          maxg = v;
        }
    }
  /* Average over COMPLETED days only -- exclude today (the last index), which is
   * still filling (often 0 g so far) and would drag the mean below every past
   * day, making them all read above-average (all green). */
  { long csum = sum - g_trend_vals[n - 1];
    g_trend_avg = (n > 1) ? (int32_t)(csum / (n - 1)) : g_trend_vals[0]; }
  lv_chart_set_range(g_trend_chart, LV_CHART_AXIS_PRIMARY_Y, 0, maxg);
  for (int i = 0; i < n; i++)
    {
      lv_chart_set_value_by_id(g_trend_chart, g_trend_ser, i, g_trend_vals[i]);
    }
  lv_chart_refresh(g_trend_chart);

  /* appetite line: each day's intake as a % of the pet's daily allowance */
  if (g_appet_chart)
    {
      lv_chart_set_point_count(g_appet_chart, n);
      for (int i = 0; i < n; i++)
        {
          int pct = (pet->daily_limit_g > 0)
                    ? (int)(g_trend_vals[i] * 100 / pet->daily_limit_g) : 0;
          lv_chart_set_value_by_id(g_appet_chart, g_appet_ser, i,
                                   pct > 100 ? 100 : pct);
        }
      lv_chart_refresh(g_appet_chart);
    }

  lv_label_set_text_fmt(g_trend_title, T(S_TREND_TITLE_FMT), pet->name,
                        g_trend_month ? T(S_WIN_30D) : T(S_WIN_7D));
  lv_label_set_text_fmt(g_trend_stats, T(S_TREND_STATS_FMT),
      velapaw_store_today_total(g_trend_pet), pet->daily_limit_g,
      velapaw_store_baseline_g(g_trend_pet),
      velapaw_store_week_total(g_trend_pet),
      velapaw_store_month_total(g_trend_pet),
      pet_appetite_pct(g_trend_pet, pet));
  lv_label_set_text(g_trend_val, T(S_TAP_BAR));

  /* highlight the active window toggle */
  if (g_btn_week && g_btn_month)
    {
      lv_obj_set_style_bg_color(g_btn_week,
          lv_color_hex(g_trend_month ? COL_BTN : COL_ACCENT), 0);
      lv_obj_set_style_bg_color(g_btn_month,
          lv_color_hex(g_trend_month ? COL_ACCENT : COL_BTN), 0);
    }

  refresh_bcs_gauge();      /* update the dial for the current pet */
}

/* Recolour each intake bar by whether that day was at/above the window average:
 * green = a good (>=avg) day, blue = below-average. The colour split IS the
 * baseline, so no separate average line is needed. LVGL 9 draw-task hook. */
static void trend_bar_color_cb(lv_event_t *e)
{
  lv_draw_task_t *task = lv_event_get_draw_task(e);
  /* LVGL 9.1 has no lv_draw_task_get_draw_dsc() (that's 9.2) -- read the base
   * descriptor from the task field, as LVGL 9.1's own chart example does. */
  lv_draw_dsc_base_t *base = task->draw_dsc;
  if (base == NULL || base->part != LV_PART_ITEMS)
    {
      return;                            /* not one of the bars */
    }
  lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
  if (fill == NULL)
    {
      return;                            /* not a fill (rectangle) task */
    }
  int n = g_trend_month ? VELAPAW_HIST_DAYS : VELAPAW_WEEK_DAYS;
  uint32_t idx = base->id1;              /* point index (id2 is the series index) */
  if ((int)idx < n)
    {
      fill->color = lv_color_hex(g_trend_vals[idx] >= g_trend_avg
                                 ? COL_FED : COL_ACCENT);
    }
}

/* Appetite line chart: each day's intake as a % of the pet's daily allowance
 * (0-100) -- a distinct read from the intake bars ("how much of their ration
 * did they eat each day"). Refreshed alongside the bars in refresh_trends. */
static void build_appetite_chart(lv_obj_t *tab)
{
  lv_obj_t *card = make_card(tab, COL_W, 150, COL_CARD);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 4, 0);

  lv_obj_t *t = make_label(card, F_HEAD, COL_ACCENT);
  lv_label_set_text(t, T(S_APPETITE_TREND));

  g_appet_chart = lv_chart_create(card);
  lv_obj_set_size(g_appet_chart, COL_W - 24, 96);
  lv_obj_set_style_bg_opa(g_appet_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_appet_chart, 0, 0);
  lv_obj_set_style_pad_all(g_appet_chart, 4, 0);
  lv_chart_set_type(g_appet_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_range(g_appet_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(g_appet_chart, 3, 0);
  lv_obj_set_style_line_width(g_appet_chart, 3, LV_PART_ITEMS);
  g_appet_ser = lv_chart_add_series(g_appet_chart, lv_color_hex(COL_ACCENT),
                                    LV_CHART_AXIS_PRIMARY_Y);
}

/* ---- BCS gauge: a 3-zone semicircular dial (Trends tab) --------------------
 * BCS is a 3-class score (0 under / 1 ideal / 2 over / -1 none). Three coloured
 * lv_arc bands over the top semicircle (amber | green | amber-red) with an
 * lv_line needle pointing at the current class. Uses only confirmed LVGL 9.1
 * widgets (arc + line) -- no lv_scale sections (their setter API isn't in 9.1).
 * Angles: LVGL arc 0 deg = 3 o'clock, clockwise, so 180->360 is the top half. */
#define GA_D    150                     /* gauge diameter (square) */
#define GA_C    (GA_D / 2)              /* pivot / centre (local)  */
#define GA_L    62                      /* needle length           */

static lv_obj_t *bcs_zone_arc(lv_obj_t *p, int a0, int a1, uint32_t col)
{
  lv_obj_t *a = lv_arc_create(p);
  lv_obj_set_size(a, GA_D, GA_D);
  lv_obj_center(a);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_bg_angles(a, a0, a1);
  lv_arc_set_angles(a, a0, a0);                       /* no indicator fill */
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);       /* no widget rectangle */
  lv_obj_set_style_arc_color(a, lv_color_hex(col), LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
  return a;
}

static void refresh_bcs_gauge(void)
{
  if (g_bcs_needle == NULL)
    {
      return;
    }
  int bcs = velapaw_store_get_bcs(g_trend_pet);
  int dx, dy; const char *nm; uint32_t lblcol; uint32_t ndlcol = 0xFFFFFF; /* white */
  switch (bcs)
    {
      case 0:  dx = -54; dy = -31;   nm = T(S_BCS_UNDER); lblcol = COL_COOL; break; /* 210 deg */
      case 1:  dx =   0; dy = -GA_L; nm = T(S_BCS_IDEAL); lblcol = COL_FED;  break; /* 270, up */
      case 2:  dx =  54; dy = -31;   nm = T(S_BCS_OVER);  lblcol = COL_UNK;  break; /* 330 deg */
      default: dx =   0; dy = -36;   nm = T(S_BCS_NA);    lblcol = COL_DIM;
               ndlcol = COL_DIM; break;                                 /* no reading: grey */
    }
  g_bcs_pts[0].x = GA_C;      g_bcs_pts[0].y = GA_C;
  g_bcs_pts[1].x = GA_C + dx; g_bcs_pts[1].y = GA_C + dy;
  lv_line_set_points(g_bcs_needle, g_bcs_pts, 2);
  lv_obj_set_style_line_color(g_bcs_needle, lv_color_hex(ndlcol), 0);
  lv_label_set_text(g_bcs_lbl, nm);
  lv_obj_set_style_text_color(g_bcs_lbl, lv_color_hex(lblcol), 0);
}

static void build_bcs_gauge(lv_obj_t *tab)
{
  lv_obj_t *card = make_card(tab, COL_W, 185, COL_CARD);
  lv_obj_t *t = make_label(card, F_HEAD, COL_ACCENT);
  lv_label_set_text(t, T(S_BCS_TITLE));
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 2, 0);

  lv_obj_t *g = lv_obj_create(card);
  lv_obj_set_size(g, GA_D, GA_D);
  lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g, 0, 0);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);

  bcs_zone_arc(g, 180, 240, COL_COOL);   /* under: left,  amber */
  bcs_zone_arc(g, 240, 300, COL_FED);    /* ideal: top,   green */
  bcs_zone_arc(g, 300, 360, COL_UNK);    /* over:  right, red   */

  g_bcs_needle = lv_line_create(g);
  lv_obj_set_style_line_width(g_bcs_needle, 7, 0);
  lv_obj_set_style_line_rounded(g_bcs_needle, true, 0);

  lv_obj_t *hub = lv_obj_create(g);        /* centre pivot cap (drawn over needle) */
  lv_obj_set_size(hub, 20, 20);
  lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hub, lv_color_hex(COL_CARD2), 0);
  lv_obj_set_style_border_color(hub, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_width(hub, 2, 0);
  lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);

  g_bcs_lbl = make_label(g, F_TITLE, COL_TEXT);
  lv_obj_align(g_bcs_lbl, LV_ALIGN_CENTER, 0, 34);

  refresh_bcs_gauge();
}

static void build_trends(lv_obj_t *tab)
{
  lv_obj_set_style_pad_all(tab, 8, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tab, 6, 0);

  g_trend_title = make_label(tab, F_TITLE, COL_ACCENT);
  lv_label_set_text(g_trend_title, T(S_TRENDS));

  lv_obj_t *ctl = lv_obj_create(tab);
  lv_obj_set_size(ctl, COL_W, 42);
  lv_obj_set_style_bg_opa(ctl, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ctl, 0, 0);
  lv_obj_set_style_pad_all(ctl, 0, 0);
  lv_obj_clear_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *bprev = make_btn(ctl, T(S_PET_PREV), 100, COL_BTN,
                             EV_TREND_PREV);
  lv_obj_align(bprev, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *bnext = make_btn(ctl, T(S_PET_NEXT), 100, COL_BTN,
                             EV_TREND_NEXT);
  lv_obj_align(bnext, LV_ALIGN_LEFT_MID, 108, 0);
  g_btn_week = make_btn(ctl, T(S_WIN_7D), 64, COL_BTN, EV_TREND_WEEK);
  lv_obj_align(g_btn_week, LV_ALIGN_RIGHT_MID, -70, 0);
  g_btn_month = make_btn(ctl, T(S_WIN_30D), 64, COL_BTN, EV_TREND_MONTH);
  lv_obj_align(g_btn_month, LV_ALIGN_RIGHT_MID, 0, 0);

  g_trend_chart = lv_chart_create(tab);
  lv_obj_set_size(g_trend_chart, COL_W, 130);
  lv_obj_set_style_bg_color(g_trend_chart, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_width(g_trend_chart, 0, 0);
  lv_obj_set_style_radius(g_trend_chart, 10, 0);
  lv_obj_set_style_pad_all(g_trend_chart, 8, 0);
  lv_chart_set_type(g_trend_chart, LV_CHART_TYPE_BAR);
  lv_chart_set_div_line_count(g_trend_chart, 4, 0);
  g_trend_ser = lv_chart_add_series(g_trend_chart, lv_color_hex(COL_ACCENT),
                                    LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_flag(g_trend_chart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_trend_chart, chart_press_cb, LV_EVENT_PRESSING, NULL);
  /* per-bar colouring (green >= window average, blue below) */
  lv_obj_add_flag(g_trend_chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
  lv_obj_add_event_cb(g_trend_chart, trend_bar_color_cb,
                      LV_EVENT_DRAW_TASK_ADDED, NULL);

  g_trend_val = make_label(tab, F_BODY, COL_FED);

  g_trend_stats = make_label(tab, F_SMALL, COL_TEXT);
  lv_label_set_long_mode(g_trend_stats, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_trend_stats, COL_W);
  lv_obj_set_style_text_align(g_trend_stats, LV_TEXT_ALIGN_CENTER, 0);

  build_appetite_chart(tab); /* appetite line chart, then the BCS dial */
  build_bcs_gauge(tab);     /* BCS dial under the stats (built before refresh) */
  refresh_trends();
}

/****************************************************************************
 * main
 ****************************************************************************/

/* True while the user is touching the screen (scrolling/pressing). We pause
 * the blocking camera capture during interaction so scrolling stays smooth. */
static bool ui_touch_active(void)
{
  lv_indev_t *indev = lv_indev_get_next(NULL);
  while (indev)
    {
      if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED)
        {
          return true;
        }
      indev = lv_indev_get_next(indev);
    }
  return false;
}

/* ---- background inference worker -----------------------------------------
 * Runs the ~1.3s embed()+match() OFF the UI thread so touch + LVGL stay live
 * while the model runs. Single producer (UI loop) / single consumer (worker),
 * coordinated by g_inf_state: 0 idle -> 1 request -> 2 busy -> 3 done -> 0.
 * The UI pauses camera preview capture while state != 0 so the shared frame
 * buffer the worker reads is never overwritten mid-inference.
 * (State lives near the top of the file -- Capture needs it too.) */

static void *recog_worker(void *arg)
{
  (void)arg;
  for (;;)
    {
      if (g_inf_state != 1) { usleep(5000); continue; }
      g_inf_state = 2;
      pthread_mutex_lock(&g_tflm_lock);      /* vs. the voice worker's Invoke */
      g_be->embed(&g_inf_frame, g_inf_emb, VELAPAW_EMBED_DIM);
      g_inf_us = g_be->last_latency_us();
      pthread_mutex_unlock(&g_tflm_lock);
      if (g_inf_mode == 0)
        {
          velapaw_identity_match(g_inf_emb, &g_inf_m);  /* recognize only */
        }
      g_inf_state = 3;
    }
  return NULL;
}

#ifdef CONFIG_VELAPAW_VOICE
/* ---- background voice worker ---------------------------------------------
 * Listens in 1 s windows while the UI says to, and posts at most one accepted
 * keyword at a time.  Structure mirrors the "velapaw kws mic" subcommand,
 * which is what proved this path on hardware; the differences are all
 * consequences of running forever instead of three times:
 *
 *   - the model is initialised HERE, on the first pass, not at startup.  It
 *     allocates two arenas and takes a moment, and doing it on the UI task
 *     would stall the first paint for no reason.
 *   - a failure is latched into g_voice_fail and the thread parks instead of
 *     retrying.  Every failure this path can produce (no mic, RX will not arm)
 *     is permanent until a power cycle, so a retry loop would only produce a
 *     console full of the same error.
 *   - silence is dropped before classification, not after.  micro_speech has
 *     no "nothing was said" answer -- fed an empty room it returns a confident
 *     wrong label -- so without this gate an idle kitchen would feed the cat.
 *
 * One capture costs ~2 s of wall time: the driver hands over two 16-bit slots
 * per frame and mic.c keeps one (see voice/mic.h), so a 1 s window is 2 s of
 * stream.  That sets the command cadence, and it is why the UI shows a
 * standing prompt rather than pretending to react instantly. */

/* Offset of the loudest win-sample window inside n samples of audio.
 *
 * Plain sum of absolute amplitude, slid one sample at a time with a running
 * total, so the answer is exact rather than quantised to a step size and the
 * whole thing is one pass over 24000 int16s -- under a millisecond, against
 * the ~130 ms the frontend and the model already cost.
 *
 * Sum-of-abs rather than sum of squares because it cannot overflow here
 * (32767 * 16000 = 524M, inside uint32) and because the two rank windows
 * identically for this purpose: the point is only to find where the energy
 * is, not to measure it.
 *
 * Centring on the loudest second is the right rule for THIS vocabulary --
 * every word is a single short utterance surrounded by room noise, so the
 * maximum-energy window is the one that contains the whole word.  It would be
 * the wrong rule for continuous speech, where the loudest second is just the
 * loudest second.
 *
 * Compiled out while the capture is exactly one window wide, because at that
 * width it can only return 0 -- and because GCC says so in a way that is worth
 * not silencing.  Build 66 warned:
 *
 *   voice_align: array subscript 16000 is outside array bounds of int16_t[16000]
 *
 * That is the second loop's i = win start, seen through the inline into
 * voice_worker where the array size is known.  It cannot fire at run time: n is
 * a 16000-sample capture, so n <= win and the function returns before touching
 * the array.  But "cannot fire because of a relationship two functions apart"
 * is exactly the kind of claim that stops being true when the width changes,
 * and leaving a standing warning in the build trains everyone to read past the
 * next one.  So the guard states the relationship instead of asserting it, and
 * the aligner comes back the moment the capture is genuinely wider.
 */

#if VOICE_CAPTURE_SAMPLES > VELAPAW_KWS_SAMPLE_RATE

static int voice_align(const int16_t *audio, int n, int win)
{
  uint32_t sum  = 0;
  uint32_t best;
  int      besti = 0;
  int      i;

  if (n <= win)
    {
      return 0;
    }

  for (i = 0; i < win; i++)
    {
      sum += (uint32_t)(audio[i] < 0 ? -audio[i] : audio[i]);
    }

  best = sum;

  for (i = win; i < n; i++)
    {
      sum += (uint32_t)(audio[i] < 0 ? -audio[i] : audio[i]);
      sum -= (uint32_t)(audio[i - win] < 0 ? -audio[i - win] : audio[i - win]);

      if (sum > best)
        {
          best  = sum;
          besti = i - win + 1;
        }
    }

  return besti;
}

#endif /* VOICE_CAPTURE_SAMPLES > VELAPAW_KWS_SAMPLE_RATE */

static void *voice_worker(void *arg)
{
  (void)arg;

  g_voice_init_rc = velapaw_kws_init();
  if (g_voice_init_rc < 0)
    {
      printf("velapaw/voice: kws init failed; voice disabled\n");
      g_voice_fail = 1;
      return NULL;
    }

  /* Build 81: open the mic session HERE, not lazily on the first capture.
   *
   * Session open measures 5350 ms on this board.  Left where it was, it
   * was spent inside the dialog's 8000 ms readiness window, leaving a good
   * boot 650 ms of margin before vsched_tick() gave up with "mic never
   * became ready" -- while the microphone was working perfectly.  Paid
   * here it costs nothing: no dialog can be open yet.
   *
   * It must be this thread that calls it.  The session records its owning
   * pid, and an owner mismatch sends the first capture down the
   * drop-and-reopen path, which is the -110 reopen bug.
   *
   * Not fatal on failure, and deliberately so: velapaw_mic_capture() still
   * opens the session inline when it finds it closed, exactly as before.
   * The return value is kept only so the probe can report it.
   */

  g_voice_preopen_rc = velapaw_mic_preopen();

  printf("velapaw/voice: worker ready (mic pre-open %d)\n",
         g_voice_preopen_rc);

  for (;;)
    {
      struct velapaw_kws_result_s r;
      const int16_t *win;
      long t0;
      long t1;
      int off;
      int wlen;
      int got;
      int rc;

      g_voice_loops++;

      if (!g_voice_run || g_voice_evt != 0)
        {
          /* Idle, or the UI has not collected the last verdict yet.  Do not
           * capture in either case: a window taken now would be stale by the
           * time anything could act on it. */

          usleep(50000);
          continue;
        }

      g_voice_caps++;

      /* t0/t1 bracket the capture; the NEXT line's timestamp minus t1 is the
       * dead time -- how long the microphone was not listening between
       * windows.  That gap is where a word spoken at the wrong moment is lost
       * entirely, which no amount of threshold tuning can recover, so it is
       * measured rather than assumed. */

      t0  = mono_ms();
      got = velapaw_mic_capture(g_voice_audio, VOICE_CAPTURE_SAMPLES,
                                VOICE_CAPTURE_MS);
      t1  = mono_ms();

      g_voice_lastret = got;
      if (got < 0)
        {
          /* Build 79: survive it.  This used to latch g_voice_fail and
           * return, which killed voice for the rest of the boot on a
           * single bad window.
           *
           * Worth being clear that this code has never actually run:
           * velapaw_mic_capture() hung and never returned, so the probe's
           * `fail 0` has been meaningless rather than reassuring.  b79
           * removes the hang, so this path executes for the first time --
           * and it should not be the thing that shoots the thread.
           */

          g_voice_capfail++;
          g_voice_consec++;

          vlog("cap FAILED %d (%d in a row)", got, g_voice_consec);
          printf("velapaw/voice: capture failed (%d), %d in a row\n",
                 got, g_voice_consec);

          if (g_voice_consec >= VOICE_MAX_CAPFAIL)
            {
              printf("velapaw/voice: %d consecutive failures; "
                     "voice disabled\n", g_voice_consec);
              g_voice_fail = 1;
              return NULL;
            }

          /* Not politeness.  Without this the thread spins against the
           * driver as fast as the timeout allows, and on this board that
           * starves the LVGL tick -- a frozen UI is a worse symptom than
           * no voice.
           */

          usleep(200 * 1000);
          continue;
        }

      /* Any success clears the run.  See VOICE_MAX_CAPFAIL. */

      g_voice_consec = 0;
      g_voice_ready = 1;

      if (got < (int)VELAPAW_KWS_MIN_SAMPLES)
        {
          vlog("cap %d in %ldms SHORT", got, t1 - t0);
          continue;                    /* short window: not enough to classify */
        }

      /* Pick the second the model will actually see, then judge THAT.
       *
       * Order matters: the speech gate counts how many eighths of its window
       * carry level, so running it on the full 1.5 s would spread one word
       * over twelve eighths and dilute exactly the measure it is testing.
       * Both the gate and the classifier get the same aligned window, which
       * is also the window the logged score belongs to.
       */

#if VOICE_CAPTURE_SAMPLES > VELAPAW_KWS_SAMPLE_RATE
      off  = voice_align(g_voice_audio, got, VELAPAW_KWS_SAMPLE_RATE);
#else
      off  = 0;                        /* capture is exactly one window wide */
#endif
      win  = g_voice_audio + off;
      wlen = got - off < VELAPAW_KWS_SAMPLE_RATE ?
             got - off : VELAPAW_KWS_SAMPLE_RATE;

      if (!velapaw_mic_speech_present(win, wlen))
        {
          vlog("cap %d in %ldms, no speech", got, t1 - t0);
          continue;                    /* an empty room, not a keyword */
        }

      vlog("cap %d in %ldms, SPEECH @%d", got, t1 - t0, off);

      /* Independent utterance: the frontend carries state between calls and
       * the previous window's noise floor must not bias this one. */

      velapaw_kws_reset();

      pthread_mutex_lock(&g_tflm_lock);
      rc = velapaw_kws_classify(win, (size_t)wlen, &r);
      pthread_mutex_unlock(&g_tflm_lock);

      if (rc < 0)
        {
          vlog("classify failed %d", rc);
          continue;
        }

      vlog("heard %s %.3f (f%u i%u)", velapaw_kws_label_name(r.label),
           (double)r.score, (unsigned)r.feature_ms, (unsigned)r.infer_ms);

      /* Post the whole result and let the UI decide.
       *
       * Build 54 filtered to yes/no with fixed thresholds right here, which
       * was right when yes/no was the entire vocabulary.  It is wrong now: the
       * scheduling dialog asks a different question at each step and the
       * 14-way winner is often not the answer to the question asked -- an
       * "_unknown_" that outscores a correctly-heard "eight" is routine, and
       * dropping it here would make the hour step unusable.  The consumers
       * apply their own subset and threshold (see vsched_step / voice_apply).
       *
       * Printing every accepted window is deliberate: this is the only view
       * of what the microphone actually heard, and it is what the on-hardware
       * threshold re-tune will be read from. */

      printf("velapaw/voice: heard %s (%.3f)\n",
             velapaw_kws_label_name(r.label), (double)r.score);

      g_voice_res   = r;
      g_voice_score = r.score;
      g_voice_evt   = 1;
    }

  return NULL;
}

/* ---- voice scheduling dialog ---------------------------------------------
 * All of this runs on the UI task: it touches LVGL, which is single-threaded.
 */

/* Show a line in the modal.  Colour carries the state, so the owner can read
 * progress without reading words -- accent while it is asking, green on a
 * commit, dim on a cancel. */

static void vsched_say(uint32_t colour, const char *text)
{
  if (g_vc_lbl != NULL)
    {
      lv_obj_set_style_text_color(g_vc_lbl, lv_color_hex(colour), 0);
      lv_label_set_text(g_vc_lbl, text);
    }
}

static void vsched_prompt(void)
{
  char buf[64];

  switch (g_vs.state)
    {
      case VS_MEAL:
        vsched_say(COL_ACCENT, T(S_VC_MEAL));
        break;

      case VS_HOUR:
        snprintf(buf, sizeof(buf), T(S_VC_HOUR_FMT), g_vs.slot + 1);
        vsched_say(COL_ACCENT, buf);
        break;

      case VS_CONFIRM:
        snprintf(buf, sizeof(buf), T(S_VC_CONFIRM_FMT), g_vs.hour);
        vsched_say(COL_COOL, buf);
        break;

      case VS_CONFIRM_PM:
        snprintf(buf, sizeof(buf), T(S_VC_CONFIRM_FMT), g_vs.hour + 12);
        vsched_say(COL_COOL, buf);
        break;

      default:
        break;
    }
}

/* Enter a step.  Every entry resets the retry budget, the votes and the clock,
 * so a step reached twice gets a full allowance both times.
 *
 * The votes especially: VS_HOUR is re-entered when the owner rejects both
 * readings of a digit at VS_CONFIRM_PM, and that rejection is the owner saying
 * the digit was WRONG.  Carrying its votes into the re-ask would let the word
 * they just overruled win on the evidence they overruled it with. */

static void vsched_enter(int state)
{
  g_vs.state    = state;
  g_vs.tries    = 0;
  g_vs.deadline = mono_ms() + VSCHED_TIMEOUT_MS;
  memset(g_vs.votes, 0, sizeof(g_vs.votes));
  vlog("-> step%d (slot %d hour %d)", state, g_vs.slot, g_vs.hour);
  vsched_prompt();
}

static void vsched_stop(uint32_t colour, const char *text)
{
  g_vs.state = VS_IDLE;
  vsched_say(colour, text);

  if (g_vc_btn != NULL)
    {
      lv_obj_set_style_bg_color(g_vc_btn, lv_color_hex(COL_BTN), 0);
    }
}

static void vsched_start(void)
{
  if (g_voice_fail)
    {
      vsched_say(COL_DIM, T(S_VOICE_OFF));
      return;
    }

  vlog("=== dialog start ===");
  g_vs.slot = 0;
  g_vs.hour = 0;
  vsched_enter(VS_MEAL);

  if (g_vc_btn != NULL)
    {
      lv_obj_set_style_bg_color(g_vc_btn, lv_color_hex(COL_ACCENT), 0);
    }
}

/* Write the spoken time into the modal's SCRATCH copy, not straight into the
 * pet.
 *
 * AUDIO_PLAN had this landing on velapaw_identity_set_meals() directly.  That
 * would be wrong: this dialog is inside a modal that already has Save and
 * Cancel, and a voice command that silently bypassed Cancel would be the one
 * place in the UI where speaking is less undoable than tapping.  The owner
 * sees the time appear in the meal row and still has to tap Save -- which is
 * also the reason the confirm step can afford to be as cheap as it is. */

static void vsched_commit(int hour)
{
  char buf[64];

  if (g_edit_pet < 0 || g_vs.slot < 0 || g_vs.slot >= VELAPAW_MAX_MEALS)
    {
      vsched_stop(COL_DIM, T(S_VC_CANCEL));
      return;
    }

  g_edit_meal[g_vs.slot] = hour * 60;
  g_meal_sel             = g_vs.slot;   /* leave the buttons on what was set */
  refresh_edit();

  vlog("COMMIT meal %d = %02d:00", g_vs.slot + 1, hour);
  snprintf(buf, sizeof(buf), T(S_VC_SAVED_FMT), g_vs.slot + 1, hour);
  vsched_stop(COL_FED, buf);

  printf("velapaw/voice: meal %d set to %02d:00 by voice\n",
         g_vs.slot + 1, hour);
}

/* One capture, interpreted in the context of the current step.  Returns with
 * the dialog either advanced, retried, or stopped. */

static void vsched_step(const struct velapaw_kws_result_s *r)
{
  static const int meal_words[] =
    { VELAPAW_KWS_ONE, VELAPAW_KWS_TWO, VELAPAW_KWS_THREE };
  static const int hour_words[] =
    { VELAPAW_KWS_ZERO, VELAPAW_KWS_ONE, VELAPAW_KWS_TWO, VELAPAW_KWS_THREE,
      VELAPAW_KWS_FOUR, VELAPAW_KWS_FIVE, VELAPAW_KWS_SIX, VELAPAW_KWS_SEVEN,
      VELAPAW_KWS_EIGHT, VELAPAW_KWS_NINE };
  static const int yesno_words[] = { VELAPAW_KWS_YES, VELAPAW_KWS_NO };

  const int *words;
  int   nwords;
  int   voting;
  float need;
  float score = 0.0f;
  int   heard;
  int   idx;

  switch (g_vs.state)
    {
      case VS_MEAL:
        words = meal_words;  nwords = 3;  need = VSCHED_MEAL_SCORE;
        voting = 1;
        break;

      case VS_HOUR:
        words = hour_words;  nwords = 10; need = VSCHED_HOUR_SCORE;
        voting = 1;
        break;

      default:
        words = yesno_words; nwords = 2;  need = VSCHED_CONFIRM_SCORE;
        voting = 0;
        break;
    }

  heard = velapaw_kws_best_of(r, words, nwords, &score);
  if (heard < 0)
    {
      return;
    }

  printf("velapaw/voice: step %d best-of-%d %s %.3f (need %.2f)\n",
         g_vs.state, nwords, velapaw_kws_label_name(heard),
         (double)score, (double)need);

  /* Every outcome below also goes to the RAM log.  The printf above has never
   * once reached the console -- see the g_vlog block -- and these scores are
   * exactly what the threshold and vote tune have to be read from.
   *
   * Where in the step's subset the heard word sits: the vote index.  best_of
   * only ever returns a member of words[], so the idx < nwords tests below are
   * belt and braces against a future subset that is not a plain lookup. */

  for (idx = 0; idx < nwords; idx++)
    {
      if (words[idx] == heard)
        {
          break;
        }
    }

  if (score < need)
    {
      /* Below the per-window gate.  Before asking again, see whether this step
       * has now heard the same word often enough for the repetition itself to
       * be the answer.  Only captures that clear the floor count: see the
       * VSCHED_VOTES block for what the floor is doing and why it is not a
       * consecutive run. */

      if (voting && score >= VSCHED_VOTE_FLOOR && idx < nwords &&
          ++g_vs.votes[idx] >= VSCHED_VOTES)
        {
          vlog("step%d of%d %s %.3f LOW but %d votes -> accept",
               g_vs.state, nwords, velapaw_kws_label_name(heard),
               (double)score, g_vs.votes[idx]);
          goto accept;
        }

      vlog("step%d of%d %s %.3f need %.2f LOW (v%d t%d)", g_vs.state, nwords,
           velapaw_kws_label_name(heard), (double)score, (double)need,
           idx < nwords ? g_vs.votes[idx] : 0, g_vs.tries + 1);

      /* Not confident enough to act on.  Nothing is lost by asking again --
       * the alternative is setting a feeding time the owner did not say. */

      if (++g_vs.tries >= VSCHED_TRIES)
        {
          vlog("step%d gave up after %d tries", g_vs.state, g_vs.tries);
          vsched_stop(COL_DIM, T(S_VC_CANCEL));
          return;
        }

      vsched_say(COL_UNK, T(S_VC_RETRY));
      return;
    }

  vlog("step%d of%d %s %.3f need %.2f OK", g_vs.state, nwords,
       velapaw_kws_label_name(heard), (double)score, (double)need);

accept:

  switch (g_vs.state)
    {
      case VS_MEAL:
        g_vs.slot = heard - VELAPAW_KWS_ONE;      /* "one" -> slot 0 */
        vsched_enter(VS_HOUR);
        break;

      case VS_HOUR:
        g_vs.hour = VELAPAW_KWS_DIGIT_OF(heard);
        vsched_enter(VS_CONFIRM);
        break;

      case VS_CONFIRM:
        if (heard == VELAPAW_KWS_YES)
          {
            vsched_commit(g_vs.hour);
          }
        else
          {
            /* Not the morning reading -- offer the afternoon one before
             * making the owner say the digit again.  This is what puts
             * 12:00-21:00 within reach of a single spoken digit. */

            vsched_enter(VS_CONFIRM_PM);
          }
        break;

      default:  /* VS_CONFIRM_PM */
        if (heard == VELAPAW_KWS_YES)
          {
            vsched_commit(g_vs.hour + 12);
          }
        else
          {
            vsched_enter(VS_HOUR);     /* misheard digit: ask it again */
          }
        break;
    }
}

/* Called from the UI loop while the dialog is up: enforces the per-step
 * deadline.  Nothing else notices a silent room -- the worker drops silence
 * before classifying, so a walked-away owner produces no events at all. */

/* Longest the dialog will sit on "listening..." waiting for the first capture
 * to prove the microphone works.  Opening the session takes well under a
 * second; the 5 s capture timeout in the worker sets the real scale, so this
 * has to exceed it or a merely slow open would be reported as a dead mic.
 */

#define VSCHED_MIC_WAIT_MS VSCHED_MIC_WAIT_MS_VAL

static void vsched_tick(void)
{
  static int  mic_wait;   /* 1 = "listening..." is the line currently shown */
  static long mic_since;  /* mono_ms() when that line went up               */

  if (g_vs.state == VS_IDLE)
    {
      mic_wait = 0;
      return;
    }

  /* The microphone died after the dialog opened.
   *
   * vsched_start() checks g_voice_fail too, but it can almost never fire:
   * the worker does not attempt a capture until g_voice_run goes high, and
   * that happens when this dialog opens, so a capture failure always lands
   * about five seconds AFTER the start check has passed.  Build 57 left the
   * wait below with no failure exit at all, which turned a dead microphone
   * into a dialog stuck on "listening..." forever -- deadline pushed forward
   * on every tick, no prompt, no timeout, no way out but Cancel.
   */

  if (g_voice_fail)
    {
      mic_wait = 0;
      vsched_stop(COL_DIM, T(S_VOICE_OFF));
      return;
    }

  /* Do not ask for a word before the capture path can hear one.
   *
   * g_voice_ready goes up when the FIRST capture returns, which is the first
   * moment the mic session is proven open -- and opening it is not instant.
   * Prompting during that window would reliably eat the owner's first
   * utterance and read as the model getting it wrong.  The deadline is held
   * off too, so waiting for the microphone never counts against the owner's
   * 15 s.
   *
   * Bounded, though.  g_voice_fail covers a capture that RETURNS an error; a
   * worker wedged inside the capture never sets it, and an unbounded hold-off
   * would wait on that for the rest of the session.
   */

  if (!g_voice_ready)
    {
      long now = mono_ms();

      if (!mic_wait)
        {
          mic_wait  = 1;
          mic_since = now;
          vsched_say(COL_DIM, T(S_VC_LISTEN));
        }
      else if (now - mic_since > VSCHED_MIC_WAIT_MS)
        {
          mic_wait = 0;
          vlog("mic never became ready in %dms", VSCHED_MIC_WAIT_MS);
          vsched_stop(COL_DIM, T(S_VOICE_OFF));
          return;
        }

      g_vs.deadline = now + VSCHED_TIMEOUT_MS;
      return;
    }

  if (mic_wait)
    {
      mic_wait = 0;
      vsched_prompt();          /* now it is a fair question to ask */
    }

  if (mono_ms() > g_vs.deadline)
    {
      vlog("step%d TIMED OUT after %dms", g_vs.state, VSCHED_TIMEOUT_MS);
      vsched_stop(COL_DIM, T(S_VC_CANCEL));
    }
}

/* Act on an accepted keyword.  Runs on the UI task -- it touches LVGL, which
 * is single-threaded, so it must not be called from the worker.
 *
 * Returns 1 if it put a verdict on screen, 0 if it dropped the word.  The
 * caller needs that to decide whether to hold the line: g_voice_shown cannot
 * answer it, because a verdict from a few seconds ago is still 3 and a
 * dropped word would then keep extending a message the owner has read. */

static int voice_apply(int label)
{
  const struct velapaw_pet *pet;
  float need;
  int today;
  int remaining;
  int amount;

  /* The filter that used to live in the worker.  Kept as a plain 14-way check
   * rather than a best_of() over {yes,no}: this tab is not asking a question,
   * so a "seven" here means the owner is talking about something else, and
   * promoting it to the nearest of yes/no would be an invention.  SILENCE and
   * UNKNOWN are the model saying it has no opinion, which is most of what a
   * kitchen produces -- dropping them is the point of having them. */

  if (label != VELAPAW_KWS_YES && label != VELAPAW_KWS_NO)
    {
      return 0;
    }

  need = (label == VELAPAW_KWS_YES) ? VOICE_YES_SCORE : VOICE_NO_SCORE;
  if (g_voice_score < need)
    {
      printf("velapaw/voice: %s %.3f below %.2f, ignored\n",
             velapaw_kws_label_name(label), (double)g_voice_score,
             (double)need);
      return 0;
    }

  g_voice_shown = 3;

  if (label == VELAPAW_KWS_NO)
    {
      g_voice_dismissed = 1;
      lv_obj_set_style_text_color(g_voice_lbl, lv_color_hex(COL_DIM), 0);
      lv_label_set_text(g_voice_lbl, T(S_VOICE_CANCELLED));
      return 1;
    }

  /* "yes" is meaningless without a subject.  Recognition names WHO is at the
   * bowl; the voice only authorises it.  Feeding whoever happens to be there
   * on an unmatched frame would defeat the entire point of the product. */

  if (g_cand == VELAPAW_PET_UNKNOWN || g_stable < RECOG_STABLE_N)
    {
      lv_obj_set_style_text_color(g_voice_lbl, lv_color_hex(COL_UNK), 0);
      lv_label_set_text(g_voice_lbl, T(S_VOICE_NO_PET));
      return 1;
    }

  pet = velapaw_identity_get(g_cand);
  if (pet == NULL)
    {
      return 0;
    }

  today = velapaw_store_today_total(g_cand);
  if (today >= pet->daily_limit_g)
    {
      lv_obj_set_style_text_color(g_voice_lbl, lv_color_hex(COL_COOL), 0);
      lv_label_set_text_fmt(g_voice_lbl, T(S_LIMIT_HIT_FMT),
                            today, pet->daily_limit_g);
      return 1;
    }

  /* Trim so the day never exceeds the owner's cap -- the same arithmetic the
   * scheduled path uses, because the cap is a property of the pet, not of the
   * route the food took. */

  remaining = pet->daily_limit_g - today;
  amount    = (pet->portion_g < remaining) ? pet->portion_g : remaining;

  velapaw_feeder_dispense(amount);

  { struct timespec _w;
    clock_gettime(CLOCK_REALTIME, &_w);
    struct velapaw_feed_event ev =
      { g_cand, (time_t)_w.tv_sec, amount, g_voice_score, g_be->name };
    velapaw_store_log_feeding(&ev); }

  g_last_fed[g_cand] = mono_ms() / 1000;

  /* Deliberately does NOT mark a meal slot fed (g_meal_fed_day).  This is an
   * off-schedule treat the owner asked for out loud; consuming a scheduled
   * meal would mean the pet silently loses the meal it was going to get, and
   * the owner never said that.  The daily limit above is what stops this from
   * being unbounded. */

  lv_obj_set_style_text_color(g_voice_lbl, lv_color_hex(COL_FED), 0);
  lv_label_set_text_fmt(g_voice_lbl, T(S_VOICE_FED_FMT), amount,
                        velapaw_store_today_total(g_cand),
                        pet->daily_limit_g);

  printf("velapaw/voice: fed %s %dg by voice (%.2f)\n",
         pet->name, amount, (double)g_voice_score);
  return 1;
}

/* The standing line on the Recognize tab when there is no verdict to show.
 * Only writes on a state CHANGE -- the UI loop runs at 50 Hz and rewriting a
 * label with the same text every 20 ms would invalidate the area forever. */

static void voice_idle_text(void)
{
  int want;

  if (g_voice_fail)
    {
      want = 2;
    }
  else if (g_voice_ready && !g_voice_dismissed)
    {
      want = 1;
    }
  else
    {
      /* Nothing to promise yet: the worker has not completed a capture, so we
       * do not know the microphone works.  An instruction that silently does
       * nothing is worse than a blank line. */

      want = 0;
    }

  if (want == g_voice_shown)
    {
      return;
    }

  g_voice_shown = want;
  lv_obj_set_style_text_color(g_voice_lbl,
                              lv_color_hex(want == 2 ? COL_DIM : COL_COOL), 0);
  lv_label_set_text(g_voice_lbl,
                    want == 2 ? T(S_VOICE_OFF) :
                    want == 1 ? T(S_VOICE_PROMPT) : "");
}

/****************************************************************************
 * Name: velapaw_ui_voice_probe
 *
 * Description:
 *   Dump the voice worker's state to the console.  Called from a CLI task
 *   ("velapaw voice") while the UI keeps running.
 *
 *   Reading another thread's globals from a different task is sound here only
 *   because this is a FLAT build: one address space, one .data.  It would be
 *   nonsense in a protected or kernel build, and this function should be the
 *   first thing deleted if the app is ever moved to one.
 *
 *   Interpretation, worker states in the order they occur:
 *
 *     init_rc -999          the thread never ran, or is still inside
 *                           velapaw_kws_init()
 *     init_rc < 0           model init failed; fail=1 and the worker exited
 *     loops 0               past init but the loop body has not run
 *     loops rising, caps 0  looping but never capturing -- run=0 (the UI is
 *                           not asking) or evt!=0 (a verdict the UI never
 *                           collected, which stalls the worker forever)
 *     caps 1, lastret -999  inside velapaw_mic_capture right now, blocked
 *     lastret < 0           capture returned an error; that IS the errno
 *
 *   The "mic stage" line narrows that last case, which is where build 60 left
 *   the investigation: it names the step inside mic.c that capture is sitting
 *   on.  Run this command TWICE, a few seconds apart, and compare:
 *
 *     same stage, same seq   that call is blocked -- it is the bug
 *     same stage, seq rising a loop at that stage is spinning, not blocked
 *     stage advancing        capture is simply slow, not stuck
 *
 *   One reading cannot separate those three, so do not draw a conclusion from
 *   one.
 ****************************************************************************/

/* Patch #33 instrumentation, defined in arch/xtensa/src/esp32s3/esp32s3_i2s.c.
 * This is a flat build, so the app links against the driver's symbols
 * directly; there is no header to include for them.
 */

extern uint32_t g_velapaw_rx_eof;
extern uint32_t g_velapaw_rx_match;
extern uint32_t g_velapaw_rx_miss;
extern uint32_t g_velapaw_rx_empty;
extern uint32_t g_velapaw_rx_start;
extern uint32_t g_velapaw_rx_busy;

extern void velapaw_i2s_rxdbg(uint32_t *rxconf, uint32_t *txconf,
                              uint32_t *rxeofn);

/* Patch #34. */

extern uint32_t g_velapaw_isr_rx;
extern uint32_t g_velapaw_isr_noeof;
extern uint32_t g_velapaw_isr_last;

extern void velapaw_i2s_rxdesc(uint32_t *ctrl, int nctrl, int *nact,
                               int *npend, int *ndone, uint32_t *intraw,
                               uint32_t *intena, uint32_t *inlink);

/* Patch #35 counters, reinterpreted by patch #36.
 *
 * cfgskip must now read 0 -- #36 reverted #35's guards, so anything else
 * means the wrong driver got linked.  chst counts real RX channel restarts
 * and resc counts containers pulled back out of rx.act after a reset
 * orphaned them.
 */

extern uint32_t g_velapaw_cfg_skip;
extern uint32_t g_velapaw_rx_chst;
extern uint32_t g_velapaw_rx_resc;

/* Build 79 recovery census.  kick is the driver-side count of
 * velapaw_i2s_rx_kick() calls; the mic pair is the app's view of the same
 * events.  The three readings that matter:
 *
 *   mickick 0                nothing ever stalled this boot
 *   mickick n  ok n          it stalls, and kicking the RX channel clears it
 *   mickick n  ok 0          it stalls and the kick does nothing, i.e. the
 *                            fault is below the DMA after all
 *
 * The third would be the first hard evidence separating `the DMA got
 * wedged` from `no samples are arriving`, and it costs one boot to get.
 */

extern uint32_t g_velapaw_rx_kick;
extern uint32_t g_mic_kicks;
extern uint32_t g_mic_kick_ok;

void velapaw_ui_voice_probe(void)
{
  printf("velapaw/voice probe: init_rc %d  loops %d  caps %d  lastret %d\n",
         g_voice_init_rc, g_voice_loops, g_voice_caps, g_voice_lastret);
  printf("velapaw/voice probe: run %d  ready %d  fail %d  evt %d  "
         "dismissed %d\n",
         g_voice_run, g_voice_ready, g_voice_fail, g_voice_evt,
         g_voice_dismissed);
  printf("velapaw/voice probe: preopen %d  capms %d  dialogms %d\n",
         g_voice_preopen_rc, VOICE_CAPTURE_MS, VSCHED_MIC_WAIT_MS);
  printf("velapaw/voice probe: mic stage %d (%s)  seq %u  lasterr %d\n",
         velapaw_mic_stage(), velapaw_mic_stage_name(velapaw_mic_stage()),
         velapaw_mic_stage_seq(), velapaw_mic_last_error());

  /* Patch #33: the I2S RX EOF census, straight out of the driver.  A stall
   * shows up as `rec 1/19` from up here, which cannot say WHY RX stopped
   * rearming.  These can:
   *
   *   busy > 0 and rising   a container is stranded in rx.act, so every
   *                         i2s_rxdma_start() bails -- the descriptor-walk
   *                         wedge, and `miss` says whether the walk desynced
   *   miss > 0              the walk desynced; patch #31 was aimed correctly
   *                         and its remediation is what needs fixing
   *   miss 0 and eof 1      RX EOF simply stopped firing -- the clock or
   *                         RX_START is the problem, not the walk
   *
   * rxconf bit 0 is RX_START; txconf bit 0 is TX_START and bit 1 TX_STOP_EN
   * (patch #16 clears it).  If TX_START is set and RX_START is set and EOF
   * still never fires, the clock is live and the fault is downstream.
   */

  {
    uint32_t rxconf = 0;
    uint32_t txconf = 0;
    uint32_t rxeofn = 0;

    velapaw_i2s_rxdbg(&rxconf, &txconf, &rxeofn);

    printf("velapaw/voice probe: i2s eof %u match %u miss %u empty %u  "
           "start %u busy %u\n",
           (unsigned)g_velapaw_rx_eof, (unsigned)g_velapaw_rx_match,
           (unsigned)g_velapaw_rx_miss, (unsigned)g_velapaw_rx_empty,
           (unsigned)g_velapaw_rx_start, (unsigned)g_velapaw_rx_busy);
    printf("velapaw/voice probe: i2s rxconf %08x txconf %08x rxeofn %u\n",
           (unsigned)rxconf, (unsigned)txconf, (unsigned)rxeofn);
  }

  /* Patch #34: the in-flight RX descriptor.  This is the measurement that
   * decides what the stall actually is.  In each ctrl word, bit 31 is the DMA
   * owner bit and bits 12-23 are the length written:
   *
   *   owner 1, len 0     armed and nothing is arriving -> the fault is
   *                      upstream of the DMA, and a re-arm will not help
   *   owner 0, len > 0   the buffer was FILLED and the EOF interrupt was
   *                      lost -> recoverable with a kick
   *
   * isr vs noeof separates "the ISR stopped firing" from "the ISR fires but
   * SUC_EOF is never set".  Printed raw; decoded off-board.
   */

  {
    uint32_t ctrl[4];
    uint32_t intraw = 0;
    uint32_t intena = 0;
    uint32_t inlink = 0;
    int nact = 0;
    int npend = 0;
    int ndone = 0;

    velapaw_i2s_rxdesc(ctrl, 4, &nact, &npend, &ndone,
                       &intraw, &intena, &inlink);

    printf("velapaw/voice probe: i2s isr %u noeof %u last %08x  "
           "act %d pend %d done %d\n",
           (unsigned)g_velapaw_isr_rx, (unsigned)g_velapaw_isr_noeof,
           (unsigned)g_velapaw_isr_last, nact, npend, ndone);
    printf("velapaw/voice probe: i2s desc %08x %08x %08x %08x\n",
           (unsigned)ctrl[0], (unsigned)ctrl[1],
           (unsigned)ctrl[2], (unsigned)ctrl[3]);
    printf("velapaw/voice probe: i2s intraw %08x intena %08x inlink %08x\n",
           (unsigned)intraw, (unsigned)intena, (unsigned)inlink);
    printf("velapaw/voice probe: i2s cfgskip %u chst %u resc %u kick %u\n",
           (unsigned)g_velapaw_cfg_skip, (unsigned)g_velapaw_rx_chst,
           (unsigned)g_velapaw_rx_resc, (unsigned)g_velapaw_rx_kick);
    printf("velapaw/voice probe: recover mickick %u ok %u  "
           "capfail %d consec %d\n",
           (unsigned)g_mic_kicks, (unsigned)g_mic_kick_ok,
           g_voice_capfail, g_voice_consec);
  }

  /* The device states each session came up in.  Build 67 established that RX
   * simply never arms for the worker (head 18, tail 0) while it arms fine for
   * `velapaw kws mic` in the same boot, and mic.h explains which of these
   * numbers distinguishes the two causes of that. */

  {
    struct velapaw_mic_open_s ol[2];
    int n;
    int i;

    n = velapaw_mic_open_log(ol, 2);
    for (i = 0; i < n; i++)
      {
        printf("velapaw/voice probe: mic open%d pid %d seq %u  "
               "reccfg %d recstart %d playstart %d  bufs %d  ret %d\n",
               i, ol[i].pid, ol[i].seq, ol[i].rec_cfg, ol[i].rec_start,
               ol[i].play_start, ol[i].nbufs, ol[i].ret);
      }

    if (n == 0)
      {
        printf("velapaw/voice probe: mic open none yet\n");
      }
  }

  /* Only meaningful once a capture has actually failed.  want == 0 is the
   * "nothing recorded" case and says so rather than printing six zeros that
   * look like measurements. */

  {
    struct velapaw_mic_fail_s f;

    velapaw_mic_last_fail(&f);

    if (f.want == 0)
      {
        printf("velapaw/voice probe: mic fail none recorded\n");
      }
    else
      {
        printf("velapaw/voice probe: mic fail got %lu/%lu  seq %d  "
               "phase %d\n", f.got, f.want, f.seq, f.phase);
        printf("velapaw/voice probe: mic fail rec %lu/%lu  play %lu/%lu "
               "(tail/head)\n", f.rtail, f.rhead, f.ptail, f.phead);
      }
  }

  printf("velapaw/voice probe: vsched state %d  slot %d  hour %d  tries %d\n",
         g_vs.state, g_vs.slot, g_vs.hour, g_vs.tries);
  fflush(stdout);

  /* The event log, oldest first.
   *
   * Paced.  One line per 30 ms is glacial by any normal standard and it is
   * deliberate: the console DROPS writes rather than blocking on them (see the
   * g_vlog block), so dumping 28 lines back to back is the one way to
   * guarantee this record shares the fate of the printfs it exists to replace.
   * A diagnostic that is unreliable exactly when there is a lot to say is
   * worse than none, and 0.8 s is free -- this runs from a CLI task with
   * nothing waiting on it.
   */

  { unsigned head = g_vlog_head;
    unsigned first = head > VLOG_N ? head - VLOG_N : 0;
    unsigned i;

    printf("velapaw/voice log: %u events%s\n", head,
           head == 0 ? " (nothing has happened yet)" : ", oldest first:");
    fflush(stdout);

    for (i = first; i < head; i++)
      {
        printf("  %s\n", g_vlog[i % VLOG_N]);
        fflush(stdout);
        usleep(30000);
      }
  }
}
#endif /* CONFIG_VELAPAW_VOICE */

/* Translated tab name for the header breadcrumb. */
static const char *tab_name(uint32_t t)
{
  return (t == 0) ? T(S_TAB_ENROLL) :
         (t == 1) ? T(S_TAB_RECOGNIZE) :
         (t == 2) ? T(S_TAB_PETS) : T(S_TAB_TRENDS);
}

/* Build (or rebuild) the whole UI tree onto g_scr in the CURRENT language.
 * Called once at startup, then again on every language toggle. The pipeline
 * (camera/backend/store/identity) is already running by then and is NOT touched
 * here -- only the LVGL widget tree is torn down and recreated, which is how the
 * one-shot labels pick up the new language + font. */
static void ui_build(void)
{
#ifdef CONFIG_VELAPAW_VOICE
  /* Every widget this dialog writes to is about to be freed.  Stop it BEFORE
   * the clean, not after: vsched_stop() would repaint a dangling label. */

  g_vs.state = VS_IDLE;
  g_vc_lbl   = NULL;
  g_vc_btn   = NULL;
#endif

  lv_obj_clean(lv_layer_top());        /* drop the old keyboard / modal / popups */
  lv_obj_clean(g_scr);                 /* drop the old root and every tab         */

  /* full-screen root */
  lv_obj_t *col = lv_obj_create(g_scr);
  lv_obj_set_size(col, SCR_W, SCR_H);
  lv_obj_align(col, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(col, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_radius(col, 0, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

  /* slim header: breadcrumb + language toggle + bell */
  lv_obj_t *hdr = lv_obj_create(col);
  lv_obj_set_size(hdr, SCR_W, 24);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 0, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  g_crumb = make_label(hdr, F_BODY, COL_TEXT);
  { int cm = wall_min();
    lv_label_set_text_fmt(g_crumb, T(S_CRUMB_FMT), cm / 60, cm % 60,
                          T(S_TAB_ENROLL)); }
  lv_obj_align(g_crumb, LV_ALIGN_LEFT_MID, 8, 0);
  g_bell = make_label(hdr, F_HEAD, COL_DIM);
  lv_label_set_text(g_bell, LV_SYMBOL_BELL);
  lv_obj_align(g_bell, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_add_flag(g_bell, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(g_bell, 16);   /* larger tap target */
  lv_obj_add_event_cb(g_bell, bell_cb, LV_EVENT_CLICKED, NULL);
  /* language toggle, sitting just left of the bell. Shows the language you'd
   * switch TO (中 while English, EN while 中). */
  g_langlbl = make_label(hdr, F_HEAD, COL_ACCENT);
  lv_label_set_text(g_langlbl, T(S_LANG_TOGGLE));
  lv_obj_align(g_langlbl, LV_ALIGN_RIGHT_MID, -34, 0);
  lv_obj_add_flag(g_langlbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(g_langlbl, 16);
  lv_obj_add_event_cb(g_langlbl, btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)EV_LANG);

  /* tabview fills the rest */
  g_tv = lv_tabview_create(col);
  lv_tabview_set_tab_bar_position(g_tv, LV_DIR_TOP);
  lv_tabview_set_tab_bar_size(g_tv, 30);
  lv_obj_set_size(g_tv, SCR_W, SCR_H - 24);
  lv_obj_align(g_tv, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_set_style_bg_color(g_tv, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_color(lv_tabview_get_content(g_tv), lv_color_hex(COL_BG), 0);

  lv_obj_t *t_en = lv_tabview_add_tab(g_tv, T(S_TAB_ENROLL));
  lv_obj_t *t_rc = lv_tabview_add_tab(g_tv, T(S_TAB_RECOGNIZE));
  lv_obj_t *t_mp = lv_tabview_add_tab(g_tv, T(S_TAB_PETS));
  lv_obj_t *t_tr = lv_tabview_add_tab(g_tv, T(S_TAB_TRENDS));
  lv_obj_set_style_bg_color(t_en, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_color(t_rc, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_color(t_mp, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_color(t_tr, lv_color_hex(COL_BG), 0);

  /* The tab-bar buttons are created internally by the tabview and use the
   * theme's default (Latin-only) font, so CJK tab names render as boxes. Force
   * the tab bar onto our font (text_font inherits down to the button labels). */
  lv_obj_set_style_text_font(lv_tabview_get_tab_bar(g_tv), F_HEAD, 0);

  /* No momentum/elastic scroll: on the slow bit-bang panel the post-release
   * glide/bounce animation is many slow full repaints -> judder. Direct
   * finger-tracked scroll stops the instant you lift = crisper. */
  lv_obj_clear_flag(t_en, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(t_rc, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(t_mp, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(t_tr, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);

  build_enroll(t_en);
  build_recognize(t_rc);
  build_pets(t_mp);
  build_trends(t_tr);
  build_edit_modal();      /* hidden until a pet's "Edit schedule" is tapped */

  /* keyboard floats above everything */
  g_kb = lv_keyboard_create(lv_layer_top());
  lv_keyboard_set_textarea(g_kb, g_name_ta);
  lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_kb, kb_cb, LV_EVENT_ALL, NULL);

  refresh_enroll();
  lv_label_set_text_fmt(g_count, T(S_ENROLLED_COUNT_FMT), velapaw_identity_count());
  lv_label_set_text_fmt(g_metrics, T(S_BACKEND_FMT), g_be->name,
                        (unsigned long)g_be->arena_bytes() / 1024);
}

void velapaw_ui_run(void)
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

  /* CRITICAL for a standalone demo: the console is a USB CDC with only a
   * 193-byte TX buffer (CONFIG_CDCACM_TXBUFSIZE). If nothing is draining the
   * port -- i.e. no PC attached, which is exactly the demo case -- that buffer
   * fills and printf() BLOCKS, freezing the UI + camera. It bit us on the first
   * feed (the feeder/store printfs). Make stdout non-blocking so printf drops
   * output instead of stalling; logging still works when a host is reading. */
  { int _fl = fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (_fl >= 0) fcntl(STDOUT_FILENO, F_SETFL, _fl | O_NONBLOCK);
    _fl = fcntl(STDERR_FILENO, F_GETFL, 0);
    if (_fl >= 0) fcntl(STDERR_FILENO, F_SETFL, _fl | O_NONBLOCK); }

  lv_init();
  lv_nuttx_dsc_init(&info);
  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      printf("velapaw/ui: LVGL display init failed\n");
      return;
    }
  printf("velapaw/ui: LVGL ready; touch indev %s\n",
         result.indev ? "ok" : "MISSING");

  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);

  g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  g_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  g_dsc.header.w = VELAPAW_FRAME_W;
  g_dsc.header.h = VELAPAW_FRAME_H;
  g_dsc.header.stride = VELAPAW_FRAME_W * 2;
  g_dsc.data = (const uint8_t *)g_img565;
  g_dsc.data_size = sizeof(g_img565);

  /* ---- init pipeline ONCE (before building tabs that read state, and before
   *      ui_build() which may run again on a language switch) ---- */
  g_be = velapaw_infer_active();
  velapaw_camera_open();
  printf("velapaw: '%s' backend init...\n", g_be->name);
  { int _ir = g_be->init();
    printf("velapaw: init ret=%d arena=%lu KB\n", _ir,
           (unsigned long)(g_be->arena_bytes() / 1024)); }
  /* spin up the background inference worker (keeps Invoke off the UI thread) */
  { pthread_t _wt; pthread_attr_t _wa; pthread_attr_init(&_wa);
    pthread_attr_setstacksize(&_wa, 32768);
    if (pthread_create(&_wt, &_wa, recog_worker, NULL) == 0)
      pthread_detach(_wt);
    pthread_attr_destroy(&_wa);
    printf("velapaw: recog worker started\n"); }
#ifdef CONFIG_VELAPAW_VOICE
  /* Same 32 KB stack as the vision worker, and for the same reason: this one
   * also runs a TFLM Invoke, and 32 KB is the figure that has been proven on
   * this board.  The 1 s audio window is NOT on this stack (see
   * g_voice_audio).  The worker opens the microphone lazily on its first
   * listening pass, so starting it here costs nothing until the user reaches
   * the Recognize tab. */
  { pthread_t _vt; pthread_attr_t _va; pthread_attr_init(&_va);
    pthread_attr_setstacksize(&_va, 32768);
    if (pthread_create(&_vt, &_va, voice_worker, NULL) == 0)
      pthread_detach(_vt);
    else
      g_voice_fail = 1;
    pthread_attr_destroy(&_va);
    printf("velapaw: voice worker started\n"); }
#endif
  velapaw_identity_init();
  velapaw_store_init();
  velapaw_feeder_init();
  for (int i = 0; i < VELAPAW_MAX_PETS; i++)
    {
      g_last_fed[i] = -100000;
      for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
        {
          g_meal_fed_day[i][k] = -1;   /* no scheduled meal eaten yet */
          g_meal_skipped[i][k] = 0;
        }
    }
  /* Time comes from the on-board PCF85063 RTC (synced in board_lcd_initialize),
   * so it persists across resets. That chip only stores years 2000-2099 -- its
   * year register is an offset from 2000 -- so the clock MUST live in that
   * range or the register wraps to garbage. If what we got back is implausible
   * (never set, or a pre-2000 epoch), seed a real date; the user then adjusts
   * the time on Enroll and it is written straight back into the chip. */
  { struct timespec _ts;
    clock_gettime(CLOCK_REALTIME, &_ts);
    if (_ts.tv_sec < 1500000000L)                  /* pre-2017 => not real */
      {
        _ts.tv_sec  = 1783900800L + 7 * 3600 + 55 * 60;  /* 2026-07-13 07:55 */
        _ts.tv_nsec = 0;
        clock_settime(CLOCK_REALTIME, &_ts);
        board_rtc_settime(_ts.tv_sec);
      } }
  g_view = 0;
  velapaw_camera_mock_set_scene(g_view);

  /* NOTE: velapaw_lang_load() temporarily removed to bisect the boot wedge. */

  g_scr = scr;
  ui_build();              /* build the whole tree in the current language */

  long last_recog = 0;
  long last_frame = 0;
  uint32_t last_tab = 99;
  g_last_touch = mono_ms();     /* else Enroll starts out "idle" and paused */
  struct velapaw_frame frame;
  cam_grab(&frame);
  show_frame(&frame);

  while (1)
    {
      lv_timer_handler();

      uint32_t tab = lv_tabview_get_tab_active(g_tv);
      static uint32_t crumb_tab = 0;
      static int last_clock_min = -1;
      /* header shows the wall clock; repaint when the minute (or tab) changes */
      { int cm = wall_min();
        if (cm != last_clock_min)
          {
            last_clock_min = cm;
            lv_label_set_text_fmt(g_crumb, T(S_CRUMB_FMT),
                                  cm / 60, cm % 60, tab_name(crumb_tab));
          } }
      if (tab != last_tab)
        {
          last_tab = tab;
          crumb_tab = tab;
          last_clock_min = -1;   /* force the header to repaint with the new tab */
          if (tab == 2)
            {
              refresh_pets();
            }
          else if (tab == 3)
            {
              refresh_trends();
            }
        }

      ui_event_t ev = g_evt;
      g_evt = EV_NONE;
      if (ev == EV_VIEW)
        {
          g_view = (g_view + 1) % NUM_VIEWS;
          velapaw_camera_mock_set_scene(g_view);
          cam_grab(&frame);
          show_frame(&frame);            /* repaint preview only on change */
          refresh_enroll();
        }
      else if (ev == EV_CAPTURE)
        {
          do_capture();
        }
      else if (ev == EV_SAVE)
        {
          do_save();
        }
      else if (ev == EV_NEXTDAY)
        {
          velapaw_store_roll_day();
          for (int i = 0; i < VELAPAW_MAX_PETS; i++)
            {
              for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
                {
                  g_meal_fed_day[i][k] = -1;  /* new day -> all meals due again */
                  g_meal_skipped[i][k] = 0;   /* skips don't carry over        */
                }
            }
          refresh_pets();
          refresh_trends();
        }
      else if (ev == EV_TREND_PREV || ev == EV_TREND_NEXT)
        {
          int cnt = velapaw_identity_count();
          if (cnt > 0)
            {
              g_trend_pet = (ev == EV_TREND_NEXT)
                            ? (g_trend_pet + 1) % cnt
                            : (g_trend_pet + cnt - 1) % cnt;
            }
          refresh_trends();
        }
      else if (ev == EV_TREND_WEEK)
        {
          g_trend_month = 0;
          refresh_trends();
        }
      else if (ev == EV_TREND_MONTH)
        {
          g_trend_month = 1;
          refresh_trends();
        }
      else if (ev == EV_MEAL_SEL)
        {
          g_meal_sel = (g_meal_sel + 1) % VELAPAW_MAX_MEALS;
          refresh_enroll();
          refresh_edit();
        }
      else if (ev == EV_MEAL_HUP || ev == EV_MEAL_HDN ||
               ev == EV_MEAL_MUP || ev == EV_MEAL_MDN)
        {
          int d = (ev == EV_MEAL_HUP) ?  60 :
                  (ev == EV_MEAL_HDN) ? -60 :
                  (ev == EV_MEAL_MUP) ?   5 : -5;
          /* acts on the edit dialog's copy when it is open, else on Enroll's */
          int *slot = &meal_target()[g_meal_sel];
          *slot = ((*slot + d) % 1440 + 1440) % 1440;
          refresh_enroll();
          refresh_edit();
        }
#ifdef CONFIG_VELAPAW_VOICE
      else if (ev == EV_MEAL_VOICE)
        {
          /* Same button starts and aborts.  A dialog the owner cannot stop
           * without waiting out a timeout would be worse than no dialog. */

          if (g_vs.state == VS_IDLE)
            {
              vsched_start();
            }
          else
            {
              vsched_stop(COL_DIM, T(S_VC_CANCEL));
            }
        }
#endif
      else if (ev == EV_EDIT_SAVE)
        {
          edit_save();
        }
      else if (ev == EV_EDIT_CANCEL)
        {
          edit_close();
        }
      else if (ev == EV_EDIT_DELETE)
        {
          /* Two taps: deleting a pet throws away its enrollment AND its whole
           * feeding history, and re-enrolling means re-capturing 5 photos. */
          if (!g_del_armed)
            {
              g_del_armed = 1;
              lv_label_set_text(lv_obj_get_child(g_del_btn, 0), T(S_SURE));
            }
          else
            {
              velapaw_pet_id_t victim = g_edit_pet;
              edit_close();                 /* clears g_edit_pet + disarms */
              do_delete_pet(victim);
              refresh_pets();
              refresh_trends();
              lv_label_set_text_fmt(g_count, T(S_ENROLLED_COUNT_FMT),
                                    velapaw_identity_count());
            }
        }
      else if (ev == EV_LANG)
        {
          /* Flip EN <-> 中, persist, and rebuild the whole tree so every
           * one-shot label picks up the new language + font. The pipeline keeps
           * running; only the LVGL widgets are recreated. */
          g_edit_pet = -1;             /* any open edit dialog is discarded */
          velapaw_lang_set(velapaw_lang_get() == VP_LANG_EN
                           ? VP_LANG_ZH : VP_LANG_EN);
          ui_build();
          last_tab = 99;               /* force header + tab refresh next pass */
          show_frame(&frame);          /* repaint the preview into the new tree */
        }
      else if (ev == EV_CLK_HUP || ev == EV_CLK_HDN ||
               ev == EV_CLK_MUP || ev == EV_CLK_MDN)
        {
          int d = (ev == EV_CLK_HUP) ?  60 :
                  (ev == EV_CLK_HDN) ? -60 :
                  (ev == EV_CLK_MUP) ?   5 : -5;
          wall_set_min(wall_min() + d);   /* wraps inside wall_set_min */
          refresh_enroll();
        }

      long now = mono_ms();

      /* Live camera preview on the tabs that show it (Enroll/Recognize).
       * Paused while the user is touching so scrolling stays smooth; My
       * Pets/Trends don't grab at all. Now safe to run continuously: the
       * camera driver stops the DMA + streaming after every frame, so
       * repeated captures no longer wedge the SoC. */
      /* Enroll pauses when nobody is there; Recognize only slows down (a cat
       * must still be seen with no one touching the screen). */
      if (ui_touch_active() || g_evt != EV_NONE)
        {
          g_last_touch = now;
        }

      int idle_enroll = (tab == 0 && now - g_last_touch > ENROLL_IDLE_MS);
      int interval    = (tab == 1 && g_cand == VELAPAW_PET_UNKNOWN)
                        ? FRAME_IDLE_MS : FRAME_INTERVAL_MS;

      if (idle_enroll != g_enroll_paused)
        {
          g_enroll_paused = idle_enroll;
          if (idle_enroll)
            {
              lv_label_set_text(g_sample_lbl, T(S_PREVIEW_PAUSED));
            }
          else
            {
              refresh_enroll();       /* restores "Sample N of 5" */
              last_frame = 0;         /* repaint immediately on wake */
            }
        }

      /* enroll capture came back from the worker */
      if (g_cap_pending && g_inf_state == 3)
        {
          finish_capture();
        }

      if ((tab == 0 || tab == 1) && !ui_touch_active() && !idle_enroll &&
          g_inf_state == 0 &&        /* don't clobber the frame mid-inference */
          now - last_frame >= interval)
        {
          cam_grab(&frame);
          show_frame(&frame);
          last_frame = now;
        }

      /* On-demand recognition: infer on the Recognize tab until a known pet is
       * identified + handled, then STOP until the user re-enters the tab. A
       * feeder recognizes on approach, not continuously; this bounds inference
       * count per visit to a handful and stays well clear of the sustained-
       * inference limit (the long INT8 Invoke on PSRAM is the scarce path). */
      static int recog_done = 0;
      static int recog_tries = 0;
      if (tab != 1)
        {
          recog_done = 0; recog_tries = 0;        /* re-arm on leave */
          g_fed_visit = VELAPAW_PET_UNKNOWN;      /* returning = a NEW visit: allow
                                                   * another feed. The per-pet cooldown
                                                   * (g_last_fed + cooldown_s) still gates
                                                   * rapid double-feeds. Needed because
                                                   * recog_done stops the "pet left" path
                                                   * that used to reset this. */
          /* drop a stale RECOGNIZE result -- but never an enroll capture that
           * is still waiting to be collected (Enroll is tab 0, i.e. tab != 1) */
          if (g_inf_state == 3 && !g_cap_pending) g_inf_state = 0;
#ifdef CONFIG_VELAPAW_VOICE
          g_voice_dismissed = 0;       /* leaving re-arms voice, like recog */
#endif
        }
      if (tab == 1 && !recog_done)
        {
          if (velapaw_identity_count() == 0)
            {
              lv_obj_set_style_text_color(g_status, lv_color_hex(COL_DIM), 0);
              lv_label_set_text(g_status, T(S_ENROLL_FIRST));
            }
          else if (g_inf_state == 0 && now - last_recog > RECOG_INTERVAL_MS)
            {
              /* Hand the current frame to the background worker; the UI loop keeps
               * running so touch + LVGL stay live during the ~1.3s Invoke. Preview
               * capture is paused (g_inf_state guard) so the shared frame buffer
               * the worker reads isn't overwritten mid-inference. */
              g_inf_frame = frame;
              g_inf_mode  = 0;       /* recognize: embed + match */
              g_inf_state = 1;
              recog_tries++;
            }
          else if (g_inf_state == 3)
            {
              struct velapaw_match m = g_inf_m;   /* result from the worker */
              lv_label_set_text_fmt(g_metrics, T(S_INFER_FMT),
                                    (unsigned long)g_inf_us / 1000,
                                    (unsigned long)g_be->arena_bytes() / 1024);

              if (m.id == VELAPAW_PET_UNKNOWN)
                {
                  g_cand = VELAPAW_PET_UNKNOWN;
                  g_stable = 0;
                  if (++g_absent >= 4)   /* ~5s gone (not a flicker) -> pet left */
                    {
                      g_fed_visit = VELAPAW_PET_UNKNOWN;
                      g_bcs_for = VELAPAW_PET_UNKNOWN;
                    }
                  lv_obj_set_style_text_color(g_status, lv_color_hex(COL_UNK), 0);
                  lv_label_set_text_fmt(g_status, T(S_UNKNOWN_FMT), m.score);
                  lv_label_set_text(g_sub, "");
                }
              else
                {
                  const struct velapaw_pet *pet = velapaw_identity_get(m.id);
                  g_absent = 0;

                  /* Debounce identity flicker: multi-pet matching bounces frame
                   * to frame, so only act once the same id holds for N frames. */
                  if (m.id == g_cand)
                    {
                      g_stable++;
                    }
                  else
                    {
                      g_cand = m.id;
                      g_stable = 1;
                    }

                  if (g_stable < RECOG_STABLE_N)
                    {
                      lv_obj_set_style_text_color(g_status, lv_color_hex(COL_DIM), 0);
                      lv_label_set_text_fmt(g_status, T(S_IDENTIFYING_FMT),
                                            pet->name);
                      lv_label_set_text(g_sub, "");
                    }
                  else
                    {
                      lv_obj_set_style_text_color(g_status, lv_color_hex(COL_TEXT), 0);
                      lv_label_set_text_fmt(g_status, T(S_MATCH_FMT), pet->name,
                                            m.score);
#ifdef VELAPAW_HAVE_BCS
                      /* Majority-vote BCS over the first BCS_VOTES frames of the
                       * visit so one bad frame can't set a wrong score. */
                      if (m.id != g_bcs_for)   /* new pet -> start a fresh vote */
                        {
                          g_bcs_for = m.id;
                          g_bcs_votes[0] = g_bcs_votes[1] = g_bcs_votes[2] = 0;
                          g_bcs_nvotes = 0;
                        }
                      if (g_bcs_nvotes < BCS_VOTES)
                        {
                          int bcls = -1;
                          float bconf = 0.0f;
                          if (velapaw_infer_bcs(&frame, &bcls, &bconf) == 0 &&
                              bcls >= 0 && bcls < 3)
                            {
                              g_bcs_votes[bcls]++;
                              g_bcs_nvotes++;
                              int best = 0;
                              if (g_bcs_votes[1] > g_bcs_votes[best]) best = 1;
                              if (g_bcs_votes[2] > g_bcs_votes[best]) best = 2;
                              velapaw_store_set_bcs(m.id, best);  /* running majority */
                            }
                        }
#endif
                      /* ---- MODEL B: recognition-gated SCHEDULED feeding ----
                       * Recognition decides WHO is at the bowl; the pet's meal
                       * time decides WHEN it may eat. Food is dispensed only if
                       * the meal is due and this pet hasn't eaten it today. */
                      long today = wall_day();
                      int  nowm  = wall_min();
                      int  has_meal = 0;    /* any slot configured?           */
                      int  slot     = -1;   /* a slot that is DUE and unfed   */
                      int  pending  = -1;   /* soonest slot still ahead today */
                      for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
                        {
                          int mm = pet->meal_min[k];
                          if (mm < 0) continue;
                          has_meal = 1;
                          if (g_meal_fed_day[m.id][k] == today) continue; /* eaten */
                          if (g_meal_skipped[m.id][k]) continue; /* owner skipped it */
                          if (nowm >= mm)
                            {
                              /* due: prefer the latest due slot we haven't eaten */
                              if (slot < 0 || mm > pet->meal_min[slot]) slot = k;
                            }
                          else if (pending < 0 || mm < pet->meal_min[pending])
                            {
                              pending = k;   /* next meal still to come */
                            }
                        }
                      int due = (slot >= 0);

                      if (due)
                        {
                          if (velapaw_store_today_total(m.id) >=
                              pet->daily_limit_g)
                            {
                              g_meal_fed_day[m.id][slot] = today;  /* don't re-try */
                              lv_obj_set_style_text_color(g_sub,
                                  lv_color_hex(COL_COOL), 0);
                              lv_label_set_text_fmt(g_sub, T(S_LIMIT_HIT_FMT),
                                  velapaw_store_today_total(m.id),
                                  pet->daily_limit_g);
                            }
                          else
                            {
                              /* trim the serving so today never exceeds the cap */
                              int remaining = pet->daily_limit_g -
                                  velapaw_store_today_total(m.id);
                              int amount = (pet->portion_g < remaining)
                                           ? pet->portion_g : remaining;
                              velapaw_feeder_dispense(amount);
                              { struct timespec _w;
                                clock_gettime(CLOCK_REALTIME, &_w);
                                struct velapaw_feed_event ev2 =
                                  { m.id, (time_t)_w.tv_sec,   /* real wall-clock time */
                                    amount, m.score, g_be->name };
                                velapaw_store_log_feeding(&ev2); }
                              g_last_fed[m.id]           = mono_ms() / 1000;
                              g_meal_fed_day[m.id][slot] = today;  /* this meal done */
                              g_meal_fed_min[m.id][slot] = nowm;   /* ...at this time */
                              g_meal_fed_g[m.id][slot]   = amount; /* ...this much */
                              lv_obj_set_style_text_color(g_sub,
                                  lv_color_hex(COL_FED), 0);
                              lv_label_set_text_fmt(g_sub, T(S_FED_FMT), amount,
                                  slot + 1, VELAPAW_MAX_MEALS,
                                  velapaw_store_today_total(m.id),
                                  pet->daily_limit_g);
                            }
                        }
                      else if (!has_meal)
                        {
                          lv_obj_set_style_text_color(g_sub, lv_color_hex(COL_DIM), 0);
                          lv_label_set_text(g_sub, T(S_NO_MEALS));
                        }
                      else if (pending >= 0)
                        {
                          lv_obj_set_style_text_color(g_sub, lv_color_hex(COL_COOL), 0);
                          lv_label_set_text_fmt(g_sub, T(S_NEXT_MEAL_FMT),
                                                pet->meal_min[pending] / 60,
                                                pet->meal_min[pending] % 60);
                        }
                      else
                        {
                          lv_obj_set_style_text_color(g_sub, lv_color_hex(COL_FED), 0);
                          lv_label_set_text_fmt(g_sub, T(S_ALL_EATEN_FMT),
                              velapaw_store_today_total(m.id));
                        }

                      /* Identified + handled: rest until re-armed (leave/return
                       * to the Recognize tab) so inference count stays bounded. */
                      recog_done = 1;
                    }
                }
              /* No stable ID after a couple of tries -> stop (bounded, no loop). */
              if (!recog_done && recog_tries >= 2)
                {
                  recog_done = 1;
                  lv_obj_set_style_text_color(g_status, lv_color_hex(COL_DIM), 0);
                  lv_label_set_text(g_status, T(S_NO_STABLE));
                }

              g_inf_state = 0;        /* worker idle; allow the next capture+infer */
              last_recog = mono_ms();
            }
        }

#ifdef CONFIG_VELAPAW_VOICE
      /* Voice is a Recognize-tab feature.  Not because the microphone is busy
       * elsewhere, but because that is the only tab where "yes" has a subject
       * -- and because listening while the owner is typing a pet name on
       * Enroll would be a surprise, not a feature.
       *
       * Note this is NOT gated on recog_done.  Recognition stops once a pet is
       * identified and handled, but that is exactly when the owner is most
       * likely to speak, so the microphone stays open for the whole visit. */

      g_voice_run = ((g_vs.state != VS_IDLE) ||
                     (tab == 1 && !g_voice_fail && !g_voice_dismissed)) ? 1 : 0;

      vsched_tick();

      if (g_voice_evt != 0)
        {
          if (g_vs.state != VS_IDLE)
            {
              /* The scheduling dialog owns the microphone while it is up.  It
               * lives in a modal over My Pets, so it can never be contending
               * with the Recognize tab for the same utterance. */

              vsched_step(&g_voice_res);
            }
          else
            {
              /* Only hold the line on screen if something was actually shown.
               * voice_apply now drops words it has no use for, and holding for
               * a drop would keep re-extending a verdict the owner has already
               * read every time they said something unrelated near the bowl. */

              if (voice_apply(g_voice_res.label))
                {
                  g_voice_hold = now + VOICE_MSG_HOLD_MS;
                }
            }

          g_voice_evt = 0;             /* releases the worker to listen again */
        }
      else if (tab == 1 && g_vs.state == VS_IDLE && now > g_voice_hold)
        {
          voice_idle_text();
        }
#endif

      usleep(20000);
    }
}
