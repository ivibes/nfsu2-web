/* hdgpu_unit.c -- runtime/hdgpu.c (when the GPU shows a paint surface's HD
 * copy, and when the game's own texture comes back), with the platform's
 * part replaced by stubs that record each call.
 *
 *   clang -O2 -Wall -ffp-contract=off -Iruntime -o out/hdgpu_unit tests/hdgpu_unit.c runtime/hdgpu.c runtime/hdskin.c runtime/vinylblend.c
 *   out/hdgpu_unit
 *
 * Cases (each prints PASS or FAIL; exit 1 on a FAIL):
 *   inert      before any blend with HD on, every hook does nothing
 *   show       a blend with a copy: shown at its size, k = log2(scale), the
 *              upload inside the blend kept under it
 *   same       the same 512 pixels uploaded after the blend: the copy stays
 *   level      a lower level uploaded: the copy stays
 *   stale      other pixels uploaded after the blend: the game's texture
 *              again, with those pixels
 *   again      the next blend shows the next copy
 *   nocopy     a blend without a copy (composite failed): hidden, with the
 *              blend's 512 pixels
 *   surface    a write through a surface outside the blend: hidden; inside
 *              the blend: nothing
 *   released   a released id: hidden, its surface's HD buffer forgotten
 *              (hdskin_get NULL), the id's later uploads the game's
 *   created    an id made again: hidden
 *   moved      the surface's copy on a new id: the old id hidden
 *   badsize    a copy that is not a whole multiple of the game's size: not shown */
#include "hdgpu.h"
#include "hdskin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { g_fail = 1; printf("FAIL " __VA_ARGS__); } }while(0)

/* ---- the platform stubs ---- */
static struct { int n; unsigned mid, k; int w, h; uint32_t first; } g_show;
static struct { int n; unsigned mid; size_t bytes; uint8_t first; unsigned ow, oh, olev, ofmt; } g_hide;
void hdgpu_be_show(unsigned mid, const uint32_t *px, int w, int h, unsigned k, uint64_t hash,
                   unsigned ow, unsigned oh, unsigned olevels, unsigned ofmt){
    (void)hash; (void)ow; (void)oh; (void)olevels; (void)ofmt;
    g_show.n++; g_show.mid = mid; g_show.k = k; g_show.w = w; g_show.h = h; g_show.first = px[0];
}
void hdgpu_be_hide(unsigned mid, const uint8_t *data, size_t n, unsigned ow, unsigned oh, unsigned olevels, unsigned ofmt){
    g_hide.n++; g_hide.mid = mid; g_hide.bytes = data ? n : 0; g_hide.first = data ? data[0] : 0;
    g_hide.ow = ow; g_hide.oh = oh; g_hide.olev = olevels; g_hide.ofmt = ofmt;
}
static void reset_calls(void){ memset(&g_show, 0, sizeof g_show); memset(&g_hide, 0, sizeof g_hide); }

#define OW 8
#define OH 8
static uint8_t g_px512[OW * OH * 4];                  /* "the game's level 0" */
static uint32_t g_hd[OW * 4 * OH * 4];               /* "the HD copy" */

static void fill(uint8_t v){ memset(g_px512, v, sizeof g_px512); }
/* one game blend of mid: its upload inside, then the copy (or none) */
static void blend(unsigned mid, uint32_t ti, uint8_t v, const uint32_t *px, int s){
    hdgpu_blend_begin(mid);
    fill(v);
    hdgpu_uploaded(mid, 0, g_px512, sizeof g_px512);
    hdgpu_surface_written(mid);                       /* inside the blend: ignored */
    hdgpu_blend_end(mid, ti, px, OW * s, OH * s, 1, OW, OH, 1, 21);
}

