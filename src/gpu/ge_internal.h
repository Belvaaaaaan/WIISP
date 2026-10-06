/**
 * WIISP - ge_internal.h
 * Estado interno del GE compartido por el procesador de listas, la
 * geometría y el rasterizador.
 *
 * Los números de comando son los de la librería sceGu del PSPSDK
 * (src/gu/guInternal.h) y los formatos de sus argumentos se han comprobado
 * con el código de esa librería (sceGuTexImage, sceGuClutMode, etc.).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_GE_INTERNAL_H
#define WIISP_GE_INTERNAL_H

#include "core/types.h"
#include "gpu/ge.h"

enum {
	GE_NOP = 0x00, GE_VADDR = 0x01, GE_IADDR = 0x02, GE_PRIM = 0x04, GE_BEZIER = 0x05,
	GE_SPLINE = 0x06, GE_BOUNDINGBOX = 0x07, GE_JUMP = 0x08, GE_BJUMP = 0x09,
	GE_CALL = 0x0A, GE_RET = 0x0B, GE_END = 0x0C, GE_SIGNAL = 0x0E, GE_FINISH = 0x0F,
	GE_BASE = 0x10, GE_VERTEXTYPE = 0x12, GE_OFFSETADDR = 0x13, GE_ORIGIN = 0x14,
	GE_REGION1 = 0x15, GE_REGION2 = 0x16,
	GE_LIGHTINGENABLE = 0x17, GE_LIGHTENABLE0 = 0x18, GE_DEPTHCLAMPENABLE = 0x1C,
	GE_CULLFACEENABLE = 0x1D, GE_TEXTUREMAPENABLE = 0x1E, GE_FOGENABLE = 0x1F,
	GE_DITHERENABLE = 0x20, GE_ALPHABLENDENABLE = 0x21, GE_ALPHATESTENABLE = 0x22,
	GE_ZTESTENABLE = 0x23, GE_STENCILTESTENABLE = 0x24, GE_ANTIALIASENABLE = 0x25,
	GE_PATCHCULLENABLE = 0x26, GE_COLORTESTENABLE = 0x27, GE_LOGICOPENABLE = 0x28,
	GE_BONEMATRIXNUMBER = 0x2A, GE_BONEMATRIXDATA = 0x2B, GE_MORPHWEIGHT0 = 0x2C,
	GE_PATCHDIVISION = 0x36, GE_PATCHPRIMITIVE = 0x37, GE_PATCHFACING = 0x38,
	GE_WORLDMATRIXNUMBER = 0x3A, GE_WORLDMATRIXDATA = 0x3B,
	GE_VIEWMATRIXNUMBER = 0x3C, GE_VIEWMATRIXDATA = 0x3D,
	GE_PROJMATRIXNUMBER = 0x3E, GE_PROJMATRIXDATA = 0x3F,
	GE_TGENMATRIXNUMBER = 0x40, GE_TGENMATRIXDATA = 0x41,
	GE_VIEWPORTXSCALE = 0x42, GE_VIEWPORTYSCALE = 0x43, GE_VIEWPORTZSCALE = 0x44,
	GE_VIEWPORTXCENTER = 0x45, GE_VIEWPORTYCENTER = 0x46, GE_VIEWPORTZCENTER = 0x47,
	GE_TEXSCALEU = 0x48, GE_TEXSCALEV = 0x49, GE_TEXOFFSETU = 0x4A, GE_TEXOFFSETV = 0x4B,
	GE_OFFSETX = 0x4C, GE_OFFSETY = 0x4D,
	GE_SHADEMODE = 0x50, GE_REVERSENORMAL = 0x51, GE_MATERIALUPDATE = 0x53,
	GE_MATERIALEMISSIVE = 0x54, GE_MATERIALAMBIENT = 0x55, GE_MATERIALDIFFUSE = 0x56,
	GE_MATERIALSPECULAR = 0x57, GE_MATERIALALPHA = 0x58, GE_MATERIALSPECULARCOEF = 0x5B,
	GE_AMBIENTCOLOR = 0x5C, GE_AMBIENTALPHA = 0x5D, GE_LIGHTMODE = 0x5E,
	GE_LIGHTTYPE0 = 0x5F, GE_LX0 = 0x63, GE_LDX0 = 0x6F, GE_LKA0 = 0x7B, GE_LKS0 = 0x87,
	GE_LKO0 = 0x8B, GE_LAC0 = 0x8F,
	GE_CULL = 0x9B, GE_FRAMEBUFPTR = 0x9C, GE_FRAMEBUFWIDTH = 0x9D,
	GE_ZBUFPTR = 0x9E, GE_ZBUFWIDTH = 0x9F, GE_TEXADDR0 = 0xA0, GE_TEXBUFWIDTH0 = 0xA8,
	GE_CLUTADDR = 0xB0, GE_CLUTADDRUPPER = 0xB1,
	GE_TRANSFERSRC = 0xB2, GE_TRANSFERSRCW = 0xB3, GE_TRANSFERDST = 0xB4, GE_TRANSFERDSTW = 0xB5,
	GE_TEXSIZE0 = 0xB8, GE_TEXMAPMODE = 0xC0, GE_TEXSHADELS = 0xC1, GE_TEXMODE = 0xC2,
	GE_TEXFORMAT = 0xC3, GE_LOADCLUT = 0xC4, GE_CLUTFORMAT = 0xC5, GE_TEXFILTER = 0xC6,
	GE_TEXWRAP = 0xC7, GE_TEXLEVEL = 0xC8, GE_TEXFUNC = 0xC9, GE_TEXENVCOLOR = 0xCA,
	GE_TEXFLUSH = 0xCB, GE_TEXSYNC = 0xCC, GE_FOG1 = 0xCD, GE_FOG2 = 0xCE, GE_FOGCOLOR = 0xCF,
	GE_TEXLODSLOPE = 0xD0, GE_FRAMEBUFPIXFORMAT = 0xD2, GE_CLEARMODE = 0xD3,
	GE_SCISSOR1 = 0xD4, GE_SCISSOR2 = 0xD5, GE_MINZ = 0xD6, GE_MAXZ = 0xD7,
	GE_COLORTEST = 0xD8, GE_COLORREF = 0xD9, GE_COLORTESTMASK = 0xDA, GE_ALPHATEST = 0xDB,
	GE_STENCILTEST = 0xDC, GE_STENCILOP = 0xDD, GE_ZTEST = 0xDE, GE_BLENDMODE = 0xDF,
	GE_BLENDFIXEDA = 0xE0, GE_BLENDFIXEDB = 0xE1, GE_DITH0 = 0xE2, GE_LOGICOP = 0xE6,
	GE_ZWRITEDISABLE = 0xE7, GE_MASKRGB = 0xE8, GE_MASKALPHA = 0xE9,
	GE_TRANSFERSTART = 0xEA, GE_TRANSFERSRCPOS = 0xEB, GE_TRANSFERDSTPOS = 0xEC,
	GE_TRANSFERSIZE = 0xEE,
	/* Vértice inmediato (depuración): posición, uv, color, primitiva */
	GE_VSCX = 0xF0, GE_VSCY = 0xF1, GE_VSCZ = 0xF2, GE_VTCS = 0xF3, GE_VTCT = 0xF4,
	GE_VTCQ = 0xF5, GE_VCV = 0xF6, GE_VAP = 0xF7, GE_VFC = 0xF8, GE_VSCV = 0xF9
};

