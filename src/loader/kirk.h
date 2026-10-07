/**
 * WIISP - kirk.h
 * Lo que hace falta del KIRK (el motor criptográfico de la PSP) para
 * descifrar ejecutables: AES-128 en modo CBC, SHA-1 y los comandos 1
 * (bloque privado) y 7 (AES-CBC con una clave del KIRK).
 *
 * Escrito para WIISP a partir de la descripción pública del KIRK; las
 * claves están en prx_keys.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_KIRK_H
#define WIISP_KIRK_H

#include "core/types.h"

typedef struct {
	u32 ek[44];   /* claves de ronda para cifrar */
	u32 dk[44];   /* y para descifrar (cifrado inverso equivalente) */
} AesKey;

void aes_set_key(AesKey *k, const u8 key[16]);
void aes_encrypt_block(const AesKey *k, const u8 in[16], u8 out[16]);
void aes_decrypt_block(const AesKey *k, const u8 in[16], u8 out[16]);
/* CBC con IV a cero; size se redondea hacia abajo a bloques de 16. out
   puede solaparse con in si out <= in. */
void aes_cbc_encrypt(const AesKey *k, const u8 *in, u8 *out, u32 size);
void aes_cbc_decrypt(const AesKey *k, const u8 *in, u8 *out, u32 size);

typedef struct {
	u32 h[5];
	u64 len;
	u8 buf[64];
	u32 used;
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, u32 len);
void sha1_final(Sha1 *s, u8 out[20]);

/* Comando 7: AES-CBC (IV 0) con la clave code del KIRK. 0 = correcto. */
int  kirk7(u8 *out, const u8 *in, u32 size, int code);
/* Comando 4: lo mismo cifrando (para las pruebas) */
int  kirk4(u8 *out, const u8 *in, u32 size, int code);

/* Cabecera del comando 1 (0x90 bytes, ya en claro salvo las claves) */
#define KIRK_CMD1_HEADER_SIZE 0x90
/* Comando 1: descifra el bloque cuya cabecera está en block (seguida de
   data_offset bytes y de los datos) y deja los datos en out. No comprueba
   la firma: quien llama ya validó la cabecera con su SHA-1. out puede ser
   anterior a block en el mismo búfer. avail: bytes legibles desde block.
   0 = correcto. */
int  kirk_cmd1(u8 *out, const u8 *block, u32 avail);

#endif
