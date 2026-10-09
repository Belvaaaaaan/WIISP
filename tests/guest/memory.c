/**
 * WIISP - tests/guest/memory.c
 * La memoria libre (sceKernelMaxFreeMemSize / TotalFreeMemSize) con
 * bloques pegados y huecos. Con bloques reservados desde arriba y el de
 * más arriba liberado, WIISP daba un hueco negativo (0xFFFE0000 en GTA
 * LCS, que con eso creaba su montón y caía).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

#define K 1024

int main(int args, void *argp){
	SceUID b[4];
	u32 total0 = sceKernelTotalFreeMemSize(), max0 = sceKernelMaxFreeMemSize(), max1, total1;
	int i;
	/* Cuatro bloques de 64 KB desde arriba, uno pegado al otro */
	for(i = 0; i < 4; i++) b[i] = sceKernelAllocPartitionMemory(2, "alto", 1, 64 * K, 0);
	for(i = 1; i < 4; i++)
		printf("bloque %d pegado debajo del anterior: %s\n", i,
		       (char *)sceKernelGetBlockHeadAddr(b[i]) + 64 * K == (char *)sceKernelGetBlockHeadAddr(b[i - 1]) ? "si" : "NO");
	/* Se libera el de más arriba: queda un hueco de 64 KB arriba */
	sceKernelFreePartitionMemory(b[0]);
	max1 = sceKernelMaxFreeMemSize();
	total1 = sceKernelTotalFreeMemSize();
	printf("total libre: %d KB menos (esperado 192)\n", (int)(total0 - total1) / K);
	printf("mayor hueco: %s\n", max1 <= max0 && max1 >= max0 - 256 * K && max1 < 0x02000000 ? "bien" : "MAL");
	printf("mayor hueco < total: %s\n", max1 <= total1 ? "si" : "NO");
	/* Un bloque de lo que dice MaxFreeMemSize tiene que caber */
	b[0] = sceKernelAllocPartitionMemory(2, "max", 0, max1, 0);
	printf("reservar el mayor hueco: %s\n", b[0] > 0 ? "si" : "NO");
	return 0;
}
