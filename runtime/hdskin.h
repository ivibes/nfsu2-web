/* hdskin.h -- HD copies of the car paint surfaces (the "HD vinyls" test
 * feature).  After the game's own blend (0x611F10, runtime/paintblend.c) has
 * painted its stock surface, hdskin_composite paints a copy of it s times
 * larger on our own memory with the same blend (runtime/vinylblend.c), from
 * HD sources: a remastered texture when the remaster directory has one, else
 * the stock pixels scaled up nearest-neighbour (so the copy is the stock look,
 * each stock pixel an s x s block).  Plain C, no game memory: the caller
 * passes the stock inputs.
 *
 * Remasters: <dir>/<TEXTURE NAME>.dds, the texture's full name as the game
 * hashes it (bStringHash 0x43DB50 of e.g. SKYLINE_TEAR_012 or
 * SKYLINE_TEAR_012_MASK; the file's stem is upper-cased first, so case does
 * not matter), uncompressed 32-bit DDS (A8R8G8B8, A8B8G8R8 or X8R8G8B8, mip 0
 * read, top row first).  Its size should be the stock size times s; an
 * integer multiple of that is box-filtered down, an integer fraction scaled
 * up nearest; any other size is refused (logged once) and the stand-in used.
 * A mask's coverage is its green channel (a grey mask works).  For a layer
 * the player colours (record +0x20 = 1) the vinyl's red, green and blue are
 * the weights of the player's colours 0, 1, 2, not colours: a remaster of
 * such a vinyl must be a weight map like the stock one, not final art.  The
 * directory is indexed once (hdskin_set_dir); files added later need a new
 * hdskin_set_dir.  Not thread-safe: call everything from the guest thread. */
#ifndef HDSKIN_H
#define HDSKIN_H
#include <stddef.h>
#include <stdint.h>
#include "vinylblend.h"

#define HD_MAX_SCALE   4
#define HD_MAX_SIDE    4096
#define HD_CACHE_BYTES (128u << 20)   /* sources kept between composites */

/* a layer texture's name; hash = bStringHash of its full name (TextureInfo
 * +0x24), 0 = computed from name (which the game truncates at 23 chars) */
typedef struct { const char *name; uint32_t hash; } HdTexName;

typedef struct {
    int remastered;       /* textures (vinyls + masks) from the directory */
    int standin;          /* textures scaled up from the stock pixels */
    int cached;           /* of those, how many were already in the cache */
    int markers;          /* HD marker texels written (blocks x s x s) */
    uint32_t generation;
    double ms_sources, ms_blend;
} HdStats;

uint32_t hd_name_hash(const char *s);            /* bStringHash: h = -1, h = h*33 + c */
/* dir NULL or "" = no remasters; returns how many .dds files it indexed */
int  hdskin_set_dir(const char *dir);

/* Paint the HD copy of surface key (its TextureInfo address) at scale s
 * (1..HD_MAX_SCALE).  stock: the record's inputs at stock size, its colours
 * raw (as before the game's in-place swaps); names[l][0] vinyl, [1] mask;
 * marks: the game's marker table (positions at stock size).  Returns 1 and
 * fills st (if given), 0 if nothing was painted (bad size, no memory). */
int  hdskin_composite(uint32_t key, const char *surface_name, const VbParams *stock,
                      const HdTexName names[VB_MAX_LAYERS][2], const VbMarkers *marks,
                      int s, HdStats *st);

/* For the GPU stage: the latest HD copy of a surface, or NULL.  generation
 * grows with every composite (of any surface), so a changed value means new
 * pixels.  Pixels as the game's (0xAARRGGBB words, top row first, pitch w). */
const uint32_t *hdskin_get(uint32_t surface_ti, int *w, int *h, uint32_t *generation);
void hdskin_forget(uint32_t surface_ti);
/* frees every surface whose keep() says 0 */
void hdskin_prune(int (*keep)(uint32_t ti, const char *name, void *ctx), void *ctx);
void hdskin_reset(void);                         /* everything: surfaces and sources */
void hdskin_memory(size_t *surfaces, size_t *sources);

/* parts, exposed for the unit test */
void hd_scale_nearest(const uint32_t *src, int w, int h, int s, uint32_t *dst);
/* marks at w x h to an s-times grid: every marker texel an s x s block */
void hd_markers_scale(const VbMarkers *m, int w, int h, int s, VbMarkers *out);
uint32_t *hd_load_dds(const char *path, int *w, int *h, char *why, size_t whylen);
#endif
