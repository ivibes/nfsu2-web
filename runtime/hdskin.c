/* hdskin.c -- HD copies of the car paint surfaces; see hdskin.h.
 *
 * Memory: one buffer per surface ((w*s) x (h*s) words: 4 MB at 1024, 16 MB
 * at 2048), kept until hdskin_forget / hdskin_prune / hdskin_reset; and a
 * cache of HD sources (stand-ins and loaded remasters, each a vinyl or a
 * mask at HD size) of at most HD_CACHE_BYTES beyond the ones the composite
 * in progress uses, least recently used dropped first.  A stand-in is keyed
 * by its name hash, a hash of its stock pixels and s, so a texture whose
 * pixels change is scaled again.  The remaster pack (tools/hdvinyls.py) stays
 * open; its table (32 bytes a texture) is read at hdskin_set_pack, a
 * texture's pixels (raw deflate, runtime/web/inflate.h, then their CRC-32)
 * when a composite first needs it: about 30 ms for a 2048 one natively. */
#include "hdskin.h"
#include "web/inflate.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6;
}

uint32_t hd_name_hash(const char *s){
    uint32_t h = 0xffffffffu;
    for (; *s; s++) h = h * 33u + (uint8_t)*s;
    return h;
}

/* ---- nearest scale and markers ------------------------------------------ */
void hd_scale_nearest(const uint32_t *src, int w, int h, int s, uint32_t *dst){
    const size_t W = (size_t)w * (size_t)s;
    for (int y = 0; y < h; y++){
        uint32_t *row = dst + (size_t)y * s * W;
        const uint32_t *sr = src + (size_t)y * w;
        for (int x = 0; x < w; x++){
            uint32_t v = sr[x];
            for (int i = 0; i < s; i++) row[(size_t)x * s + i] = v;
        }
        for (int j = 1; j < s; j++) memcpy(row + (size_t)j * W, row, W * 4u);
    }
}

void hd_markers_scale(const VbMarkers *m, int w, int h, int s, VbMarkers *out){
    const uint32_t npix = (uint32_t)w * (uint32_t)h, W = (uint32_t)w * (uint32_t)s;
    for (int k = 0; k < VB_NMARK; k++){
        int n = m->count[k] < VB_MARK_MAX ? m->count[k] : VB_MARK_MAX, o = 0;
        for (int j = 0; j < n; j++){
            uint32_t p = m->pos[k][j];
            if (p >= npix) continue;                       /* vb_blend32 skips these too */
            uint32_t x = p % (uint32_t)w, y = p / (uint32_t)w;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++)
                    if (o < VB_MARK_MAX) out->pos[k][o++] = (y * s + dy) * W + x * s + dx;
        }
        out->count[k] = o;
    }
}

/* ---- DDS ------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }

uint32_t *hd_load_dds(const char *path, int *pw, int *ph, char *why, size_t whylen){
#define BAD(...) do{ if (why) snprintf(why, whylen, __VA_ARGS__); if (f) fclose(f); free(px); return NULL; }while(0)
    FILE *f = fopen(path, "rb"); uint32_t *px = NULL;
    if (!f) BAD("cannot open");
    uint8_t hd[128];
    if (fread(hd, 1, 128, f) != 128 || memcmp(hd, "DDS ", 4) || rd32(hd + 4) != 124u || rd32(hd + 76) != 32u) BAD("not a DDS file");
    uint32_t hgt = rd32(hd + 12), wid = rd32(hd + 16), pf = rd32(hd + 80), bits = rd32(hd + 88);
    uint32_t rm = rd32(hd + 92), gm = rd32(hd + 96), bm = rd32(hd + 100), am = rd32(hd + 104);
    if (pf & 4u) BAD("compressed (FourCC %.4s); only uncompressed 32-bit is read", (const char *)hd + 84);
    if (!(pf & 0x40u) || bits != 32u || gm != 0x0000ff00u) BAD("not 32-bit RGB (flags %x, %u bits)", pf, bits);
    int swap;                                      /* 1: A8B8G8R8, red in byte 0 */
    if (rm == 0x00ff0000u && bm == 0x000000ffu) swap = 0;
    else if (rm == 0x000000ffu && bm == 0x00ff0000u) swap = 1;
    else BAD("channel masks %08x %08x %08x not handled", rm, gm, bm);
    int opaque = !(pf & 1u) || am != 0xff000000u;  /* X8R8G8B8: alpha 255 */
    if (!wid || !hgt || wid > HD_MAX_SIDE * 2u || hgt > HD_MAX_SIDE * 2u) BAD("size %ux%u", wid, hgt);
    size_t n = (size_t)wid * hgt;
    if (!(px = malloc(n * 4u))) BAD("no memory for %ux%u", wid, hgt);
    if (fread(px, 4, n, f) != n) BAD("truncated");
    fclose(f); f = NULL;
    for (size_t i = 0; i < n; i++){
        uint32_t v = px[i];
        if (swap) v = vb_swap_rb(v);
        if (opaque) v |= 0xff000000u;
        px[i] = v;
    }
    *pw = (int)wid; *ph = (int)hgt;
    return px;
