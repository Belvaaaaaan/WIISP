/**
 * WIISP - prx_decrypt.c
 * Descifrado de PRX y EBOOT.BIN cifrados ("~PSP"), ver prx_decrypt.h.
 *
 * Portado de PPSSPP (Core/ELF/PrxDecrypter.cpp, (c) 2012- PPSSPP Project,
 * GPLv2+), basado a su vez en PRXDecrypter y JPCSP. Las claves vienen de
 * prx_keys.h y el KIRK de kirk.c.
 *
 * Una PSP sabe por el firmware qué variante usa cada etiqueta; aquí, como
 * en PPSSPP, se prueban todas: cada una comprueba primero el SHA-1 de la
 * cabecera, así que solo la buena llega a descifrar.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "loader/prx_decrypt.h"
#include "loader/kirk.h"

/* Tabla de etiquetas con la clave XOR de 0x90 bytes ya desordenada
   (tipos 0 y 1) */
typedef struct {
	u32 tag;
	const u32 *key;      /* 36 palabras en little-endian */
	u8 code, code_extra;
	int scramble_key;    /* clave en bruto: pasa antes por el KIRK 7 */
} TAG_INFO;

/* Tabla de etiquetas con semilla de 16 bytes (tipos 2, 5, 6 y 9) */
typedef struct {
	u32 tag;
	const u8 *key;
	u8 code, type;
	const u8 *seed;
} TAG_INFO2;

/* Las tablas de PPSSPP omiten los campos finales que valen 0 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "loader/prx_keys.h"
#pragma GCC diagnostic pop

#define PRX_DECRYPT_MODE_SPRX 23
#define PSP_HEADER_SIZE 0x150
#define HDR_OFFSET (PSP_HEADER_SIZE - KIRK_CMD1_HEADER_SIZE - 0x80)   /* 0x40 */

static const TAG_INFO *get_tag_info(u32 tag){
	u32 i;
	for(i = 0; i < sizeof(g_tagInfo) / sizeof(g_tagInfo[0]); i++)
		if(g_tagInfo[i].tag == tag) return &g_tagInfo[i];
	return NULL;
}

static const TAG_INFO2 *get_tag_info2(u32 tag){
	u32 i;
	for(i = 0; i < sizeof(g_tagInfo2) / sizeof(g_tagInfo2[0]); i++)
		if(g_tagInfo2[i].tag == tag) return &g_tagInfo2[i];
	return NULL;
}

static int all_zero(const u8 *p, u32 n){
	while(n--) if(*p++) return 0;
	return 1;
}

/* Semilla expandida: 9 bloques numerados descifrados con el KIRK 7 */
static void expand_seed(u8 out[0x90], const u8 *seed, int code, const u8 *bonus){
	int i;
	for(i = 0; i < 0x90; i += 0x10){
		memcpy(out + i, seed, 0x10);
		out[i] = (u8)(i / 0x10);
	}
	kirk7(out, out, 0x90, code);
	if(bonus) for(i = 0; i < 0x90; i++) out[i] ^= bonus[i % 0x10];
}

static void decrypt_kirk_header(u8 *out, const u8 *in, const u8 *xorbuf, int code){
	int i;
	for(i = 0; i < 0x40; i++) out[i] = in[i] ^ xorbuf[i];
	kirk7(out, out, 0x40, code);
	for(i = 0; i < 0x40; i++) out[i] ^= xorbuf[0x40 + i];
}

static void decrypt_kirk_header_type0(u8 *out, const u8 *in, const u8 *xorbuf, int code){
	int i;
	for(i = 0; i < 0x70; i++) out[i] = in[i] ^ xorbuf[i + 0x14];
	kirk7(out, out, 0x70, code);
	for(i = 0; i < 0x70; i++) out[i] ^= xorbuf[i + 0x20];
}

static int sha1_matches(const u8 *expected, const u8 *const *parts, const u32 *sizes, int n){
	Sha1 s;
	u8 h[20];
	int i;
	sha1_init(&s);
	for(i = 0; i < n; i++) sha1_update(&s, parts[i], sizes[i]);
	sha1_final(&s, h);
	return !memcmp(h, expected, 20);
}

