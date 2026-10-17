/**
 * WIISP - tools/banco/cpu_banco.c
 * Programa de PSP para medir el intérprete (medir_cpu.sh): copias de
 * palabras, llamadas a una función pequeña y ramas, como el código normal
 * de un juego. VUELTAS se da al compilar; escribe una suma al acabar.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

static u32 a[2048], b[2048];

static u32 __attribute__((noinline)) mezcla(u32 x, u32 y){ return (x ^ (y << 3)) + (y >> 2) * 7; }

int main(void){
	u32 i, k, s = 1;
	for(i = 0; i < 2048; i++) a[i] = i * 2654435761u;
	for(k = 0; k < VUELTAS; k++){
		for(i = 0; i < 2048; i++) b[i] = a[i] + k;                     /* copia */
		for(i = 0; i < 2048; i += 4) s = mezcla(s, b[i]) ^ b[i + 1];   /* llamadas */
		for(i = 0; i < 512; i++){ if(b[i] & 1) s += i; else s -= b[i] >> 3; }   /* ramas */
	}
	printf_("banco: %08x\n", s);
	return 0;
}
