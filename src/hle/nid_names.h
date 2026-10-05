/**
 * WIISP - nid_names.h
 * Nombres de las funciones del firmware de la PSP por NID (para logs y
 * para imports.txt). La tabla se genera con tools/gen_nids.py.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_NID_NAMES_H
#define WIISP_NID_NAMES_H

#include "core/types.h"

typedef struct {
	u32 nid;
	const char *name;
} NidName;

extern const NidName nid_names[];   /* ordenada por NID */
extern const u32 nid_names_count;

/* Devuelve el nombre o NULL si el NID no está en el PSPSDK */
const char *nid_lookup(u32 nid);

#endif
