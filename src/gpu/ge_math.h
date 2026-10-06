/**
 * WIISP - ge_math.h
 * La aritmética propia del GE (precisión de 24 bits, recíproco por tablas,
 * sumas sin bits de guarda), tal como se ha medido en una PSP.
 *
 * Portado de PPSSPP (GPU/Software/GEMath.h/.cpp, (c) PPSSPP Project,
 * GPLv2+), que lo midió con sus pruebas "gpu/probe".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_GE_MATH_H
#define WIISP_GE_MATH_H

#include "core/types.h"

/* float24: float con los 8 bits bajos de la mantisa a cero */
static inline float ge_trunc24(float f){
	u32 bits;
	memcpy(&bits, &f, 4);
	bits &= 0xFFFFFF00u;
	memcpy(&f, &bits, 4);
	return f;
}

/* El producto de dos float24 se trunca en double antes de pasarlo a float */
static inline float ge_product24(double d){
	u64 bits;
	memcpy(&bits, &d, 8);
	if(((bits >> 52) & 0x7FF) != 0x7FF) bits &= ~((1ull << (52 - 15)) - 1);
	memcpy(&d, &bits, 8);
	return (float)d;
}

/* u = s * R(q): 24 bits significativos truncados (gpu/probe exp82) */
static inline float ge_uv_product(double d){
	u64 bits;
	memcpy(&bits, &d, 8);
	if(((bits >> 52) & 0x7FF) != 0x7FF) bits &= ~((1ull << (52 - 23)) - 1);
	memcpy(&d, &bits, 8);
	return (float)d;
}

/* Coordenada de textura truncada como la ve el muestreador */
static inline float ge_trunc_texcoord(float f){
	u32 bits;
	memcpy(&bits, &f, 4);
	bits &= 0xFFFFFE00u;
	memcpy(&f, &bits, 4);
	return f;
}

/* log2 en dieciseisavos a partir de los bits del float (nivel de mipmap) */
static inline int ge_log16(float delta){
	u32 u;
	memcpy(&u, &delta, 4);
	return (int)((u >> 19) & 0x0FFF) - 127 * 16;
}

float ge_recip(float w);
float ge_rsqrt(float d);
/* Suma del GE: el término menor se trunca a la precisión del mayor */
float ge_add(float a, float b);

/* Un término de fila de matriz: mantisa * 2^lsb_exp */
typedef struct { s32 mantissa; int lsb_exp; } GeRowTerm;
GeRowTerm ge_product(float a, float b);
float ge_row_sum(const GeRowTerm *terms, int count);

float ge_add24(float a, float b);
float ge_dot(const float *a, const float *b);
float ge_normalize(float *v);
/* Recíproco del montaje de triángulos: q / 2^(e+2) ~ 2^14 / abs_det */
s64 ge_setup_recip(u64 abs_det, int *e);
float ge_light_pow(float v, float e);
int ge_line_coverage_alpha(s64 x0, s64 y0, s64 x1, s64 y1, int px, int py);

/* Fila de la matriz combinada 4x4 (vector fila * matriz) para la componente c */
float ge_clip_component(const float *v3, const float *m16, int c);
/* m = a * b con la precisión del GE (4x4) */
void ge_combine_matrices(float *out, const float *a, const float *b);
/* Viewport: c/w * escala + centro, como el GE */
float ge_viewport(float clip_c, float clip_w, float scale, float center);

#endif
