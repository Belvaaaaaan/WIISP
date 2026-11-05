/**
 * WIISP - tests/test_huellas.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <stdlib.h>
#include "wii/gx_huellas.h"
#include "test.h"

static uint32_t rnd_hz = 77001u;
static uint32_t rnd(void){ rnd_hz ^= rnd_hz << 13; rnd_hz ^= rnd_hz >> 17; rnd_hz ^= rnd_hz << 5; return rnd_hz; }

void test_huellas(void){
	static const int perm[6][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 0, 2, 1 }, { 2, 1, 0 }, { 1, 0, 2 } };
	static HzTabla t;
	long distinta = 0, primero_mal = 0, choques = 0, puestos = 0, hallados = 0;
	int i, p;
	printf("prof_profundidad: huella de un triangulo sin importar el orden y su primer vertice\n");
	for(i = 0; i < 20000; i++){
		int32_t x[3], y[3], z[3];
		uint32_t base;
		int prim[3] = { -1, -1, -1 }, pb, k;
		for(k = 0; k < 3; k++){ x[k] = (int32_t)(rnd() % 7680); y[k] = (int32_t)(rnd() % 4352); z[k] = (int32_t)(rnd() & 0xFFFF); }
		if(i % 5 == 0){ y[1] = y[0]; z[2] = z[0]; }
		base = hz_huella(x, y, z, 3, &pb);
		for(p = 0; p < 6; p++){
			const int32_t px[3] = { x[perm[p][0]], x[perm[p][1]], x[perm[p][2]] }, py[3] = { y[perm[p][0]], y[perm[p][1]], y[perm[p][2]] },
			              pz[3] = { z[perm[p][0]], z[perm[p][1]], z[perm[p][2]] };
			int pr;
			if(hz_huella(px, py, pz, 3, &pr) != base) distinta++;
			if(prim[perm[p][0]] < 0) prim[perm[p][0]] = pr;
			else if(prim[perm[p][0]] != pr) primero_mal++;
		}
		if(prim[0] != pb || prim[0] == prim[1] || prim[1] == prim[2] || prim[0] == prim[2]) primero_mal++;
		{
			int pr;
			if(hz_huella(x, y, z, 4, &pr) == base) choques++;
			x[0]++;
			if(hz_huella(x, y, z, 3, &pr) == base) choques++;
			x[0]--;
			z[1]++;
			if(hz_huella(x, y, z, 3, &pr) == base) choques++;
		}
	}
	CHECK_EQ(distinta, 0);
	CHECK_EQ(primero_mal, 0);
	CHECK(choques < 3);
	CHECK_EQ(hz_poner(&t, 1234, 0, 0x10000, 1), 1);
	CHECK_EQ(hz_buscar(&t, 1234, 0, 0x10000, 1), HZ_IGUAL);
	CHECK_EQ(hz_buscar(&t, 1234, 2, 0x10000, 1), HZ_ROTADO);
	CHECK_EQ(hz_buscar(&t, 1234, 0, 0x20000, 1), HZ_OTRO_RANGO);
	CHECK_EQ(hz_buscar(&t, 1234, 1, 0x20000, 1), HZ_ROTADO_OTRO_RANGO);
	CHECK_EQ(hz_buscar(&t, 1234, 0, 0x10000, 2), HZ_SIN_PAREJA);
	CHECK_EQ(hz_buscar(&t, 4321, 0, 0x10000, 1), HZ_SIN_PAREJA);
	CHECK_EQ(hz_poner(&t, 1234, 2, 0x10000, 1), 1);
	CHECK_EQ(hz_buscar(&t, 1234, 2, 0x10000, 1), HZ_IGUAL);
	CHECK_EQ(hz_poner(&t, 1234, 1, 0x30000, 1), 1);
	CHECK_EQ(hz_buscar(&t, 1234, 0, 0x10000, 1), HZ_OTRO_RANGO);
	CHECK_EQ(hz_buscar(&t, 1234, 1, 0x30000, 1), HZ_OTRO_RANGO);
	for(i = 0; i < 2 * HZ_CASILLAS; i++){
		const uint32_t c = (uint32_t)i * 2654435761u + 17u;
		if(hz_poner(&t, c ? c : 1u, i % 3, 7, 3)) puestos++;
	}
	for(i = 0; i < 2 * HZ_CASILLAS; i++){
		const uint32_t c = (uint32_t)i * 2654435761u + 17u;
		if(hz_buscar(&t, c ? c : 1u, i % 3, 7, 3) == HZ_IGUAL) hallados++;
	}
	CHECK(puestos >= HZ_CASILLAS * 3 / 4 && puestos <= HZ_CASILLAS);
	CHECK_EQ(hallados, puestos);
	printf("  20000 triangulos en sus 6 ordenes: %ld huellas distintas, %ld primeros mal, %ld choques; tabla llena con %ld de %d\n",
	       distinta, primero_mal, choques, puestos, HZ_CASILLAS);
}
