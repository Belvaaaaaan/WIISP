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
} GeStats;
void ge_get_stats(GeStats *out);

#endif
