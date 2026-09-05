/****************************************************************************
 * VelaPaw - in-memory store backend (emulator) + feeding-behavior analytics
 *
 * Keeps a rolling 7-day history of grams + meal counts per pet (index
 * DAYS-1 == today). Powers daily/weekly intake, meal frequency, appetite
 * baseline/trend, and the sudden-appetite-drop alert. Replace with a
 * persistent backend (apps/system/settings or littlefs) on hardware so
 * history survives reboots.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "store/store.h"
#include "identity/identity.h"

#define DAYS VELAPAW_HIST_DAYS
#define TODAY (DAYS - 1)

static int g_day_g[VELAPAW_MAX_PETS][DAYS];      /* grams per day */
static int g_day_m[VELAPAW_MAX_PETS][DAYS];      /* meals per day */
static int g_bcs[VELAPAW_MAX_PETS];              /* last BCS: 0/1/2, -1 none */

/* Civil day number (see vp_days_from_civil) that owns slot TODAY, or -1
 * when it is not known yet: an unset clock, or a pre-VP02 history file.
 */
static long g_hist_day = -1;

static int valid(velapaw_pet_id_t id)
{
  return id >= 0 && id < VELAPAW_MAX_PETS;
}

/****************************************************************************
 * Agent-readable persistence
 *
 * The on-device ai_agent needs real feeding data to summarise, and its
 * read_file tool is sandboxed to the agent data dir, so everything lands
 * under <agent data dir>/velapaw/:
 *
 *   history.bin  compact machine copy of the rolling window, reloaded at
 *                init so the history survives a reboot
 *   status.md    the digest a skill reads: per-pet totals, baseline,
 *                appetite %, body condition, drop alert
 *   feed_log.md  append-only recent events, size-capped
 *
 * Doing this from the velapaw task directly is safe: since the ai_agent
 * merge every task stack is carved from the XTENSA_IMEM heap and lives in
 * internal SRAM (measured: velapaw StackBase 0x3fcabc98), so a littlefs
 * write no longer runs with the stack sitting behind the disabled flash
 * cache -- the freeze that identity.c's worker thread exists to dodge.
 * If a stack ever moves back to PSRAM (0x3c......), these calls must move
 * onto that worker-thread pattern too.
 *
 * Every write is best-effort.  If /data is not mounted the calls fail
 * quietly and the in-memory analytics behave exactly as they did before.
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#  define VP_AGENT_DIR CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#else
#  define VP_AGENT_DIR "/data/ai_agent"
#endif

#define VP_DIR      VP_AGENT_DIR "/velapaw"
#define VP_HISTORY  VP_DIR "/history.bin"
#define VP_STATUS   VP_DIR "/status.md"
#define VP_FEEDLOG  VP_DIR "/feed_log.md"

/* read_file tops out at 32 KB and the /data partition is under 1 MB. */
#define VP_LOG_MAX  8192
#define VP_MAGIC_V1 0x56503031u        /* "VP01" -- before the day stamp */
#define VP_MAGIC    0x56503032u        /* "VP02" */

/* Below this the realtime clock is clearly unset; printing a 1970 date
 * would mislead the model, so the raw epoch is logged instead. */
#define VP_TS_SANE  1600000000

static bool g_fs_ready;

static bool vp_ensure_dir(void)
{
  if (g_fs_ready)
    {
      return true;
    }

  if (mkdir(VP_AGENT_DIR, 0755) != 0 && errno != EEXIST)
    {
      return false;
    }

  if (mkdir(VP_DIR, 0755) != 0 && errno != EEXIST)
    {
      return false;
    }

  g_fs_ready = true;
  return true;
}

static void vp_history_save(void)
{
  if (!vp_ensure_dir())
    {
      return;
    }

  FILE *f = fopen(VP_HISTORY, "wb");
  if (f == NULL)
    {
      return;
    }

  /* hdr[3] is the civil day owning slot TODAY, -1 when unknown. */
  uint32_t hdr[4] =
    {
      VP_MAGIC, VELAPAW_MAX_PETS, DAYS, (uint32_t)(int32_t)g_hist_day
    };

  if (fwrite(hdr, sizeof(hdr), 1, f) == 1)
    {
      fwrite(g_day_g, sizeof(g_day_g), 1, f);
      fwrite(g_day_m, sizeof(g_day_m), 1, f);
      fwrite(g_bcs,   sizeof(g_bcs),   1, f);
    }

  fclose(f);
}

