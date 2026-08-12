/****************************************************************************
 * VelaPaw - OV5640 camera backend (real hardware, ESP32-S3 LCD_CAM/DVP)
 *
 * The board driver (esp32s3_st7789.c) owns the OV5640 + LCD_CAM + GDMA and
 * captures a QVGA (320x240) RGB565 frame on demand. This HAL downscales that
 * to the model/UI's 128x128 RGB888, applies the SELFIE mirror (the lens faces
 * the back of the board, so we flip horizontally to act like a front camera),
 * and returns it. Flat build: we call the board capture function directly.
 *
 * The raw DVP capture comes out of the sensor/mount rotated 90 deg CCW from
 * the real scene (confirmed on hardware: a hand held upright renders sideways)
 * -- flip/mirror registers (0x3820/0x3821) can never fix this, since they only
 * reverse an axis and a 90 deg turn needs an actual transpose. We correct it
 * here in the resample step with a 90 deg CW remap, combined with the selfie
 * mirror on the final (post-rotation) horizontal axis.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/camera.h"

/* Provided by the board (flat build) */
extern int board_ov5640_capture(uint8_t **buf, int *w, int *h);

static uint8_t g_framebuf[VELAPAW_FRAME_BYTES];   /* 128x128x3, black until 1st frame */

/* Live camera ignores scene selection. */
void velapaw_camera_mock_set_scene(int scene_id)
{
  (void)scene_id;
}

int velapaw_camera_open(void)
{
  /* Sensor + capture engine were configured by the board at boot
   * (board_ov5640_init). Nothing to do here.
   */
  printf("velapaw/cam: OV5640 backend (board LCD_CAM capture)\n");
  return 0;
}

int velapaw_camera_get_frame(struct velapaw_frame *frame)
{
  uint8_t *buf = NULL;
  int w = 0, h = 0;

  if (frame == NULL)
    {
      return -1;
    }

  if (board_ov5640_capture(&buf, &w, &h) != 0 || buf == NULL || w <= 0 || h <= 0)
    {
      /* capture failed: keep the previous frame (or black on first call) */
    }
  else
    {
      for (int y = 0; y < VELAPAW_FRAME_H; y++)
        {
          /* 90 deg CW correction: screen row y walks the source's columns */
          int dy = y * w / VELAPAW_FRAME_H;
          int sx = dy;
          for (int x = 0; x < VELAPAW_FRAME_W; x++)
            {
              /* selfie mirror applied on the corrected (post-rotation) x axis */
              int mox = (VELAPAW_FRAME_W - 1) - x;
              int dx = mox * h / VELAPAW_FRAME_W;
              int sy = (h - 1) - dx;
              const uint8_t *p = buf + ((size_t)sy * w + sx) * 2;

              /* RGB565, big-endian byte order in the capture buffer */
              uint16_t px = (uint16_t)((p[0] << 8) | p[1]);
              uint8_t r = (px >> 11) & 0x1f;
              uint8_t g = (px >> 5)  & 0x3f;
              uint8_t b =  px        & 0x1f;

              int o = (y * VELAPAW_FRAME_W + x) * 3;
              g_framebuf[o + 0] = (uint8_t)((r << 3) | (r >> 2));
              g_framebuf[o + 1] = (uint8_t)((g << 2) | (g >> 4));
              g_framebuf[o + 2] = (uint8_t)((b << 3) | (b >> 2));
            }
        }
    }

  frame->data     = g_framebuf;
  frame->width    = VELAPAW_FRAME_W;
  frame->height   = VELAPAW_FRAME_H;
  frame->channels = VELAPAW_FRAME_C;
  return 0;
}

void velapaw_camera_close(void)
{
}
