/**
 * WIISP - gx_ge.h
 * El GE de la PSP dibujado con GX, la GPU del Wii (ver gx_ge.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_GX_GE_H
#define WIISP_GX_GE_H

#include <gccore.h>

/* Una vez, después de GX_Init */
void gx_ge_init(void);

/* 1 = el GE dibuja con GX; 0 = con el rasterizador por software */
void gx_ge_enable(int on);
int  gx_ge_enabled(void);

/* Olvida todos los búferes y texturas (al empezar o acabar un programa) */
void gx_ge_reset(void);

/* Si el framebuffer que muestra la PSP está en la GPU, lo copia escalado
   al XFB y devuelve 1. Devuelve 0 si hay que convertirlo desde la VRAM. */
int  gx_ge_present(void *xfb, GXRModeObj *rmode, int widescreen);

/* Copia a la VRAM emulada todo lo que solo está en la GPU */
void gx_ge_sync_vram(void);

#endif
