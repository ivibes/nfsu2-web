/* hdskin_unit.c -- runtime/hdskin.c (the HD copy of a car paint surface)
 * against the game's own 512 blend, byte for byte.
 *
 *   clang -O2 -Wall -ffp-contract=off -Iruntime -o out/hdskin_unit tests/hdskin_unit.c runtime/hdskin.c runtime/vinylblend.c
 *   out/hdskin_unit <scratch dir> fixture.bin [...]
 *   out/hdskin_unit                (the pack's round trip alone, in out/hdskin_unit.tmp)
 * from the repo's root (the pack cases run python3 tools/hdvinyls.py;
 * HDSKIN_TOOL names another).
 *
 * Fixtures as for tests/vinylblend_unit.c (runtime/paintblend.c,
 * NFSU2_BLEND_FIXTURE=dir).  The scratch dir gets the fake remasters.  Cases:
 *   nearest    per fixture, s = 2 and 4, no remasters: every s x s block of
 *              the HD copy is the game's output pixel (layers and markers);
 *              run twice, the second time from the cached stand-ins
 *   remaster   (the fixture with a coloured vinyl: two layers, layer 0
 *              colourised) a fake remaster of layer 0's vinyl, found by its
 *              name (file name in lower case), different from the stand-in
 *              inside every block: the copy equals vb_blend32 on those
 *              sources, differs from the stand-in copy somewhere, and only
 *              where layer 0's stock mask covers; a .dds of another name and
 *              a remaster of a size that does not fit change nothing
 *   pack       the remaster pack (tools/hdvinyls.py): DDS files of each
 *              format the runtime reads (A8R8G8B8, A8B8G8R8, X8R8G8B8, two
 *              with the same pixels) and one it does not, packed by the tool
 *              and read back through the pack, pixel for pixel the DDS file
 *              as hd_load_dds reads it; the fake remaster of the remaster
 *              case from a pack paints the same copy as from the directory,
 *              the directory's file comes before the pack's, and a pack whose
 *              pixels were damaged is refused (CRC-32) for the stand-in
 *   api        hdskin_get, hdskin_forget, generations
 * Every case prints "PASS <case> ..." or "FAIL <case> ..."; exit 1 on a FAIL. */
#include "hdskin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { g_fail = 1; printf("FAIL " __VA_ARGS__); } }while(0)

static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }

typedef struct {
    uint8_t *data;
    VbParams P; VbMarkers M;
    const uint32_t *out;          /* the game's 512 output */
    int present;
} Fx;

static int load(const char *path, Fx *x){
    memset(x, 0, sizeof *x);
    FILE *f = fopen(path, "rb");
    if (!f){ printf("FAIL load %s: cannot open\n", path); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz){ fclose(f); printf("FAIL load %s: read\n", path); return 0; }
    fclose(f);
    if (sz < 8 + 16 + 0x1c8 + 480 || memcmp(d, "VBFX0001", 8)){ printf("FAIL load %s: not a fixture\n", path); free(d); return 0; }
    const uint8_t *p = d + 8, *end = d + sz;
    uint32_t w = rd32(p), h = rd32(p + 4); p += 16;
    const uint8_t *rec = p; p += 0x1c8;
    uint32_t flag0 = rd32(p); const uint8_t *cnt0 = p + 4, *pos0 = p + 32; p += 4 + 28 + 448;
    size_t nb = (size_t)w * h * 4u;
#define NEED(n_) do{ if ((size_t)(end - p) < (size_t)(n_)){ printf("FAIL load %s: truncated\n", path); free(d); return 0; } }while(0)
    NEED(nb);
    const uint32_t *before = (const uint32_t *)p; p += nb;
    VbParams *P = &x->P;
    P->w = (int)w; P->h = (int)h; P->base = rd32(rec + 4);
    for (int k = 0; k < VB_NMARK; k++) P->marker[k] = rd32(rec + 8 + 4 * k);
    P->nlayers = (int)rd32(rec + 0x1c4);
    for (int l = 0; l < P->nlayers && l < VB_MAX_LAYERS; l++){
        const uint8_t *L = rec + 0x24 + 0x34 * l;
        VbLayer *Y = &P->layer[l];
        Y->key = rd32(L); Y->ncolours = rd32(L + 0x20);
        for (int k = 0; k < 4; k++) Y->colour[k] = rd32(L + 0x24 + 4 * k);
        if (!Y->key) continue;
        Y->w = (int)w; Y->h = (int)h;
        NEED(2 * nb);
        Y->vinyl = (const uint32_t *)p; p += nb;
        Y->mask = (const uint32_t *)p; p += nb;
        x->present++;
    }
    NEED(4 + 28 + 448 + nb);
    const uint8_t *cnt1 = p + 4, *pos1 = p + 32; p += 4 + 28 + 448;
    x->out = (const uint32_t *)p; p += nb;
#undef NEED
    if ((long)(p - d) != sz){ printf("FAIL load %s: size\n", path); free(d); return 0; }
    /* the game's table after its blend (the hook reads it there) */
    const uint8_t *cnt = flag0 ? cnt0 : cnt1, *pos = flag0 ? pos0 : pos1;
    (void)before;
    for (int k = 0; k < VB_NMARK; k++){
        int n = (int)rd32(cnt + 4 * k);
        if (n > VB_GAME_SLOTS) n = VB_GAME_SLOTS;
        x->M.count[k] = n;
        for (int j = 0; j < n; j++) x->M.pos[k][j] = rd32(pos + 4 * (k * 16 + j));
    }
    x->data = d;
    return 1;
}

