/**
 * WIISP - ge_raster.c
 * GE: rasterizador por software y pipeline de píxeles.
 *
 * Reglas del hardware (medidas en una PSP real, según PPSSPP
 * GPU/Software, GPLv2+):
 *  - coordenadas con 4 bits de subpíxel; cada píxel se muestrea en su
 *    centro (16k + 8); bordes izquierdo y superior incluidos
 *  - sprites: primera columna (x1 + 6) >> 4, primera fila (y1 + 7) >> 4,
 *    terminan en (x2 + 7) >> 4 (exclusivo); color y z del segundo vértice
 *  - función de textura: modulate = (prim + 1) * tex / 256, etc.
 *  - blending: ((c*2+1) * (f*2+1)) / 1024 por término
 *  - orden: rango de z, test alfa, niebla, test de color, stencil, z,
 *    blending, dithering, logic op, máscaras
 *  - el canal alfa del framebuffer es el stencil: sin test de stencil se
 *    conserva el que había
 *
 * Framebuffer y z-buffer viven en la VRAM emulada.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include "gpu/ge_internal.h"
#include "core/memory.h"

/* --- Estado precalculado por tanda de primitivas ------------------------- */

static struct {
	u8 *fb, *zb;              /* punteros a la VRAM */
	u32 fb_off, z_off;        /* desplazamiento en la VRAM */
	u32 fb_stride, z_stride, fb_format;
	int sx1, sy1, sx2, sy2;   /* tijera (inclusiva) */
	int clear, clear_color, clear_stencil, clear_depth;
	int through;
	int tex_enabled;
	GeTexture tex;
	u32 tex_func, tex_rgba, tex_double;
	u32 env_color;
	int clamp_s, clamp_t;
	int mag_linear, min_linear, mipmap;
	int alpha_test, alpha_func, alpha_ref, alpha_mask;
	int color_test, color_func; u32 color_ref, color_mask;
	int stencil_test, stencil_func, stencil_ref, stencil_mask, sfail, zfail, zpass;
	int depth_test, depth_func, depth_write;
	int depth_range; int minz, maxz;
	int blend, blend_src, blend_dst, blend_eq; u32 fix_a, fix_b;
	int dither; int dither_matrix[16];
	int logic_op, logic;
	u32 write_mask;           /* bits del color destino que NO se escriben (formato 8888) */
	int fog; u32 fog_color;
} rs;

static u8 *vram_at(u32 addr){
	return psp_mem.vram + ((addr - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1));
}

