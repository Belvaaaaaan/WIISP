/**
 * WIISP - wii/gx_emit.h
 * Lo que se escribe en el FIFO de GX por cada vértice de una primitiva
 * (posición, coordenadas de textura y los dos colores), calculado aparte de
 * las escrituras. Sin libogc: las pruebas del PC comparan la versión rápida
 * con la de referencia bit a bit (tests/test_gxemit.c).
 *
 * Por qué aparte: el FIFO es memoria volatile y cada escritura puede, para
 * el compilador, cambiar cualquier float o entero global, así que tras cada
 * una volvía a leer el estado (pos_off, tex_su, inv_tw...). Calculando antes
 * los valores de los tres vértices, el estado se lee una vez por triángulo.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_GX_EMIT_H
#define WIISP_GX_EMIT_H

#include <string.h>
#include "gpu/ge_internal.h"

/* El estado que decide los valores (copia de los globales de gx_ge.c) */
typedef struct {
	int tex_on, through, tex_proj, flat, tex_norm;
	float inv_tw, inv_th, tex_su, tex_sv, pos_off;
} GxEmitCtx;

/* Un vértice tal como va al FIFO: x, y, z, s, t, q y los colores (sin dar
   la vuelta a los bytes todavía) */
typedef struct {
	float f[6];
	u32 c[2];
} GxVtxWords;

static inline int gx_fog_byte(float fogdepth){
	if(!(fogdepth > 0.0f)) return 0;
	if(fogdepth >= 1.0f) return 255;
	return (int)(fogdepth * 255.0f);
}

/* Una uv NaN o infinita muestrea el texel 0 en la PSP */
static inline float gx_finite_uv(float f){
	return f - f == 0.0f ? f : 0.0f;
}

/* La perspectiva de las texturas la hace GX con s/w, t/w y q = 1/w y la
   división s/q en cada píxel. q puede ser muy pequeña (0,001 con w = 1000) y
   el hardware la interpola con menos precisión que un float: todo el
   triángulo se multiplica por la w más cercana, s/q no cambia y q queda en
   (0, 1]. k es ese factor (1 sin normalizar). */
static inline float gx_tex_scale3(const GeVertex *v0, const GeVertex *v1, const GeVertex *v2){
	float w = v0->clipw;
	if(v1->clipw < w) w = v1->clipw;
	if(v2->clipw < w) w = v2->clipw;
	return w > 0.0f && w < 3.0e38f ? w : 1.0f;
}

/* --- Referencia: lo que hacía put_vertex ------------------------------------ */

/* v da posición, uv y niebla; cv los colores; k el factor de gx_tex_scale3 */
static inline void gx_vertex_ref(GxVtxWords *o, const GeVertex *v, const GeVertex *cv, float k, const GxEmitCtx *e){
	float s = 0.0f, t = 0.0f, q = 1.0f;
	if(e->tex_on){
		s = gx_finite_uv(v->s); t = gx_finite_uv(v->t);
		if(e->through){ s *= e->inv_tw; t *= e->inv_th; }
		else {
			float iw = k / v->clipw;
			if(e->tex_proj) q = v->q;
			s *= iw * e->tex_su; t *= iw * e->tex_sv; q *= iw;
		}
	}
	o->f[0] = (float)v->x * (1.0f / 16.0f) + e->pos_off;
	o->f[1] = (float)v->y * (1.0f / 16.0f) + e->pos_off;
	o->f[2] = (float)v->z;
	o->f[3] = s; o->f[4] = t; o->f[5] = q;
	o->c[0] = cv->color0;
	o->c[1] = (cv->color1 & 0xFFFFFF) | ((u32)gx_fog_byte(v->fogdepth) << 24);
}

static inline void gx_triangle_ref(GxVtxWords o[3], const GeVertex *v0, const GeVertex *v1, const GeVertex *v2,
                                   const GxEmitCtx *e){
	const GeVertex *c = e->flat ? v2 : NULL;
	float k = e->tex_on && !e->through && e->tex_norm ? gx_tex_scale3(v0, v1, v2) : 1.0f;
	gx_vertex_ref(&o[0], v0, c ? c : v0, k, e);
	gx_vertex_ref(&o[1], v1, c ? c : v1, k, e);
	gx_vertex_ref(&o[2], v2, c ? c : v2, k, e);
}

