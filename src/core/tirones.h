/**
 * WIISP - core/tirones.h
 * Censo de tirones (prof_tirones): lo que tarda cada imagen del juego en
 * tiempo real comparado con lo que dura en el juego, y las que van mucho
 * más lentas que lo normal de las anteriores.
 *
 * Una "imagen" va de un cambio de framebuffer al siguiente; si el juego no
 * cambia de framebuffer (copia su imagen, como GTA), son ventanas de
 * TIR_VENTANA vblanks. La velocidad de cada una es tiempo real / tiempo de
 * juego; lo normal es la mediana de las TIR_HIST anteriores. Un tirón va
 * más del doble de lento que lo normal y pierde más de TIR_MIN_MS. Así no
 * cuentan ni las pantallas de carga (pasa también tiempo de juego) ni la
 * alternancia de un vblank que dibuja y otro que no.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_TIRONES_H
#define WIISP_TIRONES_H
#include "core/types.h"
#include "core/prof.h"

#define TIR_HIST    31   /* imágenes para lo normal (mediana) */
#define TIR_VENTANA 4    /* vblanks sin cambiar de framebuffer: una imagen */
#define TIR_MIN_MS  20   /* lo que tiene que perder un tirón */

typedef struct {
	/* Lo normal: velocidades (real / juego) de las últimas imágenes */
	double hist[TIR_HIST];
	int hist_n, hist_pos;
	/* Desde el último informe */
	u32 imagenes, rel[4], n;   /* rel: hasta 1,25x, 1,25-2x, 2-4x y más de 4x lo normal */
	double real_s, perdido_s;
	u64 acc[PROF_N];           /* tiempo (ticks) de las imágenes con tirón, por cubeta */
	double peor_s, peor_perdido_s, peor_juego_s;
	u64 peor_acc[PROF_N];
} Tirones;

void tir_init(Tirones *t);
/* Lo normal ahora (mediana de las últimas): segundos reales por segundo de juego, 0 sin datos */
double tir_normal(const Tirones *t);
/* Una imagen terminada: real_s segundos reales, juego_s segundos de juego,
   acc el tiempo de cada cubeta en ella (ticks), en_juego_s el momento */
void tir_imagen(Tirones *t, double real_s, double juego_s, const u64 *acc, double en_juego_s);
/* Para el informe siguiente (lo normal se queda) */
void tir_nuevo_bloque(Tirones *t);
#endif
