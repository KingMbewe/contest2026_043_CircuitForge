/****************************************************************************
 * VelaPaw - mock feeder backend (emulator)
 *
 * Logs the dispense action. On hardware, replace with a servo driver that
 * rotates an auger/gate for a duration proportional to grams (LEDC/PWM).
 ****************************************************************************/

#include <stdio.h>
#include "hal/feeder.h"

int velapaw_feeder_init(void)
{
  return 0;
}

int velapaw_feeder_dispense(int grams)
{
  if (grams <= 0)
    {
      return -1;
    }

  printf("    [feeder] (mock) dispensing %d g\n", grams);
  return 0;
}
