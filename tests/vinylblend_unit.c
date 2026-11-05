/* vinylblend_unit.c -- runtime/vinylblend.c (the game's car-paint blend in
 * plain C) against the game's own blend, byte for byte.
 *
 *   clang -O2 -Wall -ffp-contract=off -Iruntime -o out/vinylblend_unit tests/vinylblend_unit.c runtime/vinylblend.c
 *   out/vinylblend_unit fixture.bin [...]
 *
 * A fixture is one call of the game's blend 0x611F10 as runtime/paintblend.c
 * writes it (NFSU2_BLEND_FIXTURE=dir): the record, the marker table before
 * and after, the surface before, each layer's vinyl and mask pixels and the
 * surface after.  Cases:
 *   mix        0x611B00 on hand-made inputs: a weight of 1/255 counts as 0,
 *              sums clamp at 255, alpha largest or summed
 *   scan       (a fixture of the run's first call, before the game's scan)
 *              our scan of the surface before finds the table the game made
 *   blend      our blend of the fixture's inputs is the game's output, every
 *              byte; otherwise the mismatches are counted and the first shown
 * Every case prints "PASS <case> ..." or "FAIL <case> ..."; exit 1 on a FAIL. */
#include "vinylblend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { g_fail = 1; printf("FAIL " __VA_ARGS__); } }while(0)

static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }

static void test_mix(void){
    const float k = 0x1.010102p-8f;               /* the game's 1/255 */
    uint32_t c3[3] = { 0xff102030u, 0x80ffffffu, 0x00000000u };
    float w1[3] = { 1.0f * k, 0.0f, 0.0f };       /* vinyl channel 1: skipped */
    float w2[3] = { 255.0f * k, 255.0f * k, 0.0f };
    float w3[3] = { 128.0f * k, 0.0f, 0.0f };
    uint32_t r1 = vb_mix(c3, w1, 3, 1), r2 = vb_mix(c3, w2, 3, 1), r3 = vb_mix(c3, w3, 3, 1);
    uint32_t c2[2] = { 0xff0000ffu, 0x80ff0000u };
    float a = 64.0f * 0.0078125f, w4[2] = { a, 1.0f - a };
    uint32_t r4 = vb_mix(c2, w4, 2, 0);
    /* r3: 0x10*0.50196, 0x20*.., 0x30*.., 0xff*..: 8, 16, 24, 128 (truncated) */
    CHECK(r1 == 0u && r2 == 0xffffffffu && r3 == 0x80081018u && r4 == 0xbf7f007fu,
          "mix skip=%08x clamp=%08x half=%08x pair=%08x\n", r1, r2, r3, r4);
}

