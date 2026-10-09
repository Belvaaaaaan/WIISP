/**
 * WIISP - disc.c
 * Imágenes de UMD y su sistema de archivos ISO 9660 (ver disc.h).
 *
 * CSO y ZSO comparten formato: cabecera de 24 bytes ("CISO"/"ZISO",
 * tamaño de cabecera, tamaño total en 64 bits, tamaño de bloque, versión
 * y alineación) y un índice de (bloques + 1) palabras; cada una es la
 * posición del bloque desplazada por la alineación, con el bit 31 a 1 si
 * el bloque va sin comprimir. CSO usa deflate sin envoltorio; ZSO, LZ4.
 * Se guarda en memoria el último bloque descomprimido.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "loader/disc.h"
#include "loader/inflate.h"
#include "core/prof.h"

enum { FMT_NONE, FMT_ISO, FMT_CSO, FMT_ZSO };

#define MAX_FRAME (64u * 1024)

static FILE *file;
static int format;
static u64 total_size;
static u32 frame_size, index_shift, num_frames;
static u32 *frame_index;
static u8 *frame_buf, *comp_buf;
static u32 cached_frame = 0xFFFFFFFFu;
static DiscEntry root;

static int file_read_at(u64 pos, void *out, u32 size){
	int n, old = prof_switch(PROF_ES);
	if(fseek(file, (long)pos, SEEK_SET)) n = 0;
	else n = (int)fread(out, 1, size, file);
	prof_switch(old);
	return n;
}

void disc_close(void){
	if(file) fclose(file);
	file = NULL;
	format = FMT_NONE;
	free(frame_index); frame_index = NULL;
	free(frame_buf); frame_buf = NULL;
	free(comp_buf); comp_buf = NULL;
	cached_frame = 0xFFFFFFFFu;
	total_size = 0;
}

int disc_is_open(void){ return format != FMT_NONE; }

const char *disc_format(void){
	return format == FMT_CSO ? "CSO" : format == FMT_ZSO ? "ZSO" : format == FMT_ISO ? "ISO" : "";
}

u32 disc_sectors(void){ return (u32)(total_size / DISC_SECTOR); }

static int open_compressed(const u8 *hdr){
	u32 header_size = rd_le32(hdr + 4), i, n;
	total_size = (u64)rd_le32(hdr + 8) | ((u64)rd_le32(hdr + 12) << 32);
	frame_size = rd_le32(hdr + 16);
	index_shift = hdr[21];
	if(hdr[20] > 1 || frame_size < DISC_SECTOR || frame_size > MAX_FRAME || (frame_size & (frame_size - 1)) ||
	   index_shift > 20 || total_size == 0 || total_size > (4ull << 30))
		return -1;
	num_frames = (u32)((total_size + frame_size - 1) / frame_size);
	n = num_frames + 1;
	frame_index = malloc(n * 4);
	frame_buf = malloc(frame_size);
	comp_buf = malloc(frame_size + 1024);
	if(!frame_index || !frame_buf || !comp_buf) return -1;
	if((u32)file_read_at(header_size < 24 ? 24 : header_size, frame_index, n * 4) != n * 4) return -1;
	for(i = 0; i < n; i++) frame_index[i] = rd_le32((const u8 *)&frame_index[i]);
	return 0;
}

int disc_open(const char *path){
	u8 hdr[24];
	disc_close();
	file = fopen(path, "rb");
	if(!file) return -1;
	if(file_read_at(0, hdr, 24) != 24){ disc_close(); return -2; }
	if(!memcmp(hdr, "CISO", 4) || !memcmp(hdr, "ZISO", 4)){
		format = hdr[0] == 'C' ? FMT_CSO : FMT_ZSO;
		if(open_compressed(hdr)){ disc_close(); return -2; }
	} else {
		long end;
		format = FMT_ISO;
		if(fseek(file, 0, SEEK_END) || (end = ftell(file)) <= 0){ disc_close(); return -2; }
		total_size = (u64)end;
	}

	/* Descriptor de volumen primario en el sector 16 */
	{
		u8 pvd[DISC_SECTOR];
		if(disc_read(16 * DISC_SECTOR, DISC_SECTOR, pvd) != DISC_SECTOR || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)){
			disc_close();
			return -3;
		}
		memset(&root, 0, sizeof(root));
		root.lba = rd_le32(pvd + 156 + 2);
		root.size = rd_le32(pvd + 156 + 10);
		root.is_dir = 1;
	}
	return 0;
}

