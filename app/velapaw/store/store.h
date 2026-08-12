/****************************************************************************
 * VelaPaw - persistence + feeding-behavior analytics
 *
 * Backend:
 *   store_stub.c  - in-memory (emulator dev); keeps a 7-day per-pet history
 *   real          - apps/system/settings (KVDB) or littlefs files (TODO:
 *                   persist across reboots on hardware)
 ****************************************************************************/

#ifndef VELAPAW_STORE_H
#define VELAPAW_STORE_H

#include <time.h>
#include "velapaw.h"

#define VELAPAW_HIST_DAYS 30     /* rolling window; index DAYS-1 == today */
#define VELAPAW_WEEK_DAYS 7

struct velapaw_feed_event
{
  velapaw_pet_id_t pet_id;
  time_t           ts;
  int              grams;
  float            score;
  const char      *model;
};

int velapaw_store_init(void);
int velapaw_store_log_feeding(const struct velapaw_feed_event *ev);

/* ---- feeding-behavior analytics (per pet) ---- */

/* Grams / meals fed to `pet_id` today. */
int velapaw_store_today_total(velapaw_pet_id_t pet_id);
int velapaw_store_today_meals(velapaw_pet_id_t pet_id);

/* Total grams over the last 7 days / the full 30-day window. */
int velapaw_store_week_total(velapaw_pet_id_t pet_id);
int velapaw_store_month_total(velapaw_pet_id_t pet_id);

/* Average daily grams over the *prior* days (baseline, excludes today). */
int velapaw_store_baseline_g(velapaw_pet_id_t pet_id);

/* Today's intake as a percentage of the baseline (100 == normal). */
int velapaw_store_appetite_pct(velapaw_pet_id_t pet_id);

/* Grams on a past day: day_back=0 today, 1 yesterday, ... (for sparklines). */
int velapaw_store_day_total(velapaw_pet_id_t pet_id, int day_back);

/* 1 if the most recent *completed* day was a sudden appetite drop
 * (< half the baseline of the days before it) - early illness sign.
 */
int velapaw_store_alert_drop(velapaw_pet_id_t pet_id);

/* Seed a pet's prior days with a normal baseline so trends/alerts are
 * demonstrable immediately (today starts at 0). daily_g ~ expected per day.
 */
void velapaw_store_seed(velapaw_pet_id_t pet_id, int daily_g);

/* Advance the rolling window by one day (clock tick / demo "Next Day"):
 * today's totals shift into history and a fresh day begins.
 */
void velapaw_store_roll_day(void);

/* Last Body Condition Score seen for a pet: 0 under / 1 ideal / 2 over,
 * -1 if never scored. Set from the on-device BCS model.
 */
void velapaw_store_set_bcs(velapaw_pet_id_t pet_id, int bcs);
int  velapaw_store_get_bcs(velapaw_pet_id_t pet_id);

/* Drop a pet's history and shift later pets down one slot, to match the
 * renumbering done by velapaw_identity_delete(). */
void velapaw_store_delete(velapaw_pet_id_t pet_id);

#endif /* VELAPAW_STORE_H */