static void names_for(const Fx *x, HdTexName nm[VB_MAX_LAYERS][2], const char *const *given){
    static char buf[VB_MAX_LAYERS][2][32];
    memset(nm, 0, sizeof(HdTexName) * VB_MAX_LAYERS * 2);
    for (int l = 0; l < x->P.nlayers && l < VB_MAX_LAYERS; l++){
        if (given && given[2 * l]){ nm[l][0].name = given[2 * l]; nm[l][1].name = given[2 * l + 1]; continue; }
        snprintf(buf[l][0], 32, "TEST_L%d_%08X", l, x->P.layer[l].key);
        snprintf(buf[l][1], 32, "TEST_L%d_%08X_MASK", l, x->P.layer[l].key);
        nm[l][0].name = buf[l][0]; nm[l][1].name = buf[l][1];
    }
}

/* blocks of hd (s times w x h) against ref (w x h); returns mismatching pixels */
static size_t blocks_vs(const uint32_t *hd, const uint32_t *ref, int w, int h, int s, int show){
    size_t bad = 0; const size_t W = (size_t)w * s;
    for (int y = 0; y < h * s; y++) for (int x = 0; x < w * s; x++){
        uint32_t a = hd[(size_t)y * W + x], b = ref[(size_t)(y / s) * w + x / s];
        if (a == b) continue;
        if (bad < (size_t)show) printf("     differs at HD (%d,%d) = stock (%d,%d): ours %08x game %08x\n", x, y, x / s, y / s, a, b);
        bad++;
    }
    return bad;
}

static void test_nearest(const char *path, const Fx *x, uint32_t key){
    HdTexName nm[VB_MAX_LAYERS][2]; names_for(x, nm, NULL);
    for (int s = 2; s <= 4; s += 2)
        for (int pass = 0; pass < 2; pass++){
            HdStats st;
            int r = hdskin_composite(key, "TEST", &x->P, (const HdTexName (*)[2])nm, &x->M, s, &st);
            int w = 0, h = 0; uint32_t g = 0;
            const uint32_t *hd = hdskin_get(key, &w, &h, &g);
            size_t bad = r && hd && w == x->P.w * s && h == x->P.h * s ? blocks_vs(hd, x->out, x->P.w, x->P.h, s, 6) : (size_t)-1;
            int mk = 0; for (int k = 0; k < VB_NMARK; k++) mk += x->M.count[k];
            CHECK(bad == 0 && st.standin == 2 * x->present && st.remastered == 0 && st.cached == (pass ? st.standin : 0)
                  && st.markers == mk * s * s && g == st.generation,
                  "nearest %s s=%d %s: %dx%d, %zu pixels differ, %d stand-ins (%d cached), %d marker texels, %.1f ms sources + %.1f ms blend\n",
                  path, s, pass ? "cached" : "fresh", w, h, bad, st.standin, st.cached, st.markers, st.ms_sources, st.ms_blend);
        }
}

