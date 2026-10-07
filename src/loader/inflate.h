/**
 * WIISP - inflate.h
 * Descompresor de deflate (RFC 1951) y de su envoltorio gzip (RFC 1952),
 * para los PRX comprimidos y los bloques de las imágenes CSO; y de LZ4
 * (bloques de las imágenes ZSO).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_INFLATE_H
#define WIISP_INFLATE_H

#include "core/types.h"

/* Deflate sin envoltorio. Devuelve los bytes escritos en out o < 0 si los
   datos están mal o no caben en out_size. in_used: bytes consumidos. */
int inflate_raw(const u8 *in, u32 in_size, u8 *out, u32 out_size, u32 *in_used);

/* gzip: devuelve los bytes descomprimidos o < 0 */
int gunzip(const u8 *in, u32 in_size, u8 *out, u32 out_size);

/* Bloque LZ4 sin marco. Devuelve los bytes escritos o < 0 */
int lz4_decompress(const u8 *in, u32 in_size, u8 *out, u32 out_size);

#endif
