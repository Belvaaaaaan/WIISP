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
#include <string.h>   //Arreglo

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

/* --- Texturas con paleta desde la copia del EFB (CI8 + TLUT) -------------- */

/* Índices CI8 de GX (tiles de 8x4 bytes; dst_w columnas, múltiplo de 8) a
   partir de la copia RGBA8 de GX de un búfer 565 de src_w x h texels
   (src_w múltiplo de 4 y <= dst_w, h múltiplo de 4). Cada índice es el que
   daría un CLUT16 de la PSP leyendo ese búfer ya bajado a la VRAM: el 565
   de la bajada, (R >> 3) | (G >> 2) << 5 | (B >> 3) << 11, desplazado y con
   la máscara (el offset va en la TLUT). Las columnas desde src_w quedan a
   0. dst alineado a 32 bytes. */
static inline void gx_ci8_from_rgba8_565(uint8_t *dst, int dst_w, const uint8_t *src, int src_w, int h,
                                         unsigned shift, unsigned mask){
	int tiles8 = dst_w >> 3, src_tiles = src_w >> 2, ty, tx, r, c, k;
	for(ty = 0; ty < h >> 2; ty++){
		const uint8_t *srow = src + ((ty * src_tiles) << 6);
		uint8_t *d = dst + ((ty * tiles8) << 5);
		for(tx = 0; tx < tiles8; tx++, d += 32){
#ifdef GEKKO
			/* El tile CI8 es una línea de caché que se escribe entera */
			__asm__ volatile("dcbz 0, %0" : : "r"(d) : "memory");
#endif
			/* Un tile CI8 (8x4) son dos tiles RGBA8 (4x4) seguidos */
			for(k = 0; k < 2; k++){
				int x0 = tx * 8 + k * 4;
				if(x0 >= src_w){
					for(r = 0; r < 4; r++) for(c = 0; c < 4; c++) d[r * 8 + k * 4 + c] = 0;
					continue;
				}
				{
					const uint8_t *ar = srow + ((x0 >> 2) << 6), *gb = ar + 32;
					if(shift >= 5){
						/* Sin el rojo: solo la línea de verde y azul, seguida */
						for(r = 0; r < 4; r++)
							for(c = 0; c < 4; c++){
								unsigned g = gb[(r * 4 + c) * 2], b = gb[(r * 4 + c) * 2 + 1];
								unsigned v = ((g >> 2) << 5) | ((b >> 3) << 11);
								d[r * 8 + k * 4 + c] = (uint8_t)((v >> shift) & mask);
							}
					} else {
						for(r = 0; r < 4; r++)
							for(c = 0; c < 4; c++){
								unsigned i2 = (unsigned)(r * 4 + c) * 2;
								unsigned v = (unsigned)(ar[i2 + 1] >> 3) | ((unsigned)(gb[i2] >> 2) << 5) |
								             ((unsigned)(gb[i2 + 1] >> 3) << 11);
								d[r * 8 + k * 4 + c] = (uint8_t)((v >> shift) & mask);
							}
					}
				}
			}
		}
	}
}

static inline uint32_t gx_be32(const uint8_t *p){   //Arreglo
	uint32_t w;
	memcpy(&w, p, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__   //Arreglo
	w = __builtin_bswap32(w);
#endif   //Arreglo
	return w;
}

static inline void gx_put_be32(uint8_t *p, uint32_t w){   //Arreglo
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__   //Arreglo
	w = __builtin_bswap32(w);
#endif   //Arreglo
	memcpy(p, &w, 4);
}

static inline void gx_ci8_from_rgba8_565_palabras(uint8_t *dst, int dst_w, const uint8_t *src, int src_w, int h,   //Arreglo
                                                  unsigned shift, unsigned mask){
	int tiles8 = dst_w >> 3, src_tiles = src_w >> 2, ty, tx, r, k, conr = shift < 5, par = shift <= 13;
	uint32_t m2 = mask | (mask << 16);
	for(ty = 0; ty < h >> 2; ty++){
		const uint8_t *srow = src + ((ty * src_tiles) << 6);
		uint8_t *d = dst + ((ty * tiles8) << 5);
		for(tx = 0; tx < tiles8; tx++, d += 32){
#ifdef GEKKO   //Arreglo
			__asm__ volatile("dcbz 0, %0" : : "r"(d) : "memory");
#endif   //Arreglo
			for(k = 0; k < 2; k++){
				int x0 = tx * 8 + k * 4;
				uint8_t *o = d + k * 4;
				const uint8_t *ar, *gb;
				if(x0 >= src_w){
					for(r = 0; r < 4; r++) gx_put_be32(o + r * 8, 0);
					continue;
				}
				ar = srow + ((x0 >> 2) << 6);
				gb = ar + 32;
				if(par){
					for(r = 0; r < 4; r++){
						uint32_t w0 = gx_be32(gb + r * 8), w1 = gx_be32(gb + r * 8 + 4);
						uint32_t hi = (w0 & 0xFFFF0000u) | (w1 >> 16), lo = (w0 << 16) | (w1 & 0xFFFFu);
						uint32_t vh = ((hi >> 5) & 0x07E007E0u) | ((hi << 8) & 0xF800F800u);
						uint32_t vl = ((lo >> 5) & 0x07E007E0u) | ((lo << 8) & 0xF800F800u);
						if(conr){
							uint32_t a0 = gx_be32(ar + r * 8), a1 = gx_be32(ar + r * 8 + 4);
							vh |= (((a0 & 0xFFFF0000u) | (a1 >> 16)) >> 3) & 0x001F001Fu;
							vl |= (((a0 << 16) | (a1 & 0xFFFFu)) >> 3) & 0x001F001Fu;
						}
						gx_put_be32(o + r * 8, (((vh >> shift) & m2) << 8) | ((vl >> shift) & m2));
					}
					continue;
				}
				for(r = 0; r < 4; r++){
					uint32_t w0 = gx_be32(gb + r * 8), w1 = gx_be32(gb + r * 8 + 4);
					uint32_t v0 = ((w0 >> 21) & 0x7E0u) | ((w0 >> 8) & 0xF800u), v1 = ((w0 >> 5) & 0x7E0u) | ((w0 << 8) & 0xF800u);
					uint32_t v2 = ((w1 >> 21) & 0x7E0u) | ((w1 >> 8) & 0xF800u), v3 = ((w1 >> 5) & 0x7E0u) | ((w1 << 8) & 0xF800u);
					gx_put_be32(o + r * 8, (((v0 >> shift) & mask) << 24) | (((v1 >> shift) & mask) << 16) |
					                       (((v2 >> shift) & mask) << 8) | ((v3 >> shift) & mask));
				}
			}
		}
	}
}

/* Color 8888 de la PSP (rojo en el byte bajo) salido de una paleta 565 ->
   entrada RGB565 de una TLUT de GX (rojo arriba). GX lo devuelve a 8 bits
   repitiendo los bits altos, como la PSP: sale el mismo color, con alfa
   255 igual que la paleta 565. */
static inline uint16_t gx_tlut565(uint32_t c){
	return (uint16_t)(((c & 0xF8u) << 8) | (((c >> 8) & 0xFCu) << 3) | ((c >> 19) & 0x1Fu));
}

#endif
