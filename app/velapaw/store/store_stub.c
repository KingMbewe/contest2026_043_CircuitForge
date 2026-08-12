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
#include "store/store.h"

#define DAYS VELAPAW_HIST_DAYS
#define TODAY (DAYS - 1)

static int g_day_g[VELAPAW_MAX_PETS][DAYS];      /* grams per day */
static int g_day_m[VELAPAW_MAX_PETS][DAYS];      /* meals per day */
static int g_bcs[VELAPAW_MAX_PETS];              /* last BCS: 0/1/2, -1 none */

static int valid(velapaw_pet_id_t id)
{
  return id >= 0 && id < VELAPAW_MAX_PETS;
}

int velapaw_store_init(void)
{
  memset(g_day_g, 0, sizeof(g_day_g));
  memset(g_day_m, 0, sizeof(g_day_m));
  for (int i = 0; i < VELAPAW_MAX_PETS; i++)
    {
      g_bcs[i] = -1;
    }
  return 0;
}

int velapaw_store_log_feeding(const struct velapaw_feed_event *ev)
{
  if (ev == NULL || !valid(ev->pet_id))
    {
      return -1;
    }

  g_day_g[ev->pet_id][TODAY] += ev->grams;
  g_day_m[ev->pet_id][TODAY] += 1;

  printf("    [store] event pet=%d grams=%d score=%.2f model=%s\n",
         ev->pet_id, ev->grams, ev->score, ev->model ? ev->model : "?");
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
}

void velapaw_store_roll_day(void)
{
  for (int p = 0; p < VELAPAW_MAX_PETS; p++)
    {
      for (int d = 0; d < TODAY; d++)
        {
          g_day_g[p][d] = g_day_g[p][d + 1];
          g_day_m[p][d] = g_day_m[p][d + 1];
        }
      g_day_g[p][TODAY] = 0;
      g_day_m[p][TODAY] = 0;
    }
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
}

void velapaw_store_set_bcs(velapaw_pet_id_t id, int bcs)
{
  if (valid(id))
    {
      g_bcs[id] = bcs;
    }
}

int velapaw_store_get_bcs(velapaw_pet_id_t id)
{
  return valid(id) ? g_bcs[id] : -1;
}
