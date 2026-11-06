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
	{
		long canon_mal = 0;
		for(i = 0; i < 20000; i++){
			int32_t x[3], y[3], z[3];
			int k, r, pr, base = -1;
			for(k = 0; k < 3; k++){ x[k] = (int32_t)(rnd() % 64); y[k] = (int32_t)(rnd() % 8); z[k] = (int32_t)(rnd() % 4); }
			for(r = 0; r < 3; r++){
				const int32_t rx[3] = { x[r], x[(r + 1) % 3], x[(r + 2) % 3] }, ry[3] = { y[r], y[(r + 1) % 3], y[(r + 2) % 3] },
				              rz[3] = { z[r], z[(r + 1) % 3], z[(r + 2) % 3] };
				const int c = hz_canonico(rx, ry, rz), o = (c + r) % 3;
				const int32_t cx[3] = { rx[c], rx[(c + 1) % 3], rx[(c + 2) % 3] }, cy[3] = { ry[c], ry[(c + 1) % 3], ry[(c + 2) % 3] },
				              cz[3] = { rz[c], rz[(c + 1) % 3], rz[(c + 2) % 3] };
				if(base < 0) base = o;
				else if(x[o] != x[base] || y[o] != y[base] || z[o] != z[base]) canon_mal++;
				hz_huella(cx, cy, cz, 0, &pr);
				if(pr != 0 && (cx[pr] != cx[0] || cy[pr] != cy[0] || cz[pr] != cz[0])) canon_mal++;
			}
		}
		CHECK_EQ(canon_mal, 0);
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
