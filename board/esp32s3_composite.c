/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-devkit/src/esp32s3_composite.c
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

#include <stdint.h>
#include <assert.h>

#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/composite.h>
#include <nuttx/usb/cdcacm.h>
#include <nuttx/usb/cdcncm.h>

#include "esp32s3_otg.h"
#include "esp32s3-devkit.h"

#if defined(CONFIG_USBDEV_COMPOSITE) && defined(CONFIG_ESP32S3_OTG)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_composite0_connect
 *
 * Description:
 *   Build the CDCACM + CDC-NCM composite device (console + network over
 *   one USB port). CDCACM is registered first so the console always
 *   lands on interfaces 0-1, which is the only ordering that has worked
 *   for the console -- CDC-NCM takes interfaces 2-3. Unlike RNDIS,
 *   Windows' inbox NCM driver (usbncm.sys) is not known to require its
 *   function to be interface 0, so this ordering should not break it.
 *
 ****************************************************************************/

static void *board_composite0_connect(void)
{
  struct composite_devdesc_s dev[2];
  int ifnobase = 0;
  int strbase  = COMPOSITE_NSTRIDS;
  int dev_idx  = 0;
  int epin     = 1;
  int epout    = 1;

#ifdef CONFIG_CDCACM_COMPOSITE
  cdcacm_get_composite_devdesc(&dev[dev_idx]);
  dev[dev_idx].classobject  = cdcacm_classobject;
  dev[dev_idx].uninitialize = cdcacm_uninitialize;
  dev[dev_idx].devinfo.ifnobase = ifnobase;
  dev[dev_idx].minor = 0;
  dev[dev_idx].devinfo.strbase = strbase;
  dev[dev_idx].devinfo.epno[CDCACM_EP_INTIN_IDX]   = epin++;
  dev[dev_idx].devinfo.epno[CDCACM_EP_BULKIN_IDX]  = epin++;
  dev[dev_idx].devinfo.epno[CDCACM_EP_BULKOUT_IDX] = epout++;
  ifnobase += dev[dev_idx].devinfo.ninterfaces;
  strbase  += dev[dev_idx].devinfo.nstrings;
  dev_idx  += 1;
#endif

#ifdef CONFIG_CDCNCM_COMPOSITE
  cdcncm_get_composite_devdesc(&dev[dev_idx]);
  dev[dev_idx].devinfo.ifnobase = ifnobase;
  dev[dev_idx].minor = 0;
  dev[dev_idx].devinfo.strbase = strbase;
  dev[dev_idx].devinfo.epno[CDCNCM_EP_INTIN_IDX]   = epin++;
  dev[dev_idx].devinfo.epno[CDCNCM_EP_BULKIN_IDX]  = epin++;
  dev[dev_idx].devinfo.epno[CDCNCM_EP_BULKOUT_IDX] = epout++;
  ifnobase += dev[dev_idx].devinfo.ninterfaces;
  strbase  += dev[dev_idx].devinfo.nstrings;
  dev_idx  += 1;
#endif

  DEBUGASSERT(epin  <= (CONFIG_ESP32S3_OTG_ENDPOINT_NUM + 1));
  DEBUGASSERT(epout <= (CONFIG_ESP32S3_OTG_ENDPOINT_NUM + 1));

  return composite_initialize(composite_getdevdescs(), dev, dev_idx);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_composite_initialize
 ****************************************************************************/

int board_composite_initialize(int port)
{
  return OK;
}

/****************************************************************************
 * Name: board_composite_connect
 ****************************************************************************/

void *board_composite_connect(int port, int configid)
{
  if (configid == 0)
    {
      return board_composite0_connect();
    }
  else
    {
      return NULL;
    }
}

#endif /* CONFIG_USBDEV_COMPOSITE && CONFIG_ESP32S3_OTG */
