/* paintblend.c -- the game's car-paint blend 0x611F10 (see vinylblend.c),
 * with a test-only trace.  Without the variables below it runs the game's
 * own blend and nothing else.
 *
 *   NFSU2_BLEND_TRACE=1      per call: the record's surface, base colour,
 *                            marker colours, layer count, the marker table
 *                            state and the x87 control word; per layer its
 *                            key, texture names, pixel hashes, +0x1C, +0x20
 *                            and colours; the output's hash after
 *   NFSU2_BLEND_FIXTURE=dir  also writes each call's exact inputs and output
 *                            to dir/blend_NNN.bin (the first
 *                            NFSU2_BLEND_FIXTURE_MAX calls, default 4), the
 *                            format tests/vinylblend_unit.c reads
 * Both read the surface by locking it (0x5B96A0/0x5B96D0) once before and
 * once after the game's blend; only when they are set.
 *
 * The HD copy (runtime/hdskin.c), off by default:
 *   NFSU2_HD_VINYLS = off | 1024 | 2048     settings.ini hd_vinyls (2 and 4,
 *                            the scale, are taken too).  After the game's
 *                            blend, a native copy of the surface 2 or 4
 *                            times larger is painted from the record (read
 *                            before the game's in-place swaps) and the game's
 *                            marker table; the game's memory is not written.
 *                            Only where a layer has a remaster: a paint of
 *                            stand-ins alone would be the stock look scaled
 *                            up, so the game's own texture stays.  One log
 *                            line per composite.
 *   TEXPACKS/hdvinyls.pak    in the game folder: the remasters
 *                            (tools/hdvinyls.py), read when a composite needs
 *                            them; none = every texture the stock one
 *   NFSU2_HD_VINYL_DIR=dir   remastered textures, <dir>/<TEXTURE NAME>.dds
 *                            (see hdskin.h), before the pack's (development)
 *   NFSU2_HD_VINYLS_ALL=1    the copy for every paint, stand-ins alone too
 *                            (tests: the stand-in copy and the GPU swap)
 *   NFSU2_HD_PROBE=1         per HD copy shown on the GPU, its hash and (Mac)
 *                            the GPU texture read back (runtime/hdgpu.c)
 * After each composite the copy is shown on the GPU in place of the
 * surface's texture (runtime/hdgpu.h): its id is TextureInfo +0 -> +0x18 (the
 * IDirect3DTexture9 that 0x5B96A0 locks) -> com.c's +28. */
#include "cpu.h"
#include "hdskin.h"
#include "hdgpu.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

void dispatch_indirect(CPU *c, uint32_t addr);
void metal_tex_size(unsigned id, unsigned *w, unsigned *h);   /* metal.m / gpu_web.c */
void sub_00611f10_orig(CPU *c);

#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

#define FN_LOCK      0x005b96a0u   /* thiscall TextureInfo (int) -> pixels */
#define FN_UNLOCK    0x005b96d0u   /* thiscall TextureInfo (int) */
#define G_MARK_FLAG  0x008a1d18u   /* marker scan done */
#define G_MARK_CNT   0x008a1d1cu   /* 7 counts */
#define G_MARK_POS   0x008a05e8u   /* 7 x 16 pixel indices */
#define REC_SIZE     0x1c8u
#define LAYER_SIZE   0x34u

static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

static int ok(uint32_t a, uint32_t n){ return a && n && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

static uint32_t fnv(uint32_t a, uint32_t n){
    if (!ok(a, n)) return 0;
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++){ h ^= g_mem[a + i]; h *= 0x01000193u; }
    return h;
}

static const char *tname(uint32_t ti, char *buf){
    if (!ok(ti, 0x30u)){ strcpy(buf, "-"); return buf; }
    memcpy(buf, g_mem + ti + 12u, 24); buf[24] = 0;
    for (int i = 0; i < 24 && buf[i]; i++) if (buf[i] < 32 || buf[i] > 126) buf[i] = '?';
    return buf;
}

/* the surface's pixels now, through the game's own lock */
static uint32_t read_surface(CPU *c, uint32_t ti, uint32_t n, uint8_t *out){
    uint32_t one = 1, zero = 0;
    uint32_t px = gcall(c, FN_LOCK, ti, 1, &one);
    uint32_t h = 0;
    if (ok(px, n)){ memcpy(out, g_mem + px, n); h = fnv(px, n); }
    gcall(c, FN_UNLOCK, ti, 1, &zero);
    return h;
}

