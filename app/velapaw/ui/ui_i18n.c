/****************************************************************************
 * VelaPaw - UI internationalisation (EN / 中文)  -- see ui_i18n.h
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>

#include "ui/ui_i18n.h"

/* ---- string table --------------------------------------------------------
 * One row per S_* id, { English, 中文 }. Rows whose text carries printf
 * conversions keep the SAME conversions in both columns (order may differ only
 * where the language allows the args in that order). LV_SYMBOL_* are compile
 * time string literals, so they concatenate here just like at the call site.
 * ------------------------------------------------------------------------- */
static const char *const g_strtab[S_COUNT][VP_LANG_N] =
{
  [S_BRAND]            = { "VelaPaw", "VelaPaw" },

  /* tabs + header */
  [S_TAB_ENROLL]       = { "Enroll",    "录入" },
  [S_TAB_RECOGNIZE]    = { "Recognize", "识别" },
  [S_TAB_PETS]         = { "My Pets",   "我的宠物" },
  [S_TAB_TRENDS]       = { "Trends",    "趋势" },
  [S_CRUMB_FMT]        = { "VelaPaw %02d:%02d  -  %s",
                           "VelaPaw %02d:%02d  -  %s" },

  /* enroll */
  [S_PET_NAME]         = { "Pet name", "宠物名称" },
  [S_SAMPLE_FMT]       = { "Sample %d of %d", "样本 %d / %d" },
  [S_CAPTURING]        = { "capturing...", "拍摄中..." },
  [S_CAP_AT_LEAST]     = { "capture at least 1 sample", "至少拍摄 1 张样本" },
  [S_PREVIEW_PAUSED]   = { "preview paused - tap to resume", "预览已暂停 - 点击继续" },
  [S_PORTION_FMT]      = { "Portion: %d g", "每份: %d 克" },
  [S_DAILY_LIMIT_FMT]  = { "Daily limit: %d g", "每日上限: %d 克" },
  [S_MEALS]            = { LV_SYMBOL_BELL " Meals:", LV_SYMBOL_BELL " 用餐:" },
  [S_CLOCK_NOW_FMT]    = { "Clock now: %02d:%02d  (set it)",
                           "当前时间: %02d:%02d  (点此设置)" },
  [S_CAPTURE]          = { LV_SYMBOL_IMAGE " Capture", LV_SYMBOL_IMAGE " 拍摄" },
  [S_SAVE_PET]         = { LV_SYMBOL_OK " Save Pet", LV_SYMBOL_OK " 保存宠物" },
  [S_ENROLLED]         = { LV_SYMBOL_OK "  Enrolled", LV_SYMBOL_OK "  已录入" },
  [S_ENROLLED_MSG_FMT] = { "%s enrolled. The feeder can now recognize this pet.",
                           "%s 已录入。喂食器现在可以识别它了。" },
  [S_OK]               = { "OK", "确定" },

  /* recognize */
  [S_POINT_AT_PET]     = { "Point at a pet", "请对准宠物" },
  [S_ENROLL_FIRST]     = { "Enroll a pet first", "请先录入宠物" },
  [S_UNKNOWN_FMT]      = { "Unknown (%.2f)", "未识别 (%.2f)" },
  [S_IDENTIFYING_FMT]  = { "identifying %s ...", "正在识别 %s ..." },
  [S_MATCH_FMT]        = { "%s  (%.2f)", "%s  (%.2f)" },
  [S_NO_STABLE]        = { "no stable match - re-enter tab to retry",
                           "未稳定匹配 - 重新进入该页重试" },
  [S_NO_MEALS]         = { "no meals scheduled", "未安排用餐" },
  [S_NEXT_MEAL_FMT]    = { "next meal at %02d:%02d", "下一餐 %02d:%02d" },
  [S_ALL_EATEN_FMT]    = { "all meals eaten today (%dg)", "今日各餐已吃完 (%d克)" },
  [S_LIMIT_HIT_FMT]    = { "daily limit reached (%d/%dg)", "已达每日上限 (%d/%d克)" },
  [S_FED_FMT]          = { "Fed %dg (meal %d of %d, today %d/%dg)",
                           "已喂 %d克 (第%d/%d餐, 今日%d/%d克)" },

  /* voice */
  [S_VOICE_PROMPT]     = { "say \"yes\" to feed  -  \"no\" to dismiss",
                           "\"yes\" 喂食  -  \"no\" 取消" },
  [S_VOICE_HEARD_FMT]  = { "heard \"%s\" (%.2f)", "识别 \"%s\" (%.2f)" },
  [S_VOICE_FED_FMT]    = { LV_SYMBOL_OK " \"yes\": fed %dg (today %d/%dg)",
                           LV_SYMBOL_OK " \"yes\": 已喂 %d克 (今日%d/%d克)" },
  [S_VOICE_CANCELLED]  = { "\"no\": dismissed", "\"no\": 取消" },
  [S_VOICE_NO_PET]     = { "no pet identified yet", "未识别宠物" },
  [S_VOICE_OFF]        = { "voice off (mic unavailable)", "停用" },

  /* voice scheduling dialog.  The ASCII hyphen in S_VC_CONFIRM_FMT is not a
   * typo -- the em-dash used elsewhere in this file is outside the shipped
   * glyph subset and would render blank. */
  [S_VC_MEAL]          = { "which meal? say  1 / 2 / 3",
                           "第 ? 餐:  1 / 2 / 3" },
  [S_VC_HOUR_FMT]      = { "meal %d. hour? say  0-9",
                           "第 %d 餐。时? 0-9" },
  [S_VC_CONFIRM_FMT]   = { "%02d:00 - say \"yes\" or \"no\"",
                           "%02d:00 - \"yes\" / \"no\"?" },
  [S_VC_SAVED_FMT]     = { LV_SYMBOL_OK " meal %d at %02d:00 - tap Save",
                           LV_SYMBOL_OK " 已设置: 第 %d 餐 %02d:00" },
  [S_VC_RETRY]         = { "say again", "请重试" },
  [S_VC_CANCEL]        = { "cancelled", "已取消" },
  [S_VC_LISTEN]        = { "listening...", "等待中..." },

  /* notifications (bell) */
  [S_NOTIFICATIONS]    = { LV_SYMBOL_BELL "  Notifications",
                           LV_SYMBOL_BELL "  通知" },
  [S_ALL_NORMAL]       = { LV_SYMBOL_OK "  All pets eating normally.",
                           LV_SYMBOL_OK "  所有宠物进食正常。" },
  [S_CLOSE]            = { "Close", "关闭" },
  [S_ALERT_APPETITE_FMT] = { LV_SYMBOL_WARNING " %s: sudden appetite drop - consider a vet check.",
                             LV_SYMBOL_WARNING " %s: 食欲骤降 - 建议就医检查。" },
  [S_ALERT_BCS_FMT]    = { LV_SYMBOL_WARNING " %s: body condition looks %s - consider a vet check.",
                           LV_SYMBOL_WARNING " %s: 体态偏%s - 建议就医检查。" },
  [S_UNDERWEIGHT]      = { "underweight", "瘦" },
  [S_OVERWEIGHT]       = { "overweight", "胖" },

  /* my pets */
  [S_ENROLLED_PETS]    = { "Enrolled pets", "已录入的宠物" },
  [S_NO_PETS]          = { "No pets yet - enroll one", "还没有宠物 - 请先录入" },
  [S_ENROLLED_COUNT_FMT] = { "enrolled pets: %d", "已录入宠物: %d" },
  [S_TODAY_FMT]        = { "Today: %d / %d g   -   %d meals",
                           "今日: %d / %d 克   -   %d 餐" },
  [S_LIMIT_REACHED]    = { LV_SYMBOL_OK " Daily limit reached",
                           LV_SYMBOL_OK " 已达每日上限" },
  [S_WAITING]          = { "  %02d:%02d   waiting for pet", "  %02d:%02d   等待宠物" },
  [S_PENDING]          = { "  %02d:%02d   pending", "  %02d:%02d   待进行" },
  [S_ATE_FMT]          = { "  %02d:%02d   " LV_SYMBOL_OK " ate %02d:%02d   %dg",
                           "  %02d:%02d   " LV_SYMBOL_OK " 已吃 %02d:%02d   %d克" },
  [S_SKIPPED]          = { "  %02d:%02d   skipped", "  %02d:%02d   已跳过" },
  [S_NO_MEALS_SCHED]   = { "  no meals scheduled", "  未安排用餐" },
  [S_AVG_WEEK_FMT]     = { "Avg: %d g/day    Week: %d g",
                           "平均: %d 克/天    本周: %d 克" },
  [S_APPETITE_FMT]     = { "Appetite: %d%%", "食欲: %d%%" },
  [S_APPETITE_DROP_FMT] = { LV_SYMBOL_WARNING " Appetite drop (%d%%)",
                            LV_SYMBOL_WARNING " 食欲下降 (%d%%)" },
  [S_BCS_FMT]          = { "BCS: %s", "体态: %s" },
  [S_BCS_VET_FMT]      = { "BCS: %s  " LV_SYMBOL_RIGHT "  consider a vet check",
                           "体态: %s  " LV_SYMBOL_RIGHT "  建议就医检查" },
  [S_BCS_UNDER]        = { "Under", "偏瘦" },
  [S_BCS_IDEAL]        = { "Ideal", "理想" },
  [S_BCS_OVER]         = { "Over",  "偏胖" },
  [S_BCS_NA]           = { "-", "-" },
  [S_BCS_TITLE]        = { "Body condition", "体态评分" },
  [S_EDIT_SCHEDULE]    = { LV_SYMBOL_EDIT " Edit schedule", LV_SYMBOL_EDIT " 编辑计划" },
  [S_BCS_DISCLAIMER]   = { LV_SYMBOL_WARNING "  BCS is an AI estimate, not a diagnosis - consult a vet.",
                           LV_SYMBOL_WARNING "  体态评分为 AI 估计, 非诊断 - 请咨询兽医。" },
  [S_NEXT_DAY]         = { LV_SYMBOL_REFRESH " Next Day (demo)",
                           LV_SYMBOL_REFRESH " 下一天 (演示)" },

  /* edit-pet modal */
  [S_EDIT_PET]         = { "Edit pet", "编辑宠物" },
  [S_EDIT_TITLE_FMT]   = { "Edit %s", "编辑 %s" },
  [S_CANCEL]           = { "Cancel", "取消" },
  [S_DELETE]           = { LV_SYMBOL_TRASH " Delete", LV_SYMBOL_TRASH " 删除" },
  [S_SURE]             = { LV_SYMBOL_WARNING " Sure?", LV_SYMBOL_WARNING " 确定?" },
  [S_SAVE]             = { LV_SYMBOL_OK " Save", LV_SYMBOL_OK " 保存" },

  /* trends */
  [S_TRENDS]           = { "Trends", "趋势" },
  [S_APPETITE_TREND]   = { "Appetite trend", "食欲趋势" },
  [S_TRENDS_ENROLL_FIRST] = { "Trends - enroll a pet first", "趋势 - 请先录入宠物" },
  [S_TREND_TITLE_FMT]  = { "Trends: %s  (%s)", "趋势: %s  (%s)" },
  [S_WIN_7D]           = { "7d", "7天" },
  [S_WIN_30D]          = { "30d", "30天" },
  [S_PET_PREV]         = { LV_SYMBOL_LEFT " Pet", LV_SYMBOL_LEFT " 宠物" },
  [S_PET_NEXT]         = { "Pet " LV_SYMBOL_RIGHT, "宠物 " LV_SYMBOL_RIGHT },
  [S_TAP_BAR]          = { "tap a bar to read its value", "点击柱状条查看数值" },
  [S_TREND_STATS_FMT]  = { "Today %d/%dg   Avg %dg/day\nWeek %dg   Month %dg   Appetite %d%%",
                           "今日 %d/%d克   平均 %d克/天\n本周 %d克   本月 %d克   食欲 %d%%" },
  [S_TODAY_VAL_FMT]    = { "today: %d g", "今日: %d 克" },
  [S_YDAY_VAL_FMT]     = { "yesterday: %d g", "昨日: %d 克" },
  [S_NDAYS_VAL_FMT]    = { "%d days ago: %d g", "%d 天前: %d 克" },

  /* misc */
  [S_BACKEND_FMT]      = { "backend %s   arena %lu KB", "后端 %s   内存 %lu KB" },
  [S_INFER_FMT]        = { "infer %lu ms   arena %lu KB", "推理 %lu 毫秒   内存 %lu KB" },
  /* The toggle shows the language you'd switch TO, so it reads the OTHER
   * language: while EN it offers 中; while 中 it offers EN. */
  [S_LANG_TOGGLE]      = { "中文", "EN" },
};

