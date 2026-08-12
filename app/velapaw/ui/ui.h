/****************************************************************************
 * VelaPaw - LVGL UI entry
 ****************************************************************************/

#ifndef VELAPAW_UI_H
#define VELAPAW_UI_H

/* Run the LVGL dashboard demo (init display, enroll, live recognition loop).
 * Does not return (live loop) unless display init fails. */
void velapaw_ui_run(void);

#ifdef CONFIG_VELAPAW_VOICE

/* Dump the voice worker's state to the console.  Safe to call from a CLI task
 * while the UI runs -- flat build, shared .data.  See ui_lvgl.c for how to
 * read the numbers. */

void velapaw_ui_voice_probe(void);

#endif

#endif /* VELAPAW_UI_H */
