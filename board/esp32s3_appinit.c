/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-devkit/src/esp32s3_appinit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sched.h>
#include <unistd.h>
#include <syslog.h>
#include <nuttx/board.h>

#include "esp32s3-devkit.h"

#ifdef CONFIG_BOARDCTL

/****************************************************************************
 * VelaPaw auto-start
 *
 * A finished appliance boots into its UI: power on -> pet feeder. Without this
 * the board comes up to a bare NSH prompt and a GREY SCREEN until someone
 * types "velapaw" over a serial console, which is not something a judge (or an
 * owner) is going to do.
 *
 * We launch it from board_app_initialize() -- NSH calls this via
 * boardctl(BOARDIOC_INIT) at startup, AFTER esp32s3_bringup() has registered
 * the LCD, touch panel and camera. That ordering is the whole reason this hook
 * works: velapaw needs those devices to exist, which is exactly why running it
 * by hand from NSH succeeds today.
 *
 * Deliberately NOT done by making velapaw the init entrypoint: that would
 * replace NSH and we would lose the shell. We still need it -- the hardware-SPI
 * display bug is unsolved and will need console access with a logic analyzer.
 * This way the app auto-starts AND the shell stays live, so you can always
 * `kill` velapaw and have the board to yourself.
 *
 * To disable (e.g. while debugging the display): set VELAPAW_AUTOSTART to 0
 * and rebuild.
 ****************************************************************************/

#define VELAPAW_AUTOSTART        1
#define VELAPAW_AUTOSTART_PRIO   100
#define VELAPAW_AUTOSTART_STACK  131072   /* matches STACKSIZE in the app's Makefile */
#define VELAPAW_AUTOSTART_DELAY  500      /* ms; let NSH finish coming up first */

#if defined(CONFIG_LVX_USE_DEMO_CONTEST2026_043_VELAPAW) && VELAPAW_AUTOSTART

extern int velapaw_main(int argc, char *argv[]);

/* Small shim so we can pause before handing over: board_app_initialize() runs
 * inside NSH's own startup, and we would rather not race its console setup. */

static int velapaw_launcher(int argc, char *argv[])
{
  usleep(VELAPAW_AUTOSTART_DELAY * 1000);
  return velapaw_main(argc, argv);
}

static void velapaw_autostart(void)
{
  int pid = task_create("velapaw",
                        VELAPAW_AUTOSTART_PRIO,
                        VELAPAW_AUTOSTART_STACK,
                        velapaw_launcher,
                        NULL);

  syslog(LOG_ERR, "[velapaw] autostart pid=%d\n", pid);
}

#else
#  define velapaw_autostart()
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  This function is never
 *   called directly from application code, but only indirectly via the
 *   (non-standard) boardctl() interface using the command BOARDIOC_INIT.
 *
 * Input Parameters:
 *   arg - The boardctl() argument is passed to the board_app_initialize()
 *         implementation without modification.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure to indicate the nature of the failure.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
  int ret = OK;

#ifndef CONFIG_BOARD_LATE_INITIALIZE
  /* Perform board-specific initialization.  (When CONFIG_BOARD_LATE_INITIALIZE
   * is set this has already been done by board_late_initialize().)
   */

  ret = esp32s3_bringup();
#endif

  /* The LCD / touch / camera exist by now -- safe to hand the screen to the
   * app. No-op unless velapaw is configured in.
   */

  velapaw_autostart();

  return ret;
}

#endif /* CONFIG_BOARDCTL */
