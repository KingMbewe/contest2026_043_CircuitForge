/****************************************************************************
 * VelaPaw - enrollment store + cosine matcher (in-memory)
 *
 * Persistence is delegated to store/ (stub for now). Embeddings are assumed
 * L2-normalized, so cosine similarity == dot product.
 ****************************************************************************/

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include "identity/identity.h"

static struct velapaw_pet g_pets[VELAPAW_MAX_PETS];
static int                g_npets;

/****************************************************************************
 * Flash persistence: enrolled pets survive a reset / power-off.
 *
 * Layout at VELAPAW_PET_FLASH_OFF (well past the models @0x600000 / @0x760000):
 *   magic "VPAW" | version | npets | g_pets[npets]      (raw POD copy)
 * A blank or garbage sector fails the magic check and reads as "no pets",
 * so we never restore nonsense.
 *
 * IMPORTANT: erase/write SUSPEND THE FLASH CACHE, so they must not run on a
 * PSRAM stack (the velapaw task's stack is PSRAM). Same trap the model loader
 * hit -- so, like backend_tflm.cc, we run them on a pthread whose stack is an
 * internal-RAM .bss buffer.
 ****************************************************************************/

extern int spi_flash_read(uint32_t src, void *dst, uint32_t size);
extern int spi_flash_write(uint32_t dst, const void *src, uint32_t size);
extern int spi_flash_erase_range(uint32_t start, uint32_t size);

#define VELAPAW_PET_FLASH_OFF  0x800000u          /* 8MB mark, past the models */
#define VELAPAW_PET_MAGIC      0x57415056u        /* "VPAW" */
#define VELAPAW_PET_VERSION    1u

/* Header only (12 B). We deliberately do NOT keep a serialization copy of the
 * pets -- we write/read g_pets[] in place, so the RAM cost here is just the
 * header + the flash thread stack (~4 KB), not another 4.5 KB of pets. */
struct pet_hdr { uint32_t magic; uint32_t version; uint32_t npets; };

#define PET_HDR_RESERVED  16u                     /* keeps pets 16-B aligned */
#define PET_AREA_BYTES    8192u                   /* 2 x 4 KB flash sectors  */

static struct pet_hdr g_hdr;                      /* 12 bytes, .bss          */
static uint8_t g_pet_flash_stack[4096];           /* internal-RAM stack      */

/* --- per-pet card icons ---------------------------------------------------
 * Kept in a SEPARATE flash region from the pet record so old pets still load,
 * and written only at enroll/delete (not on every schedule edit). The icon
 * buffers live in .bss (internal SRAM), so the flash write reads them
 * coherently while the cache is suspended -- no PSRAM bounce needed. */
#define VELAPAW_ICON_PX        (VELAPAW_ICON_W * VELAPAW_ICON_H)
#define VELAPAW_ICON_FLASH_OFF 0x810000u          /* past the pet record       */
#define VELAPAW_ICON_MAGIC     0x4E434956u        /* "VICN"                    */
#define VELAPAW_ICON_AREA_BYTES 32768u            /* 8 x 4 KB sectors          */

static uint16_t g_icon[VELAPAW_MAX_PETS][VELAPAW_ICON_PX];  /* ~25 KB .bss     */
static uint8_t  g_icon_valid[VELAPAW_MAX_PETS];             /* has an icon?    */
struct icon_hdr { uint32_t magic; uint32_t version; };
static struct icon_hdr g_ihdr;

struct pet_io { int write; int rc; };

/* Runs on the internal-RAM-stack thread: flash erase/write suspend the cache,
 * so none of this may execute on the velapaw task's PSRAM stack. */
static void *pet_flash_thread(void *arg)
{
  struct pet_io *io = (struct pet_io *)arg;

  if (io->write)
    {
      io->rc = spi_flash_erase_range(VELAPAW_PET_FLASH_OFF, PET_AREA_BYTES);
      if (io->rc >= 0)
        {
          io->rc = spi_flash_write(VELAPAW_PET_FLASH_OFF, &g_hdr, sizeof(g_hdr));
        }
      if (io->rc >= 0)
        {
          io->rc = spi_flash_write(VELAPAW_PET_FLASH_OFF + PET_HDR_RESERVED,
                                   g_pets, sizeof(g_pets));
        }
    }
  else
    {
      io->rc = spi_flash_read(VELAPAW_PET_FLASH_OFF, &g_hdr, sizeof(g_hdr));
      if (io->rc >= 0 &&
          g_hdr.magic   == VELAPAW_PET_MAGIC &&
          g_hdr.version == VELAPAW_PET_VERSION &&
          g_hdr.npets   <= VELAPAW_MAX_PETS)
        {
          /* header is good -> only now do we overwrite g_pets */
          io->rc = spi_flash_read(VELAPAW_PET_FLASH_OFF + PET_HDR_RESERVED,
                                  g_pets, sizeof(g_pets));
        }
      else
        {
          io->rc = -1;                            /* blank/garbage -> no pets */
        }
    }
  return NULL;
}

