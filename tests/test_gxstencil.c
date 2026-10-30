/**
 * WIISP - tests/test_gxstencil.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <string.h>
#include "wii/gx_stencil.h"
#include "wii/gx_tiles.h"
#include "test.h"

static uint32_t rnd_st = 4242;
static uint32_t rnd(void){ rnd_st = rnd_st * 1103515245u + 12345u; return rnd_st >> 8; }

static int efb6(int a){ return (a & 0xFF) >> 2; }
static int copia_a8(int c, int forma){ return forma == 0 ? (c << 2) | (c >> 4) : forma == 1 ? c << 2 : (c << 2) | 3; }

static int vuelve(StCod *t, int v, int forma){
	int a = st_codigo(t, v);
	const uint8_t *dec;
	static uint8_t ident[256];
	int i;
	for(i = 0; i < 256; i++) ident[i] = (uint8_t)i;
	if(a < 0) return -1;
	dec = st_tabla(t, ident);
	return dec[copia_a8(efb6(a), forma)];
}

void test_gxstencil(void){
	static const int dbz[8] = { 0x00, 0x01, 0x05, 0x06, 0x08, 0x0D, 0x1C, 0x1F };
	static uint8_t ident[256], hay[256];
	StCod t;
	int i, j, forma, codigos[8], malos = 0, rondas;
	uint16_t tl[256];
	uint32_t pal[128];
	printf("gx_stencil_exacto: codigos de 6 bits para los valores de stencil\n");
	for(i = 0; i < 256; i++) ident[i] = (uint8_t)i;
	memset(&t, 0xA5, sizeof(t));
	st_vaciar(&t);
	CHECK(st_tabla(&t, ident) == ident);
	CHECK_EQ(st_codigo(&t, 0x08), 0x08);
	CHECK_EQ(st_codigo(&t, 0x1C), 0x1C);
	CHECK_EQ(st_codigo(&t, 0x08), 0x08);
	CHECK_EQ(t.raros, 0u);
	CHECK(st_tabla(&t, ident) == t.dec);
	CHECK_EQ(t.dec[0x08], 0x08);
	CHECK_EQ(t.dec[0x0B], 0x08);
	CHECK_EQ(t.dec[0x40], 0x40);

	for(forma = 0; forma < 3; forma++){
		st_vaciar(&t);
		for(i = 0; i < 8; i++) CHECK_EQ(vuelve(&t, dbz[i], forma), dbz[i]);
		for(i = 0; i < 8; i++) CHECK_EQ(vuelve(&t, dbz[i], forma), dbz[i]);
		for(i = 0; i < 8; i++){
			codigos[i] = efb6(st_codigo(&t, dbz[i]));
			for(j = 0; j < i; j++) CHECK(codigos[i] != codigos[j]);
		}
	}
	{
		static const int esperados[8] = { 0, 1, 2, 3, 4, 5, 7, 8 };
		for(i = 0; i < 8; i++) CHECK_EQ(codigos[i], esperados[i]);
	}
	CHECK_EQ(t.raros, 6u);
	CHECK_EQ(t.perdidos, 0u);

	st_vaciar(&t);
	CHECK_EQ(st_codigo(&t, 0x05), 0x05);
	CHECK_EQ(st_codigo(&t, 0x06), st_enc(2));
	CHECK_EQ(st_codigo(&t, 0x40), 0x40);
	CHECK_EQ(st_codigo(&t, 0x41), st_enc(17));
	CHECK_EQ(st_codigo(&t, 0x3C), 0x3C);
	CHECK_EQ(st_codigo(&t, 0x3D), st_enc(14));
	CHECK_EQ(vuelve(&t, 0x3D, 2), 0x3D);
	memset(hay, 0, sizeof(hay));
	for(i = 250; i < 256; i++) hay[i] = 1;
	CHECK(st_monotono(&t, hay));
	for(i = 250; i < 256; i++){
		CHECK_EQ(efb6(st_codigo(&t, i)), 58 + i - 250);
		CHECK_EQ(vuelve(&t, i, 1), i);
	}
	st_vaciar(&t);
	for(i = 0; i < 64; i++) CHECK(st_codigo(&t, i * 3) >= 0);
	CHECK_EQ(t.usados, ~(uint64_t)0);
	CHECK_EQ(st_codigo(&t, 1), -1);
	CHECK_EQ(t.perdidos, 1u);
	for(i = 0; i < 64; i++) CHECK_EQ(vuelve(&t, i * 3, 0), i * 3);

	st_vaciar(&t);
	CHECK(st_codigo(&t, 0x05) >= 0);
	st_cerrar(&t);
	CHECK_EQ(vuelve(&t, 0x05, 0), 0x05);
	CHECK_EQ(st_codigo(&t, 0x06), -1);
	CHECK_EQ(st_tabla(&t, ident)[0x40], 0x40);
	memset(&t, 0xA5, sizeof(t));
	st_vaciar(&t);
	st_cerrar(&t);
	CHECK_EQ(st_tabla(&t, ident)[0x41], 0x41);
	CHECK_EQ(st_codigo(&t, 0x41), -1);

	memset(hay, 0, sizeof(hay));
	for(i = 0; i < 40; i++) hay[i] = 1;
	hay[255] = hay[128] = 1;
	CHECK(st_monotono(&t, hay));
	for(i = 0, j = -1; i < 256; i++){
		if(!hay[i]) continue;
		CHECK(efb6(st_codigo(&t, i)) > j);
		j = efb6(st_codigo(&t, i));
		CHECK_EQ(vuelve(&t, i, 0), i);
	}
	CHECK_EQ(efb6(st_codigo(&t, 255)), 63);
	CHECK_EQ(efb6(st_codigo(&t, 128)), 40);
	memset(hay, 1, 64);
	memset(hay + 64, 0, 192);
	CHECK(st_monotono(&t, hay));
	for(i = 0; i < 64; i++) CHECK_EQ(efb6(st_codigo(&t, i)), i);
	hay[200] = 1;
	CHECK(!st_monotono(&t, hay));
	CHECK(st_tabla(&t, ident)[0x33] == 0x33);
	CHECK_EQ(st_codigo(&t, 7), -1);

	for(rondas = 0; rondas < 3000; rondas++){
		int n = 1 + (int)(rnd() % 64), vals[64], k;
		memset(hay, 0, sizeof(hay));
		for(k = 0; k < n; ){
			int v = (int)(rnd() & 0xFF);
			if(rnd() & 1) v = (int)(rnd() % 40);
			if(hay[v]) continue;
			hay[v] = 1;
			vals[k++] = v;
		}
		st_vaciar(&t);
		if(rondas & 1){
			CHECK(st_monotono(&t, hay));
			for(k = 1; k < n; k++) malos += (efb6(st_codigo(&t, vals[k])) > efb6(st_codigo(&t, vals[k - 1]))) != (vals[k] > vals[k - 1]);
		}
		for(k = 0; k < n; k++) malos += vuelve(&t, vals[k], (int)(rnd() % 3)) != vals[k];
		for(k = 0; k < n; k++)
			for(j = 0; j < k; j++) malos += efb6(st_codigo(&t, vals[k])) == efb6(st_codigo(&t, vals[j]));
		malos += t.perdidos != 0;
	}
	CHECK_EQ(malos, 0);

	for(i = 0; i < 128; i++) pal[i] = (uint32_t)i * 0x020406u + 0x102030u;
	st_vaciar(&t);
	for(i = 0; i < 8; i++) st_codigo(&t, dbz[i] | 0x80);
	{
		const uint8_t *dec = st_tabla(&t, ident);
		for(i = 0; i < 256; i++) tl[i] = gx_tlut565(pal[((((uint32_t)dec[i] << 24) >> 24) & 0x7F)]);
		for(i = 0; i < 8; i++){
			const int a8 = copia_a8(efb6(st_codigo(&t, dbz[i] | 0x80)), 0);
			CHECK_EQ(tl[a8], gx_tlut565(pal[dbz[i]]));
		}
	}
}
