/**
 * WIISP - ge_vertex.c
 * GE: decodificación de vértices, transformación, luces, coordenadas de
 * textura, recorte y ensamblado de primitivas.
 *
 * Es un port a C de la parte de geometría del renderizador por software de
 * PPSSPP (GPU/Software/TransformUnit.cpp, Clipper.cpp, Lighting.cpp y el
 * decodificador de GPU/Common/VertexDecoderCommon.cpp; (c) PPSSPP Project,
 * GPLv2+), con la aritmética del GE que midieron sus pruebas "gpu/probe":
 * matrices sumadas como filas de punto fijo, float24, el recíproco del GE,
 * morph y skinning en el orden del hardware, luces con factores de 8 bits.
 *
 * VERTEXTYPE (bits):  0-1 uv, 2-4 color, 5-6 normal, 7-8 posición,
 * 9-10 pesos, 11-12 índices, 14-16 nº de pesos - 1, 18-20 nº de morph - 1,
 * 23 modo through.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "core/prof.h"

/* --- Formato de vértice ---------------------------------------------------- */

typedef struct {
	u32 vtype;
	int tc, col, nrm, pos, wt, idx, nweights, nmorph, through;
	u32 off_w, off_tc, off_col, off_nrm, off_pos;
	u32 onesize;   /* un objetivo de morph */
	u32 size;      /* el vértice completo */
	u32 biggest;   /* alineación */
} VFormat;

