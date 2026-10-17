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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

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
#include "gpu/texcache.h"
#include "core/memory.h"
#include "hle/hle.h"
#include "core/prof.h"
#include "core/opt.h"
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
#include "wii/gx_tiles.h"
#include "wii/gx_emit.h"

#define EFB_W 640
#define EFB_H 528
#define VP 1024.0f            /* el viewport cubre 1024x1024 píxeles */
#define MAX_SURF 16
#define SURF_Z 4              /* formato de un búfer de profundidad */
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
	unsigned tex_gen;        /* sube cada vez que cambia tex */
	/* Texturas con paleta desde la GPU (ge_rtt_gpu): índices CI8 de GX de
	   ci_width() columnas (potencia de 2, como la textura de la PSP que lo
	   lee), con una fila de tiles de más para el vecino del filtro */
	u8 *ci;
	int ci_kind;             /* CI_A8 (copia del alfa del EFB) o CI_565 */
	unsigned ci_gen;         /* tex_gen del que salió (CI_565) */
	u32 ci_key;              /* desplazamiento y máscara (CI_565) */
	int ci_valid;
	int a8_want;             /* copiar el alfa en cada efb_to_tex */
	int gpu_bad;             /* primitivas que leían fuera; con 4, no se intenta más */
	/* Hueco (ge_fb_hueco): filas [hole0, hole0 + hole_n) en las que manda la
	   VRAM porque otro búfer más pequeño se dibujó encima */
	int hole0, hole_n;
} Surface;

enum { CI_NONE, CI_A8, CI_565 };

