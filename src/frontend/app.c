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

static char screenshot_path[512];

/* Expansión de bits igual que pspautotests/common/common.c */
static u32 bmp_pixel(const u8 *p, u32 format){
	u32 r, g, b, a;
	if(format == 3){ r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
	else {
		u32 c = (u32)p[0] | ((u32)p[1] << 8);
		if(format == 2){
			r = (c & 15) * 17; g = ((c >> 4) & 15) * 17; b = ((c >> 8) & 15) * 17; a = ((c >> 12) & 15) * 17;
		} else if(format == 1){
			r = ((c & 31) << 3) | ((c & 31) >> 2); g = (((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2);
			b = (((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2); a = (c & 0x8000) ? 255 : 0;
		} else {
			r = ((c & 31) << 3) | ((c & 31) >> 2); g = (((c >> 5) & 63) << 2) | (((c >> 5) & 63) >> 4);
			b = (((c >> 11) & 31) << 3) | (((c >> 11) & 31) >> 2); a = 255;
		}
	}
	return b | (g << 8) | (r << 16) | (a << 24);
}

int app_write_bmp(const char *path){
	static const u8 header[54] = {
		0x42, 0x4D, 0x38, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00,
		0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x10, 0x01,
		0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x80,
		0x08, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	HleFramebuffer fb;
	FILE *f;
	u32 y, x, bpp;
	hle_get_framebuffer(&fb);
	f = fopen(path, "wb");
	if(!f) return -1;
	fwrite(header, 1, sizeof(header), f);
	bpp = fb.format == 3 ? 4 : 2;
	for(y = 0; y < 272; y++){
		u8 row[512 * 4];
		for(x = 0; x < 512; x++){
			const u8 *p = fb.addr ? mem_ptr_r(fb.addr + ((271 - y) * fb.stride + x) * bpp, bpp) : NULL;
			u32 v = p ? bmp_pixel(p, fb.format) : 0;
			wr_le32(row + x * 4, v);
		}
		fwrite(row, 1, sizeof(row), f);
	}
	fclose(f);
	return 0;
}

static void on_screenshot(void){
	if(screenshot_path[0]) app_write_bmp(screenshot_path);
}

void app_set_screenshot_path(const char *path){
	snprintf(screenshot_path, sizeof(screenshot_path), "%s", path ? path : "");
}

int app_start(void){
	hle_set_output(forward_output);
	hle_set_screenshot(on_screenshot);
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
	return mem_ptr_r(fb.addr, fb.stride * (APP_SCREEN_H - 1) * bpp + APP_SCREEN_W * bpp);
}
