/****************************************************************************
 * VelaPaw - multi-pet recognition smart feeder (contest2026_043)
 *
 * Emulator-first scaffold: the architecture and module boundaries are real;
 * camera/feeder are mocked and inference is a stub, so the full vertical
 * slice runs in QEMU today. Replace the backends (see README.md) for
 * hardware. Pipeline:
 *   init -> enroll demo pets -> loop[ capture -> presence gate -> embed ->
 *           match -> cooldown check -> dispense + log ]
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "velapaw.h"
#include "hal/camera.h"
#include "hal/feeder.h"
#include "infer/infer.h"
#include "identity/identity.h"
#include "store/store.h"
#ifdef CONFIG_VELAPAW_UI
#include "ui/ui.h"
#endif
#ifdef CONFIG_VELAPAW_VOICE
#include "voice/kws.h"
#include "voice/mic.h"
#endif

/* Presence gate (MVP: cheap mean-intensity threshold). On hardware this
 * becomes frame-difference motion or a PIR GPIO so we only run inference
 * when a pet is actually at the bowl. */
static int presence_detected(const struct velapaw_frame *f)
{
  long sum = 0;
  int  n   = f->width * f->height * f->channels;

  for (int i = 0; i < n; i++)
    {
      sum += f->data[i];
    }

  return (sum / n) > 8;   /* empty scene is all-zero -> no presence */
}

static velapaw_pet_id_t enroll_scene(const struct velapaw_infer_backend *be,
                                     int scene, const char *name)
{
  float samples[3][VELAPAW_EMBED_DIM];
  struct velapaw_frame frame;

  velapaw_camera_mock_set_scene(scene);
  for (int i = 0; i < 3; i++)
    {
      velapaw_camera_get_frame(&frame);
      be->embed(&frame, samples[i], VELAPAW_EMBED_DIM);
    }

  static const int def_meals[VELAPAW_MAX_MEALS] =
    { 8 * 60, 12 * 60 + 30, 18 * 60 };   /* 08:00, 12:30, 18:00 */
  velapaw_pet_id_t id = velapaw_identity_enroll(
      name, VELAPAW_DEFAULT_PORTION_G, VELAPAW_DEFAULT_COOLDOWN_S,
      VELAPAW_DEFAULT_PORTION_G * 3, def_meals,
      samples, 3);

  printf("[enroll] %s -> pet_id %d\n", name, id);
  return id;
}

