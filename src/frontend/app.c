/**
 * WIISP - app.c
 * Lógica de la aplicación compartida entre plataformas (ver app.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "frontend/app.h"
#include "core/memory.h"
#include "loader/loader.h"
#include "hle/hle.h"

#define MAX_EXEC_SIZE (64u * 1024 * 1024)

static PspModule module;
static char exec_path[256];
static char host_dir[256];
static AppOutputFunc output_func;
static int verbose = 1;

void app_set_verbose(int v){ verbose = v; }

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

int app_load(const char *path, int max_imports, const char *imports_out){
	u32 len = 0;
	u8 *buf;
	const char *slash;
	int err;

	if(verbose) printf("Cargando %s\n", path);
	buf = read_file(path, &len);
	if(!buf){
		printf("Error: no se pudo leer el archivo\n");
		return -1;
	}

	hle_shutdown();
	loader_free(&module);
	mem_reset();
	err = loader_load(buf, len, 0, &module);
	free(buf);
	if(err){
		printf("Error: %s\n", loader_strerror(err));
		return -1;
	}
	if(verbose) loader_print_info(&module, max_imports);

	/* La carpeta del ejecutable hace de ms0:/ y host0:/ */
	snprintf(host_dir, sizeof(host_dir), "%s", path);
	slash = strrchr(host_dir, '/');
	if(slash) host_dir[slash - host_dir] = 0;
	else strcpy(host_dir, ".");
	snprintf(exec_path, sizeof(exec_path), "ms0:/PSP/GAME/WIISP/%s", slash ? slash + 1 : path);

	if(imports_out){
		if(hle_write_imports_report(&module, path, imports_out) == 0)
			printf("Informe de imports: %s\n", imports_out);
		else
			printf("Aviso: no se pudo escribir %s\n", imports_out);
	}
	return 0;
}

static void forward_output(const char *text, u32 len){
	if(output_func) output_func(text, len);
	else fwrite(text, 1, len, stdout);
}

static unsigned frames_run;
static u64 executed_at_start;

int app_start(void){
	hle_set_output(forward_output);
	frames_run = 0;
	executed_at_start = cpu_executed;
	return hle_init(&module, host_dir, exec_path);
}

int app_run_frame(void){
	frames_run++;
	return hle_run_frame();
}

void app_get_stats(unsigned *frames, unsigned long long *instructions){
	*frames = frames_run;
	*instructions = cpu_executed - executed_at_start;
}

void app_imports_path(const char *path, char *out, size_t out_size){
	const char *slash = strrchr(path, '/'), *name = slash ? slash + 1 : path;
	const char *dot = strrchr(name, '.');
	int dir_len = (int)(name - path);
	if(!strcasecmp(name, "EBOOT.PBP"))
		snprintf(out, out_size, "%.*simports.txt", dir_len, path);
	else
		snprintf(out, out_size, "%.*s%.*s.imports.txt", dir_len, path,
		         (int)(dot ? dot - name : (long)strlen(name)), name);
}
const char *app_exit_reason(void){ return hle_exit_reason(); }
void app_set_output(AppOutputFunc func){ output_func = func; }

void app_set_input(unsigned buttons, unsigned char lx, unsigned char ly){
	hle_set_input(buttons, lx, ly);
}

const unsigned char *app_get_framebuffer(unsigned *stride, unsigned *format){
	HleFramebuffer fb;
	u32 bpp;
	hle_get_framebuffer(&fb);
	if(!fb.addr) return NULL;
	bpp = fb.format == 3 ? 4 : 2;
	*stride = fb.stride;
	*format = fb.format;
	return mem_ptr(fb.addr, fb.stride * (APP_SCREEN_H - 1) * bpp + APP_SCREEN_W * bpp);
}
