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
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "core/prof.h"
#include "core/opt.h"
#include "hle/hle.h"

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

/* --- ge_vdec_psq (1.2b): conversiones con psq_l -------------------------------
   El Broadway convierte al cargar: psq_l lee un entero de 8 o 16 bits y lo
   deja en float por 2^-escala, con el tipo y la escala de un GQR. Los de 8
   bits salen tal cual (s8/128, u8/128). Los de 16 están en little-endian y
   psq_l los leería al revés: van en dos bytes, el bajo sin signo por 2^-15
   y el alto (con o sin signo) por 2^-7, sumados. Los dos sumandos y el
   resultado tienen como mucho 16 bits significativos, así que todo es exacto
   en float: los mismos bits que (float)x * (1/32768), sin pasar el entero
   por la memoria para convertirlo (el Broadway no tiene fcfid).
   libogc solo guarda GQR0 (que el compilador necesita en 0) al cambiar de
   hilo o atender una excepción: los GQR2-4 se ponen antes de cada lote. */
#ifdef GEKKO
/* GQR2: u8 por 2^-7; GQR3: s8 por 2^-7; GQR4: u8 por 2^-15 (para la carga:
   escala << 24 | tipo << 16, con 4 = u8 y 6 = s8) */
static inline void psq_setup(void){
	__asm__ __volatile__("mtspr 914,%0\n\tmtspr 915,%1\n\tmtspr 916,%2"
	                     :: "r"(0x07040000u), "r"(0x07060000u), "r"(0x0F040000u));
}

/* psq_l del byte p[off] (off constante). Sin operando de memoria: con él
   el lote hace ~11 instrucciones más por vértice (uv16 c4444 p16). Es
   seguro porque batch_range solo lee la RAM de la PSP (escribe en el caché
   de vértices y en la pila) y volatile mantiene el orden con psq_setup. Si
   algún día el lote escribe la RAM de la PSP, hay que ponérselo. */
#define PSQ_LOAD(r, p, off, gqr) __asm__ __volatile__("psq_l %0," #off "(%1),1," #gqr : "=f"(r) : "b"(p))

static inline __attribute__((always_inline)) float psq_u8(const u8 *p){ float r; PSQ_LOAD(r, p, 0, 2); return r; }
static inline __attribute__((always_inline)) float psq_s8(const u8 *p){ float r; PSQ_LOAD(r, p, 0, 3); return r; }
static inline __attribute__((always_inline)) float psq_u16le(const u8 *p){
	float lo, hi;
	PSQ_LOAD(lo, p, 0, 4);
	PSQ_LOAD(hi, p, 1, 2);
	return hi + lo;
}
static inline __attribute__((always_inline)) float psq_s16le(const u8 *p){
	float lo, hi;
	PSQ_LOAD(lo, p, 0, 4);
	PSQ_LOAD(hi, p, 1, 3);
	return hi + lo;
}
#endif

/* raw_comp y raw_weight; psq (constante): con psq_l en el Wii */
static inline __attribute__((always_inline)) float raw_comp_q(const u8 *p, int fmt, int i, const int psq){
#ifdef GEKKO
	if(psq && fmt == 1) return psq_s8(p + i);
	if(psq && fmt == 2) return psq_s16le(p + 2 * i);
#else
	(void)psq;
#endif
	return raw_comp(p, fmt, i);
}