static void put32(FILE *f, uint32_t v){ fwrite(&v, 4, 1, f); }

/* ---- the HD copy ------------------------------------------------------------- */
int fileio_resolve_game(const char *win, char *out, size_t outsz);   /* fileio.c */
static int hd_all(void){ const char *e = ENVC("NFSU2_HD_VINYLS_ALL"); return e && *e && strcmp(e, "0"); }
static int hd_scale(void){
    static int s = -1;
    if (s >= 0) return s;
    const char *e = getenv("NFSU2_HD_VINYLS");
    s = 0;
    if (e && *e){
        if (!strcmp(e, "2") || !strcmp(e, "1024")) s = 2;
        else if (!strcmp(e, "4") || !strcmp(e, "2048")) s = 4;
        else if (strcmp(e, "off") && strcmp(e, "0"))
            fprintf(stderr, "[hd-vinyls] NFSU2_HD_VINYLS=%s ignored: off, 1024 or 2048\n", e);
    }
    if (s){
        char path[1024];
        int np = fileio_resolve_game("TEXPACKS\\hdvinyls.pak", path, sizeof path) ? hdskin_set_pack(path) : 0;
        const char *d = getenv("NFSU2_HD_VINYL_DIR");
        int n = hdskin_set_dir(d);
        fprintf(stderr, "[hd-vinyls] on: %d (scale %d), pack %s (%d textures), remaster directory %s (%d .dds files)%s\n",
                512 * s, s, np ? "TEXPACKS/hdvinyls.pak" : "none", np, d && *d ? d : "none", n,
                hd_all() ? ", every paint" : "");
    }
    return s;
}
static int hd_keep(uint32_t ti, const char *name, void *ctx){
    (void)ctx; char nm[32];
    return ok(ti, 0x50u) && MR8(ti + 0x4au) == 0x20u && !strcmp(tname(ti, nm), name);
}
/* the GPU texture id of a paint surface (com.c's texture object: +4 w, +8 h,
 * +20 D3DFORMAT, +24 levels, +28 Metal id); 0 if it is not the surface's size */