static int pet_flash_run(int write)
{
  struct pet_io io = { write, -1 };
  pthread_attr_t attr;
  pthread_t th;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, g_pet_flash_stack, sizeof(g_pet_flash_stack));
  if (pthread_create(&th, &attr, pet_flash_thread, &io) != 0)
    {
      pthread_attr_destroy(&attr);
      return -1;
    }
  pthread_join(th, NULL);
  pthread_attr_destroy(&attr);
  return io.rc;
}

int velapaw_identity_save(void)
{
  g_hdr.magic   = VELAPAW_PET_MAGIC;
  g_hdr.version = VELAPAW_PET_VERSION;
  g_hdr.npets   = (uint32_t)g_npets;
  int rc = pet_flash_run(1);
  printf("velapaw/identity: saved %d pet(s) to flash rc=%d\n", g_npets, rc);
  return rc;
}

/* Icon flash I/O, same internal-RAM-stack pattern as the pets. Reuses the pet
 * flash stack (they never run at the same time). */
static void *icon_flash_thread(void *arg)
{
  struct pet_io *io = (struct pet_io *)arg;

  if (io->write)
    {
      g_ihdr.magic = VELAPAW_ICON_MAGIC; g_ihdr.version = 1;
      io->rc = spi_flash_erase_range(VELAPAW_ICON_FLASH_OFF,
                                     VELAPAW_ICON_AREA_BYTES);
      if (io->rc >= 0)
        io->rc = spi_flash_write(VELAPAW_ICON_FLASH_OFF, &g_ihdr, sizeof(g_ihdr));
      if (io->rc >= 0)
        io->rc = spi_flash_write(VELAPAW_ICON_FLASH_OFF + 16,
                                 g_icon_valid, sizeof(g_icon_valid));
      if (io->rc >= 0)
        io->rc = spi_flash_write(VELAPAW_ICON_FLASH_OFF + 32,
                                 g_icon, sizeof(g_icon));
    }
  else
    {
      io->rc = spi_flash_read(VELAPAW_ICON_FLASH_OFF, &g_ihdr, sizeof(g_ihdr));
      if (io->rc >= 0 && g_ihdr.magic == VELAPAW_ICON_MAGIC)
        {
          spi_flash_read(VELAPAW_ICON_FLASH_OFF + 16,
                         g_icon_valid, sizeof(g_icon_valid));
          spi_flash_read(VELAPAW_ICON_FLASH_OFF + 32, g_icon, sizeof(g_icon));
        }
      else
        {
          io->rc = -1;                         /* blank/old flash -> no icons */
        }
    }
  return NULL;
}

static int icon_flash_run(int write)
{
  struct pet_io io = { write, -1 };
  pthread_attr_t attr; pthread_t th;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, g_pet_flash_stack, sizeof(g_pet_flash_stack));
  if (pthread_create(&th, &attr, icon_flash_thread, &io) != 0)
    {
      pthread_attr_destroy(&attr);
      return -1;
    }
  pthread_join(th, NULL);
  pthread_attr_destroy(&attr);
  return io.rc;
}

void velapaw_identity_set_icon(velapaw_pet_id_t id, const uint16_t *rgb565)
{
  if (id < 0 || id >= VELAPAW_MAX_PETS || rgb565 == NULL)
    {
      return;
    }
  memcpy(g_icon[id], rgb565, sizeof(g_icon[id]));
  g_icon_valid[id] = 1;
}

const uint16_t *velapaw_identity_get_icon(velapaw_pet_id_t id)
{
  if (id < 0 || id >= VELAPAW_MAX_PETS || !g_icon_valid[id])
    {
      return NULL;
    }
  return g_icon[id];
}

int velapaw_identity_save_icons(void)
{
  int rc = icon_flash_run(1);
  printf("velapaw/identity: saved icons to flash rc=%d\n", rc);
  return rc;
}

int velapaw_identity_load(void)
{
  /* icons load independently -- old pets have none */
  if (icon_flash_run(0) < 0)
    {
      memset(g_icon_valid, 0, sizeof(g_icon_valid));
    }

  if (pet_flash_run(0) < 0)
    {
      printf("velapaw/identity: no saved pets (blank/invalid flash)\n");
      g_npets = 0;
      return -1;
    }
  g_npets = (int)g_hdr.npets;
  printf("velapaw/identity: restored %d pet(s) from flash\n", g_npets);
  return g_npets;
}

void velapaw_identity_init(void)
{
  memset(g_pets, 0, sizeof(g_pets));
  memset(g_icon_valid, 0, sizeof(g_icon_valid));
  g_npets = 0;
  velapaw_identity_load();      /* bring back previously enrolled pets + icons */
}

int velapaw_identity_count(void)
{
  return g_npets;
}

