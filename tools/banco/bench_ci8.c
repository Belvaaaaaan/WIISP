/* Banco: índices CI8 desde la copia RGBA8 de un búfer 565 (ge_rtt_gpu), con
   la conversión de siempre y con la que lee y escribe palabras (ge_ci8_rapido), en el tamaño
   de DBZ (480x272 en una textura de 512 de ancho).
   bench_ci8 REPETICIONES [palabras] [desplazamiento máscara]
   Las funciones se compilan con devkitPPC (-DSOLO_FUNCIONES) y el resto con
   el gcc de Linux (build_banco.sh). Con qprof.py (dos corridas de distinto
   número de repeticiones), las instrucciones por píxel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void ci8_siempre(uint8_t *dst, const uint8_t *src, unsigned shift, unsigned mask);
void ci8_palabras(uint8_t *dst, const uint8_t *src, unsigned shift, unsigned mask);

#ifdef SOLO_FUNCIONES
#include "wii/gx_tiles.h"
void ci8_siempre(uint8_t *dst, const uint8_t *src, unsigned shift, unsigned mask){
	gx_ci8_from_rgba8_565(dst, 512, src, 480, 272, shift, mask);
}
void ci8_palabras(uint8_t *dst, const uint8_t *src, unsigned shift, unsigned mask){
	gx_ci8_from_rgba8_565_palabras(dst, 512, src, 480, 272, shift, mask);
}
#else
static uint8_t src[480 * 272 * 4] __attribute__((aligned(32)));
static uint8_t dst[512 * 272] __attribute__((aligned(32)));

int main(int argc, char **argv){
	int reps = argc > 1 ? atoi(argv[1]) : 10, palabras = argc > 2 && atoi(argv[2]), i;
	unsigned shift = argc > 3 ? (unsigned)atoi(argv[3]) : 5, mask = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 0xFF;
	uint32_t s = 1, h = 2166136261u;
	for(i = 0; i < (int)sizeof(src); i++){ s ^= s << 13; s ^= s >> 17; s ^= s << 5; src[i] = (uint8_t)s; }
	for(i = 0; i < reps; i++){
		if(palabras) ci8_palabras(dst, src, shift, mask);
		else ci8_siempre(dst, src, shift, mask);
	}
	for(i = 0; i < (int)sizeof(dst); i++) h = (h ^ dst[i]) * 16777619u;
	printf("huella %08X\n", (unsigned)h);
	return 0;
}
#endif
