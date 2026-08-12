/****************************************************************************
 * VelaPaw - V4L2 camera backend (emulator goldfish webcam / real sensor)
 *
 * Captures RGB565 frames from /dev/video and downscales (nearest) to
 * FRAME_W x FRAME_H RGB888 for the model + UI. Lets the user enroll a REAL
 * pet through the device camera instead of the embedded demo photos.
 *
 * Non-blocking: if no frame is ready, the last frame is returned (never
 * blocks the UI). On the emulator, goldfish_camera_initialize() registers
 * /dev/video (the vela bringup doesn't); flat build lets the app call it.
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <nuttx/video/video.h>
#include <nuttx/video/v4l2_cap.h>
#ifdef CONFIG_GOLDFISH_CAMERA
#include <nuttx/video/goldfish_camera.h>
#endif

#include "hal/camera.h"

#ifdef CONFIG_VELAPAW_INFER_TFLM
#include "velapaw_assets.h"   /* embedded photos: fallback when no live camera */
#endif

#define CAM_W       640
#define CAM_H       480
#define CAM_BUFNUM  3

static int g_view;            /* fallback: which embedded pet is "in view" */

static int       g_fd = -1;
static uint8_t  *g_buf[CAM_BUFNUM];
static size_t    g_bufsize;
static int       g_started;
static uint8_t   g_framebuf[VELAPAW_FRAME_BYTES];   /* zero (black) until 1st frame */

/* Live camera ignores scenes; fallback uses it to pick the embedded pet. */
void velapaw_camera_mock_set_scene(int scene_id)
{
  g_view = scene_id;
}

static int cam_qbuf(int index)
{
  struct v4l2_buffer b;
  memset(&b, 0, sizeof(b));
  b.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory    = V4L2_MEMORY_USERPTR;
  b.index     = index;
  b.m.userptr = (unsigned long)(uintptr_t)g_buf[index];
  b.length    = g_bufsize;
  return ioctl(g_fd, VIDIOC_QBUF, (uintptr_t)&b);
}

int velapaw_camera_open(void)
{
#ifdef CONFIG_GOLDFISH_CAMERA
  int initret = goldfish_camera_initialize();
  printf("velapaw/cam: goldfish_camera_initialize -> %d\n", initret);
#endif

  g_fd = open("/dev/video", O_RDWR);   /* O_NONBLOCK set after open (below) */
  if (g_fd < 0)
    {
      int e1 = errno;     /* EINVAL(22)=node exists but cam open rejected;
                           * ENOENT(2)=no camera registered (get_list==0) */
      g_fd = open("/dev/video0", O_RDWR);
      printf("velapaw/cam: open /dev/video errno=%d, /dev/video0 errno=%d\n",
             e1, (g_fd < 0) ? errno : 0);
    }
  if (g_fd < 0)
    {
      printf("velapaw/cam: no live camera; using embedded photos\n");
      return 0;   /* fall back to embedded photos in get_frame */
    }

  int fl = fcntl(g_fd, F_GETFL, 0);
  if (fl >= 0)
    {
      fcntl(g_fd, F_SETFL, fl | O_NONBLOCK);   /* DQBUF returns EAGAIN, no hang */
    }

  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = CAM_W;
  fmt.fmt.pix.height      = CAM_H;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(g_fd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0)
    {
      printf("velapaw/cam: S_FMT RGB565 %dx%d failed (errno %d)\n",
             CAM_W, CAM_H, errno);
      close(g_fd); g_fd = -1; return 0; /* fall back to photos */
    }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;
  req.count  = CAM_BUFNUM;
  if (ioctl(g_fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      printf("velapaw/cam: REQBUFS failed (errno %d)\n", errno);
      close(g_fd); g_fd = -1; return 0; /* fall back to photos */
    }

  g_bufsize = (size_t)CAM_W * CAM_H * 2;   /* RGB565 = 2 bytes/pixel */
  for (int i = 0; i < CAM_BUFNUM; i++)
    {
      g_buf[i] = (uint8_t *)memalign(32, g_bufsize);
      if (g_buf[i] == NULL || cam_qbuf(i) < 0)
        {
          printf("velapaw/cam: buffer %d setup failed (errno %d)\n", i, errno);
          close(g_fd); g_fd = -1; return 0; /* fall back to photos */
        }
    }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(g_fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("velapaw/cam: STREAMON failed (errno %d)\n", errno);
      close(g_fd); g_fd = -1; return 0; /* fall back to photos */
    }

  g_started = 1;
  printf("velapaw/cam: streaming %dx%d RGB565 from /dev/video\n", CAM_W, CAM_H);
  return 0;
}

int velapaw_camera_get_frame(struct velapaw_frame *frame)
{
  if (frame == NULL)
    {
      return -1;
    }

  if (g_fd >= 0)
    {
      struct v4l2_buffer b;
      memset(&b, 0, sizeof(b));
      b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      b.memory = V4L2_MEMORY_USERPTR;
      if (ioctl(g_fd, VIDIOC_DQBUF, (uintptr_t)&b) == 0)
        {
          const uint16_t *src = (const uint16_t *)(uintptr_t)g_buf[b.index];

          for (int y = 0; y < VELAPAW_FRAME_H; y++)
            {
              int sy = y * CAM_H / VELAPAW_FRAME_H;
              for (int x = 0; x < VELAPAW_FRAME_W; x++)
                {
                  int sx = x * CAM_W / VELAPAW_FRAME_W;
                  uint16_t p = src[sy * CAM_W + sx];
                  uint8_t r = (p >> 11) & 0x1f;
                  uint8_t g = (p >> 5) & 0x3f;
                  uint8_t bl = p & 0x1f;
                  int o = (y * VELAPAW_FRAME_W + x) * 3;
                  g_framebuf[o + 0] = (uint8_t)((r << 3) | (r >> 2));
                  g_framebuf[o + 1] = (uint8_t)((g << 2) | (g >> 4));
                  g_framebuf[o + 2] = (uint8_t)((bl << 3) | (bl >> 2));
                }
            }

          ioctl(g_fd, VIDIOC_QBUF, (uintptr_t)&b);   /* requeue */
        }
      /* else EAGAIN: no new frame -> return the previous g_framebuf */
    }
#ifdef CONFIG_VELAPAW_INFER_TFLM
  else
    {
      /* No live camera: serve the current view's embedded pet photo. */
      static int cur;
      int pet = (g_view >= 0 && g_view < VELAPAW_NUM_PETS) ? g_view : 0;
      int idxs[VELAPAW_NUM_IMAGES];
      int n = 0;
      for (int i = 0; i < VELAPAW_NUM_IMAGES; i++)
        {
          if (g_velapaw_image_pet[i] == pet)
            {
              idxs[n++] = i;
            }
        }
      int chosen = (n > 0) ? idxs[cur % n] : 0;
      cur++;
      memcpy(g_framebuf, g_velapaw_images[chosen], sizeof(g_framebuf));
    }
#endif

  frame->data     = g_framebuf;
  frame->width    = VELAPAW_FRAME_W;
  frame->height   = VELAPAW_FRAME_H;
  frame->channels = VELAPAW_FRAME_C;
  return 0;
}

void velapaw_camera_close(void)
{
  if (g_fd < 0)
    {
      return;
    }
  if (g_started)
    {
      enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      ioctl(g_fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
      g_started = 0;
    }
  for (int i = 0; i < CAM_BUFNUM; i++)
    {
      free(g_buf[i]);
      g_buf[i] = NULL;
    }
  close(g_fd);
  g_fd = -1;
}
