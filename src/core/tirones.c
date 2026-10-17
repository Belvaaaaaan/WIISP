/**
 * WIISP - core/tirones.c
 * Censo de tirones (ver tirones.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <string.h>
#include "core/tirones.h"

void tir_init(Tirones *t){ memset(t, 0, sizeof(*t)); }

double tir_normal(const Tirones *t){
	double v[TIR_HIST];
	int i, j, n = t->hist_n;
	if(!n) return 0.0;
	memcpy(v, t->hist, sizeof(v[0]) * (size_t)n);
	/* Inserción: son pocas */
	for(i = 1; i < n; i++){
		double x = v[i];
		for(j = i; j > 0 && v[j - 1] > x; j--) v[j] = v[j - 1];
		v[j] = x;
	}
	return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void tir_imagen(Tirones *t, double real_s, double juego_s, const u64 *acc, double en_juego_s){
	double normal, r;
	int i;
	if(juego_s <= 0.0) return;
	r = real_s / juego_s;
	normal = t->hist_n >= 8 ? tir_normal(t) : 0.0;
	if(normal > 0.0){
		double x = r / normal, perdido = real_s - juego_s * normal;
		t->imagenes++;
		t->real_s += real_s;
		t->rel[x <= 1.25 ? 0 : x <= 2.0 ? 1 : x <= 4.0 ? 2 : 3]++;
		if(x > 2.0 && perdido * 1000.0 > TIR_MIN_MS){
			t->n++;
			t->perdido_s += perdido;
			for(i = 0; i < PROF_N; i++) t->acc[i] += acc[i];
			if(perdido > t->peor_perdido_s){
				t->peor_perdido_s = perdido;
				t->peor_s = real_s;
				t->peor_juego_s = en_juego_s;
				memcpy(t->peor_acc, acc, sizeof(t->peor_acc));
			}
		}
	}
	t->hist[t->hist_pos] = r;
	t->hist_pos = (t->hist_pos + 1) % TIR_HIST;
	if(t->hist_n < TIR_HIST) t->hist_n++;
}

void tir_nuevo_bloque(Tirones *t){
	t->imagenes = t->n = 0;
	memset(t->rel, 0, sizeof(t->rel));
	t->real_s = t->perdido_s = 0.0;
	memset(t->acc, 0, sizeof(t->acc));
	t->peor_s = t->peor_perdido_s = t->peor_juego_s = 0.0;
	memset(t->peor_acc, 0, sizeof(t->peor_acc));
}
