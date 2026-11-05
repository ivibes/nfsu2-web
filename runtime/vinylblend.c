/* vinylblend.c -- the game's 32-bit car-paint blend in plain C.
 *
 * The game (SPEED2.EXE v1.2):
 *   0x61D790  the painter: per car it fills a 0x1C8-byte record and calls
 *             0x611F10 (or 0x612280 for an 8-bit surface)
 *   0x611F10  the 32-bit blend, cdecl (record):
 *     +0x000  paint surface TextureInfo; no surface or not 32 bpp (+0x4A
 *             != 0x20): returns 0
 *     +0x004  base colour          +0x008  7 marker colours
 *     +0x024  8 layers, 0x34 each  +0x1C4  layer count
 *     1. lock the surface (0x5B96A0); w, h from TextureInfo +0x44, +0x46
 *     2. once per run (flag 0x8A1D18): every texel equal to a key colour
 *        (vb_key_colour) is recorded as its pixel index at
 *        0x8A05E8[colour*16 + n], n counted at 0x8A1D1C[colour], unchecked
 *     3. each marker colour is swapped in place (0x5BE040: bytes 0 and 2)
 *     4. the surface is filled with the swapped base colour
 *     5. per layer whose +0x00 is not 0: if +0x20 is not 0 its 4 colours
 *        are swapped in place; then per pixel, vinyl v, mask m (byte 1):
 *          colour  = +0x20 ? 0x611B00(colours 0..2, weights (v byte 2,
 *                    byte 1, byte 0) * (1/255.0f), 3, alpha max) : v
 *          m > 127 the pixel is that colour; m 1..127 it is 0x611B00(
 *          (colour, pixel), (a = m * (1/128.0f), 1 - a), 2, alpha sum);
 *          m 0 it stays (the colour is not computed when m is 0)
 *     6. each recorded marker texel is set to its swapped marker colour
 *     7. unlock (0x5B96D0); returns 1
 *   0x611B00  (colours, weights, n, alpha_max): per colour i whose weight
 *             is above 1/255.0f (so a vinyl channel of 1 counts as 0):
 *             bytes 0..2 summed as trunc(byte * w) (x87 at PC=24: a float
 *             multiply, then _ftol), byte 3 the largest trunc(byte * w)
 *             (as a byte) or their sum; each sum clamped to 255.
 * The game thread runs the x87 at PC=24, RC=nearest (Direct3D's control
 * word): every product above is a float product.  Here they are float
 * products, so the results are the same bit for bit (compile without fused
 * multiply-add contraction; there is nothing to contract).
 * The swaps the game makes in place (so a record blended twice would swap
 * back) are made here on copies: the inputs are never written. */
#include "vinylblend.h"
#include <string.h>

const uint32_t vb_key_colour[VB_NMARK] = {
    0xbfff0000u, 0xbf00ff00u, 0xbf0000ffu, 0xbfff00ffu, 0xbf00ffffu, 0xbfffff00u, 0xbfffffffu,
};

/* the game's constants: 0x784548, 0x787AF0 */
#define K_255  0x1.010102p-8f     /* 0x3B808081, 1/255 rounded to float */
#define K_128  0.0078125f         /* 0x3C000000 */

void vb_marker_scan(const uint32_t *px, int npix, VbMarkers *m){
    memset(m->count, 0, sizeof m->count);
    for (int i = 0; i < npix; i++)
        for (int k = 0; k < VB_NMARK; k++)
            if (px[i] == vb_key_colour[k]){
                if (m->count[k] < VB_MARK_MAX) m->pos[k][m->count[k]] = (uint32_t)i;
                m->count[k]++;
                break;
            }
}

static inline int32_t ftol(float x){ return (int32_t)x; }   /* _ftol: toward zero */

uint32_t vb_mix(const uint32_t *col, const float *wt, int n, int alpha_max){
    int32_t s0 = 0, s1 = 0, s2 = 0, a = 0;
    for (int i = 0; i < n; i++){
        float w = wt[i];
        if (!(w > K_255)) continue;              /* fcom; test ah,0x41: <=, or unordered */
        uint32_t c = col[i];
        s2 += ftol((float)((c >> 16) & 0xffu) * w);
        s1 += ftol((float)((c >> 8) & 0xffu) * w);
        s0 += ftol((float)(c & 0xffu) * w);
        int32_t t = ftol((float)(c >> 24) * w);
        if (alpha_max){ t &= 0xff; if (t > a) a = t; }
        else a += t;
    }
    uint32_t r0 = s0 > 255 ? 255u : (uint32_t)s0 & 0xffu;
    uint32_t r1 = s1 > 255 ? 255u : (uint32_t)s1 & 0xffu;
    uint32_t r2 = s2 > 255 ? 255u : (uint32_t)s2 & 0xffu;
    uint32_t r3 = a  > 255 ? 255u : (uint32_t)a  & 0xffu;
    return r0 | r1 << 8 | r2 << 16 | r3 << 24;
}

void vb_blend32(uint32_t *dst, const VbParams *p, const VbMarkers *m){
    const int npix = p->w * p->h;
    const uint32_t base = vb_swap_rb(p->base);
    for (int i = 0; i < npix; i++) dst[i] = base;
    for (int l = 0; l < p->nlayers && l < VB_MAX_LAYERS; l++){
        const VbLayer *L = &p->layer[l];
        if (!L->key || L->w != p->w || L->h != p->h) continue;
        uint32_t col[4];
        for (int k = 0; k < 4; k++) col[k] = L->ncolours ? vb_swap_rb(L->colour[k]) : L->colour[k];
        for (int i = 0; i < npix; i++){
            uint32_t v = L->vinyl[i];
            uint32_t cov = (L->mask[i] >> 8) & 0xffu;
            if (!cov) continue;
            uint32_t c = v;
            if (L->ncolours){
                float wt[3] = {
                    (float)((v >> 16) & 0xffu) * K_255,
                    (float)((v >> 8) & 0xffu) * K_255,
                    (float)(v & 0xffu) * K_255,
                };
                c = vb_mix(col, wt, 3, 1);
            }
            if (cov > 127u){ dst[i] = c; continue; }
            float a = (float)cov * K_128, b = 1.0f - a;   /* cov 1..127: a < 1 */
            uint32_t pair[2] = { c, dst[i] };
            float w2[2] = { a, b };
            dst[i] = vb_mix(pair, w2, 2, 0);
        }
    }
    for (int k = 0; k < VB_NMARK; k++){
        uint32_t mc = vb_swap_rb(p->marker[k]);
        int n = m->count[k] < VB_MARK_MAX ? m->count[k] : VB_MARK_MAX;
        for (int j = 0; j < n; j++)
            if (m->pos[k][j] < (uint32_t)npix) dst[m->pos[k][j]] = mc;
    }
}
