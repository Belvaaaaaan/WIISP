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
 * Las salidas van a la carpeta de la sesión (sd:/wiisp/0001, 0002...), con
 * el nombre del programa sin su ruta.
 * Al final de cada programa se añaden a <programa>.stats los FPS y MIPS y
 * la última imagen va a <programa>.gx.bmp o .soft.bmp. Al acabar escribe
 * autotest.done y apaga.
 *
 * Escenas de prueba repetibles (ver frontend/guion.h):
 *   - Una ruta con dispositivo (sd:/ISO/juego.iso) se usa tal cual.
 *   - Si existe <programa>.guion, se reproduce: el mando lo pone el guion, el
 *     límite de frames es su línea "fin", las partidas salen de
 *     <programa>.partidas (vaciada y rellenada desde <programa>.partidas_base
 *     si existe) y el registro completo, con [TIEMPOS], va a <programa>.log.
 *     Las huellas van a <programa>.huellas y las capturas a
 *     <programa>.c<cuadro>.bmp.
 *   - "veces N" corre N veces cada programa siguiente (salidas con .v1,
 *     .v2...), para comprobar que la escena sale idéntica.
 *   - "opt NOMBRE 0|1" cambia un interruptor de optimización (core/opt.h)
 *     para los programas siguientes.
 *   - "sufijo X": las salidas de los programas siguientes llevan ".X" (para
 *     correr el mismo juego con otras opciones; "sufijo" solo lo quita);
 *   - "capturas N": con guion, una captura cada N cuadros (400 por defecto;
 *     0 = ninguna, para medir velocidad), apuntada con sus opciones en
 *     <programa>.capturas.txt.
 *
 * Capturas: al abrir un juego, -/L las activa o quita (se recuerda en
 * wiisp.cfg). Activadas, se guarda una captura cada 400 cuadros, con o sin
 * guion; sin guion van a <juego>.jN.c<cuadro>.bmp y <juego>.jN.capturas.txt.
 *
 * Guiones desde el menú: al abrir un juego, +/R pasa por "guion no",
 * "GRABAR" (se juega normalmente desde el arranque y al salir queda
 * <juego>.guion, con sus partidas de prueba en <juego>.partidas) y
 * "REPRODUCIR" (el juego se juega solo con <juego>.guion y las salidas van a
 * <juego>.rN.*, con N el primer número libre).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>   //Arreglo
#include <fat.h>
#include "wii/wii.h"
#include "core/opt.h"   //Arreglo
#include "wii/gx_ge.h"
#include "frontend/app.h"
#include "frontend/wiimote.h"
#include "hle/hle.h"
#include "core/prof.h"
#include "cpu/predec.h"   //arreglo
#include "cpu/jit.h"   //Arreglo
#ifdef WIISP_PROF
extern unsigned long long ge_prof[8];
#endif

/* La RAM de la PSP (32 MB) no cabe en MEM1 (24 MB), así que se recorta
   del principio de la arena de MEM2 antes de que malloc la use.
   Se llama al arrancar, antes de crear hilos. */
/* Dónde empieza el montón en cada memoria (para la línea de memoria) */
static u32 heap1_ini, heap2_ini;

void *plat_alloc_big(size_t size){
	u8 *lo, *hi;

	size = (size + 31) & ~(size_t)31;
	lo = (u8 *)(((u32)SYS_GetArena2Lo() + 31) & ~31u);
	hi = (u8 *)SYS_GetArena2Hi();
	if(lo + size > hi) return NULL;
	SYS_SetArena2Lo(lo + size);
	heap2_ini = (u32)(lo + size);   /* lo que el montón tome de MEM2 irá detrás */
	return lo;
}

//arreglo
/* 3 MB de MEM1 para los bloques del intérprete predecodificado
   (predec_mem1): se recortan del principio de la arena de MEM1 antes de
   que malloc la use, como la RAM de la PSP en MEM2. Sin el interruptor no
   se usan (el montón tiene 3 MB menos de MEM1 y sigue en MEM2). */
static void reserva_mem1(void){
	const u32 size = 3u << 20;
	u8 *lo = (u8 *)(((u32)SYS_GetArena1Lo() + 31) & ~31u), *hi = (u8 *)SYS_GetArena1Hi();
	if(lo + size > hi) return;
	SYS_SetArena1Lo(lo + size);
	cpu_predec_reserva(lo, size);
}

