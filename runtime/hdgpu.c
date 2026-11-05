/* hdgpu.c -- the HD paint copies on the GPU; see hdgpu.h. */
#include "hdgpu.h"
#include "hdskin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XXH_INLINE_ALL
#include "xxhash.h"

#define HDGPU_MAXTEX 16384u               /* = texrep.h TEXREP_MAXTEX, metal.m / gpu_web.c MAXTEX */

typedef struct {
    uint32_t ti, gen;                     /* the surface shown and its composite */
    uint64_t h512;                        /* the game's level 0 of the last blend */
    uint8_t *last;                        /* a copy of it, for hdgpu_be_hide */
    size_t nlast;
    uint16_t ow, oh; uint8_t olev, shown, have; uint32_t ofmt;
} Slot;
static Slot *g_t;                         /* per texture id; NULL until the first blend with HD on */
static unsigned g_blend_mid;              /* inside the game's blend of this id */
static unsigned long g_n_show, g_n_same, g_n_inblend, g_n_stale, g_n_surf, g_n_fail, g_n_rel, g_n_moved;

uint64_t hdgpu_hash(const void *p, size_t n){ return XXH3_64bits(p, n); }

int hdgpu_probe(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NFSU2_HD_PROBE"); v = e && *e && *e != '0'; }
    return v;
}

/* recreated: the id already holds a new texture of its own size, so the
 * platform only stops showing the copy (ow = oh = 0) */
static void slot_hide_as(unsigned mid, const uint8_t *data, size_t n, int recreated){
    Slot *s = &g_t[mid];
    if (!s->shown) return;
    if (recreated) hdgpu_be_hide(mid, NULL, 0, 0, 0, 0, s->ofmt);
    else hdgpu_be_hide(mid, data, n, s->ow, s->oh, s->olev, s->ofmt);
    s->shown = 0;
}
static void slot_hide(unsigned mid, const uint8_t *data, size_t n){ slot_hide_as(mid, data, n, 0); }
static void slot_clear(unsigned mid){
    Slot *s = &g_t[mid];
    free(s->last);
    memset(s, 0, sizeof *s);
}

void hdgpu_blend_begin(unsigned mid){
    if (!g_t && !(g_t = calloc(HDGPU_MAXTEX, sizeof *g_t))) return;
    g_blend_mid = mid < HDGPU_MAXTEX ? mid : 0;
}

void hdgpu_blend_end(unsigned mid, uint32_t ti, const uint32_t *px, int w, int h, uint32_t gen,
                     unsigned ow, unsigned oh, unsigned olevels, unsigned ofmt){
    g_blend_mid = 0;
    if (!g_t || !mid || mid >= HDGPU_MAXTEX) return;
    Slot *s = &g_t[mid];
    if (!px || w <= 0 || h <= 0 || !ow || !oh || (unsigned)w % ow || (unsigned)h % oh || (unsigned)w / ow != (unsigned)h / oh){
        if (s->shown){
            g_n_fail++;
            fprintf(stderr, "[hd-vinyls] gpu t%u: no HD copy from this blend, the game's own texture\n", mid);
            slot_hide(mid, s->have ? s->last : NULL, s->have ? s->nlast : 0);
        }
        return;
    }
    /* the surface on another texture id before (the game made its texture again) */
    if (ti != s->ti)
        for (unsigned i = 1; i < HDGPU_MAXTEX; i++)
            if (i != mid && g_t[i].shown && g_t[i].ti == ti){ g_n_moved++; slot_hide(i, NULL, 0); slot_clear(i); }
    unsigned sc = (unsigned)w / ow, k = 0;
    while ((1u << k) < sc) k++;
    uint64_t hh = hdgpu_probe() ? hdgpu_hash(px, (size_t)w * (size_t)h * 4u) : 0;
    s->ti = ti; s->gen = gen;
    s->ow = (uint16_t)ow; s->oh = (uint16_t)oh; s->olev = (uint8_t)(olevels > 255 ? 255 : olevels); s->ofmt = ofmt;
    hdgpu_be_show(mid, px, w, h, k, hh, ow, oh, olevels, ofmt);
    s->shown = 1;
    g_n_show++;
    if (hdgpu_probe())
        fprintf(stderr, "[hd-vinyls] gpu show t%u ti=%08x %ux%u -> %dx%d k=%u gen=%u hd=%016llx game512=%016llx%s\n",
                mid, ti, ow, oh, w, h, k, gen, (unsigned long long)hh, (unsigned long long)s->h512,
                s->have ? "" : " (no upload seen in the blend)");
}