int main(void){
    /* inert */
    fill(1);
    int r = hdgpu_uploaded(5, 0, g_px512, sizeof g_px512);
    hdgpu_surface_written(5); hdgpu_released(5); hdgpu_created(5);
    hdgpu_blend_end(5, 0x1000, g_hd, OW * 4, OH * 4, 1, OW, OH, 1, 21);   /* no blend_begin yet: no table */
    CHECK(r == 0 && !g_show.n && !g_hide.n && !hdgpu_shown(5), "inert  hooks do nothing before the first blend\n");

    /* show */
    reset_calls();
    g_hd[0] = 0x11223344u;
    blend(5, 0x1000, 0xA0, g_hd, 4);
    CHECK(g_show.n == 1 && g_show.mid == 5 && g_show.k == 2 && g_show.w == OW * 4 && g_show.h == OH * 4 && g_show.first == 0x11223344u
          && hdgpu_shown(5) && !g_hide.n, "show  copy shown at x4, k=2, the blend's own upload and surface write kept\n");

    /* same */
    reset_calls(); fill(0xA0);
    r = hdgpu_uploaded(5, 0, g_px512, sizeof g_px512);
    CHECK(r == 1 && !g_hide.n && hdgpu_shown(5), "same  the blend's 512 pixels again after it: copy stays\n");

    /* level */
    reset_calls(); fill(0x55);
    r = hdgpu_uploaded(5, 1, g_px512, sizeof g_px512 / 4);
    CHECK(r == 1 && !g_hide.n && hdgpu_shown(5), "level  a lower level uploaded: copy stays\n");

    /* stale */
    reset_calls(); fill(0xB0);
    r = hdgpu_uploaded(5, 0, g_px512, sizeof g_px512);
    CHECK(r == 0 && g_hide.n == 1 && g_hide.mid == 5 && g_hide.bytes == sizeof g_px512 && g_hide.first == 0xB0
          && g_hide.ow == OW && g_hide.oh == OH && g_hide.olev == 1 && g_hide.ofmt == 21 && !hdgpu_shown(5),
          "stale  other pixels after the blend: the game's texture again, with them\n");
    reset_calls(); fill(0xC0);
    r = hdgpu_uploaded(5, 0, g_px512, sizeof g_px512);
    CHECK(r == 0 && !g_hide.n && !g_show.n, "stale  once hidden, uploads are the game's\n");

    /* again */
    reset_calls();
    g_hd[0] = 0x55667788u;
    blend(5, 0x1000, 0xA1, g_hd, 4);
    CHECK(g_show.n == 1 && g_show.first == 0x55667788u && hdgpu_shown(5), "again  the next blend shows the next copy\n");

    /* nocopy */
    reset_calls();
    blend(5, 0x1000, 0xA2, NULL, 4);
    CHECK(!g_show.n && g_hide.n == 1 && g_hide.bytes == sizeof g_px512 && g_hide.first == 0xA2 && !hdgpu_shown(5),
          "nocopy  a blend without a copy: hidden, with that blend's 512 pixels\n");

    /* surface */
    blend(5, 0x1000, 0xA3, g_hd, 4);
    reset_calls();
    hdgpu_surface_written(5);
    CHECK(g_hide.n == 1 && g_hide.first == 0xA3 && !hdgpu_shown(5), "surface  written through a surface outside the blend: hidden\n");

    /* released: with a real HD buffer for the surface */
    {
        static VbParams P; static VbMarkers M; HdTexName names[VB_MAX_LAYERS][2];
        memset(&P, 0, sizeof P); memset(&M, 0, sizeof M); memset(names, 0, sizeof names);
        P.w = OW; P.h = OH; P.base = 0xff336699u;
        int ok = hdskin_composite(0x2000, "DUMMY_SKIN3", &P, (const HdTexName (*)[2])names, &M, 2, NULL);
        int w = 0, h = 0; uint32_t gen = 0;
        const uint32_t *px = hdskin_get(0x2000, &w, &h, &gen);
        blend(7, 0x2000, 0xD0, px, 2);
        CHECK(ok && px && w == OW * 2 && hdgpu_shown(7) && g_show.k == 1, "released  (set up: x2 copy of a composited surface shown, k=1)\n");
        reset_calls();
        hdgpu_released(7);
        CHECK(g_hide.n == 1 && g_hide.mid == 7 && !hdgpu_shown(7) && !hdskin_get(0x2000, NULL, NULL, NULL),
              "released  hidden and the surface's HD buffer forgotten\n");
        reset_calls(); fill(0xD1);
        r = hdgpu_uploaded(7, 0, g_px512, sizeof g_px512);
        CHECK(r == 0 && !g_hide.n, "released  the id's later uploads are the game's\n");
    }

    /* created */
    blend(9, 0x3000, 0xE0, g_hd, 4);
    reset_calls();
    hdgpu_created(9);
    CHECK(g_hide.n == 1 && g_hide.mid == 9 && !g_hide.ow && !g_hide.oh && !hdgpu_shown(9), "created  an id made again: hidden, the new texture not resized\n");

    /* moved */
    blend(10, 0x4000, 0xE1, g_hd, 4);
    reset_calls();
    blend(11, 0x4000, 0xE2, g_hd, 4);
    CHECK(g_hide.n == 1 && g_hide.mid == 10 && !hdgpu_shown(10) && hdgpu_shown(11) && g_show.mid == 11,
          "moved  the surface's copy on a new id: the old id hidden\n");

    /* badsize */
    reset_calls();
    hdgpu_blend_begin(12);
    hdgpu_blend_end(12, 0x5000, g_hd, OW * 3 + 1, OH * 3, 1, OW, OH, 1, 21);
    CHECK(!g_show.n && !hdgpu_shown(12), "badsize  not a whole multiple: not shown\n");
    hdgpu_blend_begin(12);
    hdgpu_blend_end(12, 0x5000, g_hd, OW * 2, OH * 4, 1, OW, OH, 1, 21);
    CHECK(!g_show.n && !hdgpu_shown(12), "badsize  different scales across and down: not shown\n");

    hdgpu_report();
    printf(g_fail ? "hdgpu_unit: FAILED\n" : "hdgpu_unit: all passed\n");
    return g_fail;
}