//Arreglo
static void *arena_jit(u32 bytes){
	u32 nivel, hi, lo, r;
	u32 *p = NULL;
	_CPU_ISR_Disable(nivel);
	hi = (u32)SYS_GetArena2Hi() & ~31u;
	lo = (u32)SYS_GetArena2Lo();
	if(hi > lo && hi - lo >= bytes + (1u << 20) && broadway_ibat_cubre(hi - bytes, bytes)){
		p = (u32 *)(hi - bytes);
		SYS_SetArena2Hi(p);
	}
	_CPU_ISR_Restore(nivel);
	if(!p){
		hle_log_quiet("[JIT] sin arena: MEM2 libre %08X-%08X, ningun IBAT la cubre o no cabe\n", lo, hi);
		return NULL;
	}
	p[0] = 0x386005A5u;
	p[1] = 0x4E800020u;
	jit_publicar(p, 8);
	r = ((u32 (*)(void))p)();
	hle_log_quiet("[JIT] arena de %u KB en %08X (punta de MEM2): la prueba da %X (bien: 5A5)\n", bytes / 1024, (u32)p, r);
	return r == 0x5A5 ? p : NULL;
}

/* Rutas normales de las partidas y del registro (las pruebas las cambian) */
static char savedata_normal[340], log_normal[340];

/* Carpeta de esta sesión: <carpeta de WIISP>/0001, 0002... (la primera
   libre). Todo lo que se escribe al jugar o al correr pruebas (wiisp.log,
   registros, huellas, capturas y estadísticas de cada juego) va dentro, en
   vez de junto a la ISO. */
static char session_dir[300];

static void session_open(const char *wiisp_dir){
	struct stat st;
	int n;
	mkdir(wiisp_dir, 0777);
	for(n = 1; n <= 9999; n++){
		snprintf(session_dir, sizeof(session_dir), "%s/%04d", wiisp_dir, n);
		if(stat(session_dir, &st) != 0) break;
	}
	if(n > 9999 || mkdir(session_dir, 0777) != 0) snprintf(session_dir, sizeof(session_dir), "%s", wiisp_dir);
}

/* La salida de un juego en la carpeta de la sesión: su nombre sin la ruta */
static void session_path(char *out, size_t size, const char *path){
	const char *name = strrchr(path, '/');
	if(!name) name = strrchr(path, ':');
	name = name ? name + 1 : path;
	snprintf(out, size, "%s/%s", session_dir, name);
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
		if(len > 0 && (size_t)len < size){
			gx_ge_texture_report(buf + len, size - (size_t)len);
			len = (int)strlen(buf);
			if((size_t)len + 2 < size){
				buf[len++] = '\n';
				gx_ge_rtt_report(buf + len, size - (size_t)len);
			}
		}
	} else snprintf(buf, size, "modo EXACTO (dibujo por software)");
	/* Memoria: para ver fugas en sesiones largas o al cambiar de juego. El
	   montón empieza en MEM1 y sigue en MEM2 (detrás de la RAM de la PSP):
	   mallinfo cuenta como en uso el hueco de direcciones entre las dos, así
	   que lo que usa de verdad es lo que pidió a cada una menos lo libre */
	len = (int)strlen(buf);
	if((size_t)len + 200 < size){
		struct mallinfo mi = mallinfo();
		unsigned tb, tn, sn, sb, df;
		u32 pedido = ((u32)SYS_GetArena1Lo() - heap1_ini) + ((u32)SYS_GetArena2Lo() - heap2_ini);
		u32 en_uso = pedido > (u32)mi.fordblks ? pedido - (u32)mi.fordblks : 0;
		gx_ge_memory(&tb, &tn, &sn, &sb, &df);
		snprintf(buf + len, size - (size_t)len, "%smemoria: monton %u KB en uso y %u KB libres dentro; MEM1 libre %u KB, "
		         "MEM2 libre %u KB; texturas %u KB (%u); buferes %u (%u KB); %u por liberar",
		         len ? "\n" : "", en_uso / 1024, (unsigned)mi.fordblks / 1024,
		         (unsigned)((u32)SYS_GetArena1Hi() - (u32)SYS_GetArena1Lo()) / 1024,
		         (unsigned)((u32)SYS_GetArena2Hi() - (u32)SYS_GetArena2Lo()) / 1024, tb / 1024, tn, sn, sb / 1024, df);
	}
}

