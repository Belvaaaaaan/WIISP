/**
 * WIISP - frontend/guion.h
 * Guion de botones: qué tiene pulsado el mando de la PSP en cada cuadro
 * emulado, para que una escena de prueba se repita exactamente igual.
 *
 * Formato (texto, una línea por cambio; '#' empieza un comentario):
 *
 *     # cuadro  botones (se mantienen hasta la siguiente línea)
 *     600   START
 *     606   -
 *     900   CRUZ
 *     1500  DERECHA stick 255 128
 *     3000  captura
 *     5400  fin
 *
 *   - El número es el cuadro (vblank) emulado desde el arranque, contado
 *     igual que en app_run_frame: lo de la línea N se aplica justo antes de
 *     emular el cuadro N.
 *   - Una línea con botones fija el estado completo del mando: los botones
 *     que no aparecen quedan sueltos y el stick vuelve al centro si no se
 *     indica. "-" es "nada pulsado".
 *   - Botones: SELECT START ARRIBA DERECHA ABAJO IZQUIERDA L R TRIANGULO
 *     CIRCULO CRUZ CUADRADO (da igual mayúsculas o minúsculas).
 *   - "stick X Y": stick analógico, 0-255 (128 es el centro; Y = 0 arriba).
 *   - "captura": guarda la imagen en ese momento (no cambia el mando).
 *   - "fin": termina la prueba.
 *
 * Este módulo solo lee y escribe guiones; quien los reproduce o graba
 * durante la emulación es app.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_GUION_H
#define WIISP_GUION_H

#include <stddef.h>
#include "core/types.h"

enum {
	GUION_MANDO = 1,     /* la línea fija botones y stick */
	GUION_CAPTURA = 2,
	GUION_FIN = 4
};

typedef struct {
	u32 cuadro;
	u32 botones;         /* APP_BTN_* */
	u8 lx, ly;
	u8 flags;            /* GUION_* */
} GuionLinea;

typedef struct {
	GuionLinea *l;
	u32 n, cap;
} Guion;

/* Lee un guion desde texto o desde un archivo. 0 = correcto; si falla, err
   dice la línea y el motivo y el guion queda vacío. */
int  guion_parse(Guion *g, const char *texto, char *err, size_t errlen);
int  guion_load(Guion *g, const char *path, char *err, size_t errlen);
void guion_free(Guion *g);

/* Cuadro de la línea "fin" (0 si no hay) */
u32  guion_fin(const Guion *g);

/* Escribe una línea de mando ("900 CRUZ stick 128 0"), sin salto de línea.
   Devuelve la longitud. */
int  guion_format(char *out, size_t size, u32 cuadro, u32 botones, u8 lx, u8 ly);

/* Al grabar, el stick se redondea para que el guion no tenga una línea por
   cada temblor del mando: zona muerta de ±24 alrededor del centro y pasos de
   8. El juego recibe ya el valor redondeado, así que la grabación y la
   reproducción ven exactamente lo mismo. */
u8   guion_cuantizar(u8 eje);

/* Partidas de una prueba: vacía `dir` y copia dentro el contenido de `base`
   (si existe), para que cada corrida empiece con las mismas partidas.
   Por seguridad solo acepta carpetas cuyo nombre termine en ".partidas".
   0 = correcto. */
int  guion_preparar_partidas(const char *dir, const char *base);

#endif
