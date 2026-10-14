/**
 * WIISP - tests/test_skin.c
 * Skinning rápido (ge_skin_rapido, solo con la matemática rápida del backend
 * por hardware) contra el exacto del GE: de 1 a 8 huesos, pesos de 8 y 16
 * bits y float, posiciones y normales de los tres tamaños y con morph. La
 * diferencia tiene que quedar en el redondeo de float24; con el interruptor
 * apagado, el resultado tiene que ser el exacto bit a bit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/opt.h"
#include "test.h"

static u32 rnd_state = 4242;
static u32 rnd(void){ rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static float frnd(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() & 0xFFFF) / 65535.0f; }

static const u32 csize[4] = { 0, 1, 2, 4 };
static u32 align_to(u32 n, u32 a){ return (n + a - 1) & ~(a - 1); }

static void put_comp(u8 *p, int fmt, float v, int sign){
	switch(fmt){
	case 1: p[0] = (u8)(sign ? (s8)(int)(v * 127.0f) : (u8)(int)(v * 127.0f)); break;
	case 2: { u16 x = (u16)(sign ? (s16)(int)(v * 32767.0f) : (u16)(int)(v * 32767.0f)); p[0] = (u8)x; p[1] = (u8)(x >> 8); break; }
	default: { u32 x; memcpy(&x, &v, 4); p[0] = (u8)x; p[1] = (u8)(x >> 8); p[2] = (u8)(x >> 16); p[3] = (u8)(x >> 24); }
	}
}

/* Cota del error: la suma de los valores absolutos de todos los términos */
static float bound(int nw, const float *w, const float *src, int with_t){
	float b = 0.0f;
	int i, j, k;
	for(i = 0; i < nw; i++)
		for(k = 0; k < 3; k++){
			float t = with_t ? fabsf(ge.bone[i][9 + k]) : 0.0f;
			for(j = 0; j < 3; j++) t += fabsf(src[j] * ge.bone[i][j * 3 + k]);
			b += fabsf(w[i]) * t;
		}
	return b;
}

void test_skin(void){
	static u8 buf[2048];
	int caso, peor_caso = -1, fast_old = ge_fast_math, con_valor = 0;
	float peor = 0.0f;
	printf("Skinning rapido contra el exacto del GE\n");
	for(caso = 0; caso < 600; caso++){
		int wt = 1 + (int)(rnd() % 3), nrm = (int)(rnd() % 4), pos = 1 + (int)(rnd() % 3);
		int nw = 1 + (int)(rnd() % 8), nm = caso % 5 == 0 ? 2 + (int)(rnd() % 3) : 1, i, k, m;
		u32 vt = (u32)wt << 9 | (u32)nrm << 5 | (u32)pos << 7 | (u32)(nw - 1) << 14 | (u32)(nm - 1) << 18;
		u32 off_w = 0, off_n, off_p, size, biggest = csize[wt], onesize;
		float w[8], src[3] = { 0, 0, 0 }, a_pos[3], a_nrm[3], b_pos[3], b_nrm[3], c_pos[3], c_nrm[3];
		size = csize[wt] * (u32)nw;
		if(nrm){ size = align_to(size, csize[nrm]); off_n = size; size += 3 * csize[nrm]; if(csize[nrm] > biggest) biggest = csize[nrm]; }
		else off_n = 0;
		size = align_to(size, csize[pos]); off_p = size; size += 3 * csize[pos];
		if(csize[pos] > biggest) biggest = csize[pos];
		onesize = align_to(size, biggest);
		memset(buf, 0, sizeof(buf));
		for(i = 0; i < 8; i++)
			for(k = 0; k < 12; k++) ge.bone[i][k] = frnd(-2.0f, 2.0f);
		for(m = 0; m < 8; m++){
			float mw = frnd(0.0f, 1.0f);
			u32 bits;
			memcpy(&bits, &mw, 4);
			ge.cmd[GE_MORPHWEIGHT0 + m] = bits >> 8;
		}
		for(m = 0; m < nm; m++){
			u8 *v = buf + (u32)m * onesize;
			for(i = 0; i < nw; i++){
				w[i] = rnd() % 6 == 0 ? 0.0f : frnd(0.0f, 1.0f);
				put_comp(v + off_w + (u32)i * csize[wt], wt, w[i], 0);
			}
			for(i = 0; i < 3; i++){
				src[i] = frnd(-0.99f, 0.99f);
				put_comp(v + off_p + (u32)i * csize[pos], pos, src[i], 1);
				if(nrm) put_comp(v + off_n + (u32)i * csize[nrm], nrm, frnd(-0.99f, 0.99f), 1);
			}
		}
		ge_fast_math = 0;
		ge_test_decode_vertex(vt, buf, a_pos, a_nrm);           /* exacto */
		ge_fast_math = 1;
		opt_on[OPT_GE_SKIN_RAPIDO] = 0;
		ge_test_decode_vertex(vt, buf, c_pos, c_nrm);           /* rápido sin el interruptor */
		opt_on[OPT_GE_SKIN_RAPIDO] = 1;
		ge_test_decode_vertex(vt, buf, b_pos, b_nrm);           /* skinning rápido */
		CHECK(!memcmp(a_pos, c_pos, sizeof(a_pos)) && !memcmp(a_nrm, c_nrm, sizeof(a_nrm)));
		if(fabsf(a_pos[0]) + fabsf(a_pos[1]) + fabsf(a_pos[2]) > 0.1f) con_valor++;
		{
			/* Con morph la cota usa pesos y posiciones de la última copia: se
			   amplía (los valores mezclados quedan dentro del mismo rango) */
			float wb[8], ones[3] = { 1.0f, 1.0f, 1.0f }, bp, bn;
			for(i = 0; i < nw; i++) wb[i] = nm > 1 ? (float)nm : 1.0f;
			bp = bound(nw, wb, ones, 1) + 1e-6f;
			bn = bound(nw, wb, ones, 0) + 1e-6f;
			for(i = 0; i < 3; i++){
				float ep = fabsf(a_pos[i] - b_pos[i]) / bp, en = fabsf(a_nrm[i] - b_nrm[i]) / bn;
				if(ep > peor){ peor = ep; peor_caso = caso; }
				if(nrm && en > peor){ peor = en; peor_caso = caso; }
			}
		}
	}
	ge_fast_math = fast_old;
	opt_reset();
	printf("  peor diferencia: %.2g de la suma de los terminos (caso %d)\n", (double)peor, peor_caso);
	CHECK(peor < 2e-4f);
	CHECK(con_valor > 400);   /* la prueba mueve vértices de verdad */
}