static int g_lang = VP_LANG_EN;

const char *velapaw_tr(int id)
{
  if (id < 0 || id >= S_COUNT)
    {
      return "";
    }
  const char *s = g_strtab[id][g_lang];
  if (s == NULL)                       /* missing translation -> fall back to EN */
    {
      s = g_strtab[id][VP_LANG_EN];
    }
  return s ? s : "";
}

int velapaw_lang_get(void)
{
  return g_lang;
}

/* ---- persistence: intentionally RAM-only ----------------------------------
 * An earlier version stored a magic-tagged cell at flash 0x818000 (just past
 * identity's icon area). That raw spi_flash access WEDGED the SoC: a gray screen
 * at boot (the read) and a hard hang the instant you toggled language (the
 * write). 0x818000 is outside the range this board's spi_flash path handles
 * safely -- identity only ever touches 0x800000 (pets) and 0x810000 (icons).
 *
 * So language now lives only in RAM: the toggle works within a session and
 * resets to English on reboot -- fine for a demo. To persist it later, route it
 * through identity's proven flash helper (an in-range, erase-managed slot), not
 * a raw offset guessed to be "free". */

void velapaw_lang_set(int lang)
{
  if (lang < 0 || lang >= VP_LANG_N)
    {
      return;
    }
  g_lang = lang;
}