static void vp_history_load(void)
{
  FILE *f = fopen(VP_HISTORY, "rb");
  if (f == NULL)
    {
      return;
    }

  uint32_t hdr[4];
  bool ok;

  /* Geometry is part of the format: a rebuild that changes MAX_PETS or the
   * window length invalidates the file instead of misreading it. */
  ok = fread(hdr, sizeof(uint32_t), 3, f) == 3
       && hdr[1] == VELAPAW_MAX_PETS
       && hdr[2] == DAYS;

  if (ok && hdr[0] == VP_MAGIC)
    {
      ok = fread(&hdr[3], sizeof(uint32_t), 1, f) == 1;
      g_hist_day = ok ? (long)(int32_t)hdr[3] : -1;
    }
  else if (ok && hdr[0] == VP_MAGIC_V1)
    {
      /* Pre-day-stamp file: the arrays are still good, the day simply was
       * never recorded.  vp_sync_day() adopts today without shifting,
       * rather than guessing and throwing real days away. */
      g_hist_day = -1;
    }
  else
    {
      ok = false;
    }

  if (ok)
    {
      if (fread(g_day_g, sizeof(g_day_g), 1, f) != 1
          || fread(g_day_m, sizeof(g_day_m), 1, f) != 1
          || fread(g_bcs, sizeof(g_bcs), 1, f) != 1)
        {
          /* Short or corrupt: a clean slate beats a half-loaded history. */
          memset(g_day_g, 0, sizeof(g_day_g));
          memset(g_day_m, 0, sizeof(g_day_m));
          for (int i = 0; i < VELAPAW_MAX_PETS; i++)
            {
              g_bcs[i] = -1;
            }

          printf("velapaw/store: %s corrupt, history reset\n", VP_HISTORY);
        }
      else
        {
          printf("velapaw/store: history restored from %s\n", VP_HISTORY);
        }
    }

  fclose(f);
}

static void vp_status_write(void)
{
  static const char *bcs_txt[] =
    {
      "under", "ideal", "over"
    };

  if (!vp_ensure_dir())
    {
      return;
    }

  FILE *f = fopen(VP_STATUS, "w");
  if (f == NULL)
    {
      return;
    }

  fputs("# VelaPaw Feeding Status\n\n", f);
  fprintf(f, "Rolling %d-day window. Grams dispensed by the feeder to each "
             "recognised pet.\n\n", DAYS);

  int n = velapaw_identity_count();
  if (n <= 0)
    {
      fputs("No pets enrolled yet.\n", f);
      fclose(f);
      return;
    }

  for (velapaw_pet_id_t id = 0; id < n && id < VELAPAW_MAX_PETS; id++)
    {
      const struct velapaw_pet *p = velapaw_identity_get(id);
      const char *name = (p != NULL && p->name[0]) ? p->name : "(unnamed)";
      int b = g_bcs[id];

      fprintf(f, "## %s\n", name);
      fprintf(f, "- today: %d g in %d meal(s)\n",
              velapaw_store_today_total(id), velapaw_store_today_meals(id));
      fprintf(f, "- last 7 days: %d g\n", velapaw_store_week_total(id));
      fprintf(f, "- baseline (average of prior days): %d g/day\n",
              velapaw_store_baseline_g(id));
      fprintf(f, "- appetite today: %d%% of baseline\n",
              velapaw_store_appetite_pct(id));
      fprintf(f, "- body condition: %s\n",
              (b >= 0 && b <= 2) ? bcs_txt[b] : "not scored");
      fprintf(f, "- owner daily limit: %d g\n", p != NULL ? p->daily_limit_g : 0);

      if (velapaw_store_alert_drop(id))
        {
          fputs("- ALERT: intake on the last completed day was under half "
                "the baseline, an early illness sign worth flagging to the "
                "owner\n", f);
        }

      fputs("\n", f);
    }

  fclose(f);
}

static void vp_feedlog_append(const struct velapaw_feed_event *ev)
{
  struct stat st;
  bool fresh;
  char when[40];

  if (!vp_ensure_dir())
    {
      return;
    }

  /* Restart rather than grow without bound. */
  fresh = (stat(VP_FEEDLOG, &st) != 0) || (st.st_size > VP_LOG_MAX);

  FILE *f = fopen(VP_FEEDLOG, fresh ? "w" : "a");
  if (f == NULL)
    {
      return;
    }

  if (fresh)
    {
      fputs("# VelaPaw Feed Log\n\n"
            "Most recent feeding events, newest last.\n\n", f);
    }

  if (ev->ts > VP_TS_SANE)
    {
      struct tm tmv;
      localtime_r(&ev->ts, &tmv);
      strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
    }
  else
    {
      snprintf(when, sizeof(when), "epoch %ld (clock unset)", (long)ev->ts);
    }

  const struct velapaw_pet *p = velapaw_identity_get(ev->pet_id);

  fprintf(f, "- [%s] %s fed %d g (match %.2f, model %s)\n",
          when,
          (p != NULL && p->name[0]) ? p->name : "(unnamed)",
          ev->grams, ev->score, ev->model ? ev->model : "?");

  fclose(f);
}

