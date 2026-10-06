/**
 * WIISP - ge_raster.c
 * GE: rasterizador por software, muestreo de texturas y operaciones de píxel.
 *
 * Es un port a C del renderizador por software de PPSSPP
 * (GPU/Software/Rasterizer.cpp, RasterizerRectangle.cpp, DrawPixel.cpp,
 * Sampler.cpp y FuncId.cpp; (c) PPSSPP Project, GPLv2+), sin el JIT, el
 * SIMD, los hilos ni los atajos que dan el mismo resultado. PPSSPP midió
 * con sus pruebas "gpu/probe" lo que hace el GE bit a bit; esas reglas
 * están aquí:
 *
 *  - Coordenadas de pantalla en 1/16 de píxel; centros de píxel en 16k + 8.
 *  - Profundidad, color Gouraud, niebla y uv se interpolan con "planos" de
 *    punto fijo hechos con el recíproco de montaje del GE, anclados en un
 *    vértice, y se evalúan en el centro de cada píxel.
 *  - Los píxeles se recorren en bloques de 2x2 (cuenta para el mipmap).
 *  - Texels truncados a 1/16, filtro bilineal con 4 bits de fracción.
 *  - Mezcla ((2c + 1) * (2f + 1)) >> 10, tramado, operaciones lógicas.
 *
 * La profundidad se guarda con el swizzle del espejo 3 de la VRAM, como el
 * hardware: leída por 0x04600000 sale lineal.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"

#define SSF 16  /* SCREEN_SCALE_FACTOR */

enum { CMP_NEVER, CMP_ALWAYS, CMP_EQUAL, CMP_NOTEQUAL, CMP_LESS, CMP_LEQUAL, CMP_GREATER, CMP_GEQUAL };
enum { SOP_KEEP, SOP_ZERO, SOP_REPLACE, SOP_INVERT, SOP_INCR, SOP_DECR };
enum {
	BF_OTHERCOLOR, BF_INVOTHERCOLOR, BF_SRCALPHA, BF_INVSRCALPHA, BF_DSTALPHA, BF_INVDSTALPHA,
	BF_DOUBLESRCALPHA, BF_DOUBLEINVSRCALPHA, BF_DOUBLEDSTALPHA, BF_DOUBLEINVDSTALPHA, BF_FIX
};
enum { BEQ_ADD, BEQ_SUB, BEQ_REVSUB, BEQ_MIN, BEQ_MAX, BEQ_ABSDIFF };
enum { TF_MODULATE, TF_DECAL, TF_BLEND, TF_REPLACE, TF_ADD };
enum { LOD_AUTO, LOD_CONST, LOD_SLOPE };

/* --- Conversiones de color -------------------------------------------------- */

static inline u32 c5to8(u32 v){ return (v << 3) | (v >> 2); }
static inline u32 c6to8(u32 v){ return (v << 2) | (v >> 4); }

static inline u32 rgb565_to_8888(u32 c){
	return c5to8(c & 0x1F) | (c6to8((c >> 5) & 0x3F) << 8) | (c5to8((c >> 11) & 0x1F) << 16) | 0xFF000000u;
}
static inline u32 rgba5551_to_8888(u32 c){
	return c5to8(c & 0x1F) | (c5to8((c >> 5) & 0x1F) << 8) | (c5to8((c >> 10) & 0x1F) << 16) |
	       ((c & 0x8000) ? 0xFF000000u : 0);
}
static inline u32 rgba4444_to_8888(u32 c){
	return ((c & 0xF) * 0x11) | ((((c >> 4) & 0xF) * 0x11) << 8) | ((((c >> 8) & 0xF) * 0x11) << 16) |
	       ((((c >> 12) & 0xF) * 0x11) << 24);
}
static inline u32 rgba8888_to_565(u32 c){
	return ((c & 0xFF) >> 3) | (((c >> 10) & 0x3F) << 5) | (((c >> 19) & 0x1F) << 11);
}
static inline u32 rgba8888_to_5551(u32 c){
	return ((c & 0xFF) >> 3) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15);
}
static inline u32 rgba8888_to_4444(u32 c){
	return ((c >> 4) & 0xF) | (((c >> 12) & 0xF) << 4) | (((c >> 20) & 0xF) << 8) | ((c >> 28) << 12);
}

void ge_decode_color(u32 format, u32 raw, u8 rgba[4]){
	u32 c;
	switch(format){
	case GE_FMT_565:  c = rgb565_to_8888(raw); break;
	case GE_FMT_5551: c = rgba5551_to_8888(raw); break;
	case GE_FMT_4444: c = rgba4444_to_8888(raw); break;
	default:          c = raw; break;
	}
	rgba[0] = (u8)c; rgba[1] = (u8)(c >> 8); rgba[2] = (u8)(c >> 16); rgba[3] = (u8)(c >> 24);
}

