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
	u32 vertices;     /* vértices calculados (decodificar + transformar) */
	u64 pixels;
	u32 lists;        /* veces que se procesó la cola de listas */
	u64 host_ticks;   /* tiempo del anfitrión dentro del GE (si hay reloj) */

	/* Detalle para [TIEMPOS] */
	u32 draws;              /* llamadas de dibujo (PRIM) */
	u32 draws_indexed;      /* con índices */
	u32 draws_reuse;        /* con índices repetidos: cada vértice se calcula una vez */
	u32 draws_prim[8];      /* por tipo de primitiva (GE_PRIM_*) */
	u32 curves;             /* bezier y spline */
	u32 vertex_reads;       /* vértices que pidieron las primitivas */
	u32 vertices_skinned;   /* calculados con huesos */
	u32 vertices_morph;
	u32 vertices_through;   /* ya en coordenadas de pantalla (2D) */
	u32 vertices_lit;       /* con luces */
	u32 lights;             /* suma de luces encendidas de los iluminados */
	u32 tris;               /* triángulos recibidos */
	u32 tris_drawn;
	u32 tris_back;          /* de espaldas o sin área */
	u32 tris_outside;       /* fuera de la pantalla o de la tijera */
	u32 tris_clipped;       /* cortados por el plano cercano */
	u32 sprites;
} GeStats;
void ge_get_stats(GeStats *out);

/* Sube cada vez que la CPU se sincroniza con el GE (sceGeDrawSync,
   sceGeListSync, fin de una lista) y cuando un framebuffer de la GPU baja a
   la memoria. Una textura no puede cambiar dentro de un mismo periodo sin
   avisar (como textureSyncTimeDomain de PPSSPP): las cachés de texturas la
   comprueban como mucho una vez por periodo. */
extern u32 ge_sync_domain;

/* Turbo: las llamadas de dibujo (PRIM, bezier, spline) solo avanzan sus
   direcciones, sin calcular vértices ni dibujar. Lo demás del GE (estado,
   transferencias, CLUT, señales) sigue igual, así que el juego avanza
   mucho más rápido mientras la pantalla se queda quieta. */
extern int ge_turbo;

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
