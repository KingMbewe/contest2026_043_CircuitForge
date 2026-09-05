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
#define VELAPAW_AUTOSTART_STACK  49152    /* reduced 2026-08-26: 131072 exceeded imem free heap after ai_agent merge, silently failed task_create() */
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
 * Telnet console auto-start
 *
 * The USB CDC-ACM console has proven unreliable during bring-up (both TX
 * and RX went silent in this build, independent of board health -- see the
 * 2026-09-01 session notes). RNDIS-over-USB networking is solid, so bring
 * up telnetd directly on top of it instead of going through the
 * apps/system/telnetd builtin, whose telnetd.c has a compile-time bug in
 * this vendor tree (macro corruption on its own main()). Call
 * telnetd_daemon() directly with a hand-built config, using nshlib's
 * nsh_telnetmain() as the per-connection session entry point -- both of
 * those compile and link fine; only the CLI wrapper is broken.
 ****************************************************************************/

#if defined(CONFIG_NETUTILS_TELNETD) && defined(CONFIG_NSH_TELNET)

#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

/* Local mirror of apps/include/netutils/telnetd.h's struct and
 * apps/nshlib/nshlib.h's nsh_telnetmain() prototype -- board code only
 * sees nuttx/include on its compile path, not apps/include, so the real
 * headers aren't reachable from here. main_t comes from <sched.h>,
 * already included above. Field types copied VERBATIM from the real
 * header (checked 2026-09-01) -- a size mismatch here (e.g. t_priority
 * as plain int instead of uint8_t) silently corrupts every field after
 * it once telnetd_daemon(), built against the real header, reads this
 * struct. This was the actual bug the first attempt hit: TCP port 23
 * accepted no connections at all because t_entry/t_argv were reading
 * garbage offsets.
 */

struct telnetd_config_s
{
  uint16_t d_port;
  sa_family_t d_family;
  uint8_t t_priority;
  size_t t_stacksize;
#ifndef CONFIG_BUILD_KERNEL
  main_t t_entry;
#endif
#ifdef CONFIG_LIBC_EXECFUNCS
  FAR const char *t_path;
#endif
  FAR char * const *t_argv;
};

int telnetd_daemon(FAR const struct telnetd_config_s *config);
int nsh_telnetmain(int argc, FAR char *argv[]);

#define TELNETD_PRIO           100
#define TELNETD_STACK          4096
#define TELNETD_SESSION_PRIO   100
#define TELNETD_SESSION_STACK  4096

/* UDP announce, not a file write: /data's mount health is itself unknown
 * right now, and a failed open() would silently no-op, indistinguishable
 * from "task never ran" -- the network path is independently proven
 * working (real ICMP replies), so use it instead for a console-free
 * signal.
 */

static void telnetd_debug_announce(FAR const char *msg)
{
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0)
    {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = HTONS(9999);
      addr.sin_addr.s_addr = HTONL(0x0a000001); /* 10.0.0.1, the Windows host */
      sendto(s, msg, strlen(msg), 0,
             (FAR struct sockaddr *)&addr, sizeof(addr));
      close(s);
    }
}

static int telnetd_task_entry(int argc, FAR char *argv[])
{
  FAR char *targv[] =
  {
    "telnetd",
    "-c",
    NULL,
  };

  struct telnetd_config_s config =
  {
    HTONS(23),
#ifdef CONFIG_NET_IPv4
    AF_INET,
#else
    AF_INET6,
#endif
    TELNETD_SESSION_PRIO,
    TELNETD_SESSION_STACK,
#ifndef CONFIG_BUILD_KERNEL
    nsh_telnetmain,
#endif
#ifdef CONFIG_LIBC_EXECFUNCS
    "telnetd",
#endif
    targv,
  };

  int ret;
  char buf[64];

  telnetd_debug_announce("telnetd task entered\n");

  ret = telnetd_daemon(&config);

  /* telnetd_daemon() only returns on error -- if we get here, announce
   * why.
   */

  snprintf(buf, sizeof(buf), "telnetd_daemon returned %d\n", ret);
  telnetd_debug_announce(buf);

  return ret;
}

static void telnetd_autostart(void)
{
  int pid = task_create("telnetd",
                        TELNETD_PRIO,
                        TELNETD_STACK,
                        telnetd_task_entry,
                        NULL);

  syslog(LOG_ERR, "[telnetd] autostart pid=%d\n", pid);
}

#else
#  define telnetd_autostart()
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
  // telnetd_autostart(); // TEMP-DISABLED 2026-09-02 for console-close(0/1/2) hang isolation test

  return ret;
}

#endif /* CONFIG_BOARDCTL */
