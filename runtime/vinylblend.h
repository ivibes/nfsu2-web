/* vinylblend.h -- the game's 32-bit car-paint blend (0x611F10, its per-pixel
 * mix 0x611B00) in plain C, on our own buffers: it never reads or writes the
 * game's memory.  See vinylblend.c for what each step is in the game. */
#ifndef VINYLBLEND_H
#define VINYLBLEND_H
#include <stdint.h>

#define VB_MAX_LAYERS   8
#define VB_NMARK        7
#define VB_GAME_SLOTS   16        /* the game's table: 16 per colour, unchecked */
#define VB_MARK_MAX     4096      /* ours: room for an HD template's blocks */

/* One layer as the painter (0x61D790) fills its 0x34-byte record.  The
 * colours are the record's raw words (the blend swaps their bytes 0 and 2
 * itself, as the game does in place). */
typedef struct {
    uint32_t key;                 /* +0x00 texture name hash; 0 = no layer */
    int w, h;                     /* vinyl and mask size; a layer whose size is
                                     not the surface's is skipped */
    const uint32_t *vinyl;        /* +0x0C its pixels (w*h words) */
    const uint32_t *mask;         /* +0x14 its _MASK pixels; byte 1 = coverage */
    uint32_t ncolours;            /* +0x20 0 = vinyl pixels used as they are */
    uint32_t colour[4];           /* +0x24..+0x30; the blend mixes the first 3 */
} VbLayer;

typedef struct {
    int w, h;                     /* the paint surface (TextureInfo +0x44/+0x46) */
    uint32_t base;                /* +0x004 base paint colour, raw */
    uint32_t marker[VB_NMARK];    /* +0x008..+0x020 the marker colours, raw */
    int nlayers;                  /* +0x1C4 */
    VbLayer layer[VB_MAX_LAYERS]; /* +0x024, 0x34 each */
} VbParams;

/* Where the key-colour texels are: pixel indices per colour (red, green,
 * blue, magenta, cyan, yellow, white; all alpha 0xBF).  Unlike the game's
 * table (16 per colour, overflow spilling into the next colour's slots),
 * this keeps up to VB_MARK_MAX per colour, so it matches the game only
 * while every colour has at most VB_GAME_SLOTS markers (the stock template
 * has exactly 16). */
typedef struct {
    int count[VB_NMARK];
    uint32_t pos[VB_NMARK][VB_MARK_MAX];
} VbMarkers;

extern const uint32_t vb_key_colour[VB_NMARK];

/* 0x5BE040: bytes 0 and 2 swapped */
static inline uint32_t vb_swap_rb(uint32_t v){
    return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
}
/* The marker scan the game makes once per run (flag 0x8A1D18) on the stock
 * template, before its first paint. */
void vb_marker_scan(const uint32_t *px, int npix, VbMarkers *m);
/* 0x611B00: n colours (as words in memory) mixed by n float weights;
 * alpha_max: the alpha byte is the largest weighted one, else their sum. */
uint32_t vb_mix(const uint32_t *col, const float *wt, int n, int alpha_max);
/* 0x611F10 after its marker scan: base fill, the layers, the markers
 * written back.  dst: w*h words, pitch w. */
void vb_blend32(uint32_t *dst, const VbParams *p, const VbMarkers *m);
#endif
