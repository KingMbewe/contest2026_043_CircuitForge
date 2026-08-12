/****************************************************************************
 * VelaPaw - mock camera backend (emulator)
 *
 * With CONFIG_VELAPAW_INFER_TFLM: serves embedded REAL pet photos (generated
 * by host/gen_device_assets.py) so on-device recognition is meaningful. A
 * "scene" id selects a pet; successive captures of that scene cycle through
 * that pet's images (so enrollment averages real samples). scene < 0 = empty.
 *
 * Otherwise: deterministic synthetic gradients (for the stub backend).
 *
 * Replace either with OV2640 capture (NuttX V4L2 / goldfish_camera) on hardware.
 ****************************************************************************/

#include <nuttx/config.h>
#include <string.h>
#include "hal/camera.h"

static uint8_t g_framebuf[VELAPAW_FRAME_BYTES];
static int     g_scene = -1;

#ifdef CONFIG_VELAPAW_INFER_TFLM
#include "velapaw_assets.h"

static int g_cursor[VELAPAW_NUM_PETS];

void velapaw_camera_mock_set_scene(int scene_id)
{
  g_scene = scene_id;
}

int velapaw_camera_open(void)
{
  g_scene = -1;
  memset(g_cursor, 0, sizeof(g_cursor));
  return 0;
}

int velapaw_camera_get_frame(struct velapaw_frame *frame)
{
  if (frame == NULL)
    {
      return -1;
    }

  if (g_scene < 0 || g_scene >= VELAPAW_NUM_PETS)
    {
      memset(g_framebuf, 0, sizeof(g_framebuf));   /* empty bowl */
    }
  else
    {
      /* Find the (cursor)-th embedded image belonging to this pet. */
      int want = g_cursor[g_scene];
      int seen = 0;
      int chosen = -1;
      for (int i = 0; i < VELAPAW_NUM_IMAGES; i++)
        {
          if (g_velapaw_image_pet[i] == g_scene)
            {
              if (seen == want)
                {
                  chosen = i;
                  break;
                }
              seen++;
            }
        }

      if (chosen < 0)            /* wrapped past this pet's images */
        {
          g_cursor[g_scene] = 0;
          for (int i = 0; i < VELAPAW_NUM_IMAGES; i++)
            if (g_velapaw_image_pet[i] == g_scene) { chosen = i; break; }
        }

      g_cursor[g_scene]++;
      memcpy(g_framebuf, g_velapaw_images[chosen], sizeof(g_framebuf));
    }

  frame->data     = g_framebuf;
  frame->width    = VELAPAW_FRAME_W;
  frame->height   = VELAPAW_FRAME_H;
  frame->channels = VELAPAW_FRAME_C;
  return 0;
}

#else  /* stub backend: synthetic gradient scenes (3-channel) */

void velapaw_camera_mock_set_scene(int scene_id)
{
  g_scene = scene_id;
}

int velapaw_camera_open(void)
{
  g_scene = -1;
  return 0;
}

int velapaw_camera_get_frame(struct velapaw_frame *frame)
{
  if (frame == NULL)
    {
      return -1;
    }

  for (int y = 0; y < VELAPAW_FRAME_H; y++)
    for (int x = 0; x < VELAPAW_FRAME_W; x++)
      {
        int v = (g_scene < 0) ? 0
                : ((x * (g_scene + 1) + y * (g_scene + 3)) & 0xff);
        int base = (y * VELAPAW_FRAME_W + x) * VELAPAW_FRAME_C;
        for (int c = 0; c < VELAPAW_FRAME_C; c++)
          g_framebuf[base + c] = (uint8_t)v;
      }

  frame->data     = g_framebuf;
  frame->width    = VELAPAW_FRAME_W;
  frame->height   = VELAPAW_FRAME_H;
  frame->channels = VELAPAW_FRAME_C;
  return 0;
}

#endif

void velapaw_camera_close(void)
{
}
