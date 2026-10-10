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
 *      menú. Con 2 / Y se cambia antes el renderizador: GX (la GPU del
 *      Wii, rápido) o software (exacto pero lento).
 *
 * Si el Homebrew Channel nos pasa una ruta como argumento, se abre directa.
 *
 * Pruebas automáticas (en Dolphin o en la consola): si existe
 * sd:/wiisp/autotest.txt (o autotest.txt junto al boot.dol), se ejecuta
 * cada programa de la lista (rutas relativas a la carpeta autotest/ de al
 * lado) sin mandos; su salida va a
 * <programa>.out y la captura que pida a <programa>.bmp. Las líneas "gx" y
 * "soft" eligen el renderizador y "frames N" el máximo de frames (1200).
 * Al final de cada programa se añaden a <programa>.stats los FPS y MIPS y
 * la última imagen va a <programa>.gx.bmp o .soft.bmp. Al acabar escribe
 * autotest.done y apaga.
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
#include "wii/gx_ge.h"
#include "frontend/app.h"
#include "hle/hle.h"
#include "core/prof.h"
#ifdef WIISP_PROF
extern unsigned long long ge_prof[8];
#endif

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
static unsigned long long host_clock(void){ return gettime(); }
static u64 prof_ticks(void){ return gettime(); }

/* Lo que añade el Wii al desglose de tiempos: el renderizador y, con GX,
   cuántos framebuffers movió y qué hizo la caché de texturas */
static void profile_hook(char *buf, size_t size){
	unsigned long long setup, state, present;
	unsigned cnt[4];
	int len;
	gx_ge_profile(&setup, &state, &present, cnt);
	if(gx_ge_enabled()){
		len = snprintf(buf, size, "modo rapido (GX): %u bajadas y %u subidas de framebuffer, %u imagenes desde la VRAM\n",
		               cnt[1], cnt[2], cnt[0]);
		if(len > 0 && (size_t)len < size) gx_ge_texture_report(buf + len, size - (size_t)len);
	} else snprintf(buf, size, "modo EXACTO (dibujo por software)");
}

/* Muestreo del desglose ([TIEMPOS] GE por dentro): una alarma cada 1 ms
   apunta qué estaba haciendo el emulador. Corre en una interrupción: solo
   cuenta (sin coma flotante). */
static syswd_t sample_alarm;
static int sample_alarm_on;

static void sample_cb(syswd_t alarm, void *arg){
	(void)alarm;
	(void)arg;
	prof_sample();
}

static void sampler_start(void){
	struct timespec tp = { 0, 1000000 };
	if(sample_alarm_on || SYS_CreateAlarm(&sample_alarm) != 0) return;
	SYS_SetPeriodicAlarm(sample_alarm, &tp, &tp, sample_cb, NULL);
	sample_alarm_on = 1;
}

static void sampler_stop(void){
	if(!sample_alarm_on) return;
	SYS_RemoveAlarm(sample_alarm);
	sample_alarm_on = 0;
}