/* Las opciones con que corre el juego, para las capturas y las huellas:
   renderizador, M(atemática) R(ápida)/E(xacta), T(exturas) N(ormalizadas)/
   S(in normalizar), caché de texturas y turbo */
static void mode_text(char *buf, size_t size){
	if(gx_ge_enabled())
		snprintf(buf, size, "GX M%c T%c texturas %s%s", app_fast_math() ? 'R' : 'E', gx_ge_tex_norm() ? 'N' : 'S',
		         menu_lazy_textures() ? "rapidas" : "seguras", app_turbo() ? " TURBO" : "");
	else snprintf(buf, size, "software (exacto)%s", app_turbo() ? " TURBO" : "");
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
	gx_ge_set_tex_norm(1);
	app_set_turbo(0);
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
	printf("\n--- Ejecutando (mantener HOME o Z+START para volver) ---\n");
	hle_set_log_screen(0);   /* la consola no se ve: solo a wiisp.log */
	while(!exited){
		Input in;
		u64 t;
		int diag;
		input_read(&in);
		if(input_wants_exit(&in)) break;
		if(input_turbo_toggle(&in)){
			app_set_turbo(!app_turbo());
			hle_log_quiet("[TURBO] %s\n", app_turbo() ? "activado (sin dibujar)" : "quitado");
		}
		diag = input_diag_toggle(&in);
		if(diag && gx_ge_enabled()){
			if(diag & WM_DIAG_MATH){
				app_set_fast_math(!app_fast_math());
				hle_log_quiet("[DIAG] geometria con matematica %s\n", app_fast_math() ? "rapida (float)" : "exacta (la del GE)");
			}
			if(diag & WM_DIAG_TEX){
				gx_ge_set_tex_norm(!gx_ge_tex_norm());
				hle_log_quiet("[DIAG] perspectiva de texturas %s\n", gx_ge_tex_norm() ? "normalizada por triangulo" : "sin normalizar (como la 0.5.3)");
			}
		}
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
			/* Con HOME pulsado, que se vea que va a salir */
			char text[64];
			if(input_home_ms() > 0) snprintf(text, sizeof(text), "SALIR...");
			else {
				/* Con GX: M(atemática) R(ápida) o E(xacta), T(exturas)
				   N(ormalizadas) o S(in normalizar) */
				char tag[8] = "";
				if(gx_ge_enabled()) snprintf(tag, sizeof(tag), "M%c T%c  ", app_fast_math() ? 'R' : 'E', gx_ge_tex_norm() ? 'N' : 'S');
				snprintf(text, sizeof(text), "%s%s%s%s",
				         app_guion_activo() == 2 ? "GRABANDO  " : app_guion_activo() == 1 ? "GUION  " : "",
				         app_turbo() ? "TURBO  " : "", tag, overlay);
			}
			video_draw_psp_frame(text);
			prof_switch(old);
		}
	}
	sampler_stop();
	hle_set_log_screen(1);
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

/* Grabar un guion: partidas de prueba y <juego>.guion (el anterior queda
   como .guion.bak). 0 = grabando. */
static int start_recording(const char *path){
	char guion[560], bak[580], partidas[560], base[580];
	int fotos;
	snprintf(partidas, sizeof(partidas), "%s.partidas", path);
	snprintf(base, sizeof(base), "%s_base", partidas);
	/* Se empieza desde tus partidas reales de este juego, copiadas: el guion
	   se reproduce luego con esa misma copia y las reales no se tocan */
	fotos = app_foto_partidas(base, savedata_normal);
	if(fotos < 0){
		printf("\nError: no se pudieron copiar las partidas a %s\n", base);
		return -1;
	}
	printf("Partidas de %s copiadas para el guion: %d\n", app_game_id()[0] ? app_game_id() : "?", fotos);
	if(app_preparar_partidas(partidas, base)){
		printf("\nError: no se pudieron preparar %s\n", partidas);
		return -1;
	}
	snprintf(guion, sizeof(guion), "%s.guion", path);
	snprintf(bak, sizeof(bak), "%s.bak", guion);
	if(access(guion, F_OK) == 0){
		unlink(bak);
		rename(guion, bak);
	}
	if(app_guion_grabar(guion)){
		printf("\nError: no se puede escribir %s\n", guion);
		app_set_savedata_dir(savedata_normal);
		return -1;
	}
	hle_log_quiet("[GUION] grabando %s (partidas en %s, %d copiadas de %s)\n", guion, partidas, fotos, savedata_normal);
	return 0;
}

