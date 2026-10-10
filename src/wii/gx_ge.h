/**
 * WIISP - gx_ge.h
 * El GE de la PSP dibujado con GX, la GPU del Wii (ver gx_ge.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_GX_GE_H
#define WIISP_GX_GE_H

#include <stddef.h>
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

/* Ticks gastados desde la última llamada: preparar búferes y texturas,
   estado de GX y presentar; counts: presentaciones desde la VRAM, bajadas
   a la VRAM, subidas desde la VRAM y texturas decodificadas */
void gx_ge_profile(unsigned long long *setup, unsigned long long *state, unsigned long long *present,
                   unsigned *counts);

/* Espaciado de las comprobaciones de texturas (gpu/texcache.h); activado
   por defecto */
void gx_ge_set_lazy_textures(int on);

/* Qué hizo la caché de texturas desde la última llamada (una línea) */
void gx_ge_texture_report(char *buf, size_t size);

/* Copia a la VRAM emulada todo lo que solo está en la GPU */
void gx_ge_sync_vram(void);

#endif
