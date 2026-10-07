/**
 * WIISP - ge.h
 * GE (Graphics Engine) de la PSP: interfaz pública.
 *
 * El GE ejecuta listas de comandos de 32 bits (8 bits de comando y 24 de
 * argumento) que el juego escribe en memoria y encola con sceGeListEnQueue.
 * Aquí se interpretan de forma síncrona: cada vez que la lista avanza
 * (encolado o actualización del "stall") se ejecuta hasta donde se pueda.
 *
 * El dibujo por software escribe directamente en la VRAM emulada, igual que
 * el hardware real, así que el framebuffer y las texturas renderizadas se
 * comportan como en la PSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_GE_H
#define WIISP_GE_H

#include "core/types.h"

void ge_init(void);
void ge_shutdown(void);

/* Estadísticas por frame (para el medidor del frontend) */
typedef struct {
	u32 commands;
	u32 primitives;
	u32 vertices;
	u64 pixels;
	u64 host_ticks;   /* tiempo del anfitrión dentro del GE (si hay reloj) */
} GeStats;
void ge_get_stats(GeStats *out);

/* Reloj del anfitrión para medir el GE (perfilado); NULL = sin medir */
extern unsigned long long (*ge_host_clock)(void);
#ifdef WIISP_PROF
/* Perfilado (make PROF=1): ticks en vértices, backend, ge_draw_prim,
   su preparación, luces y decodificación */
extern unsigned long long ge_prof[8];
#define GE_PROF_T0 unsigned long long prof_t0 = ge_host_clock ? ge_host_clock() : 0
#define GE_PROF_ADD(n) do { if(ge_host_clock) ge_prof[n] += ge_host_clock() - prof_t0; } while(0)
#else
#define GE_PROF_T0 (void)0
#define GE_PROF_ADD(n) (void)0
#endif

#endif
