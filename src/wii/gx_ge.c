/**
 * WIISP - gx_ge.c
 * El GE de la PSP dibujado con GX, la GPU del Wii.
 *
 * La geometría sigue en la CPU (ge_vertex.c, la misma que usa el
 * rasterizador por software: transformación, luces, recorte, curvas,
 * sprites...) y aquí llegan las primitivas ya en coordenadas de pantalla de
 * la PSP. GX solo rasteriza:
 *
 *  - Proyección ortográfica en píxeles: x, y, z de la PSP tal cual; el
 *    z-buffer de 24 bits guarda z * 256.
 *  - Perspectiva de las texturas: s/w, t/w y 1/w van en la normal y una
 *    generación de coordenadas 3x4 divide en cada píxel. Color y niebla se
 *    interpolan lineales en pantalla, como en la PSP.
 *  - El rango de profundidad (MINZ/MAXZ) es el recorte en z de GX.
 *  - Función de textura, color secundario y niebla en el TEV; mezcla, tests
 *    de alfa y profundidad, operaciones lógicas y tramado en el hardware.
 *    El stencil de la PSP vive en el alfa del framebuffer: se escribe con
 *    alfa de destino constante (KEEP, ZERO, REPLACE); el test no existe en
 *    GX y se ignora, igual que el test de color y las máscaras parciales.
 *
 * Framebuffers: el EFB (640x528) tiene el búfer de color y el de
 * profundidad en los que dibuja la PSP; los demás viven en texturas (copias
 * del EFB). Cada búfer tiene un estado frente a su copia en la VRAM
 * emulada: igual, más nuevo en la GPU o más nuevo en la VRAM. Los accesos
 * de la CPU (y del propio GE: texturas, CLUT, transferencias) a la VRAM
 * pasan por un gancho de memory.c que baja antes el contenido de la GPU;
 * lo que la CPU escribe se sube antes de volver a dibujar encima.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

/* core/types.h y libogc usan los mismos nombres de tipos con distinta base
   (unsigned int frente a unsigned long): los del núcleo se renombran */
#define u8 core_u8
#define u16 core_u16
#define u32 core_u32
#define u64 core_u64
#define s8 core_s8
#define s16 core_s16
#define s32 core_s32
#define s64 core_s64
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "hle/hle.h"
#undef u8
#undef u16
#undef u32
#undef u64
#undef s8
#undef s16
#undef s32
#undef s64

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include "wii/gx_ge.h"

#define EFB_W 640
#define EFB_H 528
#define VP 1024.0f            /* el viewport cubre 1024x1024 píxeles */
#define MAX_SURF 16
#define SURF_Z 4              /* formato de un búfer de profundidad */
#define MAX_TEX 256
#define TEX_BUDGET (12u << 20)
#define MAX_TEX_SIZE 512

enum { SYNCED, GPU_NEWER, VRAM_NEWER };
/* Dónde está el alfa (stencil) de un búfer de color. GX no lo toca al
   dibujar salvo con operaciones de stencil o borrados; mientras tanto el
   EFB va en RGB8 (color exacto):
     A_UNIFORM  todo el búfer tiene el alfa ualpha
     A_VRAM     el alfa de verdad sigue en la VRAM emulada
     A_GPU      alfa por píxel en el EFB (RGBA6, 6 bits) */
enum { A_UNIFORM, A_VRAM, A_GPU };
enum { MODE_NONE, MODE_EMU, MODE_BLIT };

typedef struct {
	int used;
	core_u32 addr, stride;   /* offset en la VRAM y píxeles por fila */
	int fmt;                 /* GE_FMT_* o SURF_Z */
	int w, h;                /* tamaño en GX, múltiplos de 4 */
	u8 *tex;                 /* copia RGBA8 (color) o Z24X8 (profundidad) */
	int has_tex;             /* tex tiene el contenido de la GPU */
	int state;
	int amode;               /* A_* */
	int ualpha;
} Surface;

typedef struct {
	int used;
	core_u32 addr, fmt, size, bufw, clutfmt, clut_hash, hash, mipkey;
	int levels;                /* niveles de mipmap en data */
	unsigned checked_frame, used_frame;
	int w, h;
	u32 bytes;
	u8 *data;
	GXTexObj obj;
} TexEntry;

static int enabled;
static Surface surf[MAX_SURF];
static Surface *cur_c, *cur_z;          /* búferes en el EFB */
static int efb_c_newer, efb_z_newer;    /* el EFB es más nuevo que su tex */
static int restored_c, restored_z;      /* el EFB ya tiene su contenido */
static int efb_fmt = -1;
static int mode = MODE_NONE;

static int targets_dirty = 1, gx_dirty = 1, skip_draws;
static int touch_c, touch_z, writes_c, writes_z;
static int stencil_val;                 /* alfa que escribe la primitiva o -1 */
static int reads_dst_alpha;             /* la mezcla usa el alfa del destino */
static int want_promote;
static unsigned frame;

static TexEntry texc[MAX_TEX];
static TexEntry *cur_tex, *last_tex;
static u32 tex_total;
static int tex_flushed = 1;
static core_u32 last_clut_gen;
static u8 empty_tex[64] __attribute__((aligned(32)));
static GXTexObj empty_obj;

/* Estado de las primitivas */
static int tex_on, tex_proj, through, flat;
static float inv_tw, inv_th, tex_su = 1.0f, tex_sv = 1.0f;
static GXTexObj rtt_obj;
static int use_rtt;                     /* la textura es un framebuffer en la GPU */
static float pos_off;

/* Perfilado (ticks del Wii) */
static u64 prof_setup, prof_state, prof_present;
static unsigned prof_vram_presents, prof_downloads, prof_uploads, prof_decodes;

static void *defer_list[64];
static int ndefer;

/* Tramado de la PSP: textura 4x4 (128 + valor) que suma el TEV */
static u8 dither_tex[8][64] __attribute__((aligned(32)));
static GXTexObj dither_obj;
static int dither_idx, dither_ready, last_dither[16];

static const u8 cmp_map[8] = {
	GX_NEVER, GX_ALWAYS, GX_EQUAL, GX_NEQUAL, GX_LESS, GX_LEQUAL, GX_GREATER, GX_GEQUAL
};

/* --- Utilidades ------------------------------------------------------------- */

static void flush_deferred(void){
	while(ndefer) free(defer_list[--ndefer]);
}

/* Espera a que la GPU acabe todo lo pendiente */
static void wait_gpu(void){
	GX_DrawDone();
	flush_deferred();
}

/* Libera memoria que la GPU aún puede estar leyendo */
static void defer_free(void *p){
	if(!p) return;
	if(ndefer == (int)(sizeof(defer_list) / sizeof(defer_list[0]))) wait_gpu();
	defer_list[ndefer++] = p;
}

static inline u8 *tile_ptr(u8 *base, int w, int x, int y){
	return base + ((((y >> 2) * (w >> 2) + (x >> 2)) << 6) | ((((y & 3) << 2) | (x & 3)) << 1));
}

