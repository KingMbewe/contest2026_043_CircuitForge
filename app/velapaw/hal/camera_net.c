/****************************************************************************
 * VelaPaw - adb-bridge camera backend (host webcam into the emulator)
 *
 * The goldfish camera can't access the host webcam on Windows, AND the guest
 * virtio-net is broken (wlan0 UP-not-RUNNING, connect()->ENETUNREACH). So we
 * route AROUND the dead IP stack using adb's qemud-pipe transport, which lands
 * on the guest LOOPBACK (lo is RUNNING):
 *
 *   device: TCP server on 127.0.0.1:8081 (this file)
 *   host  : adb forward tcp:9999 tcp:8081
 *           webcam_adb.py connects to 127.0.0.1:9999 and pushes frames
 *
 * Protocol: device (server) sends 'G'; host replies with W*H*3 RGB888 bytes.
 * Falls back to embedded photos whenever no client is connected.
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "hal/camera.h"

#ifdef CONFIG_VELAPAW_INFER_TFLM
#include "velapaw_assets.h"   /* embedded photos: fallback when no client */
#endif

#define BRIDGE_PORT 8081

static int      g_listen = -1;
static int      g_conn   = -1;
static int      g_view;
static uint8_t  g_framebuf[VELAPAW_FRAME_BYTES];

void velapaw_camera_mock_set_scene(int scene_id)
{
  g_view = scene_id;   /* used only by the embedded-photo fallback */
}

int velapaw_camera_open(void)
{
  g_listen = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen < 0)
    {
      printf("velapaw/cam: socket failed (errno %d); embedded photos\n", errno);
      return 0;
    }

  int on = 1;
  setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family      = AF_INET;
  a.sin_port        = htons(BRIDGE_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);   /* adbd connects via loopback */
  if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 ||
      listen(g_listen, 1) < 0)
    {
      printf("velapaw/cam: bind/listen :%d failed (errno %d); embedded photos\n",
             BRIDGE_PORT, errno);
      close(g_listen);
      g_listen = -1;
      return 0;
    }

  /* Non-blocking accept so the UI never stalls waiting for a client. */
  int fl = fcntl(g_listen, F_GETFL, 0);
  if (fl >= 0)
    {
      fcntl(g_listen, F_SETFL, fl | O_NONBLOCK);
    }

  printf("velapaw/cam: webcam bridge listening on :%d "
         "(host: adb forward tcp:9999 tcp:%d)\n", BRIDGE_PORT, BRIDGE_PORT);
  return 0;
}

static int recv_full(int s, uint8_t *buf, int n)
{
  int got = 0;
  while (got < n)
    {
      int r = recv(s, buf + got, n - got, 0);
      if (r <= 0)
        {
          return -1;
        }
      got += r;
    }
  return 0;
}

int velapaw_camera_get_frame(struct velapaw_frame *frame)
{
  if (frame == NULL)
    {
      return -1;
    }

  /* Accept the host pusher if it has connected (non-blocking). */
  if (g_listen >= 0 && g_conn < 0)
    {
      int c = accept(g_listen, NULL, NULL);
      if (c >= 0)
        {
          int fl = fcntl(c, F_GETFL, 0);
          if (fl >= 0)
            {
              fcntl(c, F_SETFL, fl & ~O_NONBLOCK);   /* blocking + timeout */
            }
          struct timeval tv;
          tv.tv_sec  = 2;
          tv.tv_usec = 0;
          setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
          setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
          g_conn = c;
          printf("velapaw/cam: webcam client connected\n");
        }
    }

  if (g_conn >= 0)
    {
      char req = 'G';
      if (send(g_conn, &req, 1, 0) == 1 &&
          recv_full(g_conn, g_framebuf, VELAPAW_FRAME_BYTES) == 0)
        {
          /* live RGB888 frame received into g_framebuf */
        }
      else
        {
          close(g_conn);
          g_conn = -1;
          printf("velapaw/cam: webcam client disconnected; embedded photos\n");
        }
    }

  if (g_conn < 0)
    {
#ifdef CONFIG_VELAPAW_INFER_TFLM
      /* fallback: serve the current view's embedded photo */
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
#endif
    }

  frame->data     = g_framebuf;
  frame->width    = VELAPAW_FRAME_W;
  frame->height   = VELAPAW_FRAME_H;
  frame->channels = VELAPAW_FRAME_C;
  return 0;
}

void velapaw_camera_close(void)
{
  if (g_conn >= 0)
    {
      close(g_conn);
      g_conn = -1;
    }
  if (g_listen >= 0)
    {
      close(g_listen);
      g_listen = -1;
    }
}