/* Tipos de primitiva */
enum {
	GE_PRIM_POINTS = 0, GE_PRIM_LINES, GE_PRIM_LINE_STRIP, GE_PRIM_TRIANGLES,
	GE_PRIM_TRIANGLE_STRIP, GE_PRIM_TRIANGLE_FAN, GE_PRIM_RECTANGLES, GE_PRIM_CONTINUE
};

/* Formatos de píxel (framebuffer, texturas y CLUT) */
enum {
	GE_FMT_565 = 0, GE_FMT_5551 = 1, GE_FMT_4444 = 2, GE_FMT_8888 = 3,
	GE_TFMT_CLUT4 = 4, GE_TFMT_CLUT8 = 5, GE_TFMT_CLUT16 = 6, GE_TFMT_CLUT32 = 7,
	GE_TFMT_DXT1 = 8, GE_TFMT_DXT3 = 9, GE_TFMT_DXT5 = 10
};

typedef struct {
	u32 cmd[256];              /* último argumento (24 bits) de cada comando;
	                              los NUMBER de matrices llevan el contador */

	float world[12], view[12], proj[16], tgen[12];
	float bone[8][12];

	u32 offset;                /* ORIGIN / OFFSETADDR para saltos */
	u32 vaddr, iaddr;          /* direcciones de vértices e índices ya resueltas */

	u8  clut[2048];            /* CLUT cargada (en orden de la PSP) */
	u32 clut_bytes;

	int bbox_visible;          /* resultado de BOUNDINGBOX para BJUMP */

	/* Primitiva anterior (para GE_PRIM_CONTINUE) */
	u32 last_prim;

	/* Vértice inmediato pendiente */
	int immediate_count;
} GeState;

