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
 * once after the game's blend; only when they are set. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dispatch_indirect(CPU *c, uint32_t addr);
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

void sub_00611f10(CPU *restrict c){
    const char *tr = ENVC("NFSU2_BLEND_TRACE"), *fx = ENVC("NFSU2_BLEND_FIXTURE");
    if (!tr && !fx){ sub_00611f10_orig(c); return; }

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
    c->eax = r;
}
