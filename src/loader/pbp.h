/**
 * WIISP - pbp.h
 * Contenedor EBOOT.PBP: cabecera de 40 bytes con 8 offsets a sus secciones.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_PBP_H
#define WIISP_PBP_H

#include "core/types.h"

enum {
	PBP_PARAM_SFO = 0,
	PBP_ICON0_PNG,
	PBP_ICON1_PMF,
	PBP_PIC0_PNG,
	PBP_PIC1_PNG,
	PBP_SND0_AT3,
	PBP_DATA_PSP,   /* el ejecutable (ELF/PRX, o "~PSP" si está cifrado) */
	PBP_DATA_PSAR,
	PBP_NUM_SECTIONS
};

typedef struct {
	u32 version;
	const u8 *data[PBP_NUM_SECTIONS];
	u32 size[PBP_NUM_SECTIONS];
} PbpFile;

int pbp_is_pbp(const u8 *buf, u32 len);
/* Valida la cabecera y apunta cada sección dentro de buf (sin copiar). */
int pbp_parse(const u8 *buf, u32 len, PbpFile *out);
const char *pbp_section_name(int section);

#endif
