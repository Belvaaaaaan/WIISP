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
#include "loader/prx_decrypt.h"
#include "loader/inflate.h"

#define MAX_ELF_SIZE (24u * 1024 * 1024)

u32 loader_last_tag;

/* Quita los envoltorios de un ejecutable de PSP: la cabecera "~SCE" y el
   cifrado "~PSP" (con gzip si comp_attribute lo pide). Si hace falta
   memoria, *owned la recibe y hay que liberarla. Con inplace, buf se
   descifra en su sitio (el Wii no tiene memoria para varias copias de un
   EBOOT de varios MB). */
static int unwrap(const u8 *buf, u32 len, const u8 **elf, u32 *elf_len, u8 **owned, int inplace){
	*owned = NULL;
	if(len >= 8 && !memcmp(buf, "~SCE", 4)){
		u32 hs = rd_le32(buf + 4);
		if(hs < len){ buf += hs; len -= hs; }
	}
	*elf = buf;
	*elf_len = len;
	if(len > 0x150 + 4 && !memcmp(buf, "~PSP", 4)){
		u32 comp = rd_le16(buf + 6), elf_size = rd_le32(buf + 0x28), psp_size = rd_le32(buf + 0x2C);
		u32 max = elf_size > psp_size ? elf_size : psp_size;
		u8 *out;
		int n;
		loader_last_tag = rd_le32(buf + 0xD0);
		if(psp_size > len || psp_size < 0x150 || max > MAX_ELF_SIZE) return LOADER_ERR_ENCRYPTED;
		if(inplace && !(comp & 1) && elf_size <= psp_size) out = (u8 *)buf;
		else {
			out = malloc(max);
			if(!out) return LOADER_ERR_NOMEM;
		}
		n = prx_decrypt(buf, out, psp_size, NULL);
		/* Sin cifrar de verdad: el ELF va justo detrás de la cabecera */
		if(n <= 0 && rd_le32(buf + 0x150) == 0x464C457Fu){
			n = (int)(psp_size - 0x150);
			memmove(out, buf + 0x150, (u32)n);
			comp = 0;
		}
		if(out == buf){
			if(n <= 0 || (u32)n > max) return LOADER_ERR_ENCRYPTED;
			*elf_len = (u32)n;
			return LOADER_OK;
		}
		if(n <= 0 || (u32)n > max){ free(out); return LOADER_ERR_ENCRYPTED; }
		if(comp & 1){
			u8 *z = malloc((u32)n);
			int m;
			if(!z){ free(out); return LOADER_ERR_NOMEM; }
			memcpy(z, out, (u32)n);
			m = gunzip(z, (u32)n, out, max);   /* KL4E/KL3E: aún no */
			free(z);
			if(m <= 0){ free(out); return LOADER_ERR_UNSUPPORTED; }
			n = m;
		}
		*owned = out;
		*elf = out;
		*elf_len = (u32)n;
	}
	return LOADER_OK;
}

int loader_unwrap(u8 *buf, u32 len, const u8 **elf, u32 *elf_len, u8 **owned){
	return unwrap(buf, len, elf, elf_len, owned, 1);
}

void loader_psp_name(const u8 *buf, u32 len, char *out, u32 out_size){
	u32 n = 0;
	out[0] = 0;
	if(len >= 8 && !memcmp(buf, "~SCE", 4)){
		u32 hs = rd_le32(buf + 4);
		if(hs < len){ buf += hs; len -= hs; }
	}
	if(len < 0x26 || memcmp(buf, "~PSP", 4) || !out_size) return;
	while(n + 1 < out_size && n < 28 && buf[0x0A + n]){ out[n] = (char)buf[0x0A + n]; n++; }
	out[n] = 0;
}

static int load_unwrapped(const u8 *buf, u32 len, u32 prx_base, PspModule *mod, int inplace){
	const u8 *elf;
	u32 elf_len;
	u8 *owned;
	int err = unwrap(buf, len, &elf, &elf_len, &owned, inplace);
	if(err){ memset(mod, 0, sizeof(*mod)); return err; }
	err = loader_load_elf(elf, elf_len, prx_base, mod);
	free(owned);
	return err;
}

static int load_any(const u8 *buf, u32 len, u32 prx_base, PspModule *mod, int inplace){
	PbpFile pbp;
	SfoFile sfo;
	char title[sizeof(mod->title)] = "", disc_id[sizeof(mod->disc_id)] = "";
	int err;

	if(!pbp_is_pbp(buf, len))
		return load_unwrapped(buf, len, prx_base, mod, inplace);

	memset(mod, 0, sizeof(*mod));
	err = pbp_parse(buf, len, &pbp);
	if(err) return err;
	if(!pbp.data[PBP_DATA_PSP]) return LOADER_ERR_CORRUPT;

	if(pbp.data[PBP_PARAM_SFO] &&
	   sfo_parse(pbp.data[PBP_PARAM_SFO], pbp.size[PBP_PARAM_SFO], &sfo) == LOADER_OK){
		sfo_get_string(&sfo, "TITLE", title, sizeof(title));
		sfo_get_string(&sfo, "DISC_ID", disc_id, sizeof(disc_id));
	}

	err = load_unwrapped(pbp.data[PBP_DATA_PSP], pbp.size[PBP_DATA_PSP], prx_base, mod, inplace);
	if(err) return err;
	memcpy(mod->title, title, sizeof(title));
	memcpy(mod->disc_id, disc_id, sizeof(disc_id));
	return LOADER_OK;
}

int loader_load(const u8 *buf, u32 len, u32 prx_base, PspModule *mod){
	return load_any(buf, len, prx_base, mod, 0);
}

int loader_load_inplace(u8 *buf, u32 len, u32 prx_base, PspModule *mod){
	return load_any(buf, len, prx_base, mod, 1);
}

void loader_free(PspModule *mod){
	free(mod->imports);
	free(mod->exports);
	memset(mod, 0, sizeof(*mod));
}

const char *loader_strerror(int err){
	switch(err){
	case LOADER_OK:              return "correcto";
	case LOADER_ERR_FORMAT:      return "formato no reconocido (no es PBP ni ELF de PSP)";
	case LOADER_ERR_CORRUPT:     return "archivo corrupto o truncado";
	case LOADER_ERR_ENCRYPTED:   return "ejecutable cifrado (~PSP) que no se pudo descifrar";
	case LOADER_ERR_MEMORY:      return "no cabe en la RAM emulada";
	case LOADER_ERR_UNSUPPORTED: return "caracteristica no soportada todavia";
	case LOADER_ERR_NOMEM:       return "sin memoria en el host";
	default:                     return "error desconocido";
	}
}
