/**
 * WIISP - app.h
 * Lógica de la aplicación compartida entre plataformas (PC y Wii).
 *
 * Este header solo usa tipos de C estándar a propósito: el frontend del Wii
 * incluye gccore.h, cuyos typedef (u8, u32...) chocarían con core/types.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_APP_H
#define WIISP_APP_H

#include <stddef.h>

/* Implementadas por cada plataforma */
/* Memoria grande y alineada a 32 bytes para la RAM/VRAM de la PSP.
   En el Wii sale de MEM2. Devuelve NULL si no hay. */
void *plat_alloc_big(size_t size);

/* Reserva la memoria de la PSP (32 MB de RAM). 0 = correcto. */
int  app_init(void);

/* Carga un EBOOT.PBP / ELF / PRX, imprime su resumen y lo deja en la
   memoria emulada. max_imports < 0 = listar todos. 0 = correcto. */
int  app_load(const char *path, int max_imports);

#endif