/* Descifra el bloque del KIRK 1 que empieza en out + HDR_OFFSET. El
   tamaño se lee antes: out puede ser la entrada */
static int finish(const u8 *in, u8 *out, u32 size){
	s32 decrypted = (s32)rd_le32(in + 0xB0);
	if(kirk_cmd1(out, out + HDR_OFFSET, size - HDR_OFFSET)) return -4;
	return decrypted;
}

/* Tipos 0 y 1: la cabecera del KIRK con una clave XOR de 0x90 bytes.
   Se reordena como en PPSSPP: tag, sha1, unused, bloque del KIRK, cabecera */
#define T_TAG 0x00
#define T_SHA1 0x04
#define T_UNUSED 0x18
#define T_KIRK 0x40
#define T_PRXH 0xD0

static int decrypt_type01(const u8 *in, u8 *out, u32 size, int type1){
	const TAG_INFO *pti = get_tag_info(rd_le32(in + 0xD0));
	u8 xorbuf[0x90], t[PSP_HEADER_SIZE];
	const u8 *parts[4];
	u32 sizes[4];
	int i;
	if(!pti) return -1;
	for(i = 0; i < 36; i++) wr_le32(xorbuf + 4 * i, pti->key[i]);
	if(!type1 && pti->scramble_key) kirk7(xorbuf, xorbuf, 0x90, pti->code);

	memcpy(t + T_TAG, in + 0xD0, 4);
	memcpy(t + T_SHA1, in + 0xD4, 0x14);
	memcpy(t + T_UNUSED, in + 0xE8, 0x28);
	memcpy(t + T_KIRK, in + 0x110, 0x40);
	memcpy(t + T_KIRK + 0x40, in + 0x80, 0x50);
	memcpy(t + T_PRXH, in, 0x80);
	if(type1) kirk7(t + T_SHA1 + 0xC, t + T_SHA1 + 0xC, 0xA0, pti->code);

	parts[0] = xorbuf; sizes[0] = 0x14;
	parts[1] = t + T_UNUSED; sizes[1] = 0x28;
	parts[2] = t + T_KIRK; sizes[2] = 0x90;
	parts[3] = t + T_PRXH; sizes[3] = 0x80;
	if(!sha1_matches(t + T_SHA1, parts, sizes, 4)) return -3;

	if(out != in) memcpy(out, in, size);
	memcpy(out + HDR_OFFSET, t + T_KIRK, KIRK_CMD1_HEADER_SIZE);
	memcpy(out + HDR_OFFSET + KIRK_CMD1_HEADER_SIZE, t + T_PRXH, 0x80);
	decrypt_kirk_header_type0(out + HDR_OFFSET, t + T_KIRK, xorbuf, pti->code);
	return finish(in, out, size);
}

/* Tipos 2, 5, 6 y 9: semilla expandida. Disposición (0x150 bytes): tag,
   relleno (0x58, o 0x38 + la cola de la firma ECDSA), id, sha1, cabecera
   del KIRK, sus metadatos y la cabecera del PRX */
#define U_TAG 0x00
#define U_EMPTY 0x04
#define U_ECDSA 0x3C
#define U_ID 0x5C
#define U_SHA1 0x6C
#define U_KIRK 0x80
#define U_META 0xC0
#define U_PRXH 0xD0

enum { KIND_2, KIND_5, KIND_6, KIND_9 };

