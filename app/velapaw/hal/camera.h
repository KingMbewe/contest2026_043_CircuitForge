/****************************************************************************
 * VelaPaw - camera HAL
 *
 * Interface used by the app; backends select at build time:
 *   camera_mock.c    - emulator (synthetic scenes)
 *   camera_ov2640.c  - ESP32-S3-EYE OV2640 via NuttX video framework (TODO)
 ****************************************************************************/

#ifndef VELAPAW_HAL_CAMERA_H
#define VELAPAW_HAL_CAMERA_H

#include <stdint.h>
#include "velapaw.h"

/* MVP frame format: 8-bit grayscale, VELAPAW_FRAME_W x VELAPAW_FRAME_H. */
struct velapaw_frame
{
  uint8_t *data;     /* width*height*channels bytes, HWC (owned by HAL) */
  int      width;
  int      height;
  int      channels; /* 1=grayscale, 3=RGB */
};

int  velapaw_camera_open(void);
int  velapaw_camera_get_frame(struct velapaw_frame *frame);
void velapaw_camera_close(void);

/* Mock-only hook: select which synthetic scene the next frame depicts.
 * scene >= 0 : a distinct repeatable "pet" pattern; scene < 0 : empty bowl.
 * No-op on real backends. */
void velapaw_camera_mock_set_scene(int scene_id);

#endif /* VELAPAW_HAL_CAMERA_H */
