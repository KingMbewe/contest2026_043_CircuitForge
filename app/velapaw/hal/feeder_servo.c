/****************************************************************************
 * VelaPaw - feeder HAL: real servo backend (ESP32-S3 LEDC/PWM on GPIO43)
 *
 * The PWM itself lives in the board file (it owns the LEDC registers; the
 * camera's XCLK already uses ch0/timer0, so the servo takes ch1/timer1 @50Hz).
 * Here we only bridge the app API to it.
 *
 * IMPORTANT: board_feeder_dispense() SLEEPS while the gate is held open
 * (grams x SERVO_MS_PER_GRAM, e.g. ~1.8 s for 15 g). Calling that from the UI
 * task would freeze the screen + camera -- the same class of bug as a blocking
 * printf. So we run it on a detached worker thread and return immediately;
 * the UI keeps rendering while the food drops.
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include "hal/feeder.h"

/* Provided by the board (nuttx/boards/.../esp32s3_st7789.c) */
extern void board_feeder_init(void);
extern int  board_feeder_dispense(int grams);

static void *dispense_thread(void *arg)
{
  int grams = (int)(intptr_t)arg;
  board_feeder_dispense(grams);
  return NULL;
}

int velapaw_feeder_init(void)
{
  /* The board already inits the servo at boot (board_lcd_initialize), so this
   * is a no-op; kept so the HAL contract is unchanged. */
  return 0;
}

int velapaw_feeder_dispense(int grams)
{
  pthread_t th;
  pthread_attr_t attr;

  if (grams <= 0)
    {
      return -1;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2048);
  if (pthread_create(&th, &attr, dispense_thread,
                     (void *)(intptr_t)grams) != 0)
    {
      pthread_attr_destroy(&attr);
      return -1;
    }
  pthread_detach(th);
  pthread_attr_destroy(&attr);

  printf("    [feeder] servo dispensing %d g (async)\n", grams);
  return 0;
}