void ge_raster_begin(void){
	int i;
	rs.fb_off = (ge_fb_addr() - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1);
	rs.z_off = (ge_z_addr() - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1);
	rs.fb = vram_at(ge_fb_addr());
	rs.zb = vram_at(ge_z_addr());
	rs.fb_stride = ge_fb_stride();
	rs.z_stride = ge_z_stride();
	rs.fb_format = ge_fb_format();

	rs.sx1 = (int)(ge.cmd[GE_SCISSOR1] & 0x3FF);
	rs.sy1 = (int)((ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF);
	rs.sx2 = (int)(ge.cmd[GE_SCISSOR2] & 0x3FF);
	rs.sy2 = (int)((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF);

	rs.clear = ge.cmd[GE_CLEARMODE] & 1;
	rs.clear_color = (ge.cmd[GE_CLEARMODE] >> 8) & 1;
	rs.clear_stencil = (ge.cmd[GE_CLEARMODE] >> 9) & 1;
	rs.clear_depth = (ge.cmd[GE_CLEARMODE] >> 10) & 1;
	rs.through = ge_through();

	rs.tex_enabled = ge_enabled(GE_TEXTUREMAPENABLE) && !rs.clear;
	if(rs.tex_enabled) ge_texture_setup(&rs.tex);
	rs.tex_func = ge.cmd[GE_TEXFUNC] & 7;
	rs.tex_rgba = (ge.cmd[GE_TEXFUNC] >> 8) & 1;
	rs.tex_double = (ge.cmd[GE_TEXFUNC] >> 16) & 1;
	rs.env_color = ge.cmd[GE_TEXENVCOLOR];
	rs.clamp_s = ge.cmd[GE_TEXWRAP] & 1;
	rs.clamp_t = (ge.cmd[GE_TEXWRAP] >> 8) & 1;
	rs.mag_linear = (ge.cmd[GE_TEXFILTER] >> 8) & 1;
	rs.min_linear = ge.cmd[GE_TEXFILTER] & 1;
	rs.mipmap = (ge.cmd[GE_TEXFILTER] >> 2) & 1;

	rs.alpha_test = ge_enabled(GE_ALPHATESTENABLE) && !rs.clear;
	rs.alpha_func = (int)(ge.cmd[GE_ALPHATEST] & 7);
	rs.alpha_mask = (int)((ge.cmd[GE_ALPHATEST] >> 16) & 0xFF);
	rs.alpha_ref = (int)((ge.cmd[GE_ALPHATEST] >> 8) & 0xFF) & rs.alpha_mask;

	rs.color_test = ge_enabled(GE_COLORTESTENABLE) && !rs.clear;
	rs.color_func = (int)(ge.cmd[GE_COLORTEST] & 3);
	rs.color_mask = ge.cmd[GE_COLORTESTMASK] & 0xFFFFFF;
	rs.color_ref = ge.cmd[GE_COLORREF] & rs.color_mask;

	rs.stencil_test = ge_enabled(GE_STENCILTESTENABLE) && !rs.clear && rs.fb_format != GE_FMT_565;
	rs.stencil_func = (int)(ge.cmd[GE_STENCILTEST] & 7);
	rs.stencil_mask = (int)((ge.cmd[GE_STENCILTEST] >> 16) & 0xFF);
	rs.stencil_ref = (int)((ge.cmd[GE_STENCILTEST] >> 8) & 0xFF);
	rs.sfail = (int)(ge.cmd[GE_STENCILOP] & 7);
	rs.zfail = (int)((ge.cmd[GE_STENCILOP] >> 8) & 7);
	rs.zpass = (int)((ge.cmd[GE_STENCILOP] >> 16) & 7);

	rs.depth_test = ge_enabled(GE_ZTESTENABLE) && !rs.clear;
	rs.depth_func = (int)(ge.cmd[GE_ZTEST] & 7);
	rs.depth_write = rs.depth_test && !(ge.cmd[GE_ZWRITEDISABLE] & 1);
	rs.depth_range = !rs.through;
	rs.minz = (int)(ge.cmd[GE_MINZ] & 0xFFFF);
	rs.maxz = (int)(ge.cmd[GE_MAXZ] & 0xFFFF);

	rs.blend = ge_enabled(GE_ALPHABLENDENABLE) && !rs.clear;
	rs.blend_src = (int)(ge.cmd[GE_BLENDMODE] & 0xF);
	rs.blend_dst = (int)((ge.cmd[GE_BLENDMODE] >> 4) & 0xF);
	rs.blend_eq = (int)((ge.cmd[GE_BLENDMODE] >> 8) & 7);
	rs.fix_a = ge.cmd[GE_BLENDFIXEDA];
	rs.fix_b = ge.cmd[GE_BLENDFIXEDB];

	rs.dither = ge_enabled(GE_DITHERENABLE);
	for(i = 0; i < 16; i++){
		int v = (int)((ge.cmd[GE_DITH0 + i / 4] >> ((i % 4) * 4)) & 0xF);
		rs.dither_matrix[i] = v >= 8 ? v - 16 : v;
	}
	rs.logic_op = ge_enabled(GE_LOGICOPENABLE) && !rs.clear;
	rs.logic = (int)(ge.cmd[GE_LOGICOP] & 0xF);
	rs.write_mask = (ge.cmd[GE_MASKRGB] & 0xFFFFFF) | ((ge.cmd[GE_MASKALPHA] & 0xFF) << 24);

	rs.fog = ge_enabled(GE_FOGENABLE) && !rs.through && !rs.clear;
	rs.fog_color = ge.cmd[GE_FOGCOLOR];
}

/* --- Píxeles ----------------------------------------------------------------- */

static inline int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

static inline u32 read_fb(int x, int y){
	u32 off = (rs.fb_off + ((u32)y * rs.fb_stride + (u32)x) * (rs.fb_format == GE_FMT_8888 ? 4u : 2u)) & (PSP_VRAM_SIZE - 1);
	u8 *p = psp_mem.vram + off;
	if(rs.fb_format == GE_FMT_8888) return rd_le32(p);
	{
		u8 c[4];
		ge_decode_color(rs.fb_format, rd_le16(p), c);
		if(rs.fb_format == GE_FMT_565) c[3] = 0;
		return (u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16) | ((u32)c[3] << 24);
	}
}

static inline u16 to16(u32 fmt, u32 c){
	u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
	switch(fmt){
	case GE_FMT_565:  return (u16)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
	case GE_FMT_5551: return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15));
	default:          return (u16)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
	}
}

/* Escribe respetando la máscara (bits a 1 = conservar el valor anterior) */
static inline void write_fb(int x, int y, u32 value, u32 old, u32 mask){
	u32 off = (rs.fb_off + ((u32)y * rs.fb_stride + (u32)x) * (rs.fb_format == GE_FMT_8888 ? 4u : 2u)) & (PSP_VRAM_SIZE - 1);
	u8 *p = psp_mem.vram + off;
	if(rs.fb_format == GE_FMT_8888){
		wr_le32(p, (value & ~mask) | (old & mask));
	} else {
		u16 v = to16(rs.fb_format, value);
		if(mask){
			u16 m = to16(rs.fb_format, mask), o = rd_le16(p);
			v = (u16)((v & ~m) | (o & m));
		}
		wr_le16(p, v);
	}
}

