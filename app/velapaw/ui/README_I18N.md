# VelaPaw UI — bilingual (English / 中文)

The UI runs in English or Chinese and switches live from the **中 / EN** toggle in
the top-right of the header (next to the bell). The choice is saved to flash and
restored on the next boot.

## How it is wired

| Piece | File | What it does |
|---|---|---|
| String table | [ui_i18n.c](ui_i18n.c) | one row per `S_*` id, `{ English, 中文 }` |
| Lookup | `T(id)` → `velapaw_tr()` | returns the current-language string |
| Switch + persist | `velapaw_lang_set()` | flips language, writes one flash cell |
| Font seam | `velapaw_font(level)` | the **only** place fonts are chosen |
| Live rebuild | `ui_build()` in [ui_lvgl.c](ui_lvgl.c) | tears down + recreates the tree so one-shot labels re-read the language |

Every label goes through `T(...)` for its text and `velapaw_font(...)` for its
font, so adding a language or a font is a change in one place, not 79.

## The Chinese font — generated (2026-07-21)

`velapaw_font_cjk_14/16/20.c` are checked in: a **subset of Noto Sans SC**
(SIL Open Font License 1.1 — redistributable) containing only ASCII + the ~150
Chinese glyphs this UI actually uses, at bpp 4. Enable with
`CONFIG_VELAPAW_FONT_CJK=y` and `velapaw_font()` returns them for both languages.
They add ~560 KB of C (glyph bitmaps), comfortable on the 16 MB flash.

Exact command used (Noto Sans SC Regular OTF, one per size):

```bash
lv_font_conv --font NotoSansSC-Regular.otf --size 14 --bpp 4 --format lvgl \
    --range 0x20-0x7F,<the 150 CJK code points> --no-compress \
    -o velapaw_font_cjk_14.c        # repeat --size 16 / 20
```

The glyph set is derived straight from the 中文 column of `ui_i18n.c`, so if you
add Chinese strings, regenerate (widen `--range`).

> **License note:** Noto Sans SC is OFL 1.1 — fine to embed and publish. Keep the
> OFL attribution with the repo. Do **not** substitute a Microsoft font
> (YaHei/SimSun) in the shipped build; their EULAs don't cover redistribution.

### Historical: why this was needed

The stock `lv_font_montserrat_*` fonts are **Latin-only** — with
`CONFIG_VELAPAW_FONT_CJK=n` (the default) the toggle still works but Chinese text
renders as blank boxes. English is fully functional either way.

To make Chinese actually render:

1. **Generate a subset font** from a CJK TTF (Noto Sans CJK SC / Source Han Sans),
   containing only ASCII + the glyphs this UI uses. Using LVGL's
   [font converter](https://github.com/lvgl/lv_font_conv):

   ```bash
   # collect every Chinese glyph the UI can show (from the 中文 column)
   #   -> feed the unique code points to --range or --symbols
   lv_font_conv --font NotoSansCJKsc-Regular.otf \
       --size 14 --bpp 2 --format lvgl \
       --symbols "$(cat ui_i18n.c | grep -o '[一-龥]' | sort -u | tr -d '\n')" \
       --range 0x20-0x7F \
       -o velapaw_font_cjk_14.c
   # repeat at --size 16 and --size 20 -> _16.c and _20.c
   ```

   The three blobs must define `velapaw_font_cjk_14 / _16 / _20`
   (rename the `lv_font_t` in each generated file to match, or pass
   `--force-fast-kern-format` / edit the symbol name).

2. **Add the three `.c` files** to `CSRCS` (Makefile) and the CMake `VELAPAW_SRCS`
   list, right next to `ui/ui_i18n.c`.

3. **Enable the option:** `CONFIG_VELAPAW_FONT_CJK=y`. `velapaw_font()` then returns
   the CJK font for every level, and — because that font also carries Latin —
   both languages render from it.

Budget ~50–150 KB of flash for the glyph subset (a few hundred characters × 3
sizes). The board has 16 MB, so this is comfortable.

## Adding / editing strings

1. Add an `S_*` id to the enum in [ui_i18n.h](ui_i18n.h).
2. Add its `{ "English", "中文" }` row to `g_strtab[]` in [ui_i18n.c](ui_i18n.c).
3. Use `T(S_YOUR_ID)` at the call site (with `lv_label_set_text` or
   `lv_label_set_text_fmt`).

Rows that carry `printf` conversions (`%d`, `%s`, `%.2f`, …) **must keep the same
conversions in both languages** — the same values are formatted into whichever
string is active.
