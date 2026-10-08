/**
 * WIISP - host/main.c
 * Herramienta de línea de comandos para PC: usa el mismo núcleo que el Wii.
 * Sirve para depurar sin la consola y para correr pspautotests.
 *
 *   wiisp-cli [opciones] EBOOT.PBP|archivo.elf|archivo.prx
 *     --all             listar todos los imports
 *     --imports FILE    escribir el informe de imports en FILE
 *     --run             ejecutar el programa (su salida va a stdout)
 *     --frames N        límite de frames al ejecutar (por defecto 1800 = 30 s)
 *     --quiet           no imprimir el resumen del módulo
 *     --screenshot FILE guardar el framebuffer final como PPM
 *     --bmp FILE        guardar como BMP la captura que pida el programa
 *                       (devctl de pspautotests), igual que en una PSP
 *     --null-gpu        no dibujar (para medir el resto: CPU y geometría)
 *     --fast-math       geometría con float normal, como con GX en el Wii
 *     --root DIR        DIR hace de ms0:/ y el ejecutable está dentro (como
 *                       el --root de PPSSPP para pspautotests: "../x" funciona)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "frontend/app.h"
#include "hle/hle.h"

void *plat_alloc_big(size_t size){
	return aligned_alloc(32, (size + 31) & ~(size_t)31);
}

static void output(const char *text, unsigned len){
	fwrite(text, 1, len, stdout);
	fflush(stdout);
}

/* Convierte un píxel de la PSP (little-endian) a RGB de 8 bits */
static void pixel_rgb(const unsigned char *p, unsigned format, unsigned char *rgb){
	unsigned v = p[0] | (p[1] << 8);
	switch(format){
	case 0: rgb[0] = (v & 31) << 3; rgb[1] = ((v >> 5) & 63) << 2; rgb[2] = ((v >> 11) & 31) << 3; break;
	case 1: rgb[0] = (v & 31) << 3; rgb[1] = ((v >> 5) & 31) << 3; rgb[2] = ((v >> 10) & 31) << 3; break;
	case 2: rgb[0] = (v & 15) << 4; rgb[1] = ((v >> 4) & 15) << 4; rgb[2] = ((v >> 8) & 15) << 4; break;
	default: rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; break;
	}
}

static int save_screenshot(const char *path){
	unsigned stride, format, x, y;
	const unsigned char *fb = app_get_framebuffer(&stride, &format);
	FILE *f;
	if(!fb){ fprintf(stderr, "No hay framebuffer que guardar\n"); return -1; }
	f = fopen(path, "wb");
	if(!f) return -1;
	fprintf(f, "P6\n%d %d\n255\n", APP_SCREEN_W, APP_SCREEN_H);
	for(y = 0; y < APP_SCREEN_H; y++)
		for(x = 0; x < APP_SCREEN_W; x++){
			unsigned char rgb[3];
			pixel_rgb(fb + (y * stride + x) * (format == 3 ? 4 : 2), format, rgb);
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	return 0;
}

int main(int argc, char **argv){
	int max_imports = 16, run = 0, frames = 1800, i, exited = 0;
	const char *path = NULL, *imports = NULL, *screenshot = NULL;

	for(i = 1; i < argc; i++){
		if(!strcmp(argv[i], "--all")) max_imports = -1;
		else if(!strcmp(argv[i], "--run")) run = 1;
		else if(!strcmp(argv[i], "--quiet")) app_set_verbose(0);
		else if(!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--imports") && i + 1 < argc) imports = argv[++i];
		else if(!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
		else if(!strcmp(argv[i], "--bmp") && i + 1 < argc) app_set_screenshot_path(argv[++i]);
		else if(!strcmp(argv[i], "--null-gpu")) app_set_null_renderer();
		else if(!strcmp(argv[i], "--fast-math")) app_set_fast_math(1);
		else if(!strcmp(argv[i], "--root") && i + 1 < argc) app_set_root(argv[++i]);
		else if(!strcmp(argv[i], "--log") && i + 1 < argc) hle_set_log_file(argv[++i]);   /* como wiisp.log */
		else path = argv[i];
	}
	if(!path){
		fprintf(stderr, "uso: %s [--all] [--imports FILE] [--run] [--frames N] [--quiet]\n"
		                "          [--screenshot FILE] EBOOT.PBP|archivo.elf|archivo.prx\n", argv[0]);
		return 2;
	}
	if(app_init()) return 1;
	if(app_load(path, max_imports, imports)) return 1;
	if(!run) return 0;

	app_set_output(output);
	if(app_start()) return 1;
	{
		clock_t start = clock();
		unsigned run_frames;
		unsigned long long instr;
		double secs;
		for(i = 0; i < frames && !exited; i++) exited = app_run_frame();
		secs = (double)(clock() - start) / CLOCKS_PER_SEC;
		app_get_stats(&run_frames, &instr);
		fprintf(stderr, "[WIISP] %s tras %d frames (%.1f MIPS en este PC)\n",
		        exited ? app_exit_reason() : "limite de frames alcanzado", i,
		        secs > 0 ? instr / secs / 1e6 : 0.0);
	}
	if(screenshot) save_screenshot(screenshot);
	return 0;
}
