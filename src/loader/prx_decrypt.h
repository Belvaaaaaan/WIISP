/**
 * WIISP - prx_decrypt.h
 * Descifrado de PRX y EBOOT.BIN cifrados de la PSP ("~PSP").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_PRX_DECRYPT_H
#define WIISP_PRX_DECRYPT_H

#include "core/types.h"

/* Descifra los size bytes de in (desde la cabecera "~PSP", size = su
   psp_size) en out, que puede ser el mismo búfer y debe tener size bytes.
   El ELF (quizá comprimido con gzip) queda al principio de out. seed: la
   clave de NPDRM o NULL. Devuelve el tamaño descifrado o < 0 si ninguna
   variante reconoce la etiqueta o el SHA-1 no coincide. */
int prx_decrypt(const u8 *in, u8 *out, u32 size, const u8 *seed);

#endif
