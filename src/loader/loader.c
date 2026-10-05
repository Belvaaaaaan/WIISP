/**
 * WIISP - loader.c
 * Punto de entrada del cargador: detecta PBP o ELF (ver loader.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include "loader/loader.h"
#include "loader/pbp.h"
#include "loader/sfo.h"

int loader_load(const u8 *buf, u32 len, u32 prx_base, PspModule *mod){
	PbpFile pbp;
	SfoFile sfo;
	char title[sizeof(mod->title)] = "", disc_id[sizeof(mod->disc_id)] = "";
	int err;

	if(!pbp_is_pbp(buf, len))
		return loader_load_elf(buf, len, prx_base, mod);

	memset(mod, 0, sizeof(*mod));
	err = pbp_parse(buf, len, &pbp);
	if(err) return err;
	if(!pbp.data[PBP_DATA_PSP]) return LOADER_ERR_CORRUPT;

	if(pbp.data[PBP_PARAM_SFO] &&
	   sfo_parse(pbp.data[PBP_PARAM_SFO], pbp.size[PBP_PARAM_SFO], &sfo) == LOADER_OK){
		sfo_get_string(&sfo, "TITLE", title, sizeof(title));
		sfo_get_string(&sfo, "DISC_ID", disc_id, sizeof(disc_id));
	}

	err = loader_load_elf(pbp.data[PBP_DATA_PSP], pbp.size[PBP_DATA_PSP], prx_base, mod);
	if(err) return err;
	memcpy(mod->title, title, sizeof(title));
	memcpy(mod->disc_id, disc_id, sizeof(disc_id));
	return LOADER_OK;
}

void loader_free(PspModule *mod){
	free(mod->imports);
	memset(mod, 0, sizeof(*mod));
}

const char *loader_strerror(int err){
	switch(err){
	case LOADER_OK:              return "correcto";
	case LOADER_ERR_FORMAT:      return "formato no reconocido (no es PBP ni ELF de PSP)";
	case LOADER_ERR_CORRUPT:     return "archivo corrupto o truncado";
	case LOADER_ERR_ENCRYPTED:   return "ejecutable cifrado (~PSP), aun no soportado";
	case LOADER_ERR_MEMORY:      return "no cabe en la RAM emulada";
	case LOADER_ERR_UNSUPPORTED: return "caracteristica no soportada todavia";
	case LOADER_ERR_NOMEM:       return "sin memoria en el host";
	default:                     return "error desconocido";
	}
}