static int run_fixture(const char *path){
    FILE *f = fopen(path, "rb");
    if (!f){ printf("FAIL blend %s: cannot open\n", path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz){ fclose(f); printf("FAIL blend %s: read\n", path); return 1; }
    fclose(f);
    if (sz < 8 + 16 + 0x1c8 + 480 || memcmp(d, "VBFX0001", 8)){ printf("FAIL blend %s: not a fixture\n", path); return 1; }
    const uint8_t *p = d + 8;
    uint32_t w = rd32(p), h = rd32(p + 4), fcw = rd32(p + 8), ret = rd32(p + 12); p += 16;
    const uint8_t *rec = p; p += 0x1c8;
    uint32_t flag0 = rd32(p); const uint8_t *cnt0 = p + 4, *pos0 = p + 32; p += 4 + 28 + 448;
    size_t nb = (size_t)w * h * 4u;
    const uint8_t *end = d + sz;
#define NEED(n_) do{ if ((size_t)(end - p) < (size_t)(n_)){ printf("FAIL blend %s: truncated\n", path); free(d); return 1; } }while(0)
    NEED(nb);
    const uint32_t *before = (const uint32_t *)p; p += nb;

    static VbParams P; static VbMarkers M;
    memset(&P, 0, sizeof P); memset(&M, 0, sizeof M);
    P.w = (int)w; P.h = (int)h;
    P.base = rd32(rec + 4);
    for (int k = 0; k < VB_NMARK; k++) P.marker[k] = rd32(rec + 8 + 4 * k);
    P.nlayers = (int)rd32(rec + 0x1c4);
    int present = 0;
    for (int l = 0; l < P.nlayers && l < VB_MAX_LAYERS; l++){
        const uint8_t *L = rec + 0x24 + 0x34 * l;
        VbLayer *Y = &P.layer[l];
        Y->key = rd32(L);
        Y->ncolours = rd32(L + 0x20);
        for (int k = 0; k < 4; k++) Y->colour[k] = rd32(L + 0x24 + 4 * k);
        if (!Y->key) continue;
        Y->w = (int)w; Y->h = (int)h;
        NEED(2 * nb);
        Y->vinyl = (const uint32_t *)p; p += nb;
        Y->mask = (const uint32_t *)p; p += nb;
        present++;
    }
    NEED(4 + 28 + 448 + nb);
    uint32_t flag1 = rd32(p); const uint8_t *cnt1 = p + 4, *pos1 = p + 32; p += 4 + 28 + 448;
    const uint32_t *out = (const uint32_t *)p; p += nb;
    if ((long)(p - d) != sz){ printf("FAIL blend %s: size %ld, expected %ld\n", path, sz, (long)(p - d)); return 1; }
    printf("     %s: %ux%u, fcw %04x, caller %08x, %d layers (%d present), scanned before %u after %u\n",
           path, w, h, fcw, ret, P.nlayers, present, flag0, flag1);

    if (!flag0){
        vb_marker_scan(before, (int)(w * h), &M);
        int same = 1;
        for (int k = 0; k < VB_NMARK; k++){
            if ((uint32_t)M.count[k] != rd32(cnt1 + 4 * k)) same = 0;
            for (int j = 0; j < M.count[k] && j < VB_GAME_SLOTS; j++)
                if (M.pos[k][j] != rd32(pos1 + 4 * (k * 16 + j))) same = 0;
        }
        CHECK(same, "scan %s: counts %d,%d,%d,%d,%d,%d,%d\n", path,
              M.count[0], M.count[1], M.count[2], M.count[3], M.count[4], M.count[5], M.count[6]);
    } else {
        /* the game's table as it is: flat, 16 per colour */
        for (int k = 0; k < VB_NMARK; k++){
            int n = (int)rd32(cnt0 + 4 * k);
            if (n > VB_GAME_SLOTS){ printf("FAIL blend %s: %d markers of colour %d, more than the game's table holds\n", path, n, k); free(d); return 1; }
            M.count[k] = n;
            for (int j = 0; j < n; j++) M.pos[k][j] = rd32(pos0 + 4 * (k * 16 + j));
        }
    }

    uint32_t *mine = malloc(nb);
    vb_blend32(mine, &P, &M);
    size_t bad = 0, badbytes = 0;
    for (size_t i = 0; i < (size_t)w * h; i++){
        if (mine[i] == out[i]) continue;
        for (int b = 0; b < 4; b++) if (((mine[i] ^ out[i]) >> (8 * b)) & 0xffu) badbytes++;
        if (bad < 12){
            printf("     differs at (%zu,%zu): ours %08x game %08x", i % w, i / w, mine[i], out[i]);
            for (int l = 0; l < P.nlayers; l++) if (P.layer[l].key)
                printf("  L%d v=%08x m=%08x", l, P.layer[l].vinyl[i], P.layer[l].mask[i]);
            printf("\n");
        }
        bad++;
    }
#undef NEED
    CHECK(bad == 0, "blend %s: %zu of %u pixels differ (%zu bytes)\n", path, bad, w * h, badbytes);
    free(mine); free(d);
    return bad != 0;
}

int main(int argc, char **argv){
    test_mix();
    if (argc < 2) printf("     no fixture given: blend not checked\n");
    for (int i = 1; i < argc; i++) if (run_fixture(argv[i])) g_fail = 1;
    return g_fail;
}
