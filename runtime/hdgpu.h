/* hdgpu.h -- shows the HD copies of the car paint (runtime/hdskin.c) on the
 * GPU: after each HD composite the paint surface's GPU texture is sampled at
 * the HD size, with a full mip chain, under the car's unchanged UVs.  The
 * game keeps its own 512 texture, sizes and memory.
 *
 * Plain C, guest thread only.  The swap itself is the platform's
 * (hdgpu_be_show / hdgpu_be_hide below): runtime/texrep.m on the Mac binds
 * an HD Metal texture in the draw encoder in place of the game's (as its
 * texture packs do); runtime/web/texrep_web.c makes the browser's texture
 * again at the HD size (metal_replace_texture, as the Detailed Map pack).
 *
 * When the game uploads that texture again:
 *   - inside its own blend (between hdgpu_blend_begin and hdgpu_blend_end)
 *     the upload is the 512 copy of what the HD composite paints next: the
 *     HD copy stays, and the new one is swapped in at hdgpu_blend_end;
 *   - elsewhere, the same 512 pixels as the last blend's (a lock to read):
 *     the HD copy stays;
 *   - elsewhere, other pixels, or a write through a surface: the HD copy is
 *     not current any more; the game's own texture is shown again until its
 *     next blend.
 * A released or re-created texture id drops its HD copy (and hdskin_forget
 * of the surface it showed).  Nothing here runs before the first HD copy is
 * shown: with NFSU2_HD_VINYLS unset every hook returns at its first line. */
#ifndef HDGPU_H
#define HDGPU_H
#include <stddef.h>
#include <stdint.h>

/* paintblend.c, around the game's blend 0x611F10 of the surface whose GPU
 * texture is mid (0: none found) */
void hdgpu_blend_begin(unsigned mid);
/* px: hdskin_get's copy of surface ti after the composite (NULL: none, the
 * composite failed or was not run), w x h at s times the game's ow x oh
 * texture of olevels levels in D3DFORMAT ofmt */
void hdgpu_blend_end(unsigned mid, uint32_t ti, const uint32_t *px, int w, int h, uint32_t gen,
                     unsigned ow, unsigned oh, unsigned olevels, unsigned ofmt);

/* texrep hooks (com.c calls them through texrep_*).  hdgpu_uploaded returns
 * 1 when the upload stays under the HD copy shown (the browser's gpu_web.c
 * has dropped it already), 0 when the game's texture shows it. */
int  hdgpu_uploaded(unsigned mid, unsigned level, const uint8_t *data, size_t n);
void hdgpu_surface_written(unsigned mid);
void hdgpu_created(unsigned mid);
void hdgpu_released(unsigned mid);

int  hdgpu_shown(unsigned mid);                 /* 1 while an HD copy is shown for mid */
int  hdgpu_probe(void);                         /* NFSU2_HD_PROBE=1: GPU read-back checks */
void hdgpu_report(void);                        /* exit line (texrep_report) */
uint64_t hdgpu_hash(const void *p, size_t n);  /* XXH3-64, the probes' hash */

/* ---- the platform's part (texrep.m / texrep_web.c; the unit test's stubs) ----
 * show: from now on mid is sampled as px, w x h (0xAARRGGBB words, top row
 * first), with every mip level below it; k = log2(w / the game's width);
 * hash = hdgpu_hash of px when hdgpu_probe(), else 0.  Called again for the
 * next copy.  hide: the game's own texture again; data = the game's level 0
 * as it uploaded it last (NULL: nothing kept), for a platform whose own
 * uploads were dropped meanwhile; ow = oh = 0: the id was made again for a
 * new texture, only stop showing the copy. */
void hdgpu_be_show(unsigned mid, const uint32_t *px, int w, int h, unsigned k, uint64_t hash,
                   unsigned ow, unsigned oh, unsigned olevels, unsigned ofmt);
void hdgpu_be_hide(unsigned mid, const uint8_t *data, size_t n,
                   unsigned ow, unsigned oh, unsigned olevels, unsigned ofmt);
#endif