/* Un bloque de CSO/ZSO en frame_buf */
static int load_frame(u32 f){
	u32 a, b, len;
	u64 pos, end;
	int plain;
	if(f == cached_frame) return 0;
	if(f >= num_frames) return -1;
	a = frame_index[f];
	b = frame_index[f + 1];
	pos = (u64)(a & 0x7FFFFFFF) << index_shift;
	end = (u64)(b & 0x7FFFFFFF) << index_shift;
	plain = (a & 0x80000000u) != 0;
	if(end < pos || (!plain && end - pos > frame_size + 1024)) return -1;
	len = (u32)(end - pos);
	if(plain){
		u32 got = (u32)file_read_at(pos, frame_buf, frame_size);
		if(got < frame_size) memset(frame_buf + got, 0, frame_size - got);
	} else {
		int n;
		if((u32)file_read_at(pos, comp_buf, len) != len) return -1;
		if(format == FMT_CSO) n = inflate_raw(comp_buf, len, frame_buf, frame_size, NULL);
		else n = lz4_decompress(comp_buf, len, frame_buf, frame_size);
		if(n < 0){ cached_frame = 0xFFFFFFFFu; return -1; }
		if((u32)n < frame_size) memset(frame_buf + n, 0, frame_size - (u32)n);
	}
	cached_frame = f;
	return 0;
}

u32 disc_read(u64 offset, u32 size, void *out){
	u8 *o = out;
	u32 done = 0;
	if(!disc_is_open() || offset >= total_size) return 0;
	if(size > total_size - offset) size = (u32)(total_size - offset);
	if(format == FMT_ISO) return (u32)file_read_at(offset, out, size);
	while(done < size){
		u64 p = offset + done;
		u32 f = (u32)(p / frame_size), in = (u32)(p % frame_size), n = frame_size - in;
		if(n > size - done) n = size - done;
		if(load_frame(f)) break;
		memcpy(o + done, frame_buf + in, n);
		done += n;
	}
	return done;
}

/* --- ISO 9660 ------------------------------------------------------------------ */

/* Registro de directorio -> entrada. Devuelve 0 si es "." o ".." */
static int parse_record(const u8 *r, DiscEntry *e){
	u32 nlen = r[32], i;
	if(nlen == 1 && (r[33] == 0 || r[33] == 1)) return 0;
	e->lba = rd_le32(r + 2);
	e->size = rd_le32(r + 10);
	e->is_dir = (r[25] & 2) != 0;
	if(nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
	memcpy(e->name, r + 33, nlen);
	e->name[nlen] = 0;
	/* Fuera la versión ";1" y el punto final de los nombres sin extensión */
	for(i = 0; i < nlen; i++) if(e->name[i] == ';'){ e->name[i] = 0; nlen = i; break; }
	if(nlen > 1 && e->name[nlen - 1] == '.') e->name[nlen - 1] = 0;
	return 1;
}

int disc_dir_entry(const DiscEntry *dir, int index, DiscEntry *out){
	u8 sec[DISC_SECTOR];
	u32 off, n = 0;
	if(!disc_is_open() || !dir->is_dir) return 0;
	for(off = 0; off < dir->size; off += DISC_SECTOR){
		u32 p = 0;
		if(disc_read((u64)dir->lba * DISC_SECTOR + off, DISC_SECTOR, sec) != DISC_SECTOR) return 0;
		while(p + 34 <= DISC_SECTOR){
			u32 len = sec[p];
			if(len == 0) break;            /* resto del sector vacío */
			if(len < 34 || p + len > DISC_SECTOR || 33u + sec[p + 32] > len) break;
			if(parse_record(sec + p, out)){
				if((int)n == index) return 1;
				n++;
			}
			p += len;
		}
	}
	return 0;
}

static int name_eq(const char *a, const char *b, u32 blen){
	u32 i;
	for(i = 0; i < blen; i++)
		if(!a[i] || toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return 0;
	return a[blen] == 0;
}

int disc_lookup(const char *path, DiscEntry *out){
	DiscEntry cur = root;
	if(!disc_is_open()) return -1;
	for(;;){
		const char *end;
		u32 len;
		int i, found = 0;
		while(*path == '/') path++;
		if(!*path) break;
		end = strchr(path, '/');
		len = end ? (u32)(end - path) : (u32)strlen(path);
		if(len == 1 && path[0] == '.'){ path += len; continue; }
		if(!cur.is_dir) return -1;
		for(i = 0; ; i++){
			DiscEntry e;
			if(!disc_dir_entry(&cur, i, &e)) break;
			if(name_eq(e.name, path, len)){ cur = e; found = 1; break; }
		}
		if(!found) return -1;
		path += len;
	}
	*out = cur;
	if(cur.lba == root.lba) strcpy(out->name, "");
	return 0;
}

u8 *disc_read_file(const char *path, u32 *size, u32 max){
	DiscEntry e;
	u8 *buf;
	if(disc_lookup(path, &e) || e.is_dir || e.size > max) return NULL;
	buf = malloc(e.size ? e.size : 1);
	if(!buf) return NULL;
	if(disc_read((u64)e.lba * DISC_SECTOR, e.size, buf) != e.size){ free(buf); return NULL; }
	*size = e.size;
	return buf;
}