int main(int argc, char *argv[])
{
  /* Subcommands run to completion and return.  They are checked before the
   * UI branch below, which never returns.
   */

#ifdef CONFIG_VELAPAW_VOICE
  if (argc > 1 && strcmp(argv[1], "kws") == 0)
    {
      /* "velapaw kws mic [reps]" -- capture a second of live audio and
       * classify it, reps times.  The embedded-clip self-test has already
       * proven the model path, so a failure here is capture-side by
       * construction.
       *
       * reps exists because one capture per process is NOT how the product
       * works and hid a bug for several builds.  Each `velapaw kws mic` is a
       * separate builtin task; NuttX closes its file descriptors at task
       * exit, and that close is what leaves I2S RX unable to arm again.  So
       * running the command twice tested process teardown, not repeat
       * capture, and no amount of cleanup logic inside mic.c could have
       * changed the outcome.
       *
       * The real feeder captures from the long-lived UI task, which does not
       * exit between keywords.  Looping here reproduces that.
       */

      if (argc > 2 && strcmp(argv[2], "mic") == 0)
        {
          static int16_t audio[VELAPAW_KWS_SAMPLE_RATE];
          struct velapaw_kws_result_s r;
          int reps = 1;
          int rep;
          int n;
          int i;

          if (argc > 3)
            {
              reps = atoi(argv[3]);
              if (reps < 1)
                {
                  reps = 1;
                }
            }

          if (velapaw_kws_init() < 0)
            {
              return 1;
            }

          /* Report the arenas here, not only from init.
           *
           * init prints them too, but init runs once and the UI task gets
           * there first at boot, so by the time a console is attached the
           * numbers have scrolled away -- and they are what the arena sizes
           * are supposed to be trimmed to.  Reprinting on a command that is
           * already the voice test costs two lines and no boot-timing race.
           */

          { size_t fe = 0;
            size_t cl = 0;
            velapaw_kws_arena_used(&fe, &cl);
            printf("velapaw: kws arenas: frontend %u/%d B, "
                   "classifier %u/%d B\n",
                   (unsigned)fe, VELAPAW_KWS_FRONTEND_ARENA,
                   (unsigned)cl, VELAPAW_KWS_CLASSIFIER_ARENA);
            fflush(stdout); }

          for (rep = 1; rep <= reps; rep++)
            {
              /* The vocabulary has been 14 words since build 55.  This prompt
               * still said "yes / no" from the 4-label days, which actively
               * misleads whoever is running the test: a digit spoken into a
               * prompt asking for yes/no looks like a misrecognition when it
               * is the tester following instructions.
               */

              printf("\nvelapaw: [%d/%d] speak now "
                     "(yes, no, or zero..nine), capturing 1 s...\n",
                     rep, reps);
              fflush(stdout);

              n = velapaw_mic_capture(audio, VELAPAW_KWS_SAMPLE_RATE, 5000);
              if (n < 0)
                {
                  printf("velapaw: capture %d failed: %d\n", rep, n);
                  velapaw_mic_shutdown();
                  return 1;
                }

              velapaw_mic_stats(audio, n);

              /* Silence is rejected here rather than by the model, which has
               * no opinion about it: fed an empty room micro_speech returns a
               * confident wrong label, not "silence".  See mic.h.
               */

              if (!velapaw_mic_speech_present(audio, n))
                {
                  printf("velapaw: no speech in window, not classifying\n");
                  continue;
                }

              /* Independent utterance: do not let the previous capture's
               * noise floor bias this one.
               */

              velapaw_kws_reset();

              if (velapaw_kws_classify(audio, n, &r) < 0)
                {
                  printf("velapaw: classify failed\n");
                  velapaw_mic_shutdown();
                  return 1;
                }

              printf("velapaw: heard %s (%.3f)  feat %ums infer %ums\n",
                     velapaw_kws_label_name(r.label), (double)r.score,
                     (unsigned)r.feature_ms, (unsigned)r.infer_ms);

              for (i = 0; i < VELAPAW_KWS_CATEGORIES; i++)
                {
                  printf("    %-8s %.4f\n",
                         velapaw_kws_label_name(i), (double)r.scores[i]);
                }
            }

          /* Explicit, so the codec is released at a point we chose rather
           * than by the kernel closing our fds on the way out.
           */

          velapaw_mic_shutdown();
          return 0;
        }

#ifdef CONFIG_VELAPAW_KWS_SELFTEST
      return velapaw_kws_selftest() < 0 ? 1 : 0;
#else
      printf("velapaw: rebuild with CONFIG_VELAPAW_KWS_SELFTEST=y\n");
      return 1;
#endif
    }
#endif

#if defined(CONFIG_VELAPAW_VOICE) && defined(CONFIG_VELAPAW_UI)

  /* "velapaw voice" -- print where the UI's voice worker has got to.
   *
   * The worker starts at boot and says what went wrong on stdout, which on
   * this board is a USB CDC that has not enumerated yet.  Its diagnosis is
   * therefore always lost, and the visible symptom -- a dialog that says
   * "voice off" -- is the same whatever the cause.  This reads the state out
   * of memory instead, from a task, after the console exists.
   */

  if (argc > 1 && strcmp(argv[1], "voice") == 0)
    {
      velapaw_ui_voice_probe();
      return 0;
    }

#endif

  (void)argc;
  (void)argv;

#ifdef CONFIG_VELAPAW_UI
  velapaw_ui_run();   /* LVGL dashboard (does not return) */
  return 0;
#endif

  printf("VelaPaw scaffold starting (team contest2026_043)\n");

  const struct velapaw_infer_backend *be = velapaw_infer_active();

  velapaw_camera_open();
  be->init();
  velapaw_identity_init();
  velapaw_store_init();
  velapaw_feeder_init();
  printf("[init] inference backend: %s (arena %lu bytes)\n",
         be->name, (unsigned long)be->arena_bytes());

  /* --- Enrollment: two demo pets from distinct synthetic scenes --- */
  enroll_scene(be, 0, "Rex");
  enroll_scene(be, 1, "Milo");
  printf("[init] %d pets enrolled\n", velapaw_identity_count());

  /* --- "Live" loop over a scripted sequence of scenes ---
   * 0=Rex, 1=Milo, 0=Rex again (cooldown), -1=empty (presence gate), 1=Milo.
   */
  static const int script[] = { 0, 1, 0, -1, 1 };
  time_t last_fed[VELAPAW_MAX_PETS] = { 0 };

  for (unsigned s = 0; s < sizeof(script) / sizeof(script[0]); s++)
    {
      int scene = script[s];
      struct velapaw_frame frame;

      velapaw_camera_mock_set_scene(scene);
      velapaw_camera_get_frame(&frame);

      if (!presence_detected(&frame))
        {
          printf("[loop %u] no presence; skip\n", s);
          continue;
        }

      float emb[VELAPAW_EMBED_DIM];
      be->embed(&frame, emb, VELAPAW_EMBED_DIM);
      printf("    [infer] %s: %lu us\n", be->name,
             (unsigned long)be->last_latency_us());

      struct velapaw_match m;
      velapaw_identity_match(emb, &m);

      if (m.id == VELAPAW_PET_UNKNOWN)
        {
          printf("[loop %u] unknown pet (score %.2f); no dispense\n",
                 s, m.score);
          continue;
        }

      const struct velapaw_pet *pet = velapaw_identity_get(m.id);
      time_t now = time(NULL);

      if (now - last_fed[m.id] < pet->cooldown_s)
        {
          printf("[loop %u] %s recognized (%.2f) but within cooldown; "
                 "no dispense\n", s, pet->name, m.score);
          continue;
        }

      velapaw_feeder_dispense(pet->portion_g);

      struct velapaw_feed_event ev =
        {
          .pet_id = m.id,
          .ts     = now,
          .grams  = pet->portion_g,
          .score  = m.score,
          .model  = be->name,
        };
      velapaw_store_log_feeding(&ev);
      last_fed[m.id] = now;

      printf("[loop %u] fed %s %dg (score %.2f, today total %dg)\n",
             s, pet->name, pet->portion_g, m.score,
             velapaw_store_today_total(m.id));
    }

  printf("VelaPaw scaffold done.\n");

  be->deinit();
  velapaw_camera_close();
  return 0;
}
