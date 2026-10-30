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
void gx_ge_prueba(void);   //Arreglo

/* Si el framebuffer que muestra la PSP está en la GPU, lo copia escalado
   al XFB y devuelve 1. Devuelve 0 si hay que convertirlo desde la VRAM. */
int  gx_ge_present(void *xfb, GXRModeObj *rmode, int widescreen);
/* Con gx_paralelo, gx_ge_present vuelve sin esperar a que la GPU acabe la
   imagen: esto espera a que esté en el XFB (1) o no hay ninguna pendiente (0) */
int  gx_ge_present_wait(void);
/* ¿La última gx_ge_present dejó la imagen a medias en la GPU? */
int  gx_ge_present_pendiente(void);

/* Ticks gastados desde la última llamada: preparar búferes y texturas,
   estado de GX y presentar; counts: presentaciones desde la VRAM, bajadas
   a la VRAM, subidas desde la VRAM y texturas decodificadas */
void gx_ge_profile(unsigned long long *setup, unsigned long long *state, unsigned long long *present,
                   unsigned *counts);

/* Espaciado de las comprobaciones de texturas (gpu/texcache.h); activado
   por defecto */
void gx_ge_set_lazy_textures(int on);
/* Diagnóstico del warping: 1 (normal) = perspectiva de las texturas
   normalizada por triángulo; 0 = s/w, t/w y 1/w tal cual (hasta la 0.5.3) */
void gx_ge_set_tex_norm(int on);
int  gx_ge_tex_norm(void);

/* Qué hizo la caché de texturas desde la última llamada (una línea) */
void gx_ge_texture_report(char *buf, size_t size);
/* Censo de destinos de dibujo desde la última llamada (varias líneas "rtt:") */
void gx_ge_rtt_report(char *buf, size_t size);

/* Copia a la VRAM emulada todo lo que solo está en la GPU */
void gx_ge_sync_vram(void);

/* Memoria del renderizador: texturas decodificadas (bytes y cuántas),
   búferes vivos (cuántos y bytes) y bloques pendientes de liberar */
void gx_ge_memory(unsigned *tex_bytes, unsigned *tex_n, unsigned *surf_n, unsigned *surf_bytes, unsigned *deferred);
#endif
