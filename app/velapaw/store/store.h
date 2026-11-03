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

/* Average daily grams over the prior days the pet was fed (empty days, e.g.
 * before tracking began, don't count); 0 if none.  Excludes today. */
int velapaw_store_baseline_g(velapaw_pet_id_t pet_id);

/* Today's intake as a percentage of the meals due so far (the due_g of
 * velapaw_store_set_plan()); 100 while none is due, never above 100. */
int velapaw_store_appetite_pct(velapaw_pet_id_t pet_id);

/* Grams on a past day: day_back=0 today, 1 yesterday, ... (for sparklines). */
int velapaw_store_day_total(velapaw_pet_id_t pet_id, int day_back);

/* 1 if the last day that ended with the board running (midnight, or the demo
 * "Next Day") got under half its plan - early illness sign.  The verdict
 * comes from velapaw_store_judge_day() and a boot starts with none.
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

/* Rewrite status.md from the current pets.  Meal times, names and limits live
 * in identity, which no store call sees change -- whoever changes them calls
 * this after velapaw_identity_save(), or the agent reads the old values. */
void velapaw_store_status_refresh(void);

/* Reconcile the rolling window with the wall clock.  Only feeds and boot did
 * this, so a board left running past midnight kept yesterday as "today" until
 * the next feed.  The UI calls it when the day number changes.  UI thread
 * only. */
void velapaw_store_sync_day(void);

/* The day number (days since 1970-01-01) that "today" in the window belongs
 * to, or -1 before the first reconcile.  Can be ahead of the clock (the demo
 * "Next Day" button).  The boot clock floor starts the day after it. */
long velapaw_store_hist_day(void);

/* The UI's plan for a pet today: portion x meals set, minus the meals the
 * owner skipped, at most the daily limit; due_g is the part of it due by
 * now.  status.md prints both.  RAM only.  1 if either changed. */
int  velapaw_store_set_plan(velapaw_pet_id_t pet_id, int plan_g, int due_g);

/* Judge the day that just ended (yesterday's grams) against the plan it had;
 * plan_g <= 0 clears the verdict.  RAM only, so a boot -- every unplug, on
 * this battery-less board -- clears it too. */
void velapaw_store_judge_day(velapaw_pet_id_t pet_id, int plan_g);

/* % of its plan the last judged day got, -1 if none since boot. */
int  velapaw_store_last_day_pct(velapaw_pet_id_t pet_id);

/* Prior days in the window with any food logged (what the baseline averages). */
int  velapaw_store_fed_days(velapaw_pet_id_t pet_id);

/* Small UI state that must survive a reset, kept as a file under the velapaw
 * data dir.  put: 0, or -1 if any of it failed.  get: bytes read, or -1. */
int  velapaw_store_put(const char *name, const void *buf, size_t len);
int  velapaw_store_get(const char *name, void *buf, size_t len);

/* Append one line to health.log (boots, the 10-minute health line, stalls).
 * At 4 KB it becomes health.old and a new log starts.  Any thread. */
void velapaw_store_log_health(const char *line);

#endif /* VELAPAW_STORE_H */
