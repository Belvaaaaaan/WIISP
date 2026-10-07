/**
 * WIISP - disc.h
 * Imágenes de UMD: ISO, CSO (deflate) y ZSO (LZ4), con su sistema de
 * archivos ISO 9660. Hay un único disco montado a la vez, como en la PSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_DISC_H
#define WIISP_DISC_H

#include "core/types.h"

#define DISC_SECTOR 2048

/* Monta la imagen. 0 = correcto; < 0 = no se pudo abrir o no es una
   imagen válida */
int  disc_open(const char *path);
void disc_close(void);
int  disc_is_open(void);
/* "ISO", "CSO" o "ZSO" */
const char *disc_format(void);
u32  disc_sectors(void);

/* Lee bytes del disco. Devuelve los leídos (menos al final del disco) */
u32  disc_read(u64 offset, u32 size, void *out);

typedef struct {
	u32 lba;          /* primer sector */
	u32 size;         /* bytes */
	int is_dir;
	char name[128];   /* sin el ";1" */
} DiscEntry;

/* Busca una ruta desde la raíz ("PSP_GAME/SYSDIR/EBOOT.BIN"; vale "" o
   "/" para la raíz). Sin distinguir mayúsculas. 0 = encontrada. */
int  disc_lookup(const char *path, DiscEntry *out);

/* La entrada index (0, 1...) de un directorio, sin "." ni "..".
   Devuelve 1 si existe, 0 al acabar. */
int  disc_dir_entry(const DiscEntry *dir, int index, DiscEntry *out);

/* Lee un archivo entero del disco. Devuelve un búfer de malloc (y su
   tamaño en *size) o NULL. max: tamaño máximo aceptado. */
u8  *disc_read_file(const char *path, u32 *size, u32 max);

#endif