static void run_program(void){
	int exited = 0;
	unsigned frames = 0, last_frames = 0;
	unsigned long long instr = 0, last_instr = 0;
	u64 start = now_ms(), last = start, elapsed;
	char overlay[48] = "";

	gx_ge_enable(menu_renderer_gx());
	gx_ge_set_lazy_textures(menu_lazy_textures());
	app_set_output(program_output);
	/* Desglose del tiempo real cada 30 s en wiisp.log ([TIEMPOS]) */
	prof_set_clock(prof_ticks, (u64)TB_TIMER_CLOCK * 1000ull);
	hle_set_profile_hook(profile_hook);
	if(app_start()){
		printf("Error: no se pudo iniciar el programa\n");
		return;
	}
	sampler_start();
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
		{
			int old = prof_switch(PROF_PRESENTAR);
			video_draw_psp_frame(overlay);
			prof_switch(old);
		}
	}
	sampler_stop();
	video_show_console();
	/* Antes de olvidar las texturas: el informe final las cuenta */
	if(!exited) hle_dump_state("detenido por el usuario");
	gx_ge_reset();

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
		printf("A: ejecutar   B: volver al menu\n");
		for(;;){
			Input in;
			printf("\r2/Y: renderizador %s  1/X: texturas %s  ",
			       menu_renderer_gx() ? "GX (rapido)      " : "software (exacto)",
			       menu_lazy_textures() ? "rapidas" : "seguras");
			fflush(stdout);
			do {
				VIDEO_WaitVSync();
				input_read(&in);
			} while(!(in.menu & (IN_ACCEPT | IN_BACK | IN_EXIT | IN_OPTION | IN_SWITCH)));
			if(in.menu & IN_OPTION){ menu_set_renderer_gx(!menu_renderer_gx()); continue; }
			if(in.menu & IN_SWITCH){ menu_set_lazy_textures(!menu_lazy_textures()); continue; }
			printf("\n");
			if(in.menu & IN_ACCEPT) break;
			return;
		}
		run_program();
	}
	printf("\nPulsa A o B para volver al menu.\n");
	wait_accept();
}

/* --- Pruebas automáticas ------------------------------------------------ */

static FILE *autotest_file;

static void autotest_output(const char *text, unsigned len){
	if(autotest_file) fwrite(text, 1, len, autotest_file);
}

/* Busca autotest.txt en sd:/wiisp/ y, si no, junto al boot.dol */
static int autotest(const char *appdir){
	FILE *list, *done;
	char base[300], line[256], path[600], out[620];
	int count = 0, max_frames = 1200;
	snprintf(base, sizeof(base), "sd:/wiisp");
	snprintf(path, sizeof(path), "%s/autotest.txt", base);
	list = fopen(path, "r");
	if(!list && appdir){
		snprintf(base, sizeof(base), "%s", appdir);
		snprintf(path, sizeof(path), "%s/autotest.txt", base);
		list = fopen(path, "r");
	}
	if(!list) return 0;
	printf("Pruebas automaticas (%s)\n", path);
	gx_ge_enable(1);
	while(fgets(line, sizeof(line), list)){
		int frames, exited = 0;
		line[strcspn(line, "\r\n")] = 0;
		if(!line[0] || line[0] == '#') continue;
		if(!strcmp(line, "gx")){ gx_ge_enable(1); continue; }
		if(!strcmp(line, "soft")){ gx_ge_enable(0); continue; }
		if(!strncmp(line, "frames ", 7)){ max_frames = atoi(line + 7); continue; }
		snprintf(path, sizeof(path), "%s/autotest/%s", base, line);
		printf("%s\n", line);
		snprintf(out, sizeof(out), "%s.bmp", path);
		app_set_screenshot_path(out);
		snprintf(out, sizeof(out), "%s.out", path);
		autotest_file = fopen(out, "w");
		app_set_verbose(0);
		app_set_output(autotest_output);
		if(app_load(path, 8, NULL) == 0 && app_start() == 0){
			u64 start = now_ms();
			unsigned done_frames;
			unsigned long long instr, ge0 = app_ge_host_ticks(), ps, pst, pp;
			unsigned cnt[4];
			FILE *st;
			app_set_host_clock(host_clock);
			prof_set_clock(NULL, 0);   /* las pruebas miden con lo suyo */
			gx_ge_profile(&ps, &pst, &pp, cnt);
			for(frames = 0; frames < max_frames && !exited; frames++){
				exited = app_run_frame();
				video_draw_psp_frame(NULL);
			}
			app_get_stats(&done_frames, &instr);
			snprintf(out, sizeof(out), "%s.stats", path);
			st = fopen(out, "a");
			if(st){
				u64 ms = now_ms() - start;
				fprintf(st, "%s: %u frames en %u ms: %.1f FPS, %.1f MIPS\n", gx_ge_enabled() ? "gx" : "soft",
				        done_frames, (unsigned)ms, ms ? done_frames * 1000.0 / ms : 0.0, ms ? instr / 1000.0 / ms : 0.0);
				gx_ge_profile(&ps, &pst, &pp, cnt);
				fprintf(st, "   GE %u ms (buferes/texturas %u, estado GX %u), presentar %u ms\n"
				            "   %u presentaciones desde la VRAM, %u bajadas, %u subidas, %u texturas decodificadas\n",
				        (unsigned)ticks_to_millisecs(app_ge_host_ticks() - ge0), (unsigned)ticks_to_millisecs(ps),
				        (unsigned)ticks_to_millisecs(pst), (unsigned)ticks_to_millisecs(pp), cnt[0], cnt[1], cnt[2], cnt[3]);
#ifdef WIISP_PROF
				fprintf(st, "   prof: vertices %u ms, backend %u ms, draw_prim %u ms (preparar %u ms), luces %u ms, decodificar %u ms\n",
				        (unsigned)ticks_to_millisecs(ge_prof[0]), (unsigned)ticks_to_millisecs(ge_prof[1]),
				        (unsigned)ticks_to_millisecs(ge_prof[2]), (unsigned)ticks_to_millisecs(ge_prof[3]),
				        (unsigned)ticks_to_millisecs(ge_prof[4]), (unsigned)ticks_to_millisecs(ge_prof[5]));
				memset(ge_prof, 0, sizeof(ge_prof));
#endif
				fclose(st);
			}
			snprintf(out, sizeof(out), "%s.%s.bmp", path, gx_ge_enabled() ? "gx" : "soft");
			app_write_bmp(out);
		}
		if(autotest_file) fclose(autotest_file);
		autotest_file = NULL;
		gx_ge_reset();
		count++;
	}
	fclose(list);
	snprintf(path, sizeof(path), "%s/autotest.done", base);
	done = fopen(path, "w");
	if(done){ fprintf(done, "%d\n", count); fclose(done); }
	hle_set_log_file(NULL);
	fatUnmount("sd:");
	SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	return 1;
}