/* Una textura decodificada; cuándo vale la decide texcache.c (misma casilla) */
typedef struct {
	int w, h, levels;          /* niveles de mipmap en data */
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

static TexCache tcache;
static TexEntry texc[TC_MAX];
static TexEntry *cur_tex;
static u32 tex_total;
static int lazy_textures = 1;          /* espaciado de las comprobaciones */
static int tex_norm = 1;               /* s/w, t/w y 1/w normalizadas por triángulo */
static core_u32 clut_memo_gen, clut_memo_hash;
static int clut_memo_valid;
static unsigned prof_clut_hashes;
static TcStats tex_stats_base;
static u8 empty_tex[64] __attribute__((aligned(32)));
static GXTexObj empty_obj;

/* Estado de las primitivas */
static int tex_on, tex_proj, through, flat;
static float inv_tw, inv_th, tex_su = 1.0f, tex_sv = 1.0f;
static GXTexObj rtt_obj;
static int use_rtt;                     /* la textura es un framebuffer en la GPU */
static GXTexObj clut_obj;
static int use_clut_gpu;                /* textura con paleta desde la GPU (ge_rtt_gpu) */
static Surface *clut_src;               /* su búfer de origen */
/* TLUT de GX en anillo: GX_LoadTlut lee la memoria cuando la GPU llega al
   comando, así que un búfer no se reescribe hasta después de un
   GX_DrawDone (wait_gpu) */
#define TLUT_RING 64
static u16 tlut_ring[TLUT_RING][256] __attribute__((aligned(32)));
static GXTlutObj tlut_obj;
static int tlut_pos, tlut_used, tlut_loaded = -1;
/* Censo de este camino */
static unsigned clut_gpu_uses[3], clut_gpu_fallbacks, clut_ci_builds, clut_tlut_loads;
static u64 clut_ci_ticks;
static float pos_off;

/* Perfilado (ticks del Wii) */
static u64 prof_setup, prof_state, prof_present;
static unsigned prof_vram_presents, prof_downloads, prof_uploads, prof_decodes;

/* Censo de destinos de dibujo ([TIEMPOS] rtt: en wiisp.log). Mide, antes de
   construir el render-to-texture en la GPU, a dónde va cada búfer que dibuja
   el juego y quién lo consume:
     - bajadas de la GPU a la VRAM emulada, según quién las pidió (lo que
       estaba haciendo el emulador: buscar una textura, la CPU del juego,
       componer la pantalla o el GE leyendo una paleta o copiando bloques),
       con su tiempo;
     - texturas decodificadas, según de dónde salen (un búfer dibujado, otra
       parte de la VRAM o la RAM), con tiempo y bytes;
     - texturas que caen dentro de un búfer dibujado: cuántas se usaron
       directamente desde la GPU y, cuando no se pudo, por qué;
     - los búferes con más actividad.
   Y para el plan de las texturas con paleta sobre framebuffers (paso 0):
     - qué rectángulo de la textura muestrean de verdad los vértices (la
       última fila y columna leídas, contando el vecino del filtro bilineal
       solo si pesa), con qué filtro y repetición, y el alfa del búfer de
       origen (uniforme, en la VRAM o en la GPU);
     - el motivo detallado de cada bajada (textura, búfer arrastrado por el
       rango de una textura, solape al crear otro destino, copia de bloques
       del GE...) con la espera a la GPU y la conversión por separado;
     - cómo empieza cada destino al usarse: borrado completo, restaurado
       desde su copia o subido desde la VRAM. */
enum { RD_TEX, RD_CPU, RD_PANT, RD_GE, RD_N };
enum { RO_FB, RO_VRAM, RO_RAM, RO_N };
enum { RM_FMT, RM_SWZ, RM_INEXACT, RM_VRAM, RM_ALPHA, RM_NOTEX, RM_N };
/* Motivo detallado de una bajada */
enum { DX_TEX, DX_ARRASTRE, DX_CPU, DX_PANT, DX_SOLAPE, DX_CRECE, DX_EXPULSA, DX_TRANSFER, DX_CLUT, DX_GE,
       DX_SINC, DX_N };
#define RTT_MAX 24
typedef struct {
	core_u32 addr, stride;
	int fmt, w, h;
	unsigned binds, dl[RD_N], gpu, dec, miss;
	unsigned first_clear, first_blit, first_upload;   /* cómo empezó cada uso */
	unsigned cover_opaque, cover_blend;   /* primer dibujo: sprite que lo tapa entero */
	unsigned vram_writes;                 /* veces que se escribió su VRAM (pasa a mandar la VRAM) */
	u64 dl_ticks, up_ticks, dec_ticks;
	int amode, ualpha;
} RttTarget;
static RttTarget rtt_t[RTT_MAX];
static int rtt_n;
/* Cómo lee el juego un framebuffer como textura con paleta: decide si se
   puede hacer en la GPU (copia de un canal del EFB + TLUT de GX) y qué
   parte hace falta decodificar */
#define RTT_PAL_MAX 12
typedef struct {
	core_u32 addr;           /* offset de la textura en la VRAM */
	int texfmt, surf_fmt, clut_fmt, tw, th, bufw;
	core_u32 shift, mask, offset;
	unsigned n;
	unsigned prims, sprites, through, linear, clamp_s, clamp_t, neighbor_s, neighbor_t;
	float umin, umax, vmin, vmax;   /* texels en los vértices */
	int col_max, row_max;           /* última columna y fila leídas (con peso) */
	unsigned amode[3];
	int ualpha;
	int src_w, src_h;               /* tamaño del búfer de origen */
	float smax, tmax;               /* coordenada máxima muestreada (texels) */
	unsigned beyond_s, beyond_t;    /* sprites que leen fuera del búfer de origen */
	float beyond_ws, beyond_wt;     /* peso máximo de esa columna/fila */
	unsigned clut_changes;          /* la paleta cambió respecto al uso anterior */
	core_u32 clut_last;
	core_u32 dest_addr;             /* destino de los dibujos */
	int dest_fmt;
	unsigned dest_other, blend;     /* usos con otro destino; usos con mezcla */
	struct { int w, h; float ku, kv, du, dv; unsigned n; } kind[4];   /* sprites por forma */
} RttPal;
static RttPal rtt_pal[RTT_PAL_MAX];
static int rtt_npal;
static RttPal *rtt_cur;                 /* la de las primitivas en curso */
static core_u32 rtt_tex_off = 0xFFFFFFFFu;   /* textura que se está buscando */

/* Censo de estado (prof_estado): avisos que invalidan los destinos entre
   dos llamadas (census_dst) y primera primitiva de la llamada */
static int cs_aviso = 1, cs_primero;
/* Alguien más que apply_gx tocó el estado de GX (copias, vuelta a la CPU de
   las paletas, formato del EFB, presentar): la próxima vez se emite entero
   (ge_estado_sucio) */
static int gx_tocado = 1;
/* ge_estado_sucio: el próximo triángulo de esta llamada no tiene nada que
   preparar ni que marcar (lo que miran prepare_idle y drawn). Lo quita todo
   lo que entra en este archivo desde fuera entre dos triángulos: la llamada
   siguiente, los ganchos de la VRAM, las demás primitivas, presentar... */
static int tri_listo;
static unsigned rtt_dl[RD_N], rtt_dec[RO_N], rtt_gpu, rtt_miss[RM_N];
static u64 rtt_dl_ticks[RD_N], rtt_dec_ticks[RO_N], rtt_dec_bytes[RO_N];
static unsigned rtt_dx[DX_N], rtt_uploads;
static u64 rtt_dx_wait[DX_N], rtt_dx_conv[DX_N], rtt_up_ticks;
/* Filas que tocó el acceso que provocó cada bajada (lo que bastaría bajar) */
static u64 rtt_dx_rows[DX_N], rtt_dx_rows_all[DX_N];
static unsigned rtt_dx_pad[DX_N];       /* el acceso solo tocó el relleno de las filas */
static core_u32 acc_off, acc_len;       /* rango que está pidiendo el gancho de la VRAM */
/* Cargas de paleta (LOADCLUT) que caen en un búfer dibujado */
#define RTT_CLUT_MAX 6
typedef struct {
	core_u32 off, len, surf_addr;
	int surf_fmt, r0, r1, c0, c1, pad;
	unsigned n, same;
	core_u32 hash;
} RttClut;
static RttClut rtt_clut[RTT_CLUT_MAX];
static int rtt_nclut;
static unsigned rtt_pad_skips;          /* accesos al relleno que no bajaron nada */
static unsigned hole_made, hole_filled, hole_rows_dl, hole_rows_up, hole_skips;
static u64 hole_ticks;
/* El primer dibujo tras usar un destino, si es un sprite */
static const GeVertex *first_v0, *first_v1;
static int dl_force = -1;               /* motivo que fija quien llama */
/* Comandos del GE que provocan bajadas sin ser texturas ni copias */
#define RTT_CMD_MAX 6
static unsigned rtt_cmd_n[RTT_CMD_MAX];
static core_u32 rtt_cmd[RTT_CMD_MAX];
/* Búfer bajado al crear otro destino encima */
#define RTT_PAIR_MAX 8
typedef struct {
	core_u32 old_addr, new_addr;
	int old_fmt, new_fmt, old_w, old_h, new_w, new_h;
	int r0, r1;              /* filas del viejo que pisa el nuevo */
	unsigned n, downloads;   /* veces; las que hubo que bajarlo */
} RttPair;
static RttPair rtt_pair[RTT_PAIR_MAX];
static int rtt_npair;
static core_u32 dl_new_addr;            /* el destino que se está creando */
static int dl_new_fmt, dl_new_w, dl_new_h;

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
static void wait_gpu_body(void);

static void wait_gpu(void){
	int old = prof_switch(PROF_ESPERA_GX);
	wait_gpu_body();
	prof_switch(old);
}

/* gx_paralelo: la presentación no espera a la GPU. Deja un testigo
   (GX_SetDrawSync, otro registro que el de GX_DrawDone) detrás de la copia
   al XFB; cuando la GPU lo pasa, la imagen está en el XFB y lo que se
   liberó o se cargó en el anillo de TLUT antes de pedirlo ya no lo usa. */
static u16 present_tok;
static int present_pend, defer_at_tok, tlut_at_tok;
static unsigned par_presents, par_waits, par_lost;
static u64 par_wait_ticks;

static void wait_gpu_body(void){
	GX_DrawDone();
	flush_deferred();
	tlut_used = 0;    /* la GPU ya leyó todas las TLUT encoladas */
	present_pend = 0;   /* y pasó cualquier testigo */
	defer_at_tok = tlut_at_tok = 0;
}

/* La GPU pasó el testigo de la última presentación */
static void present_done(void){
	int i, n = defer_at_tok < ndefer ? defer_at_tok : ndefer;
	for(i = 0; i < n; i++) free(defer_list[i]);
	memmove(defer_list, defer_list + n, (size_t)(ndefer - n) * sizeof(defer_list[0]));
	ndefer -= n;
	tlut_used = tlut_used > tlut_at_tok ? tlut_used - tlut_at_tok : 0;
	present_pend = 0;
	defer_at_tok = tlut_at_tok = 0;
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
	gx_tocado = 1;
	tri_listo = 0;
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
		if(opt_on[OPT_GE_Z_RANGO]){
			/* Los bytes de la profundidad están en vram_depth: el swizzle
			   no los saca de su bloque (2-64 KB según la traducción; con
			   la de arranque, 16 KB). Sin esto, una textura pegada detrás
			   del búfer (DBZ: 0x04158000) lo bajaba entero en cada imagen. */
			core_u32 b = mem_vram_swizzle_block();
			a &= ~(b - 1);
			e = (e + b - 1) & ~(b - 1);
		} else {
			/* El swizzle mueve los bytes de la profundidad hasta ~32 KB */
			a = a > 0x10000 ? a - 0x10000 : 0;
			e += 0x10000;
		}
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
static void tex_to_vram_body(const Surface *s, int y0, int y1);

static int in_hole_row(const Surface *s, int y){ return s->hole_n && y >= s->hole0 && y < s->hole0 + s->hole_n; }

static void tex_to_vram(const Surface *s){
	int old = prof_switch(PROF_FB);
	tex_to_vram_body(s, 0, s->h - 1);
	prof_switch(old);
}

/* Las filas [y0, y1]; las del hueco no (ahí manda la VRAM) */
static void tex_to_vram_body(const Surface *s, int y0, int y1){
	int x, y, w = visible_w(s), keep_a = s->amode == A_VRAM;
	u32 ua = (u32)s->ualpha;
	DCInvalidateRange(s->tex, (u32)(s->w * s->h * 4));
	for(y = y0; y <= y1; y++){
		const u8 *trow = s->tex + (u32)(((y >> 2) * (s->w >> 2)) << 6) + (u32)((y & 3) << 3);
		if(in_hole_row(s, y)) continue;
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
static void vram_to_tex_body(Surface *s);

static void vram_to_tex(Surface *s){
	int old = prof_switch(PROF_FB);
	vram_to_tex_body(s);
	prof_switch(old);
}

static void vram_to_tex_body(Surface *s){
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
	s->tex_gen++;
	s->hole_n = 0;
	if(s->ci_kind == CI_A8) s->ci_valid = 0;   /* el alfa ya no es el del EFB */
}

/* Solo las filas [y0, y1] de la VRAM a tex (las del hueco), sin tocar el
   resto ni el modo de alfa. Solo búferes de color. */
static void vram_to_tex_rows(Surface *s, int y0, int y1){
	int x, y, w = visible_w(s), bpp = surf_bpp(s->fmt);
	u32 lo = (u32)(((y0 >> 2) * (s->w >> 2)) << 6), hi = (u32)((((y1 >> 2) + 1) * (s->w >> 2)) << 6);
	int old = prof_switch(PROF_FB);
	/* Las demás filas de esos tiles las escribió la GPU: se leen de la
	   memoria antes de cambiar estas */
	wait_gpu();
	DCInvalidateRange(s->tex + lo, hi - lo);
	for(y = y0; y <= y1; y++)
		for(x = 0; x < w; x++){
			const u8 *q = vram_color(s, x, y, bpp);
			u8 *t = tile_ptr(s->tex, s->w, x, y);
			core_u32 c = bpp == 4 ? (core_u32)q[0] | ((core_u32)q[1] << 8) | ((core_u32)q[2] << 16) | ((core_u32)q[3] << 24)
			                      : vram16_to_8888(s->fmt, (core_u32)q[0] | ((core_u32)q[1] << 8));
			t[0] = (u8)(c >> 24); t[1] = (u8)c; t[32] = (u8)(c >> 8); t[33] = (u8)(c >> 16);
		}
	DCFlushRange(s->tex + lo, hi - lo);
	GX_InvalidateTexAll();
	s->tex_gen++;
	if(s->ci_kind == CI_A8) s->ci_valid = 0;
	prof_switch(old);
}

/* EFB -> tex (sin esperar a la GPU) */
static void efb_to_tex_body(Surface *s);

static void efb_to_tex(Surface *s){
	int old = prof_switch(PROF_FB);
	efb_to_tex_body(s);
	prof_switch(old);
}

/* Ancho de los índices CI8 de un búfer: la potencia de 2 que lo cubre,
   como la textura de la PSP que lo lee (512 para 480) */
static int ci_width(const Surface *s){
	int w = 8;
	while(w < s->w) w <<= 1;
	return w;
}

/* Memoria de los índices CI8 (a cero; una fila de tiles de más: la que lee
   el filtro con peso 0 justo debajo de la última) */
static int ensure_ci(Surface *s){
	u32 bytes;
	if(s->ci) return 1;
	if(ci_width(s) > MAX_TEX_SIZE || ci_width(s) > EFB_W) return 0;
	bytes = (u32)ci_width(s) * (u32)(s->h + 4);
	s->ci = memalign(32, bytes);
	if(!s->ci) return 0;
	memset(s->ci, 0, bytes);
	DCFlushRange(s->ci, bytes);
	s->ci_valid = 0;
	return 1;
}

static void efb_to_tex_body(Surface *s){
	copy_filter_none();
	GX_SetTexCopySrc(0, 0, (u16)s->w, (u16)s->h);
	GX_SetTexCopyDst((u16)s->w, (u16)s->h, s->fmt == SURF_Z ? GX_TF_Z24X8 : GX_TF_RGBA8, GX_FALSE);
	GX_CopyTex(s->tex, GX_FALSE);
	s->tex_gen++;
	if(s->a8_want && s->fmt == GE_FMT_8888 && s->amode == A_GPU && ensure_ci(s)){
		/* El alfa (6 bits del EFB, a 8 como en la copia RGBA8) como
		   índices CI8 de un CLUT32 que lo lee (ge_rtt_gpu) */
		int cw = ci_width(s);
		GX_SetTexCopySrc(0, 0, (u16)cw, (u16)s->h);
		GX_SetTexCopyDst((u16)cw, (u16)s->h, GX_CTF_A8, GX_FALSE);
		GX_CopyTex(s->ci, GX_FALSE);
		s->ci_kind = CI_A8;
		s->ci_valid = 1;
	} else if(s->ci_kind == CI_A8) s->ci_valid = 0;
	GX_PixModeSync();
	GX_InvalidateTexAll();
	s->has_tex = 1;
}

static void save_efb(void){
	if(cur_c && efb_c_newer){ efb_to_tex(cur_c); efb_c_newer = 0; }
	if(cur_z && efb_z_newer){ efb_to_tex(cur_z); efb_z_newer = 0; }
}

/* Entrada del censo de un búfer (por dirección, ancho de fila y formato) */
static RttTarget *rtt_target(core_u32 addr, core_u32 stride, int fmt){
	int i;
	for(i = 0; i < rtt_n; i++)
		if(rtt_t[i].addr == addr && rtt_t[i].stride == stride && rtt_t[i].fmt == fmt) return &rtt_t[i];
	if(rtt_n == RTT_MAX) return NULL;
	memset(&rtt_t[rtt_n], 0, sizeof(rtt_t[rtt_n]));
	rtt_t[rtt_n].addr = addr;
	rtt_t[rtt_n].stride = stride;
	rtt_t[rtt_n].fmt = fmt;
	return &rtt_t[rtt_n++];
}

static RttTarget *rtt_surface(const Surface *s){
	RttTarget *t = rtt_target(s->addr, s->stride, s->fmt);
	if(t){ t->w = s->w; t->h = s->h; t->amode = s->amode; t->ualpha = s->ualpha; }
	return t;
}

/* Quién pidió una bajada: lo dice la cubeta del desglose en curso */
static int rtt_reason(void){
	switch(prof_cur){
	case PROF_TEXTURAS: return RD_TEX;
	case PROF_CPU: case PROF_HLE: case PROF_AUDIO: case PROF_ES: return RD_CPU;
	case PROF_PRESENTAR: return RD_PANT;
	default: return RD_GE;
	}
}

/* Búfer de color usado (no el de profundidad) que cubre la dirección de
   VRAM `off`, o NULL */
static Surface *surface_at(core_u32 off){
	int i;
	for(i = 0; i < MAX_SURF; i++){
		core_u32 a, e;
		if(!surf[i].used || surf[i].fmt == SURF_Z) continue;
		surf_range(&surf[i], &a, &e);
		if(off >= a && off < e) return &surf[i];
	}
	return NULL;
}

static core_u32 clut_hash_now(void);

/* Una textura del GE: si cae dentro de un búfer dibujado, se apunta si se
   usó desde la GPU o, si no, por qué no (las mismas condiciones que
   texture_from_surface, en orden) */
static void rtt_note_texture(const GeRasterState *r, int from_gpu){
	core_u32 addr = r->texaddr[0] & PSP_ADDR_MASK, off;
	Surface *s;
	RttTarget *t;
	int why;
	if(addr < PSP_VRAM_BASE || addr >= PSP_VRAM_BASE + PSP_VRAM_SIZE) return;
	off = addr - PSP_VRAM_BASE;
	if(!(s = surface_at(off))) return;
	t = rtt_surface(s);
	/* 1: copia directa del búfer; 2: con paleta desde la GPU (también se
	   apunta en el censo de paletas) */
	if(from_gpu){
		rtt_gpu++;
		if(t) t->gpu++;
		if(from_gpu == 1) return;
	}
	if(r->texfmt > GE_FMT_8888){
		core_u32 cf = r->clutformat;
		RttPal k;
		int i;
		why = RM_FMT;
		memset(&k, 0, sizeof(k));
		k.addr = off;
		k.texfmt = r->texfmt; k.surf_fmt = s->fmt; k.clut_fmt = r->clut_fmt;
		k.tw = 1 << r->width0_shift; k.th = 1 << r->height0_shift; k.bufw = r->texbufw[0];
		k.shift = (cf >> 2) & 0x1F; k.mask = (cf >> 8) & 0xFF; k.offset = (cf >> 16) & 0x1F;
		for(i = 0; i < rtt_npal; i++){
			RttPal *p = &rtt_pal[i];
			if(p->addr == k.addr && p->texfmt == k.texfmt && p->surf_fmt == k.surf_fmt && p->clut_fmt == k.clut_fmt &&
			   p->tw == k.tw && p->th == k.th && p->bufw == k.bufw && p->shift == k.shift && p->mask == k.mask &&
			   p->offset == k.offset){
				p->n++;
				break;
			}
		}
		if(i == rtt_npal && rtt_npal < RTT_PAL_MAX){
			k.n = 1;
			k.umin = k.vmin = 1e30f; k.umax = k.vmax = -1e30f;
			k.smax = k.tmax = -1e30f;
			k.col_max = k.row_max = -1;
			k.clut_last = clut_hash_now();
			k.dest_addr = cur_c ? cur_c->addr : 0xFFFFFFFFu;
			k.dest_fmt = cur_c ? cur_c->fmt : -1;
			rtt_pal[rtt_npal++] = k;
		}
		if(i < rtt_npal){
			RttPal *p = &rtt_pal[i];
			core_u32 ch = clut_hash_now();
			rtt_cur = p;
			p->amode[s->amode < 3 ? s->amode : 0]++;
			p->ualpha = s->ualpha;
			p->src_w = s->w;
			p->src_h = s->h;
			if(ch != p->clut_last){ p->clut_changes++; p->clut_last = ch; }
			if(!cur_c || cur_c->addr != p->dest_addr || cur_c->fmt != p->dest_fmt) p->dest_other++;
			if(r->alpha_blend) p->blend++;
		}
	}
	else if(r->swizzle) why = RM_SWZ;
	else if(s->addr != off || s->stride != r->texbufw[0] || s->fmt != r->texfmt) why = RM_INEXACT;
	else if(s->state == VRAM_NEWER) why = RM_VRAM;
	else if(r->use_tex_alpha && s->amode != A_GPU && !(s->amode == A_UNIFORM && s->ualpha == 255) && has_alpha_fmt(s->fmt))
		why = RM_ALPHA;
	else why = RM_NOTEX;
	if(from_gpu) return;
	rtt_miss[why]++;
	if(t) t->miss++;
}

/* Motivo detallado de una bajada (censo) */
static int rtt_detail(const Surface *s, int why){
	if(dl_force >= 0) return dl_force;
	switch(why){
	case RD_TEX: return s->addr == rtt_tex_off ? DX_TEX : DX_ARRASTRE;
	case RD_CPU: return DX_CPU;
	case RD_PANT: return DX_PANT;
	default: return ge.cur_cmd == GE_TRANSFERSTART ? DX_TRANSFER : ge.cur_cmd == GE_LOADCLUT ? DX_CLUT : DX_GE;
	}
}

static void rtt_note_detail(const Surface *s, int dx){
	int i;
	if(dx == DX_GE){
		for(i = 0; i < RTT_CMD_MAX && rtt_cmd_n[i] && rtt_cmd[i] != ge.cur_cmd; i++){}
		if(i < RTT_CMD_MAX){ rtt_cmd[i] = ge.cur_cmd; rtt_cmd_n[i]++; }
	}
}

/* Filas de un búfer que toca el rango [off, off+len) de la VRAM (lineal,
   también para la profundidad); pad = solo el relleno de las filas */
static int rows_touched(const Surface *s, core_u32 off, core_u32 len, int *r0, int *r1, int *pad){
	core_u32 rowb = s->stride * (core_u32)surf_bpp(s->fmt), a, e, vis = (core_u32)s->w * (core_u32)surf_bpp(s->fmt);
	if(!rowb || !len || off + len <= s->addr) return 0;
	a = off > s->addr ? off - s->addr : 0;
	e = off + len - s->addr;
	*r0 = (int)(a / rowb);
	*r1 = (int)((e - 1) / rowb);
	if(*r0 >= s->h) return 0;
	if(*r1 >= s->h) *r1 = s->h - 1;
	*pad = *r0 == *r1 && a % rowb >= vis && (e - 1) % rowb >= vis;
	return 1;
}

/* Un búfer que deja de existir en la GPU porque se crea otro encima */
static void rtt_note_pair(const Surface *o, core_u32 a, core_u32 e){
	RttPair *p = NULL;
	int i, r0 = 0, r1 = 0, pad;
	for(i = 0; i < rtt_npair; i++){
		p = &rtt_pair[i];
		if(p->old_addr == o->addr && p->old_fmt == o->fmt && p->new_addr == dl_new_addr && p->new_fmt == dl_new_fmt) break;
	}
	if(i == rtt_npair){
		if(rtt_npair == RTT_PAIR_MAX) return;
		p = &rtt_pair[rtt_npair++];
		memset(p, 0, sizeof(*p));
		p->old_addr = o->addr; p->old_fmt = o->fmt; p->old_w = o->w; p->old_h = o->h;
		p->new_addr = dl_new_addr; p->new_fmt = dl_new_fmt; p->new_w = dl_new_w; p->new_h = dl_new_h;
		p->r0 = 1 << 30; p->r1 = -1;
	}
	if(rows_touched(o, a, e - a, &r0, &r1, &pad)){
		if(r0 < p->r0) p->r0 = r0;
		if(r1 > p->r1) p->r1 = r1;
	}
	p->n++;
	if(o->state == GPU_NEWER) p->downloads++;
}

/* Contenido de la GPU -> VRAM */
static void download(Surface *s){
	if(s == cur_c && efb_c_newer){ efb_to_tex(s); efb_c_newer = 0; }
	if(s == cur_z && efb_z_newer){ efb_to_tex(s); efb_z_newer = 0; }
	if(s->has_tex){
		core_u32 a, e;
		int why = rtt_reason(), dx = rtt_detail(s, why);
		RttTarget *t = rtt_surface(s);
		u64 t0 = gettime(), t1;
		wait_gpu();
		t1 = gettime();
		tex_to_vram(s);
		prof_downloads++;
		rtt_dl[why]++;
		rtt_dl_ticks[why] += gettime() - t0;
		rtt_dx[dx]++;
		rtt_dx_wait[dx] += t1 - t0;
		rtt_dx_conv[dx] += gettime() - t1;
		rtt_note_detail(s, dx);
		{
			int r0, r1, pad;
			rtt_dx_rows_all[dx] += (u64)s->h;
			if(acc_len && rows_touched(s, acc_off, acc_len, &r0, &r1, &pad)){
				rtt_dx_rows[dx] += (u64)(r1 - r0 + 1);
				rtt_dx_pad[dx] += pad;
			} else rtt_dx_rows[dx] += (u64)s->h;
		}
		if(t){ t->dl[why]++; t->dl_ticks += gettime() - t0; }
		/* Las texturas que estén ahí cambiaron (y empieza otro periodo,
		   como en PPSSPP al leer un framebuffer) */
		surf_range(s, &a, &e);
		mem_note_write(PSP_VRAM_BASE + a, e - a);
		ge_sync_domain++;
	}
	s->state = SYNCED;
}

static void drop_surface(Surface *s){
	if(s->state == GPU_NEWER) download(s);
	if(s == cur_c){ cur_c = NULL; efb_c_newer = 0; restored_c = 0; }
	if(s == cur_z){ cur_z = NULL; efb_z_newer = 0; restored_z = 0; }
	if(s == clut_src){ clut_src = NULL; use_clut_gpu = 0; targets_dirty = 1; cs_aviso = 1; }
	defer_free(s->tex);
	defer_free(s->ci);
	memset(s, 0, sizeof(*s));
}

/* Gancho de memory.c: la CPU va a tocar [off, off+len) de la VRAM */
/* Censo: una carga de paleta que cae en un búfer dibujado (ya bajado) */
static void rtt_note_clut(const Surface *s, core_u32 off, core_u32 len){
	RttClut *c = NULL;
	core_u32 h = 2166136261u, i;
	int k, r0, r1, pad;
	for(i = 0; i < len && off + i < PSP_VRAM_SIZE; i++){ h ^= psp_mem.vram[off + i]; h *= 16777619u; }
	for(k = 0; k < rtt_nclut; k++)
		if(rtt_clut[k].off == off && rtt_clut[k].len == len){ c = &rtt_clut[k]; break; }
	if(!c){
		if(rtt_nclut == RTT_CLUT_MAX) return;
		c = &rtt_clut[rtt_nclut++];
		memset(c, 0, sizeof(*c));
		c->off = off; c->len = len; c->surf_addr = s->addr; c->surf_fmt = s->fmt;
		c->r0 = c->r1 = -1;
		if(rows_touched(s, off, len, &r0, &r1, &pad)){
			core_u32 bpp = (core_u32)surf_bpp(s->fmt), rowb = s->stride * bpp;
			c->r0 = r0; c->r1 = r1; c->pad = pad;
			c->c0 = (int)(((off - s->addr) % rowb) / bpp);
			c->c1 = (int)(((off + len - 1 - s->addr) % rowb) / bpp);
		}
	} else if(c->hash == h) c->same++;
	c->hash = h;
	c->n++;
}

static int only_hole(const Surface *s, core_u32 off, core_u32 len);

/* ¿[off, off+len) solo toca el relleno de una fila del búfer (las columnas
   desde el ancho dibujado)? GX nunca escribe ahí: en esos bytes manda
   siempre la VRAM, así que leerlos o escribirlos no necesita bajar el
   búfer ni invalidarlo (DBZ guarda una paleta en el relleno de la primera
   fila de su framebuffer y la carga en cada imagen) */
static int only_padding(const Surface *s, core_u32 off, core_u32 len){
	core_u32 bpp, rowb, vis, a, e;
	if(s->fmt == SURF_Z || !len || off < s->addr) return 0;
	bpp = (core_u32)surf_bpp(s->fmt);
	rowb = s->stride * bpp;
	vis = (core_u32)visible_w(s) * bpp;
	if(!rowb || vis >= rowb) return 0;
	a = off - s->addr;
	e = a + len - 1;
	return a / rowb == e / rowb && a % rowb >= vis;
}

static void vram_hook(core_u32 off, core_u32 len, int write){
	tri_listo = 0;   /* puede bajar o invalidar el búfer en el que se dibuja */
	int i;
	const Surface *clut_s = NULL;
	acc_off = off;
	acc_len = len;
	for(i = 0; i < MAX_SURF; i++){
		Surface *s = &surf[i];
		core_u32 a, e;
		if(!s->used || s->state == VRAM_NEWER) continue;
		surf_range(s, &a, &e);
		if(off >= e || off + len <= a) continue;
		if(opt_on[OPT_GE_FB_RELLENO] && only_padding(s, off, len)){
			rtt_pad_skips++;
			continue;
		}
		if(only_hole(s, off, len)) continue;
		if(s->state == GPU_NEWER) download(s);
		if(!write && ge.cur_cmd == GE_LOADCLUT && prof_cur != PROF_TEXTURAS && s->fmt != SURF_Z) clut_s = s;
		if(write){
			RttTarget *t = rtt_surface(s);
			if(t) t->vram_writes++;
			s->state = VRAM_NEWER;
			s->hole_n = 0;
			if(s == cur_c){ restored_c = 0; efb_c_newer = 0; }
			if(s == cur_z){ restored_z = 0; efb_z_newer = 0; }
			targets_dirty = 1;
			cs_aviso = 1;
		}
	}
	acc_len = 0;
	if(clut_s) rtt_note_clut(clut_s, off, len);
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

/* --- Huecos (ge_fb_hueco) ------------------------------------------------------- */

/* DBZ dibuja en cada imagen un búfer 4444 de 64x64 encima de las 8 primeras
   filas de su 565 de 480x272, y después vuelve al 565: tirar el 565 de la
   GPU obligaba a bajarlo y a subirlo entero en cada imagen. Con el hueco,
   el grande se queda en la GPU; solo sus filas pisadas van y vuelven por la
   VRAM (bajar antes de que el pequeño se suba desde ahí, subir cuando el
   grande se vuelve a usar). En las filas del hueco manda la VRAM: no se
   bajan, y el búfer no se usa como textura ni se muestra hasta llenarlo. */

/* ¿El acceso solo toca filas del hueco del búfer? (no es asunto suyo) */
static int only_hole(const Surface *s, core_u32 off, core_u32 len){
	int r0, r1, pad;
	if(!s->hole_n || !rows_touched(s, off, len, &r0, &r1, &pad)) return 0;
	return off >= s->addr && r0 >= s->hole0 && r1 < s->hole0 + s->hole_n &&
	       off + len <= s->addr + s->stride * (core_u32)s->h * (core_u32)surf_bpp(s->fmt);
}

/* Las filas [y0, y1] de la GPU a la VRAM; el resto sigue en la GPU */
static void download_rows(Surface *s, int y0, int y1){
	core_u32 rowb = s->stride * (core_u32)surf_bpp(s->fmt);
	int old;
	if(s == cur_c && efb_c_newer){ efb_to_tex(s); efb_c_newer = 0; }
	if(!s->has_tex) return;
	wait_gpu();
	old = prof_switch(PROF_FB);
	tex_to_vram_body(s, y0, y1);
	prof_switch(old);
	mem_note_write(PSP_VRAM_BASE + s->addr + (core_u32)y0 * rowb, (core_u32)(y1 - y0 + 1) * rowb);
	ge_sync_domain++;
	hole_rows_dl += (unsigned)(y1 - y0 + 1);
}

/* Se va a crear un búfer de formato fmt en [a, e) encima del búfer o:
   ¿basta con un hueco en o? Si sí, lo deja hecho. */
static int keep_with_hole(Surface *o, int fmt, core_u32 a, core_u32 e){
	int r0, r1, pad, h0, h1;
	u64 t0;
	if(!opt_on[OPT_GE_FB_HUECO] || fmt == SURF_Z || o->fmt == SURF_Z || o == cur_z) return 0;
	if(o->state == VRAM_NEWER || !o->has_tex) return 0;
	/* El alfa de las filas que vuelvan de la VRAM tiene que valer tal cual */
	if(o->fmt != GE_FMT_565 && o->amode != A_GPU && o->amode != A_VRAM) return 0;
	if(a < o->addr) a = o->addr;
	if(!rows_touched(o, a, e - a, &r0, &r1, &pad)) return 0;
	h0 = r0; h1 = r1;
	if(o->hole_n){
		if(o->hole0 < h0) h0 = o->hole0;
		if(o->hole0 + o->hole_n - 1 > h1) h1 = o->hole0 + o->hole_n - 1;
	}
	if((h1 - h0 + 1) * 4 > o->h) return 0;
	t0 = gettime();
	/* Todo el hueco nuevo baja (las filas que ya eran hueco se saltan): si
	   se une a uno anterior, también las que quedan entre los dos */
	if(o->state == GPU_NEWER) download_rows(o, h0, h1);
	o->hole0 = h0;
	o->hole_n = h1 - h0 + 1;
	if(o == clut_src){ clut_src = NULL; use_clut_gpu = 0; }
	hole_made++;
	hole_ticks += gettime() - t0;
	return 1;
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
		if(s->w >= need_w && s->h >= need_h){
			if(s->hole_n){
				/* Vuelve: lo que se dibujó encima de su hueco baja a la VRAM
				   (y desde ahí, el hueco se sube al restaurar) */
				core_u32 sa = s->addr, se = s->addr + s->stride * (core_u32)s->h * (core_u32)surf_bpp(s->fmt);
				dl_new_addr = addr; dl_new_fmt = fmt; dl_new_w = s->w; dl_new_h = s->h;
				dl_force = DX_SOLAPE;
				for(i = 0; i < MAX_SURF; i++){
					Surface *o = &surf[i];
					core_u32 oa, oe;
					if(!o->used || o == s) continue;
					oa = o->addr;
					oe = o->addr + o->stride * (core_u32)o->h * (core_u32)surf_bpp(o->fmt);
					if(sa < oe && oa < se){
						rtt_note_pair(o, sa > oa ? sa : oa, se < oe ? se : oe);
						drop_surface(o);
					}
				}
				dl_force = -1;
			}
			return s;
		}
		/* Crece: la VRAM pasa a mandar y se vuelve a subir */
		w = s->w > need_w ? s->w : need_w;
		h = s->h > need_h ? s->h : need_h;
		dl_force = DX_CRECE;
		drop_surface(s);
		dl_force = -1;
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
	dl_new_addr = addr; dl_new_fmt = fmt; dl_new_w = w; dl_new_h = h;
	dl_force = DX_SOLAPE;
	for(i = 0; i < MAX_SURF; i++){
		Surface *o = &surf[i];
		core_u32 oa, oe;
		if(!o->used) { if(!slot) slot = o; continue; }
		oa = o->addr;
		oe = o->addr + o->stride * (core_u32)o->h * (core_u32)surf_bpp(o->fmt);
		if(a < oe && oa < e){
			rtt_note_pair(o, a > oa ? a : oa, e < oe ? e : oe);
			if(!keep_with_hole(o, fmt, a, e)) drop_surface(o);
		}
		if(!o->used && !slot) slot = o;
	}
	dl_force = -1;
	if(!slot){
		/* Sin sitio: fuera el primero que no esté en el EFB */
		dl_force = DX_EXPULSA;
		for(i = 0; i < MAX_SURF && !slot; i++)
			if(&surf[i] != cur_c && &surf[i] != cur_z){ drop_surface(&surf[i]); slot = &surf[i]; }
		dl_force = -1;
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
	gx_tocado = 1;
}

/* --- Restaurar el EFB ---------------------------------------------------------- */

static void restore_color_body(void);

static void restore_color(void){
	int old = prof_switch(PROF_FB);
	restore_color_body();
	prof_switch(old);
}

static void restore_color_body(void){
	Surface *s = cur_c;
	if(s->hole_n && s->state != VRAM_NEWER && s->has_tex){
		u64 t0 = gettime();
		vram_to_tex_rows(s, s->hole0, s->hole0 + s->hole_n - 1);
		hole_rows_up += (unsigned)s->hole_n;
		hole_filled++;
		hole_ticks += gettime() - t0;
		s->hole_n = 0;
	}
	if(s->state == VRAM_NEWER || !s->has_tex){
		u64 t0 = gettime();
		wait_gpu();
		vram_to_tex(s);
		rtt_uploads++;
		rtt_up_ticks += gettime() - t0;
		{
			RttTarget *t = rtt_surface(s);
			if(t) t->up_ticks += gettime() - t0;
		}
		s->state = SYNCED;
		recompute_watch();
	}
	set_pf(surface_pf(s));
	blit(s, NULL, 1, s->amode == A_GPU, 0.0f, 0.0f, (float)s->w, (float)s->h, 1.0f, 1.0f, 0, -1);
	restored_c = 1;
	efb_c_newer = 0;
}

static void restore_depth_body(void);

static void restore_depth(void){
	int old = prof_switch(PROF_FB);
	restore_depth_body();
	prof_switch(old);
}

static void restore_depth_body(void){
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
	if(s){
		RttTarget *t = rtt_surface(s);
		if(t) t->binds++;
	}
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
		s->tex_gen++;
		const_alpha = -1;
	}
	/* El EFB pasa a RGBA6: hasta la próxima copia no hay alfa copiado */
	if(s->ci_kind == CI_A8) s->ci_valid = 0;
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

static void free_texture(int i){
	TexEntry *e = &texc[i];
	if(e->data){
		defer_free(e->data);
		tex_total -= e->bytes;
	}
	if(e == cur_tex) cur_tex = NULL;
	memset(e, 0, sizeof(*e));
	tc_free(&tcache, i);
}

/* Casilla para una textura nueva: una libre o la usada hace más tiempo */
static int alloc_texture_slot(TexCache *tc, void *ctx){
	int i, oldest = -1;
	(void)ctx;
	for(i = 0; i < TC_MAX; i++){
		if(!tc->e[i].used) return i;
		if(tc->e[i].used_frame != frame && (oldest < 0 || tc->e[i].used_frame < tc->e[oldest].used_frame)) oldest = i;
	}
	if(oldest < 0) oldest = 0;
	free_texture(oldest);
	return oldest;
}

static void trim_textures(void){
	while(tex_total > TEX_BUDGET){
		int i, oldest = -1;
		for(i = 0; i < TC_MAX; i++)
			if(tcache.e[i].used && tcache.e[i].used_frame != frame && &texc[i] != cur_tex &&
			   (oldest < 0 || tcache.e[i].used_frame < tcache.e[oldest].used_frame)) oldest = i;
		if(oldest < 0) break;
		free_texture(oldest);
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
	if(!src || (fmt >= GE_TFMT_CLUT16 && fmt <= GE_TFMT_CLUT32 && !opt_on[OPT_GE_TEX_CLUT]) || (int)bufw < w ||
	   (level && !r->use_shared_clut)){
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

	if(fmt == GE_TFMT_CLUT16 || fmt == GE_TFMT_CLUT32){
		/* ge_tex_clut: los framebuffers que el juego lee como textura con
		   paleta (efectos) llegan aquí y con el camino genérico costaban
		   ~38 ms cada uno en el Wii */
		if(!(w & 3) && !(h & 3) && tw == w){
			/* Por franjas de 4 filas: las 4 filas lineales caben en la
			   caché y cada tile se escribe entero de una vez */
			ge_clut16_32_prepare();
			for(y = 0; y < h; y += 4){
				ge_clut16_32_rows(src + (u32)y * pitch, pitch, w, 4, decode_buf, w);
				gx_tile_band_rgba8(data, tw, y, decode_buf, w);
			}
			return GX_TF_RGBA8;
		}
		ge_texture_decode_clut16_32(src, pitch, w, h, decode_buf, w);
		for(y = 0; y < h; y++)
			for(x = 0; x < w; x++) put_rgba8(tile_ptr(data, tw, x, y), decode_buf[y * w + x]);
		return GX_TF_RGBA8;
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
	/* (e->w, e->h y e->levels ya puestos) */
	int w = e->w, h = e->h, gxfmt = GX_TF_RGBA8, i;
	u32 bytes = 0, off = 0;
	u8 *data;
	for(i = 0; i < e->levels; i++){
		int lw = w >> i, lh = h >> i;
		bytes += level_bytes(lw ? lw : 1, lh ? lh : 1);
	}
	data = memalign(32, bytes);
	if(!data) return 0;
	/* Con ancho y alto múltiplos de 4, cada nivel escribe todos sus tiles:
	   borrar antes es escribir 2 veces (las texturas de los framebuffers
	   ocupan ~640 KB y se rehacen en cada imagen) */
	for(i = 0; i < e->levels; i++){
		int lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
		if((lw & 3) || (lh & 3)){ memset(data, 0, bytes); break; }
	}
	for(i = 0; i < e->levels; i++){
		int lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
		gxfmt = decode_into(r, i, data + off, lw, lh, round4(lw), e->levels == 1);
		off += level_bytes(lw, lh);
	}
	if(gxfmt == GX_TF_RGB565){
		/* 2 bytes por texel: solo se usó la primera mitad. Se pasa a un
		   búfer de su tamaño: la memoria y la cuenta del presupuesto
		   (tex_total) coinciden */
		u8 *half = memalign(32, bytes / 2);
		if(half){
			memcpy(half, data, bytes / 2);
			free(data);   /* la GPU todavía no lo ha visto */
			data = half;
			bytes /= 2;
		}   /* sin memoria se queda el grande, y se cuenta entero */
	}
	DCFlushRange(data, bytes);
	e->data = data;
	e->bytes = bytes;
	tex_total += bytes;
	GX_InitTexObj(&e->obj, data, (u16)w, (u16)h, (u8)gxfmt, GX_REPEAT, GX_REPEAT, e->levels > 1 ? GX_TRUE : GX_FALSE);
	GX_InvalidateTexAll();
	return 1;
}

/* Textura actual del GE (estado de ge_raster_begin) */
static TexEntry *lookup_texture_body(const GeRasterState *r);

static TexEntry *lookup_texture(const GeRasterState *r){
	int old = prof_switch(PROF_TEXTURAS);
	TexEntry *e = lookup_texture_body(r);
	prof_switch(old);
	return e;
}

/* Lo que decode_texture necesita al llamarla texcache.c */
static struct {
	const GeRasterState *r;
	int w, h, levels;
} want_tex;

static int decode_slot(TexCache *tc, void *ctx, int idx){
	TexEntry *e = &texc[idx];
	(void)tc; (void)ctx;
	memset(e, 0, sizeof(*e));
	e->w = want_tex.w;
	e->h = want_tex.h;
	e->levels = want_tex.levels;
	prof_decodes++;
	{
		core_u32 addr = want_tex.r->texaddr[0] & PSP_ADDR_MASK;
		int from = RO_RAM;
		u64 t0 = gettime();
		RttTarget *dt = NULL;
		if(addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE){
			Surface *fs = surface_at(addr - PSP_VRAM_BASE);
			from = fs ? RO_FB : RO_VRAM;
			if(fs){
				dt = rtt_surface(fs);
				if(dt) dt->dec++;
			}
		}
		if(!decode_texture(e, want_tex.r)){ memset(e, 0, sizeof(*e)); return 0; }
		if(dt) dt->dec_ticks += gettime() - t0;
		rtt_dec[from]++;
		rtt_dec_ticks[from] += gettime() - t0;
		rtt_dec_bytes[from] += e->bytes;
	}
	return 1;
}

/* ¿Hay un búfer dibujado por la GPU encima de esos bytes? Leerlos lo baja
   antes a la VRAM (gancho de memory.c), así que hay que comprobar. */
static int gpu_newer_range(void *ctx, core_u32 addr, core_u32 len){
	core_u32 a = addr & PSP_ADDR_MASK, off;
	int i;
	(void)ctx;
	if(a < PSP_VRAM_BASE || a >= PSP_VRAM_MIRROR_END || !len) return 0;
	off = (a - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1);
	if(off >= mem_vram_watch_hi[0] || off + len <= mem_vram_watch_lo[0]) return 0;
	for(i = 0; i < MAX_SURF; i++){
		core_u32 sa, se;
		if(!surf[i].used || surf[i].state != GPU_NEWER) continue;
		surf_range(&surf[i], &sa, &se);
		if(off < se && off + len > sa) return 1;
	}
	return 0;
}

/* La paleta se resume una vez por carga (LOADCLUT), no en cada dibujo */
static core_u32 clut_hash_now(void){
	if(!clut_memo_valid || clut_memo_gen != ge.clut_gen){
		clut_memo_hash = tc_hash(ge.clut, ge.clut_bytes ? ge.clut_bytes : 1024, 0x12345u);
		clut_memo_gen = ge.clut_gen;
		clut_memo_valid = 1;
		prof_clut_hashes++;
	}
	return clut_memo_hash;
}

static TexEntry *lookup_texture_body(const GeRasterState *r){
	int ws = r->width0_shift, hs = r->height0_shift, w, h, i, idx;
	u32 decodes = tcache.stats.decodes;
	TcQuery q;

	memset(&q, 0, sizeof(q));
	if(ws > 9) ws = 9;
	if(hs > 9) hs = 9;
	w = 1 << ws;
	h = 1 << hs;
	q.key.addr = r->texaddr[0];
	q.key.fmt = (core_u32)r->texfmt | ((core_u32)r->swizzle << 8);
	q.key.size = (core_u32)w | ((core_u32)h << 16);
	q.key.bufw = (core_u32)r->texbufw[0];
	q.key.levels = 1;
	/* Mipmaps: los niveles que van a la mitad, como los quiere GX */
	for(i = 1; i <= r->max_tex_level && i < TC_LEVELS; i++){
		int lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
		if(!r->texvalid[i] || r->size_w[i] != lw || r->size_h[i] != lh) break;
		q.key.levels = (core_u32)i + 1;
		q.key.mipkey = (q.key.mipkey ^ r->texaddr[i] ^ ((core_u32)r->texbufw[i] << 20)) * 0x9E3779B1u;
	}
	if(r->texfmt >= GE_TFMT_CLUT4 && r->texfmt <= GE_TFMT_CLUT32){
		q.key.clutfmt = r->clutformat;
		q.key.clut_hash = clut_hash_now();
	}
	/* Bytes de cada nivel; sin puntero contiguo (espejos con swizzle), 0 */
	for(i = 0; i < (int)q.key.levels; i++){
		int lh = h >> i ? h >> i : 1;
		u32 want = texture_bytes(r->texfmt, r->texbufw[i], lh);
		q.lv_addr[i] = r->texaddr[i];
		q.lv_len[i] = available_bytes(r->texaddr[i], want);
	}

	want_tex.r = r;
	want_tex.w = w;
	want_tex.h = h;
	want_tex.levels = (int)q.key.levels;
	tcache.frame = frame;
	tcache.backoff = lazy_textures;
	idx = tc_lookup(&tcache, &q);
	if(idx < 0) return NULL;
	if(tcache.stats.decodes != decodes) trim_textures();
	return &texc[idx];
}

/* La textura es un framebuffer que está en la GPU: se usa su copia sin
   pasar por la VRAM (efectos que dibujan en un búfer y lo usan después) */
static int texture_from_surface(const GeRasterState *r){
	core_u32 addr = r->texaddr[0] & PSP_ADDR_MASK;
	Surface *ts;
	if(r->texfmt > GE_FMT_8888 || r->swizzle || addr < PSP_VRAM_BASE || addr >= PSP_VRAM_BASE + PSP_VRAM_SIZE) return 0;
	ts = find_surface(addr - PSP_VRAM_BASE, r->texbufw[0], r->texfmt);
	if(!ts || ts->state == VRAM_NEWER || ts->hole_n) return 0;
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

/* --- Texturas con paleta desde la GPU (ge_rtt_gpu) ----------------------------- */

/* Carga la TLUT (256 entradas RGB565 de GX) en GX_TLUT0 si no es la que ya
   está */
static void load_tlut(const u16 *tl){
	if(tlut_loaded >= 0 && !memcmp(tlut_ring[tlut_loaded], tl, sizeof(tlut_ring[0]))) return;
	if(tlut_used >= TLUT_RING) wait_gpu();
	memcpy(tlut_ring[tlut_pos], tl, sizeof(tlut_ring[0]));
	DCFlushRange(tlut_ring[tlut_pos], sizeof(tlut_ring[0]));
	GX_InitTlutObj(&tlut_obj, tlut_ring[tlut_pos], GX_TL_RGB565, 256);
	GX_LoadTlut(&tlut_obj, GX_TLUT0);
	tlut_loaded = tlut_pos;
	tlut_pos = (tlut_pos + 1) % TLUT_RING;
	tlut_used++;
	clut_tlut_loads++;
}

/* Índices CI8 de un búfer 565 a partir de su copia RGBA8 */
static int build_ci565(Surface *ts, u32 shift, u32 mask){
	int old = prof_switch(PROF_TEXTURAS);
	u64 t0 = gettime();
	int cw = ci_width(ts);
	if(!ensure_ci(ts)){ prof_switch(old); return 0; }
	wait_gpu();   /* la copia de efb_to_tex puede no haber terminado */
	DCInvalidateRange(ts->tex, (u32)(ts->w * ts->h * 4));
	gx_ci8_from_rgba8_565(ts->ci, cw, ts->tex, ts->w, ts->h, shift, mask);
	DCFlushRange(ts->ci, (u32)cw * (u32)ts->h);
	GX_InvalidateTexAll();
	ts->ci_kind = CI_565;
	ts->ci_gen = ts->tex_gen;
	ts->ci_key = shift | (mask << 8);
	ts->ci_valid = 1;
	clut_ci_builds++;
	clut_ci_ticks += gettime() - t0;
	prof_switch(old);
	return 1;
}

/* Una textura con paleta que lee un framebuffer que está en la GPU: la GPU
   busca en la paleta (TLUT 565) con índices CI8 sacados de la copia del
   EFB, sin bajar el búfer ni decodificarlo en la CPU:
     - CLUT32 que lee el alfa (desplazamiento >= 24) de un 8888 con el alfa
       en la GPU: los índices son una copia A8 del EFB (efb_to_tex);
     - CLUT16 sobre un 565: los índices salen de la copia RGBA8.
   Solo con paleta 565 (la TLUT RGB565 da el mismo color que la PSP), sin
   mipmaps ni swizzle y con clamp. La textura de GX es del tamaño de la de
   la PSP, con las mismas coordenadas que el camino de la CPU; cada
   primitiva comprueba antes que no lee más allá del búfer (clut_gpu_*),
   donde la PSP leería otra memoria, y si no, vuelve al camino de la CPU. */
static int clut_from_surface(const GeRasterState *r){
	core_u32 addr = r->texaddr[0] & PSP_ADDR_MASK, shift, mask, offset;
	const u32 *pal;
	Surface *ts;
	u16 tl[256];
	int tw = 1 << r->width0_shift, th = 1 << r->height0_shift, i, kind;
	if(!opt_on[OPT_GE_RTT_GPU]) return 0;
	if(r->texfmt != GE_TFMT_CLUT16 && r->texfmt != GE_TFMT_CLUT32) return 0;
	if(r->clut_fmt != GE_FMT_565 || r->swizzle || !r->clamp_s || !r->clamp_t || r->texture_proj || r->max_tex_level > 0)
		return 0;
	if(addr < PSP_VRAM_BASE || addr >= PSP_VRAM_BASE + PSP_VRAM_SIZE) return 0;
	ts = find_surface(addr - PSP_VRAM_BASE, r->texbufw[0], r->texfmt == GE_TFMT_CLUT32 ? GE_FMT_8888 : GE_FMT_565);
	if(!ts || ts->gpu_bad >= 4 || ts->state == VRAM_NEWER || ts->hole_n) return 0;
	if(tw != ci_width(ts) || th < ts->h || tw > MAX_TEX_SIZE || th > MAX_TEX_SIZE) return 0;
	kind = r->texfmt == GE_TFMT_CLUT32 ? CI_A8 : CI_565;
	if(kind == CI_A8 && ts->amode != A_GPU) return 0;
	ge_clut16_32_prepare();
	pal = ge_clut16_32_info(&shift, &mask, &offset);
	if(kind == CI_A8 && shift < 24) return 0;
	if(ts == cur_c && efb_c_newer){ efb_to_tex(ts); efb_c_newer = 0; }
	if(!ts->has_tex) return 0;
	if(kind == CI_A8){
		/* La copia del alfa se hace al sacar el búfer del EFB: desde la
		   próxima vez */
		if(!ts->a8_want){ ts->a8_want = 1; return 0; }
		if(ts->ci_kind != CI_A8 || !ts->ci_valid) return 0;
		for(i = 0; i < 256; i++) tl[i] = gx_tlut565(pal[((((u32)i << 24) >> shift) & mask) | offset]);
	} else {
		if(!(ts->ci_kind == CI_565 && ts->ci_valid && ts->ci_gen == ts->tex_gen && ts->ci_key == (shift | (mask << 8))) &&
		   !build_ci565(ts, shift, mask))
			return 0;
		for(i = 0; i < 256; i++) tl[i] = gx_tlut565(pal[(u32)i | offset]);
	}
	load_tlut(tl);
	GX_InitTexObjCI(&clut_obj, ts->ci, (u16)tw, (u16)th, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
	clut_src = ts;
	clut_gpu_uses[kind]++;
	return 1;
}

/* Último texel leído a lo largo de un eje de un sprite, contando solo los
   píxeles que se dibujan (dentro de [clip_lo, clip_hi]) y el vecino del
   filtro lineal solo si pesa; -1 si no dibuja nada */
static int axis_last_texel(float p0, float p1, float c0, float c1, int linear, int clip_lo, int clip_hi){
	float lo = p0 < p1 ? p0 : p1, hi = p0 < p1 ? p1 : p0;
	float first = ceilf(lo - 0.5f) + 0.5f, last = ceilf(hi - 0.5f) - 0.5f, a, b, m, f;
	int i;
	if(first < (float)clip_lo + 0.5f) first = (float)clip_lo + 0.5f;
	if(last > (float)clip_hi + 0.5f) last = (float)clip_hi + 0.5f;
	if(last < first || p1 == p0) return -1;
	a = c0 + (first - p0) * (c1 - c0) / (p1 - p0);
	b = c0 + (last - p0) * (c1 - c0) / (p1 - p0);
	m = a > b ? a : b;
	if(!(m < 65536.0f)) return 1 << 20;
	if(!linear) return (int)floorf(m);
	f = m - 0.5f;
	i = (int)floorf(f);
	return f > (float)i ? i + 1 : i;
}

/* Zona que se dibuja (tijera dentro del destino), como en apply_gx */
static void draw_clip(int *x1, int *y1, int *x2, int *y2){
	const GeRasterState *r = ge_raster_state();
	*x1 = r->sc_x1 / 16; *y1 = r->sc_y1 / 16;
	*x2 = r->sc_x2 / 16; *y2 = r->sc_y2 / 16;
	if(*x2 >= cur_c->w) *x2 = cur_c->w - 1;
	if(*y2 >= cur_c->h) *y2 = cur_c->h - 1;
}

static float clut_texel(const GeVertex *v, int t){
	float c = t ? v->t : v->s;
	if(!through) c *= (float)(1 << (t ? ge_raster_state()->height0_shift : ge_raster_state()->width0_shift));
	return c;
}

/* ¿El sprite solo lee texels del búfer de origen? */
static int clut_gpu_sprite_ok(const GeVertex *v0, const GeVertex *v1){
	const GeRasterState *r = ge_raster_state();
	int linear = r->mag_filt || r->min_filt, swap = (v0->x < v1->x) != (v0->y < v1->y), x1, y1, x2, y2, c, w;
	float px0 = (float)v0->x / 16.0f, px1 = (float)v1->x / 16.0f, py0 = (float)v0->y / 16.0f, py1 = (float)v1->y / 16.0f;
	float s0 = clut_texel(v0, 0), s1 = clut_texel(v1, 0), t0 = clut_texel(v0, 1), t1 = clut_texel(v1, 1);
	if(!through) return 0;   /* la perspectiva de los sprites no es lineal */
	draw_clip(&x1, &y1, &x2, &y2);
	if(swap){
		c = axis_last_texel(py0, py1, s0, s1, linear, y1, y2);
		w = axis_last_texel(px0, px1, t0, t1, linear, x1, x2);
	} else {
		c = axis_last_texel(px0, px1, s0, s1, linear, x1, x2);
		w = axis_last_texel(py0, py1, t0, t1, linear, y1, y2);
	}
	return c < clut_src->w && w < clut_src->h;
}

/* Triángulos: el muestreo no sale de los vértices; con filtro lineal, el
   vecino del último texel pesa 0 si la coordenada no pasa de su centro */
static int clut_gpu_triangle_ok(const GeVertex *const *v){
	const GeRasterState *r = ge_raster_state();
	float lim_s = (float)clut_src->w, lim_t = (float)clut_src->h;
	int i;
	if(r->mag_filt || r->min_filt){ lim_s -= 0.5f; lim_t -= 0.5f; }
	for(i = 0; i < 3; i++){
		float s = clut_texel(v[i], 0), t = clut_texel(v[i], 1);
		if(r->mag_filt || r->min_filt){ if(!(s <= lim_s && t <= lim_t)) return 0; }
		else if(!(s < lim_s && t < lim_t)) return 0;
	}
	return 1;
}

/* Esta primitiva leería fuera del búfer: de aquí al final del dibujo, el
   camino de la CPU (si le pasa a menudo a un búfer, ya no se intenta) */
static void clut_gpu_fallback(void){
	const GeRasterState *r = ge_raster_state();
	if(clut_src) clut_src->gpu_bad++;
	use_clut_gpu = 0;
	clut_src = NULL;
	clut_gpu_fallbacks++;
	cur_tex = lookup_texture(r);
	gx_dirty = 1;
	gx_tocado = 1;
	tri_listo = 0;
}

/* --- Estado de GX a partir del GE ---------------------------------------------- */

/* --- Censo de estado (prof_estado) ----------------------------------------------
   Si lo que sale de setup_targets y lo que lee apply_gx es lo mismo que en la
   llamada anterior, y si entre medias hubo avisos (VRAM escrita, búferes
   quitados, presentar) o alguien más tocó GX (copias, vuelta a la CPU).
   Solo mide: no cambia nada. */
typedef struct {
	Surface *c, *z, *clut_src;
	TexEntry *tex;
	RttPal *rtt;
	int skip, stencil, rda, wc, wz, tc, tz, through, tex_on, use_rtt, use_clut, tex_proj, flat;
	float su, sv, itw, ith;
	core_u32 rtt_off;
} CsDst;

typedef struct {
	GeRasterState r;
	Surface *c, *z;
	TexEntry *tex;
	int fmt, amode, ualpha, w, h, through, tex_on, use_rtt, use_clut, levels, mode;
	GXTexObj obj;
	core_u32 mrgb, ma, lmode;
} CsGx;

static CsDst cs_dst;
static CsGx cs_gx;
static int cs_dst_ok, cs_gx_ok;

static void __attribute__((noinline)) census_dst(void){
	CsDst d;
	memset(&d, 0, sizeof(d));   /* el relleno también: se compara con memcmp */
	d.c = cur_c; d.z = cur_z; d.clut_src = clut_src; d.tex = cur_tex; d.rtt = rtt_cur;
	d.skip = skip_draws; d.stencil = stencil_val; d.rda = reads_dst_alpha;
	d.wc = writes_c; d.wz = writes_z; d.tc = touch_c; d.tz = touch_z;
	d.through = through; d.tex_on = tex_on; d.use_rtt = use_rtt; d.use_clut = use_clut_gpu;
	d.tex_proj = tex_proj; d.flat = flat;
	d.su = tex_su; d.sv = tex_sv; d.itw = inv_tw; d.ith = inv_th;
	d.rtt_off = rtt_tex_off;
	ge_stats.cs_dst++;
	ge_stats.cs_dst_igual += cs_dst_ok && !memcmp(&d, &cs_dst, sizeof(d));
	ge_stats.cs_dst_aviso += cs_aviso;
	cs_aviso = 0;
	if(d.tex_on && !d.skip && !d.use_rtt && !d.use_clut){
		ge_stats.cs_tex++;
		ge_stats.cs_tex_igual += cs_dst_ok && cs_dst.tex_on && d.tex == cs_dst.tex;
	}
	memcpy(&cs_dst, &d, sizeof(d));
	cs_dst_ok = 1;
}

static void __attribute__((noinline)) census_gx(void){
	CsGx g;
	memset(&g, 0, sizeof(g));
	memcpy(&g.r, ge_raster_state(), sizeof(g.r));
	memset(g.r.snap_size, 0, sizeof(g.r.snap_size));
	g.c = cur_c; g.z = cur_z; g.tex = cur_tex;
	if(cur_c){ g.fmt = cur_c->fmt; g.amode = cur_c->amode; g.ualpha = cur_c->ualpha; g.w = cur_c->w; g.h = cur_c->h; }
	g.through = through; g.tex_on = tex_on; g.use_rtt = use_rtt; g.use_clut = use_clut_gpu; g.mode = mode;
	if(use_rtt) memcpy(&g.obj, &rtt_obj, sizeof(g.obj));
	else if(use_clut_gpu) memcpy(&g.obj, &clut_obj, sizeof(g.obj));
	else if(cur_tex){ memcpy(&g.obj, &cur_tex->obj, sizeof(g.obj)); g.levels = cur_tex->levels; }
	g.mrgb = ge.cmd[GE_MASKRGB]; g.ma = ge.cmd[GE_MASKALPHA]; g.lmode = ge.cmd[GE_LIGHTMODE];
	ge_stats.cs_gx++;
	ge_stats.cs_gx_igual += cs_gx_ok && !gx_tocado && !memcmp(&g, &cs_gx, sizeof(g));
	ge_stats.cs_gx_tocado += gx_tocado;
	memcpy(&cs_gx, &g, sizeof(g));
	cs_gx_ok = 1;
}

/* Cada primitiva que llega a GX: ¿haría falta preparar algo o marcar el
   búfer como dibujado? (lo que miran prepare_idle y drawn) */
static int estado_listo(void);

static void __attribute__((noinline)) census_prim(void){
	int listo = estado_listo();
	ge_stats.cs_tri++;
	ge_stats.cs_tri_listo += listo;
	if(cs_primero){ ge_stats.cs_tri_primero++; cs_primero = 0; }
	else if(!listo) ge_stats.cs_tri_mitad++;
}

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
	rtt_cur = NULL;
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
	use_clut_gpu = 0;
	clut_src = NULL;
	tex_su = tex_sv = 1.0f;
	if(!writes_c) tex_on = 0;
	if(tex_on){
		core_u32 ta = r->texaddr[0] & PSP_ADDR_MASK;
		rtt_tex_off = ta >= PSP_VRAM_BASE && ta < PSP_VRAM_BASE + PSP_VRAM_SIZE ? ta - PSP_VRAM_BASE : 0xFFFFFFFFu;
		use_rtt = r->texvalid[0] && texture_from_surface(r);
		if(!use_rtt && r->texvalid[0]) use_clut_gpu = clut_from_surface(r);
		if(r->texvalid[0]) rtt_note_texture(r, use_rtt ? 1 : use_clut_gpu ? 2 : 0);
		if(!use_rtt && !use_clut_gpu) cur_tex = r->texvalid[0] ? lookup_texture(r) : NULL;
		if(rtt_cur){
			rtt_cur->linear += r->mag_filt || r->min_filt;
			rtt_cur->clamp_s += r->clamp_s != 0;
			rtt_cur->clamp_t += r->clamp_t != 0;
		}
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
		} else if(use_clut_gpu){
			GX_InitTexObjFilterMode(&clut_obj, r->min_filt ? GX_LINEAR : GX_NEAR, r->mag_filt ? GX_LINEAR : GX_NEAR);
			GX_LoadTexObj(&clut_obj, GX_TEXMAP0);
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

/* Censo: el sprite (v0, v1) tapa todo el destino actual: 1 opaco (todo
   lo de antes se pierde), 2 con mezcla, pruebas o máscara (lo necesita), 0 no */
static int rtt_cover(const GeVertex *v0, const GeVertex *v1){
	const GeRasterState *r = ge_raster_state();
	int xl = (v0->x < v1->x ? v0->x : v1->x) / 16, xr = (v0->x > v1->x ? v0->x : v1->x) / 16;
	int yt = (v0->y < v1->y ? v0->y : v1->y) / 16, yb = (v0->y > v1->y ? v0->y : v1->y) / 16;
	int sx1 = r->sc_x1 / 16, sy1 = r->sc_y1 / 16, sx2 = r->sc_x2 / 16 + 1, sy2 = r->sc_y2 / 16 + 1;
	if(xl < sx1) xl = sx1;
	if(yt < sy1) yt = sy1;
	if(xr > sx2) xr = sx2;
	if(yb > sy2) yb = sy2;
	if(xl > 0 || yt > 0 || xr < cur_c->w || yb < cur_c->h) return 0;
	if(r->alpha_blend || r->apply_logic_op || (ge.cmd[GE_MASKRGB] & 0xFFFFFF) || r->alpha_test_func != CMP_ALWAYS ||
	   r->color_test || r->stencil_test || (touch_z && r->depth_test_func != CMP_ALWAYS))
		return 2;
	return 1;
}

/* Antes de cada primitiva: búferes en el EFB con su contenido. full: el
   rectángulo de borrado tapa todo el búfer de color (1) y/o de
   profundidad (2), que no hace falta restaurar. 0 = no dibujar. */
static int prepare_targets(int full){
	if(targets_dirty){
		u64 t0 = gettime();
		int phase = prof_ge_enter(GEF_ESTADO);
		setup_targets();
		targets_dirty = 0;
		gx_dirty = 1;
		if(opt_on[OPT_PROF_ESTADO]) census_dst();
		prof_ge_leave(phase);
		prof_setup += gettime() - t0;
	}
	if(skip_draws) return 0;
	if(touch_c && !restored_c){
		RttTarget *t = rtt_surface(cur_c);
		if(full & 1){
			restored_c = 1;
			if(cur_c->hole_n){ cur_c->hole_n = 0; hole_skips++; }   /* se borra entero */
			if(t) t->first_clear++;
		} else {
			if(t){
				if(cur_c->state == VRAM_NEWER || !cur_c->has_tex) t->first_upload++;
				else t->first_blit++;
				if(first_v0){
					int c = rtt_cover(first_v0, first_v1);
					if(c == 1) t->cover_opaque++;
					else if(c == 2) t->cover_blend++;
				}
			}
			restore_color();
		}
	}
	if(touch_z && cur_z && !restored_z){
		if(full & 2) restored_z = 1;
		else restore_depth();
	}
	return 1;
}

/* --- Estado sucio (ge_estado_sucio) en GX ----------------------------------------
   apply_gx es función de lo que lee: el estado del rasterizador (ge_raster_gen
   sube cada vez que se calcula de nuevo), los búferes en el EFB, la textura
   (con los bytes de su objeto de GX) y unos pocos comandos. Si todo es lo de
   la última vez y nadie más tocó GX (gx_tocado), no hace falta emitirlo. */
typedef struct {
	u32 rgen;
	Surface *c, *z;
	TexEntry *tex;
	int fmt, amode, ualpha, w, h, through, tex_on, use_rtt, use_clut, levels, mode;
	GXTexObj obj;
	core_u32 mrgb, ma, lmode;
} GxKey;
typedef char gx_key_words[sizeof(GxKey) % 4 == 0 ? 1 : -1];

static GxKey gx_key_last;
static int gx_key_ok;

static void gx_key(GxKey *k){
	memset(k, 0, sizeof(*k));   /* el relleno también: se compara con memcmp */
	k->rgen = ge_raster_gen;
	k->c = cur_c; k->z = cur_z; k->tex = cur_tex;
	if(cur_c){ k->fmt = cur_c->fmt; k->amode = cur_c->amode; k->ualpha = cur_c->ualpha; k->w = cur_c->w; k->h = cur_c->h; }
	k->through = through; k->tex_on = tex_on; k->use_rtt = use_rtt; k->use_clut = use_clut_gpu; k->mode = mode;
	if(use_rtt) memcpy(&k->obj, &rtt_obj, sizeof(k->obj));
	else if(use_clut_gpu) memcpy(&k->obj, &clut_obj, sizeof(k->obj));
	else if(cur_tex){ memcpy(&k->obj, &cur_tex->obj, sizeof(k->obj)); k->levels = cur_tex->levels; }
	k->mrgb = ge.cmd[GE_MASKRGB]; k->ma = ge.cmd[GE_MASKALPHA]; k->lmode = ge.cmd[GE_LIGHTMODE];
}

/* ge_verificar: el estado interno de libogc (__gxregs, 0x598 bytes; su
   dirección se lee de las dos primeras instrucciones que la cargan en
   GX_SetTevOrder). Al volver a emitir lo que se habría saltado, si saltarlo
   era correcto no cambia nada salvo las banderas de estado sucio (palabra
   48). Sin él (otra libogc, el banco de qemu) no se comprueba. */
#define GX_SHADOW_SIZE 0x598
#define GX_SHADOW_DIRTY 48

static const u8 *gx_shadow(void){
	static int tried;
	static const u8 *p;
	if(!tried){
		const u32 *code = (const u32 *)(const void *)GX_SetTevOrder;
		int i, reg = -1;
		u32 hi = 0;
		tried = 1;
		for(i = 0; i < 12 && !p; i++){
			u32 w = code[i];
			if((w & 0xFC1F0000u) == 0x3C000000u){ reg = (int)((w >> 21) & 31); hi = w & 0xFFFFu; continue; }   /* lis */
			if(reg >= 0 && (w >> 26) == 14 && (int)((w >> 21) & 31) == reg && (int)((w >> 16) & 31) == reg)      /* addi */
				p = (const u8 *)(uintptr_t)((hi << 16) + (u32)(s32)(s16)(w & 0xFFFFu));
		}
		if(p && ((uintptr_t)p < 0x80000000u || (uintptr_t)p + GX_SHADOW_SIZE > 0x81800000u)) p = NULL;
	}
	return p;
}

static void __attribute__((noinline)) gx_emit_verify(void){
	static u8 before[GX_SHADOW_SIZE];
	static unsigned logged;
	const u8 *sh = gx_shadow();
	int i;
	if(!sh){ ge_stats.es_gx_verif_sin++; apply_gx(); return; }
	memcpy(before, sh, sizeof(before));
	apply_gx();
	ge_stats.es_gx_verif++;
	for(i = 0; i < GX_SHADOW_SIZE; i++){
		if(i >= GX_SHADOW_DIRTY && i < GX_SHADOW_DIRTY + 4) continue;
		if(before[i] != sh[i]) break;
	}
	if(i == GX_SHADOW_SIZE) return;
	ge_stats.es_gx_verif_bad++;
	if(logged++ < 16)
		hle_log_quiet("[VERIF] estado de GX: se habria saltado y cambia el byte 0x%03X de libogc (%02X -> %02X)\n",
		              (unsigned)i, (unsigned)before[i], (unsigned)sh[i]);
}

/* Palabra a palabra (memcmp de newlib va byte a byte si no sabe la alineación) */
static inline int gx_key_eq(const GxKey *a, const GxKey *b){
	const core_u32 *x = (const core_u32 *)(const void *)a, *y = (const core_u32 *)(const void *)b;
	core_u32 d = 0;
	unsigned i;
	for(i = 0; i < sizeof(GxKey) / 4; i++) d |= x[i] ^ y[i];
	return !d;
}

static void gx_emit_state(void){
	GxKey k;
	gx_key(&k);
	ge_stats.es_gx++;
	if(gx_key_ok && !gx_tocado && gx_key_eq(&k, &gx_key_last)){
		/* Lo mismo que la vez anterior: con ge_verificar se emite igual y se comprueba */
		if(opt_on[OPT_GE_VERIF]) gx_emit_verify();
		else ge_stats.es_gx_saltado++;
		return;
	}
	apply_gx();
	memcpy(&gx_key_last, &k, sizeof(k));
	gx_key_ok = 1;
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
		int phase = prof_ge_enter(GEF_ESTADO);
		if(opt_on[OPT_PROF_ESTADO]) census_gx();
		if(opt_on[OPT_GE_ESTADO_SUCIO]) gx_emit_state();
		else apply_gx();
		gx_tocado = 0;
		gx_dirty = 0;
		prof_ge_leave(phase);
		prof_state += gettime() - t0;
	}
}

static int prepare(int full){
	if(!prepare_targets(full)) return 0;
	prepare_state();
	return 1;
}

/* prepare(0) no haría nada: ni búferes ni estado nuevos, ni restaurar, ni
   alfa por píxel que promover (gx_envio_rapido) */
static inline int prepare_idle(void){
	return !targets_dirty && !skip_draws && (!touch_c || restored_c) && (!touch_z || !cur_z || restored_z) &&
	       stencil_val < 0 && !reads_dst_alpha && !want_promote && !gx_dirty;
}

/* ge_estado_sucio: ni prepare(0) ni drawn() harían nada. Más fino que
   prepare_idle: con alfa por stencil o mezcla que lee el alfa del destino,
   prepare_state no hace nada si el búfer ya tiene el alfa en la GPU (o no
   tiene alfa, o es el mismo uniforme) */
static int estado_listo(void){
	int promote;
	if(targets_dirty || skip_draws || !cur_c || gx_dirty || want_promote) return 0;
	if((touch_c && !restored_c) || (touch_z && cur_z && !restored_z)) return 0;
	if((writes_c && !efb_c_newer) || (writes_z && cur_z && !efb_z_newer)) return 0;
	promote = (stencil_val >= 0 && !(cur_c->amode == A_UNIFORM && stencil_val == cur_c->ualpha)) ||
	          (reads_dst_alpha && cur_c->amode == A_VRAM);
	return !promote || cur_c->amode == A_GPU || !has_alpha_fmt(cur_c->fmt);
}

static inline void drawn(void){
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

static inline void put_words(const GxVtxWords *w){
	put(w->f[0], w->f[1], w->f[2], w->f[3], w->f[4], w->f[5], w->c[0], w->c[1]);
}

/* El estado de gx_emit.h, de los globales de aquí */
static inline void emit_ctx(GxEmitCtx *e){
	e->tex_on = tex_on; e->through = through; e->tex_proj = tex_proj; e->flat = flat; e->tex_norm = tex_norm;
	e->inv_tw = inv_tw; e->inv_th = inv_th; e->tex_su = tex_su; e->tex_sv = tex_sv; e->pos_off = pos_off;
}

/* v da posición, uv y niebla; cv los colores; k el factor de gx_tex_scale3
   (el de siempre; gx_vertex_ref en gx_emit.h hace las mismas cuentas) */
static inline void put_vertex(const GeVertex *v, const GeVertex *cv, float k){
	float s = 0.0f, t = 0.0f, q = 1.0f;
	if(tex_on){
		s = gx_finite_uv(v->s); t = gx_finite_uv(v->t);
		if(through){ s *= inv_tw; t *= inv_th; }
		else {
			float iw = k / v->clipw;
			if(tex_proj) q = v->q;
			s *= iw * tex_su; t *= iw * tex_sv; q *= iw;
		}
	}
	put((float)v->x * (1.0f / 16.0f) + pos_off, (float)v->y * (1.0f / 16.0f) + pos_off, (float)v->z, s, t, q,
	    cv->color0, (cv->color1 & 0xFFFFFF) | ((core_u32)gx_fog_byte(v->fogdepth) << 24));
}

/* --- Censo: qué parte de una textura sobre un framebuffer se lee ----------- */

/* Coordenada de textura en texels */
static float rtt_texel(const GeVertex *v, int t){
	float c = t ? v->t : v->s;
	if(!through) c *= (float)(t ? rtt_cur->th : rtt_cur->tw);
	return c;
}

static void rtt_bounds(const GeVertex *v){
	float u = rtt_texel(v, 0), t = rtt_texel(v, 1);
	if(u < rtt_cur->umin) rtt_cur->umin = u;
	if(u > rtt_cur->umax) rtt_cur->umax = u;
	if(t < rtt_cur->vmin) rtt_cur->vmin = t;
	if(t > rtt_cur->vmax) rtt_cur->vmax = t;
}

/* Último texel leído a lo largo de un eje de un sprite: posiciones p0, p1
   (píxeles) con coordenadas c0, c1 (texels). GX muestrea en el centro de
   cada píxel cubierto; con filtro lineal lee floor(c - 0.5) y el siguiente,
   que solo cuenta si su peso no es 0. -1 si no cubre ningún píxel. */
static float rtt_axis_m, rtt_axis_w;   /* coordenada máxima y peso del vecino del último eje */

static int rtt_axis_last(float p0, float p1, float c0, float c1, int linear, unsigned *neighbor){
	float lo = p0 < p1 ? p0 : p1, hi = p0 < p1 ? p1 : p0;
	float first = ceilf(lo - 0.5f) + 0.5f, last = ceilf(hi - 0.5f) - 0.5f, a, b, m, f;
	int i;
	if(last < first || p1 == p0) return -1;
	a = c0 + (first - p0) * (c1 - c0) / (p1 - p0);
	b = c0 + (last - p0) * (c1 - c0) / (p1 - p0);
	m = a > b ? a : b;
	rtt_axis_m = m;
	rtt_axis_w = 0.0f;
	if(!linear) return (int)floorf(m);
	f = m - 0.5f;
	i = (int)floorf(f);
	if(f > (float)i){ (*neighbor)++; rtt_axis_w = f - (float)i; return i + 1; }
	return i;
}

/* Forma de un sprite: tamaño en píxeles, texels por píxel y desfase (texel
   que cae en el centro del primer píxel menos el que tocaría a escala) */
static void rtt_note_kind(float x0, float x1, float y0, float y1, float s0, float s1, float t0, float t1){
	int w = (int)fabsf(x1 - x0), h = (int)fabsf(y1 - y0), k, free_k = -1;
	float ku = x1 != x0 ? (s1 - s0) / (x1 - x0) : 0.0f, kv = y1 != y0 ? (t1 - t0) / (y1 - y0) : 0.0f;
	float du = s0 - x0 * ku, dv = t0 - y0 * kv;
	du = floorf(du * 64.0f + 0.5f) / 64.0f;
	dv = floorf(dv * 64.0f + 0.5f) / 64.0f;
	for(k = 0; k < 4; k++){
		if(!rtt_cur->kind[k].n){ if(free_k < 0) free_k = k; continue; }
		if(rtt_cur->kind[k].w == w && rtt_cur->kind[k].h == h && rtt_cur->kind[k].ku == ku && rtt_cur->kind[k].kv == kv &&
		   rtt_cur->kind[k].du == du && rtt_cur->kind[k].dv == dv){ rtt_cur->kind[k].n++; return; }
	}
	if(free_k >= 0){
		rtt_cur->kind[free_k].w = w; rtt_cur->kind[free_k].h = h;
		rtt_cur->kind[free_k].ku = ku; rtt_cur->kind[free_k].kv = kv;
		rtt_cur->kind[free_k].du = du; rtt_cur->kind[free_k].dv = dv;
		rtt_cur->kind[free_k].n = 1;
	}
}

static void rtt_note_sprite(const GeVertex *v0, const GeVertex *v1){
	const GeRasterState *r = ge_raster_state();
	int linear = r->mag_filt || r->min_filt, swap = (v0->x < v1->x) != (v0->y < v1->y), c, w;
	float x0 = (float)v0->x / 16.0f, x1 = (float)v1->x / 16.0f, y0 = (float)v0->y / 16.0f, y1 = (float)v1->y / 16.0f;
	float s0 = rtt_texel(v0, 0), s1 = rtt_texel(v1, 0), t0 = rtt_texel(v0, 1), t1 = rtt_texel(v1, 1);
	rtt_cur->prims++;
	rtt_cur->sprites++;
	rtt_cur->through += through;
	rtt_bounds(v0);
	rtt_bounds(v1);
	/* Con la orientación cambiada, s va a lo largo de y y t de x (draw_sprite) */
	c = swap ? rtt_axis_last(y0, y1, s0, s1, linear, &rtt_cur->neighbor_s) : rtt_axis_last(x0, x1, s0, s1, linear, &rtt_cur->neighbor_s);
	if(c >= 0){
		if(rtt_axis_m > rtt_cur->smax) rtt_cur->smax = rtt_axis_m;
		if(rtt_cur->src_w && c >= rtt_cur->src_w){
			rtt_cur->beyond_s++;
			if(rtt_axis_w > rtt_cur->beyond_ws) rtt_cur->beyond_ws = rtt_axis_w;
		}
	}
	w = swap ? rtt_axis_last(x0, x1, t0, t1, linear, &rtt_cur->neighbor_t) : rtt_axis_last(y0, y1, t0, t1, linear, &rtt_cur->neighbor_t);
	if(w >= 0){
		if(rtt_axis_m > rtt_cur->tmax) rtt_cur->tmax = rtt_axis_m;
		if(rtt_cur->src_h && w >= rtt_cur->src_h){
			rtt_cur->beyond_t++;
			if(rtt_axis_w > rtt_cur->beyond_wt) rtt_cur->beyond_wt = rtt_axis_w;
		}
	}
	if(c > rtt_cur->col_max) rtt_cur->col_max = c;
	if(w > rtt_cur->row_max) rtt_cur->row_max = w;
	if(!swap) rtt_note_kind(x0, x1, y0, y1, s0, s1, t0, t1);
}

/* Triángulos: solo los límites de los vértices (y el vecino, por si acaso) */
static void rtt_note_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	const GeVertex *v[3] = { v0, v1, v2 };
	int i;
	rtt_cur->prims++;
	for(i = 0; i < 3; i++){
		int c = (int)ceilf(rtt_texel(v[i], 0)), w = (int)ceilf(rtt_texel(v[i], 1));
		rtt_bounds(v[i]);
		if(c > rtt_cur->col_max) rtt_cur->col_max = c;
		if(w > rtt_cur->row_max) rtt_cur->row_max = w;
	}
}

/* gx_envio_rapido / ge_verificar: triángulos comprobados contra la referencia */
static u32 verif_tris, verif_tris_bad, verif_tris_logged;

static void __attribute__((noinline)) verify_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	GxVtxWords r[3], w[3];
	GxEmitCtx ec, *e = &ec;
	int i;
	emit_ctx(e);
	gx_triangle_ref(r, v0, v1, v2, e);
	gx_triangle_fast(w, v0, v1, v2, e);
	verif_tris++;
	if(!memcmp(r, w, sizeof(r))) return;
	verif_tris_bad++;
	if(verif_tris_logged++ < 16){
		u32 a[24], b[24];
		char line[400];
		size_t len;
		memcpy(a, r, sizeof(a));
		memcpy(b, w, sizeof(b));
		len = (size_t)snprintf(line, sizeof(line), "[VERIF] GX triangulo (tex %d through %d proj %d plano %d norm %d):",
		                       e->tex_on, e->through, e->tex_proj, e->flat, e->tex_norm);
		for(i = 0; i < 24 && len + 32 < sizeof(line); i++)
			if(a[i] != b[i]) len += (size_t)snprintf(line + len, sizeof(line) - len, " [%d] %08X/%08X", i, (unsigned)a[i], (unsigned)b[i]);
		hle_log_quiet("%s\n", line);
	}
}

/* El de siempre (sin gx_envio_rapido) */
static void __attribute__((noinline)) gx_triangle_ref_path(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	const GeVertex *c;
	float k;
	if(!prepare(0)) return;
	if(use_clut_gpu && tex_on){
		const GeVertex *vv[3] = { v0, v1, v2 };
		if(!clut_gpu_triangle_ok(vv)){
			clut_gpu_fallback();
			prepare_state();
		}
	}
	if(rtt_cur && tex_on) rtt_note_triangle(v0, v1, v2);
	c = flat ? v2 : NULL;
	k = tex_on && !through && tex_norm ? gx_tex_scale3(v0, v1, v2) : 1.0f;
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
	put_vertex(v0, c ? c : v0, k);
	put_vertex(v1, c ? c : v1, k);
	put_vertex(v2, c ? c : v2, k);
	GX_End();
	drawn();
}

/* gx_envio_rapido: entre dos triángulos de la misma llamada no suele haber
   nada que preparar (se mira sin llamar a nadie) y los valores de los tres
   vértices se calculan antes de escribir el primero (gx_emit.h) */
static void gx_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	GxVtxWords w;
	GxEmitLocal l;
	const GeVertex *c;
	int listo;
	if(opt_on[OPT_PROF_ESTADO]) census_prim();
	if(!opt_on[OPT_GX_ENVIO]){ gx_triangle_ref_path(v0, v1, v2); return; }
	listo = tri_listo && opt_on[OPT_GE_ESTADO_SUCIO];
	if(listo && opt_on[OPT_GE_VERIF]){
		/* Comprobar que nada cambió sin quitar tri_listo */
		ge_stats.es_tri_verif++;
		if(!estado_listo()){
			static unsigned logged;
			ge_stats.es_tri_verif_bad++;
			if(logged++ < 16) hle_log_quiet("[VERIF] triangulo sin preparar con algo que preparar\n");
			listo = 0;
		}
	}
	ge_stats.es_tri++;
	if(listo) ge_stats.es_tri_listo++;
	else if(!prepare_idle() && !prepare(0)) return;
	if(use_clut_gpu && tex_on){
		const GeVertex *vv[3] = { v0, v1, v2 };
		if(!clut_gpu_triangle_ok(vv)){
			clut_gpu_fallback();
			prepare_state();
		}
	}
	if(rtt_cur && tex_on) rtt_note_triangle(v0, v1, v2);
	/* Antes de cargar el estado: así no se guarda en la pila para la llamada */
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
	l.tex_on = tex_on; l.through = through; l.tex_proj = tex_proj;
	l.inv_tw = inv_tw; l.inv_th = inv_th; l.tex_su = tex_su; l.tex_sv = tex_sv; l.pos_off = pos_off;
	l.k = tex_on && !through && tex_norm ? gx_tex_scale3(v0, v1, v2) : 1.0f;
	c = flat ? v2 : NULL;
	w = gx_vertex_fast(v0, c ? c : v0, &l); put_words(&w);
	w = gx_vertex_fast(v1, c ? c : v1, &l); put_words(&w);
	w = gx_vertex_fast(v2, c ? c : v2, &l); put_words(&w);
	GX_End();
	if(opt_on[OPT_GE_VERIF]) verify_triangle(v0, v1, v2);
	if(!listo || !tri_listo){
		drawn();
		tri_listo = opt_on[OPT_GE_ESTADO_SUCIO] && estado_listo();
	}
}

/* Esquina de un sprite: x de vx, y de vy; s y t según la orientación */
static void put_corner(const GeVertex *vx, const GeVertex *vy, const GeVertex *v1, int swap_st, float iw){
	float s = 0.0f, t = 0.0f, q = 1.0f;
	if(tex_on){
		s = gx_finite_uv(swap_st ? vy->s : vx->s);
		t = gx_finite_uv(swap_st ? vx->t : vy->t);
		if(through){ s *= inv_tw; t *= inv_th; }
		else {
			if(tex_proj) q = vx->q;
			s *= iw * tex_su; t *= iw * tex_sv; q *= iw;
		}
	}
	put((float)vx->x * (1.0f / 16.0f) + pos_off, (float)vy->y * (1.0f / 16.0f) + pos_off, (float)v1->z, s, t, q,
	    v1->color0, (v1->color1 & 0xFFFFFF) | ((core_u32)gx_fog_byte(v1->fogdepth) << 24));
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
	int ok;
	tri_listo = 0;
	if(v0->x == v1->x || v0->y == v1->y) return;
	if(opt_on[OPT_PROF_ESTADO]) census_prim();
	first_v0 = v0; first_v1 = v1;
	ok = prepare(0);
	first_v0 = first_v1 = NULL;
	if(!ok) return;
	if(use_clut_gpu && tex_on && !clut_gpu_sprite_ok(v0, v1)){
		clut_gpu_fallback();
		prepare_state();
	}
	if(rtt_cur && tex_on) rtt_note_sprite(v0, v1);
	draw_sprite(v0, v1);
	drawn();
}

static void gx_clear_rect(const GeVertex *v0, const GeVertex *v1){
	const GeRasterState *r = ge_raster_state();
	int full = 0, covers = 0, color_full = 0, alpha_w = 0, aval = (int)(v1->color0 >> 24);
	core_u32 mrgb = ge.cmd[GE_MASKRGB] & 0xFFFFFF, ma = ge.cmd[GE_MASKALPHA] & 0xFF;
	tri_listo = 0;
	if(v0->x == v1->x || v0->y == v1->y) return;
	if(targets_dirty){
		int phase = prof_ge_enter(GEF_ESTADO);
		setup_targets();
		targets_dirty = 0;
		gx_dirty = 1;
		if(opt_on[OPT_PROF_ESTADO]) census_dst();
		prof_ge_leave(phase);
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
	tri_listo = 0;
	if(!prepare(0)) return;
	if(use_clut_gpu && tex_on){ clut_gpu_fallback(); prepare_state(); }   /* sin comprobación de límites */
	pos_off = 0.5f;
	GX_Begin(GX_LINES, GX_VTXFMT0, 2);
	put_vertex(v0, flat ? v1 : v0, 1.0f);
	put_vertex(v1, v1, 1.0f);
	GX_End();
	pos_off = 0.0f;
	drawn();
}

static void gx_point(const GeVertex *v0){
	tri_listo = 0;
	if(!prepare(0)) return;
	if(use_clut_gpu && tex_on){ clut_gpu_fallback(); prepare_state(); }
	pos_off = 0.5f;
	GX_Begin(GX_POINTS, GX_VTXFMT0, 1);
	put_vertex(v0, v0, 1.0f);
	GX_End();
	pos_off = 0.0f;
	drawn();
}

static void gx_begin(void){
	targets_dirty = 1;
	cs_primero = 1;
	tri_listo = 0;
}

/* TEXFLUSH vacía el caché de texturas del GE, pero no dice que la memoria
   cambiara: el SDK lo manda cada vez que se elige una textura
   (sceGuTexImage). Lo que cambia la memoria avisa por memory.c. */
static void gx_tex_flush(void){
}

static const GeHwRenderer gx_renderer = {
	gx_begin, gx_triangle, gx_rect, gx_clear_rect, gx_line, gx_point, gx_tex_flush
};

/* --- Presentación -------------------------------------------------------------- */

static Surface display_tmp;

static int present_body(void *xfb, GXRModeObj *rm, int widescreen);

int gx_ge_present_pendiente(void){ return present_pend; }

int gx_ge_present_wait(void){
	if(!present_pend) return 0;
	if(GX_GetDrawSync() != present_tok){
		int old = prof_switch(PROF_ESPERA_GX);
		u64 t0 = gettime(), t;
		while(GX_GetDrawSync() != present_tok){
			t = gettime() - t0;
			if(ticks_to_millisecs(t) >= 200){
				/* El testigo no llega (no debería): como sin gx_paralelo */
				par_lost++;
				wait_gpu_body();
				break;
			}
		}
		par_wait_ticks += gettime() - t0;
		par_waits++;
		prof_switch(old);
	}
	if(present_pend) present_done();
	return 1;
}

int gx_ge_present(void *xfb, GXRModeObj *rm, int widescreen){
	u64 t0 = gettime();
	int r;
	gx_ge_present_wait();   /* la anterior ya tiene que estar (video.c la espera antes) */
	r = present_body(xfb, rm, widescreen);
	tri_listo = 0;
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

/* Memoria del renderizador: texturas, búferes vivos y lo que espera a la GPU para liberarse */
void gx_ge_memory(unsigned *tex_bytes, unsigned *tex_n, unsigned *surf_n, unsigned *surf_bytes, unsigned *deferred){
	int i;
	*tex_bytes = tex_total;
	*tex_n = 0;
	for(i = 0; i < TC_MAX; i++) *tex_n += texc[i].data != NULL;
	*surf_n = *surf_bytes = 0;
	for(i = 0; i < MAX_SURF; i++){
		if(!surf[i].used) continue;
		(*surf_n)++;
		if(surf[i].tex) *surf_bytes += (unsigned)(surf[i].w * surf[i].h * 4);
		if(surf[i].ci) *surf_bytes += (unsigned)(ci_width(&surf[i]) * (surf[i].h + 4));
	}
	*deferred = (unsigned)ndefer;
}

void gx_ge_set_lazy_textures(int on){ lazy_textures = on; }
void gx_ge_set_tex_norm(int on){ tex_norm = on; }
int gx_ge_tex_norm(void){ return tex_norm; }

/* Sin acentos: también sale en la consola del Wii */
void gx_ge_texture_report(char *buf, size_t size){
	const TcStats *t = &tcache.stats, *b = &tex_stats_base;
	u32 lookups = t->lookups - b->lookups, quick = t->quick - b->quick;
	snprintf(buf, size, "texturas: %u elegidas (%.0f%% sin leerlas), %u comprobadas (%.1f MB), %u decodificadas, "
	         "%u paletas; cambios: %u con aviso, %u sin aviso (%u por el vistazo), %d inestables%s",
	         (unsigned)lookups, lookups ? 100.0 * quick / lookups : 0.0, (unsigned)(t->checks - b->checks),
	         (double)(t->check_bytes - b->check_bytes) / (1024.0 * 1024.0), (unsigned)(t->decodes - b->decodes),
	         prof_clut_hashes, (unsigned)(t->changes_notified - b->changes_notified),
	         (unsigned)(t->changes_silent - b->changes_silent), (unsigned)(t->sample_misses - b->sample_misses),
	         tc_unstable_count(&tcache), lazy_textures ? "" : " (sin espaciado)");
	tex_stats_base = *t;
	prof_clut_hashes = 0;
	if(opt_on[OPT_GE_VERIF]){
		size_t len = strlen(buf);
		if(len + 120 < size)
			snprintf(buf + len, size - len, "\nVerificacion del envio a GX (ge_verificar): %u triangulos comparados, %u distintos",
			         (unsigned)verif_tris, (unsigned)verif_tris_bad);
		verif_tris = verif_tris_bad = 0;
	}
}

static unsigned tk_ms(u64 t){ return (unsigned)ticks_to_millisecs(t); }

/* Sin acentos: también sale en la consola del Wii. Varias líneas. */
void gx_ge_rtt_report(char *buf, size_t size){
	static const char *const fmts[] = { "565", "5551", "4444", "8888" };
	size_t len;
	int i, k, used[RTT_MAX] = { 0 };
	unsigned misses = 0;
	if(!size) return;
	for(i = 0; i < RM_N; i++) misses += rtt_miss[i];
	len = (size_t)snprintf(buf, size,
	        "rtt: bajadas %u (%u ms): por textura %u (%u ms), por la CPU %u (%u ms), por la pantalla %u (%u ms), por el GE %u (%u ms)\n"
	        "rtt: decodificadas %u: de framebuffers %u (%u ms, %.1f MB), otra VRAM %u (%u ms), RAM %u (%u ms, %.1f MB)\n"
	        "rtt: texturas en un framebuffer: %u usadas desde la GPU, %u no (formato %u, swizzle %u, no exacta %u, ya en VRAM %u, alfa %u, sin copia %u)\n",
	        rtt_dl[0] + rtt_dl[1] + rtt_dl[2] + rtt_dl[3],
	        tk_ms(rtt_dl_ticks[0] + rtt_dl_ticks[1] + rtt_dl_ticks[2] + rtt_dl_ticks[3]),
	        rtt_dl[RD_TEX], tk_ms(rtt_dl_ticks[RD_TEX]), rtt_dl[RD_CPU], tk_ms(rtt_dl_ticks[RD_CPU]),
	        rtt_dl[RD_PANT], tk_ms(rtt_dl_ticks[RD_PANT]), rtt_dl[RD_GE], tk_ms(rtt_dl_ticks[RD_GE]),
	        rtt_dec[0] + rtt_dec[1] + rtt_dec[2],
	        rtt_dec[RO_FB], tk_ms(rtt_dec_ticks[RO_FB]), (double)rtt_dec_bytes[RO_FB] / (1024.0 * 1024.0),
	        rtt_dec[RO_VRAM], tk_ms(rtt_dec_ticks[RO_VRAM]),
	        rtt_dec[RO_RAM], tk_ms(rtt_dec_ticks[RO_RAM]), (double)rtt_dec_bytes[RO_RAM] / (1024.0 * 1024.0),
	        rtt_gpu, misses, rtt_miss[RM_FMT], rtt_miss[RM_SWZ], rtt_miss[RM_INEXACT], rtt_miss[RM_VRAM],
	        rtt_miss[RM_ALPHA], rtt_miss[RM_NOTEX]);
	/* Bajadas por motivo detallado, con la espera a la GPU aparte */
	if(len < size){
		static const char *const dxn[DX_N] = {
			"textura", "arrastradas por el rango de una textura", "CPU", "pantalla", "solape al crear otro destino",
			"el destino crece", "sin sitio", "copia de bloques del GE", "carga de paleta del GE", "otros comandos del GE",
			"capturas y sincronizar"
		};
		int any = 0;
		len += (size_t)snprintf(buf + len, size - len, "rtt: bajadas por motivo (espera a la GPU + conversion):");
		for(i = 0; i < DX_N && len < size; i++){
			if(!rtt_dx[i]) continue;
			len += (size_t)snprintf(buf + len, size - len, "%s %s %u (%u+%u ms)", any ? "," : "", dxn[i], rtt_dx[i],
			                        tk_ms(rtt_dx_wait[i]), tk_ms(rtt_dx_conv[i]));
			any = 1;
		}
		if(len < size)
			len += (size_t)snprintf(buf + len, size - len, "%s; subidas VRAM->GPU %u (%u ms)\n", any ? "" : " ninguna",
			                        rtt_uploads, tk_ms(rtt_up_ticks));
		/* Lo que habría bastado bajar: las filas que tocó el acceso */
		if(any && len < size){
			len += (size_t)snprintf(buf + len, size - len, "rtt: filas que tocaba cada motivo (de las bajadas):");
			any = 0;
			for(i = 0; i < DX_N && len < size; i++){
				if(!rtt_dx[i]) continue;
				len += (size_t)snprintf(buf + len, size - len, "%s %s %llu de %llu (solo relleno %u)", any ? "," : "", dxn[i],
				                        (unsigned long long)rtt_dx_rows[i], (unsigned long long)rtt_dx_rows_all[i], rtt_dx_pad[i]);
				any = 1;
			}
			if(len < size) len += (size_t)snprintf(buf + len, size - len, "\n");
		}
	}
	/* Los caminos que evitan bajar y decodificar */
	if(len < size)
		len += (size_t)snprintf(buf + len, size - len,
		        "rtt: paleta desde la GPU (ge_rtt_gpu): CLUT32 por alfa %u, CLUT16 sobre 565 %u, vueltas a la CPU %u; "
		        "indices CI8 calculados %u (%u ms), TLUT cargadas %u | relleno sin bajar (ge_fb_relleno) %u | "
		        "huecos (ge_fb_hueco): hechos %u, llenados %u, borrados %u, filas bajadas %u y subidas %u (%u ms)\n",
		        clut_gpu_uses[CI_A8], clut_gpu_uses[CI_565], clut_gpu_fallbacks, clut_ci_builds, tk_ms(clut_ci_ticks),
		        clut_tlut_loads, rtt_pad_skips, hole_made, hole_filled, hole_skips, hole_rows_dl, hole_rows_up,
		        tk_ms(hole_ticks));
	/* Cargas de paleta dentro de búferes dibujados */
	for(i = 0; i < rtt_nclut && len < size; i++){
		const RttClut *c = &rtt_clut[i];
		len += (size_t)snprintf(buf + len, size - len,
		        "rtt: carga de paleta de %08X (%u bytes) en el bufer %08X %s: filas %d..%d, columnas %d..%d%s; %u veces, "
		        "%u con los mismos bytes que la anterior\n",
		        (unsigned)(PSP_VRAM_BASE + c->off), (unsigned)c->len, (unsigned)(PSP_VRAM_BASE + c->surf_addr),
		        fmts[c->surf_fmt & 3], c->r0, c->r1, c->c0, c->c1, c->pad ? " (solo relleno)" : "", c->n, c->same);
	}
	if(rtt_dx[DX_GE] && len < size){
		len += (size_t)snprintf(buf + len, size - len, "rtt: bajadas por comandos del GE:");
		for(i = 0; i < RTT_CMD_MAX && rtt_cmd_n[i] && len < size; i++)
			len += (size_t)snprintf(buf + len, size - len, " %02X x%u", (unsigned)rtt_cmd[i], rtt_cmd_n[i]);
		if(len < size) len += (size_t)snprintf(buf + len, size - len, "\n");
	}
	for(i = 0; i < rtt_npair && len < size; i++){
		const RttPair *p = &rtt_pair[i];
		len += (size_t)snprintf(buf + len, size - len,
		        "rtt: solape: %08X %s %dx%d deja la GPU al crear %08X %s %dx%d: %u veces (%u con bajada), pisa sus filas %d..%d\n",
		        (unsigned)(PSP_VRAM_BASE + p->old_addr), p->old_fmt == SURF_Z ? "Z" : fmts[p->old_fmt & 3], p->old_w, p->old_h,
		        (unsigned)(PSP_VRAM_BASE + p->new_addr), p->new_fmt == SURF_Z ? "Z" : fmts[p->new_fmt & 3], p->new_w, p->new_h,
		        p->n, p->downloads, p->r0, p->r1);
	}
	/* Paletas sobre framebuffers, de más a menos usadas */
	{
		static const char *const tf[] = { "565", "5551", "4444", "8888", "CLUT4", "CLUT8", "CLUT16", "CLUT32",
		                                  "DXT1", "DXT3", "DXT5" };
		int usedp[RTT_PAL_MAX] = { 0 };
		for(k = 0; k < 6 && len < size; k++){
			int best = -1;
			for(i = 0; i < rtt_npal; i++)
				if(!usedp[i] && (best < 0 || rtt_pal[i].n > rtt_pal[best].n)) best = i;
			if(best < 0) break;
			usedp[best] = 1;
			{
				const RttPal *p = &rtt_pal[best];
				len += (size_t)snprintf(buf + len, size - len,
				        "rtt: paleta sobre framebuffer: %08X %s leyendo un %s (desplazamiento %u, mascara %02X, offset %u, "
				        "paleta %s) %dx%d fila %d: %u veces\n",
				        (unsigned)(PSP_VRAM_BASE + p->addr), tf[p->texfmt < 11 ? p->texfmt : 0],
				        p->surf_fmt == SURF_Z ? "Z" : fmts[p->surf_fmt & 3],
				        (unsigned)p->shift, (unsigned)p->mask, (unsigned)p->offset, fmts[p->clut_fmt & 3],
				        p->tw, p->th, p->bufw, p->n);
				if(len < size && p->prims)
					len += (size_t)snprintf(buf + len, size - len,
					        "rtt:   %u primitivas (%u sprites, %u through), filtro lineal %u, clamp s %u t %u; "
					        "vertices u %.2f..%.2f v %.2f..%.2f; lee hasta columna %d y fila %d "
					        "(vecino con peso: s %u, t %u); alfa del origen: uniforme %u (ultimo %d), en VRAM %u, en GPU %u\n",
					        p->prims, p->sprites, p->through, p->linear, p->clamp_s, p->clamp_t,
					        (double)p->umin, (double)p->umax, (double)p->vmin, (double)p->vmax, p->col_max, p->row_max,
					        p->neighbor_s, p->neighbor_t, p->amode[A_UNIFORM], p->ualpha, p->amode[A_VRAM], p->amode[A_GPU]);
				else if(len < size)
					len += (size_t)snprintf(buf + len, size - len,
					        "rtt:   sin primitivas; alfa del origen: uniforme %u (ultimo %d), en VRAM %u, en GPU %u\n",
					        p->amode[A_UNIFORM], p->ualpha, p->amode[A_VRAM], p->amode[A_GPU]);
				if(len < size && p->sprites){
					int q;
					len += (size_t)snprintf(buf + len, size - len,
					        "rtt:   origen %dx%d; muestreo maximo s %.3f t %.3f; sprites que leen fuera del origen: columnas %u "
					        "(peso max %.3f), filas %u (peso max %.3f); paleta cambia %u; destino %08X %s (otro %u), mezcla %u;",
					        p->src_w, p->src_h, (double)p->smax, (double)p->tmax, p->beyond_s, (double)p->beyond_ws,
					        p->beyond_t, (double)p->beyond_wt, p->clut_changes,
					        (unsigned)(PSP_VRAM_BASE + (p->dest_addr == 0xFFFFFFFFu ? 0 : p->dest_addr)),
					        p->dest_fmt < 0 ? "-" : p->dest_fmt == SURF_Z ? "Z" : fmts[p->dest_fmt & 3], p->dest_other, p->blend);
					for(q = 0; q < 4 && len < size; q++)
						if(p->kind[q].n)
							len += (size_t)snprintf(buf + len, size - len, " %dx%d u=%.3fx%+.3f v=%.3fy%+.3f (%u)",
							        p->kind[q].w, p->kind[q].h, (double)p->kind[q].ku, (double)p->kind[q].du,
							        (double)p->kind[q].kv, (double)p->kind[q].dv, p->kind[q].n);
					if(len < size) len += (size_t)snprintf(buf + len, size - len, "\n");
				}
			}
		}
	}
	/* Los 7 búferes con más bajadas y decodificaciones */
	for(k = 0; k < 7 && len < size; k++){
		int best = -1;
		unsigned bs = 0;
		for(i = 0; i < rtt_n; i++){
			const RttTarget *t = &rtt_t[i];
			unsigned sc = t->dl[0] + t->dl[1] + t->dl[2] + t->dl[3] + t->dec + t->binds;
			if(!used[i] && (best < 0 || sc > bs)){ best = i; bs = sc; }
		}
		if(best < 0 || !bs) break;
		used[best] = 1;
		{
			const RttTarget *t = &rtt_t[best];
			static const char *const am[3] = { "uniforme", "en VRAM", "en GPU" };
			len += (size_t)snprintf(buf + len, size - len,
			        "rtt: %08X %dx%d %s (fila %u): destino %u veces, bajado %u (textura %u, CPU %u, pantalla %u, GE %u), "
			        "como textura: GPU %u, decodificado %u, no %u; empieza: borrado %u, copia %u, subida %u; alfa %s",
			        (unsigned)(PSP_VRAM_BASE + t->addr), t->w, t->h, t->fmt == SURF_Z ? "Z" : fmts[t->fmt & 3],
			        (unsigned)t->stride, t->binds, t->dl[0] + t->dl[1] + t->dl[2] + t->dl[3], t->dl[RD_TEX], t->dl[RD_CPU],
			        t->dl[RD_PANT], t->dl[RD_GE], t->gpu, t->dec, t->miss, t->first_clear, t->first_blit, t->first_upload,
			        t->fmt == SURF_Z || !has_alpha_fmt(t->fmt) ? "-" : am[t->amode < 3 ? t->amode : 0]);
			if(len < size && t->fmt != SURF_Z && has_alpha_fmt(t->fmt) && t->amode == A_UNIFORM)
				len += (size_t)snprintf(buf + len, size - len, " %d", t->ualpha);
			if(len < size)
				len += (size_t)snprintf(buf + len, size - len,
				        "; primer dibujo tapa todo: opaco %u, con mezcla %u; VRAM escrita %u; ms: bajar %u, subir %u, decodificar %u\n",
				        t->cover_opaque, t->cover_blend, t->vram_writes, tk_ms(t->dl_ticks), tk_ms(t->up_ticks),
				        tk_ms(t->dec_ticks));
		}
	}
	if(par_presents && len < size){
		len += (size_t)snprintf(buf + len, size - len, "gx_paralelo: %u imagenes sin esperar a la GPU; en %u la "
		                        "siguiente tuvo que esperarla (%u ms)%s\n", par_presents, par_waits, tk_ms(par_wait_ticks),
		                        par_lost ? "; ALGUN TESTIGO NO LLEGO (se espero con GX_DrawDone)" : "");
		if(len >= size) len = size - 1;
	}
	if(len && len < size && buf[len - 1] == '\n') buf[--len] = 0;
	/* Empieza otro periodo */
	par_presents = par_waits = 0;
	par_wait_ticks = 0;
	rtt_n = 0;
	rtt_npal = 0;
	memset(rtt_dl, 0, sizeof(rtt_dl)); memset(rtt_dl_ticks, 0, sizeof(rtt_dl_ticks));
	memset(rtt_dec, 0, sizeof(rtt_dec)); memset(rtt_dec_ticks, 0, sizeof(rtt_dec_ticks));
	memset(rtt_dec_bytes, 0, sizeof(rtt_dec_bytes));
	memset(rtt_miss, 0, sizeof(rtt_miss));
	rtt_gpu = 0;
	memset(rtt_dx, 0, sizeof(rtt_dx)); memset(rtt_dx_wait, 0, sizeof(rtt_dx_wait));
	memset(rtt_dx_conv, 0, sizeof(rtt_dx_conv));
	memset(rtt_cmd_n, 0, sizeof(rtt_cmd_n));
	memset(rtt_dx_rows, 0, sizeof(rtt_dx_rows)); memset(rtt_dx_rows_all, 0, sizeof(rtt_dx_rows_all));
	memset(rtt_dx_pad, 0, sizeof(rtt_dx_pad));
	rtt_nclut = 0;
	rtt_npair = 0;
	memset(clut_gpu_uses, 0, sizeof(clut_gpu_uses));
	clut_gpu_fallbacks = clut_ci_builds = clut_tlut_loads = 0;
	clut_ci_ticks = 0;
	rtt_pad_skips = 0;
	hole_made = hole_filled = hole_skips = hole_rows_dl = hole_rows_up = 0;
	hole_ticks = 0;
	rtt_uploads = 0;
	rtt_up_ticks = 0;
	rtt_cur = NULL;
}

static int present_body(void *xfb, GXRModeObj *rm, int widescreen){
	HleFramebuffer fb;
	Surface *s = NULL;
	float x0, y0, x1, y1;
	core_u32 addr;

	hle_get_framebuffer(&fb);
	if(!fb.addr || fb.stride == 0) return 0;
	frame++;
	ge_sync_domain++;
	addr = fb.addr & PSP_ADDR_MASK;

	if(enabled && addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE){
		s = find_surface(addr - PSP_VRAM_BASE, fb.stride, (int)fb.format);
		if(s && (s->state == VRAM_NEWER || s->hole_n || s->w < 480 || s->h < 272)) s = NULL;
		if(s && s == cur_c && efb_c_newer){ efb_to_tex(s); efb_c_newer = 0; }
		if(s && !s->has_tex) s = NULL;
	}

	/* El EFB se va a usar para componer la imagen */
	save_efb();
	cur_c = cur_z = NULL;
	restored_c = restored_z = 0;
	targets_dirty = 1;
	cs_aviso = gx_tocado = 1;   /* censo: la imagen se compone con otro estado de GX */

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
	if(opt_on[OPT_GX_PARALELO]){
		/* Sin esperar: la imagen estará en el XFB cuando la GPU pase el
		   testigo (gx_ge_present_wait, en la presentación siguiente) */
		present_tok++;
		GX_SetDrawSync(present_tok);
		present_pend = 1;
		defer_at_tok = ndefer;
		tlut_at_tok = tlut_used;
		par_presents++;
	} else wait_gpu();
	return 1;
}

/* --- Control ---------------------------------------------------------------------- */

void gx_ge_sync_vram(void){
	int i;
	tri_listo = 0;
	dl_force = DX_SINC;
	for(i = 0; i < MAX_SURF; i++)
		if(surf[i].used && surf[i].state == GPU_NEWER) download(&surf[i]);
	dl_force = -1;
	recompute_watch();
}

void gx_ge_reset(void){
	int i;
	tri_listo = 0;
	wait_gpu();
	for(i = 0; i < MAX_SURF; i++)
		if(surf[i].used){ free(surf[i].tex); free(surf[i].ci); memset(&surf[i], 0, sizeof(surf[i])); }
	clut_src = NULL;
	use_clut_gpu = 0;
	tlut_loaded = -1;
	for(i = 0; i < TC_MAX; i++)
		if(texc[i].data){ free(texc[i].data); memset(&texc[i], 0, sizeof(texc[i])); }
	tc_init(&tcache);
	tcache.alloc = alloc_texture_slot;
	tcache.decode = decode_slot;
	tcache.gpu_newer = gpu_newer_range;
	tex_total = 0;
	clut_memo_valid = 0;
	cur_c = cur_z = NULL;
	cur_tex = NULL;
	efb_c_newer = efb_z_newer = restored_c = restored_z = 0;
	targets_dirty = gx_dirty = 1;
	cs_aviso = gx_tocado = 1;
	recompute_watch();
}

void gx_ge_init(void){
	Mtx id;
	tri_listo = 0;
	gx_tocado = 1;
	tc_init(&tcache);
	tcache.alloc = alloc_texture_slot;
	tcache.decode = decode_slot;
	tcache.gpu_newer = gpu_newer_range;
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