const struct velapaw_pet *velapaw_identity_get(velapaw_pet_id_t id)
{
  if (id < 0 || id >= g_npets)
    {
      return NULL;
    }

  return &g_pets[id];
}

velapaw_pet_id_t velapaw_identity_enroll(
    const char *name, int portion_g, int cooldown_s, int daily_limit_g,
    const int *meal_min,
    const float (*samples)[VELAPAW_EMBED_DIM], int n_samples)
{
  if (g_npets >= VELAPAW_MAX_PETS || samples == NULL || n_samples <= 0)
    {
      return VELAPAW_PET_UNKNOWN;
    }

  struct velapaw_pet *p = &g_pets[g_npets];

  p->id            = g_npets;
  p->portion_g     = portion_g;
  p->cooldown_s    = cooldown_s;
  p->daily_limit_g = daily_limit_g;
  p->samples_n     = n_samples;
  for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
    {
      p->meal_min[k] = meal_min ? meal_min[k] : -1;
    }
  strncpy(p->name, name ? name : "pet", VELAPAW_NAME_MAX - 1);
  p->name[VELAPAW_NAME_MAX - 1] = '\0';

  /* Average the sample embeddings. */
  for (int d = 0; d < VELAPAW_EMBED_DIM; d++)
    {
      float acc = 0.0f;
      for (int s = 0; s < n_samples; s++)
        {
          acc += samples[s][d];
        }

      p->embedding[d] = acc / (float)n_samples;
    }

  /* Re-normalize the averaged centroid. */
  float norm = 0.0f;
  for (int d = 0; d < VELAPAW_EMBED_DIM; d++)
    {
      norm += p->embedding[d] * p->embedding[d];
    }

  norm = sqrtf(norm);
  if (norm < 1e-6f)
    {
      norm = 1.0f;
    }

  for (int d = 0; d < VELAPAW_EMBED_DIM; d++)
    {
      p->embedding[d] /= norm;
    }

  return g_pets[g_npets++].id;
}

int velapaw_identity_update(velapaw_pet_id_t id, int portion_g,
                            int daily_limit_g, const int *meal_min)
{
  if (id < 0 || id >= g_npets)
    {
      return -1;
    }

  struct velapaw_pet *p = &g_pets[id];

  if (portion_g > 0)
    {
      p->portion_g = portion_g;
    }

  if (daily_limit_g > 0)
    {
      /* a limit below one serving would block every feed */
      p->daily_limit_g = (daily_limit_g < p->portion_g) ? p->portion_g
                                                        : daily_limit_g;
    }

  if (meal_min != NULL)
    {
      for (int k = 0; k < VELAPAW_MAX_MEALS; k++)
        {
          p->meal_min[k] = meal_min[k];
        }
    }

  /* embedding / name / samples_n deliberately untouched: editing a schedule
   * must never disturb recognition. */
  return 0;
}

int velapaw_identity_delete(velapaw_pet_id_t id)
{
  if (id < 0 || id >= g_npets)
    {
      return -1;
    }

  /* Compact the array and renumber: ids are indices, so every pet after the
   * deleted one shifts down by one. Callers MUST shift their own per-pet
   * tables the same way or the data will belong to the wrong pet. */
  for (int i = id; i < g_npets - 1; i++)
    {
      g_pets[i]    = g_pets[i + 1];
      g_pets[i].id = i;
      memcpy(g_icon[i], g_icon[i + 1], sizeof(g_icon[0]));  /* icon follows pet */
      g_icon_valid[i] = g_icon_valid[i + 1];
    }

  memset(&g_pets[g_npets - 1], 0, sizeof(g_pets[0]));
  g_icon_valid[g_npets - 1] = 0;
  g_npets--;
  printf("velapaw/identity: deleted pet %d, %d left\n", (int)id, g_npets);
  return 0;
}

void velapaw_identity_match(const float *embedding, struct velapaw_match *out)
{
  if (out == NULL)
    {
      return;
    }

  out->id     = VELAPAW_PET_UNKNOWN;
  out->score  = -1.0f;
  out->margin = 0.0f;

  if (embedding == NULL || g_npets == 0)
    {
      return;
    }

  float best = -1.0f;
  float second = -1.0f;
  velapaw_pet_id_t best_id = VELAPAW_PET_UNKNOWN;

  for (int i = 0; i < g_npets; i++)
    {
      float dot = 0.0f;
      for (int d = 0; d < VELAPAW_EMBED_DIM; d++)
        {
          dot += embedding[d] * g_pets[i].embedding[d];
        }

      if (dot > best)
        {
          second  = best;
          best    = dot;
          best_id = g_pets[i].id;
        }
      else if (dot > second)
        {
          second = dot;
        }
    }

  out->score  = best;
  out->margin = (second > -1.0f) ? (best - second) : best;

  if (best >= VELAPAW_MATCH_THRESHOLD)
    {
      out->id = best_id;
    }
}
