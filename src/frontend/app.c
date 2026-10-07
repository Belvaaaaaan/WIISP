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
#include "loader/disc.h"
#include "loader/sfo.h"
#include "hle/hle.h"
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"

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

int app_is_disc_image(const char *path){
	const char *dot = strrchr(path, '.');
	return dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".cso") || !strcasecmp(dot, ".zso"));
}

/* Un UMD: el ejecutable es PSP_GAME/SYSDIR/EBOOT.BIN (normalmente cifrado);
   si no se puede usar, BOOT.BIN, que algunos discos traen sin cifrar */
static int load_disc(const char *path){
	static const char *const execs[] = { "PSP_GAME/SYSDIR/EBOOT.BIN", "PSP_GAME/SYSDIR/BOOT.BIN" };
	char title[sizeof(module.title)] = "", disc_id[sizeof(module.disc_id)] = "";
	u32 len, i;
	u8 *buf;
	int err = LOADER_ERR_FORMAT;

	if(disc_open(path)){
		printf("Error: no es una imagen de UMD valida (ISO/CSO/ZSO)\n");
		return -1;
	}
	if(verbose) printf("Imagen %s: %u sectores\n", disc_format(), disc_sectors());
	buf = disc_read_file("PSP_GAME/PARAM.SFO", &len, 64 * 1024);
	if(buf){
		SfoFile sfo;
		if(sfo_parse(buf, len, &sfo) == LOADER_OK){
			sfo_get_string(&sfo, "TITLE", title, sizeof(title));
			sfo_get_string(&sfo, "DISC_ID", disc_id, sizeof(disc_id));
		}
		free(buf);
	}
	for(i = 0; i < 2; i++){
		u32 k;
		buf = disc_read_file(execs[i], &len, MAX_EXEC_SIZE);
		if(!buf) continue;
		for(k = 0; k < len && !buf[k]; k++);
		if(k == len){ free(buf); continue; }   /* BOOT.BIN vacío */
		mem_reset();
		err = loader_load_inplace(buf, len, 0, &module);
		free(buf);
		if(err == LOADER_OK){
			if(verbose && i) printf("EBOOT.BIN no se pudo usar: se carga BOOT.BIN\n");
			break;
		}
	}
	if(err){
		printf("Error: %s\n", loader_strerror(err));
		if(err == LOADER_ERR_ENCRYPTED) printf("Etiqueta de cifrado: 0x%08X\n", loader_last_tag);
		return -1;
	}
	memcpy(module.title, title, sizeof(title));
	memcpy(module.disc_id, disc_id, sizeof(disc_id));
	snprintf(exec_path, sizeof(exec_path), "disc0:/%s", execs[i]);
	return 0;
}

int app_load(const char *path, int max_imports, const char *imports_out){
	u32 len = 0;
	u8 *buf;
	const char *slash;
	int err;

	if(verbose) printf("Cargando %s\n", path);
	hle_shutdown();
	loader_free(&module);
	disc_close();

	if(app_is_disc_image(path)){
		if(load_disc(path)) return -1;
	} else {
		buf = read_file(path, &len);
		if(!buf){
			printf("Error: no se pudo leer el archivo\n");
			return -1;
		}
		mem_reset();
		err = loader_load_inplace(buf, len, 0, &module);
		free(buf);
		if(err){
			printf("Error: %s\n", loader_strerror(err));
			if(err == LOADER_ERR_ENCRYPTED) printf("Etiqueta de cifrado: 0x%08X\n", loader_last_tag);
			return -1;
		}
	}
	if(verbose) loader_print_info(&module, max_imports);

	/* La carpeta del ejecutable (o de la imagen) hace de ms0:/ y host0:/ */
	snprintf(host_dir, sizeof(host_dir), "%s", path);
	slash = strrchr(host_dir, '/');
	if(slash) host_dir[slash - host_dir] = 0;
	else strcpy(host_dir, ".");
	if(!disc_is_open()) snprintf(exec_path, sizeof(exec_path), "ms0:/PSP/GAME/WIISP/%s", slash ? slash + 1 : path);

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

void app_set_host_clock(unsigned long long (*clock)(void)){ ge_host_clock = clock; }

unsigned long long app_ge_host_ticks(void){
	GeStats st;
	ge_get_stats(&st);
	return st.host_ticks;
}

static void null_begin(void){}
static void null_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ (void)a; (void)b; (void)c; }
static void null_two(const GeVertex *a, const GeVertex *b){ (void)a; (void)b; }
static void null_one(const GeVertex *a){ (void)a; }
static const GeHwRenderer null_renderer = { null_begin, null_tri, null_two, null_two, null_two, null_one, null_begin };

void app_set_null_renderer(void){ ge_hw = &null_renderer; ge_fast_math = 1; }

void app_set_fast_math(int on){ ge_fast_math = on; }