typedef struct { unsigned mid, w, h, levels, fmt; } HdTex;
static HdTex hd_tex(uint32_t ti){
    HdTex t = {0, 0, 0, 0, 0};
    if (!ok(ti, 0x50u)) return t;
    uint32_t p = MR32(ti);
    if (!ok(p, 0x1cu)) return t;
    uint32_t d = MR32(p + 0x18u);
    if (!ok(d, 32u)) return t;
    t.w = MR32(d + 4u); t.h = MR32(d + 8u); t.fmt = MR32(d + 20u); t.levels = MR32(d + 24u);
    if (t.w != MR16(ti + 0x44u) || t.h != MR16(ti + 0x46u) || t.fmt != 21u) return t;
    t.mid = MR32(d + 28u);
    return t;
}
static int hd_composite(const uint8_t *rec);
/* after the game's blend (result r) of rec, the record as it was before it */
static void hd_after(const uint8_t *rec, uint32_t r, HdTex tx){
    uint32_t ti; memcpy(&ti, rec, 4);
    int done = r && hd_composite(rec);
    int w = 0, h = 0; uint32_t gen = 0;
    const uint32_t *px = done ? hdskin_get(ti, &w, &h, &gen) : NULL;
    hdgpu_blend_end(tx.mid, ti, px, w, h, gen, tx.w, tx.h, tx.levels, tx.fmt);
}
/* rec: the record as it was before the game's blend; called after it */
static int hd_composite(const uint8_t *rec){
    int s = hd_scale();
    uint32_t ti; memcpy(&ti, rec, 4);
    if (!s || !ok(ti, 0x50u) || MR8(ti + 0x4au) != 0x20u) return 0;
    double t0; { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); t0 = (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }
    static VbParams P; static VbMarkers M;
    static char nmbuf[VB_MAX_LAYERS][2][32];
    HdTexName names[VB_MAX_LAYERS][2];
    memset(&P, 0, sizeof P); memset(names, 0, sizeof names);
    P.w = (int16_t)MR16(ti + 0x44u); P.h = (int16_t)MR16(ti + 0x46u);
    if (P.w <= 0 || P.h <= 0) return 0;
    uint32_t nb = (uint32_t)P.w * (uint32_t)P.h * 4u;
#define R32(o_) ({ uint32_t v_; memcpy(&v_, rec + (o_), 4); v_; })
    P.base = R32(4);
    for (int k = 0; k < VB_NMARK; k++) P.marker[k] = R32(8 + 4 * k);
    P.nlayers = (int)R32(0x1c4);
    if (P.nlayers > VB_MAX_LAYERS) P.nlayers = VB_MAX_LAYERS;
    int present = 0, hashbad = 0;
    for (int l = 0; l < P.nlayers; l++){
        uint32_t L = 0x24u + (uint32_t)l * LAYER_SIZE;
        VbLayer *Y = &P.layer[l];
        Y->key = R32(L); Y->ncolours = R32(L + 0x20u);
        for (int k = 0; k < 4; k++) Y->colour[k] = R32(L + 0x24u + 4u * (uint32_t)k);
        if (!Y->key) continue;
        uint32_t v = R32(L + 0xcu), m = R32(L + 0x14u);
        if (!ok(v, nb) || !ok(m, nb)){ Y->key = 0; continue; }     /* not readable: left out */
        Y->w = P.w; Y->h = P.h;                                      /* the game reads w*h of each */
        Y->vinyl = (const uint32_t *)(g_mem + v); Y->mask = (const uint32_t *)(g_mem + m);
        for (int t = 0; t < 2; t++){
            uint32_t tx = R32(L + 4u + 4u * (uint32_t)t);
            names[l][t].name = tname(tx, nmbuf[l][t]);
            names[l][t].hash = ok(tx, 0x28u) ? MR32(tx + 0x24u) : 0;
            if (names[l][t].hash && strlen(nmbuf[l][t]) < 23 && hd_name_hash(nmbuf[l][t]) != names[l][t].hash) hashbad++;
        }
        present++;
    }
#undef R32
    for (int k = 0; k < VB_NMARK; k++){
        uint32_t n = MR32(G_MARK_CNT + 4u * (uint32_t)k);
        M.count[k] = n > VB_GAME_SLOTS ? VB_GAME_SLOTS : (int)n;    /* the game's table holds 16 per colour */
        for (int j = 0; j < M.count[k]; j++) M.pos[k][j] = MR32(G_MARK_POS + 4u * (uint32_t)(k * 16 + j));
    }
    char snm[32]; tname(ti, snm);
    hdskin_prune(hd_keep, NULL);
    int any = hd_all();
    for (int l = 0; l < P.nlayers && !any; l++)
        if (P.layer[l].key) for (int t = 0; t < 2; t++)
            if (hdskin_has_remaster(names[l][t].hash ? names[l][t].hash : hd_name_hash(names[l][t].name))) any = 1;
    if (!any){                                       /* the stock look: the game's own texture, no copy kept */
        hdskin_forget(ti);
        fprintf(stderr, "[hd-vinyls] composite surface=%s ti=%08x %dx%d layers=%d remastered=0: skipped, the game's own texture\n",
                snm, ti, P.w, P.h, present);
        return 0;
    }
    HdStats st;
    int r = hdskin_composite(ti, snm, &P, (const HdTexName (*)[2])names, &M, s, &st);
    double t1; { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); t1 = (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }
    size_t ms, mc; hdskin_memory(&ms, &mc);
    if (!r){ fprintf(stderr, "[hd-vinyls] composite surface=%s ti=%08x %dx%d x%d: FAILED (size or memory)\n", snm, ti, P.w, P.h, s); return 0; }
    fprintf(stderr, "[hd-vinyls] composite surface=%s ti=%08x %dx%d -> %dx%d layers=%d remastered=%d stand-ins=%d cached=%d"
            " marker_texels=%d gen=%u ms=%.2f (read %.2f, sources %.2f, blend %.2f) memory: surfaces %zu MB, sources %zu MB%s\n",
            snm, ti, P.w, P.h, P.w * s, P.h * s, present, st.remastered, st.standin, st.cached, st.markers, st.generation,
            t1 - t0, t1 - t0 - st.ms_sources - st.ms_blend, st.ms_sources, st.ms_blend, ms >> 20, mc >> 20,
            hashbad ? " NAME-HASH MISMATCH" : "");
    return 1;
}