#undef BAD
}

/* ---- the remaster directory ------------------------------------------------ */
static struct { uint32_t hash; char *path; int refused; } *g_idx;   /* refused: logged once, then the pack's or the stand-in */
static int g_nidx;

static void cache_drop_remasters(void);

int hdskin_set_dir(const char *dir){
    for (int i = 0; i < g_nidx; i++) free(g_idx[i].path);
    free(g_idx); g_idx = NULL; g_nidx = 0;
    cache_drop_remasters();
    if (!dir || !*dir) return 0;
    DIR *d = opendir(dir);
    if (!d){ fprintf(stderr, "[hd-vinyls] remaster directory %s: cannot open\n", dir); return 0; }
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))){
        size_t l = strlen(e->d_name);
        if (l < 5 || l > 200) continue;
        const char *x = e->d_name + l - 4;
        if (!(x[0] == '.' && (x[1] | 32) == 'd' && (x[2] | 32) == 'd' && (x[3] | 32) == 's')) continue;
        char stem[204];
        for (size_t i = 0; i < l - 4; i++){ char ch = e->d_name[i]; stem[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch; }
        stem[l - 4] = 0;
        if (g_nidx == cap){ cap = cap ? cap * 2 : 64; void *n = realloc(g_idx, (size_t)cap * sizeof *g_idx); if (!n) break; g_idx = n; }
        size_t pl = strlen(dir) + l + 2;
        char *p = malloc(pl);
        if (!p) break;
        snprintf(p, pl, "%s/%s", dir, e->d_name);
        g_idx[g_nidx].hash = hd_name_hash(stem); g_idx[g_nidx].path = p; g_idx[g_nidx].refused = 0; g_nidx++;
    }
    closedir(d);
    return g_nidx;
}
static int idx_find(uint32_t h){
    for (int i = 0; i < g_nidx; i++) if (g_idx[i].hash == h) return i;
    return -1;
}

/* ---- the remaster pack (tools/hdvinyls.py) --------------------------------- */
typedef struct { uint32_t hash; uint16_t w, h; uint32_t off, size, name, crc, r0, r1; } PackEnt;
_Static_assert(sizeof(PackEnt) == 32, "hdvinyls.pak entry");
static FILE *g_pack; static char g_pack_path[1024];
static PackEnt *g_pent; static uint8_t *g_prefused; static int g_npent;   /* g_prefused: 1 refused (logged once), 2 unusable */