int hdgpu_uploaded(unsigned mid, unsigned level, const uint8_t *data, size_t n){
    if (!g_t || !mid || mid >= HDGPU_MAXTEX || !data) return 0;
    Slot *s = &g_t[mid];
    if (mid == g_blend_mid){
        if (level) return s->shown;
        g_n_inblend++;
        s->h512 = hdgpu_hash(data, n);
        if (s->nlast != n){ free(s->last); s->last = malloc(n); s->nlast = s->last ? n : 0; }
        s->have = s->last != NULL;
        if (s->last) memcpy(s->last, data, n);
        return s->shown;
    }
    if (!s->shown) return 0;
    if (level) return 1;                              /* below the HD chain: not shown */
    if (s->have && n == s->nlast && hdgpu_hash(data, n) == s->h512){ g_n_same++; return 1; }
    g_n_stale++;
    fprintf(stderr, "[hd-vinyls] gpu t%u: the game uploaded other pixels outside its blend: its own texture again\n", mid);
    slot_hide(mid, data, n);
    if (s->nlast == n && s->last){ memcpy(s->last, data, n); s->h512 = hdgpu_hash(data, n); }
    return 0;
}

void hdgpu_surface_written(unsigned mid){
    if (!g_t || !mid || mid >= HDGPU_MAXTEX || mid == g_blend_mid || !g_t[mid].shown) return;
    g_n_surf++;
    fprintf(stderr, "[hd-vinyls] gpu t%u: written through a surface outside the blend: the game's own texture again\n", mid);
    slot_hide(mid, g_t[mid].have ? g_t[mid].last : NULL, g_t[mid].have ? g_t[mid].nlast : 0);
}

static void gone(unsigned mid, int recreated){
    if (!g_t || !mid || mid >= HDGPU_MAXTEX) return;
    Slot *s = &g_t[mid];
    if (s->shown){
        g_n_rel++;
        if (hdgpu_probe()) fprintf(stderr, "[hd-vinyls] gpu t%u released: HD copy of ti=%08x dropped\n", mid, s->ti);
        slot_hide_as(mid, NULL, 0, recreated);
        hdskin_forget(s->ti);
    }
    if (mid == g_blend_mid) g_blend_mid = 0;
    slot_clear(mid);
}
void hdgpu_created(unsigned mid){ gone(mid, 1); }
void hdgpu_released(unsigned mid){ gone(mid, 0); }

int hdgpu_shown(unsigned mid){ return g_t && mid && mid < HDGPU_MAXTEX && g_t[mid].shown; }

void hdgpu_report(void){
    if (!g_t) return;
    unsigned live = 0;
    for (unsigned i = 1; i < HDGPU_MAXTEX; i++) live += g_t[i].shown;
    fprintf(stderr, "[hd-vinyls] gpu: %lu HD copies shown (%u now); game uploads: %lu in its blend, %lu the same pixels after it"
            " (kept), %lu other pixels (own texture again); %lu surface writes, %lu blends without a copy, %lu released,"
            " %lu moved to a new texture\n",
            g_n_show, live, g_n_inblend, g_n_same, g_n_stale, g_n_surf, g_n_fail, g_n_rel, g_n_moved);
}