/* Reproducir <juego>.guion desde el menú: las salidas van a <juego>.rN.*
   (el primer N libre, para poder comparar una reproducción con otra) y el
   registro de esta corrida a <juego>.rN.log. 0 = listo. */
static char replay_prefix[580];

static int start_replay(const char *path){
	char guion[560], partidas[560], base[580], probe[620], err[160];
	int n;
	snprintf(guion, sizeof(guion), "%s.guion", path);
	if(access(guion, F_OK) != 0){
		printf("\nNo hay guion: primero graba uno (%s)\n", guion);
		return -1;
	}
	for(n = 1; n < 1000; n++){
		char out[560];
		session_path(out, sizeof(out), path);
		snprintf(replay_prefix, sizeof(replay_prefix), "%s.r%d", out, n);
		snprintf(probe, sizeof(probe), "%s.huellas", replay_prefix);
		if(access(probe, F_OK) != 0) break;
	}
	snprintf(partidas, sizeof(partidas), "%s.partidas", path);
	snprintf(base, sizeof(base), "%s_base", partidas);
	if(app_preparar_partidas(partidas, base)){
		printf("\nError: no se pudieron preparar %s\n", partidas);
		return -1;
	}
	snprintf(probe, sizeof(probe), "%s.log", replay_prefix);
	hle_set_log_file(probe);
	if(app_guion_reproducir(guion, replay_prefix, err, sizeof(err))){
		printf("\nGuion %s: %s\n", guion, err);
		app_set_savedata_dir(savedata_normal);
		hle_reopen_log_file(log_normal);
		return -1;
	}
	hle_log("[GUION] reproduciendo %s hasta el cuadro %u\n", guion, app_guion_fin());
	return 0;
}

/* Capturas jugando sin guion: <juego>.jN.* (el primer N libre) */
static char shots_prefix[580];

static void start_screenshots(const char *path){
	char probe[620];
	int n;
	for(n = 1; n < 1000; n++){
		char out[560];
		session_path(out, sizeof(out), path);
		snprintf(shots_prefix, sizeof(shots_prefix), "%s.j%d", out, n);
		snprintf(probe, sizeof(probe), "%s.capturas.txt", shots_prefix);
		if(access(probe, F_OK) != 0) break;
	}
	app_capturas_sin_guion(shots_prefix);
}

