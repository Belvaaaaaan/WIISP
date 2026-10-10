/**
 * WIISP - core/prof.h
 * Desglose del tiempo real: a qué dedica el emulador cada segundo de verdad.
 *
 * El tiempo se reparte en cubetas exclusivas que suman el 100 %: en cada
 * momento solo cuenta una, la de lo que se esté haciendo (ejecutar
 * instrucciones, un syscall, procesar una lista del GE...). Al entrar en
 * algo se cambia de cubeta y al salir se vuelve a la anterior:
 *
 *     int old = prof_switch(PROF_GE);
 *     ...
 *     prof_switch(old);
 *
 * Solo se mide en puntos gruesos (syscalls, listas del GE, texturas,
 * framebuffers, lecturas...), nunca por instrucción ni por vértice, así que
 * puede ir siempre activo. hle.c escribe el resumen en wiisp.log cada 30 s
 * reales ([TIEMPOS]).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_PROF_H
#define WIISP_PROF_H

#include "core/types.h"

enum {
	PROF_OTRO,      /* lo que no está en otra cubeta (bucle principal, mando...) */
	PROF_CPU,       /* el intérprete ejecutando instrucciones de la PSP */
	PROF_HLE,       /* dentro de syscalls (sin el GE, la E/S ni el audio) */
	PROF_GE,        /* listas del GE: comandos, vértices y envío a GX */
	PROF_RASTER,    /* rasterizado por software (modo exacto) */
	PROF_TEXTURAS,  /* buscar (hash) y decodificar texturas */
	PROF_FB,        /* framebuffers: bajar/subir entre GX y la memoria de la PSP */
	PROF_PRESENTAR, /* componer y mostrar la imagen en la tele */
	PROF_ESPERA_GX, /* esperar a que la GPU del Wii termine (GX_DrawDone) */
	PROF_ES,        /* leer y escribir archivos (ISO, partidas) */
	PROF_AUDIO,     /* mezcla de sceAudio y sceSas */
	PROF_REGISTRO,  /* escribir wiisp.log */
	PROF_N
};

extern u64 (*prof_clock)(void);
extern u64 prof_hz;
extern u64 prof_acc[PROF_N];
extern u64 prof_last;
extern volatile int prof_cur;   /* volatile: lo lee el temporizador del muestreo */

/* Reloj del anfitrión (ticks y ticks por segundo); sin él no se mide */
void prof_set_clock(u64 (*clock)(void), u64 hz);

/* Pasa a contar en la cubeta b; devuelve la anterior para volver a ella */
static inline int prof_switch(int b){
	int old = prof_cur;
	if(prof_clock){
		u64 now = prof_clock();
		prof_acc[old] += now - prof_last;
		prof_last = now;
	}
	prof_cur = b;
	return old;
}

/* Cuenta lo pendiente de la cubeta actual (antes de leer prof_acc) */
void prof_flush(void);

/* --- Qué hace el GE por dentro (muestreo) ---------------------------------
   Medir con el reloj cada vértice costaría más que el propio vértice. En
   su lugar, un temporizador del anfitrión llama a prof_sample() unas 1000
   veces por segundo y apunta la cubeta actual y, dentro del GE, la fase en
   marcha. Cambiar de fase solo escribe una variable:

       int old = prof_ge_enter(GEF_LEER);
       ...
       prof_ge_leave(old);
*/
enum {
	GEF_COMANDOS,     /* leer y aplicar comandos, preparar cada dibujo */
	GEF_LEER,         /* decodificar vértices (formato, huesos, morph) */
	GEF_TRANSFORMAR,  /* transformar, luces, uv y niebla */
	GEF_ENSAMBLAR,    /* triángulos: descarte, recorte, pantalla */
	GEF_ENVIAR,       /* mandar cada primitiva al renderizador (GX) */
	GEF_ESTADO,       /* preparar búferes y estado de GX */
	GEF_CURVAS,       /* teselar curvas (bezier/spline) */
	GEF_N
};

extern volatile int prof_ge_phase;
extern volatile u32 prof_samples[PROF_N], prof_ge_samples[GEF_N];

static inline int prof_ge_enter(int f){
	int old = prof_ge_phase;
	prof_ge_phase = f;
	return old;
}

static inline void prof_ge_leave(int old){ prof_ge_phase = old; }

/* Desde el temporizador (interrupción o hilo aparte) */
void prof_sample(void);

#endif