void sub_00611f10(CPU *restrict c){
    const char *tr = ENVC("NFSU2_BLEND_TRACE"), *fx = ENVC("NFSU2_BLEND_FIXTURE");
    if (!tr && !fx){
        if (!hd_scale()){
            uint32_t P0 = MR32(c->esp + 4u);
            sub_00611f10_orig(c);
            if (hdgpu_probe() && ok(P0, 4u)){            /* HD off: what the GPU has for the surface */
                uint32_t ti = MR32(P0); HdTex tx = hd_tex(ti); unsigned gw = 0, gh = 0;
                if (tx.mid) metal_tex_size(tx.mid, &gw, &gh);
                fprintf(stderr, "[hd-vinyls] probe (HD off) t%u ti=%08x game %ux%u, GPU texture %ux%u, HD shown %d\n",
                        tx.mid, ti, tx.w, tx.h, gw, gh, hdgpu_shown(tx.mid));
            }
            return;
        }
        uint32_t P = MR32(c->esp + 4u);
        if (!ok(P, REC_SIZE)){ sub_00611f10_orig(c); return; }
        uint8_t rec[REC_SIZE]; memcpy(rec, g_mem + P, REC_SIZE);
        HdTex tx = hd_tex(MR32(P));
        hdgpu_blend_begin(tx.mid);
        sub_00611f10_orig(c);
        uint32_t r = c->eax;
        hd_after(rec, r, tx);
        c->eax = r;
        return;
    }

    static unsigned calls, dumps;
    unsigned id = calls++;
    uint32_t ret = MR32(c->esp), P = MR32(c->esp + 4u);
    if (!ok(P, REC_SIZE)){ sub_00611f10_orig(c); return; }
    uint8_t rec[REC_SIZE]; memcpy(rec, g_mem + P, REC_SIZE);
    uint32_t ti = MR32(P), base = MR32(P + 4u), nl = MR32(P + 0x1c4u);
    uint32_t w = 0, h = 0, bpp = 0;
    if (ok(ti, 0x50u)){ w = (uint32_t)(int32_t)(int16_t)MR16(ti + 0x44u); h = (uint32_t)(int32_t)(int16_t)MR16(ti + 0x46u); bpp = MR8(ti + 0x4au); }
    uint32_t npx = w * h, nb = npx * 4u;
    int good = ti && bpp == 0x20u && npx && npx <= 4096u * 4096u;
    uint32_t flag0 = MR32(G_MARK_FLAG), cnt0[7], pos0[112];
    for (int k = 0; k < 7; k++) cnt0[k] = MR32(G_MARK_CNT + 4u * k);
    for (int k = 0; k < 112; k++) pos0[k] = MR32(G_MARK_POS + 4u * k);
    char nm[32], nm2[32];

    if (tr){
        fprintf(stderr, "[blend-trace] call=%u ret=%08x rec=%08x surface=%08x name=%s size=%ux%u bpp=%u base=%08x layers=%u"
                " scanned=%u fcw=%04x counts=%u,%u,%u,%u,%u,%u,%u\n",
                id, ret, P, ti, tname(ti, nm), w, h, bpp, base, nl, flag0, c->fcw,
                cnt0[0], cnt0[1], cnt0[2], cnt0[3], cnt0[4], cnt0[5], cnt0[6]);
        fprintf(stderr, "[blend-trace] call=%u markers=%08x,%08x,%08x,%08x,%08x,%08x,%08x\n", id,
                MR32(P + 8u), MR32(P + 12u), MR32(P + 16u), MR32(P + 20u), MR32(P + 24u), MR32(P + 28u), MR32(P + 32u));
        for (uint32_t l = 0; l < 8u; l++){
            uint32_t L = P + 0x24u + l * LAYER_SIZE;
            if (l >= nl && !MR32(L)) continue;
            fprintf(stderr, "[blend-trace] call=%u layer=%u%s key=%08x vinyl_ti=%08x(%s) mask_ti=%08x(%s) vinyl=%08x hash=%08x"
                    " mask=%08x hash=%08x +10=%08x +18=%08x +1c=%08x ncol=%08x colours=%08x,%08x,%08x,%08x\n",
                    id, l, l < nl ? "" : "(past count)", MR32(L), MR32(L + 4u), tname(MR32(L + 4u), nm), MR32(L + 8u), tname(MR32(L + 8u), nm2),
                    MR32(L + 0xcu), good && MR32(L) ? fnv(MR32(L + 0xcu), nb) : 0u,
                    MR32(L + 0x14u), good && MR32(L) ? fnv(MR32(L + 0x14u), nb) : 0u,
                    MR32(L + 0x10u), MR32(L + 0x18u), MR32(L + 0x1cu), MR32(L + 0x20u),
                    MR32(L + 0x24u), MR32(L + 0x28u), MR32(L + 0x2cu), MR32(L + 0x30u));
        }
    }

    unsigned maxd = 4; { const char *e = ENVC("NFSU2_BLEND_FIXTURE_MAX"); if (e) maxd = (unsigned)atoi(e); }
    FILE *f = NULL; uint8_t *buf = NULL;
    if (fx && good && dumps < maxd && nl <= 8u && (buf = malloc(nb))){
        char path[1024]; snprintf(path, sizeof path, "%s/blend_%03u.bin", fx, id);
        f = fopen(path, "wb");
        if (f){
            dumps++;
            fwrite("VBFX0001", 8, 1, f);
            put32(f, w); put32(f, h); put32(f, c->fcw); put32(f, ret);
            fwrite(rec, REC_SIZE, 1, f);
            put32(f, flag0); fwrite(cnt0, 4, 7, f); fwrite(pos0, 4, 112, f);
            uint32_t hb = read_surface(c, ti, nb, buf);
            fwrite(buf, nb, 1, f);
            for (uint32_t l = 0; l < nl; l++){
                uint32_t L = P + 0x24u + l * LAYER_SIZE;
                if (!MR32(L)) continue;
                uint32_t v = MR32(L + 0xcu), m = MR32(L + 0x14u);
                if (ok(v, nb)) fwrite(g_mem + v, nb, 1, f); else { memset(buf, 0, nb); fwrite(buf, nb, 1, f); }
                if (ok(m, nb)) fwrite(g_mem + m, nb, 1, f); else { memset(buf, 0, nb); fwrite(buf, nb, 1, f); }
            }
            if (tr) fprintf(stderr, "[blend-trace] call=%u fixture=%s before=%08x\n", id, path, hb);
        }
    }

    HdTex tx = hd_scale() ? hd_tex(ti) : (HdTex){0, 0, 0, 0, 0};
    if (hd_scale()) hdgpu_blend_begin(tx.mid);
    sub_00611f10_orig(c);
    uint32_t r = c->eax;

    if (good && (tr || f)){
        uint8_t *out = buf ? buf : malloc(nb);
        uint32_t ha = out ? read_surface(c, ti, nb, out) : 0;
        if (f){
            put32(f, MR32(G_MARK_FLAG));
            for (int k = 0; k < 7; k++) put32(f, MR32(G_MARK_CNT + 4u * k));
            for (int k = 0; k < 112; k++) put32(f, MR32(G_MARK_POS + 4u * k));
            fwrite(out, nb, 1, f);
            fclose(f); f = NULL;
        }
        if (tr) fprintf(stderr, "[blend-trace] call=%u result=%u output=%08x scanned=%u counts=%u,%u,%u,%u,%u,%u,%u\n",
                        id, r, ha, MR32(G_MARK_FLAG), MR32(G_MARK_CNT), MR32(G_MARK_CNT + 4u), MR32(G_MARK_CNT + 8u),
                        MR32(G_MARK_CNT + 12u), MR32(G_MARK_CNT + 16u), MR32(G_MARK_CNT + 20u), MR32(G_MARK_CNT + 24u));
        if (out != buf) free(out);
    } else if (tr) fprintf(stderr, "[blend-trace] call=%u result=%u (not a 32-bit surface)\n", id, r);
    if (f) fclose(f);
    free(buf);
    if (hd_scale()) hd_after(rec, r, tx);
    c->eax = r;
}
