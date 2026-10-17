/**
 * WIISP - tests/test_tirones.c
 * El censo de tirones (core/tirones.c) con tiempos inventados: lo normal
 * es la mediana, una pantalla de carga no es un tirón (pasa también tiempo
 * de juego), una imagen el triple de lenta sí, y lo perdido es lo que pasa
 * de lo normal.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <string.h>
#include "core/tirones.h"
#include "test.h"

static int cerca(double a, double b){ return a - b < 1e-9 && b - a < 1e-9; }

void test_tirones(void){
	Tirones t;
	u64 acc[PROF_N];
	int i;
	printf("Censo de tirones: por imagen y respecto a lo normal\n");
	memset(acc, 0, sizeof(acc));
	acc[PROF_CPU] = 7;
	tir_init(&t);
	CHECK(cerca(tir_normal(&t), 0.0));
	/* Mediana con pares e impares */
	tir_imagen(&t, 3.0, 1.0, acc, 0.0);
	tir_imagen(&t, 1.0, 1.0, acc, 0.0);
	CHECK(cerca(tir_normal(&t), 2.0));
	tir_imagen(&t, 2.0, 1.0, acc, 0.0);
	CHECK(cerca(tir_normal(&t), 2.0));
	/* Calentando: no se clasifica nada */
	CHECK_EQ(t.imagenes, 0u);
	tir_init(&t);
	/* 30 imágenes por segundo de juego (2 vblanks) a un tercio de velocidad */
	for(i = 0; i < 40; i++) tir_imagen(&t, 0.1, 1.0 / 30.0, acc, i / 30.0);
	CHECK_EQ(t.n, 0u);
	CHECK(t.imagenes == 32u);
	CHECK(cerca(tir_normal(&t), 3.0));
	/* Una pantalla de carga: 2 s de juego en 6 s reales, al mismo paso */
	tir_imagen(&t, 6.0, 2.0, acc, 2.0);
	CHECK_EQ(t.n, 0u);
	/* Una imagen el triple de lenta: tirón, pierde 0,2 s */
	acc[PROF_ES] = 1000;
	tir_imagen(&t, 0.3, 1.0 / 30.0, acc, 5.0);
	CHECK_EQ(t.n, 1u);
	CHECK(t.perdido_s > 0.199 && t.perdido_s < 0.201);
	CHECK(cerca(t.peor_s, 0.3));
	CHECK(cerca(t.peor_juego_s, 5.0));
	CHECK_EQ(t.peor_acc[PROF_ES], (u64)1000);
	CHECK_EQ(t.acc[PROF_CPU], (u64)7);
	CHECK_EQ(t.rel[2], 1u);
	/* Más del doble pero pierde menos de TIR_MIN_MS: no es tirón */
	tir_imagen(&t, 0.0021, 0.0002, acc, 6.0);
	CHECK_EQ(t.n, 1u);
	/* Un bloque nuevo: los contadores a cero y lo normal se queda */
	tir_nuevo_bloque(&t);
	CHECK_EQ(t.imagenes, 0u);
	CHECK_EQ(t.n, 0u);
	CHECK(cerca(tir_normal(&t), 3.0));
}