/* Anything that changes the window rewrites both the machine copy and the
 * digest, so the agent never reads a stale status.md. */
static void vp_persist(void)
{
  vp_history_save();
  vp_status_write();
}

/****************************************************************************
 * Day stamping
 *
 * The window is an array whose last slot is "today", so something has to
 * move it.  Until now the only thing that did was the EV_NEXTDAY demo
 * button, which means a board left running past midnight kept adding to the
 * same slot for ever.  Over-reporting intake makes the feeder withhold real
 * meals, so this is the dangerous direction to get wrong.
 ****************************************************************************/

static long vp_days_from_civil(int y, unsigned m, unsigned d)
{
  /* Exact day number across month and year boundaries.  tm_yday alone is
   * not enough: 12-31 -> 01-01 has to be a delta of 1, not -364.
   */
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

static long vp_today_num(void)
{
  time_t now = time(NULL);
  struct tm tmv;

  /* Same sanity floor the feed log uses: an unset clock must never be
   * allowed to shift real history.
   */
  if (now < VP_TS_SANE)
    {
      return -1;
    }

  localtime_r(&now, &tmv);
  return vp_days_from_civil(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static void vp_shift_days(int n)
{
  if (n <= 0)
    {
      return;
    }

  if (n > DAYS)
    {
      n = DAYS;
    }

  for (int p = 0; p < VELAPAW_MAX_PETS; p++)
    {
      int d = 0;
      for (; d < DAYS - n; d++)
        {
          g_day_g[p][d] = g_day_g[p][d + n];
          g_day_m[p][d] = g_day_m[p][d + n];
        }

      for (; d < DAYS; d++)
        {
          g_day_g[p][d] = 0;
          g_day_m[p][d] = 0;
        }
    }
}

/* Reconcile the window with the wall clock.  Safe on every path that
 * touches the history -- it is a no-op within a single day.
 */
static void vp_sync_day(void)
{
  long today = vp_today_num();

  if (today < 0)
    {
      return;              /* clock unusable -- leave the window alone */
    }

  if (g_hist_day < 0)
    {
      /* First run, or a VP01 file: the data in the window is real, we just
       * never knew which day it belonged to.  Adopt today, do not shift.
       */
      g_hist_day = today;
      vp_history_save();
      return;
    }

  if (today <= g_hist_day)
    {
      /* Same day, or the clock jumped backwards -- which the build-date
       * boot anchor does on every cold start.  Never shift backwards: it
       * would drop real days off the front and zero slots that hold data.
       */
      return;
    }

  vp_shift_days((int)(today - g_hist_day));
  g_hist_day = today;
  vp_history_save();
}

int velapaw_store_init(void)
{
  memset(g_day_g, 0, sizeof(g_day_g));
  memset(g_day_m, 0, sizeof(g_day_m));
  for (int i = 0; i < VELAPAW_MAX_PETS; i++)
    {
      g_bcs[i] = -1;
    }

  vp_history_load();

  /* A board that has been off -- or simply running -- across midnight has
   * to shift before it reports anything.
   */
  vp_sync_day();

  /* Write the digest at startup too.  Without this, status.md does not exist
   * until the next feed, enroll or BCS update, so a freshly booted board
   * gives the agent's skill nothing to read. */
  vp_status_write();
  return 0;
}

int velapaw_store_log_feeding(const struct velapaw_feed_event *ev)
{
  if (ev == NULL || !valid(ev->pet_id))
    {
      return -1;
    }

  /* Bucket the event in the right day even if the board has been up since
   * before midnight.
   */
  vp_sync_day();

  g_day_g[ev->pet_id][TODAY] += ev->grams;
  g_day_m[ev->pet_id][TODAY] += 1;

  printf("    [store] event pet=%d grams=%d score=%.2f model=%s\n",
         ev->pet_id, ev->grams, ev->score, ev->model ? ev->model : "?");

  vp_feedlog_append(ev);
  vp_persist();
  return 0;
}

int velapaw_store_today_total(velapaw_pet_id_t id)
{
  return valid(id) ? g_day_g[id][TODAY] : 0;
}

int velapaw_store_today_meals(velapaw_pet_id_t id)
{
  return valid(id) ? g_day_m[id][TODAY] : 0;
}

int velapaw_store_week_total(velapaw_pet_id_t id)
{
  if (!valid(id))
    {
      return 0;
    }
  int sum = 0;
  for (int d = 0; d < VELAPAW_WEEK_DAYS && d <= TODAY; d++)
    {
      sum += g_day_g[id][TODAY - d];   /* last 7 days incl. today */
    }
  return sum;
}

int velapaw_store_month_total(velapaw_pet_id_t id)
{
  if (!valid(id))
    {
      return 0;
    }
  int sum = 0;
  for (int d = 0; d < DAYS; d++)
    {
      sum += g_day_g[id][d];
    }
  return sum;
}

int velapaw_store_day_total(velapaw_pet_id_t id, int day_back)
{
  if (!valid(id) || day_back < 0 || day_back >= DAYS)
    {
      return 0;
    }
  return g_day_g[id][TODAY - day_back];
}

/* average of the prior days (everything except today) */
int velapaw_store_baseline_g(velapaw_pet_id_t id)
{
  if (!valid(id))
    {
      return 0;
    }
  int sum = 0;
  for (int d = 0; d < TODAY; d++)
    {
      sum += g_day_g[id][d];
    }
  return sum / (DAYS - 1);
}

int velapaw_store_appetite_pct(velapaw_pet_id_t id)
{
  int base = velapaw_store_baseline_g(id);
  if (base <= 0)
    {
      return 100;
    }
  return g_day_g[id][TODAY] * 100 / base;
}

/* Alert on the most recent *completed* day (yesterday = TODAY-1) vs the
 * baseline of the days before it: a >50% drop is the early illness sign.
 */
int velapaw_store_alert_drop(velapaw_pet_id_t id)
{
  if (!valid(id) || DAYS < 3)
    {
      return 0;
    }
  int last = g_day_g[id][TODAY - 1];
  int sum = 0;
  for (int d = 0; d < TODAY - 1; d++)
    {
      sum += g_day_g[id][d];
    }
  int base = sum / (DAYS - 2);
  return (base > 0 && last * 2 < base) ? 1 : 0;
}

void velapaw_store_seed(velapaw_pet_id_t id, int daily_g)
{
  if (!valid(id) || daily_g <= 0)
    {
      return;
    }
  /* fill the prior days with a normal baseline (deterministic +/- noise);
   * leave today (TODAY) at 0 so live feeding fills it in.
   */
  for (int d = 0; d < TODAY; d++)
    {
      int noise = ((id * 3 + d * 5) % 21) - 10;   /* -10..+10 g */
      int g = daily_g + noise;
      g_day_g[id][d] = g > 0 ? g : 0;
      g_day_m[id][d] = 2 + ((id + d) % 3);          /* 2..4 meals */
    }

  /* Seeding happens at enrollment, which is the first moment a pet exists
   * to write a digest about. */
  vp_persist();
}

void velapaw_store_roll_day(void)
{
  vp_shift_days(1);

  /* Keep the stamp in step, or the next clock reconcile would shift a
   * second time for the same day.
   */
  if (g_hist_day >= 0)
    {
      g_hist_day++;
    }

  vp_persist();
}

/* Drop a pet's history and shift the rest down. Pet ids are array indices, so
 * velapaw_identity_delete() renumbers every later pet -- this keeps the history
 * attached to the right animal instead of silently inheriting it. */
void velapaw_store_delete(velapaw_pet_id_t id)
{
  if (!valid(id))
    {
      return;
    }

  for (int p = id; p < VELAPAW_MAX_PETS - 1; p++)
    {
      memcpy(g_day_g[p], g_day_g[p + 1], sizeof(g_day_g[0]));
      memcpy(g_day_m[p], g_day_m[p + 1], sizeof(g_day_m[0]));
      g_bcs[p] = g_bcs[p + 1];
    }

  memset(g_day_g[VELAPAW_MAX_PETS - 1], 0, sizeof(g_day_g[0]));
  memset(g_day_m[VELAPAW_MAX_PETS - 1], 0, sizeof(g_day_m[0]));
  g_bcs[VELAPAW_MAX_PETS - 1] = -1;

  vp_persist();
}

void velapaw_store_set_bcs(velapaw_pet_id_t id, int bcs)
{
  if (valid(id))
    {
      g_bcs[id] = bcs;
      vp_persist();
    }
}

int velapaw_store_get_bcs(velapaw_pet_id_t id)
{
  return valid(id) ? g_bcs[id] : -1;
}