static int decrypt_type_seed(const u8 *in, u8 *out, u32 size, int kind, const u8 *seed){
	const TAG_INFO2 *pti = get_tag_info2(rd_le32(in + 0xD0));
	u8 xorbuf[0x90], t[PSP_HEADER_SIZE], *h;
	const u8 *parts[8];
	u32 sizes[8];
	int i, n = 0, ecdsa = kind == KIND_6 || kind == KIND_9;
	if(!pti) return -1;

	switch(kind){
	case KIND_2: if(!all_zero(in + 0xD4, 0x58)) return -2; break;
	case KIND_5: if(!all_zero(in + 0xD5, 0x57)) return -2; break;
	case KIND_6: if(!all_zero(in + 0xD4, 0x38)) return -2; break;
	default:     if(!all_zero(in + 0xD4, 0x104 - 0xD4)) return -2; break;
	}
	expand_seed(xorbuf, pti->key, pti->code, kind == KIND_5 ? seed : NULL);

	memset(t, 0, sizeof(t));
	memcpy(t + U_TAG, in + 0xD0, 4);
	if(kind == KIND_6) memcpy(t + U_ECDSA, in + 0x10C, 0x20);
	memcpy(t + U_ID, in + 0x140, 0x10);
	memcpy(t + U_SHA1, in + 0x12C, 0x14);
	memcpy(t + U_KIRK, in + 0x80, 0x30);
	memcpy(t + U_KIRK + 0x30, in + 0xC0, 0x10);
	memcpy(t + U_META, in + 0xB0, 0x10);
	memcpy(t + U_PRXH, in, 0x80);

	if(kind == KIND_5){
		/* El modo de descifrado de un SPRX manda sobre la semilla de la tabla */
		const u8 *xor1 = in[0x7C] == PRX_DECRYPT_MODE_SPRX ? xor_91E0A9AD : pti->seed;
		u8 data[0x50];
		memcpy(data, t + U_KIRK, 0x40);
		memcpy(data + 0x40, t + U_SHA1, 0x10);
		for(i = 0; i < 0x50; i++){
			if(xor1) data[i] ^= xor1[i % 0x10];
			if(seed) data[i] ^= seed[i % 0x10];
		}
		kirk7(data, data, 0x50, pti->code);
		memcpy(t + U_KIRK, data, 0x40);
		memcpy(t + U_SHA1, data + 0x40, 0x10);
		if(xor1) for(i = 0; i < 0x60; i++) t[U_ID + i] ^= xor1[i % 0x10];
	}
	kirk7(t + U_ID, t + U_ID, 0x60, pti->code);

	parts[n] = t + U_TAG; sizes[n++] = 4;
	parts[n] = xorbuf; sizes[n++] = 0x10;
	if(ecdsa){
		parts[n] = t + U_EMPTY; sizes[n++] = 0x38;
		parts[n] = t + U_ECDSA; sizes[n++] = 0x20;
	} else {
		parts[n] = t + U_EMPTY; sizes[n++] = 0x58;
	}
	parts[n] = t + U_ID; sizes[n++] = 0x10;
	parts[n] = t + U_KIRK; sizes[n++] = 0x40;
	parts[n] = t + U_META; sizes[n++] = 0x10;
	parts[n] = t + U_PRXH; sizes[n++] = 0x80;
	if(!sha1_matches(t + U_SHA1, parts, sizes, n)) return -3;

	if(out != in) memcpy(out, in, size);
	h = out + HDR_OFFSET;
	memset(h, 0, KIRK_CMD1_HEADER_SIZE);
	if(kind == KIND_6) memcpy(h + 0x40, t + U_ECDSA, 0x20);
	memcpy(h + 0x70, t + U_META, 0x10);   /* data_size, data_offset... */
	memcpy(h + KIRK_CMD1_HEADER_SIZE, t + U_PRXH, 0x80);
	decrypt_kirk_header(h, t + U_KIRK, xorbuf + 0x10, pti->code);
	wr_le32(h + 0x60, 1);                 /* modo CMD1 */
	h[0x64] = kind == KIND_6;             /* hash ECDSA */
	return finish(in, out, size);
}

int prx_decrypt(const u8 *in, u8 *out, u32 size, const u8 *seed){
	int r;
	if(size < PSP_HEADER_SIZE) return -1;
	if((r = decrypt_type01(in, out, size, 0)) >= 0) return r;
	if((r = decrypt_type01(in, out, size, 1)) >= 0) return r;
	if((r = decrypt_type_seed(in, out, size, KIND_2, seed)) >= 0) return r;
	if((r = decrypt_type_seed(in, out, size, KIND_5, seed)) >= 0) return r;
	if((r = decrypt_type_seed(in, out, size, KIND_6, seed)) >= 0) return r;
	/* El último: su comprobación es un subconjunto de la del tipo 6 */
	return decrypt_type_seed(in, out, size, KIND_9, seed);
}
