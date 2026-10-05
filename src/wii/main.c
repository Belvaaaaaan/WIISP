/**
 * WIISP - wii/main.c
 * Frontend del Wii: arranque y bucle principal.
 *
 *   1. Menú para elegir un .pbp/.prx/.elf en la SD o en el USB.
 *   2. Lo carga, muestra su información y escribe el informe de imports
 *      junto a él (imports.txt para un EBOOT.PBP, <nombre>.imports.txt para
 *      el resto).
 *   3. Con A lo ejecuta. El texto que escribe sale en la consola; si
 *      configura un framebuffer, se muestra en la tele con FPS, MIPS y
 *      velocidad respecto a una PSP real arriba. HOME / Z+START vuelven al
 *      menú.
 *
 * Si el Homebrew Channel nos pasa una ruta como argumento, se abre directa.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include "wii/wii.h"
#include "frontend/app.h"

/* La RAM de la PSP (32 MB) no cabe en MEM1 (24 MB), así que se recorta
   del principio de la arena de MEM2 antes de que malloc la use.
   Se llama al arrancar, antes de crear hilos. */
void *plat_alloc_big(size_t size){
	u8 *lo, *hi;

	size = (size + 31) & ~(size_t)31;
	lo = (u8 *)(((u32)SYS_GetArena2Lo() + 31) & ~31u);
	hi = (u8 *)SYS_GetArena2Hi();
	if(lo + size > hi) return NULL;
	SYS_SetArena2Lo(lo + size);
	return lo;
}

static void program_output(const char *text, unsigned len){
	fwrite(text, 1, len, stdout);
}

/* Espera A o B; devuelve 1 si fue A */
static int wait_accept(void){
	for(;;){
		Input in;
		input_read(&in);
		if(in.menu & IN_ACCEPT) return 1;
		if(in.menu & (IN_BACK | IN_EXIT)) return 0;
		VIDEO_WaitVSync();
	}
}

static u64 now_ms(void){ return ticks_to_millisecs(gettime()); }

static void run_program(void){
	int exited = 0;
	unsigned frames = 0, last_frames = 0;
	unsigned long long instr = 0, last_instr = 0;
	u64 start = now_ms(), last = start, elapsed;
	char overlay[48] = "";

	app_set_output(program_output);
	if(app_start()){
		printf("Error: no se pudo iniciar el programa\n");
		return;
	}
	printf("\n--- Ejecutando (HOME o Z+START para volver) ---\n");
	while(!exited){
		Input in;
		u64 t;
		input_read(&in);
		if(input_wants_exit(&in)) break;
		input_send_to_psp(&in);
		exited = app_run_frame();

		/* FPS emulados, millones de instrucciones de la PSP por segundo y
		   velocidad respecto a una PSP real (59,94 frames por segundo) */
		t = now_ms();
		if(t - last >= 1000){
			app_get_stats(&frames, &instr);
			double secs = (double)(t - last) / 1000.0;
			double fps = (frames - last_frames) / secs;
			double mips = (double)(instr - last_instr) / secs / 1e6;
			snprintf(overlay, sizeof(overlay), "%.1f FPS  %.1f MIPS  VEL %d%%",
			         fps, mips, (int)(fps * 100.0 / 59.94 + 0.5));
			last = t;
			last_frames = frames;
			last_instr = instr;
		}
		video_draw_psp_frame(overlay);
	}
	video_show_console();

	app_get_stats(&frames, &instr);
	elapsed = now_ms() - start;
	printf("\n--- %s tras %u frames ---\n",
	       exited ? app_exit_reason() : "detenido por el usuario", frames);
	if(elapsed)
		printf("Media: %.1f FPS, %.1f MIPS (%u frames en %.1f s)\n",
		       frames * 1000.0 / elapsed, instr / 1000.0 / elapsed, frames, elapsed / 1000.0);
}

static void open_file(const char *path){
	char imports[512];

	video_clear_console();
	app_imports_path(path, imports, sizeof(imports));
	if(app_load(path, 8, imports) == 0){
		menu_remember(path);
		printf("\nA: ejecutar   B: volver al menu\n");
		if(wait_accept()) run_program();
		else return;
	}
	printf("\nPulsa A o B para volver al menu.\n");
	wait_accept();
}

int main(int argc, char **argv){
	char path[512], config[300];
	const char *slash;

	video_init();
	input_init();

	printf("\n\nWIISP - emulador de PSP para Wii\n\n");
	if(app_init() || !fatInitDefault()){
		printf("Error: %s\n", "no hay memoria o no se pudo montar la SD/USB");
		printf("Pulsa A o B para salir.\n");
		wait_accept();
		return 1;
	}

	/* La configuración vive junto al boot.dol (p. ej. sd:/apps/wiisp/) */
	slash = (argc > 0 && argv[0]) ? strrchr(argv[0], '/') : NULL;
	if(slash){
		snprintf(config, sizeof(config), "%.*s/wiisp.cfg", (int)(slash - argv[0]), argv[0]);
		menu_set_config_path(config);
	}

	if(argc > 1 && argv[1] && access(argv[1], F_OK) == 0)
		open_file(argv[1]);

	while(menu_choose_file(path, sizeof(path)))
		open_file(path);
	return 0;
}
