/**
 * WIISP - sfo.h
 * PARAM.SFO: tabla clave/valor con los metadatos del juego (título, ID...).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_SFO_H
#define WIISP_SFO_H

#include "core/types.h"

#define SFO_FMT_UTF8_RAW  0x0004   /* utf8 sin terminar en 0 */
#define SFO_FMT_UTF8      0x0204
#define SFO_FMT_INT32     0x0404

typedef struct {
	const u8 *buf;
	u32 len;
	u32 key_table;
	u32 data_table;
	u32 count;
} SfoFile;

int sfo_parse(const u8 *buf, u32 len, SfoFile *out);
/* Copia el valor de texto de key a dst (siempre terminado en 0).
   Devuelve 0 si existe, -1 si no. */
int sfo_get_string(const SfoFile *sfo, const char *key, char *dst, u32 dst_size);
int sfo_get_int(const SfoFile *sfo, const char *key, u32 *out);

#endif