static inline u16 *zptr(int x, int y){
	return (u16 *)(void *)(psp_mem.vram + ((rs.z_off + ((u32)y * rs.z_stride + (u32)x) * 2) & (PSP_VRAM_SIZE - 1)));
}

static inline u16 read_z(int x, int y){ return rd_le16(zptr(x, y)); }
static inline void write_z(int x, int y, u16 z){ wr_le16(zptr(x, y), z); }

static inline int compare(int func, int a, int b){
	switch(func){
	case 0: return 0;
	case 1: return 1;
	case 2: return a == b;
	case 3: return a != b;
	case 4: return a < b;
	case 5: return a <= b;
	case 6: return a > b;
	default: return a >= b;
	}
}

static inline int stencil_of(u32 dst){
	switch(rs.fb_format){
	case GE_FMT_565:  return 0;
	case GE_FMT_5551: return (dst >> 31) ? 0xFF : 0;
	case GE_FMT_4444: return (int)((dst >> 24) & 0xF0) | (int)((dst >> 28) & 0xF);
	default:          return (int)(dst >> 24);
	}
}

static inline int stencil_op(int op, int old){
	switch(op){
	case 0: return old;
	case 1: return 0;
	case 2: return rs.stencil_ref;
	case 3: return (~old) & 0xFF;
	case 4:
		if(rs.fb_format == GE_FMT_8888) return old != 0xFF ? old + 1 : old;
		if(rs.fb_format == GE_FMT_5551) return 0xFF;
		return old < 0xF0 ? old + 0x10 : old;
	default:
		if(rs.fb_format == GE_FMT_4444) return old >= 0x10 ? old - 0x10 : old;
		if(rs.fb_format == GE_FMT_5551) return 0;
		return old ? old - 1 : 0;
	}
}

/* Escribe solo el stencil (canal alfa) cuando falla un test */
static void write_stencil(int x, int y, int stencil){
	u32 old = read_fb(x, y);
	write_fb(x, y, (old & 0x00FFFFFF) | ((u32)stencil << 24), old, rs.write_mask | 0x00FFFFFF);
}

static inline int blend_factor(int f, int is_src, const int *src, const int *dst, int ch){
	switch(f){
	case 0: return is_src ? dst[ch] : src[ch];
	case 1: return 255 - (is_src ? dst[ch] : src[ch]);
	case 2: return src[3];
	case 3: return 255 - src[3];
	case 4: return dst[3];
	case 5: return 255 - dst[3];
	case 6: return 2 * src[3];
	case 7: return 255 - 2 * src[3];
	case 8: return 2 * dst[3];
	case 9: return 255 - 2 * dst[3];
	default: return (int)(((is_src ? rs.fix_a : rs.fix_b) >> (ch * 8)) & 0xFF);
	}
}

static inline int blend_term(int c, int f){
	int t = ((2 * c + 1) * (2 * (f < 0 ? -f : f) + 1)) >> 10;
	return f < 0 ? -t : t;
}

