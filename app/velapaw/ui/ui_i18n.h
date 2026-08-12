/****************************************************************************
 * VelaPaw - UI internationalisation (EN / 中文)
 *
 * One string table, one runtime language switch, one font seam.
 *
 *   T(id)              -> the string for `id` in the current language
 *   velapaw_lang_get() -> 0 = English, 1 = 中文
 *   velapaw_lang_set() -> switch language (persists to flash)
 *   velapaw_font(lvl)  -> the font to use RIGHT NOW for a given text level
 *
 * The font accessor is the ONLY place that needs to change to render Chinese:
 * the stock Montserrat fonts have no CJK glyphs, so today velapaw_font() always
 * returns Montserrat and Chinese would render as blank boxes. Build with
 * CONFIG_VELAPAW_FONT_CJK=y (and link a generated CJK font blob, see
 * ui/README_I18N.md) and velapaw_font() returns the CJK font instead -- every
 * label in the UI picks it up automatically because they all go through here.
 ****************************************************************************/

#ifndef VELAPAW_UI_I18N_H
#define VELAPAW_UI_I18N_H

#include <lvgl/lvgl.h>

/* Language codes. Index into every row of the string table. */
enum { VP_LANG_EN = 0, VP_LANG_ZH = 1, VP_LANG_N };

/* Text "levels" -- mirror the old F_TITLE/F_HEAD/F_BODY/F_SMALL sizes. */
enum { VP_FONT_TITLE, VP_FONT_HEAD, VP_FONT_BODY, VP_FONT_SMALL };

/* String IDs. Keep in lock-step with g_strtab[] in ui_i18n.c.
 * Entries whose value contains a printf conversion (%d, %s, %.2f, ...) are used
 * with lv_label_set_text_fmt and MUST keep the same conversions, in an order
 * the target language can honour, in BOTH languages. */
enum
{
  S_BRAND = 0,          /* "VelaPaw" (brand -- same in both, centralised) */

  /* tabs + header */
  S_TAB_ENROLL, S_TAB_RECOGNIZE, S_TAB_PETS, S_TAB_TRENDS,
  S_CRUMB_FMT,          /* "VelaPaw %02d:%02d  -  %s"  (clock + tab name)  */

  /* enroll */
  S_PET_NAME, S_SAMPLE_FMT, S_CAPTURING, S_CAP_AT_LEAST, S_PREVIEW_PAUSED,
  S_PORTION_FMT, S_DAILY_LIMIT_FMT, S_MEALS, S_CLOCK_NOW_FMT,
  S_CAPTURE, S_SAVE_PET, S_ENROLLED, S_ENROLLED_MSG_FMT, S_OK,

  /* recognize */
  S_POINT_AT_PET, S_ENROLL_FIRST, S_UNKNOWN_FMT, S_IDENTIFYING_FMT,
  S_MATCH_FMT, S_NO_STABLE,
  S_NO_MEALS, S_NEXT_MEAL_FMT, S_ALL_EATEN_FMT, S_LIMIT_HIT_FMT, S_FED_FMT,

  /* voice (keyword spotting) -- the Recognize tab's second line.
   * The keywords themselves stay ASCII "yes"/"no" in BOTH languages on
   * purpose: micro_speech is trained on the English words, so that is what
   * the owner has to say whichever language the UI is in.  Quoting them
   * rather than translating them is the honest instruction.
   * The 中文 column here is built only from glyphs already present in the
   * subset font (see README_I18N.md) -- a new character would render blank. */
  S_VOICE_PROMPT, S_VOICE_HEARD_FMT, S_VOICE_FED_FMT, S_VOICE_CANCELLED,
  S_VOICE_NO_PET, S_VOICE_OFF,

  /* voice scheduling dialog (edit-pet modal).  Same rule as above and it
   * bites harder here: the owner speaks DIGITS, and the model only knows the
   * English ones, so the prompts quote "1 / 2 / 3" and "0-9" verbatim in both
   * columns.  Every 中文 glyph below was checked against the shipped subset
   * before being written -- an unshipped glyph renders blank with no build
   * error, so "语音" for the button is deliberately absent and the button is
   * an LVGL symbol in both languages. */
  S_VC_MEAL, S_VC_HOUR_FMT, S_VC_CONFIRM_FMT, S_VC_SAVED_FMT,
  S_VC_RETRY, S_VC_CANCEL, S_VC_LISTEN,

  /* notifications (bell) */
  S_NOTIFICATIONS, S_ALL_NORMAL, S_CLOSE,
  S_ALERT_APPETITE_FMT, S_ALERT_BCS_FMT, S_UNDERWEIGHT, S_OVERWEIGHT,

  /* my pets */
  S_ENROLLED_PETS, S_NO_PETS, S_ENROLLED_COUNT_FMT,
  S_TODAY_FMT, S_LIMIT_REACHED, S_WAITING, S_PENDING, S_ATE_FMT, S_SKIPPED,
  S_NO_MEALS_SCHED, S_AVG_WEEK_FMT, S_APPETITE_FMT, S_APPETITE_DROP_FMT,
  S_BCS_FMT, S_BCS_VET_FMT, S_BCS_UNDER, S_BCS_IDEAL, S_BCS_OVER, S_BCS_NA,
  S_BCS_TITLE,          /* gauge card heading */
  S_EDIT_SCHEDULE, S_BCS_DISCLAIMER, S_NEXT_DAY,

  /* edit-pet modal */
  S_EDIT_PET, S_EDIT_TITLE_FMT, S_CANCEL, S_DELETE, S_SURE, S_SAVE,

  /* trends */
  S_TRENDS, S_TRENDS_ENROLL_FIRST, S_TREND_TITLE_FMT, S_WIN_7D, S_WIN_30D,
  S_APPETITE_TREND,     /* appetite line-chart card heading */
  S_PET_PREV, S_PET_NEXT, S_TAP_BAR, S_TREND_STATS_FMT,
  S_TODAY_VAL_FMT, S_YDAY_VAL_FMT, S_NDAYS_VAL_FMT,

  /* misc */
  S_BACKEND_FMT, S_INFER_FMT, S_LANG_TOGGLE,

  S_COUNT
};

/* Current-language string for `id`. Never returns NULL. */
const char *velapaw_tr(int id);
#define T(id) velapaw_tr(id)

int  velapaw_lang_get(void);           /* VP_LANG_EN / VP_LANG_ZH */
void velapaw_lang_set(int lang);       /* switch + persist to flash */
void velapaw_lang_load(void);          /* restore persisted choice (call at boot) */

/* The font to use right now for a given text level. Single CJK seam. */
const lv_font_t *velapaw_font(int level);

#endif /* VELAPAW_UI_I18N_H */
