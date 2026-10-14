/**
 * WIISP - wii/gx_tiles.h
 * Escritura de texturas RGBA8 de GX por franjas de 4 filas.
 *
 * GX guarda RGBA8 en tiles de 4x4 texels de 64 bytes: los primeros 32 con
 * alfa y rojo de los 16 texels y los otros 32 con verde y azul. Escribir
 * texel a texel salta entre dos líneas de caché por texel y obliga a leer
 * cada línea antes de escribirla. Por franjas de 4 filas, cada tile se
 * escribe entero y seguido, y en el Wii `dcbz` prepara sus dos líneas de
 * caché sin leerlas de la memoria (se van a sobrescribir enteras).
 *
 * Es C normal salvo el dcbz (solo con GEKKO), para poder probarlo en el PC
 * contra la escritura texel a texel (tests/test_gxtiles.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_GX_TILES_H
#define WIISP_GX_TILES_H

#include <stdint.h>

/* Byte (x, y) de una textura RGBA8 de GX de `w` texels de ancho (múltiplo
   de 4): ahí van alfa y rojo; 32 bytes más allá, verde y azul */
static inline uint8_t *gx_tile_ptr(uint8_t *base, int w, int x, int y){
	return base + ((((y >> 2) * (w >> 2) + (x >> 2)) << 6) | ((((y & 3) << 2) | (x & 3)) << 1));
}

/* Texel en formato de la PSP (rojo en el byte bajo) -> RGBA8 de GX */
static inline void gx_put_rgba8(uint8_t *t, uint32_t c){
	t[0] = (uint8_t)(c >> 24); t[1] = (uint8_t)c; t[32] = (uint8_t)(c >> 8); t[33] = (uint8_t)(c >> 16);
}

/* Escribe las filas y0..y0+3 (y0 múltiplo de 4) de una textura de `tw`
   texels de ancho (múltiplo de 4) a partir de 4 filas lineales `rows` de
   `w` texels (w múltiplo de 4, w <= tw). `data` alineado a 32 bytes. */
static inline void gx_tile_band_rgba8(uint8_t *data, int tw, int y0, const uint32_t *rows, int w){
	uint8_t *tile = data + (((y0 >> 2) * (tw >> 2)) << 6);
	int tx, r, k;
	for(tx = 0; tx < w; tx += 4, tile += 64){
#ifdef GEKKO
		/* Las dos líneas de 32 bytes del tile se escriben enteras */
		__asm__ volatile("dcbz 0, %0" : : "r"(tile) : "memory");
		__asm__ volatile("dcbz 0, %0" : : "r"(tile + 32) : "memory");
#endif
		for(r = 0; r < 4; r++){
			const uint32_t *src = rows + r * w + tx;
			uint8_t *ar = tile + (r << 3), *gb = tile + 32 + (r << 3);
			for(k = 0; k < 4; k++){
				uint32_t c = src[k];
				ar[k * 2] = (uint8_t)(c >> 24);
				ar[k * 2 + 1] = (uint8_t)c;
				gb[k * 2] = (uint8_t)(c >> 8);
				gb[k * 2 + 1] = (uint8_t)(c >> 16);
			}
		}
	}
}

#endif