static void open_file(const char *path){
	enum { GUION_NO, GUION_GRABAR, GUION_REPRODUCIR };
	static const char *const guion_names[] = { "no        ", "GRABAR    ", "REPRODUCIR" };
	char imports[512];
	static int guion_mode;

	video_clear_console();
	app_imports_path(path, imports, sizeof(imports));
	if(app_load(path, 8, imports) == 0){
		menu_remember(path);
		printf("A: ejecutar   B: volver al menu\n");
		for(;;){
			Input in;
			printf("\r2/Y: renderizador %s  1/X: texturas %s\n+/R: guion %s  -/L: capturas %s",
			       menu_renderer_gx() ? "GX (rapido)      " : "software (exacto)",
			       menu_lazy_textures() ? "rapidas" : "seguras", guion_names[guion_mode],
			       menu_screenshots() ? "SI (cada 400 cuadros)" : "no                   ");
			printf("\x1b[1A");   /* vuelve a la primera línea para repintar las dos */
			fflush(stdout);
			do {
				VIDEO_WaitVSync();
				input_read(&in);
			} while(!(in.menu & (IN_ACCEPT | IN_BACK | IN_EXIT | IN_OPTION | IN_SWITCH | IN_EXTRA | IN_EXTRA2)));
			if(in.menu & IN_OPTION){ menu_set_renderer_gx(!menu_renderer_gx()); continue; }
			if(in.menu & IN_SWITCH){ menu_set_lazy_textures(!menu_lazy_textures()); continue; }
			if(in.menu & IN_EXTRA){ guion_mode = (guion_mode + 1) % 3; continue; }
			if(in.menu & IN_EXTRA2){ menu_set_screenshots(!menu_screenshots()); continue; }
			printf("\n\n");
			if(in.menu & IN_ACCEPT) break;
			return;
		}
		app_guion_capturas_cada(menu_screenshots() ? 400 : 0);
		if((guion_mode == GUION_GRABAR && start_recording(path)) ||
		   (guion_mode == GUION_REPRODUCIR && start_replay(path))){
			guion_mode = GUION_NO;
			printf("Pulsa A o B para volver al menu.\n");
			wait_accept();
			return;
		}
		if(guion_mode == GUION_NO && menu_screenshots()) start_screenshots(path);
		run_program();
		if(!app_guion_activo() && shots_prefix[0]){
			app_guion_parar();
			printf("Capturas en %s.c*.bmp (lista en %s.capturas.txt)\n", shots_prefix, shots_prefix);
		}
		shots_prefix[0] = 0;
		if(app_guion_activo()){
			int replay = app_guion_activo() == 1;
			if(replay) hle_profile_report("final de la escena");
			app_guion_parar();
			app_set_savedata_dir(savedata_normal);
			if(replay){
				hle_reopen_log_file(log_normal);
				printf("Reproduccion guardada en %s.* (log, huellas, capturas)\n", replay_prefix);
			} else printf("Guion grabado en %s.guion\n", path);
		}
		guion_mode = GUION_NO;   /* que no se vuelva a grabar encima sin querer */
	}
	printf("\nPulsa A o B para volver al menu.\n");
	wait_accept();
}

/* --- Pruebas automáticas ------------------------------------------------ */

static FILE *autotest_file;

static void autotest_output(const char *text, unsigned len){
	if(autotest_file) fwrite(text, 1, len, autotest_file);
}

/* Una ejecución de la lista de pruebas. prefix es la ruta de las salidas
   (la del programa, más ".vN" si se repite). */
