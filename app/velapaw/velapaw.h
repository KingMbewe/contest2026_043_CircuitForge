/****************************************************************************
 * VelaPaw - shared types and tunables (contest2026_043)
 *
 * Multi-pet recognition smart feeder. On-device, open-set identity by
 * embedding + cosine match. This header holds the cross-module contract;
 * defaults come from IMPLEMENTATION_PLAN.md Appendix.
 ****************************************************************************/

#ifndef VELAPAW_H
#define VELAPAW_H

#include <stdint.h>
#include <stddef.h>

/* Embedding / model geometry */
#define VELAPAW_EMBED_DIM           128         /* L2-normalized embedding length */
#define VELAPAW_FRAME_W             128         /* model input width  (128px TSFM model) */
#define VELAPAW_FRAME_H             128         /* model input height */
#define VELAPAW_FRAME_C             3           /* channels: RGB (matches model) */
#define VELAPAW_FRAME_BYTES         (VELAPAW_FRAME_W * VELAPAW_FRAME_H * VELAPAW_FRAME_C)

/* Identity store */
#define VELAPAW_MAX_PETS            8
#define VELAPAW_MAX_MEALS           3       /* scheduled meals per pet per day */
#define VELAPAW_NAME_MAX            16
#define VELAPAW_MATCH_THRESHOLD     0.45f       /* cosine; on-device tuned: bob 0.53-0.84 vs unknown <=0.22 */

/* Feeder defaults */
#define VELAPAW_DEFAULT_PORTION_G   15
#define VELAPAW_DEFAULT_COOLDOWN_S  300

/* Sentinel for "no confident match" */
#define VELAPAW_PET_UNKNOWN         (-1)

typedef int velapaw_pet_id_t;

#endif /* VELAPAW_H */