/* --- Rápida (gx_envio_rapido) --------------------------------------------------
   Las mismas expresiones, con cambios que no cambian ningún bit:
   - x, y, z llegan ya en float (fx, fy, fz del GE, iguales a (float)x...):
     sin las tres conversiones de entero por vértice y triángulo;
   - el vértice cuya w es la del factor k da k / w = 1 exacto (x / x = 1 en
     IEEE para x finito y no nulo; k nunca es 0, infinito ni NaN): esa
     división se ahorra;
   - el estado va en variables locales: se lee una vez por triángulo y no
     después de cada escritura al FIFO;
   - uv finita sin saltos (fsel) y la niebla de 1 (sin niebla) por sus bits. */

/* gx_finite_uv: f - f es +0 si f es finito y NaN si no; fsel elige f si
   f - f >= 0 y 0 si no (NaN incluido). En el PC, la de referencia. */
static inline __attribute__((always_inline)) float gx_finite_uv_fast(float f){
#ifdef GEKKO
	float r;
	__asm__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(f - f), "f"(f), "f"(0.0f));
	return r;
#else
	return gx_finite_uv(f);
#endif
}

/* gx_fog_byte: con la niebla en 1 exacto (lo de casi siempre) 255 sin
   comparar floats */
static inline __attribute__((always_inline)) u32 gx_fog_byte_fast(const float *fogdepth){
	u32 b;
	memcpy(&b, fogdepth, 4);
	return b == 0x3F800000u ? 255u : (u32)gx_fog_byte(*fogdepth);
}

typedef struct {
	int tex_on, through, tex_proj;
	float k, inv_tw, inv_th, tex_su, tex_sv, pos_off;
} GxEmitLocal;

static inline __attribute__((always_inline)) GxVtxWords gx_vertex_fast(const GeVertex *v, const GeVertex *cv,
                                                                       const GxEmitLocal *l){
	GxVtxWords o;
	float s = 0.0f, t = 0.0f, q = 1.0f;
	if(l->tex_on){
		s = gx_finite_uv_fast(v->s); t = gx_finite_uv_fast(v->t);
		if(l->through){ s *= l->inv_tw; t *= l->inv_th; }
		else {
			float iw = v->clipw == l->k ? 1.0f : l->k / v->clipw;
			if(l->tex_proj) q = v->q;
			s *= iw * l->tex_su; t *= iw * l->tex_sv; q *= iw;
		}
	}
	o.f[0] = v->fx * (1.0f / 16.0f) + l->pos_off;
	o.f[1] = v->fy * (1.0f / 16.0f) + l->pos_off;
	o.f[2] = v->fz;
	o.f[3] = s; o.f[4] = t; o.f[5] = q;
	o.c[0] = cv->color0;
	o.c[1] = (cv->color1 & 0xFFFFFF) | (gx_fog_byte_fast(&v->fogdepth) << 24);
	return o;
}

/* El estado y el factor k de un triángulo */
static inline __attribute__((always_inline)) void gx_triangle_local(GxEmitLocal *l, const GeVertex *v0, const GeVertex *v1,
                                                                    const GeVertex *v2, const GxEmitCtx *e){
	l->tex_on = e->tex_on; l->through = e->through; l->tex_proj = e->tex_proj;
	l->inv_tw = e->inv_tw; l->inv_th = e->inv_th; l->tex_su = e->tex_su; l->tex_sv = e->tex_sv; l->pos_off = e->pos_off;
	l->k = e->tex_on && !e->through && e->tex_norm ? gx_tex_scale3(v0, v1, v2) : 1.0f;
}

/* Los tres vértices en un arreglo (pruebas y verificación) */
static inline void gx_triangle_fast(GxVtxWords o[3], const GeVertex *v0, const GeVertex *v1, const GeVertex *v2,
                                    const GxEmitCtx *e){
	GxEmitLocal l;
	const GeVertex *c = e->flat ? v2 : NULL;
	gx_triangle_local(&l, v0, v1, v2, e);
	o[0] = gx_vertex_fast(v0, c ? c : v0, &l);
	o[1] = gx_vertex_fast(v1, c ? c : v1, &l);
	o[2] = gx_vertex_fast(v2, c ? c : v2, &l);
}

#endif