/* kind 0: A8R8G8B8, 1: A8B8G8R8 (px written with red and blue swapped), 2: X8R8G8B8, 3: DXT1 (refused) */
static int write_dds_as(const char *path, const uint32_t *px, int w, int h, int kind){
    uint8_t hd[128]; memset(hd, 0, sizeof hd);
    uint32_t v[32]; memset(v, 0, sizeof v);
    memcpy(hd, "DDS ", 4);
    v[1] = 124; v[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8; v[3] = (uint32_t)h; v[4] = (uint32_t)w; v[5] = (uint32_t)w * 4u;
    v[19] = 32; v[20] = 0x40 | 0x1; v[22] = 32; v[23] = 0x00ff0000u; v[24] = 0x0000ff00u; v[25] = 0x000000ffu; v[26] = 0xff000000u;
    if (kind == 1){ v[23] = 0x000000ffu; v[25] = 0x00ff0000u; }
    if (kind == 2){ v[20] = 0x40; v[26] = 0; }
    if (kind == 3){ v[20] = 0x4; memcpy(&v[21], "DXT1", 4); }
    v[27] = 0x1000;
    memcpy(hd + 4, v + 1, 124);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(hd, 1, 128, f) == 128;
    for (size_t i = 0; ok && i < (size_t)w * h; i++){
        uint32_t q = kind == 1 ? vb_swap_rb(px[i]) : px[i];
        ok = fwrite(&q, 4, 1, f) == 1;
    }
    return fclose(f) == 0 && ok;
}
static int write_dds(const char *path, const uint32_t *px, int w, int h){ return write_dds_as(path, px, w, h, 0); }

/* the pack tool on a folder; 1 if it wrote the pack */
static int run_tool(const char *src, const char *pak){
    const char *tool = getenv("HDSKIN_TOOL");
    char cmd[3200];
    snprintf(cmd, sizeof cmd, "python3 '%s' --out '%s' '%s' > '%s.log' 2>&1", tool && *tool ? tool : "tools/hdvinyls.py", pak, src, pak);
    remove(pak);
    int r = system(cmd);
    FILE *f = fopen(pak, "rb");
    if (f) fclose(f);
    if (r || !f) printf("     %s: exit %d, see %s.log\n", cmd, r, pak);
    return !r && f;
}
static uint32_t g_seed = 12345u;
static uint32_t rnd(void){ g_seed = g_seed * 1103515245u + 12345u; return g_seed ^ (g_seed >> 15); }

static void test_remaster(const char *dir, const char *path, const Fx *x){
    const int s = 2, w = x->P.w, h = x->P.h, W = w * s, H = h * s;
    const VbLayer *L0 = &x->P.layer[0];
    /* the fake: the stand-in with the colour weights changed on every
     * texel but each block's top-left one (so its blocks are no longer flat) */
    uint32_t *fake = malloc((size_t)W * H * 4u);
    hd_scale_nearest(L0->vinyl, w, h, s, fake);
    for (int y = 0; y < H; y++) for (int xx = 0; xx < W; xx++)
        if ((xx % s) || (y % s)) fake[(size_t)y * W + xx] ^= 0x00c0c0c0u;
    char p[1024];
    snprintf(p, sizeof p, "%s/skyline_tear_012.dds", dir);                 /* lower case on purpose */
    int wrote = write_dds(p, fake, W, H);
    snprintf(p, sizeof p, "%s/SOMETHING_ELSE.dds", dir);                    /* a name no layer has */
    wrote = wrote && write_dds(p, fake, W, H);
    uint32_t *odd = calloc((size_t)(W - 8) * (H - 8), 4);                   /* a size that does not fit */
    snprintf(p, sizeof p, "%s/SKYLINE_DOORLINE_MASK.dds", dir);
    wrote = wrote && write_dds(p, odd, W - 8, H - 8);
    free(odd);
    if (!wrote){ printf("FAIL remaster: cannot write fakes into %s\n", dir); g_fail = 1; free(fake); return; }

    static const char *const given[4] = { "SKYLINE_TEAR_012", "SKYLINE_TEAR_012_MASK", "SKYLINE_DOORLINE", "SKYLINE_DOORLINE_MASK" };
    HdTexName nm[VB_MAX_LAYERS][2]; names_for(x, nm, given);
    int n = hdskin_set_dir(dir);
    HdStats st;
    int r = hdskin_composite(0xabc0u, "TEST", &x->P, (const HdTexName (*)[2])nm, &x->M, s, &st);
    const uint32_t *hd = hdskin_get(0xabc0u, NULL, NULL, NULL);

    /* the reference: vb_blend32 on the same sources built here */
    static VbParams R; static VbMarkers RM;
    R = x->P; R.w = W; R.h = H;
    uint32_t *bufs[2 * VB_MAX_LAYERS] = { 0 };
    for (int l = 0; l < R.nlayers; l++){
        if (!R.layer[l].key) continue;
        bufs[2 * l] = malloc((size_t)W * H * 4u); bufs[2 * l + 1] = malloc((size_t)W * H * 4u);
        hd_scale_nearest(x->P.layer[l].vinyl, w, h, s, bufs[2 * l]);
        hd_scale_nearest(x->P.layer[l].mask, w, h, s, bufs[2 * l + 1]);
        if (l == 0) memcpy(bufs[0], fake, (size_t)W * H * 4u);
        R.layer[l].vinyl = bufs[2 * l]; R.layer[l].mask = bufs[2 * l + 1]; R.layer[l].w = W; R.layer[l].h = H;
    }
    hd_markers_scale(&x->M, w, h, s, &RM);
    uint32_t *ref = malloc((size_t)W * H * 4u);
    vb_blend32(ref, &R, &RM);
    size_t vsref = 0, changed = 0, outside = 0;
    for (int y = 0; y < H; y++) for (int xx = 0; xx < W; xx++){
        size_t i = (size_t)y * W + xx, si = (size_t)(y / s) * w + xx / s;
        uint32_t a = hd ? hd[i] : 0;
        if (a != ref[i]) vsref++;
        if (a != x->out[si]){
            changed++;
            if (!((L0->mask[si] >> 8) & 0xffu)) outside++;
        }
    }
    CHECK(r && hd && n == 3 && st.remastered == 1 && st.standin == 3 && vsref == 0 && changed > 0 && outside == 0,
          "remaster %s s=%d: %d files indexed, %d remastered, %d stand-ins; %zu pixels differ from vb_blend32 on the same sources,"
          " %zu from the stock look, %zu of those outside layer 0's mask\n",
          path, s, n, st.remastered, st.standin, vsref, changed, outside);
    hdskin_set_dir(NULL);
    r = hdskin_composite(0xabc0u, "TEST", &x->P, (const HdTexName (*)[2])nm, &x->M, s, &st);
    hd = hdskin_get(0xabc0u, NULL, NULL, NULL);
    size_t back = r && hd ? blocks_vs(hd, x->out, w, h, s, 3) : (size_t)-1;
    CHECK(back == 0 && st.remastered == 0, "remaster %s off again: %zu pixels differ from the stock look\n", path, back);
    for (int i = 0; i < 2 * VB_MAX_LAYERS; i++) free(bufs[i]);
    free(ref); free(fake);
}

/* the remaster pack: the tool's pack, read back, equals the DDS files */
static void test_pack_roundtrip(const char *dir){
    char src[1024], pak[1100], p[1200];
    snprintf(src, sizeof src, "%s/pack-src", dir);
    snprintf(p, sizeof p, "mkdir -p '%s' && rm -f '%s'/*.dds", src, src);
    if (system(p)){ printf("FAIL pack: cannot make %s\n", src); g_fail = 1; return; }
    /* 1. round trip: one file of each format, two with the same pixels, one refused */
    static const char *const nm[5] = { "PACK_ARGB", "pack_abgr", "PACK_XRGB", "PACK_SAME", "PACK_DXT1" };
    static const int kind[5] = { 0, 1, 2, 0, 3 }, side[5] = { 1024, 256, 2048, 1024, 64 };
    uint32_t *px[5] = { 0 };
    int wrote = 1;
    for (int i = 0; i < 5; i++){
        size_t n = (size_t)side[i] * side[i];
        px[i] = malloc(n * 4u);
        for (size_t j = 0; j < n; j++) px[i][j] = i == 3 ? px[0][j] : (j % 7 == 0 ? 0 : rnd());   /* zeros, as a vinyl's clear */
        snprintf(p, sizeof p, "%s/%s.dds", src, nm[i]);
        wrote = wrote && write_dds_as(p, px[i], side[i], side[i], kind[i]);
    }
    snprintf(pak, sizeof pak, "%s/test.pak", dir);
    int built = wrote && run_tool(src, pak);
    int n = built ? hdskin_set_pack(pak) : 0;
    size_t differ = 0; int read = 0, refused = 0;
    for (int i = 0; i < 5; i++){
        char upper[32]; size_t k = 0;
        for (; nm[i][k]; k++) upper[k] = (char)(nm[i][k] >= 'a' && nm[i][k] <= 'z' ? nm[i][k] - 32 : nm[i][k]);
        upper[k] = 0;
        int pw = 0, ph = 0, dw = 0, dh = 0; char why[160] = "";
        uint32_t *a = hdskin_pack_pixels(hd_name_hash(upper), &pw, &ph, why, sizeof why);
        snprintf(p, sizeof p, "%s/%s.dds", src, nm[i]);
        uint32_t *b = hd_load_dds(p, &dw, &dh, NULL, 0);
        if (kind[i] == 3){ refused += !a && !b && !hdskin_has_remaster(hd_name_hash(upper)); free(a); free(b); continue; }
        if (a && b && pw == dw && ph == dh && pw == side[i]){
            read++;
            for (size_t j = 0; j < (size_t)pw * ph; j++) differ += a[j] != b[j];
        } else differ++;
        free(a); free(b);
    }
    long psize = 0; { FILE *f = fopen(pak, "rb"); if (f){ fseek(f, 0, SEEK_END); psize = ftell(f); fclose(f); } }
    CHECK(built && n == 4 && read == 4 && differ == 0 && refused == 1 && psize > 0 && psize < (long)(2048 * 2048 * 4 + 1024 * 1024 * 4 + 256 * 256 * 4 + (1 << 20)),   /* PACK_SAME shares */
          "pack: round trip: tool %s, %d textures in the pack (%ld bytes), %d read back, %zu pixels differ from the DDS files,"
          " %d refused by the tool\n", built ? "ran" : "FAILED", n, psize, read, differ, refused);
    for (int i = 0; i < 5; i++) free(px[i]);
    hdskin_set_pack(NULL);
}

/* the remaster case's copy from a pack (the files test_remaster wrote): as from the directory */
static void test_pack(const char *dir, const char *path, const Fx *x){
    char src[1024], pak[1100], p[1200];
    snprintf(src, sizeof src, "%s/pack-src", dir);
    snprintf(pak, sizeof pak, "%s/test.pak", dir);
    snprintf(p, sizeof p, "mkdir -p '%s'", src);
    if (system(p)){ printf("FAIL pack: cannot make %s\n", src); g_fail = 1; return; }
    /* 2. the remaster case from the pack: the same copy as from the directory */
    const int s = 2;
    static const char *const given[4] = { "SKYLINE_TEAR_012", "SKYLINE_TEAR_012_MASK", "SKYLINE_DOORLINE", "SKYLINE_DOORLINE_MASK" };
    HdTexName names[VB_MAX_LAYERS][2]; names_for(x, names, given);
    hdskin_set_pack(NULL);
    hdskin_set_dir(dir);
    HdStats st;
    int r1 = hdskin_composite(0xdd0u, "TEST", &x->P, (const HdTexName (*)[2])names, &x->M, s, &st);
    const uint32_t *hd = hdskin_get(0xdd0u, NULL, NULL, NULL);
    const size_t N = (size_t)x->P.w * s * x->P.h * s;
    uint32_t *fromdir = malloc(N * 4u);
    if (r1 && hd) memcpy(fromdir, hd, N * 4u);
    int rem_dir = st.remastered;
    snprintf(p, sizeof p, "rm -f '%s'/*.dds && cp '%s/skyline_tear_012.dds' '%s/'", src, dir, src);
    int cp = !system(p) && run_tool(src, pak);
    hdskin_set_dir(NULL);
    int n2 = cp ? hdskin_set_pack(pak) : 0;
    int has = hdskin_has_remaster(hd_name_hash("SKYLINE_TEAR_012")), hasnot = hdskin_has_remaster(hd_name_hash("SKYLINE_DOORLINE"));
    int r2 = hdskin_composite(0xdd1u, "TEST", &x->P, (const HdTexName (*)[2])names, &x->M, s, &st);
    hd = hdskin_get(0xdd1u, NULL, NULL, NULL);
    size_t vsdir = r1 && r2 && hd ? 0 : (size_t)-1;
    if (vsdir == 0) for (size_t i = 0; i < N; i++) vsdir += hd[i] != fromdir[i];
    CHECK(rem_dir == 1 && n2 == 1 && has && !hasnot && st.remastered == 1 && st.standin == 3 && vsdir == 0,
          "pack: remaster %s s=%d: %d remastered from the directory, %d from the pack (%d stand-ins); %zu pixels differ\n",
          path, s, rem_dir, st.remastered, st.standin, vsdir);

    /* 3. the directory before the pack: a directory whose file differs paints its own */
    char d2[1100]; snprintf(d2, sizeof d2, "%s/override", dir);
    uint32_t *flat = malloc(N * 4u);
    for (size_t i = 0; i < N; i++) flat[i] = 0xff102030u;
    snprintf(p, sizeof p, "mkdir -p '%s'", d2);
    snprintf(src, sizeof src, "%s/SKYLINE_TEAR_012.dds", d2);
    int ov = !system(p) && write_dds(src, flat, x->P.w * s, x->P.h * s);
    hdskin_set_dir(d2);
    int r3 = hdskin_composite(0xdd2u, "TEST", &x->P, (const HdTexName (*)[2])names, &x->M, s, &st);
    hd = hdskin_get(0xdd2u, NULL, NULL, NULL);
    size_t vs = 0;
    if (r3 && hd) for (size_t i = 0; i < N; i++) vs += hd[i] != fromdir[i];
    CHECK(ov && r3 && hd && st.remastered == 1 && vs > 0, "pack: and directory: the directory's file painted (%zu pixels differ from the pack's)\n", vs);
    hdskin_set_dir(NULL);
    free(flat);

    /* 4. a damaged pack: its CRC-32, then a byte of its pixels changed: refused, the stand-in (the stock look) */
    for (int dmg = 0; dmg < 2; dmg++){
        FILE *f = fopen(pak, "r+b");
        long at = 16 + 20;                                         /* entry 0's crc */
        if (f){
            if (dmg){ fseek(f, 0, SEEK_END); at = ftell(f) - 64; }
            fseek(f, at, SEEK_SET); int c = fgetc(f); fseek(f, at, SEEK_SET); fputc(c ^ 0x5a, f); fclose(f);
        }
        int n4 = hdskin_set_pack(pak);
        int pw = 0, ph = 0; char why[160] = "";
        uint32_t *bad = hdskin_pack_pixels(hd_name_hash("SKYLINE_TEAR_012"), &pw, &ph, why, sizeof why);
        int r4 = hdskin_composite(0xdd3u, "TEST", &x->P, (const HdTexName (*)[2])names, &x->M, s, &st);
        hd = hdskin_get(0xdd3u, NULL, NULL, NULL);
        size_t back = r4 && hd ? blocks_vs(hd, x->out, x->P.w, x->P.h, s, 3) : (size_t)-1;
        /* a changed pixel byte may still unpack: then its CRC-32 refuses it */
        CHECK(f && n4 == 1 && !bad && (strstr(why, "CRC-32") || (dmg && strstr(why, "unpack"))) && back == 0 && st.remastered == 0
              && !hdskin_has_remaster(hd_name_hash("SKYLINE_TEAR_012")),
              "pack: damaged (%s): refused (%s), the stock look (%zu pixels differ)\n", dmg ? "a pixel byte" : "its CRC-32", why, back);
        free(bad);
        if ((f = fopen(pak, "r+b"))){                              /* undone: each case one kind of damage */
            fseek(f, at, SEEK_SET); int c = fgetc(f); fseek(f, at, SEEK_SET); fputc(c ^ 0x5a, f); fclose(f);
        }
    }
    free(fromdir);
    hdskin_set_pack(NULL);
}

static void test_api(const Fx *x){
    HdTexName nm[VB_MAX_LAYERS][2]; names_for(x, nm, NULL);
    HdStats a, b;
    hdskin_composite(0x111u, "A", &x->P, (const HdTexName (*)[2])nm, &x->M, 2, &a);
    hdskin_composite(0x222u, "B", &x->P, (const HdTexName (*)[2])nm, &x->M, 4, &b);
    int w1 = 0, w2 = 0, h2 = 0; uint32_t g1 = 0, g2 = 0;
    const uint32_t *p1 = hdskin_get(0x111u, &w1, NULL, &g1), *p2 = hdskin_get(0x222u, &w2, &h2, &g2);
    size_t ms = 0; hdskin_memory(&ms, NULL);
    hdskin_forget(0x111u);
    const uint32_t *gone = hdskin_get(0x111u, NULL, NULL, NULL);
    hdskin_composite(0x222u, "B", &x->P, (const HdTexName (*)[2])nm, &x->M, 2, &b);
    int w3 = 0; uint32_t g3 = 0; hdskin_get(0x222u, &w3, NULL, &g3);
    CHECK(p1 && p2 && w1 == 2 * x->P.w && w2 == 4 * x->P.w && h2 == 4 * x->P.h && g2 > g1 && !gone && w3 == 2 * x->P.w && g3 > g2
          && !hdskin_get(0x333u, NULL, NULL, NULL),
          "api: two surfaces %d and %d wide (gen %u, %u), forget, reuse at %d wide (gen %u); before forget %zu MB of surfaces\n",
          w1, w2, g1, g2, w3, g3, ms >> 20);
}

int main(int argc, char **argv){
    /* no fixtures: the pack's round trip alone (web/test/runtime-unit.test.mjs) */
    const char *dir = argc > 1 ? argv[1] : "out/hdskin_unit.tmp";
    if (argc < 2 && system("mkdir -p out/hdskin_unit.tmp")){ printf("FAIL setup: cannot make %s\n", dir); return 1; }
    test_pack_roundtrip(dir);
    int remastered = 0;
    for (int i = 2; i < argc; i++){
        static Fx x;
        if (!load(argv[i], &x)){ g_fail = 1; continue; }
        test_nearest(argv[i], &x, 0x1000u + (uint32_t)i);
        if (!remastered && x.present == 2 && x.P.layer[0].ncolours && !x.P.layer[1].ncolours){ test_remaster(dir, argv[i], &x); test_pack(dir, argv[i], &x); remastered = 1; }
        if (i == 2) test_api(&x);
        size_t ms, mc; hdskin_memory(&ms, &mc);
        printf("     memory now: surfaces %zu MB, cached sources %zu MB\n", ms >> 20, mc >> 20);
        free(x.data);
        hdskin_reset();
    }
    if (argc > 2 && !remastered){ printf("FAIL remaster: no fixture with a colourised layer 0 and a plain layer 1\n"); g_fail = 1; }
    printf(g_fail ? "hdskin_unit: FAILED\n" : "hdskin_unit: all passed\n");
    return g_fail;
}
