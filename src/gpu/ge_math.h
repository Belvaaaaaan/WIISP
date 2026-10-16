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

#include <math.h>
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

/* 1 = transformación y luces de los vértices con float normal en lugar de
   la aritmética del GE (varias veces más rápido; para el backend por
   hardware, donde ±1 en la profundidad o el color no se ve). El resto
   (skinning, morph, curvas, recorte) sigue siendo exacto. */
extern int ge_fast_math;

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

/* --- Versiones rápidas para ge_fast_math ---------------------------------
   Broadway no tiene instrucciones de raíz ni de redondeo: sqrtf y floorf
   de newlib son rutinas de cientos de ciclos. */

/* Sumas de productos con la fusión (fmadds) escrita: en PowerPC el
   compilador fusiona a * b + c * d en fmadds(a, b, c * d) o en fmadds(c, d,
   a * b) según el orden en que le queden las operaciones, que puede cambiar
   entre dos copias en línea del mismo código (el camino por lotes y el de
   uno en uno) y dar un bit distinto. Así es la que hace con el código en
   orden (el primer producto se fusiona), en todas las copias. En x86 no
   hay fusión: el mismo orden de sumas (x + y = y + x en IEEE). */
#if defined(__PPC__) || defined(__powerpc__) || defined(GEKKO)
#define GE_FMADD(a, b, c) __builtin_fmaf((a), (b), (c))
#else
#define GE_FMADD(a, b, c) ((a) * (b) + (c))
#endif

/* a0 * b0 + a1 * b1 + a2 * b2 */
static inline float ge_dot3_fused(float a0, float b0, float a1, float b1, float a2, float b2){
	return GE_FMADD(a2, b2, GE_FMADD(a0, b0, a1 * b1));
}

/* floorf sin libm; idéntico salvo que -0 da +0. Con |f| < 2^22, sumar y
   restar 1,5 * 2^23 redondea al entero más cercano sin pasar por un entero
   (en el Broadway, float -> int -> float va por la memoria); si se pasó, uno
   menos. Mismo resultado que el camino de siempre, que sigue para el resto. */
static inline float ge_floorf(float f){
	if(f > -4194304.0f && f < 4194304.0f){
		float t = (f + 12582912.0f) - 12582912.0f;
		return t > f ? t - 1.0f : t;
	}
	if(f > -8388608.0f && f < 8388608.0f){
		float t = (float)(int)f;
		return t > f ? t - 1.0f : t;
	}
	return f;  /* ya entero, infinito o NaN */
}

/* 1/sqrt(x) para x finito y > 0 */
static inline float ge_fast_rsqrt(float x){
#ifdef GEKKO
	double xd = x, y;
	__asm__("frsqrte %0,%1" : "=f"(y) : "f"(xd));
	y = y * (1.5 - 0.5 * xd * y * y);
	y = y * (1.5 - 0.5 * xd * y * y);
	return (float)y;
#else
	return 1.0f / sqrtf(x);
#endif
}

/* ge_light_pow en float y enteros de 32 bits (sin trunc ni double) */
static inline float ge_fast_light_pow(float v, float e){
	if(e <= 0.0f) return 1.0f;
	if(v > 0.0f){
		s32 ix, iy;
		float a, y;
		memcpy(&ix, &v, 4);
		a = e * (float)(ix - 0x3F800000) * (1.0f / 16.0f);
		if(a < -66584576.0f) a = -66584576.0f;
		else if(a > 67108863.0f) a = 67108863.0f;
		iy = (s32)a * 16 + 0x3F800000;
		memcpy(&y, &iy, 4);
		return y;
	}
	return v;
}

int ge_line_coverage_alpha(s64 x0, s64 y0, s64 x1, s64 y1, int px, int py);

/* Fila de la matriz combinada 4x4 (vector fila * matriz) para la componente c */
float ge_clip_component(const float *v3, const float *m16, int c);
/* m = a * b con la precisión del GE (4x4) */
void ge_combine_matrices(float *out, const float *a, const float *b);
/* Viewport: c/w * escala + centro, como el GE */
float ge_viewport(float clip_c, float clip_w, float scale, float center);

#endif
