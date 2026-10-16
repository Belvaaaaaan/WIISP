/**
 * WIISP - tests/test_gxemit.c
 * El envío de triángulos a GX (wii/gx_emit.h): la versión rápida
 * (gx_envio_rapido) tiene que dar los mismos valores, bit a bit, que la de
 * referencia (lo que hacía put_vertex), con estados y vértices al azar:
 * uv NaN e infinitas, w iguales (el atajo de k / w), negativas o enormes,
 * sombreado plano, through, proyección, niebla y coordenadas de pantalla.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <string.h>
#include "gpu/ge_internal.h"
#include "wii/gx_emit.h"
#include "test.h"

static u32 rs = 77;
static u32 rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f; }

static float odd_float(void){
	static const float special[] = { 0.0f, -0.0f, 1.0f, 1e-30f, -1e-30f, 3.0e38f, -3.0e38f };
	u32 b;
	float f;
	switch(rnd() % 8){
	case 0: b = 0x7FC00000u; memcpy(&f, &b, 4); return f;     /* NaN */
	case 1: b = 0x7F800000u | (rnd() & 0x80000000u); memcpy(&f, &b, 4); return f;   /* infinito */
	case 2: return special[rnd() % (sizeof(special) / sizeof(special[0]))];
	default: return frnd(-4.0f, 4.0f);
	}
}

static void random_vertex(GeVertex *v, float shared_w){
	int x = (int)(rnd() % 70000u) - 4000, y = (int)(rnd() % 70000u) - 4000;
	memset(v, 0, sizeof(*v));
	v->s = rnd() % 6 ? frnd(-2.0f, 2.0f) : odd_float();
	v->t = rnd() % 6 ? frnd(-2.0f, 2.0f) : odd_float();
	v->q = rnd() % 6 ? frnd(-2.0f, 2.0f) : odd_float();
	switch(rnd() % 6){
	case 0: v->clipw = shared_w; break;            /* la misma que otro: k / w = 1 */
	case 1: v->clipw = odd_float(); break;
	default: v->clipw = frnd(0.01f, 500.0f); break;
	}
	v->color0 = rnd();
	v->color1 = rnd();
	ge_vertex_set_xyz(v, x, y, (u16)rnd());
	switch(rnd() % 4){
	case 0: v->fogdepth = 1.0f; break;
	case 1: v->fogdepth = odd_float(); break;
	default: v->fogdepth = frnd(-0.2f, 1.2f); break;
	}
}

void test_gxemit(void){
	int n, bad = 0, shortcut = 0;
	printf("Envio a GX: rapido contra referencia\n");
	for(n = 0; n < 200000; n++){
		GeVertex v[3];
		GxVtxWords a[3], b[3];
		GxEmitCtx e;
		float w = rnd() % 3 ? frnd(0.01f, 300.0f) : odd_float();
		int i;
		for(i = 0; i < 3; i++) random_vertex(&v[i], w);
		e.tex_on = rnd() % 4 != 0;
		e.through = rnd() % 4 == 0;
		e.tex_proj = rnd() % 3 == 0;
		e.flat = rnd() % 4 == 0;
		e.tex_norm = rnd() % 5 != 0;
		e.inv_tw = 1.0f / (float)(1 << (rnd() % 10));
		e.inv_th = 1.0f / (float)(1 << (rnd() % 10));
		e.tex_su = rnd() % 4 ? 1.0f : frnd(0.1f, 3.0f);
		e.tex_sv = rnd() % 4 ? 1.0f : frnd(0.1f, 3.0f);
		e.pos_off = rnd() % 3 ? 0.0f : 0.5f;
		gx_triangle_ref(a, &v[0], &v[1], &v[2], &e);
		gx_triangle_fast(b, &v[0], &v[1], &v[2], &e);
		if(memcmp(a, b, sizeof(a))){
			if(bad++ < 5) printf("  caso %d distinto\n", n);
		}
		if(e.tex_on && !e.through && e.tex_norm){
			float k = gx_tex_scale3(&v[0], &v[1], &v[2]);
			for(i = 0; i < 3; i++) shortcut += v[i].clipw == k;
		}
	}
	CHECK_EQ(bad, 0);
	CHECK(shortcut > 10000);   /* el atajo de k / w se prueba de verdad */
	printf("  %d triangulos, %d vertices por el atajo de k / w\n", n, shortcut);
}