static void autotest_run(const char *path, const char *prefix, int max_frames){
	char out[700], guion[640];
	int frames, exited = 0, scripted;

	snprintf(guion, sizeof(guion), "%s.guion", path);
	scripted = access(guion, F_OK) == 0;
	snprintf(out, sizeof(out), "%s.bmp", prefix);
	app_set_screenshot_path(out);
	snprintf(out, sizeof(out), "%s.out", prefix);
	autotest_file = fopen(out, "w");
	app_set_verbose(0);
	app_set_output(autotest_output);
	if(scripted){
		char partidas[640], base[660];
		snprintf(partidas, sizeof(partidas), "%s.partidas", path);
		snprintf(base, sizeof(base), "%s_base", partidas);
		int bad = app_preparar_partidas(partidas, base);
		if(bad) printf("  no se pudieron preparar %s\n", partidas);
		snprintf(out, sizeof(out), "%s.log", prefix);
		hle_set_log_file(out);
		if(bad) hle_log("[GUION] no se pudieron preparar %s: se usan las partidas normales\n", partidas);
		if(opt_on[OPT_PRUEBA_PS]) broadway_prueba_ps(1u << 23);   //Arreglo
		if(opt_on[OPT_PRUEBA_JIT]) jit_banco();   //Arreglo
	}
	if(app_load(path, 8, NULL) == 0){
		char err[160];
		if(scripted && app_guion_reproducir(guion, prefix, err, sizeof(err))){
			printf("  guion: %s\n", err);
			hle_log("[GUION] %s: %s\n", guion, err);
			scripted = -1;   /* no se ejecuta: sin guion la escena no vale */
		}
	} else exited = -1;   /* no se pudo cargar */
	if(scripted >= 0 && exited == 0 && app_start() == 0){
		u64 start = now_ms();
		unsigned done_frames;
		unsigned long long instr, ge0 = app_ge_host_ticks(), ps, pst, pp;
		unsigned cnt[4];
		FILE *st;
		app_set_host_clock(host_clock);
		if(scripted){
			/* Como al jugar: desglose de tiempos cada 30 s en el registro */
			hle_log("[GUION] %s: hasta el cuadro %u, %s\n", guion, app_guion_fin(),
			        gx_ge_enabled() ? "GX" : "software");
			prof_set_clock(prof_ticks, (u64)TB_TIMER_CLOCK * 1000ull);
			hle_set_profile_hook(profile_hook);
			sampler_start();
			if(app_guion_fin()) max_frames = (int)app_guion_fin() + 1;
		} else prof_set_clock(NULL, 0);   /* las pruebas miden con lo suyo */
		gx_ge_profile(&ps, &pst, &pp, cnt);
		hle_set_log_screen(0);   /* como al jugar: la consola no se ve */
		for(frames = 0; frames < max_frames && !exited; frames++){
			exited = app_run_frame();
			video_draw_psp_frame(NULL);
		}
		if(scripted){
			hle_profile_report("final de la escena");
			sampler_stop();
			prof_set_clock(NULL, 0);
		}
		hle_set_log_screen(1);
		app_get_stats(&done_frames, &instr);
		snprintf(out, sizeof(out), "%s.stats", prefix);
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
			/* Por vértice y por primitiva ya no se mide con el reloj (costaba
			   ~3 lecturas por vértice): eso lo dan el muestreo y prof_pmc */
			fprintf(st, "   prof: draw_prim %u ms (preparar %u ms)\n",
			        (unsigned)ticks_to_millisecs(ge_prof[2]), (unsigned)ticks_to_millisecs(ge_prof[3]));
			memset(ge_prof, 0, sizeof(ge_prof));
#endif
			fclose(st);
		}
		gx_ge_sync_vram();
		snprintf(out, sizeof(out), "%s.%s.bmp", prefix, gx_ge_enabled() ? "gx" : "soft");
		app_write_bmp(out);
	}
	app_guion_parar();
	if(autotest_file) fclose(autotest_file);
	autotest_file = NULL;
	gx_ge_reset();
	if(scripted){
		app_set_savedata_dir(savedata_normal);
		hle_reopen_log_file(log_normal);
	}
}

/* Busca autotest.txt en sd:/wiisp/ y, si no, junto al boot.dol */
static int autotest(const char *appdir){
	FILE *list, *done;
	char base[300], line[256], path[600], prefix[620];
	int count = 0, max_frames = 1200, times = 1;
	char suffix[40] = "";
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
	app_set_osk(NULL);   /* sin nadie delante, el teclado se cierra solo */
	gx_ge_enable(1);
	while(fgets(line, sizeof(line), list)){
		int k;
		line[strcspn(line, "\r\n")] = 0;
		if(!line[0] || line[0] == '#') continue;
		if(!strcmp(line, "gx")){ gx_ge_enable(1); continue; }
		if(!strcmp(line, "soft")){ gx_ge_enable(0); continue; }
		if(!strncmp(line, "frames ", 7)){ max_frames = atoi(line + 7); continue; }
		if(!strncmp(line, "veces ", 6)){ times = atoi(line + 6); if(times < 1) times = 1; continue; }
		if(!strncmp(line, "capturas ", 9)){ app_guion_capturas_cada((unsigned)atoi(line + 9)); continue; }
		if(!strncmp(line, "ventana ", 8)){ hle_set_ventana_juego((u32)atoi(line + 8)); continue; }   //Arreglo
		if(!strncmp(line, "sufijo", 6)){
			/* Para correr el mismo juego con otras opciones sin pisar las salidas */
			const char *v = line + 6;
			while(*v == ' ') v++;
			snprintf(suffix, sizeof(suffix), "%.39s", v);
			continue;
		}
		if(!strncmp(line, "opt ", 4)){
			int r = app_opt_set(line + 4);
			if(r) printf("%s: %s\n", line, r > 0 ? "todavia no esta implementada" : "no se entiende");
			hle_log("[OPT] %s%s\n", line + 4, r > 0 ? " (todavia no esta implementada)" : r ? " (no se entiende)" : "");
			continue;
		}
		if(strstr(line, ":/")) snprintf(path, sizeof(path), "%s", line);
		else snprintf(path, sizeof(path), "%s/autotest/%s", base, line);
		for(k = 1; k <= times; k++){
			char base_prefix[sizeof(prefix) - 8], out[560];
			session_path(out, sizeof(out), path);
			snprintf(base_prefix, sizeof(base_prefix), "%s%s%s", out, suffix[0] ? "." : "", suffix);
			if(times > 1) snprintf(prefix, sizeof(prefix), "%s.v%d", base_prefix, k);
			else snprintf(prefix, sizeof(prefix), "%s", base_prefix);
			printf("%s%s\n", line, times > 1 ? (k == 1 ? " (vez 1)" : " (otra vez)") : "");
			autotest_run(path, prefix, max_frames);
			count++;
		}
	}
	fclose(list);
	snprintf(path, sizeof(path), "%s/autotest.done", base);
	done = fopen(path, "w");
	if(done){ fprintf(done, "%d\n", count); fclose(done); }
	app_shutdown();
	hle_set_log_file(NULL);
	fatUnmount("sd:");
	SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	return 1;
}

