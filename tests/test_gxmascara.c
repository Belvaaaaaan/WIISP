/**
 * WIISP - tests/test_gxmascara.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include "wii/gx_mascara.h"
#include "test.h"

static uint32_t rnd_gm = 20261104u;
static uint32_t rnd(void){ rnd_gm ^= rnd_gm << 13; rnd_gm ^= rnd_gm >> 17; rnd_gm ^= rnd_gm << 5; return rnd_gm; }

static uint32_t a_nativo(int fmt, uint32_t c){
	uint32_t n = 0;
	int k, pos = 0;
	for(k = 0; k < 3; k++){
		const int b = gm_bits(fmt, k);
		n |= (((c >> (8 * k)) & 0xFFu) >> (8 - b)) << pos;
		pos += b;
	}
	return n;
}

static uint32_t psp(int fmt, uint32_t mascara, uint32_t color, uint32_t viejo_n){
	const uint32_t m = a_nativo(fmt, mascara);
	return (a_nativo(fmt, color) & ~m) | (viejo_n & m);
}

static uint32_t efb_desde(int fmt, uint32_t n){
	uint32_t c = 0;
	int k, pos = 0;
	for(k = 0; k < 3; k++){
		const int b = gm_bits(fmt, k);
		const uint32_t v = (n >> pos) & ((1u << b) - 1u);
		c |= ((v << (8 - b)) | (rnd() & ((1u << (8 - b)) - 1u))) << (8 * k);
		pos += b;
	}
	return c;
}

static uint32_t gx(int op, uint32_t efb, uint32_t konst, int rgba6){
	uint32_t r = 0;
	int k;
	for(k = 0; k < 3; k++){
		uint32_t d = (efb >> (8 * k)) & 0xFFu, s = (konst >> (8 * k)) & 0xFFu, o;
		if(rgba6){ d >>= 2; s >>= 2; }
		o = op == GM_OR ? (d | s) : (d & s);
		if(rgba6) o = (o << 2) | (o >> 4);
		r |= o << (8 * k);
	}
	return r;
}

void test_gxmascara(void){
	static const uint32_t dbz[2] = { 0xFFFFEFu, 0xFFFFDFu };
	unsigned i, ops[GM_N] = { 0 };
	uint32_t k;
	int malos = 0;
	printf("gx_mascara_bits: mascara de color parcial con la operacion logica de GX\n");
	CHECK_EQ(gm_escribibles(0, 0), 0xF8FCF8u);
	CHECK_EQ(gm_escribibles(1, 0), 0xF8F8F8u);
	CHECK_EQ(gm_escribibles(2, 0), 0xF0F0F0u);
	CHECK_EQ(gm_escribibles(3, 0), 0xFFFFFFu);
	CHECK_EQ(gm_escribibles(2, 0xFFFFEFu), 0x10u);
	CHECK_EQ(gm_escribibles(2, 0xFFFFDFu), 0x20u);
	CHECK_EQ(gm_escribibles(0, 0xFFFFEFu), 0x10u);
	CHECK_EQ(gm_escribibles(0, 0xFFFF07u), 0xF8u);
	CHECK_EQ(gm_escribibles(3, 0x123456u), 0xEDCBA9u);
	CHECK_EQ(gm_calcular(2, 0xFFFFEFu, 0xFFFFFFu, 1, 0, 0, &k), GM_OR);
	CHECK_EQ(k, 0x10u);
	CHECK_EQ(gm_calcular(2, 0xFFFFDFu, 0xFFFFFFu, 1, 0, 1, &k), GM_OR);
	CHECK_EQ(k, 0x20u);
	CHECK_EQ(gm_calcular(2, 0xFFFFEFu, 0x000000u, 1, 0, 0, &k), GM_AND);
	CHECK_EQ(k, 0xFFFFEFu);
	CHECK_EQ(gm_calcular(2, 0x0F0F0Fu, 0xFFFFFFu, 1, 0, 0, &k), GM_NORMAL);
	CHECK_EQ(gm_calcular(2, 0xF0F0F0u, 0x123456u, 0, 1, 1, &k), GM_NADA);
	CHECK_EQ(gm_calcular(2, 0xFFFFEFu, 0xFFFFFFu, 0, 0, 0, &k), GM_NO_COLOR);
	CHECK_EQ(gm_calcular(2, 0xFFFFEFu, 0xFFFFFFu, 1, 1, 0, &k), GM_NO_ALFA);
	CHECK_EQ(gm_calcular(2, 0xFFFFCFu, 0xFFFF10u, 1, 0, 0, &k), GM_NO_BITS);
	CHECK_EQ(gm_calcular(3, 0xFFFFFEu, 0xFFFFFFu, 1, 0, 1, &k), GM_NO_RGBA6);
	CHECK_EQ(gm_calcular(3, 0xFFFFFEu, 0xFFFFFFu, 1, 0, 0, &k), GM_OR);
	CHECK_EQ(gm_calcular(3, 0xFFFFFDu, 0xFFFFFFu, 1, 0, 1, &k), GM_NO_RGBA6);
	CHECK_EQ(gm_calcular(3, 0xFFFFFBu, 0xFFFFFFu, 1, 0, 1, &k), GM_OR);
	for(i = 0; i < 400000; i++){
		const int fmt = (int)(rnd() & 3), rgba6 = (int)(rnd() & 1);
		uint32_t mascara = rnd() & 0xFFFFFFu, color = rnd() & 0xFFFFFFu, viejo, efb, res, n, want;
		int op;
		if(i & 1) mascara |= rnd() & rnd() & 0xFFFFFFu;
		if(i % 5 == 0) mascara = dbz[i / 5 % 2];
		if(i % 3 == 0) color = (i / 3 & 1) ? 0xFFFFFFu : 0;
		viejo = a_nativo(fmt, rnd());
		op = gm_calcular(fmt, mascara, color, 1, 0, rgba6, &k);
		ops[op]++;
		if(op == GM_NORMAL || op >= GM_NO_COLOR) continue;
		efb = efb_desde(fmt, viejo);
		res = op == GM_NADA ? efb : gx(op, efb, k, rgba6);
		n = a_nativo(fmt, res);
		want = psp(fmt, mascara, color, viejo);
		if(rgba6 && fmt == 3){ n &= 0xFCFCFCu; want &= 0xFCFCFCu; }
		if(n != want && malos++ < 5)
			printf("  distinto: formato %d %s, mascara %06X, color %06X, viejo %04X: GX %04X, PSP %04X\n", fmt, rgba6 ? "RGBA6" : "RGB8",
			       (unsigned)mascara, (unsigned)color, (unsigned)viejo, (unsigned)n, (unsigned)want);
	}
	CHECK_EQ(malos, 0);
	CHECK(ops[GM_OR] > 1000 && ops[GM_AND] > 1000 && ops[GM_NADA] > 100 && ops[GM_NO_BITS] > 1000 && ops[GM_NO_RGBA6] > 100);
	printf("  %u al azar: OR %u, AND %u, nada %u, como antes %u, ponen y quitan bits %u, bits bajos en RGBA6 %u\n", i, ops[GM_OR], ops[GM_AND],
	       ops[GM_NADA], ops[GM_NORMAL], ops[GM_NO_BITS], ops[GM_NO_RGBA6]);
}
