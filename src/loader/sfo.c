/**
 * WIISP - sfo.c
 * PARAM.SFO (ver sfo.h).
 *
 * Cabecera (20 bytes): "\0PSF", versión, offset de claves, offset de datos,
 * número de entradas. Cada entrada (16 bytes): u16 offset de clave,
 * u16 formato, u32 longitud usada, u32 longitud máxima, u32 offset de dato.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "loader/sfo.h"
#include "loader/loader.h"

#define SFO_HEADER_SIZE 20
#define SFO_ENTRY_SIZE  16

int sfo_parse(const u8 *buf, u32 len, SfoFile *out){
	memset(out, 0, sizeof(*out));
	if(!buf || len < SFO_HEADER_SIZE) return LOADER_ERR_FORMAT;
	if(buf[0] != 0 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F')
		return LOADER_ERR_FORMAT;

	out->buf = buf;
	out->len = len;
	out->key_table = rd_le32(buf + 8);
	out->data_table = rd_le32(buf + 12);
	out->count = rd_le32(buf + 16);

	if(out->count > (len - SFO_HEADER_SIZE) / SFO_ENTRY_SIZE ||
	   out->key_table > len || out->data_table > len)
		return LOADER_ERR_CORRUPT;
	return LOADER_OK;
}

/* Busca key; devuelve un puntero a la entrada o NULL */
static const u8 *find_entry(const SfoFile *sfo, const char *key){
	u32 i, key_len = (u32)strlen(key);
	for(i = 0; i < sfo->count; i++){
		const u8 *e = sfo->buf + SFO_HEADER_SIZE + i*SFO_ENTRY_SIZE;
		u32 key_off = sfo->key_table + rd_le16(e);
		if(key_off >= sfo->len || sfo->len - key_off <= key_len) continue;
		if(!memcmp(sfo->buf + key_off, key, key_len + 1)) return e;
	}
	return NULL;
}

/* Devuelve el dato de una entrada si está dentro del archivo */
static const u8 *entry_data(const SfoFile *sfo, const u8 *e, u32 *size){
	u32 data_len = rd_le32(e + 4);
	u32 off = sfo->data_table + rd_le32(e + 12);
	if(off > sfo->len || data_len > sfo->len - off) return NULL;
	*size = data_len;
	return sfo->buf + off;
}

int sfo_get_string(const SfoFile *sfo, const char *key, char *dst, u32 dst_size){
	const u8 *e, *data;
	u32 size, n;

	if(!dst_size) return -1;
	dst[0] = 0;
	e = find_entry(sfo, key);
	if(!e) return -1;
	if(rd_le16(e + 2) != SFO_FMT_UTF8 && rd_le16(e + 2) != SFO_FMT_UTF8_RAW) return -1;
	data = entry_data(sfo, e, &size);
	if(!data) return -1;

	for(n = 0; n < size && n + 1 < dst_size && data[n]; n++)
		dst[n] = (char)data[n];
	dst[n] = 0;
	return 0;
}

int sfo_get_int(const SfoFile *sfo, const char *key, u32 *out){
	const u8 *e = find_entry(sfo, key), *data;
	u32 size;
	if(!e || rd_le16(e + 2) != SFO_FMT_INT32) return -1;
	data = entry_data(sfo, e, &size);
	if(!data || size < 4) return -1;
	*out = rd_le32(data);
	return 0;
}