static void copy_filter_none(void){
	static u8 sp[12][2] = {
		{ 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
		{ 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }
	};
	static u8 vf[7] = { 0, 0, 21, 22, 21, 0, 0 };
	GX_SetCopyFilter(GX_FALSE, sp, GX_FALSE, vf);
}

static void set_pf(int pf){
	if(pf == efb_fmt) return;
	GX_SetPixelFmt((u8)pf, GX_ZC_LINEAR);
	efb_fmt = pf;
}

static int has_alpha_fmt(int fmt){ return fmt == GE_FMT_5551 || fmt == GE_FMT_4444 || fmt == GE_FMT_8888; }

/* RGB8 (color exacto) salvo que el búfer necesite alfa por píxel */
static int surface_pf(const Surface *s){
	return has_alpha_fmt(s->fmt) && s->amode == A_GPU ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24;
}

/* --- Rangos vigilados de la VRAM --------------------------------------------- */

static int surf_bpp(int fmt){ return fmt == GE_FMT_8888 ? 4 : 2; }

static void surf_range(const Surface *s, core_u32 *lo, core_u32 *hi){
	core_u32 a = s->addr, e = s->addr + s->stride * (core_u32)s->h * (core_u32)surf_bpp(s->fmt);
	if(s->fmt == SURF_Z){
		/* El swizzle mueve los bytes de la profundidad hasta ~32 KB */
		a = a > 0x10000 ? a - 0x10000 : 0;
		e += 0x10000;
	}
	if(e > PSP_VRAM_SIZE) e = PSP_VRAM_SIZE;
	*lo = a;
	*hi = e;
}

static void recompute_watch(void){
	core_u32 lo[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu }, hi[2] = { 0, 0 }, a, e;
	int i, w;
	for(i = 0; i < MAX_SURF; i++){
		const Surface *s = &surf[i];
		if(!s->used || s->state == VRAM_NEWER) continue;
		surf_range(s, &a, &e);
		for(w = s->state == GPU_NEWER ? 0 : 1; w < 2; w++){
			if(a < lo[w]) lo[w] = a;
			if(e > hi[w]) hi[w] = e;
		}
	}
	for(w = 0; w < 2; w++){
		if(lo[w] >= hi[w]) lo[w] = hi[w] = 0;
		mem_vram_watch_lo[w] = lo[w];
		mem_vram_watch_hi[w] = hi[w];
	}
}

/* --- Conversión entre la VRAM y las copias de GX ------------------------------ */

static inline u8 *vram_color(const Surface *s, int x, int y, int bpp){
	return psp_mem.vram + ((s->addr + ((core_u32)y * s->stride + (core_u32)x) * (core_u32)bpp) & (PSP_VRAM_SIZE - 1));
}

static inline u8 *vram_depth(const Surface *s, int x, int y){
	core_u32 off = (s->addr + ((core_u32)y * s->stride + (core_u32)x) * 2) & (PSP_VRAM_SIZE - 1);
	return psp_mem.vram + mem_vram_deswizzle(off, 1);
}

static int visible_w(const Surface *s){
	return s->w < (int)s->stride ? s->w : (int)s->stride;
}

/* tex (GPU) -> VRAM, fila a fila y por formato */
static void tex_to_vram(const Surface *s){
	int x, y, w = visible_w(s), keep_a = s->amode == A_VRAM;
	u32 ua = (u32)s->ualpha;
	DCInvalidateRange(s->tex, (u32)(s->w * s->h * 4));
	for(y = 0; y < s->h; y++){
		const u8 *trow = s->tex + (u32)(((y >> 2) * (s->w >> 2)) << 6) + (u32)((y & 3) << 3);
		if(s->fmt == SURF_Z){
			for(x = 0; x < w; x++){
				const u8 *t = trow + ((x >> 2) << 6) + ((x & 3) << 1);
				u8 *p = vram_depth(s, x, y);
				p[0] = t[32];          /* z24 >> 8 */
				p[1] = t[1];
			}
			continue;
		}
		if(((s->addr + (u32)y * s->stride * (u32)surf_bpp(s->fmt)) & (PSP_VRAM_SIZE - 1)) + (u32)w * (u32)surf_bpp(s->fmt) > PSP_VRAM_SIZE){
			/* La fila da la vuelta al final de la VRAM: píxel a píxel */
			for(x = 0; x < w; x++){
				const u8 *t = trow + ((x >> 2) << 6) + ((x & 3) << 1);
				u32 a = s->amode == A_GPU ? t[0] : ua, r = t[1], g = t[32], b = t[33], v;
				u8 *p;
				if(s->fmt == GE_FMT_8888){
					p = vram_color(s, x, y, 4);
					p[0] = (u8)r; p[1] = (u8)g; p[2] = (u8)b;
					if(!keep_a) p[3] = (u8)a;
					continue;
				}
				p = vram_color(s, x, y, 2);
				v = s->fmt == GE_FMT_565 ? (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
				  : s->fmt == GE_FMT_5551 ? (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15)
				  : (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
				if(keep_a && s->fmt != GE_FMT_565){
					u32 k = s->fmt == GE_FMT_5551 ? 0x8000u : 0xF000u;
					v = (v & ~k) | ((p[0] | ((u32)p[1] << 8)) & k);
				}
				p[0] = (u8)v; p[1] = (u8)(v >> 8);
			}
			continue;
		}
		{
			u8 *p = vram_color(s, 0, y, surf_bpp(s->fmt));
			switch(s->fmt){
			case GE_FMT_8888:
				for(x = 0; x < w; x++, p += 4){
					const u8 *t = trow + ((x >> 2) << 6) + ((x & 3) << 1);
					p[0] = t[1]; p[1] = t[32]; p[2] = t[33];
					if(!keep_a) p[3] = s->amode == A_GPU ? t[0] : (u8)ua;
				}
				break;
			case GE_FMT_565:
				for(x = 0; x < w; x++, p += 2){
					const u8 *t = trow + ((x >> 2) << 6) + ((x & 3) << 1);
					u32 v = (u32)(t[1] >> 3) | ((u32)(t[32] >> 2) << 5) | ((u32)(t[33] >> 3) << 11);
					p[0] = (u8)v; p[1] = (u8)(v >> 8);
				}
				break;
			default: {
				int is5551 = s->fmt == GE_FMT_5551;
				u32 k = is5551 ? 0x8000u : 0xF000u;
				for(x = 0; x < w; x++, p += 2){
					const u8 *t = trow + ((x >> 2) << 6) + ((x & 3) << 1);
					u32 a = s->amode == A_GPU ? t[0] : ua, v;
					v = is5551 ? (u32)(t[1] >> 3) | ((u32)(t[32] >> 3) << 5) | ((u32)(t[33] >> 3) << 10) | ((a >> 7) << 15)
					           : (u32)(t[1] >> 4) | ((u32)(t[32] >> 4) << 4) | ((u32)(t[33] >> 4) << 8) | ((a >> 4) << 12);
					if(keep_a) v = (v & ~k) | ((p[0] | ((u32)p[1] << 8)) & k);
					p[0] = (u8)v; p[1] = (u8)(v >> 8);
				}
				break;
			}
			}
		}
	}
}

static inline core_u32 vram16_to_8888(int fmt, core_u32 v){
	core_u32 r, g, b, a;
	switch(fmt){
	case GE_FMT_565:
		r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31;
		return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
	case GE_FMT_5551:
		r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31;
		return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | ((v & 0x8000) ? 0xFF000000u : 0);
	default:
		r = v & 15; g = (v >> 4) & 15; b = (v >> 8) & 15; a = v >> 12;
		return (r * 0x11) | ((g * 0x11) << 8) | ((b * 0x11) << 16) | ((a * 0x11) << 24);
	}
}

/* VRAM -> tex. Un alfa igual en todo el búfer se queda en ualpha. */
static void vram_to_tex(Surface *s){
	int x, y, w = visible_w(s), first_a = -1, uniform = 1, bpp = surf_bpp(s->fmt);
	prof_uploads++;
	memset(s->tex, 0, (size_t)(s->w * s->h * 4));
	for(y = 0; y < s->h; y++){
		u8 *trow = s->tex + (u32)(((y >> 2) * (s->w >> 2)) << 6) + (u32)((y & 3) << 3);
		int wraps = s->fmt != SURF_Z &&
		            ((s->addr + (u32)y * s->stride * (u32)bpp) & (PSP_VRAM_SIZE - 1)) + (u32)w * (u32)bpp > PSP_VRAM_SIZE;
		const u8 *p = s->fmt != SURF_Z && !wraps ? vram_color(s, 0, y, bpp) : NULL;
		for(x = 0; x < w; x++){
			u8 *t = trow + ((x >> 2) << 6) + ((x & 3) << 1);
			core_u32 c;
			if(s->fmt == SURF_Z){
				const u8 *z = vram_depth(s, x, y);
				t[0] = 0xFF; t[1] = z[1]; t[32] = z[0]; t[33] = 0;
				continue;
			}
			if(!p){
				const u8 *q = vram_color(s, x, y, bpp);
				c = bpp == 4 ? (core_u32)q[0] | ((core_u32)q[1] << 8) | ((core_u32)q[2] << 16) | ((core_u32)q[3] << 24)
				             : vram16_to_8888(s->fmt, (core_u32)q[0] | ((core_u32)q[1] << 8));
			} else if(bpp == 4){
				const u8 *q = p + x * 4;
				c = (core_u32)q[0] | ((core_u32)q[1] << 8) | ((core_u32)q[2] << 16) | ((core_u32)q[3] << 24);
			} else {
				const u8 *q = p + x * 2;
				c = vram16_to_8888(s->fmt, (core_u32)q[0] | ((core_u32)q[1] << 8));
			}
			t[0] = (u8)(c >> 24); t[1] = (u8)c; t[32] = (u8)(c >> 8); t[33] = (u8)(c >> 16);
			if(first_a < 0) first_a = (int)(c >> 24);
			else if((int)(c >> 24) != first_a) uniform = 0;
		}
	}
	if(s->fmt != SURF_Z){
		s->amode = has_alpha_fmt(s->fmt) && !uniform ? A_VRAM : A_UNIFORM;
		s->ualpha = first_a < 0 || !has_alpha_fmt(s->fmt) ? 0 : first_a;
	}
	DCFlushRange(s->tex, (u32)(s->w * s->h * 4));
	GX_InvalidateTexAll();
	s->has_tex = 1;
}

/* EFB -> tex (sin esperar a la GPU) */
static void efb_to_tex(Surface *s){
	copy_filter_none();
	GX_SetTexCopySrc(0, 0, (u16)s->w, (u16)s->h);
	GX_SetTexCopyDst((u16)s->w, (u16)s->h, s->fmt == SURF_Z ? GX_TF_Z24X8 : GX_TF_RGBA8, GX_FALSE);
	GX_CopyTex(s->tex, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();
	s->has_tex = 1;
}

static void save_efb(void){
	if(cur_c && efb_c_newer){ efb_to_tex(cur_c); efb_c_newer = 0; }
	if(cur_z && efb_z_newer){ efb_to_tex(cur_z); efb_z_newer = 0; }
}

/* Contenido de la GPU -> VRAM */
static void download(Surface *s){
	if(s == cur_c && efb_c_newer){ efb_to_tex(s); efb_c_newer = 0; }
	if(s == cur_z && efb_z_newer){ efb_to_tex(s); efb_z_newer = 0; }
	if(s->has_tex){
		wait_gpu();
		tex_to_vram(s);
		prof_downloads++;
	}
	s->state = SYNCED;
}

static void drop_surface(Surface *s){
	if(s->state == GPU_NEWER) download(s);
	if(s == cur_c){ cur_c = NULL; efb_c_newer = 0; restored_c = 0; }
	if(s == cur_z){ cur_z = NULL; efb_z_newer = 0; restored_z = 0; }
	defer_free(s->tex);
	memset(s, 0, sizeof(*s));
}

/* Gancho de memory.c: la CPU va a tocar [off, off+len) de la VRAM */
static void vram_hook(core_u32 off, core_u32 len, int write){
	int i;
	for(i = 0; i < MAX_SURF; i++){
		Surface *s = &surf[i];
		core_u32 a, e;
		if(!s->used || s->state == VRAM_NEWER) continue;
		surf_range(s, &a, &e);
		if(off >= e || off + len <= a) continue;
		if(s->state == GPU_NEWER) download(s);
		if(write){
			s->state = VRAM_NEWER;
			if(s == cur_c){ restored_c = 0; efb_c_newer = 0; }
			if(s == cur_z){ restored_z = 0; efb_z_newer = 0; }
			targets_dirty = 1;
		}
	}
	recompute_watch();
}

/* --- Búferes ----------------------------------------------------------------- */

static int round4(int v){ return (v + 3) & ~3; }

static Surface *find_surface(core_u32 addr, core_u32 stride, int fmt){
	int i;
	for(i = 0; i < MAX_SURF; i++)
		if(surf[i].used && surf[i].addr == addr && surf[i].stride == stride && surf[i].fmt == fmt) return &surf[i];
	return NULL;
}

/* Búfer de la PSP de al menos need_w x need_h. NULL si no se puede. */
static Surface *get_surface(core_u32 addr, core_u32 stride, int fmt, int need_w, int need_h){
	Surface *s = find_surface(addr, stride, fmt), *slot = NULL;
	core_u32 a, e;
	int i, w, h;
	need_w = round4(need_w);
	need_h = round4(need_h);
	if(need_w > EFB_W) need_w = EFB_W;
	if(need_h > EFB_H) need_h = EFB_H;
	if(need_w > (int)(stride & ~3u)) need_w = (int)(stride & ~3u);
	if(need_w <= 0 || need_h <= 0) return NULL;

	if(s){
		if(s->w >= need_w && s->h >= need_h) return s;
		/* Crece: la VRAM pasa a mandar y se vuelve a subir */
		w = s->w > need_w ? s->w : need_w;
		h = s->h > need_h ? s->h : need_h;
		drop_surface(s);
	} else {
		w = need_w;
		h = need_h;
	}

	/* Lo que se solape con él deja de existir en la GPU */
	{
		Surface tmp;
		memset(&tmp, 0, sizeof(tmp));
		tmp.addr = addr; tmp.stride = stride; tmp.fmt = fmt; tmp.h = h;
		surf_range(&tmp, &a, &e);
		if(fmt == SURF_Z){ a = addr; e = addr + stride * (core_u32)h * 2; }
	}
	for(i = 0; i < MAX_SURF; i++){
		Surface *o = &surf[i];
		core_u32 oa, oe;
		if(!o->used) { if(!slot) slot = o; continue; }
		oa = o->addr;
		oe = o->addr + o->stride * (core_u32)o->h * (core_u32)surf_bpp(o->fmt);
		if(a < oe && oa < e) drop_surface(o);
		if(!o->used && !slot) slot = o;
	}
	if(!slot){
		/* Sin sitio: fuera el primero que no esté en el EFB */
		for(i = 0; i < MAX_SURF && !slot; i++)
			if(&surf[i] != cur_c && &surf[i] != cur_z){ drop_surface(&surf[i]); slot = &surf[i]; }
		if(!slot) return NULL;
	}
	slot->tex = memalign(32, (size_t)(w * h * 4));
	if(!slot->tex) return NULL;
	slot->used = 1;
	slot->addr = addr;
	slot->stride = stride;
	slot->fmt = fmt;
	slot->w = w;
	slot->h = h;
	slot->has_tex = 0;
	slot->state = VRAM_NEWER;
	return slot;
}

/* --- Modos de GX ------------------------------------------------------------- */

static void load_pixel_projection(float minz, float maxz){
	Mtx44 m;
	float range = maxz - minz;
	memset(m, 0, sizeof(m));
	if(range < 1.0f) range = 1.0f;
	/* x' = X / 512 - 1, y' = 1 - Y / 512: el viewport de 1024 lo devuelve a
	   píxeles. z' = (Z - maxz) / (maxz - minz) en [-1, 0]. */
	m[0][0] = 2.0f / VP; m[0][3] = -1.0f;
	m[1][1] = -2.0f / VP; m[1][3] = 1.0f;
	m[2][2] = 1.0f / range; m[2][3] = -(minz + range) / range;
	m[3][3] = 1.0f;
	GX_LoadProjectionMtx(m, GX_ORTHOGRAPHIC);
	GX_SetViewport(0, 0, VP, VP, minz * 256.0f / 16777215.0f, (minz + range) * 256.0f / 16777215.0f);
}

static void set_mode(int m){
	if(mode == m) return;
	mode = m;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	if(m == MODE_EMU){
		GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
		GX_SetNumChans(2);
		GX_SetNumTexGens(2);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_NRM, GX_IDENTITY);
		GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX0);
	} else {
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetNumChans(0);
		GX_SetNumTexGens(1);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	}
	gx_dirty = 1;
}

/* Dibuja una copia (color y/o profundidad) en (x0,y0)-(x1,y1) del EFB */
static void blit(const Surface *c, const Surface *z, int color, int alpha, float x0, float y0, float x1, float y1,
                 float u1, float v1, int linear, int const_alpha){
	static GXTexObj co, zo;
	int stages = 0;
	set_mode(MODE_BLIT);
	load_pixel_projection(0.0f, 65535.0f);
	GX_SetScissor(0, 0, EFB_W, EFB_H);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetColorUpdate(color ? GX_TRUE : GX_FALSE);
	GX_SetAlphaUpdate(alpha ? GX_TRUE : GX_FALSE);
	GX_SetDstAlpha(GX_FALSE, 0);
	GX_SetDither(GX_FALSE);
	if(c){
		GX_InitTexObj(&co, c->tex, (u16)c->w, (u16)c->h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&co, linear ? GX_LINEAR : GX_NEAR, linear ? GX_LINEAR : GX_NEAR);
		GX_LoadTexObj(&co, GX_TEXMAP0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		if(const_alpha >= 0){
			GX_SetTevKColor(GX_KCOLOR3, (GXColor){ 0, 0, 0, (u8)const_alpha });
			GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K3_A);
			GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		}
		stages = 1;
	}
	if(z){
		u8 st = (u8)(GX_TEVSTAGE0 + stages);
		GX_InitTexObj(&zo, z->tex, (u16)z->w, (u16)z->h, GX_TF_Z24X8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&zo, GX_NEAR, GX_NEAR);
		GX_LoadTexObj(&zo, GX_TEXMAP1);
		GX_SetTevOrder(st, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
		if(stages) GX_SetTevOp(st, GX_PASSCLR);
		else GX_SetTevOp(st, GX_REPLACE);
		GX_SetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
		GX_SetZCompLoc(GX_FALSE);
		GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
		stages++;
	} else {
		GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	}
	if(!stages){
		/* Solo un color (negro) */
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		stages = 1;
	}
	GX_SetNumTevStages((u8)stages);

	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
	GX_Position3f32(x0, y0, 0.0f); GX_TexCoord2f32(0.0f, 0.0f);
	GX_Position3f32(x1, y0, 0.0f); GX_TexCoord2f32(u1, 0.0f);
	GX_Position3f32(x1, y1, 0.0f); GX_TexCoord2f32(u1, v1);
	GX_Position3f32(x0, y1, 0.0f); GX_TexCoord2f32(0.0f, v1);
	GX_End();

	if(z){
		GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
	gx_dirty = 1;
}

/* --- Restaurar el EFB ---------------------------------------------------------- */

static void restore_color(void){
	Surface *s = cur_c;
	if(s->state == VRAM_NEWER || !s->has_tex){
		wait_gpu();
		vram_to_tex(s);
		s->state = SYNCED;
		recompute_watch();
	}
	set_pf(surface_pf(s));
	blit(s, NULL, 1, s->amode == A_GPU, 0.0f, 0.0f, (float)s->w, (float)s->h, 1.0f, 1.0f, 0, -1);
	restored_c = 1;
	efb_c_newer = 0;
}

static void restore_depth(void){
	Surface *s = cur_z;
	if(s->state == VRAM_NEWER || !s->has_tex){
		wait_gpu();
		vram_to_tex(s);
		s->state = SYNCED;
		recompute_watch();
	}
	blit(NULL, s, 0, 0, 0.0f, 0.0f, (float)s->w, (float)s->h, 1.0f, 1.0f, 0, -1);
	restored_z = 1;
	efb_z_newer = 0;
}

static void bind_color(Surface *s){
	int pf;
	if(cur_c == s) return;
	if(cur_c && efb_c_newer){ efb_to_tex(cur_c); efb_c_newer = 0; }
	cur_c = s;
	restored_c = 0;
	pf = surface_pf(s);
	set_pf(pf);
}

/* El búfer en el EFB pasa a tener alfa por píxel (RGB8 -> RGBA6) */
static void promote_alpha(void){
	Surface *s = cur_c;
	int const_alpha = s->ualpha;
	if(s->amode == A_GPU || !has_alpha_fmt(s->fmt)) return;
	efb_to_tex(s);
	if(s->amode == A_VRAM){
		/* El color del EFB con el alfa de la VRAM */
		int x, y, w = visible_w(s);
		wait_gpu();
		DCInvalidateRange(s->tex, (u32)(s->w * s->h * 4));
		for(y = 0; y < s->h; y++)
			for(x = 0; x < w; x++){
				core_u8 c[4];
				const u8 *p = vram_color(s, x, y, surf_bpp(s->fmt));
				core_u32 raw = s->fmt == GE_FMT_8888 ? (core_u32)p[3] << 24 : (core_u32)p[0] | ((core_u32)p[1] << 8);
				ge_decode_color((core_u32)s->fmt, raw, c);
				tile_ptr(s->tex, s->w, x, y)[0] = c[3];
			}
		DCFlushRange(s->tex, (u32)(s->w * s->h * 4));
		GX_InvalidateTexAll();
		const_alpha = -1;
	}
	s->amode = A_GPU;
	set_pf(GX_PF_RGBA6_Z24);
	blit(s, NULL, 1, 1, 0.0f, 0.0f, (float)s->w, (float)s->h, 1.0f, 1.0f, 0, const_alpha);
	efb_c_newer = 1;
}

static void bind_depth(Surface *s){
	if(cur_z == s) return;
	if(cur_z && efb_z_newer){ efb_to_tex(cur_z); efb_z_newer = 0; }
	cur_z = s;
	restored_z = 0;
}

/* --- Caché de texturas -------------------------------------------------------- */

static core_u32 hash_bytes(const u8 *p, u32 n, core_u32 h){
	u32 i;
	for(i = 0; i + 4 <= n; i += 4){
		h ^= (core_u32)p[i] | ((core_u32)p[i + 1] << 8) | ((core_u32)p[i + 2] << 16) | ((core_u32)p[i + 3] << 24);
		h = (h << 5 | h >> 27) * 0x9E3779B1u;
	}
	for(; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
	return h;
}

static u32 texture_bytes(int fmt, int bufw, int h){
	switch(fmt){
	case GE_TFMT_CLUT4: case GE_TFMT_DXT1: return (u32)(bufw * h) / 2;
	case GE_TFMT_CLUT8: case GE_TFMT_DXT3: case GE_TFMT_DXT5: return (u32)(bufw * h);
	case GE_FMT_8888: case GE_TFMT_CLUT32: return (u32)(bufw * h) * 4;
	default: return (u32)(bufw * h) * 2;
	}
}

/* Bytes accesibles desde addr (hasta el final de su región) */
static u32 available_bytes(core_u32 addr, u32 want){
	core_u32 a = addr & PSP_ADDR_MASK, end;
	if(a >= PSP_RAM_BASE) end = PSP_RAM_BASE + psp_mem.ram_size;
	else if(a >= PSP_VRAM_BASE && a < PSP_VRAM_MIRROR_END && !(((a - PSP_VRAM_BASE) >> 21) & 1))
		end = (a & ~(PSP_VRAM_SIZE - 1)) + PSP_VRAM_SIZE;
	else return 0;
	if(a >= end) return 0;
	return end - a < want ? end - a : want;
}

static void free_texture(TexEntry *e){
	if(e->data){
		defer_free(e->data);
		tex_total -= e->bytes;
	}
	if(e == last_tex) last_tex = NULL;
	if(e == cur_tex) cur_tex = NULL;
	memset(e, 0, sizeof(*e));
}

static TexEntry *alloc_texture_slot(void){
	int i, oldest = -1;
	for(i = 0; i < MAX_TEX; i++){
		if(!texc[i].used) return &texc[i];
		if(texc[i].used_frame != frame && (oldest < 0 || texc[i].used_frame < texc[oldest].used_frame)) oldest = i;
	}
	if(oldest < 0) oldest = 0;
	free_texture(&texc[oldest]);
	return &texc[oldest];
}

static void trim_textures(void){
	while(tex_total > TEX_BUDGET){
		int i, oldest = -1;
		for(i = 0; i < MAX_TEX; i++)
			if(texc[i].used && texc[i].used_frame != frame && &texc[i] != cur_tex &&
			   (oldest < 0 || texc[i].used_frame < texc[oldest].used_frame)) oldest = i;
		if(oldest < 0) break;
		free_texture(&texc[oldest]);
	}
}

static core_u32 decode_buf[MAX_TEX_SIZE * MAX_TEX_SIZE];
static u8 linear_buf[MAX_TEX_SIZE * MAX_TEX_SIZE * 4];

static inline core_u32 rd16le(const u8 *p){ return (core_u32)p[0] | ((core_u32)p[1] << 8); }
static inline core_u32 rd32le(const u8 *p){ return (core_u32)p[0] | ((core_u32)p[1] << 8) | ((core_u32)p[2] << 16) | ((core_u32)p[3] << 24); }

/* Color de la PSP (rojo en el byte bajo) -> texel RGBA8 de GX */
static inline void put_rgba8(u8 *t, core_u32 c){
	t[0] = (u8)(c >> 24); t[1] = (u8)c; t[32] = (u8)(c >> 8); t[33] = (u8)(c >> 16);
}

static inline core_u32 psp16_to_8888(int fmt, core_u32 v){
	core_u32 r, g, b, a;
	switch(fmt){
	case GE_FMT_565:
		r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31;
		return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
	case GE_FMT_5551:
		r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31;
		return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | ((v & 0x8000) ? 0xFF000000u : 0);
	default:
		r = v & 15; g = (v >> 4) & 15; b = (v >> 8) & 15; a = v >> 12;
		return (r * 0x11) | ((g * 0x11) << 8) | ((b * 0x11) << 16) | ((a * 0x11) << 24);
	}
}

/* Bloques de 16 bytes x 8 filas -> lineal (pitch en bytes) */
static void unswizzle(const u8 *src, u8 *dst, u32 pitch, int rows){
	u32 bx, r, blocks = pitch / 16;
	int by;
	for(by = 0; by < (rows + 7) / 8; by++)
		for(bx = 0; bx < blocks; bx++)
			for(r = 0; r < 8; r++){
				int y = by * 8 + (int)r;
				if(y >= rows) break;
				memcpy(dst + (u32)y * pitch + bx * 16, src + ((u32)by * blocks + bx) * 128 + r * 16, 16);
			}
}

/* Decodifica en data (tiles de GX). Formatos habituales con lectura
   directa; el resto texel a texel como el muestreador. Devuelve el formato
   de GX usado. */
static int decode_into(const GeRasterState *r, int level, u8 *data, int w, int h, int tw, int allow565){
	int fmt = r->texfmt, bits, x, y;
	u32 bufw = r->texbufw[level], pitch, bytes;
	const u8 *src;

	switch(fmt){
	case GE_FMT_565: case GE_FMT_5551: case GE_FMT_4444: case GE_TFMT_CLUT16: bits = 16; break;
	case GE_FMT_8888: case GE_TFMT_CLUT32: bits = 32; break;
	case GE_TFMT_CLUT8: bits = 8; break;
	case GE_TFMT_CLUT4: bits = 4; break;
	default: bits = 0; break;
	}
	pitch = bufw * (u32)bits / 8;
	bytes = pitch * (u32)((h + 7) & ~7);
	src = bits && r->texvalid[level] && available_bytes(r->texaddr[level], bytes) == bytes && bytes <= sizeof(linear_buf)
	      ? mem_ptr_r(r->texaddr[level], bytes) : NULL;
	if(!src || (fmt >= GE_TFMT_CLUT16 && fmt <= GE_TFMT_CLUT32) || (int)bufw < w || (level && !r->use_shared_clut)){
		/* Genérico */
		ge_texture_decode(level, decode_buf, w, h, w);
		for(y = 0; y < h; y++)
			for(x = 0; x < w; x++) put_rgba8(tile_ptr(data, tw, x, y), decode_buf[y * w + x]);
		return GX_TF_RGBA8;
	}
	if(r->swizzle){
		unswizzle(src, linear_buf, pitch, h);
		src = linear_buf;
	}

	if(fmt == GE_FMT_565 && allow565){
		/* RGB565 de GX: mismos bits con rojo arriba, big-endian */
		for(y = 0; y < h; y++){
			const u8 *row = src + (u32)y * pitch;
			for(x = 0; x < w; x++){
				core_u32 v = rd16le(row + (u32)x * 2);
				u8 *t = data + ((((y >> 2) * (tw >> 2) + (x >> 2)) << 5) | ((((y & 3) << 2) | (x & 3)) << 1));
				v = ((v & 31) << 11) | (v & 0x07E0) | ((v >> 11) & 31);
				t[0] = (u8)(v >> 8);
				t[1] = (u8)v;
			}
		}
		return GX_TF_RGB565;
	}
	if(fmt == GE_TFMT_CLUT4 || fmt == GE_TFMT_CLUT8){
		/* Paleta ya convertida (con desplazamiento, máscara y offset) */
		static core_u32 pal[256];
		int n = fmt == GE_TFMT_CLUT4 ? 16 : 256, i;
		u32 shift = (r->clutformat >> 2) & 0x1F, mask = (r->clutformat >> 8) & 0xFF;
		u32 offset = ((r->clutformat >> 16) & 0x1F) << 4, offset_mask = r->clut_fmt == GE_FMT_8888 ? 0xFF : 0x1FF;
		for(i = 0; i < n; i++){
			u32 idx = (r->has_clut_shift || r->has_clut_mask || r->has_clut_offset)
			          ? ((((u32)i >> shift) & mask) | (offset & offset_mask)) : (u32)i;
			pal[i] = r->clut_fmt == GE_FMT_8888 ? rd32le(ge.clut + (idx & 0x1FF) * 4)
			                                    : psp16_to_8888(r->clut_fmt, rd16le(ge.clut + (idx & 0x3FF) * 2));
		}
		for(y = 0; y < h; y++){
			const u8 *row = src + (u32)y * pitch;
			if(fmt == GE_TFMT_CLUT8)
				for(x = 0; x < w; x++) put_rgba8(tile_ptr(data, tw, x, y), pal[row[x]]);
			else
				for(x = 0; x < w; x++) put_rgba8(tile_ptr(data, tw, x, y), pal[(row[x >> 1] >> ((x & 1) * 4)) & 15]);
		}
		return GX_TF_RGBA8;
	}
	for(y = 0; y < h; y++){
		const u8 *row = src + (u32)y * pitch;
		if(fmt == GE_FMT_8888)
			for(x = 0; x < w; x++){
				const u8 *p = row + (u32)x * 4;
				u8 *t = tile_ptr(data, tw, x, y);
				t[0] = p[3]; t[1] = p[0]; t[32] = p[1]; t[33] = p[2];
			}
		else
			for(x = 0; x < w; x++) put_rgba8(tile_ptr(data, tw, x, y), psp16_to_8888(fmt, rd16le(row + (u32)x * 2)));
	}
	return GX_TF_RGBA8;
}

/* Bytes de un nivel en tiles de 4x4 de 32 bits */
static u32 level_bytes(int w, int h){ return (u32)(round4(w) * round4(h) * 4); }

static int decode_texture(TexEntry *e, const GeRasterState *r){
	int w = e->w, h = e->h, gxfmt = GX_TF_RGBA8, i;
	u32 bytes = 0, off = 0;
	u8 *data;
	for(i = 0; i < e->levels; i++){
		int lw = w >> i, lh = h >> i;
		bytes += level_bytes(lw ? lw : 1, lh ? lh : 1);
	}
	data = memalign(32, bytes);
	if(!data) return 0;
	memset(data, 0, bytes);
	for(i = 0; i < e->levels; i++){
		int lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
		gxfmt = decode_into(r, i, data + off, lw, lh, round4(lw), e->levels == 1);
		off += level_bytes(lw, lh);
	}
	if(gxfmt == GX_TF_RGB565) bytes /= 2;
	DCFlushRange(data, bytes);
	e->data = data;
	e->bytes = bytes;
	tex_total += bytes;
	GX_InitTexObj(&e->obj, data, (u16)w, (u16)h, (u8)gxfmt, GX_REPEAT, GX_REPEAT, e->levels > 1 ? GX_TRUE : GX_FALSE);
	GX_InvalidateTexAll();
	return 1;
}

/* Textura actual del GE (estado de ge_raster_begin) */
static TexEntry *lookup_texture(const GeRasterState *r){
	core_u32 addr = r->texaddr[0], fmt = (core_u32)r->texfmt | ((core_u32)r->swizzle << 8);
	int ws = r->width0_shift, hs = r->height0_shift, w, h, bufw = r->texbufw[0], i;
	core_u32 size, clutfmt = 0, clut_hash = 0, hash;
	u32 want, avail;
	const u8 *p;
	TexEntry *e = NULL;

	int levels = 1;
	core_u32 mipkey = 0;
	if(ws > 9) ws = 9;
	if(hs > 9) hs = 9;
	w = 1 << ws;
	h = 1 << hs;
	size = (core_u32)w | ((core_u32)h << 16);
	/* Mipmaps: los niveles que van a la mitad, como los quiere GX */
	for(i = 1; i <= r->max_tex_level; i++){
		int lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
		if(!r->texvalid[i] || r->size_w[i] != lw || r->size_h[i] != lh) break;
		levels = i + 1;
		mipkey = (mipkey ^ r->texaddr[i] ^ ((core_u32)r->texbufw[i] << 20)) * 0x9E3779B1u;
	}
	if(r->texfmt >= GE_TFMT_CLUT4 && r->texfmt <= GE_TFMT_CLUT32){
		clutfmt = r->clutformat;
		clut_hash = hash_bytes(ge.clut, ge.clut_bytes ? ge.clut_bytes : 1024, 0x12345u);
	}

	/* Misma textura que la anterior y nada la ha podido cambiar */
	if(last_tex && !tex_flushed && last_clut_gen == ge.clut_gen && last_tex->addr == addr && last_tex->fmt == fmt &&
	   last_tex->size == size && last_tex->bufw == (core_u32)bufw && last_tex->clutfmt == clutfmt &&
	   last_tex->checked_frame == frame && last_tex->clut_hash == clut_hash && last_tex->levels == levels &&
	   last_tex->mipkey == mipkey)
		return last_tex;

	hash = 0x811C9DC5u;
	for(i = 0; i < levels; i++){
		int lh = h >> i ? h >> i : 1;
		want = texture_bytes(r->texfmt, r->texbufw[i], lh);
		avail = available_bytes(r->texaddr[i], want);
		p = avail ? mem_ptr_r(r->texaddr[i], avail) : NULL;
		/* Sin puntero contiguo (espejos con swizzle): se decodifica cada cuadro */
		hash = p ? hash_bytes(p, avail, hash) : hash ^ frame * 0x9E3779B1u;
	}

	for(i = 0; i < MAX_TEX; i++){
		TexEntry *t = &texc[i];
		if(t->used && t->addr == addr && t->fmt == fmt && t->size == size && t->bufw == (core_u32)bufw &&
		   t->clutfmt == clutfmt && t->clut_hash == clut_hash && t->hash == hash && t->levels == levels &&
		   t->mipkey == mipkey){ e = t; break; }
	}
	if(!e){
		e = alloc_texture_slot();
		e->used = 1;
		e->addr = addr; e->fmt = fmt; e->size = size; e->bufw = (core_u32)bufw;
		e->clutfmt = clutfmt; e->clut_hash = clut_hash; e->hash = hash;
		e->levels = levels; e->mipkey = mipkey;
		e->w = w; e->h = h;
		prof_decodes++;
		if(!decode_texture(e, r)){ memset(e, 0, sizeof(*e)); return NULL; }
		trim_textures();
	}
	e->checked_frame = frame;
	e->used_frame = frame;
	tex_flushed = 0;
	last_clut_gen = ge.clut_gen;
	last_tex = e;
	return e;
}

/* La textura es un framebuffer que está en la GPU: se usa su copia sin
   pasar por la VRAM (efectos que dibujan en un búfer y lo usan después) */
static int texture_from_surface(const GeRasterState *r){
	core_u32 addr = r->texaddr[0] & PSP_ADDR_MASK;
	Surface *ts;
	if(r->texfmt > GE_FMT_8888 || r->swizzle || addr < PSP_VRAM_BASE || addr >= PSP_VRAM_BASE + PSP_VRAM_SIZE) return 0;
	ts = find_surface(addr - PSP_VRAM_BASE, r->texbufw[0], r->texfmt);
	if(!ts || ts->state == VRAM_NEWER) return 0;
	/* El alfa del búfer solo está en la GPU en modo A_GPU */
	if(r->use_tex_alpha && ts->amode != A_GPU && !(ts->amode == A_UNIFORM && ts->ualpha == 255) && has_alpha_fmt(ts->fmt))
		return 0;
	if(ts == cur_c && efb_c_newer){ efb_to_tex(ts); efb_c_newer = 0; }
	if(!ts->has_tex) return 0;
	GX_InitTexObj(&rtt_obj, ts->tex, (u16)ts->w, (u16)ts->h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	tex_su = (float)(1 << r->width0_shift) / (float)ts->w;
	tex_sv = (float)(1 << r->height0_shift) / (float)ts->h;
	return 1;
}

/* --- Estado de GX a partir del GE ---------------------------------------------- */

static void setup_targets(void){
	const GeRasterState *r = ge_raster_state();
	core_u32 fb = ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0, fbw = ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FC;
	core_u32 zb = ge.cmd[GE_ZBUFPTR] & 0x1FFFF0, zbw = ge.cmd[GE_ZBUFWIDTH] & 0x7FC;
	int fmt = (int)(ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3);
	int need_w = r->sc_x2 / 16 + 1, need_h = r->sc_y2 / 16 + 1;
	core_u32 mrgb = ge.cmd[GE_MASKRGB] & 0xFFFFFF, ma = ge.cmd[GE_MASKALPHA] & 0xFF;
	int has_alpha = fmt != GE_FMT_565, z_used;
	Surface *c, *z = NULL;

	skip_draws = 1;
	stencil_val = -1;
	reads_dst_alpha = 0;
	if(!r->clear_mode && r->alpha_blend){
		int sf = r->blend_src, df = r->blend_dst;
		reads_dst_alpha = sf == BF_DSTALPHA || sf == BF_INVDSTALPHA || sf == BF_DOUBLEDSTALPHA || sf == BF_DOUBLEINVDSTALPHA ||
		                  df == BF_DSTALPHA || df == BF_INVDSTALPHA || df == BF_DOUBLEDSTALPHA || df == BF_DOUBLEINVDSTALPHA;
	}
	if(r->apply_depth_range && r->minz > r->maxz) return;
	if(r->sc_x2 < r->sc_x1 || r->sc_y2 < r->sc_y1) return;

	/* Qué se toca */
	if(r->clear_mode){
		writes_c = (r->color_test && mrgb != 0xFFFFFF) || (r->stencil_test && has_alpha && ma != 0xFF);
		writes_z = r->depth_write;
		z_used = writes_z;
	} else {
		int stencil_w = r->stencil_test && has_alpha && ma != 0xFF && (r->zpass == SOP_ZERO || r->zpass == SOP_REPLACE);
		stencil_val = -1;
		if(stencil_w)
			stencil_val = r->zpass == SOP_ZERO ? 0 : ((r->has_stencil_test_mask ? r->stencil_ref : r->stencil_test_ref) & 0xFF);
		writes_c = mrgb != 0xFFFFFF || stencil_w;
		writes_z = r->depth_write;
		z_used = r->depth_test_func != CMP_ALWAYS || r->depth_write;
	}
	touch_c = writes_c;
	touch_z = z_used;
	if(!writes_c && !writes_z) return;

	c = get_surface(fb, fbw, fmt, need_w, need_h);
	if(!c) return;
	if(r->sc_x1 / 16 >= c->w || r->sc_y1 / 16 >= c->h) return;
	bind_color(c);
	if(z_used){
		core_u32 ca, ce, za = zb, ze = zb + zbw * (core_u32)round4(need_h) * 2;
		ca = c->addr;
		ce = c->addr + c->stride * (core_u32)c->h * (core_u32)surf_bpp(c->fmt);
		/* Profundidad encima del propio framebuffer: no se emula */
		if(zbw && !(za < ce && ca < ze)) z = get_surface(zb, zbw, SURF_Z, need_w, need_h);
		if(!z || z == c || !c->used){
			touch_z = writes_z = 0;
			z = NULL;
			if(!c->used) return;
		}
	}
	if(z) bind_depth(z);
	skip_draws = 0;

	through = (ge.cmd[GE_VERTEXTYPE] >> 23) & 1;
	tex_on = r->enable_textures;
	cur_tex = NULL;
	use_rtt = 0;
	tex_su = tex_sv = 1.0f;
	if(!writes_c) tex_on = 0;
	if(tex_on){
		use_rtt = r->texvalid[0] && texture_from_surface(r);
		if(!use_rtt) cur_tex = r->texvalid[0] ? lookup_texture(r) : NULL;
		inv_tw = tex_su / (float)(1 << r->width0_shift);
		inv_th = tex_sv / (float)(1 << r->height0_shift);
		tex_proj = r->texture_proj;
	}
	flat = !r->shade_gouraud;
}

static int nst;
static u8 next_bias = GX_TB_ZERO;

static void tev_stage(u8 tc, u32 map, u8 chan, u8 a, u8 b, u8 c, u8 d, u8 cscale,
                      u8 aa, u8 ab, u8 ac, u8 ad, u8 ksel, u8 kasel){
	u8 st = (u8)(GX_TEVSTAGE0 + nst++);
	GX_SetTevOrder(st, tc, map, chan);
	GX_SetTevColorIn(st, a, b, c, d);
	GX_SetTevColorOp(st, GX_TEV_ADD, next_bias, cscale, GX_TRUE, GX_TEVPREV);
	next_bias = GX_TB_ZERO;
	GX_SetTevAlphaIn(st, aa, ab, ac, ad);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKColorSel(st, ksel);
	GX_SetTevKAlphaSel(st, kasel);
}

static GXColor rgb_color(core_u32 c, u8 a){
	GXColor g = { (u8)c, (u8)(c >> 8), (u8)(c >> 16), a };
	return g;
}

/* Factor de mezcla de la PSP -> GX. pre: qué premultiplica al color en el
   TEV (0 nada, 1 alfa, 2 1-alfa, 3 constante) */
static u8 src_factor(int f, core_u32 fix, int *pre, int *dbl){
	switch(f){
	case BF_OTHERCOLOR:       return GX_BL_DSTCLR;
	case BF_INVOTHERCOLOR:    return GX_BL_INVDSTCLR;
	case BF_SRCALPHA:         return GX_BL_SRCALPHA;
	case BF_INVSRCALPHA:      return GX_BL_INVSRCALPHA;
	case BF_DSTALPHA:         return GX_BL_DSTALPHA;
	case BF_INVDSTALPHA:      return GX_BL_INVDSTALPHA;
	case BF_DOUBLESRCALPHA:   *pre = 1; *dbl = 1; return GX_BL_ONE;
	case BF_DOUBLEINVSRCALPHA: return GX_BL_INVSRCALPHA;
	case BF_DOUBLEDSTALPHA:   return GX_BL_DSTALPHA;
	case BF_DOUBLEINVDSTALPHA: return GX_BL_INVDSTALPHA;
	default:
		if(fix == 0) return GX_BL_ZERO;
		if(fix == 0xFFFFFF) return GX_BL_ONE;
		*pre = 3;
		return GX_BL_ONE;
	}
}

static u8 dst_factor(int f){
	switch(f){
	case BF_OTHERCOLOR:        return GX_BL_SRCCLR;
	case BF_INVOTHERCOLOR:     return GX_BL_INVSRCCLR;
	case BF_SRCALPHA: case BF_DOUBLESRCALPHA:       return GX_BL_SRCALPHA;
	case BF_INVSRCALPHA: case BF_DOUBLEINVSRCALPHA: return GX_BL_INVSRCALPHA;
	case BF_DSTALPHA: case BF_DOUBLEDSTALPHA:       return GX_BL_DSTALPHA;
	default:                   return GX_BL_INVDSTALPHA;
	}
}

/* Un factor que lee el alfa del destino cuando ese alfa es el mismo en todo
   el búfer: es una constante */
static int const_alpha_factor(int f, int a, core_u32 *fix){
	int v;
	switch(f){
	case BF_DSTALPHA:          v = a; break;
	case BF_INVDSTALPHA:       v = 255 - a; break;
	case BF_DOUBLEDSTALPHA:    v = 2 * a > 255 ? 255 : 2 * a; break;
	case BF_DOUBLEINVDSTALPHA: v = 255 - 2 * a < 0 ? 0 : 255 - 2 * a; break;
	default: return f;
	}
	*fix = (core_u32)v * 0x010101u;
	return BF_FIX;
}

static void load_dither(const GeRasterState *r){
	if(!dither_ready || memcmp(last_dither, r->dither, sizeof(last_dither))){
		u8 *t;
		int i;
		dither_idx = (dither_idx + 1) & 7;
		if(!dither_idx) wait_gpu();
		t = dither_tex[dither_idx];
		for(i = 0; i < 16; i++){
			u8 *p = tile_ptr(t, 4, i & 3, i >> 2);
			p[0] = 255;
			p[1] = p[32] = p[33] = (u8)(128 + r->dither[i]);
		}
		DCFlushRange(t, 64);
		GX_InitTexObj(&dither_obj, t, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
		GX_InitTexObjFilterMode(&dither_obj, GX_NEAR, GX_NEAR);
		GX_InvalidateTexAll();
		memcpy(last_dither, r->dither, sizeof(last_dither));
		dither_ready = 1;
	}
	GX_LoadTexObj(&dither_obj, GX_TEXMAP2);
}

static void apply_gx(void){
	const GeRasterState *r = ge_raster_state();
	core_u32 mrgb = ge.cmd[GE_MASKRGB] & 0xFFFFFF, ma = ge.cmd[GE_MASKALPHA] & 0xFF;
	int has_alpha = cur_c && cur_c->fmt != GE_FMT_565;
	int cw, aw, dsta = -1, x1, y1, x2, y2;
	int minz = r->apply_depth_range ? r->minz : 0, maxz = r->apply_depth_range ? r->maxz : 65535;
	int pre = 0, dbl = 0, alpha_k = -1;
	core_u32 blend_fix_a = r->blend_fix_a;
	u8 btype = GX_BM_NONE, bsrc = GX_BL_ONE, bdst = GX_BL_ZERO, lop = GX_LO_COPY;
	int use_sec, alpha_test;

	set_mode(MODE_EMU);
	load_pixel_projection((float)minz, (float)maxz);

	x1 = r->sc_x1 / 16; y1 = r->sc_y1 / 16;
	x2 = r->sc_x2 / 16; y2 = r->sc_y2 / 16;
	if(x2 >= cur_c->w) x2 = cur_c->w - 1;
	if(y2 >= cur_c->h) y2 = cur_c->h - 1;
	if(x1 > x2 || y1 > y2){ x1 = y1 = 0; x2 = y2 = -1; }
	GX_SetScissor((u32)x1, (u32)y1, (u32)(x2 - x1 + 1), (u32)(y2 - y1 + 1));

	/* Escrituras */
	if(r->clear_mode){
		cw = r->color_test && mrgb != 0xFFFFFF;
		aw = r->stencil_test && has_alpha && ma != 0xFF;
	} else {
		cw = mrgb != 0xFFFFFF;
		aw = 0;
		if(r->stencil_test && has_alpha && ma != 0xFF){
			int replace = r->has_stencil_test_mask ? r->stencil_ref : r->stencil_test_ref;
			if(r->zpass == SOP_ZERO){ aw = 1; dsta = 0; }
			else if(r->zpass == SOP_REPLACE){ aw = 1; dsta = replace & 0xFF; }
		}
	}
	GX_SetColorUpdate(cw ? GX_TRUE : GX_FALSE);
	GX_SetAlphaUpdate(aw ? GX_TRUE : GX_FALSE);

	/* Mezcla */
	if(!r->clear_mode && r->apply_logic_op){
		btype = GX_BM_LOGIC;
		lop = (u8)r->logic_op;
	} else if(!r->clear_mode && r->alpha_blend){
		int sf = r->blend_src, df = r->blend_dst;
		core_u32 fa = r->blend_fix_a, fb = r->blend_fix_b;
		int dst_a = !has_alpha ? 0 : cur_c->amode == A_UNIFORM ? cur_c->ualpha : -1;
		if(dst_a >= 0){
			sf = const_alpha_factor(sf, dst_a, &fa);
			df = const_alpha_factor(df, dst_a, &fb);
		}
		blend_fix_a = fa;
		switch(r->blend_eq){
		case BEQ_ADD:
			btype = GX_BM_BLEND;
			bsrc = src_factor(sf, fa, &pre, &dbl);
			if(df == BF_FIX){
				if(fb == 0) bdst = GX_BL_ZERO;
				else if(fb == 0xFFFFFF) bdst = GX_BL_ONE;
				else {
					/* Constante en el alfa de la fuente; lo que usaba ese alfa
					   se premultiplica antes */
					int k = (int)(((fb & 0xFF) + ((fb >> 8) & 0xFF) + ((fb >> 16) & 0xFF)) / 3);
					if(bsrc == GX_BL_SRCALPHA){ pre = 1; bsrc = GX_BL_ONE; }
					else if(bsrc == GX_BL_INVSRCALPHA){ pre = 2; bsrc = GX_BL_ONE; }
					alpha_k = 255 - k;
					bdst = GX_BL_INVSRCALPHA;
				}
			} else bdst = dst_factor(df);
			break;
		case BEQ_REVSUB:
			/* GX solo resta destino - fuente, sin factores */
			btype = GX_BM_SUBTRACT;
			src_factor(sf, fa, &pre, &dbl);
			if(sf == BF_SRCALPHA) pre = 1;
			else if(sf == BF_INVSRCALPHA) pre = 2;
			break;
		case BEQ_SUB:
			btype = GX_BM_BLEND;
			bsrc = src_factor(sf, fa, &pre, &dbl);
			bdst = GX_BL_ZERO;
			break;
		case BEQ_MAX:
			btype = GX_BM_BLEND; bsrc = GX_BL_ONE; bdst = GX_BL_ONE;
			break;
		case BEQ_ABSDIFF:
			btype = GX_BM_SUBTRACT;
			break;
		default:
			break;
		}
	}
	GX_SetBlendMode(btype, bsrc, bdst, lop);

	/* TEV */
	nst = 0;
	/* Color secundario: con luces y especular separado, o de un vértice
	   inmediato (VSCV) aunque las luces estén apagadas */
	use_sec = !r->clear_mode && !through && (ge.cmd[GE_LIGHTMODE] & 1);
	if(r->clear_mode || !tex_on){
		tev_stage(GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0,
		          GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_CS_SCALE_1,
		          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
	} else {
		int rgba = r->use_tex_alpha;
		u8 sc = r->color_doubling ? GX_CS_SCALE_2 : GX_CS_SCALE_1;
		u8 aa = GX_CA_ZERO, ab = rgba ? GX_CA_TEXA : GX_CA_ZERO, ac = rgba ? GX_CA_RASA : GX_CA_ZERO, ad = rgba ? GX_CA_ZERO : GX_CA_RASA;
		if(use_rtt){
			GX_InitTexObjFilterMode(&rtt_obj, r->min_filt ? GX_LINEAR : GX_NEAR, r->mag_filt ? GX_LINEAR : GX_NEAR);
			GX_LoadTexObj(&rtt_obj, GX_TEXMAP0);
		} else if(cur_tex){
			GX_InitTexObjWrapMode(&cur_tex->obj, r->clamp_s ? GX_CLAMP : GX_REPEAT, r->clamp_t ? GX_CLAMP : GX_REPEAT);
			if(cur_tex->levels > 1){
				/* LOD automático con el desplazamiento de la PSP; en modo
				   constante, el nivel fijo */
				float maxlod = (float)(cur_tex->levels - 1), bias = (float)r->tex_level_offset / 16.0f, minlod = 0.0f;
				u8 minf = r->mip_filt ? (r->min_filt ? GX_LIN_MIP_LIN : GX_NEAR_MIP_LIN)
				                      : (r->min_filt ? GX_LIN_MIP_NEAR : GX_NEAR_MIP_NEAR);
				if(r->tex_level_mode == LOD_CONST){
					minlod = bias < 0.0f ? 0.0f : bias > maxlod ? maxlod : bias;
					maxlod = minlod;
					bias = 0.0f;
				}
				GX_InitTexObjLOD(&cur_tex->obj, minf, r->mag_filt ? GX_LINEAR : GX_NEAR, minlod, maxlod, bias,
				                 GX_FALSE, GX_TRUE, GX_ANISO_1);
			} else
				GX_InitTexObjFilterMode(&cur_tex->obj, r->min_filt ? GX_LINEAR : GX_NEAR, r->mag_filt ? GX_LINEAR : GX_NEAR);
			GX_LoadTexObj(&cur_tex->obj, GX_TEXMAP0);
		} else GX_LoadTexObj(&empty_obj, GX_TEXMAP0);
		switch(r->tex_func){
		case TF_MODULATE:
			tev_stage(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO, sc,
			          aa, ab, ac, ad, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
			break;
		case TF_DECAL:
			if(rgba)
				tev_stage(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO, sc,
				          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
			else
				tev_stage(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, sc,
				          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
			break;
		case TF_BLEND:
			GX_SetTevKColor(GX_KCOLOR0, rgb_color(r->tex_blend_color, 255));
			tev_stage(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_RASC, GX_CC_KONST, GX_CC_TEXC, GX_CC_ZERO, sc,
			          aa, ab, ac, ad, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
			break;
		case TF_REPLACE:
			tev_stage(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, sc,
			          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, rgba ? GX_CA_TEXA : GX_CA_RASA, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
			break;
		default:
			tev_stage(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, sc,
			          aa, ab, ac, ad, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
			break;
		}
	}
	if(use_sec){
		int n = tex_on && r->color_doubling ? 2 : 1, i;
		for(i = 0; i < n; i++)
			tev_stage(GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_CS_SCALE_1,
			          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
	}
	if(!r->clear_mode && r->apply_fog){
		GX_SetTevKColor(GX_KCOLOR1, rgb_color(r->fog_color, 255));
		tev_stage(GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1, GX_CC_KONST, GX_CC_CPREV, GX_CC_RASA, GX_CC_ZERO, GX_CS_SCALE_1,
		          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV, GX_TEV_KCSEL_K1, GX_TEV_KASEL_K0_A);
	}
	if(pre || alpha_k >= 0){
		u8 ca = GX_CC_ZERO, cb = GX_CC_ZERO, cc = GX_CC_ZERO, cd = GX_CC_CPREV;
		GX_SetTevKColor(GX_KCOLOR2, rgb_color(blend_fix_a, (u8)(alpha_k >= 0 ? alpha_k : 255)));
		if(pre == 1){ cb = GX_CC_CPREV; cc = GX_CC_APREV; cd = GX_CC_ZERO; }
		else if(pre == 2){ ca = GX_CC_CPREV; cc = GX_CC_APREV; cd = GX_CC_ZERO; }
		else if(pre == 3){ cb = GX_CC_CPREV; cc = GX_CC_KONST; cd = GX_CC_ZERO; }
		tev_stage(GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL, ca, cb, cc, cd, dbl ? GX_CS_SCALE_2 : GX_CS_SCALE_1,
		          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, alpha_k >= 0 ? GX_CA_KONST : GX_CA_APREV,
		          GX_TEV_KCSEL_K2, GX_TEV_KASEL_K2_A);
	}
	if(r->dithering){
		/* Tramado de la PSP: + (textura - 128) */
		load_dither(r);
		next_bias = GX_TB_SUBHALF;
		tev_stage(GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_CS_SCALE_1,
		          GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV, GX_TEV_KCSEL_K0, GX_TEV_KASEL_K0_A);
	}
	GX_SetNumTevStages((u8)nst);

	/* Tests */
	alpha_test = !r->clear_mode && r->alpha_test_func != CMP_ALWAYS && alpha_k < 0;
	if(alpha_test){
		GX_SetAlphaCompare(cmp_map[r->alpha_test_func & 7], (u8)r->alpha_test_ref, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_FALSE);
	} else {
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
	if(!cur_z) GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	else if(r->clear_mode) GX_SetZMode(GX_TRUE, GX_ALWAYS, r->depth_write ? GX_TRUE : GX_FALSE);
	else if(r->depth_test_func == CMP_ALWAYS && !r->depth_write) GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	else GX_SetZMode(GX_TRUE, cmp_map[r->depth_test_func & 7], r->depth_write ? GX_TRUE : GX_FALSE);

	GX_SetDstAlpha(dsta >= 0 ? GX_TRUE : GX_FALSE, (u8)(dsta >= 0 ? dsta : 0));
	GX_SetDither(GX_FALSE);
}

/* Antes de cada primitiva: búferes en el EFB con su contenido. full: el
   rectángulo de borrado tapa todo el búfer de color (1) y/o de
   profundidad (2), que no hace falta restaurar. 0 = no dibujar. */
static int prepare_targets(int full){
	if(targets_dirty){
		u64 t0 = gettime();
		setup_targets();
		targets_dirty = 0;
		gx_dirty = 1;
		prof_setup += gettime() - t0;
	}
	if(skip_draws) return 0;
	if(touch_c && !restored_c){
		if(full & 1) restored_c = 1;
		else restore_color();
	}
	if(touch_z && cur_z && !restored_z){
		if(full & 2) restored_z = 1;
		else restore_depth();
	}
	return 1;
}

/* Después: alfa por píxel si hace falta y estado de GX */
static void prepare_state(void){
	if(stencil_val >= 0 && !(cur_c->amode == A_UNIFORM && stencil_val == cur_c->ualpha)) want_promote = 1;
	if(reads_dst_alpha && cur_c->amode == A_VRAM) want_promote = 1;
	if(want_promote){
		promote_alpha();
		want_promote = 0;
	}
	if(gx_dirty){
		u64 t0 = gettime();
		apply_gx();
		gx_dirty = 0;
		prof_state += gettime() - t0;
	}
}

static int prepare(int full){
	if(!prepare_targets(full)) return 0;
	prepare_state();
	return 1;
}

static void drawn(void){
	if(writes_c && !efb_c_newer){
		efb_c_newer = 1;
		if(cur_c->state != GPU_NEWER){ cur_c->state = GPU_NEWER; recompute_watch(); }
	}
	if(writes_z && cur_z && !efb_z_newer){
		efb_z_newer = 1;
		if(cur_z->state != GPU_NEWER){ cur_z->state = GPU_NEWER; recompute_watch(); }
	}
}

/* --- Primitivas ---------------------------------------------------------------- */

static inline int fog_byte(float fogdepth){
	if(!(fogdepth > 0.0f)) return 0;
	if(fogdepth >= 1.0f) return 255;
	return (int)(fogdepth * 255.0f);
}

static inline void put(float x, float y, float z, float s, float t, float q, core_u32 c0, core_u32 c1){
	wgPipe->F32 = x;
	wgPipe->F32 = y;
	wgPipe->F32 = z;
	wgPipe->F32 = s;
	wgPipe->F32 = t;
	wgPipe->F32 = q;
	wgPipe->U32 = __builtin_bswap32(c0);
	wgPipe->U32 = __builtin_bswap32(c1);
}

/* Una uv NaN o infinita muestrea el texel 0 en la PSP */
static inline float finite_uv(float f){
	return f - f == 0.0f ? f : 0.0f;
}

/* v da posición, uv y niebla; cv los colores */
static inline void put_vertex(const GeVertex *v, const GeVertex *cv){
	float s = 0.0f, t = 0.0f, q = 1.0f;
	if(tex_on){
		s = finite_uv(v->s); t = finite_uv(v->t);
		if(through){ s *= inv_tw; t *= inv_th; }
		else {
			float iw = 1.0f / v->clipw;
			if(tex_proj) q = v->q;
			s *= iw * tex_su; t *= iw * tex_sv; q *= iw;
		}
	}
	put((float)v->x * (1.0f / 16.0f) + pos_off, (float)v->y * (1.0f / 16.0f) + pos_off, (float)v->z, s, t, q,
	    cv->color0, (cv->color1 & 0xFFFFFF) | ((core_u32)fog_byte(v->fogdepth) << 24));
}

static void gx_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	const GeVertex *c;
	if(!prepare(0)) return;
	c = flat ? v2 : NULL;
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
	put_vertex(v0, c ? c : v0);
	put_vertex(v1, c ? c : v1);
	put_vertex(v2, c ? c : v2);
	GX_End();
	drawn();
}

/* Esquina de un sprite: x de vx, y de vy; s y t según la orientación */
static void put_corner(const GeVertex *vx, const GeVertex *vy, const GeVertex *v1, int swap_st, float iw){
	float s = 0.0f, t = 0.0f, q = 1.0f;
	if(tex_on){
		s = finite_uv(swap_st ? vy->s : vx->s);
		t = finite_uv(swap_st ? vx->t : vy->t);
		if(through){ s *= inv_tw; t *= inv_th; }
		else {
			if(tex_proj) q = vx->q;
			s *= iw * tex_su; t *= iw * tex_sv; q *= iw;
		}
	}
	put((float)vx->x * (1.0f / 16.0f) + pos_off, (float)vy->y * (1.0f / 16.0f) + pos_off, (float)v1->z, s, t, q,
	    v1->color0, (v1->color1 & 0xFFFFFF) | ((core_u32)fog_byte(v1->fogdepth) << 24));
}

static void draw_sprite(const GeVertex *v0, const GeVertex *v1){
	const GeVertex *l = v0->x < v1->x ? v0 : v1, *rr = v0->x < v1->x ? v1 : v0;
	const GeVertex *tp = v0->y < v1->y ? v0 : v1, *bt = v0->y < v1->y ? v1 : v0;
	int swap_st = (v0->x < v1->x) != (v0->y < v1->y);
	float iwl = 1.0f, iwr = 1.0f;
	if(tex_on && !through){ iwl = 1.0f / l->clipw; iwr = l == rr ? iwl : 1.0f / rr->clipw; }
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	put_corner(l, tp, v1, swap_st, iwl);
	put_corner(rr, tp, v1, swap_st, iwr);
	put_corner(rr, bt, v1, swap_st, iwr);
	put_corner(l, bt, v1, swap_st, iwl);
	GX_End();
}

static void gx_rect(const GeVertex *v0, const GeVertex *v1){
	if(v0->x == v1->x || v0->y == v1->y) return;
	if(!prepare(0)) return;
	draw_sprite(v0, v1);
	drawn();
}

static void gx_clear_rect(const GeVertex *v0, const GeVertex *v1){
	const GeRasterState *r = ge_raster_state();
	int full = 0, covers = 0, color_full = 0, alpha_w = 0, aval = (int)(v1->color0 >> 24);
	core_u32 mrgb = ge.cmd[GE_MASKRGB] & 0xFFFFFF, ma = ge.cmd[GE_MASKALPHA] & 0xFF;
	if(v0->x == v1->x || v0->y == v1->y) return;
	if(targets_dirty){
		setup_targets();
		targets_dirty = 0;
		gx_dirty = 1;
	}
	if(skip_draws) return;
	{
		int xl = (v0->x < v1->x ? v0->x : v1->x) / 16, xr = (v0->x > v1->x ? v0->x : v1->x) / 16;
		int yt = (v0->y < v1->y ? v0->y : v1->y) / 16, yb = (v0->y > v1->y ? v0->y : v1->y) / 16;
		int sx1 = r->sc_x1 / 16, sy1 = r->sc_y1 / 16, sx2 = r->sc_x2 / 16 + 1, sy2 = r->sc_y2 / 16 + 1;
		int has_alpha = has_alpha_fmt(cur_c->fmt);
		if(xl < sx1) xl = sx1;
		if(yt < sy1) yt = sy1;
		if(xr > sx2) xr = sx2;
		if(yb > sy2) yb = sy2;
		covers = xl <= 0 && yt <= 0 && xr >= cur_c->w && yb >= cur_c->h;
		alpha_w = has_alpha && r->stencil_test && ma != 0xFF;
		color_full = covers && r->color_test && mrgb == 0 && (!has_alpha || (r->stencil_test && ma == 0));
		if(color_full) full |= 1;
		if(cur_z && xl <= 0 && yt <= 0 && xr >= cur_z->w && yb >= cur_z->h && r->depth_write) full |= 2;
	}
	if(!prepare_targets(full)) return;
	/* El alfa, ya con el búfer en su sitio */
	if(color_full && has_alpha_fmt(cur_c->fmt)){
		/* Todo nuevo: alfa uniforme y color exacto */
		cur_c->amode = A_UNIFORM;
		cur_c->ualpha = aval;
		set_pf(surface_pf(cur_c));
	} else if(alpha_w && cur_c->amode != A_GPU){
		if(covers && ma == 0){ cur_c->amode = A_UNIFORM; cur_c->ualpha = aval; }
		else if(cur_c->amode != A_UNIFORM || aval != cur_c->ualpha) want_promote = 1;
	}
	prepare_state();
	draw_sprite(v0, v1);
	drawn();
}

/* GX dibuja líneas y puntos como cuadrados centrados en la coordenada; la
   PSP pinta el píxel cuya esquina es esa coordenada: medio píxel más */
static void gx_line(const GeVertex *v0, const GeVertex *v1){
	if(!prepare(0)) return;
	pos_off = 0.5f;
	GX_Begin(GX_LINES, GX_VTXFMT0, 2);
	put_vertex(v0, flat ? v1 : v0);
	put_vertex(v1, v1);
	GX_End();
	pos_off = 0.0f;
	drawn();
}

static void gx_point(const GeVertex *v0){
	if(!prepare(0)) return;
	pos_off = 0.5f;
	GX_Begin(GX_POINTS, GX_VTXFMT0, 1);
	put_vertex(v0, v0);
	GX_End();
	pos_off = 0.0f;
	drawn();
}

static void gx_begin(void){
	targets_dirty = 1;
}

static void gx_tex_flush(void){
	tex_flushed = 1;
}

static const GeHwRenderer gx_renderer = {
	gx_begin, gx_triangle, gx_rect, gx_clear_rect, gx_line, gx_point, gx_tex_flush
};

/* --- Presentación -------------------------------------------------------------- */

static Surface display_tmp;

static int present_body(void *xfb, GXRModeObj *rm, int widescreen);

int gx_ge_present(void *xfb, GXRModeObj *rm, int widescreen){
	u64 t0 = gettime();
	int r = present_body(xfb, rm, widescreen);
	prof_present += gettime() - t0;
	return r;
}

void gx_ge_profile(unsigned long long *setup, unsigned long long *state, unsigned long long *present,
                   unsigned *counts){
	*setup = prof_setup; *state = prof_state; *present = prof_present;
	counts[0] = prof_vram_presents; counts[1] = prof_downloads; counts[2] = prof_uploads; counts[3] = prof_decodes;
	prof_setup = prof_state = prof_present = 0;
	prof_vram_presents = prof_downloads = prof_uploads = prof_decodes = 0;
}

static int present_body(void *xfb, GXRModeObj *rm, int widescreen){
	HleFramebuffer fb;
	Surface *s = NULL;
	float x0, y0, x1, y1;
	core_u32 addr;

	hle_get_framebuffer(&fb);
	if(!fb.addr || fb.stride == 0) return 0;
	frame++;
	addr = fb.addr & PSP_ADDR_MASK;

	if(enabled && addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE){
		s = find_surface(addr - PSP_VRAM_BASE, fb.stride, (int)fb.format);
		if(s && (s->state == VRAM_NEWER || s->w < 480 || s->h < 272)) s = NULL;
		if(s && s == cur_c && efb_c_newer){ efb_to_tex(s); efb_c_newer = 0; }
		if(s && !s->has_tex) s = NULL;
	}

	/* El EFB se va a usar para componer la imagen */
	save_efb();
	cur_c = cur_z = NULL;
	restored_c = restored_z = 0;
	targets_dirty = 1;

	if(!s){
		/* Framebuffer escrito por la CPU: se convierte desde la memoria */
		Surface *d = &display_tmp;
		if(!d->tex){
			d->tex = memalign(32, 480 * 272 * 4);
			if(!d->tex) return 0;
		}
		prof_vram_presents++;
		/* Lo que la GPU tenga encima de esa memoria baja antes */
		mem_ptr_r(fb.addr, fb.stride * 271 * (fb.format == 3 ? 4u : 2u) + 480 * (fb.format == 3 ? 4u : 2u));
		wait_gpu();
		d->used = 1;
		d->addr = addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE ? addr - PSP_VRAM_BASE : 0;
		d->stride = fb.stride;
		d->fmt = (int)fb.format;
		d->w = 480;
		d->h = 272;
		if(addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE) vram_to_tex(d);
		else {
			/* En la RAM principal */
			int x, y, bpp = fb.format == 3 ? 4 : 2;
			const u8 *p = mem_ptr_r(fb.addr, fb.stride * 271 * (core_u32)bpp + 480 * (core_u32)bpp);
			if(!p) return 0;
			for(y = 0; y < 272; y++)
				for(x = 0; x < 480; x++){
					const u8 *q = p + ((core_u32)y * fb.stride + (core_u32)x) * (core_u32)bpp;
					core_u8 c[4];
					core_u32 raw = bpp == 4 ? (core_u32)q[0] | ((core_u32)q[1] << 8) | ((core_u32)q[2] << 16) | ((core_u32)q[3] << 24)
					                        : (core_u32)q[0] | ((core_u32)q[1] << 8);
					u8 *t = tile_ptr(d->tex, 480, x, y);
					ge_decode_color(fb.format, raw, c);
					t[0] = 0xFF; t[1] = c[0]; t[32] = c[1]; t[33] = c[2];
				}
			DCFlushRange(d->tex, 480 * 272 * 4);
			GX_InvalidateTexAll();
		}
		s = d;
	}

	if(efb_fmt != GX_PF_RGB8_Z24){
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
		efb_fmt = GX_PF_RGB8_Z24;
	}

	/* 480x272 escalado: en 16:9 llena la pantalla (el televisor lo estira);
	   en 4:3 ocupa el ancho con bandas negras */
	x0 = 0.0f; x1 = (float)rm->fbWidth;
	if(widescreen){ y0 = 0.0f; y1 = (float)rm->efbHeight; }
	else {
		float h = (float)rm->efbHeight * 0.75f * (272.0f / 480.0f) * 16.0f / 9.0f;
		y0 = ((float)rm->efbHeight - h) * 0.5f;
		y1 = y0 + h;
	}
	blit(NULL, NULL, 1, 0, 0.0f, 0.0f, (float)rm->fbWidth, (float)rm->efbHeight, 1.0f, 1.0f, 0, -1);
	blit(s, NULL, 1, 0, x0, y0, x1, y1, 480.0f / (float)s->w, 272.0f / (float)s->h, 1, -1);

	GX_SetCopyFilter(rm->aa, rm->sample_pattern, GX_TRUE, rm->vfilter);
	GX_SetDispCopySrc(0, 0, rm->fbWidth, rm->efbHeight);
	GX_SetDispCopyDst(rm->fbWidth, rm->xfbHeight);
	GX_CopyDisp(xfb, GX_FALSE);
	copy_filter_none();
	wait_gpu();
	return 1;
}

/* --- Control ---------------------------------------------------------------------- */

void gx_ge_sync_vram(void){
	int i;
	for(i = 0; i < MAX_SURF; i++)
		if(surf[i].used && surf[i].state == GPU_NEWER) download(&surf[i]);
	recompute_watch();
}

void gx_ge_reset(void){
	int i;
	wait_gpu();
	for(i = 0; i < MAX_SURF; i++)
		if(surf[i].used){ free(surf[i].tex); memset(&surf[i], 0, sizeof(surf[i])); }
	for(i = 0; i < MAX_TEX; i++)
		if(texc[i].used){ free(texc[i].data); memset(&texc[i], 0, sizeof(texc[i])); }
	tex_total = 0;
	cur_c = cur_z = NULL;
	cur_tex = NULL;
	efb_c_newer = efb_z_newer = restored_c = restored_z = 0;
	targets_dirty = gx_dirty = 1;
	tex_flushed = 1;
	recompute_watch();
}

void gx_ge_init(void){
	Mtx id;
	memset(empty_tex, 0, sizeof(empty_tex));
	DCFlushRange(empty_tex, sizeof(empty_tex));
	GX_InitTexObj(&empty_obj, empty_tex, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	guMtxIdentity(id);
	GX_LoadPosMtxImm(id, GX_PNMTX0);
	/* Coordenadas de la textura de tramado: píxel / 4 */
	guMtxScale(id, 0.25f, 0.25f, 1.0f);
	GX_LoadTexMtxImm(id, GX_TEXMTX0, GX_MTX2x4);
	guMtxIdentity(id);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetClipMode(GX_CLIP_ENABLE);
	GX_SetCoPlanar(GX_DISABLE);
	GX_SetLineWidth(6, GX_TO_ZERO);
	GX_SetPointSize(6, GX_TO_ZERO);
	GX_SetNumIndStages(0);
	copy_filter_none();
	mode = MODE_NONE;
	efb_fmt = -1;
}

void gx_ge_enable(int on){
	gx_ge_reset();
	enabled = on;
	ge_hw = on ? &gx_renderer : NULL;
	ge_fast_math = on;
	mem_vram_hook = on ? vram_hook : NULL;
	recompute_watch();
}

int gx_ge_enabled(void){ return enabled; }
