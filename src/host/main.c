/**
 * WIISP - host/main.c
 * Herramienta de línea de comandos para PC: carga un ejecutable de PSP con
 * el mismo núcleo que usa el Wii. Sirve para depurar sin la consola.
 *
 *   wiisp-cli [--all] EBOOT.PBP
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frontend/app.h"

void *plat_alloc_big(size_t size){
	return aligned_alloc(32, (size + 31) & ~(size_t)31);
}

int main(int argc, char **argv){
	int max_imports = 16;
	const char *path = NULL;
	int i;

	for(i = 1; i < argc; i++){
		if(!strcmp(argv[i], "--all")) max_imports = -1;
		else path = argv[i];
	}
	if(!path){
		fprintf(stderr, "uso: %s [--all] EBOOT.PBP|archivo.elf|archivo.prx\n", argv[0]);
		return 2;
	}
	if(app_init()) return 1;
	return app_load(path, max_imports) ? 1 : 0;
}
