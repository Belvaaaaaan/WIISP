/**
 * WIISP - app.c
 * Lógica de la aplicación compartida entre plataformas (ver app.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include "frontend/app.h"
#include "core/memory.h"
#include "loader/loader.h"

#define MAX_EXEC_SIZE (64u * 1024 * 1024)

static PspModule module;

int app_init(void){
	u8 *ram = plat_alloc_big(PSP_RAM_SIZE_32MB);
	u8 *vram = plat_alloc_big(PSP_VRAM_SIZE);
	u8 *scratch = plat_alloc_big(PSP_SCRATCH_SIZE);
	if(!ram || !vram || !scratch){
		printf("Error: no hay memoria para la RAM de la PSP\n");
		return -1;
	}
	return mem_init(ram, PSP_RAM_SIZE_32MB, vram, scratch);
}

/* Lee el archivo entero. Devuelve NULL si falla. */
static u8 *read_file(const char *path, u32 *len){
	FILE *f = fopen(path, "rb");
	long size;
	u8 *buf = NULL;

	if(!f) return NULL;
	if(fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 &&
	   (unsigned long)size <= MAX_EXEC_SIZE && fseek(f, 0, SEEK_SET) == 0){
		buf = malloc((size_t)size);
		if(buf && fread(buf, 1, (size_t)size, f) != (size_t)size){
			free(buf);
			buf = NULL;
		}
		*len = (u32)size;
	}
	fclose(f);
	return buf;
}

int app_load(const char *path, int max_imports){
	u32 len = 0;
	u8 *buf;
	int err;

	printf("Cargando %s\n", path);
	buf = read_file(path, &len);
	if(!buf){
		printf("Error: no se pudo leer el archivo\n");
		return -1;
	}

	loader_free(&module);
	mem_reset();
	err = loader_load(buf, len, 0, &module);
	free(buf);
	if(err){
		printf("Error: %s\n", loader_strerror(err));
		return -1;
	}
	loader_print_info(&module, max_imports);
	return 0;
}
