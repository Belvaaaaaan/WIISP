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
 *     --turbo           ni geometría ni dibujo (como el turbo del Wii)
 *     --fast-math       geometría con float normal, como con GX en el Wii
 *     --capturas N      cada N frames, una captura (BMP) si la imagen cambió
 *     --root DIR        DIR hace de ms0:/ y el ejecutable está dentro (como
 *                       el --root de PPSSPP para pspautotests: "../x" funciona)
 *
 * En Windows, con la consola en primer plano, W A S D mueven el stick
 * analógico de la PSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include "frontend/app.h"
#include "hle/hle.h"
#include "core/prof.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

void *plat_alloc_big(size_t size){
#ifdef _WIN32
	return _aligned_malloc((size + 31) & ~(size_t)31, 32);
#else
	return aligned_alloc(32, (size + 31) & ~(size_t)31);
#endif
}

/* Reloj para el desglose de tiempos ([TIEMPOS] en el registro) */
static u64 prof_ticks(void){
#ifdef _WIN32
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (u64)c.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

static u64 prof_ticks_hz(void){
#ifdef _WIN32
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return (u64)f.QuadPart;
#else
	return 1000000000ull;
#endif
}

/* Muestreo del desglose ([TIEMPOS] GE por dentro): un hilo aparte apunta
   cada milisegundo qué está haciendo el emulador */
static volatile int sampler_run;
#ifdef _WIN32
static HANDLE sampler_thread;

static DWORD WINAPI sampler_main(LPVOID arg){
	(void)arg;
	while(sampler_run){
		prof_sample();
		Sleep(1);
	}
	return 0;
}

static void sampler_start(void){
	sampler_run = 1;
	sampler_thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
}

static void sampler_stop(void){
	if(!sampler_thread) return;
	sampler_run = 0;
	WaitForSingleObject(sampler_thread, INFINITE);
	CloseHandle(sampler_thread);
	sampler_thread = NULL;
}
#else
static pthread_t sampler_thread;
static int sampler_on;

static void *sampler_main(void *arg){
	struct timespec ts = { 0, 1000000 };
	(void)arg;
	while(sampler_run){
		prof_sample();
		nanosleep(&ts, NULL);
	}
	return NULL;
}

static void sampler_start(void){
	sampler_run = 1;
	sampler_on = pthread_create(&sampler_thread, NULL, sampler_main, NULL) == 0;
}

static void sampler_stop(void){
	if(!sampler_on) return;
	sampler_run = 0;
	pthread_join(sampler_thread, NULL);
	sampler_on = 0;
}
#endif

#ifdef _WIN32
/* W A S D -> stick analógico (0-255, 128 en el centro, arriba 0); en
   diagonal, sobre el círculo como la cruceta del Wiimote. Solo con la
   consola en primer plano, para no leer lo que se escribe en otra ventana. */
static void keyboard_input(void){
	static int was_moving;
	HWND con = GetConsoleWindow();
	int dx = 0, dy = 0, r, moving;
	if(con && GetForegroundWindow() == con){
		if(GetAsyncKeyState('A') & 0x8000) dx--;
		if(GetAsyncKeyState('D') & 0x8000) dx++;
		if(GetAsyncKeyState('W') & 0x8000) dy--;
		if(GetAsyncKeyState('S') & 0x8000) dy++;
	}
	moving = dx || dy;
	if(!moving && !was_moving) return;
	was_moving = moving;
	r = dx && dy ? 90 : 128;
	dx = 128 + dx * r; dy = 128 + dy * r;
	app_set_input(0, (unsigned char)(dx > 255 ? 255 : dx), (unsigned char)(dy > 255 ? 255 : dy));
}
#endif

static int null_gpu;
static void profile_hook(char *buf, size_t size){
	snprintf(buf, size, "%s", null_gpu ? "sin dibujar (--null-gpu)" : "modo EXACTO (dibujo por software, el del PC)");
}

/* Ctrl+C: parar como al llegar al límite (estado de los hilos en el
   registro y captura final) en vez de cortar sin más */
static volatile sig_atomic_t stop_requested;
static void on_sigint(int sig){
	(void)sig;
	stop_requested = 1;
}

static void output(const char *text, unsigned len){
	fwrite(text, 1, len, stdout);
	fflush(stdout);
}

#define X5(v) ((((v) & 31) << 3) | (((v) & 31) >> 2))
#define X6(v) ((((v) & 63) << 2) | (((v) & 63) >> 4))
#define X4(v) ((((v) & 15) << 4) | ((v) & 15))

/* Convierte un píxel de la PSP (little-endian) a RGB de 8 bits */
static void pixel_rgb(const unsigned char *p, unsigned format, unsigned char *rgb){
	unsigned v = p[0] | (p[1] << 8);
	switch(format){
	/* Repitiendo los bits altos: 31 -> 255, no 248 (como la PSP) */
	case 0: rgb[0] = X5(v); rgb[1] = X6(v >> 5); rgb[2] = X5(v >> 11); break;
	case 1: rgb[0] = X5(v); rgb[1] = X5(v >> 5); rgb[2] = X5(v >> 10); break;
	case 2: rgb[0] = X4(v); rgb[1] = X4(v >> 4); rgb[2] = X4(v >> 8); break;
	default: rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; break;
	}
}

static void put_le32(unsigned char *p, unsigned v){
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* PPM, o BMP si el nombre acaba en .bmp (lo abre Windows sin más) */
static int save_screenshot(const char *path){
	unsigned stride, format, x, y;
	const unsigned char *fb = app_get_framebuffer(&stride, &format);
	size_t len = strlen(path);
	int bmp = len > 4 && (!strcmp(path + len - 4, ".bmp") || !strcmp(path + len - 4, ".BMP"));
	FILE *f;
	if(!fb){ fprintf(stderr, "[WIISP] no hay captura: el juego no mostraba ninguna imagen\n"); return -1; }
	f = fopen(path, "wb");
	if(!f) return -1;
	if(bmp){
		unsigned char h[54];
		unsigned size = APP_SCREEN_W * APP_SCREEN_H * 3;   /* 480 * 3: filas ya alineadas a 4 */
		memset(h, 0, sizeof(h));
		h[0] = 'B'; h[1] = 'M';
		put_le32(h + 2, 54 + size);
		put_le32(h + 10, 54);
		put_le32(h + 14, 40);
		put_le32(h + 18, APP_SCREEN_W);
		put_le32(h + 22, APP_SCREEN_H);
		h[26] = 1; h[28] = 24;
		put_le32(h + 34, size);
		fwrite(h, 1, sizeof(h), f);
	} else
		fprintf(f, "P6\n%d %d\n255\n", APP_SCREEN_W, APP_SCREEN_H);
	for(y = 0; y < APP_SCREEN_H; y++){
		unsigned row = bmp ? APP_SCREEN_H - 1 - y : y;   /* BMP: de abajo arriba, BGR */
		for(x = 0; x < APP_SCREEN_W; x++){
			unsigned char rgb[3], out[3];
			pixel_rgb(fb + (row * stride + x) * (format == 3 ? 4 : 2), format, rgb);
			if(bmp){ out[0] = rgb[2]; out[1] = rgb[1]; out[2] = rgb[0]; }
			else memcpy(out, rgb, 3);
			fwrite(out, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}

/* Para --capturas: un resumen del framebuffer visible, para no guardar
   dos veces la misma imagen */
static unsigned long long framebuffer_hash(void){
	unsigned stride, format, y, x, bpp;
	const unsigned char *fb = app_get_framebuffer(&stride, &format);
	unsigned long long h = 1469598103934665603ull;
	if(!fb) return 0;
	bpp = format == 3 ? 4 : 2;
	for(y = 0; y < APP_SCREEN_H; y++)
		for(x = 0; x < APP_SCREEN_W * bpp; x++) h = (h ^ fb[y * stride * bpp + x]) * 1099511628211ull;
	return h;
}

int main(int argc, char **argv){
	int max_imports = 16, run = 0, frames = 1800, i, exited = 0, every = 0;
	const char *path = NULL, *imports = NULL, *screenshot = NULL;

	for(i = 1; i < argc; i++){
		if(!strcmp(argv[i], "--all")) max_imports = -1;
		else if(!strcmp(argv[i], "--run")) run = 1;
		else if(!strcmp(argv[i], "--quiet")) app_set_verbose(0);
		else if(!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--imports") && i + 1 < argc) imports = argv[++i];
		else if(!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
		else if(!strcmp(argv[i], "--capturas") && i + 1 < argc) every = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--bmp") && i + 1 < argc) app_set_screenshot_path(argv[++i]);
		else if(!strcmp(argv[i], "--null-gpu")){ app_set_null_renderer(); null_gpu = 1; }
		else if(!strcmp(argv[i], "--turbo")) app_set_turbo(1);
		else if(!strcmp(argv[i], "--fast-math")) app_set_fast_math(1);
		else if(!strcmp(argv[i], "--root") && i + 1 < argc) app_set_root(argv[++i]);
		else if(!strcmp(argv[i], "--log") && i + 1 < argc) hle_set_log_file(argv[++i]);   /* como wiisp.log */
		else path = argv[i];
	}
	if(!path){
		fprintf(stderr, "uso: %s [--all] [--imports FILE] [--run] [--frames N] [--quiet]\n"
		                "          [--screenshot FILE] [--capturas N] [--log FILE]\n"
		                "          EBOOT.PBP|archivo.elf|archivo.prx|imagen.iso|imagen.cso\n", argv[0]);
		return 2;
	}
	if(app_init()) return 1;
	if(app_load(path, max_imports, imports)) return 1;
	if(!run) return 0;

	app_set_output(output);
	prof_set_clock(prof_ticks, prof_ticks_hz());
	hle_set_profile_hook(profile_hook);
	if(app_start()) return 1;
	sampler_start();
	{
		clock_t start = clock();
		unsigned run_frames;
		unsigned long long instr;
		double secs;
		unsigned long long last_hash = 0;
		clock_t last_report = start;
		signal(SIGINT, on_sigint);
		for(i = 0; i < frames && !exited && !stop_requested; i++){
#ifdef _WIN32
			keyboard_input();
#endif
			exited = app_run_frame();
			if(every > 0 && (i + 1) % every == 0){
				unsigned long long h = framebuffer_hash();
				if(h && h != last_hash){
					char name[64];
					snprintf(name, sizeof(name), "captura_%06d.bmp", i + 1);
					if(!save_screenshot(name)) fprintf(stderr, "[WIISP] %s\n", name);
					last_hash = h;
				}
			}
			/* Cada 10 s reales, por dónde va */
			if(clock() - last_report >= 10 * CLOCKS_PER_SEC){
				unsigned long long done;
				app_get_stats(&run_frames, &done);
				last_report = clock();
				fprintf(stderr, "[WIISP] %.1f s de juego (frame %d), %.1f MIPS\n", i / 60.0, i,
				        done / ((double)(last_report - start) / CLOCKS_PER_SEC) / 1e6);
			}
		}
		if(stop_requested) hle_dump_state("detenido con Ctrl+C");
		else if(!exited) hle_dump_state("limite de frames alcanzado");   /* al --log */
		secs = (double)(clock() - start) / CLOCKS_PER_SEC;
		app_get_stats(&run_frames, &instr);
		fprintf(stderr, "[WIISP] %s tras %d frames (%.1f MIPS en este PC)\n",
		        exited ? app_exit_reason() : stop_requested ? "detenido con Ctrl+C" : "limite de frames alcanzado", i,
		        secs > 0 ? instr / secs / 1e6 : 0.0);
	}
	sampler_stop();
	if(screenshot) save_screenshot(screenshot);
	return 0;
}