/* CRC-32 (zlib's), eight bytes a step: 16 MB in a few ms */
static uint32_t g_crc_tab[8][256];
static uint32_t crc32_of(const uint8_t *p, size_t n){
    if (!g_crc_tab[0][1]){
        for (uint32_t i = 0; i < 256; i++){
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            g_crc_tab[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int t = 1; t < 8; t++) g_crc_tab[t][i] = (g_crc_tab[t - 1][i] >> 8) ^ g_crc_tab[0][g_crc_tab[t - 1][i] & 0xFFu];
    }
    uint32_t c = 0xFFFFFFFFu;
    for (; n >= 8; n -= 8, p += 8){
        uint32_t a = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        c = g_crc_tab[7][a & 0xFFu] ^ g_crc_tab[6][(a >> 8) & 0xFFu] ^ g_crc_tab[5][(a >> 16) & 0xFFu] ^ g_crc_tab[4][a >> 24]
          ^ g_crc_tab[3][b & 0xFFu] ^ g_crc_tab[2][(b >> 8) & 0xFFu] ^ g_crc_tab[1][(b >> 16) & 0xFFu] ^ g_crc_tab[0][b >> 24];
    }
    while (n--) c = (c >> 8) ^ g_crc_tab[0][(c ^ *p++) & 0xFFu];
    return ~c;
}

int hdskin_set_pack(const char *path){
    if (g_pack) fclose(g_pack);
    g_pack = NULL; g_pack_path[0] = 0;
    free(g_pent); free(g_prefused); g_pent = NULL; g_prefused = NULL; g_npent = 0;
    cache_drop_remasters();
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hd[4]; long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 16 || fseek(f, 0, SEEK_SET) || fread(hd, sizeof hd, 1, f) != 1 || memcmp(hd, "NHDV", 4) || hd[1] != 1u
        || !hd[2] || hd[2] > 65536u){
        fprintf(stderr, "[hd-vinyls] %s: not a remaster pack this runtime reads\n", path); fclose(f); return 0;
    }
    PackEnt *e = malloc((size_t)hd[2] * sizeof *e);
    uint8_t *r = calloc(hd[2], 1);
    if (!e || !r || fread(e, sizeof *e, hd[2], f) != hd[2]){
        fprintf(stderr, "[hd-vinyls] %s: unreadable\n", path); free(e); free(r); fclose(f); return 0;
    }
    int bad = 0;
    for (uint32_t i = 0; i < hd[2]; i++)
        if (!e[i].w || !e[i].h || e[i].w > HD_MAX_SIDE || e[i].h > HD_MAX_SIDE || !e[i].size
            || (uint64_t)e[i].off + e[i].size > (uint64_t)size){ r[i] = 2; bad++; }
    if (bad) fprintf(stderr, "[hd-vinyls] %s: %d of %u entries out of the file or of a size this does not read\n", path, bad, hd[2]);
    g_pack = f; g_pent = e; g_prefused = r; g_npent = (int)hd[2];
    snprintf(g_pack_path, sizeof g_pack_path, "%s", path);
    return g_npent - bad;
}
static int pack_find(uint32_t h){
    int lo = 0, hi = g_npent - 1;                    /* sorted by hash (tools/hdvinyls.py) */
    while (lo <= hi){
        int m = (lo + hi) / 2;
        if (g_pent[m].hash == h) return m;
        if (g_pent[m].hash < h) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
static const char *pack_name(int i, char *buf, size_t n){
    snprintf(buf, n, "%08x", g_pent[i].hash);
    if (g_pack && !fseek(g_pack, (long)g_pent[i].name, SEEK_SET)){
        size_t k = fread(buf, 1, n - 1, g_pack); buf[k] = 0;
        for (size_t j = 0; j < k; j++) if ((uint8_t)buf[j] < 32 || (uint8_t)buf[j] > 126){ buf[j] = 0; break; }
        if (!buf[0]) snprintf(buf, n, "%08x", g_pent[i].hash);
    }
    return buf;
}
static uint32_t *pack_load(int i, int *pw, int *ph, char *why, size_t whylen){
    const PackEnt *e = &g_pent[i];
    const size_t n = (size_t)e->w * e->h * 4u;
    uint8_t *z = malloc(e->size), *px = malloc(n);
    const char *err = NULL;
    if (!z || !px) err = "no memory";
    else if (fseek(g_pack, (long)e->off, SEEK_SET) || fread(z, 1, e->size, g_pack) != e->size) err = "cannot read";
    else if (!inflate_raw(z, e->size, px, n)) err = "its pixels do not unpack";
    else if (crc32_of(px, n) != e->crc) err = "its pixels' CRC-32 differs";
    free(z);
    if (err){ snprintf(why, whylen, "%s", err); free(px); return NULL; }
    *pw = e->w; *ph = e->h;
    return (uint32_t *)px;
}
uint32_t *hdskin_pack_pixels(uint32_t hash, int *w, int *h, char *why, size_t whylen){
    int i = g_pack ? pack_find(hash) : -1;
    if (i < 0){ if (why) snprintf(why, whylen, "not in the pack"); return NULL; }
    char tmp[160];
    return pack_load(i, w, h, why ? why : tmp, why ? whylen : sizeof tmp);
}
int hdskin_has_remaster(uint32_t hash){
    int i;
    if ((i = idx_find(hash)) >= 0 && !g_idx[i].refused) return 1;
    return g_pack && (i = pack_find(hash)) >= 0 && !g_prefused[i];
}

/* ---- source cache ----------------------------------------------------------- */
typedef struct {
    uint32_t hash; uint64_t content; int s, remaster, w, h;
    uint32_t *px; size_t bytes; uint64_t used; int pinned;
} Src;
static Src *g_src; static int g_nsrc, g_capsrc;
static uint64_t g_tick;
static size_t g_srcbytes;

static void src_free(int i){
    free(g_src[i].px); g_srcbytes -= g_src[i].bytes;
    g_src[i] = g_src[--g_nsrc];
}
static void cache_drop_remasters(void){
    for (int i = g_nsrc - 1; i >= 0; i--) if (g_src[i].remaster) src_free(i);
}
static void cache_trim(void){
    for (;;){
        size_t loose = 0; int lru = -1;
        for (int i = 0; i < g_nsrc; i++) if (!g_src[i].pinned){
            loose += g_src[i].bytes;
            if (lru < 0 || g_src[i].used < g_src[lru].used) lru = i;
        }
        if (loose <= HD_CACHE_BYTES || lru < 0) return;
        src_free(lru);
    }
}
static Src *src_add(uint32_t hash, uint64_t content, int s, int remaster, int w, int h, uint32_t *px){
    if (g_nsrc == g_capsrc){
        int cap = g_capsrc ? g_capsrc * 2 : 32;
        void *n = realloc(g_src, (size_t)cap * sizeof *g_src);
        if (!n){ free(px); return NULL; }
        g_src = n; g_capsrc = cap;
    }
    Src *e = &g_src[g_nsrc++];
    e->hash = hash; e->content = content; e->s = s; e->remaster = remaster; e->w = w; e->h = h;
    e->px = px; e->bytes = (size_t)w * h * 4u; e->used = ++g_tick; e->pinned = 1;
    g_srcbytes += e->bytes;
    return e;
}

static uint64_t content_hash(const uint32_t *p, size_t n){
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++){ h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* a remaster at W x H from one of rw x rh: box down, nearest up, else NULL */
static uint32_t *fit(uint32_t *px, int rw, int rh, int W, int H){
    if (rw == W && rh == H) return px;
    uint32_t *o = NULL;
    if (rw > W && rw % W == 0 && rh % H == 0 && rw / W == rh / H){
        int k = rw / W; uint32_t kk = (uint32_t)(k * k);
        if ((o = malloc((size_t)W * H * 4u)))
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++){
                uint32_t a[4] = { 0, 0, 0, 0 };
                for (int j = 0; j < k; j++) for (int i = 0; i < k; i++){
                    uint32_t v = px[(size_t)(y * k + j) * rw + (size_t)x * k + i];
                    for (int b = 0; b < 4; b++) a[b] += (v >> (8 * b)) & 0xffu;
                }
                uint32_t v = 0;
                for (int b = 0; b < 4; b++) v |= ((a[b] + kk / 2u) / kk) << (8 * b);
                o[(size_t)y * W + x] = v;
            }
    } else if (rw < W && W % rw == 0 && H % rh == 0 && W / rw == H / rh){
        if ((o = malloc((size_t)W * H * 4u))) hd_scale_nearest(px, rw, rh, W / rw, o);
    }
    free(px);
    return o;
}

/* a remaster as the cache keeps it: at W x H (fit), or NULL with why */
static uint32_t *remaster_at(uint32_t *px, int rw, int rh, int W, int H, char *why, size_t whylen){
    if (px && !(px = fit(px, rw, rh, W, H))) snprintf(why, whylen, "size %dx%d does not fit %dx%d", rw, rh, W, H);
    return px;
}

/* one texture of a layer at HD size: pinned in the cache until the composite ends.  The remaster directory's file
 * first, then the pack's, else the stock pixels scaled up. */
static const uint32_t *source(const HdTexName *nm, const uint32_t *stock, int w, int h, int s, HdStats *st){
    uint32_t hash = nm->hash ? nm->hash : (nm->name ? hd_name_hash(nm->name) : 0);
    const int W = w * s, H = h * s;
    const int ix = hash ? idx_find(hash) : -1, pk = hash && g_pack ? pack_find(hash) : -1;
    if ((ix >= 0 && !g_idx[ix].refused) || (pk >= 0 && !g_prefused[pk])){
        for (int i = 0; i < g_nsrc; i++){
            Src *e = &g_src[i];
            if (e->remaster && e->hash == hash && e->s == s && e->w == W && e->h == H){ e->used = ++g_tick; e->pinned = 1; st->remastered++; st->cached++; return e->px; }
        }
        char why[160] = "";
        int rw = 0, rh = 0;
        uint32_t *px = NULL;
        if (ix >= 0 && !g_idx[ix].refused){
            px = hd_load_dds(g_idx[ix].path, &rw, &rh, why, sizeof why);
            px = remaster_at(px, rw, rh, W, H, why, sizeof why);
            if (!px){
                fprintf(stderr, "[hd-vinyls] remaster %s refused: %s; %s instead\n", g_idx[ix].path, why,
                        pk >= 0 && !g_prefused[pk] ? "the pack's" : "the stock pixels scaled up");
                g_idx[ix].refused = 1;
            }
        }
        if (!px && pk >= 0 && !g_prefused[pk]){
            px = pack_load(pk, &rw, &rh, why, sizeof why);
            px = remaster_at(px, rw, rh, W, H, why, sizeof why);
            if (!px){
                char nmb[64];
                fprintf(stderr, "[hd-vinyls] remaster %s in %s refused: %s; the stock pixels scaled up instead\n",
                        pack_name(pk, nmb, sizeof nmb), g_pack_path, why);
                g_prefused[pk] = 1;
            }
        }
        if (px){
            Src *e = src_add(hash, 0, s, 1, W, H, px);
            if (e){ st->remastered++; return e->px; }
        }
    }
    uint64_t ch = content_hash(stock, (size_t)w * h);
    for (int i = 0; i < g_nsrc; i++){
        Src *e = &g_src[i];
        if (!e->remaster && e->hash == hash && e->content == ch && e->s == s && e->w == W && e->h == H){
            e->used = ++g_tick; e->pinned = 1; st->standin++; st->cached++; return e->px;
        }
    }
    uint32_t *px = malloc((size_t)W * H * 4u);
    if (!px) return NULL;
    hd_scale_nearest(stock, w, h, s, px);
    Src *e = src_add(hash, ch, s, 0, W, H, px);
    if (!e) return NULL;
    st->standin++;
    return e->px;
}

/* ---- surfaces ------------------------------------------------------------- */
typedef struct { uint32_t key; char name[32]; int w, h; uint32_t *px; uint32_t gen; } Surf;
static Surf *g_surf; static int g_nsurf, g_capsurf;
static uint32_t g_gen;
static VbMarkers g_hdmarks;

static Surf *surf_find(uint32_t key){
    for (int i = 0; i < g_nsurf; i++) if (g_surf[i].key == key) return &g_surf[i];
    return NULL;
}

int hdskin_composite(uint32_t key, const char *surface_name, const VbParams *stock,
                     const HdTexName names[VB_MAX_LAYERS][2], const VbMarkers *marks,
                     int s, HdStats *st_out){
    HdStats st; memset(&st, 0, sizeof st);
    if (s < 1 || s > HD_MAX_SCALE || stock->w <= 0 || stock->h <= 0) return 0;
    const int w = stock->w, h = stock->h, W = w * s, H = h * s;
    if (W > HD_MAX_SIDE || H > HD_MAX_SIDE) return 0;
    double t0 = now_ms();

    static VbParams P;
    P = *stock; P.w = W; P.h = H;
    int ok = 1;
    for (int l = 0; l < stock->nlayers && l < VB_MAX_LAYERS; l++){
        const VbLayer *L = &stock->layer[l];
        VbLayer *Y = &P.layer[l];
        if (!L->key) continue;
        if (L->w != w || L->h != h || !L->vinyl || !L->mask){ Y->w = Y->h = 0; continue; }   /* skipped, as at stock size */
        const uint32_t *v = source(&names[l][0], L->vinyl, w, h, s, &st);
        const uint32_t *m = v ? source(&names[l][1], L->mask, w, h, s, &st) : NULL;
        if (!v || !m){ ok = 0; break; }
        Y->vinyl = v; Y->mask = m; Y->w = W; Y->h = H;
    }
    hd_markers_scale(marks, w, h, s, &g_hdmarks);
    for (int k = 0; k < VB_NMARK; k++) st.markers += g_hdmarks.count[k];

    Surf *sf = ok ? surf_find(key) : NULL;
    if (ok && !sf){
        if (g_nsurf == g_capsurf){
            int cap = g_capsurf ? g_capsurf * 2 : 16;
            void *n = realloc(g_surf, (size_t)cap * sizeof *g_surf);
            if (n){ g_surf = n; g_capsurf = cap; }
        }
        if (g_nsurf < g_capsurf){ sf = &g_surf[g_nsurf++]; memset(sf, 0, sizeof *sf); sf->key = key; }
    }
    if (sf && (sf->w != W || sf->h != H || !sf->px)){
        free(sf->px);
        sf->px = malloc((size_t)W * H * 4u);
        sf->w = sf->px ? W : 0; sf->h = sf->px ? H : 0;
    }
    ok = ok && sf && sf->px;
    double t1 = now_ms();
    if (ok){
        snprintf(sf->name, sizeof sf->name, "%s", surface_name ? surface_name : "");
        vb_blend32(sf->px, &P, &g_hdmarks);
        sf->gen = ++g_gen;
        st.generation = sf->gen;
    }
    double t2 = now_ms();
    for (int i = 0; i < g_nsrc; i++) g_src[i].pinned = 0;
    cache_trim();
    st.ms_sources = t1 - t0; st.ms_blend = t2 - t1;
    if (st_out) *st_out = st;
    return ok;
}

const uint32_t *hdskin_get(uint32_t surface_ti, int *w, int *h, uint32_t *generation){
    Surf *sf = surf_find(surface_ti);
    if (!sf || !sf->px) return NULL;
    if (w) *w = sf->w;
    if (h) *h = sf->h;
    if (generation) *generation = sf->gen;
    return sf->px;
}
void hdskin_forget(uint32_t surface_ti){
    for (int i = 0; i < g_nsurf; i++) if (g_surf[i].key == surface_ti){
        free(g_surf[i].px); g_surf[i] = g_surf[--g_nsurf]; return;
    }
}
void hdskin_prune(int (*keep)(uint32_t ti, const char *name, void *ctx), void *ctx){
    for (int i = g_nsurf - 1; i >= 0; i--)
        if (!keep(g_surf[i].key, g_surf[i].name, ctx)){ free(g_surf[i].px); g_surf[i] = g_surf[--g_nsurf]; }
}
void hdskin_reset(void){
    for (int i = 0; i < g_nsurf; i++) free(g_surf[i].px);
    free(g_surf); g_surf = NULL; g_nsurf = g_capsurf = 0;
    while (g_nsrc) src_free(g_nsrc - 1);
    free(g_src); g_src = NULL; g_capsrc = 0; g_srcbytes = 0;
    for (int i = 0; i < g_nidx; i++) g_idx[i].refused = 0;
    for (int i = 0; i < g_npent; i++) if (g_prefused[i] == 1) g_prefused[i] = 0;   /* 2: out of the file, for good */
}
void hdskin_memory(size_t *surfaces, size_t *sources){
    size_t b = 0;
    for (int i = 0; i < g_nsurf; i++) b += (size_t)g_surf[i].w * g_surf[i].h * 4u;
    if (surfaces) *surfaces = b;
    if (sources) *sources = g_srcbytes;
}