extern GeState ge;
extern GeStats ge_stats;

/* Argumento de 24 bits de un comando como float (los 24 bits altos) */
static inline float ge_f24(u32 arg){
	u32 bits = arg << 8;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

static inline u32 ge_arg(int c){ return ge.cmd[c]; }

/* float -> int definido en todas las plataformas: fuera de rango o NaN da
   INT_MIN (lo que hace x86; en PowerPC el cast sería distinto) */
static inline int ge_f2i(float f){
	if(!(f >= -2147483648.0f && f < 2147483648.0f)) return (int)0x80000000u;
	return (int)f;
}
static inline int ge_enabled(int c){ return ge.cmd[c] & 1; }

/* Dirección de 32 bits a partir de un argumento de 24 bits y BASE */
static inline u32 ge_address(u32 arg){
	return ((ge.cmd[GE_BASE] << 8) & 0x0F000000u) | (arg & 0x00FFFFFFu);
}

/* Modo "through": coordenadas ya en pantalla, sin transformar */
static inline int ge_through(void){ return (ge.cmd[GE_VERTEXTYPE] >> 23) & 1; }

/* Framebuffer y z-buffer (siempre en VRAM) */
static inline u32 ge_fb_addr(void){ return 0x04000000u | (ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0u); }
static inline u32 ge_fb_stride(void){ return ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FC; }
static inline u32 ge_fb_format(void){ return ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3; }
static inline u32 ge_z_addr(void){ return 0x04000000u | (ge.cmd[GE_ZBUFPTR] & 0x1FFFF0u); }
static inline u32 ge_z_stride(void){ return ge.cmd[GE_ZBUFWIDTH] & 0x7FC; }

/* --- Geometría (ge_vertex.c) ------------------------------------------ */

/* Vértice ya en coordenadas de dibujo (píxeles, tras el offset) */
typedef struct {
	float x, y, z;       /* z en 0..65535 */
	float w;             /* 1/w de clip (1 en modo through) para corrección de perspectiva */
	float s, t, q;       /* coordenadas de textura (s/q, t/q en unidades de textura 0..1) */
	float r, g, b, a;    /* color primario 0..255 */
	float sr, sg, sb;    /* color secundario (especular separada) */
	float fog;           /* factor de niebla 0..1 (1 = sin niebla) */
} GeVertex;

void ge_draw_prim(u32 prim, u32 count);
void ge_draw_bezier(u32 arg);
void ge_draw_spline(u32 arg);
void ge_bounding_box(u32 count);
void ge_immediate_vertex(void);

/* --- Rasterizador (ge_raster.c) -------------------------------------- */

void ge_raster_begin(void);   /* lee el estado del GE antes de una tanda */
void ge_raster_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2);
void ge_raster_rectangle(const GeVertex *v0, const GeVertex *v1);
void ge_raster_line(const GeVertex *v0, const GeVertex *v1);
void ge_raster_point(const GeVertex *v);

/* --- Texturas (ge_texture.c) ----------------------------------------- */

typedef struct {
	u32 addr, stride, width, height;  /* nivel 0 */
	u32 format;
	int swizzled;
	int levels;
	u32 level_addr[8], level_stride[8], level_w[8], level_h[8];
} GeTexture;

void ge_texture_setup(GeTexture *tex);
/* Lee un texel (nivel lvl) en coordenadas enteras ya envueltas: RGBA 8 bits */
void ge_texture_fetch(const GeTexture *tex, int lvl, int x, int y, u8 rgba[4]);
void ge_load_clut(u32 blocks);
/* Convierte un color de 16/32 bits de la PSP a RGBA de 8 bits */
void ge_decode_color(u32 format, u32 raw, u8 rgba[4]);

#endif