/* Procesa un fragmento. color: RGBA ya texturizado (sin clamp). z 0..65535, fog 0..255 */
static void draw_pixel(int x, int y, int z, int fog, int *col){
	u32 dst, out;
	int stencil, k;

	ge_stats.pixels++;
	for(k = 0; k < 4; k++) col[k] = clampi(col[k], 0, 255);

	if(rs.depth_range && (z < rs.minz || z > rs.maxz)) return;

	if(rs.clear){
		dst = read_fb(x, y);
		if(rs.clear_depth) write_z(x, y, (u16)z);
		if(rs.dither)
			for(k = 0; k < 3; k++) col[k] = clampi(col[k] + rs.dither_matrix[(y & 3) * 4 + (x & 3)], 0, 255);
		out = (u32)col[0] | ((u32)col[1] << 8) | ((u32)col[2] << 16) | ((u32)col[3] << 24);
		if(!rs.clear_color) out = (out & 0xFF000000u) | (dst & 0x00FFFFFFu);
		if(!rs.clear_stencil) out = (out & 0x00FFFFFFu) | (dst & 0xFF000000u);
		write_fb(x, y, out, dst, rs.write_mask);
		return;
	}

	if(rs.alpha_test && !compare(rs.alpha_func, col[3] & rs.alpha_mask, rs.alpha_ref)) return;

	if(rs.fog){
		u32 fc = rs.fog_color;
		for(k = 0; k < 3; k++)
			col[k] = (col[k] * fog + (int)((fc >> (k * 8)) & 0xFF) * (255 - fog) + 255) / 256;
	}

	if(rs.color_test){
		u32 c = ((u32)col[0] | ((u32)col[1] << 8) | ((u32)col[2] << 16)) & rs.color_mask;
		int pass = rs.color_func == 0 ? 0 : rs.color_func == 1 ? 1 :
		           rs.color_func == 2 ? c == rs.color_ref : c != rs.color_ref;
		if(!pass) return;
	}

	dst = read_fb(x, y);
	stencil = stencil_of(dst);
	if(rs.stencil_test){
		int s = stencil & rs.stencil_mask, ref = rs.stencil_ref & rs.stencil_mask;
		if(!compare(rs.stencil_func, ref, s)){
			write_stencil(x, y, stencil_op(rs.sfail, stencil));
			return;
		}
		if(rs.depth_test && !compare(rs.depth_func, z, read_z(x, y))){
			write_stencil(x, y, stencil_op(rs.zfail, stencil));
			return;
		}
		stencil = stencil_op(rs.zpass, stencil);
	} else if(rs.depth_test && !compare(rs.depth_func, z, read_z(x, y))){
		return;
	}
	if(rs.depth_write) write_z(x, y, (u16)z);

	if(rs.blend){
		int d[4] = { (int)(dst & 0xFF), (int)((dst >> 8) & 0xFF), (int)((dst >> 16) & 0xFF), (int)(dst >> 24) };
		int res[3];
		for(k = 0; k < 3; k++){
			int sf = blend_factor(rs.blend_src, 1, col, d, k);
			int df = blend_factor(rs.blend_dst, 0, col, d, k);
			int s = blend_term(col[k], sf), t = blend_term(d[k], df);
			switch(rs.blend_eq){
			case 0: res[k] = s + t; break;
			case 1: res[k] = s - t; break;
			case 2: res[k] = t - s; break;
			case 3: res[k] = col[k] < d[k] ? col[k] : d[k]; break;
			case 4: res[k] = col[k] > d[k] ? col[k] : d[k]; break;
			case 5: res[k] = col[k] > d[k] ? col[k] - d[k] : d[k] - col[k]; break;
			default: res[k] = col[k]; break;
			}
		}
		for(k = 0; k < 3; k++) col[k] = res[k];
	}
	if(rs.dither)
		for(k = 0; k < 3; k++) col[k] += rs.dither_matrix[(y & 3) * 4 + (x & 3)];
	for(k = 0; k < 3; k++) col[k] = clampi(col[k], 0, 255);
	out = (u32)col[0] | ((u32)col[1] << 8) | ((u32)col[2] << 16) | ((u32)stencil << 24);

	if(rs.logic_op){
		u32 o = dst, n = out;
		switch(rs.logic){
		case 0:  n &= 0xFF000000u; break;
		case 1:  n = n & (o | 0xFF000000u); break;
		case 2:  n = n & (~o | 0xFF000000u); break;
		case 3:  break;
		case 4:  n = (~n & (o & 0x00FFFFFFu)) | (n & 0xFF000000u); break;
		case 5:  n = (o & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		case 6:  n = n ^ (o & 0x00FFFFFFu); break;
		case 7:  n = n | (o & 0x00FFFFFFu); break;
		case 8:  n = (~(n | o) & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		case 9:  n = (~(n ^ o) & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		case 10: n = (~o & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		case 11: n = n | (~o & 0x00FFFFFFu); break;
		case 12: n = (~n & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		case 13: n = ((~n | o) & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		case 14: n = (~(n & o) & 0x00FFFFFFu) | (n & 0xFF000000u); break;
		default: n |= 0x00FFFFFFu; break;
		}
		out = n;
	}
	write_fb(x, y, out, dst, rs.write_mask);
}

/* --- Texturas ------------------------------------------------------------- */

static inline int wrap(int v, int size, int clamp){
	if(clamp) return v < 0 ? 0 : (v >= size ? size - 1 : v);
	return v & (size - 1);
}

static void sample(float s, float t, int lvl, int linear, int *out){
	int w = (int)rs.tex.level_w[lvl], h = (int)rs.tex.level_h[lvl];
	u8 c[4];
	if(!linear){
		int u = ge_f2i(floorf(s * (float)w)), v = ge_f2i(floorf(t * (float)h));
		ge_texture_fetch(&rs.tex, lvl, wrap(u, w, rs.clamp_s), wrap(v, h, rs.clamp_t), c);
		out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3];
	} else {
		/* 4 bits de fracción, centro del texel en +8 */
		int bu = ge_f2i(floorf(s * (float)w * 16.0f)) - 8, bv = ge_f2i(floorf(t * (float)h * 16.0f)) - 8;
		int fu = bu & 15, fv = bv & 15, k;
		int u0 = bu >> 4, v0 = bv >> 4;
		u8 c00[4], c10[4], c01[4], c11[4];
		ge_texture_fetch(&rs.tex, lvl, wrap(u0, w, rs.clamp_s), wrap(v0, h, rs.clamp_t), c00);
		ge_texture_fetch(&rs.tex, lvl, wrap(u0 + 1, w, rs.clamp_s), wrap(v0, h, rs.clamp_t), c10);
		ge_texture_fetch(&rs.tex, lvl, wrap(u0, w, rs.clamp_s), wrap(v0 + 1, h, rs.clamp_t), c01);
		ge_texture_fetch(&rs.tex, lvl, wrap(u0 + 1, w, rs.clamp_s), wrap(v0 + 1, h, rs.clamp_t), c11);
		for(k = 0; k < 4; k++){
			int top = c00[k] * (16 - fu) + c10[k] * fu;
			int bot = c01[k] * (16 - fu) + c11[k] * fu;
			out[k] = (top * (16 - fv) + bot * fv) >> 8;
		}
	}
}

/* Aplica la función de textura sobre el color primario (enteros 0..255) */
static void texture_function(int *prim, const int *tex){
	int k, rgb[3], a;
	int dbl = rs.tex_double;
	switch(rs.tex_func){
	case 0: /* modulate */
		for(k = 0; k < 3; k++) rgb[k] = (prim[k] + 1) * tex[k] * (dbl ? 2 : 1) / 256;
		a = rs.tex_rgba ? (prim[3] + 1) * tex[3] / 256 : prim[3];
		break;
	case 1: /* decal */
		if(rs.tex_rgba){
			for(k = 0; k < 3; k++)
				rgb[k] = ((prim[k] + 1) * (255 - tex[3]) + (tex[k] + 1) * tex[3]) / (dbl ? 128 : 256);
		} else for(k = 0; k < 3; k++) rgb[k] = tex[k] * (dbl ? 2 : 1);
		a = prim[3];
		break;
	case 2: /* blend con el color de entorno */
		for(k = 0; k < 3; k++){
			int env = (int)((rs.env_color >> (k * 8)) & 0xFF);
			rgb[k] = ((255 - tex[k]) * prim[k] + tex[k] * env + 255) / (dbl ? 128 : 256);
		}
		a = rs.tex_rgba ? (prim[3] + 1) * tex[3] / 256 : prim[3];
		break;
	case 3: /* replace */
		for(k = 0; k < 3; k++) rgb[k] = tex[k] * (dbl ? 2 : 1);
		a = rs.tex_rgba ? tex[3] : prim[3];
		break;
	default: /* add */
		for(k = 0; k < 3; k++) rgb[k] = (prim[k] + tex[k]) * (dbl ? 2 : 1);
		a = rs.tex_rgba ? (prim[3] + 1) * tex[3] / 256 : prim[3];
		break;
	}
	for(k = 0; k < 3; k++) prim[k] = rgb[k];
	prim[3] = a;
}

/* Elige nivel de mipmap y filtro. lod en log2 (texels por píxel). */
static void choose_level(float lod, int *lvl, int *linear){
	u32 mode = ge.cmd[GE_TEXLEVEL] & 3;
	float bias = (float)(s8)((ge.cmd[GE_TEXLEVEL] >> 16) & 0xFF) / 16.0f;
	float l = mode == 1 ? bias : lod + bias;
	int max_level = rs.tex.levels - 1;
	*linear = l > 0.0f ? rs.min_linear : rs.mag_linear;
	if(rs.mipmap && max_level > 0 && l > 0.0f){
		int n = (int)(l + 0.5f);
		*lvl = n > max_level ? max_level : n;
	} else *lvl = 0;
}

/* Color final de un fragmento con textura y color secundario */
static void shade(int *col, float s, float t, int lvl, int linear, const int *sec){
	int k;
	if(rs.tex_enabled){
		int tex[4];
		sample(s, t, lvl, linear, tex);
		for(k = 0; k < 4; k++) col[k] = clampi(col[k], 0, 255);
		texture_function(col, tex);
	}
	if(sec)
		for(k = 0; k < 3; k++) col[k] += sec[k] * (rs.tex_enabled && rs.tex_double ? 2 : 1);
}

/* --- Triángulos ------------------------------------------------------------ */

typedef struct { s32 x, y; } Pt;

static int right_or_flat_bottom(Pt v, Pt l1, Pt l2){
	if(l1.y == l2.y) return v.y < l1.y;
	{
		s64 dy = (s64)l2.y - l1.y;
		s64 lhs = (s64)(v.x - l1.x) * dy;
		s64 rhs = (s64)(l2.x - l1.x) * (v.y - l1.y);
		return dy > 0 ? lhs < rhs : lhs > rhs;
	}
}

static inline s64 edge(Pt a, Pt b, s64 x, s64 y){
	return (s64)(a.y - b.y) * x + (s64)(b.x - a.x) * y + ((s64)b.y * a.x - (s64)b.x * a.y);
}

static float texture_lod(const GeVertex *v[3], const Pt *p, s64 area16){
	/* Área en texels del triángulo frente a su área en píxeles */
	float tw = (float)rs.tex.width, th = (float)rs.tex.height;
	float s0 = v[0]->s / v[0]->q, t0 = v[0]->t / v[0]->q;
	float s1 = v[1]->s / v[1]->q, t1 = v[1]->t / v[1]->q;
	float s2 = v[2]->s / v[2]->q, t2 = v[2]->t / v[2]->q;
	float ta = fabsf(((s1 - s0) * (t2 - t0) - (s2 - s0) * (t1 - t0)) * tw * th);
	float pa = (float)(area16 < 0 ? -area16 : area16) / 256.0f;
	(void)p;
	if(pa <= 0.0f || ta <= 0.0f) return 0.0f;
	return 0.5f * log2f(ta / pa);
}

void ge_raster_triangle(const GeVertex *a, const GeVertex *b, const GeVertex *c){
	const GeVertex *v[3] = { a, b, c };
	Pt p[3];
	s64 area;
	int i, minx, miny, maxx, maxy, x, y, lvl = 0, linear = 0;
	int bias[3];
	float inv_area;

	for(i = 0; i < 3; i++){
		p[i].x = ge_f2i(floorf(v[i]->x * 16.0f));
		p[i].y = ge_f2i(floorf(v[i]->y * 16.0f));
	}
	area = (s64)(p[1].x - p[0].x) * (p[2].y - p[0].y) - (s64)(p[2].x - p[0].x) * (p[1].y - p[0].y);
	if(area == 0) return;
	if(area < 0){
		Pt tp = p[1]; p[1] = p[2]; p[2] = tp;
		{ const GeVertex *tv = v[1]; v[1] = v[2]; v[2] = tv; }
		area = -area;
	}
	bias[0] = right_or_flat_bottom(p[0], p[1], p[2]) ? -1 : 0;
	bias[1] = right_or_flat_bottom(p[1], p[2], p[0]) ? -1 : 0;
	bias[2] = right_or_flat_bottom(p[2], p[0], p[1]) ? -1 : 0;

	minx = p[0].x; maxx = p[0].x; miny = p[0].y; maxy = p[0].y;
	for(i = 1; i < 3; i++){
		if(p[i].x < minx) minx = p[i].x;
		if(p[i].x > maxx) maxx = p[i].x;
		if(p[i].y < miny) miny = p[i].y;
		if(p[i].y > maxy) maxy = p[i].y;
	}
	minx = (minx >> 4); maxx = (maxx >> 4);
	miny = (miny >> 4); maxy = (maxy >> 4);
	if(minx < rs.sx1) minx = rs.sx1;
	if(miny < rs.sy1) miny = rs.sy1;
	if(maxx > rs.sx2) maxx = rs.sx2;
	if(maxy > rs.sy2) maxy = rs.sy2;
	if(minx > maxx || miny > maxy) return;

	if(rs.tex_enabled) choose_level(texture_lod(v, p, area), &lvl, &linear);
	inv_area = 1.0f / (float)area;

	for(y = miny; y <= maxy; y++){
		s64 py = (s64)y * 16 + 8;
		for(x = minx; x <= maxx; x++){
			s64 px = (s64)x * 16 + 8;
			s64 w0 = edge(p[1], p[2], px, py), w1 = edge(p[2], p[0], px, py), w2 = edge(p[0], p[1], px, py);
			float l0, l1, l2, q0, q1, q2, qs, zf;
			int col[4], sec[3], fog, k;
			if(w0 + bias[0] < 0 || w1 + bias[1] < 0 || w2 + bias[2] < 0) continue;

			l0 = (float)w0 * inv_area; l1 = (float)w1 * inv_area; l2 = (float)w2 * inv_area;
			zf = l0 * v[0]->z + l1 * v[1]->z + l2 * v[2]->z;
			/* Corrección de perspectiva: se interpola atributo/w y 1/w */
			q0 = l0 * v[0]->w; q1 = l1 * v[1]->w; q2 = l2 * v[2]->w;
			qs = q0 + q1 + q2;
			if(qs != 0.0f){ q0 /= qs; q1 /= qs; q2 /= qs; }
			col[0] = (int)(q0 * v[0]->r + q1 * v[1]->r + q2 * v[2]->r + 0.5f);
			col[1] = (int)(q0 * v[0]->g + q1 * v[1]->g + q2 * v[2]->g + 0.5f);
			col[2] = (int)(q0 * v[0]->b + q1 * v[1]->b + q2 * v[2]->b + 0.5f);
			col[3] = (int)(q0 * v[0]->a + q1 * v[1]->a + q2 * v[2]->a + 0.5f);
			sec[0] = (int)(q0 * v[0]->sr + q1 * v[1]->sr + q2 * v[2]->sr + 0.5f);
			sec[1] = (int)(q0 * v[0]->sg + q1 * v[1]->sg + q2 * v[2]->sg + 0.5f);
			sec[2] = (int)(q0 * v[0]->sb + q1 * v[1]->sb + q2 * v[2]->sb + 0.5f);
			fog = (int)((q0 * v[0]->fog + q1 * v[1]->fog + q2 * v[2]->fog) * 255.0f);
			if(rs.tex_enabled){
				float s = q0 * v[0]->s + q1 * v[1]->s + q2 * v[2]->s;
				float t = q0 * v[0]->t + q1 * v[1]->t + q2 * v[2]->t;
				float q = q0 * v[0]->q + q1 * v[1]->q + q2 * v[2]->q;
				if(q != 0.0f && q != 1.0f){ s /= q; t /= q; }
				shade(col, s, t, lvl, linear, sec);
			} else for(k = 0; k < 3; k++) col[k] += sec[k];
			draw_pixel(x, y, clampi(ge_f2i(zf), 0, 65535), clampi(fog, 0, 255), col);
		}
	}
}

/* --- Rectángulos (sprites) ------------------------------------------------- */

void ge_raster_rectangle(const GeVertex *v0, const GeVertex *v1){
	s32 x0 = ge_f2i(floorf(v0->x * 16.0f)), y0 = ge_f2i(floorf(v0->y * 16.0f));
	s32 x1 = ge_f2i(floorf(v1->x * 16.0f)), y1 = ge_f2i(floorf(v1->y * 16.0f));
	s32 lx = x0 < x1 ? x0 : x1, rx = x0 < x1 ? x1 : x0;
	s32 ty = y0 < y1 ? y0 : y1, by = y0 < y1 ? y1 : y0;
	int cx0 = (lx + 6) >> 4, cy0 = (ty + 7) >> 4, cx1 = (rx + 7) >> 4, cy1 = (by + 7) >> 4;
	int right = x0 < x1, down = y0 < y1;
	int x, y, k, lvl = 0, linear = 0;
	float dx = (float)(rx - lx), dy = (float)(by - ty);
	float s0 = v0->s, t0 = v0->t, s1 = v1->s, t1 = v1->t;
	int z = clampi((int)v1->z, 0, 65535), fog = clampi((int)(v1->fog * 255.0f), 0, 255);

	if(cx0 < rs.sx1) cx0 = rs.sx1;
	if(cy0 < rs.sy1) cy0 = rs.sy1;
	if(cx1 > rs.sx2 + 1) cx1 = rs.sx2 + 1;
	if(cy1 > rs.sy2 + 1) cy1 = rs.sy2 + 1;
	if(cx0 >= cx1 || cy0 >= cy1) return;

	if(!rs.through && v0->q != 0.0f && v1->q != 0.0f){ s0 /= v0->q; t0 /= v0->q; s1 /= v1->q; t1 /= v1->q; }
	if(rs.tex_enabled){
		float ta = fabsf((s1 - s0) * (float)rs.tex.width * (t1 - t0) * (float)rs.tex.height);
		float pa = (dx / 16.0f) * (dy / 16.0f);
		choose_level(pa > 0 && ta > 0 ? 0.5f * log2f(ta / pa) : 0.0f, &lvl, &linear);
	}

	for(y = cy0; y < cy1; y++){
		float fy = dy > 0 ? ((float)(y * 16 + 8) - (float)ty) / dy : 0.0f;
		for(x = cx0; x < cx1; x++){
			float fx = dx > 0 ? ((float)(x * 16 + 8) - (float)lx) / dx : 0.0f;
			int col[4] = { (int)v1->r, (int)v1->g, (int)v1->b, (int)v1->a };
			int sec[3] = { (int)v1->sr, (int)v1->sg, (int)v1->sb };
			if(rs.tex_enabled){
				float s, t;
				/* Las cuatro orientaciones (un sprite al revés gira la textura) */
				if(right && down){ s = s0 + fx * (s1 - s0); t = t0 + fy * (t1 - t0); }
				else if(right){ s = s1 - fy * (s1 - s0); t = t0 + fx * (t1 - t0); }
				else if(down){ s = s0 + fy * (s1 - s0); t = t1 - fx * (t1 - t0); }
				else { s = s1 - fx * (s1 - s0); t = t1 - fy * (t1 - t0); }
				shade(col, s, t, lvl, linear, sec);
			} else for(k = 0; k < 3; k++) col[k] += sec[k];
			draw_pixel(x, y, z, fog, col);
		}
	}
}

/* --- Líneas y puntos --------------------------------------------------------- */

static void plot(const GeVertex *v, int x, int y){
	int col[4] = { (int)v->r, (int)v->g, (int)v->b, (int)v->a };
	int sec[3] = { (int)v->sr, (int)v->sg, (int)v->sb }, k;
	if(x < rs.sx1 || x > rs.sx2 || y < rs.sy1 || y > rs.sy2) return;
	if(rs.tex_enabled){
		float s = v->s, t = v->t;
		if(!rs.through && v->q != 0.0f && v->q != 1.0f){ s /= v->q; t /= v->q; }
		shade(col, s, t, 0, rs.mag_linear, sec);
	} else for(k = 0; k < 3; k++) col[k] += sec[k];
	draw_pixel(x, y, clampi((int)v->z, 0, 65535), clampi((int)(v->fog * 255.0f), 0, 255), col);
}

void ge_raster_point(const GeVertex *v){
	plot(v, ge_f2i(floorf(v->x)), ge_f2i(floorf(v->y)));
}

/* Líneas con la "regla del diamante" del GE (PPSSPP, gpu/probe exp137):
   se pinta cada píxel cuyo rombo central (radio medio píxel) atraviesa la
   línea, sin incluir el del punto final. Las diagonales cuentan como
   verticales. */
static int diamond_edge_inside(s64 dx, s64 dy, int y_major){
	if(y_major){ s64 t = dx; dx = dy; dy = t; }
	return dy < 0;
}

static s64 abs64(s64 v){ return v < 0 ? -v : v; }

static int in_diamond(s64 cx, s64 cy, s64 x, s64 y, int y_major){
	s64 dx = x - cx, dy = y - cy, d = abs64(dx) + abs64(dy);
	if(d != 8) return d < 8;
	return diamond_edge_inside(dx, dy, y_major);
}

static int crosses_diamond(s64 cx, s64 cy, s64 x0, s64 y0, s64 x1, s64 y1, int y_major){
	static const int signs[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	s64 lo_n = 0, lo_d = 1, hi_n = 1, hi_d = 1, tn, td, dxn, dyn, dn;
	int i;
	for(i = 0; i < 4; i++){
		s64 a = signs[i][0] * (x1 - x0) + signs[i][1] * (y1 - y0);
		s64 b = 8 - (signs[i][0] * (x0 - cx) + signs[i][1] * (y0 - cy));
		if(a == 0){ if(b < 0) return 0; }
		else if(a > 0){ if(b * hi_d < hi_n * a){ hi_n = b; hi_d = a; } }
		else if((-b) * lo_d > lo_n * (-a)){ lo_n = -b; lo_d = -a; }
	}
	if(lo_n * hi_d > hi_n * lo_d) return 0;
	tn = lo_n * hi_d + hi_n * lo_d;
	td = 2 * lo_d * hi_d;
	dxn = (x0 - cx) * td + (x1 - x0) * tn;
	dyn = (y0 - cy) * td + (y1 - y0) * tn;
	dn = abs64(dxn) + abs64(dyn);
	if(dn < 8 * td) return 1;
	return diamond_edge_inside(dxn, dyn, y_major);
}

void ge_raster_line(const GeVertex *va, const GeVertex *vb){
	s64 x0 = ge_f2i(floorf(va->x * 16.0f)), y0 = ge_f2i(floorf(va->y * 16.0f));
	s64 x1 = ge_f2i(floorf(vb->x * 16.0f)), y1 = ge_f2i(floorf(vb->y * 16.0f));
	int x_major = abs64(x1 - x0) > abs64(y1 - y0);
	s64 a0 = x_major ? x0 : y0, a1 = x_major ? x1 : y1;
	s64 b0 = x_major ? y0 : x0, b1 = x_major ? y1 : x1;
	int dir = a1 >= a0 ? 1 : -1, bdir = b1 >= b0 ? 1 : -1;
	s64 c, first, last;

	if(x0 == x1 && y0 == y1) return;   /* longitud cero: nada */
	first = (a0 >> 4) - dir;
	last = (a1 >> 4) + dir;
	for(c = first; c != last + dir; c += dir){
		s64 ac = c * 16 + 8;
		s64 num = b0 * (a1 - a0) + (ac - a0) * (b1 - b0), den = a1 - a0, bc, r;
		int k;
		bc = num / den;
		if((num % den != 0) && ((num < 0) != (den < 0))) bc--;
		r = bc >= 0 ? bc / 16 : -((-bc + 15) / 16);
		for(k = -1; k <= 1; k++){
			s64 rr = r + k * bdir;
			s64 px = x_major ? c : rr, py = x_major ? rr : c;
			s64 cx = px * 16 + 8, cy = py * 16 + 8;
			if(in_diamond(cx, cy, x1, y1, !x_major)) continue;
			if(in_diamond(cx, cy, x0, y0, !x_major) || crosses_diamond(cx, cy, x0, y0, x1, y1, !x_major)){
				float t = (float)(ac - a0) / (float)(a1 - a0);
				GeVertex v;
				const float *pa = &va->x, *pb = &vb->x;
				float *pv = &v.x;
				int i;
				if(t < 0.0f) t = 0.0f;
				if(t > 1.0f) t = 1.0f;
				for(i = 0; i < 15; i++) pv[i] = pa[i] + (pb[i] - pa[i]) * t;
				if(!(ge.cmd[GE_SHADEMODE] & 1)){ v.r = vb->r; v.g = vb->g; v.b = vb->b; v.a = vb->a; }
				plot(&v, (int)px, (int)py);
			}
		}
	}
}