static const u8 tc_size[4] = { 0, 2, 4, 8 }, tc_align[4] = { 0, 1, 2, 4 };
static const u8 col_size[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
static const u8 nrm_size[4] = { 0, 3, 6, 12 }, nrm_align[4] = { 0, 1, 2, 4 };
static const u8 pos_size[4] = { 3, 3, 6, 12 }, pos_align[4] = { 1, 1, 2, 4 };
static const u8 wt_size[4] = { 0, 1, 2, 4 };
static const u8 idx_size[4] = { 0, 1, 2, 4 };

/* Como el align() de PPSSPP: con alineación 0 (color no válido) da 0 */
static inline u32 align_to(u32 n, u32 a){ return (n + (a - 1)) & ~(a - 1); }

static void vformat_setup(VFormat *f, u32 vt){
	u32 size = 0, biggest = 0;
	memset(f, 0, sizeof(*f));
	f->vtype = vt;
	f->tc = vt & 3;
	f->col = (vt >> 2) & 7;
	f->nrm = (vt >> 5) & 3;
	f->pos = (vt >> 7) & 3;
	f->wt = (vt >> 9) & 3;
	f->idx = (vt >> 11) & 3;
	f->nweights = ((vt >> 14) & 7) + 1;
	f->nmorph = ((vt >> 18) & 7) + 1;
	f->through = (vt >> 23) & 1;

	if(f->wt){
		f->off_w = size;
		size += wt_size[f->wt] * (u32)f->nweights;
		if(wt_size[f->wt] > biggest) biggest = wt_size[f->wt];
	}
	if(f->tc){
		size = align_to(size, tc_align[f->tc]);
		f->off_tc = size;
		size += tc_size[f->tc];
		if(tc_align[f->tc] > biggest) biggest = tc_align[f->tc];
	}
	if(f->col){
		size = align_to(size, col_size[f->col]);
		f->off_col = size;
		size += col_size[f->col];
		if(col_size[f->col] > biggest) biggest = col_size[f->col];
	}
	if(f->nrm){
		size = align_to(size, nrm_align[f->nrm]);
		f->off_nrm = size;
		size += nrm_size[f->nrm];
		if(nrm_align[f->nrm] > biggest) biggest = nrm_align[f->nrm];
	}
	/* Siempre hay sitio para la posición, aunque no haya formato */
	size = align_to(size, pos_align[f->pos]);
	f->off_pos = size;
	size += pos_size[f->pos];
	if(pos_align[f->pos] > biggest) biggest = pos_align[f->pos];

	f->biggest = biggest;
	f->onesize = align_to(size, biggest);
	f->size = f->onesize * (u32)f->nmorph;
}

/* --- Decodificación (VertexDecoder + ApplyGEMorph/ApplyGESkinning) -------- */

typedef struct {
	float pos[3], nrm[3], uv[2];
	u32 color0;
} DecVertex;

static inline float f32_from(u32 v){ float f; memcpy(&f, &v, 4); return f; }

/* Componente con signo: s8/128, s16/32768 o float */
static inline __attribute__((always_inline)) float raw_comp(const u8 *p, int fmt, int i){
	switch(fmt){
	case 1: return (float)(s8)p[i] * (1.0f / 128.0f);
	case 2: return (float)(s16)rd_le16(p + 2 * i) * (1.0f / 32768.0f);
	case 3: return f32_from(rd_le32(p + 4 * i));
	default: return 0.0f;
	}
}

/* Peso o uv: sin signo */
static inline __attribute__((always_inline)) float raw_weight(const u8 *p, int fmt, int i){
	switch(fmt){
	case 1: return (float)p[i] * (1.0f / 128.0f);
	case 2: return (float)rd_le16(p + 2 * i) * (1.0f / 32768.0f);
	case 3: return f32_from(rd_le32(p + 4 * i));
	default: return 0.0f;
	}
}

static inline u32 c5to8(u32 v){ return (v << 3) | (v >> 2); }
static inline u32 c6to8(u32 v){ return (v << 2) | (v >> 4); }

static inline __attribute__((always_inline)) u32 color_to_8888(int col, const u8 *p){
	u32 c;
	switch(col){
	case 4:
		c = rd_le16(p);
		return c5to8(c & 0x1F) | (c6to8((c >> 5) & 0x3F) << 8) | (c5to8((c >> 11) & 0x1F) << 16) | 0xFF000000u;
	case 5:
		c = rd_le16(p);
		return c5to8(c & 0x1F) | (c5to8((c >> 5) & 0x1F) << 8) | (c5to8((c >> 10) & 0x1F) << 16) |
		       ((c & 0x8000) ? 0xFF000000u : 0);
	case 6:
		c = rd_le16(p);
		return ((c & 0xF) * 0x11) | ((((c >> 4) & 0xF) * 0x11) << 8) | ((((c >> 8) & 0xF) * 0x11) << 16) |
		       ((((c >> 12) & 0xF) * 0x11) << 24);
	case 7:
		return rd_le32(p);
	default:
		return 0;
	}
}

enum { RD_COMP, RD_WEIGHT, RD_COLOR };

static float morph_weights[8];

static float read_kind(const u8 *p, int kind, int fmt, int c){
	switch(kind){
	case RD_COMP:   return raw_comp(p, fmt, c);
	case RD_WEIGHT: return raw_weight(p, fmt, c);
	default:        return (float)((color_to_8888(fmt, p) >> (8 * c)) & 0xFF);
	}
}

/* Un componente con morph como lo hace el GE (gpu/probe exp67, exp107):
   el valor de cada objetivo por su peso en float24, sumados en orden con
   el sumador del GE */
static float ge_morph_component(const VFormat *f, const u8 *in, u32 off, int c, int kind, int fmt){
	float acc = 0.0f;
	int k;
	for(k = 0; k < f->nmorph; k++){
		float v = ge_trunc24(read_kind(in + (u32)k * f->onesize + off, kind, fmt, c));
		float w = ge_trunc24(morph_weights[k]);
		float term = v == 0.0f || w == 0.0f ? 0.0f : ge_product24((double)v * w);
		if(term != 0.0f) acc = acc == 0.0f ? term : ge_trunc24(ge_add(acc, term));
	}
	return acc;
}

static float component(const VFormat *f, const u8 *in, u32 off, int c, int kind, int fmt){
	if(f->nmorph > 1) return ge_morph_component(f, in, off, c, kind, fmt);
	return read_kind(in + off, kind, fmt, c);
}

static inline float row_value(GeRowTerm t){
	if(!t.mantissa) return 0.0f;
	if(t.lsb_exp >= -126 && t.lsb_exp <= 127){
		u32 bits = (u32)(t.lsb_exp + 127) << 23;
		float p;
		memcpy(&p, &bits, 4);
		return (float)t.mantissa * p;
	}
	return ldexpf((float)t.mantissa, t.lsb_exp);
}

static void skin_add(float *acc, float term){
	if(term != 0.0f) *acc = *acc == 0.0f ? ge_trunc24(term) : ge_trunc24(ge_add(*acc, term));
}

/* Skinning como el GE (gpu/probe exp68, exp70, exp71): cada matriz de hueso
   escalada por su peso, cada entrada un producto float24, y un acumulador
   que recorre los huesos en orden sumando su traslación y luego x, y, z por
   su columna. Las normales igual, sin traslación. El morph va antes. */
static void ge_skin(const VFormat *f, const u8 *in, float *pos, float *nrm){
	float bones[8][12], src[3];
	int b, k, c, j;
	for(b = 0; b < f->nweights; b++){
		float w = ge_trunc24(component(f, in, f->off_w, b, RD_WEIGHT, f->wt));
		for(k = 0; k < 12; k++){
			float m = ge.bone[b][k];
			bones[b][k] = m == 0.0f || w == 0.0f ? 0.0f : ge_product24((double)ge_trunc24(m) * w);
		}
	}
	if(f->pos){
		for(c = 0; c < 3; c++) src[c] = component(f, in, f->off_pos, c, RD_COMP, f->pos);
		for(c = 0; c < 3; c++){
			float acc = 0.0f;
			for(b = 0; b < f->nweights; b++){
				skin_add(&acc, bones[b][9 + c]);
				for(j = 0; j < 3; j++) skin_add(&acc, row_value(ge_product(ge_trunc24(src[j]), bones[b][j * 3 + c])));
			}
			pos[c] = acc;
		}
	}
	if(f->nrm){
		for(c = 0; c < 3; c++) src[c] = component(f, in, f->off_nrm, c, RD_COMP, f->nrm);
		for(c = 0; c < 3; c++){
			float acc = 0.0f;
			for(b = 0; b < f->nweights; b++)
				for(j = 0; j < 3; j++) skin_add(&acc, row_value(ge_product(ge_trunc24(src[j]), bones[b][j * 3 + c])));
			nrm[c] = acc;
		}
	}
}

/* NaN e infinitos de una posición float quedan finitos (como SSE en x86) */
static inline float clean_nan_inf(float x){
	if(isnan(x)) return FLT_MAX;
	if(x > FLT_MAX) return FLT_MAX;
	if(x < -FLT_MAX) return -FLT_MAX;
	return x;
}

static inline u8 sat_u8_trunc(float v){
	if(!(v > 0.0f)) return 0;      /* también NaN */
	if(v >= 255.0f) return 255;
	return (u8)(int)v;
}

/* Prescalado de uv del decodificador (solo con UV gen 3 en modo transformado) */
static int uv_prescale;
static float uv_ps_scale[2], uv_ps_off[2];

static void decode_vertex(const VFormat *f, const u8 *p, DecVertex *d){
	int i;
	memset(d, 0, sizeof(*d));
	if(!p) return;

	/* uv */
	if(f->tc){
		if(f->through){
			const u8 *t = p + f->off_tc;
			if(f->tc == 1){ d->uv[0] = (float)t[0] * (1.0f / 128.0f); d->uv[1] = (float)t[1] * (1.0f / 128.0f); }
			else if(f->tc == 2){ d->uv[0] = (float)rd_le16(t); d->uv[1] = (float)rd_le16(t + 2); }
			else { d->uv[0] = f32_from(rd_le32(t)); d->uv[1] = f32_from(rd_le32(t + 4)); }
		} else if(f->nmorph > 1){
			for(i = 0; i < 2; i++) d->uv[i] = ge_morph_component(f, p, f->off_tc, i, RD_WEIGHT, f->tc);
		} else {
			for(i = 0; i < 2; i++) d->uv[i] = raw_weight(p + f->off_tc, f->tc, i);
		}
	}

	/* color */
	if(f->col >= 4){
		if(f->nmorph == 1){
			d->color0 = color_to_8888(f->col, p + f->off_col);
		} else if(!f->through){
			for(i = 0; i < 4; i++){
				float ch = floorf(ge_morph_component(f, p, f->off_col, i, RD_COLOR, f->col));
				int v = isnan(ch) ? 0 : ch < 0.0f ? 0 : ch > 255.0f ? 255 : (int)ch;
				d->color0 |= (u32)v << (8 * i);
			}
		} else {
			/* Modo through con morph: la mezcla en float del decodificador */
			float col[4] = { 0, 0, 0, 0 };
			int k;
			for(k = 0; k < f->nmorph; k++){
				float w = morph_weights[k];
				const u8 *cp = p + (u32)k * f->onesize + f->off_col;
				u32 c = f->col == 7 ? rd_le32(cp) : rd_le16(cp);
				switch(f->col){
				case 4:
					col[0] += w * (float)(c & 0x1F) * (255.0f / 31.0f);
					col[1] += w * (float)((c >> 5) & 0x3F) * (255.0f / 63.0f);
					col[2] += w * (float)((c >> 11) & 0x1F) * (255.0f / 31.0f);
					break;
				case 5:
					col[0] += w * (float)(c & 0x1F) * (255.0f / 31.0f);
					col[1] += w * (float)((c >> 5) & 0x1F) * (255.0f / 31.0f);
					col[2] += w * (float)((c >> 10) & 0x1F) * (255.0f / 31.0f);
					col[3] += w * ((c >> 15) ? 255.0f : 0.0f);
					break;
				case 6:
					for(i = 0; i < 4; i++) col[i] += w * (float)((c >> (i * 4)) & 0xF) * (255.0f / 15.0f);
					break;
				default:
					for(i = 0; i < 4; i++) col[i] += (float)((c >> (i * 8)) & 0xFF) * w;
					break;
				}
			}
			if(f->col == 4) col[3] = 255.0f;
			for(i = 0; i < 4; i++) d->color0 |= (u32)sat_u8_trunc(col[i]) << (8 * i);
		}
	}

	if(f->through){
		/* Posiciones de 8 bits: siempre 0; de 16: x, y con signo y z sin
		   signo; float: z truncada a entero en 0..65535 */
		if(f->pos == 2){
			d->pos[0] = (float)(s16)rd_le16(p + f->off_pos);
			d->pos[1] = (float)(s16)rd_le16(p + f->off_pos + 2);
			d->pos[2] = (float)rd_le16(p + f->off_pos + 4);
		} else if(f->pos == 3){
			float z = f32_from(rd_le32(p + f->off_pos + 8));
			d->pos[0] = f32_from(rd_le32(p + f->off_pos));
			d->pos[1] = f32_from(rd_le32(p + f->off_pos + 4));
			d->pos[2] = z >= 65535.0f ? 65535.0f : (z > 0.0f ? (float)(int)z : 0.0f);
		}
		if(f->nrm) for(i = 0; i < 3; i++) d->nrm[i] = raw_comp(p + f->off_nrm, f->nrm, i);
		return;
	}

	if(f->wt){
		ge_skin(f, p, d->pos, d->nrm);
		return;
	}
	if(f->nmorph > 1){
		for(i = 0; i < 3; i++){
			if(f->pos) d->pos[i] = ge_morph_component(f, p, f->off_pos, i, RD_COMP, f->pos);
			if(f->nrm) d->nrm[i] = ge_morph_component(f, p, f->off_nrm, i, RD_COMP, f->nrm);
		}
		return;
	}
	for(i = 0; i < 3; i++){
		if(f->pos){
			d->pos[i] = raw_comp(p + f->off_pos, f->pos, i);
			if(f->pos == 3) d->pos[i] = clean_nan_inf(d->pos[i]);
		}
		if(f->nrm) d->nrm[i] = raw_comp(p + f->off_nrm, f->nrm, i);
	}
}

/* --- Luces (Lighting.cpp) --------------------------------------------------- */

typedef struct {
	float pos[3], att[3], spot_dir[3];
	float spot_dir_rsqrt, spot_dir_rsqrt_f, spot_cutoff, spot_exp;
	int ambient_cf[4], diffuse_cf[4], specular_cf[4];
	float ambient_f[4], diffuse_f[4], specular_f[4];   /* los mismos / 1024 (ge_fast_math) */
	int enabled, spot, directional, powered_diffuse, ambient, diffuse, specular;
} LightSt;

typedef struct {
	LightSt lights[4];
	int mat_ambient_cf[4], mat_diffuse_cf[4], mat_specular_cf[4];
	int base_ambient_cf[4];
	float mat_ambient_f[4], mat_diffuse_f[4], mat_specular_f[4], base_ambient_f[4], emissive_f[4];
	float specular_exp;
	float view_dir[3];
	int color_for_ambient, color_for_diffuse, color_for_specular;
	int set_color1, add_color1, uses_world_pos, uses_world_normal;
} LightState;

/* 2c + 1 por canal: las luces redondean con medio paso, como la mezcla */
static void light_color_factor(u32 c, int *out){
	int i;
	for(i = 0; i < 4; i++) out[i] = (int)((c >> (8 * i)) & 0xFF) * 2 + 1;
}

/* Algún canal de color (no alfa) por encima de 0 */
static inline int larger_than_half(const int *v){ return v[0] > 1 || v[1] > 1 || v[2] > 1; }

static inline void light_vec(int base, int light, float *out){
	int i;
	for(i = 0; i < 3; i++) out[i] = ge_f24(ge.cmd[base + light * 3 + i]);
}

/* Exponente de la potencia de luces (especular y focos): los 4 bits altos
   de la mantisa, truncado, y saturado por debajo de 512 (gpu/probe exp221) */
static float light_exponent(float e){
	u32 bits;
	if(isnan(e)) return signbit(e) ? 0.0f : 496.0f;
	if(e >= 512.0f) return 496.0f;
	memcpy(&bits, &e, 4);
	bits &= 0xFFF80000u;
	memcpy(&e, &bits, 4);
	return e;
}

static inline int light_type(int l){ return (int)((ge.cmd[GE_LIGHTTYPE0 + l] >> 8) & 3); }
static inline int light_comp(int l){ return (int)(ge.cmd[GE_LIGHTTYPE0 + l] & 3); }
static inline u32 material_ambient_rgba(void){
	return (ge.cmd[GE_MATERIALAMBIENT] & 0xFFFFFF) | ((ge.cmd[GE_MATERIALALPHA] & 0xFF) << 24);
}

static void lighting_compute_state(LightState *s, int has_color0){
	int any_ambient = 0, any_diffuse = 0, any_specular = 0, any_nondir = 0, light, i, upd;
	for(light = 0; light < 4; light++){
		LightSt *l = &s->lights[light];
		memset(l, 0, sizeof(*l));
		l->enabled = ge.cmd[GE_LIGHTENABLE0 + light] & 1;
		if(!l->enabled) continue;

		l->powered_diffuse = light_comp(light) == 2;
		l->specular = light_comp(light) == 1;

		light_color_factor(ge.cmd[GE_LAC0 + light * 3] & 0xFFFFFF, l->ambient_cf);
		l->ambient = larger_than_half(l->ambient_cf);
		any_ambient |= l->ambient;

		light_color_factor(ge.cmd[GE_LAC0 + light * 3 + 1] & 0xFFFFFF, l->diffuse_cf);
		l->diffuse = larger_than_half(l->diffuse_cf);
		any_diffuse |= l->diffuse;

		if(l->specular){
			light_color_factor(ge.cmd[GE_LAC0 + light * 3 + 2] & 0xFFFFFF, l->specular_cf);
			l->specular = larger_than_half(l->specular_cf);
			any_specular |= l->specular;
		}

		if(!l->specular && !l->ambient && !l->diffuse){ l->enabled = 0; continue; }

		light_vec(GE_LX0, light, l->pos);
		l->directional = light_type(light) == 0;
		if(l->directional){
			/* Una dirección nula se queda nula (gpu/probe exp164) */
			ge_normalize(l->pos);
		} else {
			light_vec(GE_LKA0, light, l->att);
			any_nondir = 1;
		}

		l->spot = light_type(light) >= 2;
		if(l->spot){
			float len2;
			/* La dirección no se normaliza: su producto con L se escala por
			   el rsqrt (gpu/probe exp100). Un componente inf o NaN actúa como
			   el mayor valor de su signo (gpu/commands/light). */
			light_vec(GE_LDX0, light, l->spot_dir);
			if(!isfinite(l->spot_dir[0]) || !isfinite(l->spot_dir[1]) || !isfinite(l->spot_dir[2]))
				for(i = 0; i < 3; i++)
					l->spot_dir[i] = isfinite(l->spot_dir[i]) ? 0.0f : (signbit(l->spot_dir[i]) ? -1.0f : 1.0f);
			len2 = ge_dot(l->spot_dir, l->spot_dir);
			l->spot_dir_rsqrt = len2 > 0.0f && isfinite(len2) ? ge_rsqrt(len2) : 0.0f;
			l->spot_dir_rsqrt_f = len2 > 0.0f && isfinite(len2) ? 1.0f / sqrtf(len2) : 0.0f;
			l->spot_cutoff = ge_f24(ge.cmd[GE_LKO0 + light]);
			if(isnan(l->spot_cutoff) && signbit(l->spot_cutoff)) l->spot_cutoff = 0.0f;
			l->spot_exp = light_exponent(ge_f24(ge.cmd[GE_LKS0 + light]));
			if(l->spot_exp <= 0.0f) l->spot_exp = 0.0f;
		}
	}

	upd = (int)(ge.cmd[GE_MATERIALUPDATE] & (has_color0 ? 7 : 0));
	s->color_for_ambient = (upd & 1) != 0;
	s->color_for_diffuse = (upd & 2) != 0;
	s->color_for_specular = (upd & 4) != 0;

	if(!s->color_for_ambient){
		light_color_factor(material_ambient_rgba(), s->mat_ambient_cf);
		if(!larger_than_half(s->mat_ambient_cf) && any_ambient)
			for(i = 0; i < 4; i++) s->lights[i].ambient = 0;
	}
	if(any_diffuse && !s->color_for_diffuse){
		light_color_factor(ge.cmd[GE_MATERIALDIFFUSE] & 0xFFFFFF, s->mat_diffuse_cf);
		if(!larger_than_half(s->mat_diffuse_cf)){
			any_diffuse = 0;
			for(i = 0; i < 4; i++) s->lights[i].diffuse = 0;
		}
	}
	if(any_specular && !s->color_for_specular){
		light_color_factor(ge.cmd[GE_MATERIALSPECULAR] & 0xFFFFFF, s->mat_specular_cf);
		if(!larger_than_half(s->mat_specular_cf)){
			any_specular = 0;
			for(i = 0; i < 4; i++) s->lights[i].specular = 0;
		}
	}
	if(any_diffuse || any_specular){
		s->specular_exp = light_exponent(ge_f24(ge.cmd[GE_MATERIALSPECULARCOEF]));
		if(s->specular_exp <= 0.0f) s->specular_exp = 0.0f;
	}

	light_color_factor((ge.cmd[GE_AMBIENTCOLOR] & 0xFFFFFF) | ((ge.cmd[GE_AMBIENTALPHA] & 0xFF) << 24), s->base_ambient_cf);
	for(i = 0; i < 4; i++){
		for(light = 0; light < 4; light++){
			LightSt *l = &s->lights[light];
			l->ambient_f[i] = (float)l->ambient_cf[i] * (1.0f / 1024.0f);
			l->diffuse_f[i] = (float)l->diffuse_cf[i] * (1.0f / 1024.0f);
			l->specular_f[i] = (float)l->specular_cf[i] * (1.0f / 1024.0f);
		}
		s->mat_ambient_f[i] = (float)s->mat_ambient_cf[i];
		s->mat_diffuse_f[i] = (float)s->mat_diffuse_cf[i];
		s->mat_specular_f[i] = (float)s->mat_specular_cf[i];
		s->base_ambient_f[i] = (float)s->base_ambient_cf[i] * (1.0f / 1024.0f);
		s->emissive_f[i] = (float)(((ge.cmd[GE_MATERIALEMISSIVE] & 0xFFFFFF) >> (8 * i)) & 0xFF);
	}
	s->set_color1 = (ge.cmd[GE_LIGHTMODE] & 1) && any_specular;
	s->add_color1 = !(ge.cmd[GE_LIGHTMODE] & 1) && any_specular;
	s->uses_world_pos = any_nondir;
	s->uses_world_normal = (ge.cmd[GE_TEXMAPMODE] & 3) == 2 || any_diffuse || any_specular;
}

/* Con ge_fast_math (backend por hardware) float normal y en línea */
static inline float vdot(const float *a, const float *b){
	return ge_fast_math ? a[0] * b[0] + a[1] * b[1] + a[2] * b[2] : ge_dot(a, b);
}

static inline float vnormalize(float *v){
	float d2, d, r;
	if(!ge_fast_math) return ge_normalize(v);
	d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if(!(d2 > 0.0f) || !isfinite(d2)) return 0.0f;
	r = ge_fast_rsqrt(d2);
	d = d2 * r;
	v[0] *= r; v[1] *= r; v[2] *= r;
	return d;
}

static inline float light_pow(float v, float e){
	return ge_fast_math ? ge_fast_light_pow(v, e) : ge_light_pow(v, e);
}

/* v . N para la normal sin normalizar, escalado por su rsqrt */
static inline float ge_normal_dot(const float *v, const float *n, float n_rsqrt){
	if(ge_fast_math) return vdot(v, n) * n_rsqrt;
	return ge_product24((double)ge_dot(v, n) * n_rsqrt);
}

/* Vector del vértice a una luz puntual como lo calcula el GE (gpu/probe
   exp153-158): la posición de la luz menos la traslación del mundo, menos
   la posición del modelo por la matriz de mundo, sumado como una fila */
static void ge_light_vector(const float *lpos, const float *mpos, float *L){
	const float *m = ge.world;
	float out[3];
	int i;
	for(i = 0; i < 3; i++){
		GeRowTerm t[4];
		t[0] = ge_product(1.0f, ge_add24(lpos[i], -ge_trunc24(m[9 + i])));
		t[1] = ge_product(ge_trunc24(mpos[0]), -m[i]);
		t[2] = ge_product(ge_trunc24(mpos[1]), -m[3 + i]);
		t[3] = ge_product(ge_trunc24(mpos[2]), -m[6 + i]);
		out[i] = ge_row_sum(t, 4);
	}
	memcpy(L, out, sizeof(out));
}

static float ge_shade_map_coord(int l, const float *mpos, const float *wn, float n_rsqrt, const float *view_dir){
	float L[3];
	int i;
	light_vec(GE_LX0, l, L);
	if(ge_fast_math){
		if(light_type(l) != 0){
			const float *m = ge.world;
			for(i = 0; i < 3; i++) L[i] -= mpos[0] * m[i] + mpos[1] * m[3 + i] + mpos[2] * m[6 + i] + m[9 + i];
		}
		vnormalize(L);
		if(light_comp(l) == 1){
			for(i = 0; i < 3; i++) L[i] += view_dir[i];
			vnormalize(L);
		}
		return (ge_normal_dot(L, wn, n_rsqrt) + 1.0f) * 0.5f;
	}
	if(light_type(l) != 0) ge_light_vector(L, mpos, L);
	ge_normalize(L);
	if(light_comp(l) == 1){
		for(i = 0; i < 3; i++) L[i] = ge_add24(L[i], view_dir[i]);
		ge_normalize(L);
	}
	return ge_add24(ge_normal_dot(L, wn, n_rsqrt), 1.0f) * 0.5f;
}

/* Producto de factores de luz y material: x = ((2l + 1) * (2m + 1)) >> 10 */
static inline void light_color_product(const int *l, const int *m, int *out){
	int i;
	for(i = 0; i < 4; i++) out[i] = (l[i] * m[i]) >> 10;
}

/* Escala por un factor f como un color de 8 bits s = floor(256 f)
   (gpu/probe exp61-63) */
static inline void light_color_scale(int *x, float f){
	float sf = 256.0f * f;
	int s, i;
	s = !(sf > 0.0f) ? 0 : sf >= 256.0f ? 256 : (int)sf;
	for(i = 0; i < 4; i++) x[i] = ((x[i] * 2 + 1) * (2 * s + 1)) >> 10;
}

/* Las mismas luces en float, sin pasar por enteros en cada luz: los
   factores de 8 bits del GE se aproximan con su producto en float */
static void lighting_process_fast(GeVertex *v, const float *mpos, const float *wn, float n_rsqrt, const LightState *s){
	float fc[4], spec[4] = { 0, 0, 0, 0 }, vc[4], wpos[3] = { 0, 0, 0 };
	const float *mac = s->mat_ambient_f, *mdc = s->mat_diffuse_f, *msc = s->mat_specular_f;
	int i, light, out[4];
	if(s->uses_world_pos){
		const float *m = ge.world;
		for(i = 0; i < 3; i++) wpos[i] = mpos[0] * m[i] + mpos[1] * m[3 + i] + mpos[2] * m[6 + i] + m[9 + i];
	}
	if(s->color_for_ambient || s->color_for_diffuse || s->color_for_specular){
		for(i = 0; i < 4; i++) vc[i] = (float)(int)(((v->color0 >> (8 * i)) & 0xFF) * 2 + 1);
		if(s->color_for_ambient) mac = vc;
		if(s->color_for_diffuse) mdc = vc;
		if(s->color_for_specular) msc = vc;
	}
	for(i = 0; i < 4; i++) fc[i] = s->emissive_f[i] + mac[i] * s->base_ambient_f[i];

	for(light = 0; light < 4; light++){
		const LightSt *l = &s->lights[light];
		float L[3], k = 1.0f, df = 0.0f;
		if(!l->enabled) continue;
		if(!l->directional){
			float d2, d, den;
			L[0] = l->pos[0] - wpos[0]; L[1] = l->pos[1] - wpos[1]; L[2] = l->pos[2] - wpos[2];
			d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
			d = vnormalize(L);
			if(d == 0.0f){ L[0] = 0.0f; L[1] = 0.0f; L[2] = 1.0f; }
			den = l->att[0] + l->att[1] * d + l->att[2] * d2;
			k = den > 0.0f ? 1.0f / den : 0.0f;
			if(!(k > 0.0f)) k = 0.0f;
		} else {
			L[0] = l->pos[0]; L[1] = l->pos[1]; L[2] = l->pos[2];
		}
		if(l->spot){
			float raw = (l->spot_dir[0] * L[0] + l->spot_dir[1] * L[1] + l->spot_dir[2] * L[2]) * l->spot_dir_rsqrt_f;
			if(isnan(raw)) raw = signbit(raw) ? 0.0f : 1.0f;
			if(raw < l->spot_cutoff && raw + 1e-6f >= l->spot_cutoff) raw = l->spot_cutoff;
			if(raw >= l->spot_cutoff){
				float sp = ge_fast_light_pow(raw, l->spot_exp);
				k *= sp > 0.0f ? sp : 0.0f;   /* también NaN */
			} else k = 0.0f;
		}
		if(k > 1.0f) k = 1.0f;
		if(k == 0.0f) continue;

		if(l->ambient) for(i = 0; i < 4; i++) fc[i] += l->ambient_f[i] * mac[i] * k;
		if(l->diffuse || l->specular){
			df = (L[0] * wn[0] + L[1] * wn[1] + L[2] * wn[2]) * n_rsqrt;
			if(l->powered_diffuse) df = ge_fast_light_pow(df, s->specular_exp);
		}
		if(l->diffuse && df > 0.0f){
			float kd = (df < 1.0f ? df : 1.0f) * k;
			for(i = 0; i < 4; i++) fc[i] += l->diffuse_f[i] * mdc[i] * kd;
		}
		if(l->specular && df >= 0.0f){
			float H[3], sf;
			H[0] = L[0] + s->view_dir[0]; H[1] = L[1] + s->view_dir[1]; H[2] = L[2] + s->view_dir[2];
			if(vnormalize(H) == 0.0f){ H[0] = 0.0f; H[1] = 0.0f; H[2] = 1.0f; }
			sf = ge_fast_light_pow((H[0] * wn[0] + H[1] * wn[1] + H[2] * wn[2]) * n_rsqrt, s->specular_exp);
			if(sf > 0.0f){
				float ks = (sf < 1.0f ? sf : 1.0f) * k;
				for(i = 0; i < 4; i++) spec[i] += l->specular_f[i] * msc[i] * ks;
			}
		}
	}

	if(s->add_color1) for(i = 0; i < 4; i++) fc[i] += spec[i];
	for(i = 0; i < 4; i++) out[i] = fc[i] < 255.0f ? (int)fc[i] : 255;
	v->color0 = ge_pack_rgba(out);
	if(s->set_color1){
		for(i = 0; i < 4; i++) out[i] = spec[i] < 255.0f ? (int)spec[i] : 255;
		v->color1 = ge_pack_rgba(out) & 0x00FFFFFF;
	}
}

static void lighting_process(GeVertex *v, const float *mpos, const float *wn, float n_rsqrt, const LightState *s){
	int color_factor[4] = { 0, 0, 0, 0 }, mac[4], final_color[4], spec_color[4] = { 0, 0, 0, 0 };
	int i, light;
	float wpos[3];
	if(ge_fast_math && s->uses_world_pos){
		/* Posición en el mundo una vez por vértice */
		const float *m = ge.world;
		for(i = 0; i < 3; i++) wpos[i] = mpos[0] * m[i] + mpos[1] * m[3 + i] + mpos[2] * m[6 + i] + m[9 + i];
	}
	if(s->color_for_ambient || s->color_for_diffuse || s->color_for_specular)
		light_color_factor(v->color0, color_factor);
	memcpy(mac, s->color_for_ambient ? color_factor : s->mat_ambient_cf, sizeof(mac));
	for(i = 0; i < 4; i++){
		int mec = (int)(((ge.cmd[GE_MATERIALEMISSIVE] & 0xFFFFFF) >> (8 * i)) & 0xFF);
		final_color[i] = mec + ((mac[i] * s->base_ambient_cf[i]) >> 10);
	}

	for(light = 0; light < 4; light++){
		const LightSt *l = &s->lights[light];
		float L[3], att = 1.0f, spot = 1.0f, diffuse_factor = 0.0f;
		if(!l->enabled) continue;

		memcpy(L, l->pos, sizeof(L));
		/* Atenuación y foco escalan cada uno los colores de la luz como su
		   propio factor de 8 bits (gpu/probe exp100) */
		if(!l->directional){
			float d2, d, den, k[3];
			if(ge_fast_math){ L[0] = l->pos[0] - wpos[0]; L[1] = l->pos[1] - wpos[1]; L[2] = l->pos[2] - wpos[2]; }
			else ge_light_vector(l->pos, mpos, L);
			/* El término cuadrático usa la longitud al cuadrado de la
			   normalización, no d * d (gpu/probe exp63) */
			d2 = vdot(L, L);
			d = vnormalize(L);
			if(d == 0.0f){ L[0] = 0.0f; L[1] = 0.0f; L[2] = 1.0f; }
			k[0] = 1.0f; k[1] = d; k[2] = d2;
			den = vdot(l->att, k);
			att = den > 0.0f ? (ge_fast_math ? 1.0f / den : ge_recip(den)) : 0.0f;
			if(!(att > 0.0f)) att = 0.0f;
			else if(att > 1.0f) att = 1.0f;
		}

		if(l->spot){
			float raw = ge_fast_math ? vdot(l->spot_dir, L) * l->spot_dir_rsqrt_f
			                         : ge_product24((double)ge_dot(l->spot_dir, L) * l->spot_dir_rsqrt);
			if(isnan(raw)) raw = signbit(raw) ? 0.0f : 1.0f;
			/* En float normal un foco alineado da 0,99999994 */
			if(ge_fast_math && raw < l->spot_cutoff && raw + 1e-6f >= l->spot_cutoff) raw = l->spot_cutoff;
			if(raw >= l->spot_cutoff){
				spot = light_pow(raw, l->spot_exp);
				if(isnan(spot)) spot = 0.0f;
			} else spot = 0.0f;
		}

#define SCALE_ATT_SPOT(c) do { if(att < 1.0f) light_color_scale(c, att); if(spot < 1.0f) light_color_scale(c, spot); } while(0)

		if(l->ambient){
			int la[4];
			light_color_product(l->ambient_cf, mac, la);
			SCALE_ATT_SPOT(la);
			for(i = 0; i < 4; i++) final_color[i] += la[i];
		}

		if(l->diffuse || l->specular){
			diffuse_factor = ge_normal_dot(L, wn, n_rsqrt);
			if(l->powered_diffuse) diffuse_factor = light_pow(diffuse_factor, s->specular_exp);
		}

		if(l->diffuse && diffuse_factor > 0.0f){
			int ld[4];
			light_color_product(l->diffuse_cf, s->color_for_diffuse ? color_factor : s->mat_diffuse_cf, ld);
			light_color_scale(ld, diffuse_factor);
			SCALE_ATT_SPOT(ld);
			for(i = 0; i < 4; i++) final_color[i] += ld[i];
		}

		if(l->specular && diffuse_factor >= 0.0f){
			float H[3], sf;
			if(ge_fast_math) for(i = 0; i < 3; i++) H[i] = L[i] + s->view_dir[i];
			else for(i = 0; i < 3; i++) H[i] = ge_add24(L[i], s->view_dir[i]);
			if(vnormalize(H) == 0.0f){ H[0] = 0.0f; H[1] = 0.0f; H[2] = 1.0f; }
			sf = light_pow(ge_normal_dot(H, wn, n_rsqrt), s->specular_exp);
			if(sf > 0.0f){
				int ls[4];
				light_color_product(l->specular_cf, s->color_for_specular ? color_factor : s->mat_specular_cf, ls);
				light_color_scale(ls, sf);
				SCALE_ATT_SPOT(ls);
				for(i = 0; i < 4; i++) spec_color[i] += ls[i];
			}
		}
#undef SCALE_ATT_SPOT
	}

	if(s->set_color1){
		v->color0 = ge_pack_rgba(final_color);
		v->color1 = ge_pack_rgba(spec_color) & 0x00FFFFFF;
	} else if(s->add_color1){
		for(i = 0; i < 4; i++) final_color[i] += spec_color[i];
		v->color0 = ge_pack_rgba(final_color);
	} else {
		v->color0 = ge_pack_rgba(final_color);
	}
}

/* --- Transformación (ComputeTransformState / ReadVertex) ------------------ */

typedef struct {
	int enable_transform, enable_lighting, enable_fog, negate_normals, uv_gen_mode;
	int ge_uv_scale;
	float uv_scale[2], uv_offset[2];
	float matrix[16];
	int fog_ge;
	float view_z_col[4], fog_end, fog_slope, pos_to_fog[4];
	float screen_scale[3], screen_add[3];
	int depth_clip;
	LightState light;
} TState;

static TState ts;
static u32 ts_nlights;   /* luces encendidas (ge_stats) */

static void to4x4(const float *m43, float *out){
	int r, c;
	for(r = 0; r < 4; r++){
		for(c = 0; c < 3; c++) out[r * 4 + c] = m43[r * 3 + c];
		out[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
	}
}

static void compute_transform_state(const VFormat *f){
	int texmode = (int)(ge.cmd[GE_TEXMAPMODE] & 3);
	ts.enable_transform = !f->through;
	ts.enable_lighting = ge.cmd[GE_LIGHTINGENABLE] & 1;
	ts.enable_fog = ge.cmd[GE_FOGENABLE] & 1;
	ts.ge_uv_scale = !f->through && texmode == 0;
	ts.uv_scale[0] = ge_trunc24(ge_f24(ge.cmd[GE_TEXSCALEU]));
	ts.uv_scale[1] = ge_trunc24(ge_f24(ge.cmd[GE_TEXSCALEV]));
	ts.uv_offset[0] = ge_trunc24(ge_f24(ge.cmd[GE_TEXOFFSETU]));
	ts.uv_offset[1] = ge_trunc24(ge_f24(ge.cmd[GE_TEXOFFSETV]));
	ts.negate_normals = ge.cmd[GE_REVERSENORMAL] & 1;
	ts.uv_gen_mode = texmode == 3 ? 0 : texmode;
	ts.depth_clip = ge.cmd[GE_DEPTHCLAMPENABLE] & 1;

	if(ts.enable_transform){
		float world[16], view[16], worldview[16], vd[3];
		int i;
		if(ts.enable_lighting){
			int l;
			lighting_compute_state(&ts.light, f->col != 0);
			for(ts_nlights = 0, l = 0; l < 4; l++) ts_nlights += ts.light.lights[l].enabled != 0;
		} else ts.light.uses_world_normal = ts.uv_gen_mode == 2;
		/* El observador en el infinito por +z de vista, normalizado como el GE */
		vd[0] = ge.view[2]; vd[1] = ge.view[5]; vd[2] = ge.view[8];
		if(ge_normalize(vd) == 0.0f){ vd[0] = 0.0f; vd[1] = 0.0f; vd[2] = 1.0f; }
		memcpy(ts.light.view_dir, vd, sizeof(vd));

		to4x4(ge.view, view);
		to4x4(ge.world, world);
		ge_combine_matrices(worldview, world, view);
		/* (mundo * vista) * proyección, como el GE */
		ge_combine_matrices(ts.matrix, worldview, ge.proj);

		if(ts.enable_fog){
			float fog_end = ge_f24(ge.cmd[GE_FOG1]), fog_slope = ge_f24(ge.cmd[GE_FOG2]);
			ts.pos_to_fog[0] = worldview[2];
			ts.pos_to_fog[1] = worldview[6];
			ts.pos_to_fog[2] = worldview[10];
			ts.pos_to_fog[3] = worldview[14] + fog_end;
			/* Con parámetros finitos, la aritmética del GE (gpu/probe exp20) */
			ts.fog_ge = isfinite(fog_end) && isfinite(fog_slope);
			for(i = 0; i < 4; i++) ts.view_z_col[i] = worldview[2 + 4 * i];
			ts.fog_end = ge_trunc24(fog_end);
			ts.fog_slope = ge_trunc24(fog_slope);
			if(!isfinite(fog_end)){
				/* Con inf o NaN no se mezclan inf + -inf (cielos de Outrun) */
				int sign = signbit(fog_end) != 0;
				if(signbit(fog_slope)) sign = !sign;
				if(fog_slope == 0.0f) sign = 1;
				ts.pos_to_fog[0] = ts.pos_to_fog[1] = ts.pos_to_fog[2] = 0.0f;
				ts.pos_to_fog[3] = sign ? 0.0f : 1.0f;
			} else {
				if(!isfinite(fog_slope)) fog_slope = signbit(fog_slope) ? -262144.0f : 262144.0f;
				for(i = 0; i < 4; i++) ts.pos_to_fog[i] *= fog_slope;
			}
		}

		ts.screen_scale[0] = ge_f24(ge.cmd[GE_VIEWPORTXSCALE]);
		ts.screen_scale[1] = ge_f24(ge.cmd[GE_VIEWPORTYSCALE]);
		ts.screen_scale[2] = ge_f24(ge.cmd[GE_VIEWPORTZSCALE]);
		ts.screen_add[0] = ge_f24(ge.cmd[GE_VIEWPORTXCENTER]);
		ts.screen_add[1] = ge_f24(ge.cmd[GE_VIEWPORTYCENTER]);
		ts.screen_add[2] = ge_f24(ge.cmd[GE_VIEWPORTZCENTER]);
	}
}

/* Pantalla en 1/16 de píxel sin el offset. Con el recorte de profundidad
   activo, z se satura y un vértice que el plano cercano recorta (z < -w) no
   cuenta para el rango, salvo que se pida (always) */
#define SCREEN_BOUND (4095.0f + 15.5f / 16.0f)

static int round_to_screen(float sx, float sy, float sz, const float *clip, int depth_clip, int always, GeVertex *v){
	int outside = 0;
	if(depth_clip){
		if((always || !(clip[2] < -clip[3])) && (sx >= SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0))
			outside = 1;
		if(sz < 0.0f) sz = 0.0f;
		else if(sz > 65535.0f) sz = 65535.0f;
	} else if(sx > SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0 || sz < 0.0f || sz >= 65536.0f)
		outside = 1;
	v->x = (int)((u32)ge_f2i(ge_floorf(sx * 16.0f)) - (ge.cmd[GE_OFFSETX] & 0xFFFF));
	v->y = (int)((u32)ge_f2i(ge_floorf(sy * 16.0f)) - (ge.cmd[GE_OFFSETY] & 0xFFFF));
	v->z = (u16)(u32)ge_f2i(sz);
	return outside;
}

/* ClipToScreen (recorte): siempre comprueba el rango */
static int clip_to_screen(const float *clip, GeVertex *v){
	float x = ge_viewport(clip[0], clip[3], ge_f24(ge.cmd[GE_VIEWPORTXSCALE]), ge_f24(ge.cmd[GE_VIEWPORTXCENTER]));
	float y = ge_viewport(clip[1], clip[3], ge_f24(ge.cmd[GE_VIEWPORTYSCALE]), ge_f24(ge.cmd[GE_VIEWPORTYCENTER]));
	float z = ge_floorf(ge_viewport(clip[2], clip[3], ge_f24(ge.cmd[GE_VIEWPORTZSCALE]), ge_f24(ge.cmd[GE_VIEWPORTZCENTER])));
	return round_to_screen(x, y, z, clip, ge.cmd[GE_DEPTHCLAMPENABLE] & 1, 1, v);
}

/* El factor de niebla de 8 bits: min(floor(256 f), 255), NaN e inf por signo */
static int ge_fog_factor(float f){
	u32 bits, e, m;
	memcpy(&bits, &f, 4);
	e = bits >> 23;
	if((bits & 0x80000000u) || e <= 126 - 8) return 0;
	if(e > 126) return 255;
	m = (bits & 0x007FFFFF) | 0x00800000;
	return (int)(m >> (16 + 126 - e));
}

/* Componente de la matriz de textura 4x3, sumada como una fila (gpu/probe exp64) */
static float texgen_component(const float *v, const float *m, int c){
	GeRowTerm t[4];
	t[0] = ge_product(ge_trunc24(v[0]), m[c]);
	t[1] = ge_product(ge_trunc24(v[1]), m[3 + c]);
	t[2] = ge_product(ge_trunc24(v[2]), m[6 + c]);
	t[3] = ge_product(1.0f, m[9 + c]);
	return ge_row_sum(t, 4);
}

/* El GE conserva la última uv y la última normal: un vértice sin ellas usa
   las anteriores, incluso de otra llamada (gpu/vertices/carry). La uv se
   guarda aunque la textura esté desactivada y sin escalar: la escala y el
   desplazamiento actuales se aplican al usarla. */
static float last_uv[2], last_normal[3];

static void read_vertex(const VFormat *f, const DecVertex *d, GeClipVertex *out){
	GeVertex *v = &out->v;
	float pos[3], normal[3];
	int i;

	memset(out, 0, sizeof(*out));
	memcpy(pos, d->pos, sizeof(pos));

	if(f->tc){ last_uv[0] = d->uv[0]; last_uv[1] = d->uv[1]; }
	{
		float tc[2];
		tc[0] = last_uv[0]; tc[1] = last_uv[1];
		if(ts.ge_uv_scale && ge_fast_math){
			for(i = 0; i < 2; i++) tc[i] = tc[i] * ts.uv_scale[i] + ts.uv_offset[i];
		} else if(ts.ge_uv_scale){
			/* El decodificador solo las normalizó (8 y 16 bits sin signo) */
			for(i = 0; i < 2; i++){
				float scaled = ge_product24((double)ge_trunc24(tc[i]) * ts.uv_scale[i]);
				tc[i] = ge_trunc24(ge_add(scaled, ts.uv_offset[i]));
			}
		} else if(uv_prescale){
			for(i = 0; i < 2; i++) tc[i] = tc[i] * uv_ps_scale[i] + uv_ps_off[i];
		}
		v->s = tc[0]; v->t = tc[1]; v->q = 0.0f;
	}

	if(f->nrm) memcpy(last_normal, d->nrm, sizeof(last_normal));
	memcpy(normal, last_normal, sizeof(normal));
	if(ts.negate_normals) for(i = 0; i < 3; i++) normal[i] = -normal[i];

	v->color0 = f->col ? d->color0 : material_ambient_rgba();
	v->color1 = 0;

	if(ts.enable_transform){
		float sx, sy, sz, worldnormal[3] = { 0, 0, 0 }, n_rsqrt = 1.0f;
		if(ge_fast_math){
			/* Float normal: la profundidad y los bordes pueden variar en 1
			   respecto al GE, pero igual en todas las pasadas */
			const float *m = ts.matrix;
			float iw;
			for(i = 0; i < 4; i++) out->clip[i] = pos[0] * m[i] + pos[1] * m[4 + i] + pos[2] * m[8 + i] + m[12 + i];
			iw = 1.0f / out->clip[3];
			sx = out->clip[0] * iw * ts.screen_scale[0] + ts.screen_add[0];
			sy = out->clip[1] * iw * ts.screen_scale[1] + ts.screen_add[1];
			sz = ge_floorf(out->clip[2] * iw * ts.screen_scale[2] + ts.screen_add[2]);
		} else {
			for(i = 0; i < 4; i++) out->clip[i] = ge_clip_component(pos, ts.matrix, i);
			sx = ge_viewport(out->clip[0], out->clip[3], ts.screen_scale[0], ts.screen_add[0]);
			sy = ge_viewport(out->clip[1], out->clip[3], ts.screen_scale[1], ts.screen_add[1]);
			sz = ge_floorf(ge_viewport(out->clip[2], out->clip[3], ts.screen_scale[2], ts.screen_add[2]));
		}
		v->clipw = out->clip[3];
		v->fogdepth = 1.0f;
		if(round_to_screen(sx, sy, sz, out->clip, ts.depth_clip, 0, v)){
			v->x = GE_OUTSIDE;
			return;
		}

		if(ts.enable_fog && ts.fog_ge && !ge_fast_math){
			GeRowTerm t[4];
			float vz, fz;
			t[0] = ge_product(ge_trunc24(pos[0]), ts.view_z_col[0]);
			t[1] = ge_product(ge_trunc24(pos[1]), ts.view_z_col[1]);
			t[2] = ge_product(ge_trunc24(pos[2]), ts.view_z_col[2]);
			t[3] = ge_product(1.0f, ts.view_z_col[3]);
			vz = ge_row_sum(t, 4);
			fz = ge_product24((double)ge_trunc24(ge_add(vz, ts.fog_end)) * ts.fog_slope);
			v->fogdepth = (float)ge_fog_factor(fz) * (1.0f / 256.0f);
		} else if(ts.enable_fog){
			float f4 = ts.pos_to_fog[0] * pos[0] + ts.pos_to_fog[1] * pos[1] + ts.pos_to_fog[2] * pos[2] + ts.pos_to_fog[3];
			v->fogdepth = (float)ge_fog_factor(f4) * (1.0f / 256.0f);
		}

		/* La normal se queda como la deja la matriz de mundo: las luces
		   escalan sus productos por el inverso de su longitud (gpu/probe exp3) */
		if(ts.light.uses_world_normal){
			const float *m = ge.world;
			float c0[3], c1[3], c2[3], len2;
			c0[0] = m[0]; c0[1] = m[3]; c0[2] = m[6];
			c1[0] = m[1]; c1[1] = m[4]; c1[2] = m[7];
			c2[0] = m[2]; c2[1] = m[5]; c2[2] = m[8];
			worldnormal[0] = vdot(normal, c0);
			worldnormal[1] = vdot(normal, c1);
			worldnormal[2] = vdot(normal, c2);
			len2 = vdot(worldnormal, worldnormal);
			if(len2 > 0.0f && isfinite(len2)) n_rsqrt = ge_fast_math ? ge_fast_rsqrt(len2) : ge_rsqrt(len2);
			else if(len2 != 0.0f){ worldnormal[0] = 0.0f; worldnormal[1] = 0.0f; worldnormal[2] = 1.0f; }
			/* Una normal nula sigue nula: ni difusa ni especular (gpu/probe exp173) */
		}

		if(ts.uv_gen_mode == 1){
			float src[3], r[3];
			switch((ge.cmd[GE_TEXMAPMODE] >> 8) & 3){
			case 0: memcpy(src, pos, sizeof(src)); break;
			case 1: src[0] = v->s; src[1] = v->t; src[2] = 0.0f; break;
			case 2: memcpy(src, normal, sizeof(src)); vnormalize(src); break;  /* sin (0, 0, 1) si es nula */
			default: memcpy(src, normal, sizeof(src)); break;
			}
			/* Aquí no se usan la escala ni el desplazamiento de uv */
			if(ge_fast_math){
				const float *m = ge.tgen;
				for(i = 0; i < 3; i++) r[i] = src[0] * m[i] + src[1] * m[3 + i] + src[2] * m[6 + i] + m[9 + i];
			} else for(i = 0; i < 3; i++) r[i] = texgen_component(src, ge.tgen, i);
			v->s = r[0]; v->t = r[1]; v->q = r[2];
		} else if(ts.uv_gen_mode == 2){
			v->s = ge_shade_map_coord((int)(ge.cmd[GE_TEXSHADELS] & 3), pos, worldnormal, n_rsqrt, ts.light.view_dir);
			v->t = ge_shade_map_coord((int)((ge.cmd[GE_TEXSHADELS] >> 8) & 3), pos, worldnormal, n_rsqrt, ts.light.view_dir);
		}

		if(ts.enable_lighting){
			GE_PROF_T0;
			ge_stats.vertices_lit++;
			ge_stats.lights += ts_nlights;
			if(ge_fast_math) lighting_process_fast(v, pos, worldnormal, n_rsqrt, &ts.light);
			else lighting_process(v, pos, worldnormal, n_rsqrt, &ts.light);
			GE_PROF_ADD(4);
		}
	} else {
		v->x = ge_f2i(pos[0] * 16.0f);
		v->y = ge_f2i(pos[1] * 16.0f);
		v->z = (u16)(u32)ge_f2i(pos[2]);
		v->clipw = 1.0f;
		v->fogdepth = 1.0f;
	}
}

/* --- Recorte (Clipper.cpp) -------------------------------------------------- */

static int cull_xy = 1;

static inline int outside_range(const GeClipVertex *v){ return v->v.x == GE_OUTSIDE; }
static inline int calc_clip_mask(const float *c){ return c[2] < -c[3] ? -1 : 0; }

/* Con recorte de profundidad, un vértice más allá del plano cercano se
   recorta antes del viewport: su rango no importa */
static inline int outside_range_before_clip(const GeClipVertex *v, int depth_clip){
	return outside_range(v) && !(depth_clip && calc_clip_mask(v->clip) != 0);
}

/* El GE compara las coordenadas de recorte directamente: fuera de un plano
   si |c| > w. Se descarta la primitiva con todos sus vértices fuera del
   mismo plano (gpu/probe exp176) */
static int outside_mask(const float *p){
	int m = 0;
	if(cull_xy){
		if(p[0] > p[3]) m |= 1;
		if(-p[0] > p[3]) m |= 2;
		if(p[1] > p[3]) m |= 4;
		if(-p[1] > p[3]) m |= 8;
	}
	if(p[2] > p[3]) m |= 16;
	if(-p[2] > p[3]) m |= 32;
	return m;
}

/* Interpolación del recortador: posición y uv en float; color y niebla con
   t redondeada a 1/256 (gpu/probe exp136) */
static void clip_lerp(GeClipVertex *d, float t, const GeClipVertex *a, const GeClipVertex *b){
	int ti = (int)(t * 256.0f + 0.5f), i, c0[4], c1[4];
	for(i = 0; i < 4; i++) d->clip[i] = a->clip[i] * (1.0f - t) + b->clip[i] * t;
	d->v.s = a->v.s * (1.0f - t) + b->v.s * t;
	d->v.t = a->v.t * (1.0f - t) + b->v.t * t;
	d->v.q = a->v.q * (1.0f - t) + b->v.q * t;
	d->v.fogdepth = (float)(((int)(a->v.fogdepth * 256.0f) * (256 - ti) + (int)(b->v.fogdepth * 256.0f) * ti) >> 8) * (1.0f / 256.0f);
	for(i = 0; i < 4; i++){
		int ca = (int)((a->v.color0 >> (8 * i)) & 0xFF), cb = (int)((b->v.color0 >> (8 * i)) & 0xFF);
		int sa = (int)((a->v.color1 >> (8 * i)) & 0xFF), sb = (int)((b->v.color1 >> (8 * i)) & 0xFF);
		c0[i] = (ca * (256 - ti) + cb * ti) / 256;
		c1[i] = i < 3 ? (sa * (256 - ti) + sb * ti) / 256 : 0;
	}
	d->v.color0 = ge_pack_rgba(c0);
	d->v.color1 = ge_pack_rgba(c1) & 0x00FFFFFF;
}

static void clip_interpolate(GeClipVertex *d, float t, const GeClipVertex *a, const GeClipVertex *b){
	GeClipVertex tmp = *d;
	clip_lerp(&tmp, t, a, b);
	if(clip_to_screen(tmp.clip, &tmp.v)) tmp.v.x = GE_OUTSIDE;
	tmp.v.clipw = tmp.clip[3];
	*d = tmp;
}

/* Recorte en el plano cercano como el GE (gpu/probe exp43-46): desde el
   vértice de dentro, t = d_in / (d_in - d_out) con d = z + w y el
   recíproco del GE, y cada coordenada in + t * (out - in), en float24 */
static float near_plane_t(const float *in, const float *out){
	float d_in = ge_add(in[2], in[3]), d_out = ge_add(out[2], out[3]);
	float den = ge_trunc24(ge_add(d_in, -d_out));
	return ge_product24((double)ge_trunc24(d_in) * ge_recip(den));
}

static void near_point(GeClipVertex *d, const GeClipVertex *in, const GeClipVertex *out){
	float t = near_plane_t(in->clip, out->clip), delta, in_tc[3], out_tc[3], tc[3];
	int c;
	memset(d, 0, sizeof(*d));
	clip_lerp(d, t, in, out);
	for(c = 0; c < 4; c++){
		delta = ge_trunc24(ge_add(out->clip[c], -in->clip[c]));
		d->clip[c] = ge_trunc24(ge_add(ge_product24((double)t * delta), in->clip[c]));
	}
	/* Las uv con la misma aritmética que la posición (gpu/probe exp136) */
	in_tc[0] = in->v.s; in_tc[1] = in->v.t; in_tc[2] = in->v.q;
	out_tc[0] = out->v.s; out_tc[1] = out->v.t; out_tc[2] = out->v.q;
	for(c = 0; c < 3; c++){
		delta = ge_trunc24(ge_add(out_tc[c], -in_tc[c]));
		tc[c] = ge_trunc24(ge_add(ge_product24((double)t * delta), in_tc[c]));
	}
	d->v.s = tc[0]; d->v.t = tc[1]; d->v.q = tc[2];
	if(clip_to_screen(d->clip, &d->v)) d->v.x = GE_OUTSIDE;
	d->v.clipw = d->clip[3];
}

/* Rectángulo con proyección de textura: como triángulos */
static void add_triangle_rect(const GeVertex *v0, const GeVertex *v1){
	GeVertex buf[4], *tl = &buf[0], *tr = &buf[1], *bl = &buf[2], *br = &buf[3];
	int i;
	buf[0] = *v1; buf[0].x = v0->x; buf[0].y = v0->y;
	buf[0].s = v0->s; buf[0].t = v0->t; buf[0].q = v0->q;
	buf[1] = *v1; buf[1].x = v0->x;
	buf[1].s = v0->s; buf[1].q = v0->q;
	buf[2] = *v1; buf[2].y = v0->y;
	buf[2].t = v0->t;
	buf[3] = *v1;

	/* El rasterizador siempre descarta por orientación: se ordenan */
	for(i = 0; i < 4; i++){
		if(buf[i].x < tl->x && buf[i].y < tl->y) tl = &buf[i];
		if(buf[i].x > tr->x && buf[i].y < tr->y) tr = &buf[i];
		if(buf[i].x < bl->x && buf[i].y > bl->y) bl = &buf[i];
		if(buf[i].x > br->x && buf[i].y > br->y) br = &buf[i];
	}
	if((v0->x < v1->x && v0->y > v1->y) || (v0->x > v1->x && v0->y < v1->y)){
		float s = bl->s, t = bl->t, q = bl->q;
		bl->s = tr->s; bl->t = tr->t; bl->q = tr->q;
		tr->s = s; tr->t = t; tr->q = q;
	}
	ge_raster_triangle(tl, tr, bl);
	ge_raster_triangle(bl, tr, tl);
	ge_raster_triangle(tr, br, bl);
	ge_raster_triangle(bl, br, tr);
}

static void process_rect(const GeClipVertex *v0, const GeClipVertex *v1){
	ge_stats.sprites++;
	if(!ge_through()){
		int split_fog;
		if(outside_range(v0) || outside_range(v1)) return;
		if(outside_mask(v0->clip) & outside_mask(v1->clip)) return;
		/* No se recortan: con un vértice detrás de la cámara se descarta */
		if(!(v0->clip[3] > 0.0f && v1->clip[3] > 0.0f)) return;

		split_fog = v0->v.fogdepth != v1->v.fogdepth;
		if(split_fog){
			/* En el mismo 1/255 la niebla es plana (Resistance) */
			const float half = 0.5f / 255.0f;
			if(v1->v.fogdepth - half <= v0->v.fogdepth && v1->v.fogdepth + half >= v0->v.fogdepth) split_fog = 0;
		}
		if(split_fog){
			/* La niebla de un rectángulo va por el más cercano en x, al revés */
			GeVertex h0 = v1->v, h1 = v1->v, rev = v1->v;
			h0.x = v0->v.x + (v1->v.x - v0->v.x) / 2;
			h0.s = v0->v.s + (v1->v.s - v0->v.s) / 2;
			h1.x = v0->v.x + (v1->v.x - v0->v.x) / 2;
			h1.y = v0->v.y;
			h1.s = v0->v.s + (v1->v.s - v0->v.s) / 2;
			h1.t = v0->v.t;
			rev.fogdepth = v0->v.fogdepth;
			if(ge_raster_texture_proj()){
				add_triangle_rect(&v0->v, &h0);
				add_triangle_rect(&h1, &rev);
			} else {
				ge_raster_rect(&v0->v, &h0);
				ge_raster_rect(&h1, &rev);
			}
		} else if(ge_raster_texture_proj()){
			add_triangle_rect(&v0->v, &v1->v);
		} else {
			ge_raster_rect(&v0->v, &v1->v);
		}
	} else {
		if((ge.cmd[GE_CLEARMODE] & 1) && !(ge.cmd[GE_DITHERENABLE] & 1)) ge_raster_clear_rect(&v0->v, &v1->v);
		else ge_raster_rect(&v0->v, &v1->v);
	}
}

static void process_point(const GeClipVertex *v0){
	if(!ge_through()){
		if(outside_range(v0)) return;
		if(outside_mask(v0->clip)) return;
	}
	ge_raster_point(&v0->v);
}

static void process_line(const GeClipVertex *v0, const GeClipVertex *v1){
	int depth_clip, mask0, mask1, mask;
	GeClipVertex c0, c1;
	if(ge_through()){
		ge_raster_line(&v0->v, &v1->v);
		return;
	}
	depth_clip = ge.cmd[GE_DEPTHCLAMPENABLE] & 1;
	if(outside_range_before_clip(v0, depth_clip) || outside_range_before_clip(v1, depth_clip)) return;
	if(outside_mask(v0->clip) & outside_mask(v1->clip)) return;

	mask0 = calc_clip_mask(v0->clip);
	mask1 = calc_clip_mask(v1->clip);
	mask = mask0 | mask1;
	if(!depth_clip){
		if(!(v0->clip[3] > 0.0f && v1->clip[3] > 0.0f)) return;
		mask = 0;
	}
	if(mask == 0){
		ge_raster_line(&v0->v, &v1->v);
		return;
	}

	/* CLIP_LINE con el plano z + w >= 0 */
	c0 = *v0; c1 = *v1;
	{
		float dp0 = c0.clip[2] + c0.clip[3];
		float dp1 = c1.clip[2] + c1.clip[3];
		if(mask0 && dp0 < 0){
			float t = dp1 / (dp1 - dp0);
			clip_interpolate(&c0, t, &c1, &c0);
		}
		dp0 = c0.clip[2] + c0.clip[3];
		if(mask1 && dp1 < 0){
			float t = dp1 / (dp1 - dp0);
			clip_interpolate(&c1, t, &c1, &c0);
		}
	}
	if(!outside_range(&c0) && !outside_range(&c1)) ge_raster_line(&c0.v, &c1.v);
}

static void add_tri_flat(const GeClipVertex *a, const GeClipVertex *b, const GeClipVertex *c, const GeClipVertex *provoking){
	if(!(ge.cmd[GE_SHADEMODE] & 1)){
		/* Sombreado plano: así el orden del recorte no importa */
		GeVertex c2 = c->v;
		c2.color0 = provoking->v.color0;
		c2.color1 = provoking->v.color1;
		ge_raster_triangle(&a->v, &b->v, &c2);
	} else ge_raster_triangle(&a->v, &b->v, &c->v);
}

static void process_triangle(const GeClipVertex *v0, const GeClipVertex *v1, const GeClipVertex *v2,
                             const GeClipVertex *provoking, int reversed){
	int mask = 0, i, num_outside = 0, num_tris = 0, outside[3];
	const GeClipVertex *src[3], *tris[2][3];
	GeClipVertex made[2];

	if(!ge_through()){
		int depth_clip = ge.cmd[GE_DEPTHCLAMPENABLE] & 1;
		ge_tri_flags |= GE_TRI_OUTSIDE;   /* hasta que pase los descartes */
		if(outside_range_before_clip(v0, depth_clip) || outside_range_before_clip(v1, depth_clip) ||
		   outside_range_before_clip(v2, depth_clip)) return;
		/* Con w negativa en todos también se descarta */
		if(v0->clip[3] < 0.0f && v1->clip[3] < 0.0f && v2->clip[3] < 0.0f) return;
		mask |= calc_clip_mask(v0->clip);
		mask |= calc_clip_mask(v1->clip);
		mask |= calc_clip_mask(v2->clip);
		if(outside_mask(v0->clip) & outside_mask(v1->clip) & outside_mask(v2->clip)) return;
		/* Sin recorte de profundidad, la parte detrás del plano cercano se
		   dibuja con la z extrapolada; un vértice detrás de la cámara
		   (w <= 0) descarta el triángulo */
		if(!depth_clip){
			if(!(v0->clip[3] > 0.0f && v1->clip[3] > 0.0f && v2->clip[3] > 0.0f)) return;
			mask = 0;
		}
		ge_tri_flags &= ~GE_TRI_OUTSIDE;
	}

	if(mask == 0){
		add_tri_flat(v0, v1, v2, provoking);
		return;
	}
	ge_tri_flags |= GE_TRI_CLIPPED;

	/* Recorte como el GE (gpu/probe exp43, exp44): con un vértice fuera (o),
	   el cuadrilátero se parte desde el vértice anterior (p) en el orden
	   enviado: (p, a, b) y (p, b, n) */
	src[0] = v0; src[1] = v1; src[2] = v2;
	for(i = 0; i < 3; i++){
		outside[i] = src[i]->clip[2] < -src[i]->clip[3];
		num_outside += outside[i];
	}
	if(num_outside == 1){
		int o = outside[0] ? 0 : (outside[1] ? 1 : 2);
		int p = reversed ? (o + 1) % 3 : (o + 2) % 3;
		int n = reversed ? (o + 2) % 3 : (o + 1) % 3;
		int first = reversed ? 2 : 1, second = reversed ? 1 : 2;
		near_point(&made[0], src[p], src[o]);
		near_point(&made[1], src[n], src[o]);
		tris[0][0] = src[p]; tris[0][first] = &made[0]; tris[0][second] = &made[1];
		tris[1][0] = src[p]; tris[1][first] = &made[1]; tris[1][second] = src[n];
		num_tris = 2;
	} else if(num_outside == 2){
		int in = !outside[0] ? 0 : (!outside[1] ? 1 : 2);
		int n = (in + 1) % 3, p = (in + 2) % 3;
		near_point(&made[0], src[in], src[n]);
		near_point(&made[1], src[in], src[p]);
		tris[0][0] = src[in]; tris[0][1] = &made[0]; tris[0][2] = &made[1];
		num_tris = 1;
	}
	for(i = 0; i < num_tris; i++){
		if(outside_range(tris[i][0]) || outside_range(tris[i][1]) || outside_range(tris[i][2])) continue;
		add_tri_flat(tris[i][0], tris[i][1], tris[i][2], provoking);
	}
}

/* --- Ensamblado (SubmitPrimitive) ------------------------------------------ */

enum { CULL_CW = 0, CULL_CCW = 1, CULL_OFF = 2 };

int ge_tri_flags;

static GeClipVertex data_[4];
static int data_index_;
static u32 prev_prim_ = GE_PRIM_POINTS;
static int is_imm_draw;

static void send_triangle(int cull, const GeClipVertex *v, int provoking, int reversed){
	u32 drawn = ge_stats.primitives;
	ge_tri_flags = 0;
	if(cull == CULL_OFF){
		process_triangle(&v[0], &v[1], &v[2], &v[provoking], reversed);
		process_triangle(&v[2], &v[1], &v[0], &v[provoking], !reversed);
	} else if(cull == CULL_CW){
		process_triangle(&v[2], &v[1], &v[0], &v[provoking], !reversed);
	} else {
		process_triangle(&v[0], &v[1], &v[2], &v[provoking], reversed);
	}
	ge_stats.tris++;
	if(ge_stats.primitives != drawn) ge_stats.tris_drawn++;
	else if(ge_tri_flags & GE_TRI_OUTSIDE) ge_stats.tris_outside++;
	else ge_stats.tris_back++;
	if(ge_tri_flags & GE_TRI_CLIPPED) ge_stats.tris_clipped++;
}

/* Fuente de vértices de una llamada: memoria (con índices) o una lista ya
   decodificada (curvas) */
typedef struct {
	const VFormat *f;
	u32 vaddr, iaddr;
	int use_indices, use_cache, zero;
	u32 lower, upper;
	const DecVertex *list;     /* vértices ya decodificados (curvas) */
	const u16 *idx_list;       /* sus índices */
	GeClipVertex *cache;
} VSource;

static GeClipVertex *vcache;
static int vcache_cap;

static u32 index_at(const VSource *s, int i){
	if(s->idx_list) return s->idx_list[i];
	switch(s->f->idx){
	case 1: return mem_read8(s->iaddr + (u32)i);
	case 2: return mem_read16(s->iaddr + (u32)i * 2);
	case 3: return mem_read32(s->iaddr + (u32)i * 4) & 0xFFFF;  /* solo 16 bits (hardware) */
	default: return (u32)i;
	}
}

static void read_raw(const VSource *s, u32 index, GeClipVertex *out){
	DecVertex d;
	int phase = prof_ge_enter(GEF_LEER);
	GE_PROF_T0;
	if(s->list) d = s->list[index];
	else if(s->zero) memset(&d, 0, sizeof(d));
	else decode_vertex(s->f, mem_ptr_r(s->vaddr + index * s->f->size, s->f->size), &d);
	GE_PROF_ADD(5);
	prof_ge_phase = GEF_TRANSFORMAR;
	read_vertex(s->f, &d, out);
	ge_stats.vertices++;
	if(s->f->through) ge_stats.vertices_through++;
	else if(s->f->wt) ge_stats.vertices_skinned++;
	else if(s->f->nmorph > 1) ge_stats.vertices_morph++;
	GE_PROF_ADD(0);
	prof_ge_leave(phase);
}

static void vsource_read(const VSource *s, int vtx, GeClipVertex *out){
	ge_stats.vertex_reads++;
	if(s->use_indices){
		u32 idx = index_at(s, vtx);
		if(s->use_cache){ *out = s->cache[idx - s->lower]; return; }
		read_raw(s, idx, out);
	} else read_raw(s, (u32)vtx, out);
}

static void vsource_init(VSource *s, const VFormat *f, u32 vaddr, u32 iaddr, int count){
	int i;
	memset(s, 0, sizeof(*s));
	s->f = f;
	s->vaddr = vaddr;
	s->iaddr = iaddr;
	s->use_indices = f->idx != 0;
	s->upper = count == 0 ? 0 : (u32)(count - 1);
	if(s->use_indices && count > 0){
		u32 lo = 0xFFFF, hi = 0;
		for(i = 0; i < count; i++){
			u32 v = index_at(s, i);
			if(v > hi) hi = v;
			if(v < lo) lo = v;
		}
		s->lower = lo;
		s->upper = hi;
	}
	/* Datos desalineados: el decodificador los deja a cero */
	if(vaddr & (f->biggest - 1)) s->zero = 1;
	/* Con índices repetidos se leen una vez todos los del rango, en orden
	   (eso decide qué uv o normal "arrastra" cada vértice) */
	s->use_cache = s->use_indices && count > (int)(s->upper - s->lower + 1);
	if(s->use_cache){
		int n = (int)(s->upper - s->lower + 1);
		if(n > vcache_cap){
			GeClipVertex *nc = realloc(vcache, (size_t)n * sizeof(GeClipVertex));
			if(!nc){ s->use_cache = 0; return; }
			vcache = nc;
			vcache_cap = n;
		}
		s->cache = vcache;
		for(i = 0; i < n; i++) read_raw(s, s->lower + (u32)i, &s->cache[i]);
	}
}

static void submit_primitive_body(const VSource *vr, u32 prim_type, int vertex_count);

static void submit_primitive(const VSource *vr, u32 prim_type, int vertex_count){
	int phase = prof_ge_enter(GEF_ENSAMBLAR);
	submit_primitive_body(vr, prim_type, vertex_count);
	prof_ge_leave(phase);
}

static void submit_primitive_body(const VSource *vr, u32 prim_type, int vertex_count){
	int cull_on = (ge.cmd[GE_CULLFACEENABLE] & 1) && !(ge.cmd[GE_CLEARMODE] & 1);
	int cull = cull_on ? ((ge.cmd[GE_CULL] & 1) ? CULL_CCW : CULL_CW) : CULL_OFF;
	int vtx, i;

	if(prim_type != GE_PRIM_CONTINUE){
		data_index_ = 0;
		prev_prim_ = prim_type;
	} else prim_type = prev_prim_;

	ge_raster_begin();

	/* Se permiten 0 vértices con data_index_ > 0: modo inmediato */
	switch(prim_type){
	case GE_PRIM_POINTS:
		for(i = 0; i < data_index_; i++) process_point(&data_[i]);
		data_index_ = 0;
		for(vtx = 0; vtx < vertex_count; vtx++){
			vsource_read(vr, vtx, &data_[0]);
			process_point(&data_[0]);
		}
		break;

	case GE_PRIM_LINES:
		for(i = 0; i < data_index_ - 1; i += 2) process_line(&data_[i], &data_[i + 1]);
		data_index_ &= 1;
		for(vtx = 0; vtx < vertex_count; vtx++){
			vsource_read(vr, vtx, &data_[data_index_++]);
			if(data_index_ == 2){
				process_line(&data_[0], &data_[1]);
				data_index_ = 0;
			}
		}
		break;

	case GE_PRIM_TRIANGLES:
		for(vtx = 0; vtx < vertex_count; vtx++){
			vsource_read(vr, vtx, &data_[data_index_++]);
			if(data_index_ < 3) continue;  /* uno incompleto sigue para PRIM continuar */
			data_index_ = 0;
			send_triangle(cull, data_, 2, 0);
		}
		if(data_index_ >= 3){
			send_triangle(cull, data_, 2, 0);
			data_index_ = 0;
		}
		break;

	case GE_PRIM_RECTANGLES:
		for(vtx = 0; vtx < vertex_count; vtx++){
			vsource_read(vr, vtx, &data_[data_index_++]);
			if(data_index_ == 4){
				process_rect(&data_[0], &data_[1]);
				process_rect(&data_[2], &data_[3]);
				data_index_ = 0;
			}
		}
		if(data_index_ >= 2){
			process_rect(&data_[0], &data_[1]);
			data_index_ -= 2;
		}
		break;

	case GE_PRIM_LINE_STRIP: {
		/* Sin línea al cargar el primer vértice */
		int skip = data_index_ == 0 ? 1 : 0;
		for(vtx = 0; vtx < vertex_count; vtx++){
			vsource_read(vr, vtx, &data_[(data_index_++) & 1]);
			if(skip) skip--;
			else process_line(&data_[data_index_ & 1], &data_[(data_index_ & 1) ^ 1]);
		}
		if(is_imm_draw && data_index_ >= 2)
			process_line(&data_[data_index_ & 1], &data_[(data_index_ & 1) ^ 1]);
		break;
	}

	case GE_PRIM_TRIANGLE_STRIP: {
		int skip = data_index_ >= 2 ? 0 : 2 - data_index_, start = 0;
		for(vtx = start; vtx < vertex_count && skip > 0; vtx++){
			vsource_read(vr, vtx, &data_[(data_index_++) % 3]);
			skip--;
			start++;
		}
		for(vtx = start; vtx < vertex_count; vtx++){
			int provoking = (data_index_++) % 3, wind = (data_index_ - 1) % 2;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			vsource_read(vr, vtx, &data_[provoking]);
			/* Los impares llegan en el orden contrario al del GE (gpu/probe exp45) */
			send_triangle(alt, data_, provoking, wind != 0);
		}
		if(is_imm_draw && data_index_ >= 3){
			int provoking = (data_index_ - 1) % 3, wind = (data_index_ - 1) % 2;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			send_triangle(alt, data_, provoking, wind != 0);
		}
		break;
	}

	case GE_PRIM_TRIANGLE_FAN: {
		int skip = data_index_ <= 1 ? 1 : 0, start = 0;
		/* El centro solo se lee si no se continúa */
		if(data_index_ == 0 && vertex_count > 0){
			vsource_read(vr, 0, &data_[0]);
			data_index_++;
			start = 1;
		}
		for(vtx = start; vtx < vertex_count && skip > 0; vtx++){
			int provoking = 2 - ((data_index_++) % 2);
			vsource_read(vr, vtx, &data_[provoking]);
			skip--;
			start++;
		}
		for(vtx = start; vtx < vertex_count; vtx++){
			int provoking = 2 - ((data_index_++) % 2), wind = (data_index_ - 1) % 2;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			vsource_read(vr, vtx, &data_[provoking]);
			send_triangle(alt, data_, provoking, wind != 0);
		}
		if(is_imm_draw && data_index_ >= 3){
			int wind = (data_index_ - 1) % 2, provoking = 2 - wind;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			send_triangle(alt, data_, provoking, wind != 0);
		}
		break;
	}

	default:
		break;
	}
}

static void load_morph_weights(void){
	int i;
	for(i = 0; i < 8; i++) morph_weights[i] = ge_f24(ge.cmd[GE_MORPHWEIGHT0 + i]);
}

static void setup_uv_prescale(const VFormat *f){
	/* Solo con UV gen 3 (desconocido) en modo transformado prescala el
	   decodificador; con 0 lo hace read_vertex con la aritmética del GE */
	uv_prescale = !f->through && (ge.cmd[GE_TEXMAPMODE] & 3) == 3;
	uv_ps_scale[0] = ge_f24(ge.cmd[GE_TEXSCALEU]);
	uv_ps_scale[1] = ge_f24(ge.cmd[GE_TEXSCALEV]);
	uv_ps_off[0] = ge_f24(ge.cmd[GE_TEXOFFSETU]);
	uv_ps_off[1] = ge_f24(ge.cmd[GE_TEXOFFSETV]);
}

void ge_draw_prim(u32 prim, u32 count){
	VFormat f;
	VSource vr;
	u32 vaddr = ge.vaddr, iaddr = ge.iaddr;

	if(count == 0) return;
	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	if(!mem_valid(vaddr, 1)) return;
	if(f.idx && !mem_valid(iaddr, 1)) return;

	/* Sin formato de posición no se dibuja, pero se avanza */
	if(f.pos){
		int phase;
		GE_PROF_T0;
		load_morph_weights();
		setup_uv_prescale(&f);
		compute_transform_state(&f);
		phase = prof_ge_enter(GEF_LEER);
		vsource_init(&vr, &f, vaddr, iaddr, (int)count);
		prof_ge_leave(phase);
		ge_stats.draws++;
		ge_stats.draws_prim[prim & 7]++;
		if(vr.use_indices) ge_stats.draws_indexed++;
		if(vr.use_cache) ge_stats.draws_reuse++;
		GE_PROF_ADD(3);
		submit_primitive(&vr, prim, (int)count);
		GE_PROF_ADD(2);
	}

	/* El GE avanza las direcciones tras dibujar */
	if(f.idx) ge.iaddr = iaddr + count * idx_size[f.idx];
	else ge.vaddr = vaddr + count * f.size;
}

/* --- Bounding box (para BJUMP) --------------------------------------------- */

/* La prueba aproximada de PPSSPP (DrawEngineCommon::TestBoundingBox) */
static int test_bounding_box(const VFormat *f, u32 vaddr, u32 iaddr, int count){
	float world[16], view[16], vp[16], wvp[16];
	float fox, foy, left, top, right, bottom;
	int inside[6] = { 0, 0, 0, 0, 0, 0 }, i, k, r, c, check;
	int sx1 = (int)(ge.cmd[GE_SCISSOR1] & 0x3FF), sy1 = (int)((ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF);
	int sx2 = (int)(ge.cmd[GE_SCISSOR2] & 0x3FF), sy2 = (int)((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF);
	int rx1 = (int)(ge.cmd[GE_REGION1] & 0x3FF), ry1 = (int)((ge.cmd[GE_REGION1] >> 10) & 0x3FF);
	int rx2 = (int)(ge.cmd[GE_REGION2] & 0x3FF), ry2 = (int)((ge.cmd[GE_REGION2] >> 10) & 0x3FF);
	float xs = ge_f24(ge.cmd[GE_VIEWPORTXSCALE]), xc = ge_f24(ge.cmd[GE_VIEWPORTXCENTER]);
	float ys = ge_f24(ge.cmd[GE_VIEWPORTYSCALE]), yc = ge_f24(ge.cmd[GE_VIEWPORTYCENTER]);
	VSource src;
	VFormat nt = *f;

	if(count > 1024) return 1;

	to4x4(ge.world, world);
	to4x4(ge.view, view);
	for(r = 0; r < 4; r++)
		for(c = 0; c < 4; c++){
			vp[r * 4 + c] = 0.0f;
			for(k = 0; k < 4; k++) vp[r * 4 + c] += view[r * 4 + k] * ge.proj[k * 4 + c];
		}
	for(r = 0; r < 4; r++)
		for(c = 0; c < 4; c++){
			wvp[r * 4 + c] = 0.0f;
			for(k = 0; k < 4; k++) wvp[r * 4 + c] += world[r * 4 + k] * vp[k * 4 + c];
		}

	/* No se sabe por qué arriba e izquierda van un píxel desplazados */
	fox = (float)(ge.cmd[GE_OFFSETX] & 0xFFFF) / 16.0f;
	foy = (float)(ge.cmd[GE_OFFSETY] & 0xFFFF) / 16.0f;
	left = fox + (float)((rx1 > sx1 ? rx1 : sx1) - 1);
	top = foy + (float)((ry1 > sy1 ? ry1 : sy1) - 1);
	right = fox + (float)((rx2 < sx2 ? rx2 : sx2) + 1);
	bottom = foy + (float)((ry2 < sy2 ? ry2 : sy2) + 1);
	/* Si la caja sale del espacio de 4096, todo pasa */
	if(right >= 4096.0f || bottom >= 4096.0f || left < 1.0f || top < 1.0f) return 1;

	nt.through = 0;
	memset(&src, 0, sizeof(src));
	src.f = f;
	src.iaddr = iaddr;
	for(i = 0; i < count; i++){
		DecVertex d;
		float p[4], w, dx, dy;
		u32 idx = f->idx ? index_at(&src, i) : (u32)i;
		decode_vertex(&nt, mem_ptr_r(vaddr + idx * f->size, f->size), &d);
		for(k = 0; k < 4; k++) p[k] = d.pos[0] * wvp[k] + d.pos[1] * wvp[4 + k] + d.pos[2] * wvp[8 + k] + wvp[12 + k];
		if(p[2] >= -p[3]) inside[4]++;
		if(p[2] <= p[3]) inside[5]++;
		w = p[3];
		dx = p[0] * xs + xc * w;
		dy = p[1] * ys + yc * w;
		if(dx >= left * w) inside[0]++;
		if(dx <= right * w) inside[1]++;
		if(dy >= top * w) inside[2]++;
		if(dy <= bottom * w) inside[3]++;
	}
	check = (ge.cmd[GE_DEPTHCLAMPENABLE] & 1) ? 6 : 4;
	for(i = 0; i < check; i++) if(inside[i] == 0) return 0;
	return 1;
}

void ge_bounding_box(u32 count){
	VFormat f;
	u32 vaddr = ge.vaddr, iaddr = ge.iaddr, bytes;
	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	load_morph_weights();
	uv_prescale = 0;
	bytes = (f.idx ? 1 : f.size) * count;
	if(!mem_valid(vaddr, bytes) || (f.idx && !mem_valid(iaddr, count * idx_size[f.idx]))){
		ge.bbox_visible = 1;
		return;
	}
	/* El GE solo mira un tramo de 0x100 */
	if(count > 0x200) ge.bbox_visible = test_bounding_box(&f, vaddr + (count - 0x200) * f.size, iaddr, 0x100);
	else if(count > 0x100) ge.bbox_visible = test_bounding_box(&f, vaddr, iaddr, (int)count - 0x100);
	else ge.bbox_visible = test_bounding_box(&f, vaddr, iaddr, (int)count);

	if(f.idx) ge.iaddr = iaddr + count * idx_size[f.idx];
	else ge.vaddr = vaddr + bytes;
}

/* --- Vértices inmediatos (comandos 0xF0-0xF9) ------------------------------- */

/* Como SoftGPU de PPSSPP: cada VAP se envía en el acto */
static u32 imm_prim = 0xFFFFFFFFu, imm_flags;
static int imm_first_sent;

void ge_immediate_vertex(void){
	static const int fl_cmd[6] = { GE_ANTIALIASENABLE, GE_SHADEMODE, GE_CULLFACEENABLE, GE_TEXTUREMAPENABLE,
	                               GE_FOGENABLE, GE_DITHERENABLE };
	static const u32 fl_bit[6] = { 0x800, 0x40000, 0x80000, 0x200000, 0x400000, 0x800000 };
	u32 vap = ge.cmd[GE_VAP], prim = (vap >> 8) & 7, color0, save[6], save_cull = ge.cmd[GE_CULL];
	int through = ge_through(), changed = 0, i;
	float x, y, z;
	GeClipVertex cv;
	VFormat f;
	VSource vr;

	if(through){
		x = (float)((int)(ge.cmd[GE_VSCX] & 0xFFFF) - 0x8000) / 16.0f;
		y = (float)((int)(ge.cmd[GE_VSCY] & 0xFFFF) - 0x8000) / 16.0f;
	} else {
		x = (float)((int)(ge.cmd[GE_VSCX] & 0xFFFF) - (int)(ge.cmd[GE_OFFSETX] & 0xFFFF)) / 16.0f;
		y = (float)((int)(ge.cmd[GE_VSCY] & 0xFFFF) - (int)(ge.cmd[GE_OFFSETY] & 0xFFFF)) / 16.0f;
	}
	z = (float)(ge.cmd[GE_VSCZ] & 0xFFFF);
	color0 = (ge.cmd[GE_VCV] & 0xFFFFFF) | ((vap & 0xFF) << 24);

	if(prim != GE_PRIM_CONTINUE){
		imm_prim = prim;
		/* Las banderas solo cuentan desde la primera */
		imm_flags = vap & 0x00FFF800;
		imm_first_sent = 0;
	} else if(imm_prim == 0xFFFFFFFFu) return;

	/* Un punto en (0, 0, 0) negro sale de limpiar el estado: se ignora */
	if(imm_prim == GE_PRIM_POINTS && x == 0.0f && y == 0.0f && z == 0.0f && color0 == 0) return;

	/* Las banderas mandan durante el dibujo (el antialias solo si cambia
	   alguna otra) */
	for(i = 1; i < 6; i++) if(((imm_flags & fl_bit[i]) != 0) != (int)(ge.cmd[fl_cmd[i]] & 1)) changed = 1;
	for(i = 0; i < 6; i++) save[i] = ge.cmd[fl_cmd[i]];
	if(changed) for(i = 0; i < 6; i++) ge.cmd[fl_cmd[i]] = (imm_flags & fl_bit[i]) ? 1 : 0;
	ge.cmd[GE_CULL] = (imm_flags & 0x100000) ? 1 : 0;

	/* El vértice ya transformado, con el viewport a escala 1 */
	memset(&cv, 0, sizeof(cv));
	cv.clip[0] = x; cv.clip[1] = y; cv.clip[2] = z; cv.clip[3] = 1.0f;
	cv.v.s = ge_f24(ge.cmd[GE_VTCS]);
	cv.v.t = ge_f24(ge.cmd[GE_VTCT]);
	cv.v.q = ge_f24(ge.cmd[GE_VTCQ]);
	if(through){
		cv.v.s *= (float)(1 << (ge.cmd[GE_TEXSIZE0] & 0xF));
		cv.v.t *= (float)(1 << ((ge.cmd[GE_TEXSIZE0] >> 8) & 0xF));
	} else cv.clip[2] *= 1.0f / 65535.0f;
	cv.v.clipw = 1.0f;
	cv.v.color0 = color0;
	cv.v.color1 = (ge.cmd[GE_LIGHTMODE] & 1) && !through ? (ge.cmd[GE_VSCV] & 0xFFFFFF) : 0;
	cv.v.fogdepth = (float)(ge.cmd[GE_VFC] & 0xFF) / 255.0f;
	cv.v.x = ge_f2i(x * 16.0f);
	cv.v.y = ge_f2i(y * 16.0f);
	cv.v.z = (u16)(u32)z;

	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE] | (3u << 7));
	memset(&vr, 0, sizeof(vr));
	vr.f = &f;
	/* Antes del primero, una llamada vacía reinicia la primitiva */
	if(!imm_first_sent) submit_primitive(&vr, imm_prim, 0);

	switch(prev_prim_){
	case GE_PRIM_LINE_STRIP:     data_[(data_index_++) & 1] = cv; break;
	case GE_PRIM_TRIANGLE_STRIP: data_[(data_index_++) % 3] = cv; break;
	case GE_PRIM_TRIANGLE_FAN:
		if(data_index_ == 0) data_[data_index_++] = cv;
		else data_[2 - ((data_index_++) % 2)] = cv;
		break;
	default:
		if(data_index_ < 4) data_[data_index_++] = cv;
		break;
	}
	is_imm_draw = 1;
	cull_xy = 0;
	submit_primitive(&vr, GE_PRIM_CONTINUE, 0);
	cull_xy = 1;
	is_imm_draw = 0;
	imm_first_sent = 1;

	ge.cmd[GE_CULL] = save_cull;
	for(i = 0; i < 6; i++) ge.cmd[fl_cmd[i]] = save[i];
}

/* --- Parches bezier y spline (SplineCommon.cpp) ------------------------------
   Como los evalúa el GE (gpu/probe exp48-51, exp139-145): el parámetro es
   k/256 redondeado hacia el centro del parche y se evalúan primero las
   columnas (v) y luego las filas (u), por de Casteljau (bezier) o de Boor
   (spline). Posiciones y uv: cada interpolación en punto fijo de 16 bits al
   exponente del mayor operando, a + floor((b - a) k / 256). Colores: lo
   mismo sobre (c << 7) | 0x7F. Normales: el producto cruzado (sumado como
   lo hace la unidad de matrices) de dos tangentes. Los vértices generados
   se dibujan como una lista indexada de triángulos, líneas o puntos. */

static int bezier_param256(int i, int n){
	return 2 * i <= n ? (256 * i) / n : 256 - (256 * (n - i)) / n;
}

static inline int to_fixed16(u32 bits, int ex, int e){
	int m;
	if(ex == 0 || e - ex >= 16) return 0;
	m = (int)(((bits & 0x7FFFFF) | 0x800000) >> (e - ex + 8));
	return (bits & 0x80000000u) ? -m : m;
}

static float ge_bezier_lerp(float a, float b, int k){
	u32 ba, bb, scale_bits;
	int ea, eb, e, fa, fb, r;
	float scale;
	if(k == 0) return a;
	if(k == 256) return b;
	memcpy(&ba, &a, 4);
	memcpy(&bb, &b, 4);
	ea = (int)((ba >> 23) & 0xFF);
	eb = (int)((bb >> 23) & 0xFF);
	e = ea > eb ? ea : eb;
	if(e == 0) return 0.0f;
	fa = to_fixed16(ba, ea, e);
	fb = to_fixed16(bb, eb, e);
	r = fa + (((fb - fa) * k) >> 8);
	/* r * 2^(e - 127 - 15), exacto */
	if(e <= 15) return ldexpf((float)r, e - 127 - 15);
	scale_bits = (u32)(e - 15) << 23;
	memcpy(&scale, &scale_bits, 4);
	return (float)r * scale;
}

/* También devuelve los dos últimos puntos de de Casteljau (para la normal) */
static float ge_bezier_eval(const float *p, int k, float *ab, float *bc){
	float a = ge_bezier_lerp(p[0], p[1], k), b = ge_bezier_lerp(p[1], p[2], k), c = ge_bezier_lerp(p[2], p[3], k);
	float l = ge_bezier_lerp(a, b, k), r = ge_bezier_lerp(b, c, k);
	if(ab){ *ab = l; *bc = r; }
	return ge_bezier_lerp(l, r, k);
}

static inline int lerp_color(int a, int b, int k){ return a + (((b - a) * k) >> 8); }

static int ge_bezier_eval_color(const int *p, int k){
	int a = lerp_color(p[0], p[1], k), b = lerp_color(p[1], p[2], k), c = lerp_color(p[2], p[3], k);
	return lerp_color(lerp_color(a, b, k), lerp_color(b, c, k), k);
}

/* de Boor en t = k/256: cada factor (t - K[i]) / tramo sale del nudo más
   cercano de los dos, en 1/256. Un extremo abierto repite su nudo cuatro
   veces; uno cerrado conserva el espaciado unidad. */
typedef struct { int seg, alpha[6], k; } SplineParam;

static int spline_knot(int i, int npatches, int type){
	if(i < 3 && (type & 1)) return 0;
	if(i > npatches + 3 && (type & 2)) return npatches;
	return i - 3;
}

static SplineParam spline_param_at(int index, int tess, int npatches, int type){
	static const int rs[6] = { 1, 1, 1, 2, 2, 3 };
	SplineParam p;
	int l, n, is[6];
	p.seg = index / tess;
	p.k = bezier_param256(index % tess, tess);
	if(p.seg == npatches){ p.seg = npatches - 1; p.k = 256; }
	l = p.seg + 3;
	is[0] = l - 2; is[1] = l - 1; is[2] = l; is[3] = l - 1; is[4] = l; is[5] = l;
	for(n = 0; n < 6; n++){
		int i = is[n];
		int span = spline_knot(i + 4 - rs[n], npatches, type) - spline_knot(i, npatches, type);
		int dl = 256 * (p.seg - spline_knot(i, npatches, type)) + p.k;
		int dr = 256 * span - dl;
		p.alpha[n] = span == 0 ? 0 : (dl <= dr ? dl / span : 256 - dr / span);
	}
	return p;
}

static float ge_spline_eval(const float *d, const int *a, float *ab, float *bc){
	float l0 = ge_bezier_lerp(d[0], d[1], a[0]), l1 = ge_bezier_lerp(d[1], d[2], a[1]), l2 = ge_bezier_lerp(d[2], d[3], a[2]);
	float m0 = ge_bezier_lerp(l0, l1, a[3]), m1 = ge_bezier_lerp(l1, l2, a[4]);
	if(ab){ *ab = m0; *bc = m1; }
	return ge_bezier_lerp(m0, m1, a[5]);
}

static int ge_spline_eval_color(const int *d, const int *a){
	int l0 = lerp_color(d[0], d[1], a[0]), l1 = lerp_color(d[1], d[2], a[1]), l2 = lerp_color(d[2], d[3], a[2]);
	return lerp_color(lerp_color(l0, l1, a[3]), lerp_color(l1, l2, a[4]), a[5]);
}

/* Una columna de puntos de control evaluada en un v */
typedef struct {
	float pos[3], ab[3], bc[3], tex[2];
	int col[4];   /* 15 bits */
} CurveColumn;

typedef struct {
	int sample_nrm, sample_col, sample_tex, facing;
	const DecVertex *const *pts;    /* puntos de control por índice */
	u32 defcolor;
} CurveCtx;

static void column_eval(const CurveCtx *cx, CurveColumn *col, const int *idx, int is_spline, int k, const int *alpha){
	int j, r;
	for(j = 0; j < 3; j++){
		float p[4];
		for(r = 0; r < 4; r++) p[r] = cx->pts[idx[r]]->pos[j];
		col->pos[j] = is_spline ? ge_spline_eval(p, alpha, &col->ab[j], &col->bc[j]) : ge_bezier_eval(p, k, &col->ab[j], &col->bc[j]);
	}
	if(cx->sample_tex)
		for(j = 0; j < 2; j++){
			float p[4];
			for(r = 0; r < 4; r++) p[r] = cx->pts[idx[r]]->uv[j];
			col->tex[j] = is_spline ? ge_spline_eval(p, alpha, NULL, NULL) : ge_bezier_eval(p, k, NULL, NULL);
		}
	if(cx->sample_col)
		for(j = 0; j < 4; j++){
			int p[4];
			for(r = 0; r < 4; r++) p[r] = (int)(((cx->pts[idx[r]]->color0 >> (8 * j)) & 0xFF) << 7) | 0x7F;
			col->col[j] = is_spline ? ge_spline_eval_color(p, alpha) : ge_bezier_eval_color(p, k);
		}
}

/* El vértice en (u, v) a partir de las cuatro columnas */
static void row_eval(const CurveCtx *cx, const CurveColumn *cols, DecVertex *out, int is_spline, int k, const int *alpha, float gen_u, float gen_v){
	float tu[3], tv[3];
	int j;
	memset(out, 0, sizeof(*out));
	for(j = 0; j < 3; j++){
		float row[4] = { cols[0].pos[j], cols[1].pos[j], cols[2].pos[j], cols[3].pos[j] }, ab, bc;
		out->pos[j] = is_spline ? ge_spline_eval(row, alpha, &ab, &bc) : ge_bezier_eval(row, k, &ab, &bc);
		if(cx->sample_nrm){
			float ab_row[4] = { cols[0].ab[j], cols[1].ab[j], cols[2].ab[j], cols[3].ab[j] };
			float bc_row[4] = { cols[0].bc[j], cols[1].bc[j], cols[2].bc[j], cols[3].bc[j] };
			tu[j] = ge_add(bc, -ab);
			if(is_spline) tv[j] = ge_add(ge_spline_eval(bc_row, alpha, NULL, NULL), -ge_spline_eval(ab_row, alpha, NULL, NULL));
			else tv[j] = ge_add(ge_bezier_eval(bc_row, k, NULL, NULL), -ge_bezier_eval(ab_row, k, NULL, NULL));
		}
	}
	if(cx->sample_col){
		for(j = 0; j < 4; j++){
			int row[4] = { cols[0].col[j], cols[1].col[j], cols[2].col[j], cols[3].col[j] };
			int c = is_spline ? ge_spline_eval_color(row, alpha) : ge_bezier_eval_color(row, k);
			out->color0 |= (u32)((c >> 7) & 0xFF) << (8 * j);
		}
	} else out->color0 = cx->defcolor;
	if(cx->sample_tex){
		for(j = 0; j < 2; j++){
			float row[4] = { cols[0].tex[j], cols[1].tex[j], cols[2].tex[j], cols[3].tex[j] };
			out->uv[j] = is_spline ? ge_spline_eval(row, alpha, NULL, NULL) : ge_bezier_eval(row, k, NULL, NULL);
		}
	} else {
		/* Generadas: el propio parámetro (gpu/probe exp140) */
		out->uv[0] = gen_u;
		out->uv[1] = gen_v;
	}
	if(cx->sample_nrm){
		/* Sin normalizar: las luces lo hacen (gpu/probe exp142) */
		for(j = 0; j < 3; j++){
			int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
			GeRowTerm t[2];
			t[0] = ge_product(tu[j1], tv[j2]);
			t[1] = ge_product(-tu[j2], tv[j1]);
			out->nrm[j] = ge_row_sum(t, 2);
			if(cx->facing) out->nrm[j] *= -1.0f;
		}
	} else out->nrm[2] = 1.0f;
}

static DecVertex *curve_cp, *curve_out;
static const DecVertex **curve_pts;
static u16 *curve_idx;
static CurveColumn *curve_cols;
static size_t curve_cp_cap, curve_out_cap, curve_pts_cap, curve_idx_cap, curve_cols_cap;

static void *grow(void *p, size_t *cap, size_t need, size_t elem){
	if(need <= *cap) return p;
	p = realloc(p, need * elem);
	*cap = p ? need : 0;
	return p;
}

/* Un cuadrilátero de la rejilla: líneas en zigzag (borde izquierdo abajo,
   diagonal arriba, borde derecho abajo; gpu/probe exp175) o dos triángulos */
static void quad_index(u16 **o, int prim_type, int i0, int i1, int i2, int i3){
	u16 *p = *o;
	if(prim_type == 1){ *p++ = (u16)i0; *p++ = (u16)i2; *p++ = (u16)i2; *p++ = (u16)i1; *p++ = (u16)i1; *p++ = (u16)i3; }
	else { *p++ = (u16)i0; *p++ = (u16)i2; *p++ = (u16)i1; *p++ = (u16)i1; *p++ = (u16)i2; *p++ = (u16)i3; }
	*o = p;
}

static void build_index(u16 **o, int nu, int nv, int prim_type, int total){
	int u, v;
	for(v = 0; v < nv; v++)
		for(u = 0; u < nu; u++){
			int i0 = v * (nu + 1) + u + total, i2 = (v + 1) * (nu + 1) + u + total;
			quad_index(o, prim_type, i0, i0 + 1, i2, i2 + 1);
		}
}

#define CURVE_MAX_VERTS 65536

static void submit_curve(u32 arg, int is_spline){
	VFormat f, gf;
	VSource vr, s;
	int nu = (int)(arg & 0xFF), nv = (int)((arg >> 8) & 0xFF), num_points = nu * nv;
	int tess_u = (int)(ge.cmd[GE_PATCHDIVISION] & 0x7F), tess_v = (int)((ge.cmd[GE_PATCHDIVISION] >> 8) & 0x7F);
	int type_u = (int)((arg >> 16) & 3), type_v = (int)((arg >> 18) & 3);
	int prim_type = (int)(ge.cmd[GE_PATCHPRIMITIVE] & 3), npu, npv, nverts, nquads, i;
	u32 vaddr = ge.vaddr, iaddr = ge.iaddr, lo = 0, hi;
	u16 *op;
	CurveCtx cx;
	static const u32 prims[4] = { GE_PRIM_TRIANGLES, GE_PRIM_LINES, GE_PRIM_POINTS, GE_PRIM_POINTS };

	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	if(!mem_valid(vaddr, 1)) return;
	if(f.idx && !mem_valid(iaddr, 1)) return;

	/* Con menos de 4 puntos en u o v el hardware no dibuja nada; los
	   índices avanzan igual, los vértices no */
	if(nu >= 4 && nv >= 4){
		load_morph_weights();
		uv_prescale = 0;
		memset(&s, 0, sizeof(s));
		s.f = &f;
		s.iaddr = iaddr;
		hi = (u32)num_points - 1;
		if(f.idx){
			lo = 0xFFFF; hi = 0;
			for(i = 0; i < num_points; i++){
				u32 v = index_at(&s, i);
				if(v > hi) hi = v;
				if(v < lo) lo = v;
			}
		}
		curve_cp = grow(curve_cp, &curve_cp_cap, hi + 1, sizeof(DecVertex));
		curve_pts = grow(curve_pts, &curve_pts_cap, (size_t)num_points, sizeof(*curve_pts));
		if(!curve_cp || !curve_pts) goto advance;
		for(i = (int)lo; i <= (int)hi; i++){
			DecVertex *d = &curve_cp[i];
			if(vaddr & (f.biggest - 1)) memset(d, 0, sizeof(*d));
			else decode_vertex(&f, mem_ptr_r(vaddr + (u32)i * f.size, f.size), d);
			if(!f.tc){ d->uv[0] = 0.0f; d->uv[1] = 0.0f; }
			if(!f.col) d->color0 = material_ambient_rgba();
			if(!f.nrm){ d->nrm[0] = 0.0f; d->nrm[1] = 0.0f; d->nrm[2] = 1.0f; }
		}
		for(i = 0; i < num_points; i++) curve_pts[i] = &curve_cp[f.idx ? index_at(&s, i) : (u32)i];

		if(tess_u < 1) tess_u = 1;
		if(tess_v < 1) tess_v = 1;
		if(is_spline){ npu = nu - 3; npv = nv - 3; }
		else { npu = (nu - 1) / 3; npv = (nv - 1) / 3; }
		/* Se reduce hasta que quepa (el factor mayor primero) */
		for(;;){
			long n = is_spline ? (long)(npu * tess_u + 1) * (npv * tess_v + 1) : (long)(tess_u + 1) * (tess_v + 1) * npu * npv;
			if(n <= CURVE_MAX_VERTS || (tess_u <= 1 && tess_v <= 1)) break;
			if(tess_u >= tess_v) tess_u--; else tess_v--;
		}
		nverts = is_spline ? (npu * tess_u + 1) * (npv * tess_v + 1) : (tess_u + 1) * (tess_v + 1) * npu * npv;
		nquads = npu * tess_u * npv * tess_v;
		curve_out = grow(curve_out, &curve_out_cap, (size_t)nverts, sizeof(DecVertex));
		curve_idx = grow(curve_idx, &curve_idx_cap, (size_t)nquads * 6, sizeof(u16));
		curve_cols = grow(curve_cols, &curve_cols_cap, is_spline ? (size_t)nu : (size_t)(tess_v + 1) * 4, sizeof(CurveColumn));
		if(!curve_out || !curve_idx || !curve_cols) goto advance;

		/* El sombreado de entorno usa la normal aun sin luces (gpu/probe exp143) */
		cx.sample_nrm = f.nrm || (ge.cmd[GE_LIGHTINGENABLE] & 1) || (ge.cmd[GE_TEXMAPMODE] & 3) == 2;
		cx.sample_col = f.col != 0;
		cx.sample_tex = f.tc != 0;
		cx.facing = ge.cmd[GE_PATCHFACING] & 1;
		cx.pts = curve_pts;
		cx.defcolor = curve_pts[0]->color0;

		op = curve_idx;
		if(is_spline){
			int gnu = npu * tess_u + 1, gnv = npv * tess_v + 1, iu, iv, c, r;
			for(iv = 0; iv < gnv; iv++){
				SplineParam pv = spline_param_at(iv, tess_v, npv, type_v);
				for(c = 0; c < nu; c++){
					int idx[4];
					for(r = 0; r < 4; r++) idx[r] = (pv.seg + r) * nu + c;
					column_eval(&cx, &curve_cols[c], idx, 1, 0, pv.alpha);
				}
				for(iu = 0; iu < gnu; iu++){
					SplineParam pu = spline_param_at(iu, tess_u, npu, type_u);
					row_eval(&cx, &curve_cols[pu.seg], &curve_out[iv * gnu + iu], 1, 0, pu.alpha,
					         (float)pu.seg + (float)pu.k * (1.0f / 256.0f), (float)pv.seg + (float)pv.k * (1.0f / 256.0f));
				}
			}
			build_index(&op, npu * tess_u, npv * tess_v, prim_type, 0);
		} else {
			int pu, pv, tu, tv, c, r, nvpp = (tess_u + 1) * (tess_v + 1);
			for(pu = 0; pu < npu; pu++)
				for(pv = 0; pv < npv; pv++){
					int base = pv * 3 * nu + pu * 3, patch = pv * npu + pu;
					for(tv = 0; tv <= tess_v; tv++){
						int kv = bezier_param256(tv, tess_v);
						for(c = 0; c < 4; c++){
							int idx[4];
							for(r = 0; r < 4; r++) idx[r] = base + r * nu + c;
							column_eval(&cx, &curve_cols[tv * 4 + c], idx, 0, kv, NULL);
						}
					}
					for(tu = 0; tu <= tess_u; tu++){
						int ku = bezier_param256(tu, tess_u);
						for(tv = 0; tv <= tess_v; tv++)
							row_eval(&cx, &curve_cols[tv * 4], &curve_out[tv * (tess_u + 1) + tu + nvpp * patch], 0, ku, NULL,
							         (float)pu + (float)ku * (1.0f / 256.0f),
							         (float)pv + (float)bezier_param256(tv, tess_v) * (1.0f / 256.0f));
					}
				}
			for(pu = 0; pu < npu; pu++)
				for(pv = 0; pv < npv; pv++)
					build_index(&op, tess_u, tess_v, prim_type, (pv * npu + pu) * nvpp);
		}

		/* Los vértices generados: uv, color, normal y posición float, con
		   índices de 16 bits y el modo through del original */
		vformat_setup(&gf, (3u << 0) | (7u << 2) | (3u << 5) | (3u << 7) | (2u << 11) | (ge.cmd[GE_VERTEXTYPE] & 0x800000u));
		compute_transform_state(&gf);
		memset(&vr, 0, sizeof(vr));
		vr.f = &gf;
		vr.list = curve_out;
		vr.idx_list = curve_idx;
		vr.use_indices = 1;
		vr.upper = (u32)(nverts - 1);
		/* Con índices repetidos se leen todos una vez en orden */
		vr.use_cache = (int)(op - curve_idx) > nverts;
		if(vr.use_cache){
			if(nverts > vcache_cap){
				GeClipVertex *nc = realloc(vcache, (size_t)nverts * sizeof(GeClipVertex));
				if(nc){ vcache = nc; vcache_cap = nverts; }
				else vr.use_cache = 0;
			}
			if(vr.use_cache){
				vr.cache = vcache;
				for(i = 0; i < nverts; i++) read_raw(&vr, (u32)i, &vr.cache[i]);
			}
		}
		if(op > curve_idx) submit_primitive(&vr, prims[prim_type], (int)(op - curve_idx));
	}

advance:
	if(f.idx) ge.iaddr = iaddr + (u32)num_points * idx_size[f.idx];
	else if(nu >= 4 && nv >= 4) ge.vaddr = vaddr + (u32)num_points * f.size;
}

static void draw_curve(u32 arg, int is_spline){
	int phase = prof_ge_enter(GEF_CURVAS);
	ge_stats.curves++;
	submit_curve(arg, is_spline);
	prof_ge_leave(phase);
}

void ge_draw_bezier(u32 arg){ draw_curve(arg, 0); }
void ge_draw_spline(u32 arg){ draw_curve(arg, 1); }