static inline void unpack(u32 c, int *o){
	o[0] = (int)(c & 0xFF); o[1] = (int)((c >> 8) & 0xFF); o[2] = (int)((c >> 16) & 0xFF); o[3] = (int)(c >> 24);
}
static inline int clamp255(int v){ return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline u32 pack_rgb(const int *c){
	return (u32)clamp255(c[0]) | ((u32)clamp255(c[1]) << 8) | ((u32)clamp255(c[2]) << 16);
}

/* --- Estado (PixelFuncID, SamplerID y RasterizerState de PPSSPP) ----------- */

static struct {
	/* Píxel */
	int clear_mode, color_test, stencil_test, depth_write, apply_depth_range;
	int alpha_test_func, depth_test_func, stencil_test_func, fb_format;
	int alpha_test_ref, stencil_test_ref;
	int alpha_blend, blend_eq, blend_src, blend_dst;
	int has_alpha_test_mask, has_stencil_test_mask, dithering, apply_logic_op, apply_fog;
	int apply_color_write_mask, sfail, zfail, zpass, early_z;
	u32 color_write_mask;
	int dither[16];
	u32 fog_color;
	int minz, maxz;
	u32 fb_stride, z_stride, fb_off, z_off;
	int stencil_ref, stencil_test_mask, alpha_test_mask;
	int color_test_func;
	u32 color_test_mask, color_test_ref, blend_fix_a, blend_fix_b;
	int logic_op;

	/* Muestreo */
	int enable_textures;
	int texfmt, swizzle, clut_fmt, has_clut_mask, has_clut_shift, has_clut_offset, use_shared_clut;
	u32 clutformat;
	int clamp_s, clamp_t, use_tex_alpha, color_doubling, tex_func;
	u32 tex_blend_color;
	int width0_shift, height0_shift, has_any_mips;
	int size_w[8], size_h[8];
	u32 texaddr[8];
	int texvalid[8];
	u16 texbufw[8];
	int max_tex_level, tex_level_mode, tex_level_offset, mip_filt, min_filt, mag_filt;
	float tex_lod_slope;
	int texture_proj;
	int self_texture;          /* textura dentro del búfer que se dibuja */
	const u8 *snap[8];         /* instantánea del caché de texturas */
	u32 snap_size[8];

	int shade_gouraud, through_mode, antialias_lines;
	int sc_x1, sc_y1, sc_x2, sc_y2;  /* tijera en coordenadas de pantalla */
} rs;

static const u8 tex_bits[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };
/* bufw alineado a 16 bytes */
static const u32 tex_align_mask[16] = {
	0x7FF & ~7u, 0x7FF & ~7u, 0x7FF & ~7u, 0x7FF & ~3u, 0x7FF & ~31u, 0x7FF & ~15u,
	0x7FF & ~7u, 0x7FF & ~3u, 0x7FF, 0x7FF, 0x7FF, 0, 0, 0, 0, 0
};

static int optimize_ref_compare(int func, int ref){
	if(ref == 0 && func == CMP_GREATER) return CMP_NOTEQUAL;
	if(ref == 0xFF && func == CMP_LESS) return CMP_NOTEQUAL;
	if(ref == 0 && func == CMP_GEQUAL) return CMP_ALWAYS;
	if(ref == 0xFF && func == CMP_LEQUAL) return CMP_ALWAYS;
	return func;
}

/* El test de stencil compara al revés: la referencia contra el búfer */
static int optimize_stencil_compare(int func, int ref){
	if(ref == 0 && func == CMP_GREATER) return CMP_NEVER;
	if(ref == 0xFF && func == CMP_LESS) return CMP_NEVER;
	if(ref == 0 && func == CMP_LEQUAL) return CMP_ALWAYS;
	if(ref == 0xFF && func == CMP_GEQUAL) return CMP_ALWAYS;
	return func;
}

static inline int ge_on(int c){ return (int)(ge.cmd[c] & 1); }

static void compute_pixel_state(void){
	u32 color_mask = (ge.cmd[GE_MASKRGB] & 0xFFFFFF) | ((ge.cmd[GE_MASKALPHA] & 0xFF) << 24);
	int fmt = (int)(ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3);
	int through = (ge.cmd[GE_VERTEXTYPE] >> 23) & 1;
	int x, y;

	memset(&rs, 0, offsetof(__typeof__(rs), enable_textures));
	rs.apply_depth_range = !through;
	rs.dithering = ge_on(GE_DITHERENABLE);
	rs.fb_format = fmt;
	rs.apply_color_write_mask = color_mask != 0;
	rs.clear_mode = ge.cmd[GE_CLEARMODE] & 1;

	if(rs.clear_mode){
		rs.color_test = (ge.cmd[GE_CLEARMODE] >> 8) & 1;
		rs.stencil_test = ((ge.cmd[GE_CLEARMODE] >> 9) & 1) && fmt != GE_FMT_565;
		rs.depth_write = (ge.cmd[GE_CLEARMODE] >> 10) & 1;
		rs.depth_test_func = CMP_ALWAYS;
		rs.alpha_test_func = CMP_ALWAYS;
	} else {
		int ztest = ge_on(GE_ZTESTENABLE), zfunc = (int)(ge.cmd[GE_ZTEST] & 7);
		int sfunc = (int)(ge.cmd[GE_STENCILTEST] & 7);
		int sref = (int)((ge.cmd[GE_STENCILTEST] >> 8) & 0xFF), smask = (int)((ge.cmd[GE_STENCILTEST] >> 16) & 0xFF);
		int op_sfail = (int)(ge.cmd[GE_STENCILOP] & 7), op_zfail = (int)((ge.cmd[GE_STENCILOP] >> 8) & 7);
		int op_zpass = (int)((ge.cmd[GE_STENCILOP] >> 16) & 7);
		int ctest_func = (int)(ge.cmd[GE_COLORTEST] & 3);
		int blend_eq = (int)((ge.cmd[GE_BLENDMODE] >> 8) & 7);
		u32 fixa = ge.cmd[GE_BLENDFIXEDA] & 0xFFFFFF, fixb = ge.cmd[GE_BLENDFIXEDB] & 0xFFFFFF;

		rs.color_test = ge_on(GE_COLORTESTENABLE) && ctest_func != CMP_ALWAYS;
		if(ge_on(GE_STENCILTESTENABLE) && sfunc == CMP_ALWAYS){
			/* Si siempre pasa, se apaga cuando no se va a escribir stencil */
			int stencil_write = (ge.cmd[GE_MASKALPHA] & 0xFF) != 0xFF && fmt != GE_FMT_565;
			if(ztest && zfunc != CMP_ALWAYS)
				rs.stencil_test = stencil_write && (op_zpass != SOP_KEEP || op_zfail != SOP_KEEP);
			else
				rs.stencil_test = stencil_write && op_zpass != SOP_KEEP;
		} else rs.stencil_test = ge_on(GE_STENCILTESTENABLE);
		rs.depth_write = ztest && !(ge.cmd[GE_ZWRITEDISABLE] & 1);
		rs.depth_test_func = ztest ? zfunc : CMP_ALWAYS;

		if(rs.stencil_test){
			rs.stencil_test_ref = sref & smask;
			rs.stencil_test_func = optimize_stencil_compare(sfunc, rs.stencil_test_ref);
			rs.has_stencil_test_mask = smask != 0xFF && fmt != GE_FMT_565;
			/* En 565 no hay stencil y una operación no válida es KEEP */
			if(fmt != GE_FMT_565 && op_sfail <= SOP_DECR) rs.sfail = op_sfail;
			if(fmt != GE_FMT_565 && op_zfail <= SOP_DECR) rs.zfail = ztest ? op_zfail : SOP_KEEP;
			if(fmt != GE_FMT_565 && op_zpass <= SOP_DECR) rs.zpass = op_zpass;
			if(sref == 0){
				if(rs.sfail == SOP_REPLACE) rs.sfail = SOP_ZERO;
				if(rs.zfail == SOP_REPLACE) rs.zfail = SOP_ZERO;
				if(rs.zpass == SOP_REPLACE) rs.zpass = SOP_ZERO;
			}
			if(fmt == GE_FMT_5551){
				if(rs.sfail == SOP_DECR) rs.sfail = SOP_ZERO;
				if(rs.zfail == SOP_DECR) rs.zfail = SOP_ZERO;
				if(rs.zpass == SOP_DECR) rs.zpass = SOP_ZERO;
			}
			if(rs.stencil_test_func == CMP_ALWAYS){
				if(rs.depth_test_func == CMP_ALWAYS) rs.zfail = SOP_KEEP;
				rs.sfail = SOP_KEEP;
				rs.stencil_test_ref = sref;
				rs.has_stencil_test_mask = 0;
				if(rs.sfail == SOP_KEEP && rs.zfail == SOP_KEEP && rs.zpass == SOP_KEEP){
					rs.stencil_test = 0;
					rs.stencil_test_func = 0;
					rs.stencil_test_ref = 0;
				}
			} else if(rs.depth_test_func == CMP_ALWAYS) rs.zfail = rs.zpass;
		}

		rs.alpha_test_func = ge_on(GE_ALPHATESTENABLE) ? (int)(ge.cmd[GE_ALPHATEST] & 7) : CMP_ALWAYS;
		if(rs.alpha_test_func != CMP_ALWAYS){
			int aref = (int)((ge.cmd[GE_ALPHATEST] >> 8) & 0xFF), amask = (int)((ge.cmd[GE_ALPHATEST] >> 16) & 0xFF);
			rs.alpha_test_ref = aref & amask;
			rs.has_alpha_test_mask = amask != 0xFF;
			rs.alpha_test_func = optimize_ref_compare(rs.alpha_test_func, rs.alpha_test_ref);
			if(rs.alpha_test_func == CMP_ALWAYS){ rs.alpha_test_ref = 0; rs.has_alpha_test_mask = 0; }
		}

		/* 6 y 7 no mezclan; 1 * src + 0 * dst tampoco (algunos juegos lo usan) */
		rs.alpha_blend = ge_on(GE_ALPHABLENDENABLE) && blend_eq <= 5;
		if(rs.alpha_blend && blend_eq == BEQ_ADD){
			int src_one = (ge.cmd[GE_BLENDMODE] & 0xF) == BF_FIX && fixa == 0xFFFFFF;
			int dst_zero = ((ge.cmd[GE_BLENDMODE] >> 4) & 0xF) == BF_FIX && fixb == 0;
			if(src_one && dst_zero) rs.alpha_blend = 0;
		}
		if(rs.alpha_blend) rs.blend_eq = blend_eq;
		if(rs.alpha_blend && blend_eq <= BEQ_REVSUB){
			/* Los factores > 10 son FIX */
			rs.blend_src = (int)(ge.cmd[GE_BLENDMODE] & 0xF);
			rs.blend_dst = (int)((ge.cmd[GE_BLENDMODE] >> 4) & 0xF);
			if(rs.blend_src > BF_FIX) rs.blend_src = BF_FIX;
			if(rs.blend_dst > BF_FIX) rs.blend_dst = BF_FIX;
			rs.blend_fix_a = fixa;
			rs.blend_fix_b = fixb;
		}

		/* Un test de color que no cambia nada (Ridge Racer) */
		if(rs.color_test && ctest_func == CMP_NOTEQUAL && (ge.cmd[GE_COLORREF] & 0xFFFFFF) == 0 &&
		   (ge.cmd[GE_COLORTESTMASK] & 0xFFFFFF) == 0xFFFFFF){
			if(!rs.depth_write && !rs.stencil_test && rs.alpha_blend && rs.blend_eq == BEQ_ADD &&
			   rs.blend_dst == BF_FIX && rs.blend_fix_b == 0xFFFFFF)
				rs.color_test = 0;
		}

		rs.apply_logic_op = ge_on(GE_LOGICOPENABLE) && (ge.cmd[GE_LOGICOP] & 0xF) != 3;
		rs.apply_fog = ge_on(GE_FOGENABLE) && !through;

		rs.early_z = rs.depth_test_func != CMP_ALWAYS;
		if(rs.stencil_test && rs.early_z && (rs.sfail != SOP_KEEP || rs.zfail != SOP_KEEP))
			rs.early_z = 0;
	}

	if(rs.dithering)
		for(y = 0; y < 4; y++)
			for(x = 0; x < 4; x++){
				int raw = (int)((ge.cmd[GE_DITH0 + y] >> (x * 4)) & 0xF);
				rs.dither[y * 4 + x] = raw >= 8 ? raw - 16 : raw;
			}
	if(rs.apply_color_write_mask){
		u32 mask = color_mask;
		/* stencil_test significa aquí "se escribe stencil" */
		if(!rs.stencil_test) mask |= 0xFF000000u;
		switch(fmt){
		case GE_FMT_565:  rs.color_write_mask = rgba8888_to_565(mask); break;
		case GE_FMT_5551: rs.color_write_mask = rgba8888_to_5551(mask); break;
		case GE_FMT_4444: rs.color_write_mask = rgba8888_to_4444(mask); break;
		default:          rs.color_write_mask = mask; break;
		}
	}
	if(rs.apply_fog) rs.fog_color = ge.cmd[GE_FOGCOLOR] & 0xFFFFFF;
	if(rs.apply_logic_op) rs.logic_op = (int)(ge.cmd[GE_LOGICOP] & 0xF);
	rs.minz = (int)(ge.cmd[GE_MINZ] & 0xFFFF);
	rs.maxz = (int)(ge.cmd[GE_MAXZ] & 0xFFFF);
	rs.fb_stride = ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FC;
	rs.z_stride = ge.cmd[GE_ZBUFWIDTH] & 0x7FC;
	rs.fb_off = ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0;
	rs.z_off = ge.cmd[GE_ZBUFPTR] & 0x1FFFF0;
	if(rs.has_stencil_test_mask){
		rs.stencil_ref = (int)((ge.cmd[GE_STENCILTEST] >> 8) & 0xFF);
		rs.stencil_test_mask = (int)((ge.cmd[GE_STENCILTEST] >> 16) & 0xFF);
	}
	if(rs.has_alpha_test_mask) rs.alpha_test_mask = (int)((ge.cmd[GE_ALPHATEST] >> 16) & 0xFF);
	if(!rs.clear_mode && rs.color_test){
		rs.color_test_func = (int)(ge.cmd[GE_COLORTEST] & 3);
		rs.color_test_mask = ge.cmd[GE_COLORTESTMASK] & 0xFFFFFF;
		rs.color_test_ref = ge.cmd[GE_COLORREF] & rs.color_test_mask;
	}
}

/* Direcciones de las que se puede texturizar (Memory::IsValidTextureAddress) */
static int valid_texture_address(u32 a){
	if((a & 0x3E00000F) == 0x08000000) return 1;
	if((a & 0xBF80000F) == 0x04000000) return 1;
	if((a & 0x0F) == 0 && (a & 0x3FFFFFFF) >= 0x08000000 && (a & 0x3FFFFFFF) < 0x08000000 + psp_mem.ram_size) return 1;
	return 0;
}

static void compute_sampler_state(void){
	int max_level = (ge.cmd[GE_TEXFILTER] & 4) ? (int)((ge.cmd[GE_TEXMODE] >> 16) & 7) : 0;
	int fmt = (int)(ge.cmd[GE_TEXFORMAT] & 0xF), i, clut_per_level;
	int clut_fmt = (int)(ge.cmd[GE_CLUTFORMAT] & 3);

	for(i = 0; i < 8; i++){
		u32 addr = (ge.cmd[GE_TEXADDR0 + i] & 0xFFFFF0) | ((ge.cmd[GE_TEXBUFWIDTH0 + i] << 8) & 0x0F000000);
		u32 bufw = ge.cmd[GE_TEXBUFWIDTH0 + i] & tex_align_mask[fmt];
		if(bufw == 0 && fmt <= GE_TFMT_DXT5) bufw = 128 / tex_bits[fmt];
		rs.texaddr[i] = addr;
		rs.texvalid[i] = i <= max_level && valid_texture_address(addr);
		rs.texbufw[i] = (u16)bufw;
		rs.size_w[i] = 1 << (ge.cmd[GE_TEXSIZE0 + i] & 0xF);
		rs.size_h[i] = 1 << ((ge.cmd[GE_TEXSIZE0 + i] >> 8) & 0xF);
	}
	rs.width0_shift = (int)(ge.cmd[GE_TEXSIZE0] & 0xF);
	rs.height0_shift = (int)((ge.cmd[GE_TEXSIZE0] >> 8) & 0xF);
	rs.has_any_mips = max_level != 0;
	rs.texfmt = fmt;
	rs.swizzle = ge.cmd[GE_TEXMODE] & 1;
	/* Con una CLUT por nivel, el nivel n va por encima de los bits del
	   índice: CLUT4 suma n * 16, CLUT8 con paleta de 16 bits (512 entradas)
	   suma (n & 1) * 256 y el resto vuelve a 0 (gpu/probe exp90) */
	clut_per_level = fmt == GE_TFMT_CLUT4 || (fmt == GE_TFMT_CLUT8 && clut_fmt != GE_FMT_8888);
	rs.use_shared_clut = !clut_per_level || max_level == 0 || !(ge.cmd[GE_TEXFILTER] & 4) ||
	                     !(ge.cmd[GE_TEXMODE] & 0x100);
	if(fmt >= GE_TFMT_CLUT4 && fmt <= GE_TFMT_CLUT32){
		rs.clutformat = ge.cmd[GE_CLUTFORMAT];
		rs.clut_fmt = clut_fmt;
		rs.has_clut_mask = ((rs.clutformat >> 8) & 0xFF) != 0xFF;
		rs.has_clut_shift = ((rs.clutformat >> 2) & 0x1F) != 0;
		rs.has_clut_offset = ((rs.clutformat >> 16) & 0x1F) != 0;
	} else {
		rs.clutformat = 0;
		rs.clut_fmt = 0;
		rs.has_clut_mask = rs.has_clut_shift = rs.has_clut_offset = 0;
	}
	rs.clamp_s = ge.cmd[GE_TEXWRAP] & 1;
	rs.clamp_t = (ge.cmd[GE_TEXWRAP] >> 8) & 1;
	rs.use_tex_alpha = (ge.cmd[GE_TEXFUNC] >> 8) & 1;
	rs.color_doubling = (ge.cmd[GE_TEXFUNC] >> 16) & 1;
	rs.tex_func = (int)(ge.cmd[GE_TEXFUNC] & 7);
	if(rs.tex_func > TF_ADD) rs.tex_func = TF_ADD;
	rs.tex_blend_color = ge.cmd[GE_TEXENVCOLOR] & 0xFFFFFF;

	rs.max_tex_level = rs.has_any_mips ? (int)((ge.cmd[GE_TEXMODE] >> 16) & 7) : 0;
	rs.tex_lod_slope = ge_f24(ge.cmd[GE_TEXLODSLOPE]);
	rs.tex_level_mode = (int)(ge.cmd[GE_TEXLEVEL] & 3);
	rs.tex_level_offset = (int)(s8)((ge.cmd[GE_TEXLEVEL] >> 16) & 0xFF);
	rs.mip_filt = (ge.cmd[GE_TEXFILTER] >> 1) & 1;
	rs.min_filt = ge.cmd[GE_TEXFILTER] & 1;
	rs.mag_filt = (ge.cmd[GE_TEXFILTER] >> 8) & 1;
	rs.texture_proj = (ge.cmd[GE_TEXMAPMODE] & 3) == 1;
	if(rs.texture_proj){
		/* Una proyección que no proyecta se trata como uv normales */
		int qzero_st = ge.tgen[2] == 0.0f && ge.tgen[5] == 0.0f;
		int qzero_q = ge.tgen[8] == 0.0f;
		int qfactor_zero = ((ge.cmd[GE_TEXMAPMODE] >> 8) & 3) == 1;
		if(qzero_st && (qzero_q || qfactor_zero) && ge.tgen[11] == 1.0f) rs.texture_proj = 0;
	}
}

static void compute_self_texture(void);

void ge_raster_begin(void){
	int sx2, sy2;
	compute_pixel_state();
	rs.enable_textures = ge_on(GE_TEXTUREMAPENABLE) && !rs.clear_mode;
	if(rs.enable_textures){
		compute_sampler_state();
		compute_self_texture();
	} else rs.self_texture = 0;
	memset(rs.snap, 0, sizeof(rs.snap));
	rs.shade_gouraud = !(ge.cmd[GE_CLEARMODE] & 1) && (ge.cmd[GE_SHADEMODE] & 1);
	rs.through_mode = (ge.cmd[GE_VERTEXTYPE] >> 23) & 1;
	rs.antialias_lines = ge_on(GE_ANTIALIASENABLE);

	sx2 = (int)(ge.cmd[GE_SCISSOR2] & 0x3FF);
	sy2 = (int)((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF);
	if((int)(ge.cmd[GE_REGION2] & 0x3FF) < sx2) sx2 = (int)(ge.cmd[GE_REGION2] & 0x3FF);
	if((int)((ge.cmd[GE_REGION2] >> 10) & 0x3FF) < sy2) sy2 = (int)((ge.cmd[GE_REGION2] >> 10) & 0x3FF);
	rs.sc_x1 = (int)(ge.cmd[GE_SCISSOR1] & 0x3FF) * SSF;
	rs.sc_y1 = (int)((ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF) * SSF;
	rs.sc_x2 = sx2 * SSF + SSF - 1;
	rs.sc_y2 = sy2 * SSF + SSF - 1;
}

int ge_raster_texture_proj(void){ return rs.enable_textures && rs.texture_proj; }

/* --- Caché de texturas al texturizar desde el propio búfer ----------------
   El GE muestrea a través de su caché de texturas: una primitiva que lee
   del búfer en el que dibuja ve el búfer como estaba antes de ella
   (gpu/probe exp81, exp159, exp160). Una textura que cabe en los 8 KB del
   caché se queda en él hasta un TEXFLUSH, así que las primitivas y
   llamadas siguientes la ven como cuando se leyó (gpu/probe exp174; el
   desenfoque 16x16 en 4444 de FF Type-0). Como BinManager de PPSSPP. */

static u32 tex_flush_gen;
static struct {
	u8 *buf[8];
	u32 size[8], addr[8];
	int cached;
	u32 flush_gen;
} st;

void ge_raster_tex_flush(void){ tex_flush_gen++; }

/* ¿Se cruza la textura (start, stride, w, h en bytes) con lo que se escribe? */
static int overlaps_write(u32 start, u32 stride, u32 w, u32 h, u32 base, u32 stride_bytes, u32 width_bytes, u32 height){
	u32 size = stride * (h - 1) + w, row = start, y;
	if(base == 0 || stride_bytes == 0) return 0;
	if(start >= base + height * stride_bytes || start + size <= base) return 0;
	for(y = 0; y < h; y++){
		s32 offset = (s32)(row - base);
		s32 range_y = offset / (s32)stride_bytes;
		u32 range_x = (u32)(offset % (s32)stride_bytes);
		if(range_y >= 0 && (u32)range_y < height)
			if(range_x < width_bytes || range_x + w >= stride_bytes) return 1;
		row += stride;
	}
	return 0;
}

static void compute_self_texture(void){
	u32 sx1 = ge.cmd[GE_SCISSOR1] & 0x3FF, sy1 = (ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF;
	u32 sx2 = ge.cmd[GE_SCISSOR2] & 0x3FF, sy2 = (ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF;
	u32 rx2 = ge.cmd[GE_REGION2] & 0x3FF, ry2 = (ge.cmd[GE_REGION2] >> 10) & 0x3FF;
	u32 bpp = rs.fb_format == GE_FMT_8888 ? 4 : 2, w, h, bits = tex_bits[rs.texfmt];
	u32 fb_base, z_base;
	int i;
	if(rx2 < sx2) sx2 = rx2;
	if(ry2 < sy2) sy2 = ry2;
	w = sx2 - sx1 + 1;
	h = sy2 - sy1 + 1;
	fb_base = (0x04000000u | rs.fb_off) + sy1 * rs.fb_stride * bpp + sx1 * bpp;
	z_base = (0x04000000u | rs.z_off) + sy1 * rs.z_stride * 2 + sx1 * 2;
	rs.self_texture = 0;
	for(i = 0; i <= rs.max_tex_level; i++){
		u32 a = rs.texaddr[i], stride = (u32)rs.texbufw[i] * bits / 8, tw = (u32)rs.size_w[i] * bits / 8;
		u32 th = (u32)rs.size_h[i];
		if((a & 0xBF800000u) != 0x04000000u) continue;  /* solo se escribe la VRAM */
		a &= 0x041FFFFFu;                              /* sin espejos */
		if(overlaps_write(a, stride, tw, th, fb_base, rs.fb_stride * bpp, w * bpp, h) ||
		   (rs.depth_write && overlaps_write(a, stride, tw, th, z_base, rs.z_stride * 2, w * 2, h))){
			rs.self_texture = 1;
			return;
		}
	}
}

static void copy_texture_bytes(u8 *dst, u32 addr, u32 bytes){
	const u8 *p = mem_ptr(addr, bytes);
	u32 i;
	if(p){ memcpy(dst, p, bytes); return; }
	for(i = 0; i < bytes; i++) dst[i] = mem_read8(addr + i);
}

/* Antes de cada primitiva: qué ve el muestreador */
static void texture_snapshot(void){
	u32 bits = tex_bits[rs.texfmt], total = 0;
	int i, same = 1, cache_sized;
	memset(rs.snap, 0, sizeof(rs.snap));
	if(!rs.enable_textures || !rs.self_texture) return;

	for(i = 0; i <= rs.max_tex_level; i++){
		u32 bytes = (u32)rs.size_w[i] * bits / 8 * (u32)rs.size_h[i];
		total += bytes;
		same = same && st.addr[i] == rs.texaddr[i] && st.size[i] == (u32)rs.texbufw[i] * bits / 8 * (u32)rs.size_h[i];
	}
	cache_sized = total <= 8192;
	if(!(cache_sized && st.cached && same && st.flush_gen == tex_flush_gen)){
		st.cached = cache_sized;
		st.flush_gen = tex_flush_gen;
		for(i = 0; i <= rs.max_tex_level; i++){
			u32 bytes = (u32)rs.texbufw[i] * bits / 8 * (u32)rs.size_h[i];
			if(!rs.texvalid[i] || !mem_valid(rs.texaddr[i], bytes)) continue;
			if(st.size[i] != bytes || !st.buf[i]){
				u8 *nb = realloc(st.buf[i], bytes ? bytes : 1);
				if(!nb) continue;
				st.buf[i] = nb;
			}
			copy_texture_bytes(st.buf[i], rs.texaddr[i], bytes);
			st.size[i] = bytes;
			st.addr[i] = rs.texaddr[i];
		}
	}
	for(i = 0; i <= rs.max_tex_level; i++)
		if(st.buf[i] && st.addr[i] == rs.texaddr[i]){
			rs.snap[i] = st.buf[i];
			rs.snap_size[i] = st.size[i];
		}
}

/* --- Búferes --------------------------------------------------------------- */

static inline u8 *fb_ptr(int x, int y, int bpp){
	u32 off = (rs.fb_off + ((u32)y * rs.fb_stride + (u32)x) * (u32)bpp) & (PSP_VRAM_SIZE - 1);
	return psp_mem.vram + off;
}
static inline u32 fb_get16(int x, int y){ return rd_le16(fb_ptr(x, y, 2)); }
static inline u32 fb_get32(int x, int y){ return rd_le32(fb_ptr(x, y, 4)); }
static inline void fb_set16(int x, int y, u32 v){ wr_le16(fb_ptr(x, y, 2), (u16)v); }
static inline void fb_set32(int x, int y, u32 v){ wr_le32(fb_ptr(x, y, 4), v); }

static inline u8 *z_ptr(int x, int y){
	u32 off = (rs.z_off + ((u32)y * rs.z_stride + (u32)x) * 2) & (PSP_VRAM_SIZE - 1);
	return psp_mem.vram + mem_vram_deswizzle(off, 1);
}
static inline u32 get_depth(int x, int y){ return rd_le16(z_ptr(x, y)); }
static inline void set_depth(int x, int y, u32 z){ wr_le16(z_ptr(x, y), (u16)z); }

/* --- Píxel (DrawPixel.cpp) ------------------------------------------------- */

static inline int get_stencil(int x, int y){
	switch(rs.fb_format){
	case GE_FMT_565:  return 0;
	case GE_FMT_5551: return (fb_get16(x, y) & 0x8000) ? 0xFF : 0;
	case GE_FMT_4444: return (int)((fb_get16(x, y) >> 12) * 0x11);
	default:          return (int)(fb_get32(x, y) >> 24);
	}
}

static inline void set_stencil(u32 write_mask, int x, int y, int value){
	switch(rs.fb_format){
	case GE_FMT_565: break;
	case GE_FMT_5551:
		if((write_mask & 0x8000) == 0) fb_set16(x, y, (fb_get16(x, y) & ~0x8000u) | (((u32)value & 0x80) << 8));
		break;
	case GE_FMT_4444: {
		u32 m = (write_mask | 0x0FFF) & 0xFFFF;
		fb_set16(x, y, (fb_get16(x, y) & m) | ((((u32)value & 0xFF) << 8) & ~m & 0xFFFF));
		break;
	}
	default: {
		u32 m = write_mask | 0x00FFFFFF;
		fb_set32(x, y, (fb_get32(x, y) & m) | ((((u32)value & 0xFF) << 24) & ~m));
		break;
	}
	}
}

static inline u32 get_color(int x, int y){
	switch(rs.fb_format){
	case GE_FMT_565:  return rgb565_to_8888(fb_get16(x, y)) & 0x00FFFFFF; /* alfa 0 para mezclar */
	case GE_FMT_5551: return rgba5551_to_8888(fb_get16(x, y));
	case GE_FMT_4444: return rgba4444_to_8888(fb_get16(x, y));
	default:          return fb_get32(x, y);
	}
}

static inline void set_color(int x, int y, u32 value, u32 old, u32 mask){
	switch(rs.fb_format){
	case GE_FMT_565:
		value = rgba8888_to_565(value);
		if(mask) value = (value & ~mask) | (rgba8888_to_565(old) & mask);
		fb_set16(x, y, value);
		break;
	case GE_FMT_5551:
		value = rgba8888_to_5551(value);
		if(mask) value = (value & ~mask) | (rgba8888_to_5551(old) & mask);
		fb_set16(x, y, value);
		break;
	case GE_FMT_4444:
		value = rgba8888_to_4444(value);
		if(mask) value = (value & ~mask) | (rgba8888_to_4444(old) & mask);
		fb_set16(x, y, value);
		break;
	default:
		fb_set32(x, y, (value & ~mask) | (old & mask));
		break;
	}
}

static inline int compare(int func, int a, int b){
	switch(func){
	case CMP_NEVER:    return 0;
	case CMP_ALWAYS:   return 1;
	case CMP_EQUAL:    return a == b;
	case CMP_NOTEQUAL: return a != b;
	case CMP_LESS:     return a < b;
	case CMP_LEQUAL:   return a <= b;
	case CMP_GREATER:  return a > b;
	default:           return a >= b;
	}
}

static inline int depth_passed(int x, int y, int z){
	return compare(rs.depth_test_func, (u16)z, (int)get_depth(x, y));
}

static inline int stencil_op(int op, int replace, int old){
	switch(op){
	case SOP_KEEP:    return old;
	case SOP_ZERO:    return 0;
	case SOP_REPLACE: return replace;
	case SOP_INVERT:  return (~old) & 0xFF;
	case SOP_INCR:
		switch(rs.fb_format){
		case GE_FMT_8888: return old != 0xFF ? old + 1 : old;
		case GE_FMT_5551: return 0xFF;
		case GE_FMT_4444: return old < 0xF0 ? old + 0x10 : old;
		default:          return old;
		}
	case SOP_DECR:
		switch(rs.fb_format){
		case GE_FMT_4444: return old >= 0x10 ? old - 0x10 : old;
		case GE_FMT_5551: return 0;
		default:          return old != 0 ? old - 1 : old;
		}
	}
	return old;
}

static u32 logic_op(int op, u32 old, u32 c){
	switch(op){
	case 0:  return c & 0xFF000000u;                                         /* clear */
	case 1:  return c & (old | 0xFF000000u);                                 /* and */
	case 2:  return c & (~old | 0xFF000000u);                                /* and reverse */
	case 3:  return c;                                                       /* copy */
	case 4:  return (~c & (old & 0x00FFFFFF)) | (c & 0xFF000000u);           /* and inverted */
	case 5:  return (old & 0x00FFFFFF) | (c & 0xFF000000u);                  /* noop */
	case 6:  return c ^ (old & 0x00FFFFFF);                                  /* xor */
	case 7:  return c | (old & 0x00FFFFFF);                                  /* or */
	case 8:  return (~(c | old) & 0x00FFFFFF) | (c & 0xFF000000u);           /* nor */
	case 9:  return (~(c ^ old) & 0x00FFFFFF) | (c & 0xFF000000u);           /* equiv */
	case 10: return (~old & 0x00FFFFFF) | (c & 0xFF000000u);                 /* inverted */
	case 11: return c | (~old & 0x00FFFFFF);                                 /* or reverse */
	case 12: return (~c & 0x00FFFFFF) | (c & 0xFF000000u);                   /* copy inverted */
	case 13: return ((~c | old) & 0x00FFFFFF) | (c & 0xFF000000u);           /* or inverted */
	case 14: return (~(c & old) & 0x00FFFFFF) | (c & 0xFF000000u);           /* nand */
	default: return c | 0x00FFFFFF;                                          /* set */
	}
}

static void blend_factor(int f, int is_src, const int *src, const int *dst, u32 fix, int *out){
	int i, v = 0;
	switch(f){
	case BF_OTHERCOLOR:    for(i = 0; i < 3; i++) out[i] = is_src ? dst[i] : src[i]; return;
	case BF_INVOTHERCOLOR: for(i = 0; i < 3; i++) out[i] = 255 - (is_src ? dst[i] : src[i]); return;
	case BF_SRCALPHA:          v = src[3]; break;
	case BF_INVSRCALPHA:       v = 255 - src[3]; break;
	case BF_DSTALPHA:          v = dst[3]; break;
	case BF_INVDSTALPHA:       v = 255 - dst[3]; break;
	case BF_DOUBLESRCALPHA:    v = 2 * src[3]; break;
	case BF_DOUBLEINVSRCALPHA: v = 255 - 2 * src[3]; break;
	case BF_DOUBLEDSTALPHA:    v = 2 * dst[3]; break;
	case BF_DOUBLEINVDSTALPHA: v = 255 - 2 * dst[3]; break;
	default:
		out[0] = (int)(fix & 0xFF); out[1] = (int)((fix >> 8) & 0xFF); out[2] = (int)((fix >> 16) & 0xFF);
		return;
	}
	out[0] = out[1] = out[2] = v;
}

static inline int signed_factor(int f){ return f == BF_DOUBLEINVSRCALPHA || f == BF_DOUBLEINVDSTALPHA; }

/* 255 - 2a es negativo con a >= 128 y entonces el término resta
   (gpu/probe exp116) */
static inline int blend_term(int c, int f){
	int t = ((2 * c + 1) * (2 * abs(f) + 1)) >> 10;
	return f < 0 ? -t : t;
}

static void alpha_blend(const int *src, const int *dst, int *out){
	int sf[3], df[3], i;
	blend_factor(rs.blend_src, 1, src, dst, rs.blend_fix_a, sf);
	blend_factor(rs.blend_dst, 0, src, dst, rs.blend_fix_b, df);
	if((signed_factor(rs.blend_src) || signed_factor(rs.blend_dst)) && rs.blend_eq <= BEQ_REVSUB){
		for(i = 0; i < 3; i++){
			int s = blend_term(src[i], sf[i]), d = blend_term(dst[i], df[i]);
			out[i] = rs.blend_eq == BEQ_SUB ? s - d : rs.blend_eq == BEQ_REVSUB ? d - s : s + d;
		}
		return;
	}
	for(i = 0; i < 3; i++){
		int l = ((src[i] * 2 + 1) * (sf[i] * 2 + 1)) >> 10;
		int r = ((dst[i] * 2 + 1) * (df[i] * 2 + 1)) >> 10;
		switch(rs.blend_eq){
		/* Las restas saturan en 0 antes del tramado (PSUBUSW en PPSSPP) */
		case BEQ_ADD:     out[i] = l + r; break;
		case BEQ_SUB:     out[i] = l > r ? l - r : 0; break;
		case BEQ_REVSUB:  out[i] = r > l ? r - l : 0; break;
		case BEQ_MIN:     out[i] = src[i] < dst[i] ? src[i] : dst[i]; break;
		case BEQ_MAX:     out[i] = src[i] > dst[i] ? src[i] : dst[i]; break;
		case BEQ_ABSDIFF: out[i] = abs(src[i] - dst[i]); break;
		default:          out[i] = src[i]; break;
		}
	}
}

static void draw_pixel(int x, int y, int z, int fog, const int *color_in){
	int c[4], i, stencil;
	u32 write_mask, old, nc;
	for(i = 0; i < 4; i++) c[i] = clamp255(color_in[i]);

	/* Rango de profundidad: también en modo clear, si no es through */
	if(rs.apply_depth_range && !rs.early_z && (z < rs.minz || z > rs.maxz)) return;

	if(rs.alpha_test_func != CMP_ALWAYS && !rs.clear_mode){
		int a = c[3];
		if(rs.has_alpha_test_mask) a &= rs.alpha_test_mask;
		if(!compare(rs.alpha_test_func, a, rs.alpha_test_ref)) return;
	}

	/* La niebla va antes del test de color; como el BLEND de texturas,
	   redondea siempre hacia arriba */
	if(rs.apply_fog && !rs.clear_mode){
		int fc[4];
		unpack(rs.fog_color, fc);
		for(i = 0; i < 3; i++) c[i] = (c[i] * fog + fc[i] * (255 - fog) + 255) / 256;
	}

	if(rs.color_test && !rs.clear_mode){
		u32 v = pack_rgb(c) & rs.color_test_mask;
		int pass = 1;
		switch(rs.color_test_func){
		case CMP_NEVER:    pass = 0; break;
		case CMP_EQUAL:    pass = v == rs.color_test_ref; break;
		case CMP_NOTEQUAL: pass = v != rs.color_test_ref; break;
		default:           break;
		}
		if(!pass) return;
	}

	/* En modo clear el alfa es el stencil */
	write_mask = rs.apply_color_write_mask ? rs.color_write_mask : 0;
	stencil = rs.clear_mode ? c[3] : get_stencil(x, y);
	if(rs.clear_mode){
		if(rs.depth_write) set_depth(x, y, (u32)z);
	} else if(rs.stencil_test){
		int replace = rs.has_stencil_test_mask ? rs.stencil_ref : rs.stencil_test_ref;
		int sv = stencil;
		if(rs.has_stencil_test_mask) sv &= rs.stencil_test_mask;
		if(!compare(rs.stencil_test_func, rs.stencil_test_ref, sv)){
			set_stencil(write_mask, x, y, stencil_op(rs.sfail, replace, stencil));
			return;
		}
		if(!rs.early_z && rs.depth_test_func != CMP_ALWAYS && !depth_passed(x, y, z)){
			set_stencil(write_mask, x, y, stencil_op(rs.zfail, replace, stencil));
			return;
		}
		stencil = stencil_op(rs.zpass, replace, stencil);
	} else if(!rs.early_z){
		if(rs.depth_test_func != CMP_ALWAYS && !depth_passed(x, y, z)) return;
	}

	if(rs.depth_write && !rs.clear_mode) set_depth(x, y, (u32)z);

	old = get_color(x, y);
	/* El tramado va antes de la operación lógica, en cualquier formato y
	   también en modo clear */
	if(rs.alpha_blend && !rs.clear_mode){
		int dst[4], b[3];
		unpack(old, dst);
		alpha_blend(c, dst, b);
		if(rs.dithering){
			int d = rs.dither[(y & 3) * 4 + (x & 3)];
			for(i = 0; i < 3; i++) b[i] += d;
		}
		nc = pack_rgb(b) | ((u32)(stencil & 0xFF) << 24);
	} else {
		if(rs.dithering){
			int d = rs.dither[(y & 3) * 4 + (x & 3)];
			for(i = 0; i < 3; i++) c[i] += d;
		}
		nc = pack_rgb(c) | ((u32)(stencil & 0xFF) << 24);
	}

	if(rs.apply_logic_op && !rs.clear_mode) nc = logic_op(rs.logic_op, old, nc);

	if(rs.clear_mode){
		if(!rs.color_test) nc = (nc & 0xFF000000u) | (old & 0x00FFFFFF);
		if(!rs.stencil_test) nc = (nc & 0x00FFFFFF) | (old & 0xFF000000u);
	}

	set_color(x, y, nc, old, write_mask);
	ge_stats.pixels++;
}

/* --- Muestreo de texturas (Sampler.cpp) ---------------------------------- */

static u32 pixel_offset(int bits, u32 pitch, u32 u, u32 v, int swizzled){
	u32 texels_per_tile, tile_u, tile_idx;
	if(!swizzled) return v * (pitch * (u32)bits >> 3) + (u * (u32)bits >> 3);
	texels_per_tile = 32 / (u32)bits;
	tile_u = u / texels_per_tile;
	tile_idx = (v % 8) * 4 + (v / 8) * ((pitch * (u32)bits / 32) * 8) + (tile_u % 4) + (tile_u / 4) * 32;
	return tile_idx * 4 + ((u % texels_per_tile) * (u32)bits) / 8;
}

/* Bytes de la textura del nivel `lv` (desde su dirección), o de la
   instantánea del caché si la primitiva texturiza desde su propio búfer */
static int tex_lv;
static inline const u8 *tex_p(u32 off, u32 len){
	if(rs.snap[tex_lv]) return off + len <= rs.snap_size[tex_lv] ? rs.snap[tex_lv] + off : NULL;
	return mem_ptr(rs.texaddr[tex_lv] + off, len);
}
static inline u32 tex_rd8(u32 off){ const u8 *p = tex_p(off, 1); return p ? *p : 0; }
static inline u32 tex_rd16(u32 off){ const u8 *p = tex_p(off, 2); return p ? rd_le16(p) : 0; }
static inline u32 tex_rd32(u32 off){ const u8 *p = tex_p(off, 4); return p ? rd_le32(p) : 0; }

static u32 lookup_color(u32 index, int level){
	if(!rs.use_shared_clut) index += rs.texfmt == GE_TFMT_CLUT4 ? (u32)level * 16 : (u32)(level & 1) * 256;
	switch(rs.clut_fmt){
	case GE_FMT_565:  return rgb565_to_8888(rd_le16(ge.clut + (index & 0x3FF) * 2));
	case GE_FMT_5551: return rgba5551_to_8888(rd_le16(ge.clut + (index & 0x3FF) * 2));
	case GE_FMT_4444: return rgba4444_to_8888(rd_le16(ge.clut + (index & 0x3FF) * 2));
	default:          return rd_le32(ge.clut + (index & 0x1FF) * 4);
	}
}

static u32 transform_clut_index(u32 index){
	if(rs.has_clut_shift || rs.has_clut_mask || rs.has_clut_offset){
		u32 shift = (rs.clutformat >> 2) & 0x1F, mask = (rs.clutformat >> 8) & 0xFF;
		u32 offset = ((rs.clutformat >> 16) & 0x1F) << 4;
		/* Lo que pasa del primer KB da la vuelta */
		u32 offset_mask = rs.clut_fmt == GE_FMT_8888 ? 0xFF : 0x1FF;
		return ((index >> shift) & mask) | (offset & offset_mask);
	}
	return index & 0xFF;
}

static inline int mix_2_3(int a, int b){ return (a + a + b) / 3; }
static inline u32 makecol(int r, int g, int b, int a){
	return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

static u32 dxt_texel_color(u32 block, int x, int y, int alpha){
	u32 c1 = tex_rd16(block + 4), c2 = tex_rd16(block + 6);
	int b1 = (int)((c1 << 3) & 0xF8), b2 = (int)((c2 << 3) & 0xF8);
	int g1 = (int)((c1 >> 3) & 0xFC), g2 = (int)((c2 >> 3) & 0xFC);
	int r1 = (int)((c1 >> 8) & 0xF8), r2 = (int)((c2 >> 8) & 0xF8);
	int idx = (int)((tex_rd8(block + (u32)y) >> (x * 2)) & 3);
	if(idx == 0) return makecol(r1, g1, b1, alpha);
	if(idx == 1) return makecol(r2, g2, b2, alpha);
	if(c1 > c2){
		if(idx == 2) return makecol(mix_2_3(r1, r2), mix_2_3(g1, g2), mix_2_3(b1, b2), alpha);
		return makecol(mix_2_3(r2, r1), mix_2_3(g2, g1), mix_2_3(b2, b1), alpha);
	}
	if(idx == 3) return 0;
	return makecol((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, alpha);
}

static u32 dxt5_lerp(u32 block, int n){
	int a1 = (int)tex_rd8(block + 14), a2 = (int)tex_rd8(block + 15);
	if(a1 > a2) return (u32)((((a1 * ((7 - n) << 8)) / 7) + ((a2 * (n << 8)) / 7) + 31) >> 8) & 0xFF;
	return (u32)((((a1 * ((5 - n) << 8)) / 5) + ((a2 * (n << 8)) / 5) + 31) >> 8) & 0xFF;
}

static u32 fetch_texel(int u, int v, int level){
	u32 base = 0, bufw = rs.texbufw[level], uu = (u32)u, vv = (u32)v;
	tex_lv = level;
	switch(rs.texfmt){
	case GE_FMT_4444: return rgba4444_to_8888(tex_rd16(base + pixel_offset(16, bufw, uu, vv, rs.swizzle)));
	case GE_FMT_5551: return rgba5551_to_8888(tex_rd16(base + pixel_offset(16, bufw, uu, vv, rs.swizzle)));
	case GE_FMT_565:  return rgb565_to_8888(tex_rd16(base + pixel_offset(16, bufw, uu, vv, rs.swizzle)));
	case GE_FMT_8888: return tex_rd32(base + pixel_offset(32, bufw, uu, vv, rs.swizzle));
	case GE_TFMT_CLUT32:
		return lookup_color(transform_clut_index(tex_rd32(base + pixel_offset(32, bufw, uu, vv, rs.swizzle))), 0);
	case GE_TFMT_CLUT16:
		return lookup_color(transform_clut_index(tex_rd16(base + pixel_offset(16, bufw, uu, vv, rs.swizzle))), 0);
	case GE_TFMT_CLUT8:
		return lookup_color(transform_clut_index(tex_rd8(base + pixel_offset(8, bufw, uu, vv, rs.swizzle))), level);
	case GE_TFMT_CLUT4: {
		u32 b = tex_rd8(base + pixel_offset(4, bufw, uu, vv, rs.swizzle));
		return lookup_color(transform_clut_index((uu & 1) ? (b >> 4) : (b & 0xF)), level);
	}
	case GE_TFMT_DXT1: {
		u32 block = base + ((vv >> 2) * (bufw >> 2) + (uu >> 2)) * 8;
		return dxt_texel_color(block, u & 3, v & 3, 255);
	}
	case GE_TFMT_DXT3: {
		u32 block = base + ((vv >> 2) * (bufw >> 2) + (uu >> 2)) * 16;
		u32 alpha = (tex_rd16(block + 8 + (u32)(v & 3) * 2) >> ((u & 3) * 4)) & 0xF;
		return dxt_texel_color(block, u & 3, v & 3, 0) | (alpha << 28);
	}
	case GE_TFMT_DXT5: {
		u32 block = base + ((vv >> 2) * (bufw >> 2) + (uu >> 2)) * 16;
		u64 bits = ((u64)tex_rd16(block + 12) << 32) | tex_rd32(block + 8);
		u32 color = dxt_texel_color(block, u & 3, v & 3, 0);
		int n = (int)((bits >> ((v & 3) * 12 + (u & 3) * 3)) & 7);
		int a1 = (int)tex_rd8(block + 14), a2 = (int)tex_rd8(block + 15);
		if(n == 0) return color | ((u32)a1 << 24);
		if(n == 1) return color | ((u32)a2 << 24);
		if(a1 > a2) return color | (dxt5_lerp(block, n - 1) << 24);
		if(n == 6) return color;
		if(n == 7) return color | 0xFF000000u;
		return color | (dxt5_lerp(block, n - 1) << 24);
	}
	default:
		return 0;
	}
}

static inline u32 sample_texel(int u, int v, int level){
	if(!rs.texvalid[level]) return 0;
	return fetch_texel(u, v, level);
}

static inline int clamp_uv(int v, int size){
	if(v >= size - 1) return size - 1;
	if(v >= 511) return 511;
	if(v < 0) return 0;
	return v;
}
static inline int wrap_uv(int v, int size){ return v & (size - 1) & 511; }

/* Texel en 1/16 truncado; fuera del rango de int es INT_MIN, como el
   muestreador de x86 (gpu/probe exp5) */
static inline int texel_fixed(float f){
	return f >= -2147483648.0f && f < 2147483648.0f ? (int)f : INT_MIN;
}

static void texture_function(const int *prim, const int *tex, int *out){
	int i, rgba = rs.use_tex_alpha;
	switch(rs.tex_func){
	case TF_MODULATE:
		for(i = 0; i < 3; i++)
			out[i] = rs.color_doubling ? ((prim[i] + 1) * tex[i] * 2) / 256 : (prim[i] + 1) * tex[i] / 256;
		out[3] = rgba ? (prim[3] + 1) * tex[3] / 256 : prim[3];
		break;
	case TF_DECAL:
		if(rgba){
			/* Los dos colores llevan +1: el alfa pesa algo más */
			int t = tex[3], invt = 255 - t;
			for(i = 0; i < 3; i++){
				int v = (prim[i] + 1) * invt + (tex[i] + 1) * t;
				out[i] = rs.color_doubling ? v / 128 : v / 256;
			}
		} else for(i = 0; i < 3; i++) out[i] = rs.color_doubling ? tex[i] * 2 : tex[i];
		out[3] = prim[3];
		break;
	case TF_BLEND: {
		int env[4];
		unpack(rs.tex_blend_color, env);
		for(i = 0; i < 3; i++){
			int v = (255 - tex[i]) * prim[i] + tex[i] * env[i] + 255;
			out[i] = rs.color_doubling ? v / 128 : v / 256;
		}
		out[3] = rgba ? (prim[3] + 1) * tex[3] / 256 : prim[3];
		break;
	}
	case TF_REPLACE:
		for(i = 0; i < 3; i++) out[i] = rs.color_doubling ? tex[i] * 2 : tex[i];
		out[3] = rgba ? tex[3] : prim[3];
		break;
	default: /* ADD y los valores sin uso */
		for(i = 0; i < 3; i++) out[i] = rs.color_doubling ? (prim[i] + tex[i]) * 2 : prim[i] + tex[i];
		out[3] = rgba ? (prim[3] + 1) * tex[3] / 256 : prim[3];
		break;
	}
}

static void sample_nearest_level(float s, float t, int level, int *c){
	int w = rs.size_w[level], h = rs.size_h[level];
	int u = texel_fixed(s * (float)w * 16.0f) >> 4, v = texel_fixed(t * (float)h * 16.0f) >> 4;
	u = rs.clamp_s ? clamp_uv(u, w) : wrap_uv(u, w);
	v = rs.clamp_t ? clamp_uv(v, h) : wrap_uv(v, h);
	unpack(sample_texel(u, v, level), c);
}

/* Bilineal como el GE: primero las mezclas horizontales truncadas a 8 bits
   y luego la vertical (gpu/probe exp52) */
static void sample_linear_level(float s, float t, int level, int *c){
	int w = rs.size_w[level], h = rs.size_h[level], i;
	int bu = (int)((u32)texel_fixed(s * (float)w * 16.0f) - 8u);
	int bv = (int)((u32)texel_fixed(t * (float)h * 16.0f) - 8u);
	int fu = bu & 15, fv = bv & 15, u0, u1, v0, v1;
	int tl[4], tr[4], bl[4], br[4];
	bu >>= 4; bv >>= 4;
	u0 = rs.clamp_s ? clamp_uv(bu, w) : wrap_uv(bu, w);
	u1 = rs.clamp_s ? clamp_uv(bu + 1, w) : wrap_uv(bu + 1, w);
	v0 = rs.clamp_t ? clamp_uv(bv, h) : wrap_uv(bv, h);
	v1 = rs.clamp_t ? clamp_uv(bv + 1, h) : wrap_uv(bv + 1, h);
	unpack(sample_texel(u0, v0, level), tl);
	unpack(sample_texel(u1, v0, level), tr);
	unpack(sample_texel(u0, v1, level), bl);
	unpack(sample_texel(u1, v1, level), br);
	for(i = 0; i < 4; i++){
		int top = (tl[i] * (16 - fu) + tr[i] * fu) >> 4;
		int bot = (bl[i] * (16 - fu) + br[i] * fu) >> 4;
		c[i] = (top * (16 - fv) + bot * fv) >> 4;
	}
}

static void sample(float s, float t, const int *prim, int level, int frac, int bilinear, int *out){
	int c0[4], c1[4], i;
	if(bilinear) sample_linear_level(s, t, level, c0);
	else sample_nearest_level(s, t, level, c0);
	if(frac){
		if(bilinear) sample_linear_level(s, t, level + 1, c1);
		else sample_nearest_level(s, t, level + 1, c1);
		for(i = 0; i < 4; i++) c0[i] = (c1[i] * frac + c0[i] * (16 - frac)) >> 4;
	}
	texture_function(prim, c0, out);
}

/* q es 1/w en el píxel; auto_grad el gradiente de los planos uv (o < 0) */
static void sampling_params(float ds, float dt, float q, int *level, int *level_frac, int *filt, float auto_grad){
	int width = 1 << rs.width0_shift, height = 1 << rs.height0_shift, detail;
	switch(rs.tex_level_mode){
	case LOD_AUTO:
		if(auto_grad >= 0.0f) detail = ge_log16(auto_grad) - ge_log16(q);
		else {
			float a = fabsf(ds * (float)width), b = fabsf(dt * (float)height);
			detail = ge_log16(a > b ? a : b);
		}
		break;
	case LOD_SLOPE:
		/* El mismo log2 por bits del float para q y para la pendiente,
		   más un nivel (gpu/probe exp58-60) */
		detail = 16 + ge_log16(rs.tex_lod_slope) - ge_log16(q);
		break;
	default:
		detail = 0;
		break;
	}
	detail += rs.tex_level_offset;
	if(detail > 0 && rs.max_tex_level > 0){
		int level8 = detail < rs.max_tex_level * 16 ? detail : rs.max_tex_level * 16;
		if(!rs.mip_filt) level8 += 8;
		*level = level8 >> 4;
		*level_frac = rs.mip_filt ? level8 & 0xF : 0;
	} else {
		*level = 0;
		*level_frac = 0;
	}
	*filt = detail > 0 ? rs.min_filt : rs.mag_filt;
}

/* Texturas de un bloque 2x2: el LOD automático usa sus diferencias */
static void apply_texturing4(int prim[4][4], const int *mask, const float *s, const float *t, const float *q, float auto_grad){
	float ds = fmaxf(fabsf(s[1] - s[0]), fabsf(s[2] - s[0]));
	float dt = fmaxf(fabsf(t[1] - t[0]), fabsf(t[2] - t[0]));
	int level = 0, frac = 0, bil = 0, i;
	int per_pixel = rs.tex_level_mode == LOD_SLOPE || (rs.tex_level_mode == LOD_AUTO && auto_grad >= 0.0f);
	if(!per_pixel) sampling_params(ds, dt, 0.0f, &level, &frac, &bil, -1.0f);
	for(i = 0; i < 4; i++){
		int out[4];
		if(mask[i] < 0) continue;
		if(per_pixel) sampling_params(ds, dt, q[i], &level, &frac, &bil, auto_grad);
		sample(s[i], t[i], prim[i], level, frac, bil, out);
		memcpy(prim[i], out, sizeof(out));
	}
}

static inline int double_secondary(void){ return rs.enable_textures && rs.color_doubling; }

/* --- Planos (profundidad, color, niebla y uv) ------------------------------ */

typedef struct {
	s64 base, kx, ky;    /* valor << 14 en (0, 0) y gradientes por subpíxel */
	int right_anchored;  /* el lado largo es el derecho: el GE recorre de derecha a izquierda */
} Plane;

static inline s64 plane_at(const Plane *p, s64 x, s64 y){ return (p->base + p->kx * x + p->ky * y) >> 14; }

/* El plano con que el GE interpola profundidad y color Gouraud
   (gpu/probe exp36-41, exp54): gradientes en punto fijo con 14 bits de
   fracción por subpíxel, a partir de los productos cruzados exactos y del
   recíproco de montaje, anclado en un vértice: el de más a la izquierda,
   salvo que el lado largo (de arriba abajo) sea estrictamente el derecho,
   y entonces el de más a la derecha. */
static Plane compute_plane(const s64 *X, const s64 *Y, const s64 *Z){
	Plane p;
	s64 det = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
	s64 nx, ny, q, sign, cross;
	u64 abs_det;
	int e, top = 0, mid = 1, bot = 2, anchor = 0, flat, i, tmp;
	memset(&p, 0, sizeof(p));
	if(det == 0){ p.base = Z[0] * 16384; return p; }
	nx = (Z[1] - Z[0]) * (Y[2] - Y[0]) - (Z[2] - Z[0]) * (Y[1] - Y[0]);
	ny = (Z[2] - Z[0]) * (X[1] - X[0]) - (Z[1] - Z[0]) * (X[2] - X[0]);
	abs_det = (u64)(det < 0 ? -det : det);
	q = ge_setup_recip(abs_det, &e);
	sign = det < 0 ? -1 : 1;
	p.kx = (sign * nx * q) >> (e + 2);
	p.ky = (sign * ny * q) >> (e + 2);

#define ABOVE(a, b) (Y[a] < Y[b] || (Y[a] == Y[b] && X[a] < X[b]))
	if(ABOVE(mid, top)){ tmp = mid; mid = top; top = tmp; }
	if(ABOVE(bot, mid)){ tmp = bot; bot = mid; mid = tmp; }
	if(ABOVE(mid, top)){ tmp = mid; mid = top; top = tmp; }
#undef ABOVE
	cross = (X[bot] - X[top]) * (Y[mid] - Y[top]) - (Y[bot] - Y[top]) * (X[mid] - X[top]);
	flat = Y[top] == Y[mid] || Y[mid] == Y[bot];
	if(cross > 0 && !flat){
		p.right_anchored = 1;
		for(i = 1; i < 3; i++)
			if(X[i] > X[anchor] || (X[i] == X[anchor] && Y[i] < Y[anchor])) anchor = i;
	} else {
		for(i = 1; i < 3; i++)
			if(X[i] < X[anchor] || (X[i] == X[anchor] && Y[i] < Y[anchor])) anchor = i;
	}
	p.base = Z[anchor] * 16384 - p.kx * X[anchor] - p.ky * Y[anchor];
	return p;
}

/* Coordenadas de textura con perspectiva como las interpola el GE
   (gpu/probe exp55-56): q = 1/w con el recíproco del GE y s = u * q por
   vértice en float24; s, t y q pasan a enteros de 15 bits con el mayor
   exponente de los tres vértices, van por el mismo plano que la
   profundidad, y la u de un píxel es s * 1/q (ge_uv_product). */
typedef struct {
	Plane s, t, q;
	int shift_s, shift_t, shift_q;
	int valid;
} UVPlanes;

static int shared_shift(const double *v){
	int e = INT_MIN, i;
	for(i = 0; i < 3; i++)
		if(v[i] != 0.0){
			int x = ilogb(v[i]);
			if(x > e) e = x;
		}
	return e == INT_MIN ? 0 : 14 - e;
}

/* double -> s64 como en x86: fuera de rango o NaN da INT64_MIN (en
   PowerPC la conversión satura, así que se hace a mano) */
static inline s64 d2s64(double d){
	if(!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) return INT64_MIN;
	return (s64)d;
}

static Plane fixed_plane(const s64 *X, const s64 *Y, const double *v, int shift){
	s64 V[3];
	int i;
	for(i = 0; i < 3; i++) V[i] = d2s64(ldexp(v[i], shift));
	return compute_plane(X, Y, V);
}

static UVPlanes uv_planes_stq(const s64 *X, const s64 *Y, const double *s, const double *t, const double *q){
	UVPlanes p;
	p.shift_s = shared_shift(s);
	p.shift_t = shared_shift(t);
	p.shift_q = shared_shift(q);
	p.s = fixed_plane(X, Y, s, p.shift_s);
	p.t = fixed_plane(X, Y, t, p.shift_t);
	p.q = fixed_plane(X, Y, q, p.shift_q);
	p.valid = 1;
	return p;
}

/* Con proyección de textura uq es la q de la matriz de textura y u, v
   salen divididas por ella (gpu/probe exp64) */
static UVPlanes uv_planes(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2, int proj){
	const GeVertex *v[3] = { v0, v1, v2 };
	s64 X[3], Y[3];
	double s[3], t[3], q[3];
	UVPlanes p;
	int i;
	memset(&p, 0, sizeof(p));
	for(i = 0; i < 3; i++){
		float w24 = ge_trunc24(v[i]->clipw);
		double r;
		if(!(w24 > 0.0f) || !isfinite(w24)) return p;
		r = ge_recip(w24);
		q[i] = proj ? ge_product24((double)ge_trunc24(v[i]->q) * r) : r;
		s[i] = ge_product24((double)ge_trunc24(v[i]->s) * r);
		t[i] = ge_product24((double)ge_trunc24(v[i]->t) * r);
		X[i] = v[i]->x;
		Y[i] = v[i]->y;
	}
	return uv_planes_stq(X, Y, s, t, q);
}

/* Mayor gradiente de los planos s y t en texels por píxel (LOD automático);
   in_texels: los planos llevan texels (triángulos en modo through) */
static float uv_plane_gradient(const UVPlanes *p, int in_texels){
	double per_pixel = (double)SSF / 16384.0, gs, gt;
	s64 as, at;
	if(!p->valid) return -1.0f;
	as = llabs(p->s.kx) > llabs(p->s.ky) ? llabs(p->s.kx) : llabs(p->s.ky);
	at = llabs(p->t.kx) > llabs(p->t.ky) ? llabs(p->t.kx) : llabs(p->t.ky);
	gs = ldexp((double)as * per_pixel, -p->shift_s);
	gt = ldexp((double)at * per_pixel, -p->shift_t);
	if(!in_texels){
		gs *= (double)(1 << rs.width0_shift);
		gt *= (double)(1 << rs.height0_shift);
	}
	return (float)(gs > gt ? gs : gt);
}

static inline float plane_f24(const Plane *p, int shift, s64 x, s64 y){
	return ge_trunc24((float)ldexp((double)plane_at(p, x, y), -shift));
}

static void tex_coords_ge(const UVPlanes *p, s64 cx, s64 cy, float *s, float *t, float *q){
	int i;
	for(i = 0; i < 4; i++){
		s64 x = cx + (i & 1) * SSF, y = cy + (i >> 1) * SSF;
		float qq = plane_f24(&p->q, p->shift_q, x, y);
		double r;
		q[i] = qq;
		if(!(qq > 0.0f)){ s[i] = t[i] = 0.0f; continue; }
		r = ge_recip(qq);
		s[i] = ge_uv_product((double)plane_f24(&p->s, p->shift_s, x, y) * r);
		t[i] = ge_uv_product((double)plane_f24(&p->t, p->shift_t, x, y) * r);
	}
}

/* Coordenadas por pesos baricéntricos, para cuando no hay planos (w no
   válida) */
static void tex_coords_bary(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2,
                            const s32 *w0, const s32 *w1, const s32 *w2, int proj, float *s, float *t){
	float q0 = 1.0f / v0->clipw, q1 = 1.0f / v1->clipw, q2 = 1.0f / v2->clipw;
	int i;
	for(i = 0; i < 4; i++){
		float wq0 = (float)w0[i] * q0, wq1 = (float)w1[i] * q1, wq2 = (float)w2[i] * q2, qr;
		if(proj) qr = 1.0f / (wq0 * v0->q + wq1 * v1->q + wq2 * v2->q);
		else qr = 1.0f / (wq0 + wq1 + wq2);
		s[i] = (wq0 * v0->s + wq1 * v1->s + wq2 * v2->s) * qr;
		t[i] = (wq0 * v0->t + wq1 * v1->t + wq2 * v2->t) * qr;
	}
}

/* --- Triángulos ------------------------------------------------------------ */

typedef struct { int x, y; } Pt;

static inline s64 edge_at(Pt a, Pt b, s64 x, s64 y){
	return (s64)(a.y - b.y) * x + (s64)(b.x - a.x) * y + ((s64)b.y * a.x - (s64)b.x * a.y);
}

/* Exacto en 64 bits: una división truncada daba por dentro píxeles de
   triángulos finos (Peace Walker) */
static int right_side_or_flat_bottom(Pt v, Pt l1, Pt l2){
	s64 dy, lhs, rhs;
	if(l1.y == l2.y) return v.y < l1.y;
	dy = (s64)l2.y - l1.y;
	lhs = (s64)(v.x - l1.x) * dy;
	rhs = (s64)(l2.x - l1.x) * (v.y - l1.y);
	return dy > 0 ? lhs < rhs : lhs > rhs;
}

/* Arista del recorrido por bloques 2x2, en aritmética de 32 bits como PPSSPP */
typedef struct { s32 step_x, step_y; } Edge;

static inline s32 wrap32(s64 v){ return (s32)(u32)(u64)v; }

static void edge_start(Edge *e, Pt v0, Pt v1, int ox, int oy, s32 *w){
	s32 xf = wrap32((s64)v0.y - v1.y), yf = wrap32((s64)v1.x - v0.x);
	s32 c = wrap32((s64)v1.y * v0.x - (s64)v1.x * v0.y);
	static const int ix[4] = { SSF / 2, SSF + SSF / 2, SSF / 2, SSF + SSF / 2 };
	static const int iy[4] = { SSF / 2, SSF / 2, SSF + SSF / 2, SSF + SSF / 2 };
	int i;
	e->step_x = wrap32((s64)xf * SSF * 2);
	e->step_y = wrap32((s64)yf * SSF * 2);
	for(i = 0; i < 4; i++)
		w[i] = wrap32((s64)wrap32((s64)xf * (ox + ix[i])) + (s64)wrap32((s64)yf * (oy + iy[i])) + c);
}

static inline void edge_step(s32 *w, s32 step, int times){
	int i;
	for(i = 0; i < 4; i++) w[i] = wrap32((s64)w[i] + (s64)wrap32((s64)step * times));
}

static void narrow_min_max_x(const Edge *e, const s32 *w, s64 min_x, s64 *row_min, s64 *row_max){
	s32 wmax = w[0];
	int i;
	for(i = 1; i < 4; i++) if(w[i] > wmax) wmax = w[i];
	if(wmax < 0){
		if(e->step_x > 0){
			int steps = (int)(-(s64)wmax / e->step_x);
			s64 v = min_x + (s64)steps * SSF * 2;
			if(v > *row_min) *row_min = v;
		} else *row_min = *row_max + 1;
	}
	if(wmax >= 0 && e->step_x < 0){
		int steps = (int)(-(s64)wmax / e->step_x) + 1;
		s64 v = min_x + (s64)steps * SSF * 2;
		if(v < *row_max) *row_max = v;
	}
}

static const GeVertex *tri_v[3];
static int tri_bias[3];

static int covered_at(s64 x, s64 y){
	Pt p0 = { tri_v[0]->x, tri_v[0]->y }, p1 = { tri_v[1]->x, tri_v[1]->y }, p2 = { tri_v[2]->x, tri_v[2]->y };
	return edge_at(p1, p2, x, y) + tri_bias[0] >= 0 && edge_at(p2, p0, x, y) + tri_bias[1] >= 0 &&
	       edge_at(p0, p1, x, y) + tri_bias[2] >= 0;
}

/* La q del nivel de mipmap: una por tramo de 4 píxeles de la fila, en el
   segundo según el sentido en que el GE la recorre, o si ese queda fuera,
   en el primero del tramo que esté dentro (gpu/probe exp93, exp103-106) */
static void lod_q_from_planes(const UVPlanes *p, s64 cx, s64 cy, int quad_x, float *q){
	int rtl = p->q.right_anchored, i, j;
	for(i = 0; i < 4; i++){
		int px = quad_x + (i & 1), span_x = px & ~3, pick = span_x + (rtl ? 2 : 1);
		s64 y = cy + (i >> 1) * SSF, x;
		if(!covered_at(cx + (s64)(pick - quad_x) * SSF, y)){
			for(j = 0; j < 4; j++){
				int c = rtl ? span_x + 3 - j : span_x + j;
				if(covered_at(cx + (s64)(c - quad_x) * SSF, y)){ pick = c; break; }
			}
		}
		x = cx + (s64)(pick - quad_x) * SSF;
		q[i] = plane_f24(&p->q, p->shift_q, x, y);
	}
}

static inline int clamp_fog_depth(float fogdepth){
	u32 u, ex, m;
	memcpy(&u, &fogdepth, 4);
	ex = u >> 23;
	if((u & 0x80000000u) || ex <= 126 - 8) return 0;
	if(ex > 126) return 255;
	m = (u & 0x007FFFFF) | 0x00800000;
	return (int)(m >> (16 + 126 - ex));
}

static void draw_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2, int x1, int y1, int x2, int y2){
	Pt p0 = { v0->x, v0->y }, p1 = { v1->x, v1->y }, p2 = { v2->x, v2->y };
	Edge e0, e1, e2;
	s32 w0b[4], w1b[4], w2b[4];
	int bias[3], i, c, k;
	s64 min_x = x1, max_x = x2, min_y = y1, max_y = y2, cur_y;
	int flat_z, flat_c0, flat_c1, no_fog, snap_edge = -1, snap_left = 0, lod_uses_q;
	Plane zp, c0p[4], c1p[3], fogp;
	UVPlanes uvp;
	float auto_grad, wsum_recip[4];
	int v2c0[4], v2c1[4];
	s64 X[3] = { v0->x, v1->x, v2->x }, Y[3] = { v0->y, v1->y, v2->y };

	bias[0] = right_side_or_flat_bottom(p0, p1, p2) ? -1 : 0;
	bias[1] = right_side_or_flat_bottom(p1, p2, p0) ? -1 : 0;
	bias[2] = right_side_or_flat_bottom(p2, p0, p1) ? -1 : 0;
	tri_v[0] = v0; tri_v[1] = v1; tri_v[2] = v2;
	memcpy(tri_bias, bias, sizeof(bias));

	lod_uses_q = rs.tex_level_mode != LOD_CONST && (rs.max_tex_level > 0 || rs.min_filt != rs.mag_filt);

	/* Un triángulo muy alto: con 3 dy > 2^17 subpíxeles, el primer píxel de
	   cada tramo de 4 (el último en una arista derecha) cuenta como dentro
	   del lado largo si lo está alguno del tramo (gpu/probe exp131-135) */
	{
		const GeVertex *vs[3] = { v0, v1, v2 };
		int top = 0, bot = 0;
		s64 dy;
		for(i = 1; i < 3; i++){
			if(vs[i]->y < vs[top]->y) top = i;
			if(vs[i]->y > vs[bot]->y || (vs[i]->y == vs[bot]->y && vs[i]->x < vs[bot]->x)) bot = i;
		}
		dy = (s64)vs[bot]->y - vs[top]->y;
		if(top != bot && 3 * dy > (1 << 17)){
			int third = 3 - top - bot;
			s64 ex = (s64)(vs[bot]->x - vs[top]->x) * (vs[third]->y - vs[top]->y);
			snap_edge = third;
			snap_left = (s64)(vs[third]->x - vs[top]->x) * dy > ex;
		}
	}

	edge_start(&e0, p1, p2, (int)min_x, (int)min_y, w0b);
	edge_start(&e1, p2, p0, (int)min_x, (int)min_y, w1b);
	edge_start(&e2, p0, p1, (int)min_x, (int)min_y, w2b);
	for(i = 0; i < 4; i++) wsum_recip[i] = 1.0f / (float)wrap32((s64)w0b[i] + w1b[i] + w2b[i]);

	flat_z = v0->z == v1->z && v0->z == v2->z;
	flat_c0 = !rs.shade_gouraud || (v0->color0 == v1->color0 && v0->color0 == v2->color0);
	flat_c1 = !rs.shade_gouraud || (v0->color1 == v1->color1 && v0->color1 == v2->color1);
	no_fog = rs.clear_mode || !rs.apply_fog ||
	         (v0->fogdepth >= 255.0f / 256.0f && v1->fogdepth >= 255.0f / 256.0f && v2->fogdepth >= 255.0f / 256.0f);

	if(rs.apply_depth_range && flat_z && (v0->z < rs.minz || v0->z > rs.maxz)) return;

	unpack(v2->color0, v2c0);
	unpack(v2->color1, v2c1);

	memset(&zp, 0, sizeof(zp));
	memset(&fogp, 0, sizeof(fogp));
	if(!flat_z){
		s64 Z[3] = { v0->z, v1->z, v2->z };
		zp = compute_plane(X, Y, Z);
	}
	memset(&uvp, 0, sizeof(uvp));
	/* También en modo through, donde w es 1 (gpu/probe exp73) */
	if(rs.enable_textures) uvp = uv_planes(v0, v1, v2, rs.texture_proj && !rs.through_mode);
	auto_grad = uv_plane_gradient(&uvp, rs.through_mode);
	if(!no_fog){
		s64 F[3] = { clamp_fog_depth(v0->fogdepth), clamp_fog_depth(v1->fogdepth), clamp_fog_depth(v2->fogdepth) };
		fogp = compute_plane(X, Y, F);
	}
	if(!flat_c0)
		for(c = 0; c < 4; c++){
			s64 C[3] = { (v0->color0 >> (c * 8)) & 0xFF, (v1->color0 >> (c * 8)) & 0xFF, (v2->color0 >> (c * 8)) & 0xFF };
			c0p[c] = compute_plane(X, Y, C);
		}
	if(!flat_c1)
		for(c = 0; c < 3; c++){
			s64 C[3] = { (v0->color1 >> (c * 8)) & 0xFF, (v1->color1 >> (c * 8)) & 0xFF, (v2->color1 >> (c * 8)) & 0xFF };
			c1p[c] = compute_plane(X, Y, C);
		}

	for(cur_y = min_y; cur_y <= max_y; cur_y += SSF * 2){
		s32 w0[4], w1[4], w2[4];
		s64 row_min = min_x, row_max = max_x, cur_x;
		int skip, px, py = (int)(cur_y / SSF), scissor_y1 = cur_y + SSF > max_y ? -1 : 0;
		s32 sm[4];

		memcpy(w0, w0b, sizeof(w0)); memcpy(w1, w1b, sizeof(w1)); memcpy(w2, w2b, sizeof(w2));
		/* Una arista que encaja puede encender hasta tres píxeles más allá */
		if(snap_edge != 0) narrow_min_max_x(&e0, w0, min_x, &row_min, &row_max);
		if(snap_edge != 1) narrow_min_max_x(&e1, w1, min_x, &row_min, &row_max);
		if(snap_edge != 2) narrow_min_max_x(&e2, w2, min_x, &row_min, &row_max);

		skip = (int)((row_min - min_x) / (SSF * 2));
		edge_step(w0, e0.step_x, skip);
		edge_step(w1, e1.step_x, skip);
		edge_step(w2, e2.step_x, skip);
		px = (int)((min_x / SSF + 2 * skip) & 0x3FF);

		sm[0] = 0;
		sm[1] = (s32)(row_max - row_min - SSF);
		sm[2] = scissor_y1;
		sm[3] = (s32)(row_max - row_min - SSF) | scissor_y1;

		for(cur_x = row_min; cur_x <= row_max; cur_x += SSF * 2){
			int mask[4], z[4], fog[4], prim[4][4], sec[4][4];
			int any = 0;
			s64 cx = cur_x + SSF / 2, cy = cur_y + SSF / 2;

			for(i = 0; i < 4; i++){
				s32 m = wrap32((s64)w0[i] + bias[0]) | wrap32((s64)w1[i] + bias[1]) | wrap32((s64)w2[i] + bias[2]) | sm[i];
				mask[i] = m < 0 ? -1 : 0;
			}
			if(snap_edge >= 0){
				for(i = 0; i < 4; i++){
					s64 x = cx + (i & 1) * SSF, y = cy + (i >> 1) * SSF;
					int qx = px + (i & 1), inside = 1;
					/* Solo el primer píxel del tramo (arista izquierda) o el
					   último (derecha) toma la arista del otro extremo */
					int span_x = snap_left ? ((qx & 3) == 0 ? (qx | 3) : qx) : ((qx & 3) == 3 ? (qx & ~3) : qx);
					s64 xs = x + (s64)(span_x - qx) * SSF;
					for(k = 0; k < 3 && inside; k++){
						s64 ex = k == snap_edge ? xs : x, ev;
						if(k == 0) ev = edge_at(p1, p2, ex, y) + bias[0];
						else if(k == 1) ev = edge_at(p2, p0, ex, y) + bias[1];
						else ev = edge_at(p0, p1, ex, y) + bias[2];
						inside = ev >= 0;
					}
					mask[i] = ((inside ? 0 : -1) | sm[i]) < 0 ? -1 : 0;
				}
			}
			for(i = 0; i < 4; i++) if(mask[i] >= 0) any = 1;

			if(any){
				if(flat_z){
					for(i = 0; i < 4; i++) z[i] = v2->z;
				} else {
					s64 z00 = zp.base + zp.kx * cx + zp.ky * cy, dx = zp.kx * SSF, dy = zp.ky * SSF;
					z[0] = (int)(z00 >> 14);
					z[1] = (int)((z00 + dx) >> 14);
					z[2] = (int)((z00 + dy) >> 14);
					z[3] = (int)((z00 + dx + dy) >> 14);
					/* Por debajo de 0 junto a una arista de z = 0 vale 0 (gpu/probe exp148) */
					for(i = 0; i < 4; i++) if(z[i] < 0) z[i] = 0;
				}

				if(rs.early_z){
					for(i = 0; i < 4; i++){
						if(mask[i] < 0) continue;
						if(rs.apply_depth_range && (z[i] < rs.minz || z[i] > rs.maxz)){ mask[i] = -1; continue; }
						if(!depth_passed(px + (i & 1), py + (i >> 1), z[i])) mask[i] = -1;
					}
					any = 0;
					for(i = 0; i < 4; i++) if(mask[i] >= 0) any = 1;
				}
			}

			if(any){
				/* El color no se corrige por perspectiva en la PSP */
				for(i = 0; i < 4; i++){
					s64 x = cx + (i & 1) * SSF, y = cy + (i >> 1) * SSF;
					if(!flat_c0){
						if(mask[i] >= 0)
							for(c = 0; c < 4; c++) prim[i][c] = clamp255((int)plane_at(&c0p[c], x, y));
					} else memcpy(prim[i], v2c0, sizeof(v2c0));
					if(!flat_c1){
						if(mask[i] >= 0)
							for(c = 0; c < 3; c++) sec[i][c] = clamp255((int)plane_at(&c1p[c], x, y));
					} else memcpy(sec[i], v2c1, sizeof(v2c1));
					if(double_secondary()) for(c = 0; c < 3; c++) sec[i][c] += sec[i][c];
				}

				if(rs.enable_textures && !rs.clear_mode){
					float s[4], t[4], q[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
					if(rs.through_mode){
						if(uvp.valid) tex_coords_ge(&uvp, cx, cy, s, t, q);
						else for(i = 0; i < 4; i++){
							s[i] = ((float)w0[i] * v0->s + (float)w1[i] * v1->s + (float)w2[i] * v2->s) * wsum_recip[i];
							t[i] = ((float)w0[i] * v0->t + (float)w1[i] * v1->t + (float)w2[i] * v2->t) * wsum_recip[i];
						}
						/* Los mipmaps se miden siempre respecto al nivel 0 */
						for(i = 0; i < 4; i++){
							s[i] *= 1.0f / (float)(1 << rs.width0_shift);
							t[i] *= 1.0f / (float)(1 << rs.height0_shift);
						}
					} else if(uvp.valid){
						tex_coords_ge(&uvp, cx, cy, s, t, q);
						if(lod_uses_q) lod_q_from_planes(&uvp, cx, cy, px, q);
					} else {
						tex_coords_bary(v0, v1, v2, w0, w1, w2, rs.texture_proj, s, t);
					}
					if(rs.tex_level_mode == LOD_SLOPE && !uvp.valid){
						float cw = (v0->clipw * (float)w0[0] + v1->clipw * (float)w1[0] + v2->clipw * (float)w2[0]) * wsum_recip[0];
						for(i = 0; i < 4; i++) q[i] = 1.0f / cw;
					}
					apply_texturing4(prim, mask, s, t, q, auto_grad);
				}

				if(!rs.clear_mode)
					for(i = 0; i < 4; i++) for(c = 0; c < 3; c++) prim[i][c] += sec[i][c];

				for(i = 0; i < 4; i++){
					fog[i] = 255;
					if(!no_fog){
						/* La niebla de 8 bits de cada vértice por el plano,
						   como el color (gpu/probe exp21) */
						s64 f = plane_at(&fogp, cx + (i & 1) * SSF, cy + (i >> 1) * SSF);
						fog[i] = f < 0 ? 0 : f > 255 ? 255 : (int)f;
					}
				}

				for(i = 0; i < 4; i++){
					if(mask[i] < 0) continue;
					draw_pixel(px + (i & 1), py + (i >> 1), z[i], fog[i], prim[i]);
				}
			}

			edge_step(w0, e0.step_x, 1);
			edge_step(w1, e1.step_x, 1);
			edge_step(w2, e2.step_x, 1);
			sm[1] -= SSF * 2;
			sm[3] -= SSF * 2;
			px = (px + 2) & 0x3FF;
		}

		edge_step(w0b, e0.step_y, 1);
		edge_step(w1b, e1.step_y, 1);
		edge_step(w2b, e2.step_y, 1);
	}
}

/* --- Rectángulos ------------------------------------------------------------ */

static void draw_rectangle(const GeVertex *v0, const GeVertex *v1, int rx1, int ry1, int rx2, int ry2){
	int ex1 = v0->x < v1->x ? v0->x : v1->x, ey1 = v0->y < v1->y ? v0->y : v1->y;
	int ex2 = (v0->x > v1->x ? v0->x : v1->x) - 1, ey2 = (v0->y > v1->y ? v0->y : v1->y) - 1;
	/* Centros de píxel en 16k + 7: la primera columna es (x1 + 6) >> 4, la
	   primera fila (y1 + 7) >> 4 y ambas acaban en (x2 + 7) >> 4, sin incluir */
	int min_x = ((ex1 & ~(SSF - 1)) > rx1 ? (ex1 & ~(SSF - 1)) : rx1) | (SSF / 2 - 1);
	int min_y = ((ey1 & ~(SSF - 1)) > ry1 ? (ey1 & ~(SSF - 1)) : ry1) | (SSF / 2 - 1);
	int max_x = ex2 - 1 < rx2 ? ex2 - 1 : rx2;
	int max_y = ey2 - 1 < ry2 ? ey2 - 1 : ry2;
	/* De abajo-izquierda a arriba-derecha está rotado y su primera fila
	   redondea como una columna (gpu/probe exp122) */
	int rotated = v0->x < v1->x && v0->y > v1->y;
	int fog, z = v1->z, c0[4], sec[4], i, c;
	float row_s = 0, row_t = 0, stx_s = 0, stx_t = 0, sty_s = 0, sty_t = 0, sto4[4], tto4[4];
	UVPlanes uvp;
	float auto_grad;
	s64 cur_y;

	if(min_x < ex1 - 2) min_x += SSF;
	if(min_y < ey1 - (rotated ? 2 : 1)) min_y += SSF;

	memset(&uvp, 0, sizeof(uvp));
	if(rs.enable_textures){
		float tc0s = v0->s, tc0t = v0->t, tc1s = v1->s, tc1t = v1->t;
		float diff_x, diff_y, diff_s, diff_t;
		int right = v0->x < v1->x, down = v0->y < v1->y, swap_st = right != down, k, blf;
		s64 left, top, right_x, bottom, CX[3], CY[3];
		double S[2], T[2], Q[2], cs[3], ct[3], cq[3];
		const GeVertex *vs[2] = { v0, v1 };

		if(rs.through_mode){
			tc0s *= 1.0f / (float)(1 << rs.width0_shift);
			tc1s *= 1.0f / (float)(1 << rs.width0_shift);
			tc0t *= 1.0f / (float)(1 << rs.height0_shift);
			tc1t *= 1.0f / (float)(1 << rs.height0_shift);
		}
		diff_x = (float)(ex2 - ex1 + 1) / (float)SSF;
		diff_y = (float)(ey2 - ey1 + 1) / (float)SSF;
		diff_s = tc1s - tc0s;
		diff_t = tc1t - tc0t;
		if(v0->x < v1->x){
			if(v0->y < v1->y){
				row_s = tc0s; row_t = tc0t;
				stx_s = 2.0f * diff_s / diff_x; sty_t = 2.0f * diff_t / diff_y;
			} else {
				row_s = tc1s; row_t = tc0t;
				stx_t = 2.0f * diff_t / diff_x; sty_s = 2.0f * -diff_s / diff_y;
			}
		} else {
			if(v0->y < v1->y){
				row_s = tc0s; row_t = tc1t;
				stx_t = 2.0f * -diff_t / diff_x; sty_s = 2.0f * diff_s / diff_y;
			} else {
				row_s = tc1s; row_t = tc1t;
				stx_s = 2.0f * -diff_s / diff_x; sty_t = 2.0f * -diff_t / diff_y;
			}
		}
		row_s += (stx_s / (float)(SSF * 2)) * (float)(min_x - ex1 + 1);
		row_t += (stx_t / (float)(SSF * 2)) * (float)(min_x - ex1 + 1);
		row_s += (sty_s / (float)(SSF * 2)) * (float)(min_y - ey1 + 1);
		row_t += (sty_t / (float)(SSF * 2)) * (float)(min_y - ey1 + 1);

		/* El GE interpola las uv de un sprite con los mismos planos que un
		   triángulo, desde tres esquinas que toman s del vértice que da su
		   x, t del que da su y (al revés si está rotado) y q del que da su
		   x (gpu/probe exp57, exp122, exp129) */
		for(k = 0; k < 2; k++){
			double r = rs.through_mode ? 1.0 : ge_recip(ge_trunc24(vs[k]->clipw));
			Q[k] = r;
			S[k] = ge_product24((double)ge_trunc24(k == 0 ? tc0s : tc1s) * r);
			T[k] = ge_product24((double)ge_trunc24(k == 0 ? tc0t : tc1t) * r);
		}
		left = v0->x < v1->x ? v0->x : v1->x;
		top = v0->y < v1->y ? v0->y : v1->y;
		right_x = v0->x > v1->x ? v0->x : v1->x;
		bottom = v0->y > v1->y ? v0->y : v1->y;
		blf = right && !down;
		CX[0] = left; CX[1] = right_x; CX[2] = blf ? right_x : left;
		CY[0] = blf ? bottom : top; CY[1] = top; CY[2] = bottom;
		for(k = 0; k < 3; k++){
			int xs = CX[k] == v0->x ? 0 : 1, ys = CY[k] == v0->y ? 0 : 1;
			cs[k] = S[swap_st ? ys : xs];
			ct[k] = T[swap_st ? xs : ys];
			cq[k] = Q[xs];
		}
		uvp = uv_planes_stq(CX, CY, cs, ct, cq);
	}
	/* Los planos de un sprite van en coordenadas normalizadas, también en through */
	auto_grad = uv_plane_gradient(&uvp, 0);

	sto4[0] = 0.0f; sto4[1] = 0.5f * stx_s; sto4[2] = 0.5f * sty_s; sto4[3] = 0.5f * stx_s + 0.5f * sty_s;
	tto4[0] = 0.0f; tto4[1] = 0.5f * stx_t; tto4[2] = 0.5f * sty_t; tto4[3] = 0.5f * stx_t + 0.5f * sty_t;

	fog = clamp_fog_depth(v1->fogdepth);
	unpack(v1->color0, c0);
	unpack(v1->color1, sec);
	if(double_secondary()) for(c = 0; c < 3; c++) sec[c] *= 2;

	if(rs.apply_depth_range && (v1->z < rs.minz || v1->z > rs.maxz)) return;

	for(cur_y = min_y; cur_y <= max_y; cur_y += SSF * 2, row_s += sty_s, row_t += sty_t){
		int px = min_x / SSF, py = (int)(cur_y / SSF), sy1 = cur_y + SSF > max_y ? -1 : 0;
		s32 sm[4];
		s64 cur_x;
		float st_s = row_s, st_t = row_t;
		sm[0] = 0; sm[1] = max_x - min_x - SSF; sm[2] = sy1; sm[3] = (max_x - min_x - SSF) | sy1;

		for(cur_x = min_x; cur_x <= max_x; cur_x += SSF * 2, st_s += stx_s, st_t += stx_t){
			int mask[4], prim[4][4];
			for(i = 0; i < 4; i++){
				mask[i] = sm[i] < 0 ? -1 : 0;
				memcpy(prim[i], c0, sizeof(c0));
			}
			if(rs.early_z)
				for(i = 0; i < 4; i++){
					if(mask[i] < 0) continue;
					if(!depth_passed(px + (i & 1), py + (i >> 1), z)) mask[i] = -1;
				}
			if(rs.enable_textures){
				float s[4], t[4], q[4];
				for(i = 0; i < 4; i++) q[i] = 1.0f / v1->clipw;
				if(uvp.valid){
					/* Centros en 16k + 7 aquí, en 16k + 8 para el GE */
					tex_coords_ge(&uvp, cur_x + 1, cur_y + 1, s, t, q);
				} else {
					for(i = 0; i < 4; i++){ s[i] = st_s + sto4[i]; t[i] = st_t + tto4[i]; }
				}
				apply_texturing4(prim, mask, s, t, q, auto_grad);
			}
			if(!rs.clear_mode)
				for(i = 0; i < 4; i++) for(c = 0; c < 3; c++) prim[i][c] += sec[c];
			for(i = 0; i < 4; i++){
				if(mask[i] < 0) continue;
				draw_pixel(px + (i & 1), py + (i >> 1), z, fog, prim[i]);
			}
			sm[1] -= SSF * 2;
			sm[3] -= SSF * 2;
			px = (px + 2) & 0x3FF;
		}
	}
}

static void clear_rectangle(const GeVertex *v0, const GeVertex *v1, int rx1, int ry1, int rx2, int ry2){
	int ex1 = v0->x < v1->x ? v0->x : v1->x, ey1 = v0->y < v1->y ? v0->y : v1->y;
	int ex2 = (v0->x > v1->x ? v0->x : v1->x) - 1, ey2 = (v0->y > v1->y ? v0->y : v1->y) - 1;
	int min_x = ((ex1 & ~(SSF - 1)) > rx1 ? (ex1 & ~(SSF - 1)) : rx1) | (SSF / 2 - 1);
	int min_y = ((ey1 & ~(SSF - 1)) > ry1 ? (ey1 & ~(SSF - 1)) : ry1) | (SSF / 2 - 1);
	int max_x = ex2 < rx2 ? ex2 : rx2, max_y = ey2 < ry2 ? ey2 : ry2;
	int ppx, ppy, pex, pey, w, x, y;
	u32 keep = 0xFFFFFFFFu, nc = v1->color0, nc16 = 0;

	/* Si la esquina superior izquierda pasa de la mitad, ese píxel no va */
	if(min_x < ex1 - 1) min_x += SSF;
	if(min_y < ey1 - 1) min_y += SSF;
	ppx = min_x / SSF; ppy = min_y / SSF;
	/* El último píxel solo si llega a la mitad */
	pex = (max_x - SSF / 2) / SSF; pey = (max_y - SSF / 2) / SSF;
	w = pex - ppx + 1;
	if(w <= 0) return;

	if(rs.depth_write)
		for(y = ppy; y <= pey; y++)
			for(x = 0; x < w; x++) set_depth(ppx + x, y, v1->z);

	if(rs.color_test && rs.stencil_test) keep = 0;
	else switch(rs.fb_format){
	case GE_FMT_565:  if(rs.color_test) keep = 0; break;
	case GE_FMT_5551: if(rs.color_test) keep = 0xFFFF8000u; else if(rs.stencil_test) keep = 0xFFFF7FFFu; break;
	case GE_FMT_4444: if(rs.color_test) keep = 0xFFFFF000u; else if(rs.stencil_test) keep = 0xFFFF0FFFu; break;
	default:          if(rs.color_test) keep = 0xFF000000u; else if(rs.stencil_test) keep = 0x00FFFFFFu; break;
	}
	/* Las máscaras de escritura cuentan también en modo clear */
	if(rs.apply_color_write_mask) keep |= rs.color_write_mask;

	switch(rs.fb_format){
	case GE_FMT_565:  nc16 = rgba8888_to_565(nc); break;
	case GE_FMT_5551: nc16 = rgba8888_to_5551(nc); break;
	case GE_FMT_4444: nc16 = rgba8888_to_4444(nc); break;
	default: break;
	}
	if(keep == 0xFFFFFFFFu) return;
	for(y = ppy; y <= pey; y++)
		for(x = 0; x < w; x++){
			if(rs.fb_format == GE_FMT_8888)
				fb_set32(ppx + x, y, (fb_get32(ppx + x, y) & keep) | (nc & ~keep));
			else
				fb_set16(ppx + x, y, ((fb_get16(ppx + x, y) & keep) | (nc16 & ~keep)) & 0xFFFF);
		}
	ge_stats.pixels += (u64)w * (u64)(pey - ppy + 1);
}

/* --- Puntos ----------------------------------------------------------------- */

/* Coordenadas a lo largo de una línea (p es el peso de v0) */
static void tex_coords_linear(const GeVertex *v0, const GeVertex *v1, float p, int proj, float *s, float *t){
	float q0 = 1.0f / v0->clipw, q1 = 1.0f / v1->clipw;
	float wq0 = p * q0, wq1 = (1.0f - p) * q1, qr;
	if(proj) qr = 1.0f / (v0->q * wq0 + v1->q * wq1);
	else qr = 1.0f / (wq0 + wq1);
	*s = (v0->s * wq0 + v1->s * wq1) * qr;
	*t = (v0->t * wq0 + v1->t * wq1) * qr;
}

static void draw_point(const GeVertex *v0){
	int x = v0->x / SSF, y = v0->y / SSF, z = v0->z, prim[4], sec[4], fog = 255, c;
	unpack(v0->color0, prim);

	if(rs.early_z){
		if(rs.apply_depth_range && (z < rs.minz || z > rs.maxz)) return;
		if(!depth_passed(x, y, z)) return;
	}

	if(rs.enable_textures){
		float s = v0->s, t = v0->t;
		int level, frac, bil, out[4];
		if(rs.through_mode){
			s = ge_trunc_texcoord(s) * (1.0f / (float)(1 << rs.width0_shift));
			t = ge_trunc_texcoord(t) * (1.0f / (float)(1 << rs.height0_shift));
		} else {
			/* Los mismos planos que un triángulo, planos (gpu/probe exp64) */
			UVPlanes p = uv_planes(v0, v0, v0, rs.texture_proj);
			if(p.valid){
				float s4[4], t4[4], q4[4];
				tex_coords_ge(&p, v0->x, v0->y, s4, t4, q4);
				s = s4[0]; t = t4[0];
			} else tex_coords_linear(v0, v0, 0.0f, rs.texture_proj, &s, &t);
		}
		sampling_params(0.0f, 0.0f, 1.0f / v0->clipw, &level, &frac, &bil, -1.0f);
		sample(s, t, prim, level, frac, bil, out);
		memcpy(prim, out, sizeof(out));
	}

	if(!rs.clear_mode){
		unpack(v0->color1, sec);
		for(c = 0; c < 3; c++) prim[c] += double_secondary() ? sec[c] * 2 : sec[c];
	}
	if(rs.apply_fog) fog = clamp_fog_depth(v0->fogdepth);
	draw_pixel(x, y, z, fog, prim);
}

/* --- Líneas ------------------------------------------------------------------- */

/* Qué píxeles enciende una línea, como el GE (gpu/probe exp72): salida por
   el rombo. Se enciende un píxel si la línea pasa por el interior de su
   rombo |x - cx| + |y - cy| < 1/2 y no termina dentro de él. */
typedef struct { int x, y; float t; } LinePixel;

/* Un punto en el borde del rombo cuenta como dentro en la esquina de arriba
   y sus lados, en líneas de eje mayor X; no en las esquinas izquierda y
   derecha (gpu/probe exp149). Las de eje mayor Y cambian x por y. */
static int line_diamond_edge_inside(s64 dx, s64 dy, int y_major){
	if(y_major){ s64 t = dx; dx = dy; dy = t; }
	return dy < 0;
}

static int in_line_diamond(s64 cx, s64 cy, s64 x, s64 y, int y_major){
	s64 dx = x - cx, dy = y - cy, d = llabs(dx) + llabs(dy);
	if(d != SSF / 2) return d < SSF / 2;
	return line_diamond_edge_inside(dx, dy, y_major);
}

static int line_crosses_diamond(s64 cx, s64 cy, s64 x0, s64 y0, s64 x1, s64 y1, int y_major){
	static const int signs[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	s64 lo_n = 0, lo_d = 1, hi_n = 1, hi_d = 1, tn, td, dxn, dyn, dn;
	int k;
	for(k = 0; k < 4; k++){
		s64 a = signs[k][0] * (x1 - x0) + signs[k][1] * (y1 - y0);
		s64 b = SSF / 2 - (signs[k][0] * (x0 - cx) + signs[k][1] * (y0 - cy));
		if(a == 0){
			if(b < 0) return 0;
		} else if(a > 0){
			if(b * hi_d < hi_n * a){ hi_n = b; hi_d = a; }
		} else if((-b) * lo_d > lo_n * (-a)){
			lo_n = -b; lo_d = -a;
		}
	}
	if(lo_n * hi_d > hi_n * lo_d) return 0;
	tn = lo_n * hi_d + hi_n * lo_d;
	td = 2 * lo_d * hi_d;
	dxn = (x0 - cx) * td + (x1 - x0) * tn;
	dyn = (y0 - cy) * td + (y1 - y0) * tn;
	dn = llabs(dxn) + llabs(dyn);
	if(dn < (SSF / 2) * td) return 1;
	return line_diamond_edge_inside(dxn, dyn, y_major);
}

static LinePixel *line_buf;
static int line_cap;

static int line_pixels(s64 x0, s64 y0, s64 x1, s64 y1){
	int n = 0, x_major, dir, bdir, k;
	s64 a0, a1, b0, b1, first, last, c;
	if(x0 == x1 && y0 == y1) return 0;
	/* Una diagonal es de eje mayor Y (gpu/probe exp137) */
	x_major = llabs(x1 - x0) > llabs(y1 - y0);
	a0 = x_major ? x0 : y0; a1 = x_major ? x1 : y1;
	b0 = x_major ? y0 : x0; b1 = x_major ? y1 : x1;
	dir = a1 >= a0 ? 1 : -1;
	bdir = b1 >= b0 ? 1 : -1;
	first = (a0 / SSF) - dir;
	last = (a1 / SSF) + dir;
	for(c = first; c != last + dir; c += dir){
		s64 ac = c * SSF + SSF / 2, num = b0 * (a1 - a0) + (ac - a0) * (b1 - b0), den = a1 - a0, bc, r;
		/* La coordenada menor en el centro de esta columna, por abajo */
		bc = num / den;
		if((num % den != 0) && ((num < 0) != (den < 0))) bc--;
		r = bc >= 0 ? bc / SSF : -((-bc + SSF - 1) / SSF);
		for(k = -1; k <= 1; k++){
			s64 rr = r + k * bdir, px = x_major ? c : rr, py = x_major ? rr : c;
			s64 cx = px * SSF + SSF / 2, cy = py * SSF + SSF / 2;
			if(in_line_diamond(cx, cy, x1, y1, !x_major)) continue;
			if(in_line_diamond(cx, cy, x0, y0, !x_major) || line_crosses_diamond(cx, cy, x0, y0, x1, y1, !x_major)){
				float t = (float)(ac - a0) / (float)(a1 - a0);
				if(n == line_cap){
					int ncap = line_cap ? line_cap * 2 : 256;
					LinePixel *nb = realloc(line_buf, (size_t)ncap * sizeof(LinePixel));
					if(!nb) return n;
					line_buf = nb;
					line_cap = ncap;
				}
				line_buf[n].x = (int)px;
				line_buf[n].y = (int)py;
				line_buf[n].t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
				n++;
			}
		}
	}
	return n;
}

/* Color Gouraud a lo largo de una línea (gpu/probe exp137): el gradiente
   en el eje mayor sale del recíproco de montaje, con 14 bits de fracción
   por subpíxel y por abajo, y el valor de un píxel es el color inicial más
   el gradiente por la distancia con signo de su centro a v0 */
static s64 line_fixed_at(s64 c0, s64 c1, s64 x0, s64 y0, s64 x1, s64 y1, int px, int py){
	int x_major = llabs(x1 - x0) > llabs(y1 - y0), e;
	s64 a0 = x_major ? x0 : y0, a1 = x_major ? x1 : y1;
	s64 ac = (s64)(x_major ? px : py) * SSF + SSF / 2, q, k, walk;
	if(a1 == a0) return c1;
	q = ge_setup_recip((u64)llabs(a1 - a0), &e);
	k = ((c1 - c0) * q) >> (e + 2);
	walk = a1 > a0 ? ac - a0 : a0 - ac;
	return (c0 * 16384 + k * walk) >> 14;
}

static int line_value_at(int c0, int c1, s64 x0, s64 y0, s64 x1, s64 y1, int px, int py, int max_v){
	s64 v = line_fixed_at(c0, c1, x0, y0, x1, y1, px, py);
	return (int)(v < 0 ? 0 : v > max_v ? max_v : v);
}

/* uv de una línea en modo transformado: s, t y q como en los planos de un
   triángulo, a lo largo de la línea como su color, y u = s / q con el
   recíproco del GE (gpu/texmtx/prims) */
typedef struct { double s[2], t[2], q[2]; int shift_s, shift_t, shift_q, valid; } LineUV;

static int shared_shift2(const double *v){
	double v3[3];
	v3[0] = v[0]; v3[1] = v[1]; v3[2] = 0.0;
	return shared_shift(v3);
}

static LineUV line_uv(const GeVertex *v0, const GeVertex *v1, int proj){
	LineUV uv;
	const GeVertex *v[2] = { v0, v1 };
	int i;
	memset(&uv, 0, sizeof(uv));
	for(i = 0; i < 2; i++){
		float w24 = ge_trunc24(v[i]->clipw);
		double r;
		if(!(w24 > 0.0f) || !isfinite(w24)) return uv;
		r = ge_recip(w24);
		uv.q[i] = proj ? ge_product24((double)ge_trunc24(v[i]->q) * r) : r;
		uv.s[i] = ge_product24((double)ge_trunc24(v[i]->s) * r);
		uv.t[i] = ge_product24((double)ge_trunc24(v[i]->t) * r);
	}
	uv.shift_s = shared_shift2(uv.s);
	uv.shift_t = shared_shift2(uv.t);
	uv.shift_q = shared_shift2(uv.q);
	uv.valid = 1;
	return uv;
}

static float line_uv_component(const double *c, int shift, const GeVertex *v0, const GeVertex *v1, int px, int py){
	s64 c0 = d2s64(ldexp(c[0], shift)), c1 = d2s64(ldexp(c[1], shift));
	s64 v = line_fixed_at(c0, c1, v0->x, v0->y, v1->x, v1->y, px, py);
	return ge_trunc24((float)ldexp((double)v, -shift));
}

static void line_tex_at(const LineUV *uv, const GeVertex *v0, const GeVertex *v1, int px, int py, float *s, float *t){
	float q = line_uv_component(uv->q, uv->shift_q, v0, v1, px, py);
	double r;
	if(!(q > 0.0f)){ *s = *t = 0.0f; return; }
	r = ge_recip(q);
	*s = ge_uv_product((double)line_uv_component(uv->s, uv->shift_s, v0, v1, px, py) * r);
	*t = ge_uv_product((double)line_uv_component(uv->t, uv->shift_t, v0, v1, px, py) * r);
}

static void draw_line(const GeVertex *v0, const GeVertex *v1, int rx1, int ry1, int rx2, int ry2){
	int ax = v0->x, ay = v0->y, az = v0->z, bx = v1->x, by = v1->y, bz = v1->z;
	int dx = bx - ax, dy = by - ay, steps, steps1, n, k, c;
	double xinc, yinc;
	int interp = rs.shade_gouraud && !(v0->color0 == v1->color0 && v0->color1 == v1->color1);
	int a_c0[4], b_c0[4], a_c1[4], b_c1[4];
	LineUV luv;

	steps = abs(dx) < abs(dy) ? abs(dy) / SSF : abs(dx) / SSF;
	/* Para no pasarse, ya que casi nunca empieza en el centro del píxel */
	if(dx < 0 && dx >= -SSF) dx++;
	if(dy < 0 && dy >= -SSF) dy++;
	xinc = (double)dx / steps;
	yinc = (double)dy / steps;
	unpack(v0->color0, a_c0); unpack(v1->color0, b_c0);
	unpack(v0->color1, a_c1); unpack(v1->color1, b_c1);
	steps1 = steps == 0 ? 1 : steps;
	memset(&luv, 0, sizeof(luv));
	if(rs.enable_textures && !rs.through_mode) luv = line_uv(v0, v1, rs.texture_proj);

	n = line_pixels(ax, ay, bx, by);
	for(k = 0; k < n; k++){
		const LinePixel *lp = &line_buf[k];
		int i = (int)lroundf(lp->t * (float)steps), z, px, py, ok, prim[4], sec[4], fog = 255;
		double x = (double)lp->x * SSF + SSF / 2, y = (double)lp->y * SSF + SSF / 2;
		if(i < 0) i = 0;
		if(i > steps) i = steps;
		/* La profundidad como el color Gouraud: el píxel de un extremo
		   extrapola a su centro (Coded Arms) */
		z = line_value_at(az, bz, ax, ay, bx, by, lp->x, lp->y, 65535);
		px = (int)x / SSF;
		py = (int)y / SSF;
		ok = x >= rx1 && y >= ry1 && x <= rx2 && y <= ry2;
		if(ok && rs.early_z){
			if(rs.apply_depth_range && (z < rs.minz || z > rs.maxz)) ok = 0;
			if(!depth_passed(px, py, z)) ok = 0;
		}
		if(!ok) continue;

		if(interp){
			for(c = 0; c < 4; c++){
				prim[c] = line_value_at(a_c0[c], b_c0[c], ax, ay, bx, by, lp->x, lp->y, 255);
				if(c < 3) sec[c] = line_value_at(a_c1[c], b_c1[c], ax, ay, bx, by, lp->x, lp->y, 255);
			}
		} else {
			memcpy(prim, b_c0, sizeof(prim));
			memcpy(sec, b_c1, sizeof(sec));
		}
		/* steps1: una línea de menos de un píxel conserva la niebla de v0 (SOCOM) */
		if(rs.apply_fog)
			fog = clamp_fog_depth((v0->fogdepth * (float)(steps1 - i) + v1->fogdepth * (float)i) / (float)steps1);
		if(rs.antialias_lines) prim[3] = ge_line_coverage_alpha(ax, ay, bx, by, lp->x, lp->y);

		if(rs.enable_textures){
			float s, s1, t, t1, ds, dt, w;
			int level, frac, bil, out[4];
			if(rs.through_mode){
				float tcs = (v0->s * (float)(steps1 - i) + v1->s * (float)i) / (float)steps1;
				float tct = (v0->t * (float)(steps1 - i) + v1->t * (float)i) / (float)steps1;
				float tcs1 = (v0->s * (float)(steps1 - i - 1) + v1->s * (float)(i + 1)) / (float)steps1;
				float tct1 = (v0->t * (float)(steps1 - i - 1) + v1->t * (float)(i + 1)) / (float)steps1;
				s = tcs * (1.0f / (float)(1 << rs.width0_shift));
				s1 = tcs1 * (1.0f / (float)(1 << rs.width0_shift));
				t = tct * (1.0f / (float)(1 << rs.height0_shift));
				t1 = tct1 * (1.0f / (float)(1 << rs.height0_shift));
			} else if(luv.valid){
				int x_major = abs(dx) > abs(dy);
				int sx = x_major ? (dx >= 0 ? 1 : -1) : 0, sy = x_major ? 0 : (dy >= 0 ? 1 : -1);
				line_tex_at(&luv, v0, v1, lp->x, lp->y, &s, &t);
				line_tex_at(&luv, v0, v1, lp->x + sx, lp->y + sy, &s1, &t1);
			} else {
				tex_coords_linear(v0, v1, (float)(steps1 - i) / (float)steps1, rs.texture_proj, &s, &t);
				tex_coords_linear(v0, v1, (float)(steps1 - i - 1) / (float)steps1, rs.texture_proj, &s1, &t1);
			}
			ds = xinc == 0.0 ? 0.0f : (s1 - s) * (float)SSF * (float)(1.0f / xinc);
			dt = yinc == 0.0 ? 0.0f : (t1 - t) * (float)SSF * (float)(1.0f / yinc);
			w = (v0->clipw * (float)(steps - i) + v1->clipw * (float)i) / (float)steps1;
			sampling_params(ds, dt, 1.0f / w, &level, &frac, &bil, -1.0f);
			if(rs.antialias_lines){
				s = (float)(((float)px + xinc / 32.0f) / 512.0f);
				t = (float)(((float)py + yinc / 32.0f) / 512.0f);
				bil = 1;
			}
			sample(s, t, prim, level, frac, bil, out);
			memcpy(prim, out, sizeof(out));
		}
		if(!rs.clear_mode) for(c = 0; c < 3; c++) prim[c] += sec[c];
		draw_pixel(px, py, z, fog, prim);
	}
}

/* --- Entrada (BinManager) ------------------------------------------------- */

static inline int imin(int a, int b){ return a < b ? a : b; }
static inline int imax(int a, int b){ return a > b ? a : b; }

/* Rectángulo envolvente en coordenadas de pantalla recortado por la tijera */
static int range_of(const GeVertex *const *v, int n, int *x1, int *y1, int *x2, int *y2){
	int i, mnx = v[0]->x, mny = v[0]->y, mxx = v[0]->x, mxy = v[0]->y;
	for(i = 1; i < n; i++){
		mnx = imin(mnx, v[i]->x); mny = imin(mny, v[i]->y);
		mxx = imax(mxx, v[i]->x); mxy = imax(mxy, v[i]->y);
	}
	*x1 = imax(mnx & ~(SSF - 1), rs.sc_x1);
	*y1 = imax(mny & ~(SSF - 1), rs.sc_y1);
	*x2 = imin(mxx | (SSF - 1), rs.sc_x2);
	*y2 = imin(mxy | (SSF - 1), rs.sc_y2);
	return *x2 >= *x1 && *y2 >= *y1;
}

void ge_raster_triangle(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	const GeVertex *v[3] = { v0, v1, v2 };
	s64 d01x = (s64)v0->x - v1->x, d01y = (s64)v0->y - v1->y;
	s64 d02x = (s64)v0->x - v2->x, d02y = (s64)v0->y - v2->y;
	int x1, y1, x2, y2;
	/* Solo los de orden antihorario y con área (gpu/probe exp118, exp132) */
	if(d01x * d02y - d01y * d02x <= 0) return;
	if(!range_of(v, 3, &x1, &y1, &x2, &y2)) return;
	ge_stats.primitives++;
	texture_snapshot();
	draw_triangle(v0, v1, v2, x1, y1, x2, y2);
}

void ge_raster_rect(const GeVertex *v0, const GeVertex *v1){
	const GeVertex *v[2] = { v0, v1 };
	int x1, y1, x2, y2;
	if(!range_of(v, 2, &x1, &y1, &x2, &y2)) return;
	ge_stats.primitives++;
	texture_snapshot();
	draw_rectangle(v0, v1, x1, y1, x2, y2);
}

void ge_raster_clear_rect(const GeVertex *v0, const GeVertex *v1){
	const GeVertex *v[2] = { v0, v1 };
	int x1, y1, x2, y2;
	if(!range_of(v, 2, &x1, &y1, &x2, &y2)) return;
	ge_stats.primitives++;
	clear_rectangle(v0, v1, x1, y1, x2, y2);
}

void ge_raster_line(const GeVertex *v0, const GeVertex *v1){
	const GeVertex *v[2] = { v0, v1 };
	int x1, y1, x2, y2;
	if(!range_of(v, 2, &x1, &y1, &x2, &y2)) return;
	ge_stats.primitives++;
	texture_snapshot();
	draw_line(v0, v1, x1, y1, x2, y2);
}

void ge_raster_point(const GeVertex *v0){
	const GeVertex *v[1] = { v0 };
	int x1, y1, x2, y2;
	if(!range_of(v, 1, &x1, &y1, &x2, &y2)) return;
	ge_stats.primitives++;
	texture_snapshot();
	draw_point(v0);
}