/* Vuelta al Homebrew Channel (o al menú del Wii). El orden importa: antes de
   desmontar la SD hay que cerrar todo lo que siga abierto (la ISO, los
   archivos del juego, wiisp.log), porque al salir se cierran los archivos
   pendientes y uno que apunte a una SD desmontada hace saltar la pantalla
   roja de error. También se deja la GPU parada y la imagen en negro, para
   que no se vea basura mientras carga el Homebrew Channel. */
static void shutdown_and_exit(int status){
	app_shutdown();
	hle_log_stats();
	hle_log_quiet("[WIISP] saliendo\n");
	hle_set_log_file(NULL);
	fflush(stdout);
	fatUnmount("sd:");
	fatUnmount("usb:");
	GX_DrawDone();
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	exit(status);
}

int main(int argc, char **argv){
	char path[512], config[300], appdir[300] = "";
	const char *slash;

	reserva_mem1();   //arreglo
	heap1_ini = (u32)SYS_GetArena1Lo();
	video_init();
	input_init();
	app_set_osk(wii_osk);   /* teclado en pantalla para los juegos que lo piden */

	printf("\n\nWIISP " WIISP_VERSION " - emulador de PSP para Wii\n\n");
	if(app_init() || !fatInitDefault()){
		printf("Error: %s\n", "no hay memoria o no se pudo montar la SD/USB");
		printf("Pulsa A o B para salir.\n");
		wait_accept();
		shutdown_and_exit(1);
	}

	/* La configuración vive junto al boot.dol (p. ej. sd:/apps/wiisp/) */
	slash = (argc > 0 && argv[0]) ? strrchr(argv[0], '/') : NULL;
	if(slash){
		snprintf(appdir, sizeof(appdir), "%.*s", (int)(slash - argv[0]), argv[0]);
		snprintf(config, sizeof(config), "%s/wiisp.cfg", appdir);
		menu_set_config_path(config);
	}

	/* Partidas junto a WIISP (sd:/apps/wiisp/ o sd:/wiisp/); el registro, en
	   la carpeta de la sesión */
	snprintf(savedata_normal, sizeof(savedata_normal), "%s/SAVEDATA", appdir[0] ? appdir : "sd:/wiisp");
	app_set_savedata_dir(savedata_normal);
	session_open("sd:/wiisp");
	snprintf(log_normal, sizeof(log_normal), "%s/wiisp.log", session_dir);
	hle_set_log_file(log_normal);
	broadway_report();
	broadway_pmc_init();
	jit_pedir_arena = arena_jit;   //Arreglo
	/* Las capturas de los guiones necesitan la imagen en la VRAM emulada */
	app_set_before_capture(gx_ge_sync_vram);
	app_set_mode_text(mode_text);

	autotest(appdir[0] ? appdir : NULL);

	if(argc > 1 && argv[1] && access(argv[1], F_OK) == 0)
		open_file(argv[1]);

	while(menu_choose_file(path, sizeof(path)))
		open_file(path);
	shutdown_and_exit(0);
	return 0;
}
