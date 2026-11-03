/****************************************************************************
 * VelaPaw - enrollment store + matcher
 *
 * Open-set identity: enrolled pets each hold an averaged, L2-normalized
 * embedding; matching is cosine similarity vs. each, accept if > threshold.
 ****************************************************************************/

#ifndef VELAPAW_IDENTITY_H
#define VELAPAW_IDENTITY_H

#include "velapaw.h"

struct velapaw_pet
{
  velapaw_pet_id_t id;
  char             name[VELAPAW_NAME_MAX];
  int              portion_g;
  int              cooldown_s;
  int              daily_limit_g;   /* max grams/day; feeder stops at this */
  int              meal_min[VELAPAW_MAX_MEALS]; /* meal times, min-of-day (-1 = off) */
  int              samples_n;
  float            embedding[VELAPAW_EMBED_DIM];
};

struct velapaw_match
{
  velapaw_pet_id_t id;      /* VELAPAW_PET_UNKNOWN if below threshold */
  float            score;   /* cosine similarity of best match */
  float            margin;  /* best - second_best (for benchmark panel) */
};

void velapaw_identity_init(void);

/* Enroll a pet from N sample embeddings (averaged + re-normalized).
 * Returns the new pet_id, or VELAPAW_PET_UNKNOWN on failure (e.g. store full).
 */
velapaw_pet_id_t velapaw_identity_enroll(
    const char *name, int portion_g, int cooldown_s, int daily_limit_g,
    const int *meal_min,   /* VELAPAW_MAX_MEALS entries, -1 = slot off */
    const float (*samples)[VELAPAW_EMBED_DIM], int n_samples);

/* Edit an already-enrolled pet's feeding settings. The owner can move meal
 * times / change portions WITHOUT re-enrolling: the embedding, name and sample
 * count are untouched, so recognition is unaffected. Call velapaw_identity_save()
 * afterwards to persist. Returns 0, or <0 if `id` is not enrolled. */
int velapaw_identity_update(velapaw_pet_id_t id, int portion_g,
                            int daily_limit_g,
                            const int *meal_min);  /* VELAPAW_MAX_MEALS, -1=off */

/* Remove an enrolled pet. NOTE: pet ids ARE array indices, so deleting one
 * RENUMBERS every pet after it (pet n+1 becomes n). Any per-pet table keyed by
 * id must be shifted to match -- see velapaw_store_delete() and the UI's meal
 * tables. Call velapaw_identity_save() afterwards to persist.
 * Returns 0, or <0 if `id` is not enrolled. */
int velapaw_identity_delete(velapaw_pet_id_t id);

/* Cosine match `embedding` against all enrolled pets. */
void velapaw_identity_match(const float *embedding, struct velapaw_match *out);

const struct velapaw_pet *velapaw_identity_get(velapaw_pet_id_t id);
int velapaw_identity_count(void);

/* Per-pet card thumbnail. Stored SEPARATELY from the pet record (its own flash
 * region) so pets enrolled before this feature still load -- they just have no
 * icon. `rgb565` is VELAPAW_ICON_W x VELAPAW_ICON_H, display-oriented. */
#define VELAPAW_ICON_W 40
#define VELAPAW_ICON_H 40
void velapaw_identity_set_icon(velapaw_pet_id_t id, const uint16_t *rgb565);
const uint16_t *velapaw_identity_get_icon(velapaw_pet_id_t id); /* NULL if none */
int  velapaw_identity_save_icons(void);

/* Persistence: pets are stored in raw flash so they survive reset/power-off.
 * load() is called automatically by velapaw_identity_init(); call save() after
 * enrolling (or after editing a pet). Returns <0 on failure / no saved data. */
int velapaw_identity_save(void);

/* Set ONE meal slot for a pet looked up BY NAME.  The agent and the voice
 * dialog both speak names, never ids, and ids are array indices that renumber
 * on delete -- so a name lookup is the only stable handle either of them has.
 *
 * Matching folds case, leading/trailing whitespace and trailing punctuation,
 * because names reach flash verbatim from the on-screen keyboard: the board
 * currently holds "bob," and "bob" must still match it.
 *
 * `slot` is 0-based; `minute` is minute-of-day, or -1 to switch the slot off.
 * The other two slots and the pet's portion, limit and embedding are left
 * untouched.  Persists on success.
 *
 * Returns 0 on success, -1 bad arguments, -2 no such pet, -3 the name is
 * ambiguous (two pets match), -4 the update was rejected, or the negative
 * rc from velapaw_identity_save(). */
int velapaw_meal_set_by_name(const char *name, int slot, int minute);
int velapaw_identity_load(void);

/* Changes whenever velapaw_meal_set_by_name() rewrites a pet.  That call runs
 * on the ai_agent thread, so the UI polls this to know My Pets is stale. */
unsigned velapaw_identity_gen(void);

#endif /* VELAPAW_IDENTITY_H */