void velapaw_lang_load(void)
{
  /* no-op -- RAM-only (see note above) */
}

/* ---- fonts ----------------------------------------------------------------
 * THE seam. Montserrat has no CJK glyphs, so today this always returns the
 * Latin fonts (Chinese would render blank). Build CONFIG_VELAPAW_FONT_CJK=y
 * with a generated CJK font linked (ui/README_I18N.md) and it returns that
 * instead -- every label switches to it automatically. The CJK font carries
 * Latin too, so it is used for both languages once present. */
const lv_font_t *velapaw_font(int level)
{
#ifdef CONFIG_VELAPAW_FONT_CJK
  extern const lv_font_t velapaw_font_cjk_20;
  extern const lv_font_t velapaw_font_cjk_16;
  extern const lv_font_t velapaw_font_cjk_14;
  /* The CJK subset has ASCII + Chinese but NOT the LV_SYMBOL_* icon glyphs
   * (bell/camera/OK/trash/... live in the FontAwesome range that only the
   * Montserrat builds carry). Chain each CJK font's fallback to the same-size
   * Montserrat, so any glyph the CJK font lacks -- i.e. the icons -- renders
   * from there. Copied into mutable statics because the generated fonts are
   * const; the .fallback field is the only thing we change. */
  static lv_font_t f20, f16, f14;
  static int inited;
  if (!inited)
    {
      f20 = velapaw_font_cjk_20; f20.fallback = &lv_font_montserrat_20;
      f16 = velapaw_font_cjk_16; f16.fallback = &lv_font_montserrat_16;
      f14 = velapaw_font_cjk_14; f14.fallback = &lv_font_montserrat_14;
      inited = 1;
    }
  switch (level)
    {
      case VP_FONT_TITLE: return &f20;
      case VP_FONT_HEAD:  return &f16;
      default:            return &f16;   /* BODY/SMALL bumped 14->16 for legibility */
    }
#else
  switch (level)
    {
      case VP_FONT_TITLE: return &lv_font_montserrat_20;
      case VP_FONT_HEAD:  return &lv_font_montserrat_16;
      default:            return &lv_font_montserrat_16;  /* BODY/SMALL 14->16 */
    }
#endif
}
