/****************************************************************************
 * VelaPaw - feeder HAL
 *
 * Backends:
 *   feeder_mock.c   - emulator (logs the dispense)
 *   feeder_servo.c  - ESP32-S3 servo via LEDC/PWM (TODO)
 *
 * Per-pet cooldown is enforced in the app layer, NOT here.
 ****************************************************************************/

#ifndef VELAPAW_HAL_FEEDER_H
#define VELAPAW_HAL_FEEDER_H

#include "velapaw.h"

int velapaw_feeder_init(void);

/* Dispense `grams` of food. Returns 0 on success, <0 on error. */
int velapaw_feeder_dispense(int grams);

#endif /* VELAPAW_HAL_FEEDER_H */