static inline __attribute__((always_inline)) float raw_weight_q(const u8 *p, int fmt, int i, const int psq){
#ifdef GEKKO
	if(psq && fmt == 1) return psq_u8(p + i);
	if(psq && fmt == 2) return psq_u16le(p + 2 * i);
#else
	(void)psq;
#endif
	return raw_weight(p, fmt, i);
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

/* ge_skin_rapido, con la matemática rápida del backend por hardware: las
   matrices de los huesos se mezclan por su peso en float normal y se aplica
   una sola, en vez de la aritmética exacta del GE término a término (12
   productos de 24 bits por hueso y un acumulador con su redondeo). El
   resultado difiere del exacto en el último bit de float24, como el resto de
   la geometría rápida. */
/* Sin morph (component lee igual que raw_weight/raw_comp): el cuerpo, en
   línea, con los formatos como parámetros; el camino por lotes lo usa con
   constantes (mismas expresiones, mismos bits) */
static inline __attribute__((always_inline)) void fast_skin_plain(const VFormat *f, const u8 *in, float *pos, float *nrm,
                                                                  const int wt, const int pfmt, const int nfmt, const int psq){
	float m[12] = { 0.0f }, src[3];
	int b, k, c;
	for(b = 0; b < f->nweights; b++){
		float w = raw_weight_q(in + f->off_w, wt, b, psq);
		const float *bm = ge.bone[b];
		if(w == 0.0f) continue;
		/* Desenrollado, la matriz mezclada se queda en registros (cada
		   elemento hace la misma operación: mismo resultado) */
#pragma GCC unroll 12
		for(k = 0; k < 12; k++) m[k] += w * bm[k];
	}
	/* También desenrollados: así la matriz no pasa por la pila */
	if(pfmt){
#pragma GCC unroll 3
		for(c = 0; c < 3; c++) src[c] = raw_comp_q(in + f->off_pos, pfmt, c, psq);
#pragma GCC unroll 3
		for(c = 0; c < 3; c++) pos[c] = m[9 + c] + src[0] * m[c] + src[1] * m[3 + c] + src[2] * m[6 + c];
	}
	if(nfmt){
#pragma GCC unroll 3
		for(c = 0; c < 3; c++) src[c] = raw_comp_q(in + f->off_nrm, nfmt, c, psq);
#pragma GCC unroll 3
		for(c = 0; c < 3; c++) nrm[c] = ge_dot3_fused(src[0], m[c], src[1], m[3 + c], src[2], m[6 + c]);
	}
}

static void fast_skin(const VFormat *f, const u8 *in, float *pos, float *nrm){
	float m[12] = { 0.0f }, src[3];
	int b, k, c;
	if(f->nmorph == 1){ fast_skin_plain(f, in, pos, nrm, f->wt, f->pos, f->nrm, 0); return; }
	for(b = 0; b < f->nweights; b++){
		float w = component(f, in, f->off_w, b, RD_WEIGHT, f->wt);
		const float *bm = ge.bone[b];
		if(w == 0.0f) continue;
		for(k = 0; k < 12; k++) m[k] += w * bm[k];
	}
	if(f->pos){
		for(c = 0; c < 3; c++) src[c] = component(f, in, f->off_pos, c, RD_COMP, f->pos);
		for(c = 0; c < 3; c++) pos[c] = m[9 + c] + src[0] * m[c] + src[1] * m[3 + c] + src[2] * m[6 + c];
	}
	if(f->nrm){
		for(c = 0; c < 3; c++) src[c] = component(f, in, f->off_nrm, c, RD_COMP, f->nrm);
		for(c = 0; c < 3; c++) nrm[c] = src[0] * m[c] + src[1] * m[3 + c] + src[2] * m[6 + c];
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

/* Lo que hace decode_vertex sin morph ni through, con el formato como
   parámetro (tc, col, nrm, pos, wt; skin = ge_skin_rapido; psq =
   ge_vdec_psq), en línea. Solo escribe lo que read_vertex va a leer: la uv
   si hay, el color si hay, la posición y la normal si hay. */
static inline __attribute__((always_inline)) void decode_plain(const VFormat *f, const u8 *p, DecVertex *d,
                                                               const int tc, const int col, const int nrm,
                                                               const int pos, const int wt, const int psq){
	int i;
	/* Desenrollados: cada valor va directo a su registro (si no, pasan por
	   la pila); mismas operaciones */
	if(tc){
#pragma GCC unroll 2
		for(i = 0; i < 2; i++) d->uv[i] = raw_weight_q(p + f->off_tc, tc, i, psq);
	}
	if(col >= 4) d->color0 = color_to_8888(col, p + f->off_col);
	else if(col) d->color0 = 0;
	if(wt){
		fast_skin_plain(f, p, d->pos, d->nrm, wt, pos, nrm, psq);
		return;
	}
#pragma GCC unroll 3
	for(i = 0; i < 3; i++){
		d->pos[i] = raw_comp_q(p + f->off_pos, pos, i, psq);
		if(pos == 3) d->pos[i] = clean_nan_inf(d->pos[i]);
		if(nrm) d->nrm[i] = raw_comp_q(p + f->off_nrm, nrm, i, psq);
	}
}

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
		if(ge_fast_math && opt_on[OPT_GE_SKIN_RAPIDO]) fast_skin(f, p, d->pos, d->nrm);
		else ge_skin(f, p, d->pos, d->nrm);
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
	return ge_fast_math ? ge_dot3_fused(a[0], b[0], a[1], b[1], a[2], b[2]) : ge_dot(a, b);
}

static inline float vnormalize(float *v){
	float d2, d, r;
	if(!ge_fast_math) return ge_normalize(v);
	d2 = ge_dot3_fused(v[0], v[0], v[1], v[1], v[2], v[2]);
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

/* noinline (como lighting_process_fast): una sola copia compilada para el
   camino por lotes y el de uno en uno, con las mismas fusiones */
static float __attribute__((noinline)) ge_shade_map_coord(int l, const float *mpos, const float *wn, float n_rsqrt,
                                                          const float *view_dir){
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
   factores de 8 bits del GE se aproximan con su producto en float. noinline:
   una sola copia compilada para el camino por lotes y el de uno en uno (las
   sumas de productos dan el mismo bit). No noipa: sin saber qué registros
   usa, el bucle del lote guarda más (~25 instrucciones por vértice); las
   dos llamadas pasan lo mismo, así que comparten el mismo clon */
static void __attribute__((noinline)) lighting_process_fast(GeVertex *v, const float *mpos, const float *wn, float n_rsqrt,
                                                            const LightState *s){
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

/* ge_mtx_cache: (mundo * vista) * proyección y la dirección de vista solo
   dependen de las tres matrices. Con las mismas entradas la aritmética del
   GE da los mismos bits, así que se guardan y se comparan las matrices en
   cada llamada en vez de rehacer dos productos 4x4 exactos (~5000
   instrucciones del Broadway cada vez). */
static struct {
	int valid;
	float world[12], view[12], proj[16];
	float worldview[16], matrix[16], view_dir[3];
} mc;

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
		if(opt_on[OPT_GE_MTX_CACHE] && mc.valid && !memcmp(mc.world, ge.world, sizeof(mc.world)) &&
		   !memcmp(mc.view, ge.view, sizeof(mc.view)) && !memcmp(mc.proj, ge.proj, sizeof(mc.proj))){
			memcpy(vd, mc.view_dir, sizeof(vd));
			memcpy(worldview, mc.worldview, sizeof(worldview));
			memcpy(ts.matrix, mc.matrix, sizeof(ts.matrix));
			ge_stats.mtx_reused++;
		} else {
			/* El observador en el infinito por +z de vista, normalizado como el GE */
			vd[0] = ge.view[2]; vd[1] = ge.view[5]; vd[2] = ge.view[8];
			if(ge_normalize(vd) == 0.0f){ vd[0] = 0.0f; vd[1] = 0.0f; vd[2] = 1.0f; }

			to4x4(ge.view, view);
			to4x4(ge.world, world);
			ge_combine_matrices(worldview, world, view);
			/* (mundo * vista) * proyección, como el GE */
			ge_combine_matrices(ts.matrix, worldview, ge.proj);
			ge_stats.mtx_combined++;
			memcpy(mc.world, ge.world, sizeof(mc.world));
			memcpy(mc.view, ge.view, sizeof(mc.view));
			memcpy(mc.proj, ge.proj, sizeof(mc.proj));
			memcpy(mc.view_dir, vd, sizeof(vd));
			memcpy(mc.worldview, worldview, sizeof(worldview));
			memcpy(mc.matrix, ts.matrix, sizeof(ts.matrix));
			mc.valid = 1;
		}
		memcpy(ts.light.view_dir, vd, sizeof(vd));

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

/* ge_f2i(ge_floorf(f)) sin volver a convertir el entero ya redondeado; en
   *fi, el mismo entero en float (1 si es exacto: |f| < 2^23) */
static inline int floor_to_int(float f, float *fi, int *exact){
	if(f > -8388608.0f && f < 8388608.0f){
		float t = ge_floorf(f);
		*fi = t;
		*exact = 1;
		return (int)t;
	}
	*exact = 0;
	return ge_f2i(f);
}

/* El offset de pantalla en float (cambia muy de vez en cuando) */
static u32 off_key[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
static float off_val[2];
static inline float offset_float(int k, u32 o){
	if(o != off_key[k]){ off_key[k] = o; off_val[k] = (float)o; }
	return off_val[k];
}

/* En línea: en el bucle de cada vértice, sin guardar nada en la pila para la llamada */
static inline __attribute__((always_inline)) int round_to_screen(float sx, float sy, float sz, const float *clip,
                                                                 int depth_clip, int always, GeVertex *v){
	int outside = 0;
	if(depth_clip){
		if((always || !(clip[2] < -clip[3])) && (sx >= SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0))
			outside = 1;
		if(sz < 0.0f) sz = 0.0f;
		else if(sz > 65535.0f) sz = 65535.0f;
	} else if(sx > SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0 || sz < 0.0f || sz >= 65536.0f)
		outside = 1;
	{
		u32 ox = ge.cmd[GE_OFFSETX] & 0xFFFF, oy = ge.cmd[GE_OFFSETY] & 0xFFFF;
		float fx, fy;
		int ex, ey, ix = floor_to_int(sx * 16.0f, &fx, &ex), iy = floor_to_int(sy * 16.0f, &fy, &ey);
		v->x = (int)((u32)ix - ox);
		v->y = (int)((u32)iy - oy);
		v->z = (u16)(u32)ge_f2i(sz);
		/* |i| < 2^23 y el offset < 2^16: la resta en float es exacta */
		v->fx = ex ? fx - offset_float(0, ox) : (float)v->x;
		v->fy = ey ? fy - offset_float(1, oy) : (float)v->y;
		/* Dentro del rango, sz ya es el entero (nunca -0) */
		v->fz = sz > 0.0f && sz <= 65535.0f ? sz : (float)v->z;
		/* Aquí y no al volver: sin guardar nada en la pila para la llamada */
		v->clip_flags = (u16)ge_clip_flags_xy(clip, v->x, v->y);
	}
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

/* El cuerpo de read_vertex, en línea: fast es constante en cada uso (el
   camino por lotes de ge_vdec_esp usa exactamente estas mismas expresiones,
   así que el compilador hace las mismas operaciones y salen los mismos bits) */
static inline __attribute__((always_inline)) void read_vertex_body(const VFormat *f, const DecVertex *d, GeClipVertex *out, const int fast){
	GeVertex *v = &out->v;
	float pos[3], normal[3];
	int i;

	/* Todos los campos se escriben más abajo en todos los caminos */
	memcpy(pos, d->pos, sizeof(pos));

	if(f->tc){ last_uv[0] = d->uv[0]; last_uv[1] = d->uv[1]; }
	{
		float tc[2];
		tc[0] = last_uv[0]; tc[1] = last_uv[1];
		if(ts.ge_uv_scale && fast){
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

	v->color0 = f->col ? d->color0 : material_ambient_rgba();
	v->color1 = 0;

	if(ts.enable_transform){
		float sx, sy, sz, worldnormal[3] = { 0, 0, 0 }, n_rsqrt = 1.0f;
		if(fast){
			/* Float normal: la profundidad y los bordes pueden variar en 1
			   respecto al GE, pero igual en todas las pasadas */
			const float *m = ts.matrix;
			float iw;
#pragma GCC unroll 4
			for(i = 0; i < 4; i++) out->clip[i] = ge_dot3_fused(pos[0], m[i], pos[1], m[4 + i], pos[2], m[8 + i]) + m[12 + i];
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
			ge_vertex_mark_outside(v);
			return;
		}

		if(ts.enable_fog && ts.fog_ge && !fast){
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
			float f4 = ge_dot3_fused(ts.pos_to_fog[0], pos[0], ts.pos_to_fog[1], pos[1], ts.pos_to_fog[2], pos[2]) + ts.pos_to_fog[3];
			v->fogdepth = (float)ge_fog_factor(f4) * (1.0f / 256.0f);
		}

		/* La normal (la última que llegó) solo si se usa: luces o uv a partir
		   de ella */
		if(ts.light.uses_world_normal || ts.uv_gen_mode == 1){
			memcpy(normal, last_normal, sizeof(normal));
			if(ts.negate_normals) for(i = 0; i < 3; i++) normal[i] = -normal[i];
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
			if(len2 > 0.0f && isfinite(len2)) n_rsqrt = fast ? ge_fast_rsqrt(len2) : ge_rsqrt(len2);
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
			if(fast){
				const float *m = ge.tgen;
				for(i = 0; i < 3; i++) r[i] = ge_dot3_fused(src[0], m[i], src[1], m[3 + i], src[2], m[6 + i]) + m[9 + i];
			} else for(i = 0; i < 3; i++) r[i] = texgen_component(src, ge.tgen, i);
			v->s = r[0]; v->t = r[1]; v->q = r[2];
		} else if(ts.uv_gen_mode == 2){
			v->s = ge_shade_map_coord((int)(ge.cmd[GE_TEXSHADELS] & 3), pos, worldnormal, n_rsqrt, ts.light.view_dir);
			v->t = ge_shade_map_coord((int)((ge.cmd[GE_TEXSHADELS] >> 8) & 3), pos, worldnormal, n_rsqrt, ts.light.view_dir);
		}

		if(ts.enable_lighting){
			ge_stats.vertices_lit++;
			ge_stats.lights += ts_nlights;
			if(fast) lighting_process_fast(v, pos, worldnormal, n_rsqrt, &ts.light);
			else lighting_process(v, pos, worldnormal, n_rsqrt, &ts.light);
		}
	} else {
		out->clip[0] = out->clip[1] = out->clip[2] = out->clip[3] = 0.0f;
		ge_vertex_set_xyz(v, ge_f2i(pos[0] * 16.0f), ge_f2i(pos[1] * 16.0f), (u16)(u32)ge_f2i(pos[2]));
		ge_set_clip_flags(out);
		v->clipw = 1.0f;
		v->fogdepth = 1.0f;
	}
}

static void read_vertex(const VFormat *f, const DecVertex *d, GeClipVertex *out){
	if(ge_fast_math) read_vertex_body(f, d, out, 1);
	else read_vertex_body(f, d, out, 0);
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
	if(clip_to_screen(tmp.clip, &tmp.v)) ge_vertex_mark_outside(&tmp.v);
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
	if(clip_to_screen(d->clip, &d->v)) ge_vertex_mark_outside(&d->v);
	d->v.clipw = d->clip[3];
}

/* Rectángulo con proyección de textura: como triángulos */
static void add_triangle_rect(const GeVertex *v0, const GeVertex *v1){
	GeVertex buf[4], *tl = &buf[0], *tr = &buf[1], *bl = &buf[2], *br = &buf[3];
	int i;
	buf[0] = *v1; buf[0].x = v0->x; buf[0].y = v0->y; buf[0].fx = v0->fx; buf[0].fy = v0->fy;
	buf[0].s = v0->s; buf[0].t = v0->t; buf[0].q = v0->q;
	buf[1] = *v1; buf[1].x = v0->x; buf[1].fx = v0->fx;
	buf[1].s = v0->s; buf[1].q = v0->q;
	buf[2] = *v1; buf[2].y = v0->y; buf[2].fy = v0->fy;
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
			ge_vertex_set_xyz(&h0, v0->v.x + (v1->v.x - v0->v.x) / 2, h0.y, h0.z);
			h0.s = v0->v.s + (v1->v.s - v0->v.s) / 2;
			ge_vertex_set_xyz(&h1, v0->v.x + (v1->v.x - v0->v.x) / 2, v0->v.y, h1.z);
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

/* add_tri_flat con el área ya comprobada (positiva) */
static void add_tri_ccw(const GeClipVertex *a, const GeClipVertex *b, const GeClipVertex *c, const GeClipVertex *provoking){
	if(!(ge.cmd[GE_SHADEMODE] & 1)){
		GeVertex c2 = c->v;
		c2.color0 = provoking->v.color0;
		c2.color1 = provoking->v.color1;
		ge_raster_triangle_ccw(&a->v, &b->v, &c2);
	} else ge_raster_triangle_ccw(&a->v, &b->v, &c->v);
}

/* Los descartes de un triángulo antes de recortar (no dependen del orden de
   los vértices); 1 si se descarta. mask: hay que recortar con el plano
   cercano */
static int triangle_rejected(const GeClipVertex *v0, const GeClipVertex *v1, const GeClipVertex *v2, int *maskp){
	int mask = 0;
	if(!ge_through()){
		int depth_clip = ge.cmd[GE_DEPTHCLAMPENABLE] & 1;
		ge_tri_flags |= GE_TRI_OUTSIDE;   /* hasta que pase los descartes */
		if(outside_range_before_clip(v0, depth_clip) || outside_range_before_clip(v1, depth_clip) ||
		   outside_range_before_clip(v2, depth_clip)) return 1;
		/* Con w negativa en todos también se descarta */
		if(v0->clip[3] < 0.0f && v1->clip[3] < 0.0f && v2->clip[3] < 0.0f) return 1;
		mask |= calc_clip_mask(v0->clip);
		mask |= calc_clip_mask(v1->clip);
		mask |= calc_clip_mask(v2->clip);
		if(outside_mask(v0->clip) & outside_mask(v1->clip) & outside_mask(v2->clip)) return 1;
		/* Sin recorte de profundidad, la parte detrás del plano cercano se
		   dibuja con la z extrapolada; un vértice detrás de la cámara
		   (w <= 0) descarta el triángulo */
		if(!depth_clip){
			if(!(v0->clip[3] > 0.0f && v1->clip[3] > 0.0f && v2->clip[3] > 0.0f)) return 1;
			mask = 0;
		}
		ge_tri_flags &= ~GE_TRI_OUTSIDE;
	}
	*maskp = mask;
	return 0;
}

/* ge_ensamblar_rapido: los mismos descartes con las banderas de cada
   vértice (ge_clip_flags), que solo dependen de sus coordenadas: en una tira
   cada vértice se comprobaba en tres triángulos */
static inline int triangle_rejected_flags(const GeClipVertex *v0, const GeClipVertex *v1, const GeClipVertex *v2, int *maskp){
	int mask = 0;
	if(!ge_through()){
		u32 f0 = v0->v.clip_flags, f1 = v1->v.clip_flags, f2 = v2->v.clip_flags;
		u32 any = f0 | f1 | f2, all = f0 & f1 & f2;
		int depth_clip = ge.cmd[GE_DEPTHCLAMPENABLE] & 1;
		ge_tri_flags |= GE_TRI_OUTSIDE;
		/* outside_range_before_clip: con recorte de profundidad, solo el que
		   no está más allá del plano cercano */
		if(any & GE_CF_OUTSIDE){
			const u32 m = GE_CF_OUTSIDE | GE_CF_NEAR;
			if(!depth_clip || (f0 & m) == GE_CF_OUTSIDE || (f1 & m) == GE_CF_OUTSIDE || (f2 & m) == GE_CF_OUTSIDE) return 1;
		}
		if(all & GE_CF_WNEG) return 1;
		if(any & GE_CF_NEAR) mask = -1;
		/* outside_mask: x e y solo si cull_xy */
		if(all & (cull_xy ? GE_CF_XP | GE_CF_XN | GE_CF_YP | GE_CF_YN | GE_CF_ZP | GE_CF_ZN : GE_CF_ZP | GE_CF_ZN)) return 1;
		if(!depth_clip){
			if(!(all & GE_CF_WPOS)) return 1;
			mask = 0;
		}
		ge_tri_flags &= ~GE_TRI_OUTSIDE;
	}
	*maskp = mask;
	return 0;
}

/* ge_verificar: las banderas guardadas tienen que ser las de las coordenadas
   del vértice, y los descartes los mismos que los de referencia (resultado,
   recorte y ge_tri_flags). Se queda el resultado rápido. */
static u32 verif_tris, verif_tris_bad, verif_tris_logged;

static void __attribute__((noinline)) verify_rejected(const GeClipVertex *v0, const GeClipVertex *v1, const GeClipVertex *v2,
                                                      int flags_before, int mask_before, int r, int mask){
	const GeClipVertex *v[3] = { v0, v1, v2 };
	int flags = ge_tri_flags, ref_mask = mask_before, ref_r, ref_flags, i, bad = 0;
	for(i = 0; i < 3; i++) if(v[i]->v.clip_flags != ge_clip_flags(v[i])) bad |= 1 << i;
	ge_tri_flags = flags_before;
	ref_r = triangle_rejected(v0, v1, v2, &ref_mask);
	ref_flags = ge_tri_flags;
	ge_tri_flags = flags;
	verif_tris++;
	if(!bad && ref_r == r && ref_mask == mask && ref_flags == flags) return;
	verif_tris_bad++;
	if(verif_tris_logged++ < 24)
		hle_log_quiet("[VERIF] descartes: vertices %d %d %d banderas %03X/%03X %03X/%03X %03X/%03X x %d %d %d through %d "
		              "profundidad %u: %d/%d recorte %d/%d triangulo %X/%X\n", !!(bad & 1), !!(bad & 2), !!(bad & 4),
		              (unsigned)ge_clip_flags(v0), (unsigned)v0->v.clip_flags, (unsigned)ge_clip_flags(v1), (unsigned)v1->v.clip_flags,
		              (unsigned)ge_clip_flags(v2), (unsigned)v2->v.clip_flags, v0->v.x, v1->v.x, v2->v.x, ge_through(),
		              (unsigned)(ge.cmd[GE_DEPTHCLAMPENABLE] & 1), ref_r, r, ref_mask, mask, (unsigned)ref_flags, (unsigned)flags);
}

static inline __attribute__((always_inline)) int triangle_rejected_fast(const GeClipVertex *v0, const GeClipVertex *v1,
                                                                        const GeClipVertex *v2, int *maskp){
	int flags_before = ge_tri_flags, mask_before = *maskp;
	int r = triangle_rejected_flags(v0, v1, v2, maskp);
	if(opt_on[OPT_GE_VERIF]) verify_rejected(v0, v1, v2, flags_before, mask_before, r, *maskp);
	return r;
}

static inline int triangle_rejected_any(const GeClipVertex *v0, const GeClipVertex *v1, const GeClipVertex *v2, int *maskp){
	if(opt_on[OPT_GE_ENSAMBLAR]) return triangle_rejected_fast(v0, v1, v2, maskp);
	return triangle_rejected(v0, v1, v2, maskp);
}

static void process_triangle(const GeClipVertex *v0, const GeClipVertex *v1, const GeClipVertex *v2,
                             const GeClipVertex *provoking, int reversed){
	int mask = 0, i, num_outside = 0, num_tris = 0, outside[3];
	const GeClipVertex *src[3], *tris[2][3];
	GeClipVertex made[2];

	if(triangle_rejected_any(v0, v1, v2, &mask)) return;

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
int ge_turbo;

static GeClipVertex data_[4];
static int data_index_;
static u32 prev_prim_ = GE_PRIM_POINTS;
static int is_imm_draw;

/* 1 si el rasterizador dibujaría (a, b, c) (área positiva, ge_raster_triangle),
   -1 si dibujaría (c, b, a) y 0 si ninguno. Con x e y pequeñas (GE_CF_SMALL)
   en double desde fx y fy, exacto y sin la multiplicación de 64 bits, que
   en el Broadway son varias. Si no, como el rasterizador: el área de (c, b,
   a) es -area también al desbordar (el mismo polinomio módulo 2^64), y con
   0 o INT64_MIN ninguna de las dos es positiva. */
static inline int triangle_area_sign(const GeVertex *a, const GeVertex *b, const GeVertex *c){
	if(a->clip_flags & b->clip_flags & c->clip_flags & GE_CF_SMALL){
		double d01x = (double)a->fx - b->fx, d01y = (double)a->fy - b->fy;
		double d02x = (double)a->fx - c->fx, d02y = (double)a->fy - c->fy;
		double area = d01x * d02y - d01y * d02x;
		return area > 0.0 ? 1 : area < 0.0 ? -1 : 0;
	} else {
		s64 d01x = (s64)a->x - b->x, d01y = (s64)a->y - b->y;
		s64 d02x = (s64)a->x - c->x, d02y = (s64)a->y - c->y;
		s64 area = d01x * d02y - d01y * d02x;
		return area > 0 ? 1 : area < 0 && area != INT64_MIN ? -1 : 0;
	}
}

/* v: los tres vértices de la ventana (en data_ o, con ge_ensamblar_rapido,
   en el caché de la llamada) */
static void send_triangle(int cull, const GeClipVertex *const *v, int provoking, int reversed){
	u32 drawn = ge_stats.primitives;
	ge_tri_flags = 0;
	if(cull == CULL_OFF){
		int mask = 0;
		ge_stats.tris_cull_off++;
		if(!opt_on[OPT_GE_TRI_RAPIDO]){
			process_triangle(v[0], v[1], v[2], v[provoking], reversed);
			process_triangle(v[2], v[1], v[0], v[provoking], !reversed);
		} else if(triangle_rejected_any(v[0], v[1], v[2], &mask)){
			/* ge_tri_rapido: los descartes no dependen del orden: una vez */
		} else if(mask){
			process_triangle(v[0], v[1], v[2], v[provoking], reversed);
			process_triangle(v[2], v[1], v[0], v[provoking], !reversed);
		} else if(opt_on[OPT_GE_ENSAMBLAR]){
			/* ge_ensamblar_rapido: el área se calcula aquí y el rasterizador
			   no la vuelve a mirar */
			int sign = triangle_area_sign(&v[0]->v, &v[1]->v, &v[2]->v);
			if(sign > 0) add_tri_ccw(v[0], v[1], v[2], v[provoking]);
			else if(sign < 0) add_tri_ccw(v[2], v[1], v[0], v[provoking]);
		} else {
			/* Sin recortar, el rasterizador solo dibuja el sentido con área
			   positiva: el otro (o ninguno con área 0) se iba a descartar */
			s64 d01x = (s64)v[0]->v.x - v[1]->v.x, d01y = (s64)v[0]->v.y - v[1]->v.y;
			s64 d02x = (s64)v[0]->v.x - v[2]->v.x, d02y = (s64)v[0]->v.y - v[2]->v.y;
			if(d01x * d02y - d01y * d02x > 0) add_tri_flat(v[0], v[1], v[2], v[provoking]);
			else add_tri_flat(v[2], v[1], v[0], v[provoking]);
		}
	} else if(cull == CULL_CW){
		process_triangle(v[2], v[1], v[0], v[provoking], !reversed);
	} else {
		process_triangle(v[0], v[1], v[2], v[provoking], reversed);
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
	int batch;                 /* sin índices: todos ya calculados en cache */
	u32 lower, upper;
	const DecVertex *list;     /* vértices ya decodificados (curvas) */
	const u16 *idx_list;       /* sus índices */
	GeClipVertex *cache;
} VSource;

static GeClipVertex *vcache;
static int vcache_cap;

/* Copia de un vértice palabra a palabra (una copia de estructura acababa
   en una llamada a memcpy por vértice) */
typedef u32 __attribute__((may_alias)) cv_word;
typedef char cv_size_ok[sizeof(GeClipVertex) == 68 ? 1 : -1];
static inline void copy_cv(GeClipVertex *d, const GeClipVertex *s){
	cv_word *o = (cv_word *)(void *)d;
	const cv_word *i = (const cv_word *)(const void *)s;
	o[0] = i[0]; o[1] = i[1]; o[2] = i[2]; o[3] = i[3]; o[4] = i[4]; o[5] = i[5]; o[6] = i[6]; o[7] = i[7];
	o[8] = i[8]; o[9] = i[9]; o[10] = i[10]; o[11] = i[11]; o[12] = i[12]; o[13] = i[13]; o[14] = i[14];
	o[15] = i[15]; o[16] = i[16];
}

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
	if(s->list) d = s->list[index];
	else if(s->zero) memset(&d, 0, sizeof(d));
	else decode_vertex(s->f, mem_ptr_r(s->vaddr + index * s->f->size, s->f->size), &d);
	prof_ge_enter(GEF_TRANSFORMAR);
	read_vertex(s->f, &d, out);
	ge_stats.vertices++;
	if(s->f->through) ge_stats.vertices_through++;
	else if(s->f->wt) ge_stats.vertices_skinned++;
	else if(s->f->nmorph > 1) ge_stats.vertices_morph++;
	prof_ge_leave(phase);
}

static void vsource_read(const VSource *s, int vtx, GeClipVertex *out){
	ge_stats.vertex_reads++;
	if(s->use_indices){
		u32 idx = index_at(s, vtx);
		if(s->use_cache){ copy_cv(out, &s->cache[idx - s->lower]); return; }
		read_raw(s, idx, out);
	} else if(s->batch) copy_cv(out, &s->cache[vtx]);
	else read_raw(s, (u32)vtx, out);
}

/* El vértice vtx para la ventana de triángulos. refs (ge_ensamblar_rapido):
   si ya está calculado en el caché de la llamada (por lotes o con índices
   repetidos), el del caché, sin copiar sus 68 bytes; si no, leído en slot */
static inline const GeClipVertex *vsource_ref(const VSource *s, int vtx, GeClipVertex *slot, int refs){
	if(refs){
		if(s->use_indices){
			if(s->use_cache){
				ge_stats.vertex_reads++;
				return &s->cache[index_at(s, vtx) - s->lower];
			}
		} else if(s->batch){
			ge_stats.vertex_reads++;
			return &s->cache[vtx];
		}
	}
	vsource_read(s, vtx, slot);
	return slot;
}

/* Al acabar la llamada, data_ queda como si cada vértice se hubiera copiado
   a su hueco (lo que sigue con PRIM continuar o el modo inmediato): el caché
   se reutiliza en la siguiente */
static void window_keep(const GeClipVertex **w){
	int i;
	for(i = 0; i < 4; i++){
		if(w[i] != &data_[i]) copy_cv(&data_[i], w[i]);
		w[i] = &data_[i];
	}
}

/* ge_vdec_esp: con la matemática rápida, los vértices de una llamada sin
   índices (o el rango de una con índices repetidos) se calculan todos de una
   vez y en orden, antes de ensamblar: un solo puntero a la RAM, sin cambiar
   de fase del perfil ni pasar por read_raw en cada uno. Mismo orden que al
   leerlos uno a uno (la uv y la normal que arrastra cada vértice no
   cambian) y las mismas expresiones (read_vertex_body), así que salen los
   mismos bits: tests/test_vdec.c. Solo la RAM: leer de la VRAM puede bajar
   un framebuffer. */
/* ge_verificar: cada lote se calcula también uno a uno (decode_vertex
   y read_vertex, como sin ge_vdec_esp) y se compara campo a campo; lo
   distinto va al registro con los bits y los bytes del vértice. Para
   encontrar en el Wii lo que las pruebas del PC no ven. */
static GeClipVertex *vref;
static int vref_cap;
static u32 verif_vertices, verif_bad, verif_logged;

static void verify_begin(const VSource *s, const u8 *p, int n){
	float uv[2], nrm[3];
	int i;
	if(n > vref_cap){
		GeClipVertex *nr = realloc(vref, (size_t)n * sizeof(GeClipVertex));
		if(!nr){ vref_cap = 0; return; }
		vref = nr;
		vref_cap = n;
	}
	memcpy(uv, last_uv, sizeof(uv));
	memcpy(nrm, last_normal, sizeof(nrm));
	for(i = 0; i < n; i++, p += s->f->size){
		DecVertex d;
		decode_vertex(s->f, p, &d);
		read_vertex(s->f, &d, &vref[i]);
	}
	/* El lote empieza con lo que dejó la llamada anterior, como el de referencia */
	memcpy(last_uv, uv, sizeof(uv));
	memcpy(last_normal, nrm, sizeof(nrm));
}

static void verify_end(const VFormat *f, const u8 *p, int n, const GeClipVertex *dst){
	static const char *const names[18] = { "clip0", "clip1", "clip2", "clip3", "s", "t", "q", "clipw", "color0", "color1",
	                                       "x", "y", "z", "niebla", "fx", "fy", "fz", "banderas" };
	int i, k;
	if(n > vref_cap) return;
	for(i = 0; i < n; i++){
		const GeClipVertex *a = &vref[i], *b = &dst[i];
		u32 va[18], vb[18];
		memcpy(va, a->clip, 16); memcpy(vb, b->clip, 16);
		memcpy(&va[4], &a->v.s, 4); memcpy(&vb[4], &b->v.s, 4);
		memcpy(&va[5], &a->v.t, 4); memcpy(&vb[5], &b->v.t, 4);
		memcpy(&va[6], &a->v.q, 4); memcpy(&vb[6], &b->v.q, 4);
		memcpy(&va[7], &a->v.clipw, 4); memcpy(&vb[7], &b->v.clipw, 4);
		va[8] = a->v.color0; vb[8] = b->v.color0;
		va[9] = a->v.color1; vb[9] = b->v.color1;
		va[10] = (u32)a->v.x; vb[10] = (u32)b->v.x;
		va[11] = (u32)a->v.y; vb[11] = (u32)b->v.y;
		va[12] = a->v.z; vb[12] = b->v.z;
		memcpy(&va[13], &a->v.fogdepth, 4); memcpy(&vb[13], &b->v.fogdepth, 4);
		memcpy(&va[14], &a->v.fx, 4); memcpy(&vb[14], &b->v.fx, 4);
		memcpy(&va[15], &a->v.fy, 4); memcpy(&vb[15], &b->v.fy, 4);
		memcpy(&va[16], &a->v.fz, 4); memcpy(&vb[16], &b->v.fz, 4);
		va[17] = a->v.clip_flags; vb[17] = b->v.clip_flags;
		verif_vertices++;
		if(!memcmp(va, vb, sizeof(va))) continue;
		verif_bad++;
		if(verif_logged < 24){
			char line[400];
			size_t len = (size_t)snprintf(line, sizeof(line), "[VERIF] vtype %06X vertice %d:", (unsigned)f->vtype, i);
			for(k = 0; k < 18 && len + 40 < sizeof(line); k++)
				if(va[k] != vb[k]) len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %08X/%08X", names[k],
				                                           (unsigned)va[k], (unsigned)vb[k]);
			len += (size_t)snprintf(line + len, sizeof(line) - len, " bytes");
			for(k = 0; k < (int)f->size && k < 40 && len + 4 < sizeof(line); k++)
				len += (size_t)snprintf(line + len, sizeof(line) - len, " %02X", p[(u32)i * f->size + (u32)k]);
			hle_log_quiet("%s\n", line);
			verif_logged++;
		}
	}
}

void ge_vdec_verif_report(char *buf, size_t size){
	snprintf(buf, size, "Verificacion (ge_verificar): vertices %u comparados, %u distintos; descartes de triangulos "
	         "%u comparados, %u distintos; estado guardado (ge_estado_sucio): uv y morph %u/%u, transformacion %u/%u, "
	         "rasterizador %u/%u (comparados/distintos)", (unsigned)verif_vertices, (unsigned)verif_bad, (unsigned)verif_tris,
	         (unsigned)verif_tris_bad, (unsigned)ge_estado_verif[0][0], (unsigned)ge_estado_verif[0][1],
	         (unsigned)ge_estado_verif[1][0], (unsigned)ge_estado_verif[1][1], (unsigned)ge_estado_verif[2][0],
	         (unsigned)ge_estado_verif[2][1]);
	memset(ge_estado_verif, 0, sizeof(ge_estado_verif));
	verif_vertices = verif_bad = 0;
	verif_tris = verif_tris_bad = 0;
}

static int batch_range(const VSource *s, u32 first, int n, GeClipVertex *dst){
	const VFormat *f = s->f;
	const u8 *p;
	u32 size = f->size, off;
	int i, psq = 0;
	if(!opt_on[OPT_GE_VDEC_ESP] || !ge_fast_math || f->through || f->nmorph > 1 || s->zero || s->list || n <= 0)
		return 0;
	off = ((s->vaddr + first * size) & PSP_ADDR_MASK) - PSP_RAM_BASE;
	if(off >= psp_mem.ram_size || (u64)size * (u32)n > psp_mem.ram_size - off) return 0;
	p = psp_mem.ram + off;
	if(opt_on[OPT_GE_VERIF]) verify_begin(s, p, n);
#ifdef GEKKO
	psq = opt_on[OPT_GE_VDEC_PSQ];
	if(psq) psq_setup();
#endif
	/* Formatos con decodificador propio (los del censo de [TIEMPOS]: DBZ usa
	   casi solo estos), con y sin psq_l; el resto, con el genérico */
	/* El formato de f es el del caso (el switch lo garantiza): dicho así (en
	   cada vuelta: las escrituras en dst podrían cambiar f, para el
	   compilador), quita las comprobaciones de f en read_vertex_body y sabe
	   que no lee la normal que no se decodificó */
#define BATCH1(TC, COL, NRM, POS, WT, Q) \
	for(i = 0; i < n; i++, p += size){ \
		DecVertex d; \
		if(f->tc != (TC) || f->col != (COL) || f->nrm != (NRM) || f->pos != (POS) || f->wt != (WT)) __builtin_unreachable(); \
		d.color0 = 0; \
		decode_plain(f, p, &d, TC, COL, NRM, POS, WT, Q); \
		read_vertex_body(f, &d, &dst[i], 1); \
	}
#define BATCH(TC, COL, NRM, POS, WT) do { if(psq){ BATCH1(TC, COL, NRM, POS, WT, 1) } else { BATCH1(TC, COL, NRM, POS, WT, 0) } } while(0)
	switch((f->vtype & 0x7FF) | (f->wt && !opt_on[OPT_GE_SKIN_RAPIDO] ? 0x800 : 0)){
	case 0x11A: BATCH(2, 6, 0, 2, 0); break;    /* uv16 c4444 p16 */
	case 0x11E: BATCH(2, 7, 0, 2, 0); break;    /* uv16 c8888 p16 */
	case 0x19F: { BATCH1(3, 7, 0, 3, 0, 0) } break;    /* uvf c8888 pf: nada que convertir */
	case 0x301: BATCH(1, 0, 0, 2, 1); break;    /* uv8 p16, pesos de 8 bits */
	case 0x321: BATCH(1, 0, 1, 2, 1); break;    /* uv8 n8 p16, pesos de 8 bits */
	case 0x341: BATCH(1, 0, 2, 2, 1); break;    /* uv8 n16 p16, pesos de 8 bits */
	default:
		for(i = 0; i < n; i++, p += size){
			DecVertex d;
			decode_vertex(f, p, &d);
			read_vertex_body(f, &d, &dst[i], 1);
		}
		break;
	}
#undef BATCH
#undef BATCH1
	if(opt_on[OPT_GE_VERIF]) verify_end(f, psp_mem.ram + off, n, dst);
	ge_stats.vertices += (u32)n;
	if(f->wt) ge_stats.vertices_skinned += (u32)n;
	return 1;
}

static int vcache_reserve(int n){
	if(n > vcache_cap){
		GeClipVertex *nc = realloc(vcache, (size_t)n * sizeof(GeClipVertex));
		if(!nc) return 0;
		vcache = nc;
		vcache_cap = n;
	}
	return 1;
}

static void vsource_batch(VSource *s, int count){
	if(s->use_indices || !opt_on[OPT_GE_VDEC_ESP] || !vcache_reserve(count)) return;
	if(batch_range(s, 0, count, vcache)){
		s->batch = 1;
		s->cache = vcache;
	}
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
		if(!vcache_reserve(n)){ s->use_cache = 0; return; }
		s->cache = vcache;
		if(!batch_range(s, s->lower, n, s->cache))
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
	int vtx, i, refs = opt_on[OPT_GE_ENSAMBLAR];
	/* La ventana de los triángulos: cada hueco de data_ o el vértice que le
	   tocaría, en el caché */
	const GeClipVertex *w[4] = { &data_[0], &data_[1], &data_[2], &data_[3] };

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
			w[data_index_] = vsource_ref(vr, vtx, &data_[data_index_], refs);
			data_index_++;
			if(data_index_ < 3) continue;  /* uno incompleto sigue para PRIM continuar */
			data_index_ = 0;
			send_triangle(cull, w, 2, 0);
		}
		window_keep(w);
		if(data_index_ >= 3){
			send_triangle(cull, w, 2, 0);
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
			int k = (data_index_++) % 3;
			w[k] = vsource_ref(vr, vtx, &data_[k], refs);
			skip--;
			start++;
		}
		for(vtx = start; vtx < vertex_count; vtx++){
			int provoking = (data_index_++) % 3, wind = (data_index_ - 1) % 2;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			w[provoking] = vsource_ref(vr, vtx, &data_[provoking], refs);
			/* Los impares llegan en el orden contrario al del GE (gpu/probe exp45) */
			send_triangle(alt, w, provoking, wind != 0);
		}
		window_keep(w);
		if(is_imm_draw && data_index_ >= 3){
			int provoking = (data_index_ - 1) % 3, wind = (data_index_ - 1) % 2;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			send_triangle(alt, w, provoking, wind != 0);
		}
		break;
	}

	case GE_PRIM_TRIANGLE_FAN: {
		int skip = data_index_ <= 1 ? 1 : 0, start = 0;
		/* El centro solo se lee si no se continúa */
		if(data_index_ == 0 && vertex_count > 0){
			w[0] = vsource_ref(vr, 0, &data_[0], refs);
			data_index_++;
			start = 1;
		}
		for(vtx = start; vtx < vertex_count && skip > 0; vtx++){
			int provoking = 2 - ((data_index_++) % 2);
			w[provoking] = vsource_ref(vr, vtx, &data_[provoking], refs);
			skip--;
			start++;
		}
		for(vtx = start; vtx < vertex_count; vtx++){
			int provoking = 2 - ((data_index_++) % 2), wind = (data_index_ - 1) % 2;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			w[provoking] = vsource_ref(vr, vtx, &data_[provoking], refs);
			send_triangle(alt, w, provoking, wind != 0);
		}
		window_keep(w);
		if(is_imm_draw && data_index_ >= 3){
			int wind = (data_index_ - 1) % 2, provoking = 2 - wind;
			int alt = cull == CULL_OFF ? cull : (cull ^ wind);
			send_triangle(alt, w, provoking, wind != 0);
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

/* La primitiva de los vértices inmediatos */
static u32 imm_prim = 0xFFFFFFFFu, imm_flags;
static int imm_first_sent;

/* Estado guardado de ge_estado_sucio (ver ge_draw_prim) */
static struct {
	int f_ok, v_ok, t_ok;
	u32 f_vt, v_key, t_key;
	VFormat f;
	int fast;
	const GeHwRenderer *hw;
} es;

/* --- Censo de estado (prof_estado): formato y transformación ---------------
   Lo calculado en cada llamada comparado con la anterior (ver ge.c) */
static struct {
	int valid;
	VFormat f;
	int uv_prescale;
	float uv_ps_scale[2], uv_ps_off[2], morph[8];
	TState ts;
	u32 nlights;
	u8 opt[OPT_N];
} cs;

/* Pruebas: lo que una llamada deja a la siguiente, como al arrancar */
void ge_vertex_reset(void){
	last_uv[0] = last_uv[1] = 0.0f;
	last_normal[0] = last_normal[1] = last_normal[2] = 0.0f;
	data_index_ = 0;
	prev_prim_ = GE_PRIM_POINTS;
	imm_prim = 0xFFFFFFFFu;
	imm_flags = 0;
	imm_first_sent = 0;
	mc.valid = 0;
	cs.valid = 0;
	es.f_ok = es.v_ok = es.t_ok = 0;
	ge_sucio = GE_SUCIO_TODO;
}

void ge_test_decode_vertex(u32 vt, const u8 *p, float *pos, float *nrm){
	VFormat f;
	DecVertex d;
	vformat_setup(&f, vt);
	load_morph_weights();
	decode_vertex(&f, p, &d);
	memcpy(pos, d.pos, sizeof(d.pos));
	memcpy(nrm, d.nrm, sizeof(d.nrm));
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

/* --- Censo de formatos de vértice ([TIEMPOS]) ------------------------------ */

#define VT_CENSUS 24
static struct { u32 vtype, verts, draws, prims; } vt_census[VT_CENSUS];
static int vt_n, vt_last;
static u32 vt_other;

static void vtype_note(u32 vt, u32 prim, u32 count){
	int i = vt_last;
	if(i >= vt_n || vt_census[i].vtype != vt){
		for(i = 0; i < vt_n && vt_census[i].vtype != vt; i++){}
		if(i == vt_n){
			if(vt_n == VT_CENSUS){ vt_other += count; return; }
			memset(&vt_census[vt_n++], 0, sizeof(vt_census[0]));
			vt_census[i].vtype = vt;
		}
		vt_last = i;
	}
	vt_census[i].verts += count;
	vt_census[i].draws++;
	vt_census[i].prims |= 1u << (prim & 7);
}

void ge_vtype_census(char *buf, size_t size){
	static const char *const tc[4] = { "", " uv8", " uv16", " uvf" };
	static const char *const col[8] = { "", "", "", "", " c565", " c5551", " c4444", " c8888" };
	static const char *const nrm[4] = { "", " n8", " n16", " nf" };
	static const char *const pos[4] = { " p-", " p8", " p16", " pf" };
	static const char *const wt[4] = { "", " w8", " w16", " wf" };
	static const char *const idx[4] = { "", " i8", " i16", " i32" };
	static const char *const pn[8] = { "pt", "ln", "lt", "tri", "tira", "abanico", "sprite", "?" };
	u32 total = vt_other;
	size_t len;
	int i, j, k;
	for(i = 0; i < vt_n; i++) total += vt_census[i].verts;
	len = (size_t)snprintf(buf, size, "Formatos de vertice (%d):", vt_n);
	for(k = 0; k < 8 && len + 96 < size; k++){
		int best = -1;
		for(i = 0; i < vt_n; i++)
			if(vt_census[i].verts && (best < 0 || vt_census[i].verts > vt_census[best].verts)) best = i;
		if(best < 0) break;
		{
			u32 vt = vt_census[best].vtype;
			len += (size_t)snprintf(buf + len, size - len, " %s%06X%s%s%s%s%s", k ? "| " : "", (unsigned)vt,
			                        tc[vt & 3], col[(vt >> 2) & 7], nrm[(vt >> 5) & 3], pos[(vt >> 7) & 3], wt[(vt >> 9) & 3]);
			if((vt >> 9) & 3) len += (size_t)snprintf(buf + len, size - len, "x%u", (unsigned)(((vt >> 14) & 7) + 1));
			if(len < size) len += (size_t)snprintf(buf + len, size - len, "%s%s%s", idx[(vt >> 11) & 3],
			                                        (vt >> 18) & 7 ? " morph" : "", (vt >> 23) & 1 ? " 2D" : "");
			if(len < size)
				len += (size_t)snprintf(buf + len, size - len, ": %.1f%% (%u llamadas,", total ? 100.0 * vt_census[best].verts / total : 0.0,
				                        (unsigned)vt_census[best].draws);
			for(j = 0; j < 8 && len + 16 < size; j++)
				if(vt_census[best].prims & (1u << j)) len += (size_t)snprintf(buf + len, size - len, " %s", pn[j]);
			if(len < size) len += (size_t)snprintf(buf + len, size - len, ")");
		}
		vt_census[best].verts = 0;
	}
	vt_n = 0;
	vt_last = 0;
	vt_other = 0;
}

static void __attribute__((noinline)) census_vt(const VFormat *f, int v_clean, int t_clean){
	if(cs.valid){
		/* El formato se guarda aparte, por su tipo de vértice: V son las uv y el morph */
		int v_eq = cs.uv_prescale == uv_prescale &&
		           !memcmp(cs.uv_ps_scale, uv_ps_scale, sizeof(uv_ps_scale)) && !memcmp(cs.uv_ps_off, uv_ps_off, sizeof(uv_ps_off)) &&
		           !memcmp(cs.morph, morph_weights, sizeof(morph_weights));
		int t_eq = !memcmp(&cs.ts, &ts, sizeof(ts)) && cs.nlights == ts_nlights;
		ge_stats.cs_vt++;
		ge_stats.cs_v_igual += v_eq;
		ge_stats.cs_t_igual += t_eq;
		if(v_clean){ ge_stats.cs_v_limpio++; ge_stats.cs_v_error += !v_eq; }
		if(t_clean){ ge_stats.cs_t_limpio++; ge_stats.cs_t_error += !t_eq; }
	}
	memcpy(&cs.f, f, sizeof(*f));   /* con el relleno: se compara con memcmp */
	cs.uv_prescale = uv_prescale;
	memcpy(cs.uv_ps_scale, uv_ps_scale, sizeof(uv_ps_scale));
	memcpy(cs.uv_ps_off, uv_ps_off, sizeof(uv_ps_off));
	memcpy(cs.morph, morph_weights, sizeof(morph_weights));
	memcpy(&cs.ts, &ts, sizeof(ts));
	cs.nlights = ts_nlights;
	cs.valid = 1;
}

/* --- Estado sucio (ge_estado_sucio): formato y transformación ---------------
   El formato es función del tipo de vértice; las uv, el morph (V) y la
   transformación (T) solo se rehacen si un comando que los lee cambió de
   valor (ge_sucio, ver ge.c) o si cambian los bits del tipo de vértice que
   leen: through para V; through y si hay color para T. Con ge_verificar se
   calculan siempre y se comparan con lo guardado. */

u32 ge_estado_verif[3][2];   /* V, T y R: comparados y distintos */
static u32 es_verif_logged;

/* Otra matemática u otro renderizador: todo de nuevo (lo guardado no depende
   de ningún interruptor: compute_transform_state, lighting_compute_state,
   setup_uv_prescale y el rasterizador no los leen) */
static void __attribute__((noinline)) estado_entorno_cambia(void){
	es.fast = ge_fast_math;
	es.hw = ge_hw;
	ge_sucio = GE_SUCIO_TODO;
}

static inline void estado_entorno(void){
	if(es.fast != ge_fast_math || es.hw != ge_hw) estado_entorno_cambia();
}

void ge_estado_verif_note(int grupo, int distinto){
	static const char *const nombre[3] = { "uv y morph", "transformacion", "rasterizador" };
	ge_estado_verif[grupo][0]++;
	if(!distinto) return;
	ge_estado_verif[grupo][1]++;
	if(es_verif_logged++ < 16)
		hle_log_quiet("[VERIF] estado: %s guardado distinto del recalculado (tipo de vertice %06X)\n", nombre[grupo],
		              (unsigned)ge.cmd[GE_VERTEXTYPE]);
}

/* Uv y morph; con skip, solo para comprobar lo guardado (ge_verificar) */
static void __attribute__((noinline)) estado_v(const VFormat *f, int skip){
	int old_pre = uv_prescale;
	float old_s[2], old_o[2], old_m[8];
	if(skip){
		memcpy(old_s, uv_ps_scale, sizeof(old_s));
		memcpy(old_o, uv_ps_off, sizeof(old_o));
		memcpy(old_m, morph_weights, sizeof(old_m));
	}
	load_morph_weights();
	setup_uv_prescale(f);
	if(skip)
		ge_estado_verif_note(0, old_pre != uv_prescale || memcmp(old_s, uv_ps_scale, sizeof(old_s)) ||
		                        memcmp(old_o, uv_ps_off, sizeof(old_o)) || memcmp(old_m, morph_weights, sizeof(old_m)));
}

static void __attribute__((noinline)) estado_t_verif(const VFormat *f){
	static TState old;
	u32 old_n = ts_nlights;
	memcpy(&old, &ts, sizeof(ts));
	compute_transform_state(f);
	ge_estado_verif_note(1, memcmp(&old, &ts, sizeof(ts)) || old_n != ts_nlights);
}

void ge_draw_prim(u32 prim, u32 count){
	VFormat f;
	VSource vr;
	u32 vaddr = ge.vaddr, iaddr = ge.iaddr, vt = ge.cmd[GE_VERTEXTYPE];
	int sucio = opt_on[OPT_GE_ESTADO_SUCIO];

	if(count == 0) return;
	if(sucio || opt_on[OPT_PROF_ESTADO]) estado_entorno();
	if(sucio && es.f_ok && es.f_vt == vt) memcpy(&f, &es.f, sizeof(f));
	else {
		vformat_setup(&f, vt);
		memcpy(&es.f, &f, sizeof(f));
		es.f_vt = vt;
		es.f_ok = 1;
	}
	if(!mem_valid(vaddr, 1)) return;
	if(f.idx && !mem_valid(iaddr, 1)) return;

	/* Sin formato de posición (o en turbo) no se dibuja, pero se avanza */
	if(f.pos && !ge_turbo){
		int phase, verif = opt_on[OPT_GE_VERIF];
		u32 v_key = vt & 0x800000u, t_key = v_key | (f.col != 0);
		int v_clean = es.v_ok && es.v_key == v_key && !(ge_sucio & GE_SUCIO_V);
		int t_clean = es.t_ok && es.t_key == t_key && !(ge_sucio & GE_SUCIO_T);
		GE_PROF_T0;
		if(!(sucio && v_clean)){
			load_morph_weights();
			setup_uv_prescale(&f);
		} else if(verif) estado_v(&f, 1);
		if(!(sucio && t_clean)) compute_transform_state(&f);
		else if(verif) estado_t_verif(&f);
		ge_sucio &= ~(u32)(GE_SUCIO_V | GE_SUCIO_T);
		es.v_key = v_key; es.t_key = t_key;
		es.v_ok = es.t_ok = 1;
		if(opt_on[OPT_PROF_ESTADO]) census_vt(&f, v_clean, t_clean);
		phase = prof_ge_enter(GEF_LEER);
		vsource_init(&vr, &f, vaddr, iaddr, (int)count);
		vsource_batch(&vr, (int)count);
		prof_ge_leave(phase);
		ge_stats.draws++;
		ge_stats.draws_prim[prim & 7]++;
		vtype_note(f.vtype, prim, count);
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

/* Como SoftGPU de PPSSPP: cada VAP se envía en el acto (imm_prim, imm_flags
   e imm_first_sent, arriba) */
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
	ge_sucio = GE_SUCIO_TODO;   /* censo: el estado cambia por debajo de los comandos */

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
	ge_vertex_set_xyz(&cv.v, ge_f2i(x * 16.0f), ge_f2i(y * 16.0f), (u16)(u32)z);
	ge_set_clip_flags(&cv);

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
	ge_sucio = GE_SUCIO_TODO;
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
	if(ge_turbo) goto advance;

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
		ge_sucio |= GE_SUCIO_V | GE_SUCIO_T;   /* censo: el de la curva no es el de las llamadas */
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