int main(int argc, char **argv){
	char path[512], config[300], appdir[300] = "";
	const char *slash;

	video_init();
	input_init();

	printf("\n\nWIISP " WIISP_VERSION " - emulador de PSP para Wii\n\n");
	if(app_init() || !fatInitDefault()){
		printf("Error: %s\n", "no hay memoria o no se pudo montar la SD/USB");
		printf("Pulsa A o B para salir.\n");
		wait_accept();
		return 1;
	}

	/* La configuración vive junto al boot.dol (p. ej. sd:/apps/wiisp/) */
	slash = (argc > 0 && argv[0]) ? strrchr(argv[0], '/') : NULL;
	if(slash){
		snprintf(appdir, sizeof(appdir), "%.*s", (int)(slash - argv[0]), argv[0]);
		snprintf(config, sizeof(config), "%s/wiisp.cfg", appdir);
		menu_set_config_path(config);
	}

	/* Partidas y registro junto a WIISP (sd:/apps/wiisp/ o sd:/wiisp/) */
	{
		char dir[340];
		snprintf(dir, sizeof(dir), "%s/SAVEDATA", appdir[0] ? appdir : "sd:/wiisp");
		io_set_savedata_dir(dir);
		snprintf(dir, sizeof(dir), "%s/wiisp.log", appdir[0] ? appdir : "sd:/wiisp");
		hle_set_log_file(dir);
	}

	autotest(appdir[0] ? appdir : NULL);

	if(argc > 1 && argv[1] && access(argv[1], F_OK) == 0)
		open_file(argv[1]);

	while(menu_choose_file(path, sizeof(path)))
		open_file(path);
	hle_log_stats();
	hle_set_log_file(NULL);   /* cierra wiisp.log antes de volver al Homebrew Channel */
	fatUnmount("sd:");
	fatUnmount("usb:");
	return 0;
}
