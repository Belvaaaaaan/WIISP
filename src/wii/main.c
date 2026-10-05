/**
 * WIISP - wii/main.c
 * Frontend del Wii: consola de texto, SD/USB, vídeo y mandos.
 *
 * Flujo (fase 2):
 *   1. Busca el EBOOT.PBP, lo carga y muestra su información.
 *   2. Escribe imports.txt junto al EBOOT (lista completa de funciones del
 *      firmware que usa, con su nombre y si WIISP ya las implementa).
 *   3. Con A lo ejecuta. Lo que el programa escribe por stdout sale en la
 *      consola; si configura un framebuffer, se muestra en la tele (1:1,
 *      centrado). HOME / Z+START salen.
 *
 * El EBOOT.PBP se busca en:
 *   - el primer argumento, si el Homebrew Channel nos pasa uno
 *   - la carpeta del propio boot.dol (p. ej. sd:/apps/wiisp/EBOOT.PBP)
 *   - sd:/apps/wiisp/, usb:/apps/wiisp/, sd:/wiisp/ y usb:/wiisp/
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <fat.h>
#include <wiiuse/wpad.h>
#include "frontend/app.h"

static void *console_xfb = NULL;
static u32 *game_xfb = NULL;
static GXRModeObj *rmode = NULL;
static int showing_game;

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

static void init_video(void){
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	console_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	game_xfb = (u32 *)SYS_AllocateFramebuffer(rmode); /* con caché: se escribe entero */
	console_init(console_xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(console_xfb);
	VIDEO_SetBlack(0);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static int file_exists(const char *path){
	return access(path, F_OK) == 0;
}

/* --- Mandos ----------------------------------------------------------- */

typedef struct { u32 down, held; } Pads;

static Pads read_pads(void){
	Pads p;
	WPAD_ScanPads();
	PAD_ScanPads();
	p.down = WPAD_ButtonsDown(0) | ((u32)PAD_ButtonsDown(0) << 16);
	p.held = WPAD_ButtonsHeld(0) | ((u32)PAD_ButtonsHeld(0) << 16);
	return p;
}

#define GC(b) ((u32)(b) << 16)

static int wants_exit(const Pads *p){
	return (p->down & WPAD_BUTTON_HOME) ||
	       ((p->held & GC(PAD_TRIGGER_Z)) && (p->down & GC(PAD_BUTTON_START)));
}

/* Wiimote en vertical o mando de GameCube -> botones de la PSP */
static void send_input(const Pads *p){
	static const struct { u32 wii; unsigned psp; } map[] = {
		{ WPAD_BUTTON_UP, APP_BTN_UP },       { WPAD_BUTTON_DOWN, APP_BTN_DOWN },
		{ WPAD_BUTTON_LEFT, APP_BTN_LEFT },   { WPAD_BUTTON_RIGHT, APP_BTN_RIGHT },
		{ WPAD_BUTTON_A, APP_BTN_CROSS },     { WPAD_BUTTON_B, APP_BTN_CIRCLE },
		{ WPAD_BUTTON_1, APP_BTN_SQUARE },    { WPAD_BUTTON_2, APP_BTN_TRIANGLE },
		{ WPAD_BUTTON_PLUS, APP_BTN_START },  { WPAD_BUTTON_MINUS, APP_BTN_SELECT },
		{ GC(PAD_BUTTON_UP), APP_BTN_UP },    { GC(PAD_BUTTON_DOWN), APP_BTN_DOWN },
		{ GC(PAD_BUTTON_LEFT), APP_BTN_LEFT },{ GC(PAD_BUTTON_RIGHT), APP_BTN_RIGHT },
		{ GC(PAD_BUTTON_A), APP_BTN_CROSS },  { GC(PAD_BUTTON_B), APP_BTN_CIRCLE },
		{ GC(PAD_BUTTON_Y), APP_BTN_SQUARE }, { GC(PAD_BUTTON_X), APP_BTN_TRIANGLE },
		{ GC(PAD_BUTTON_START), APP_BTN_START }, { GC(PAD_TRIGGER_Z), APP_BTN_SELECT },
		{ GC(PAD_TRIGGER_L), APP_BTN_LTRIGGER }, { GC(PAD_TRIGGER_R), APP_BTN_RTRIGGER },
	};
	unsigned buttons = 0, i;
	int sx = PAD_StickX(0), sy = PAD_StickY(0);
	for(i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if(p->held & map[i].wii) buttons |= map[i].psp;
	/* Stick de GameCube (-128..127, arriba positivo) -> PSP (0..255, arriba 0) */
	app_set_input(buttons, (unsigned char)(sx + 128), (unsigned char)(127 - sy));
}

/* --- Vídeo ------------------------------------------------------------ */

static inline void psp_pixel(const u8 *p, unsigned format, int *r, int *g, int *b){
	unsigned v = p[0] | (p[1] << 8);
	switch(format){
	case 0: *r = (v & 31) << 3; *g = ((v >> 5) & 63) << 2; *b = ((v >> 11) & 31) << 3; break;
	case 1: *r = (v & 31) << 3; *g = ((v >> 5) & 31) << 3; *b = ((v >> 10) & 31) << 3; break;
	case 2: *r = (v & 15) << 4; *g = ((v >> 4) & 15) << 4; *b = ((v >> 8) & 15) << 4; break;
	default: *r = p[0]; *g = p[1]; *b = p[2]; break;
	}
}

/* Copia el framebuffer de la PSP al XFB (YUY2: Y1 Cb Y2 Cr por cada dos
   píxeles), 1:1 y centrado. Más adelante lo hará GX con escalado. */
static void draw_psp_frame(void){
	unsigned stride, format, x, y, bpp;
	const u8 *fb = app_get_framebuffer(&stride, &format);
	u32 words_per_line = rmode->fbWidth / 2;
	u32 x0 = (rmode->fbWidth - APP_SCREEN_W) / 4;           /* en palabras */
	u32 y0 = (rmode->xfbHeight - APP_SCREEN_H) / 2;

	if(!fb) return;
	if(!showing_game){
		for(x = 0; x < words_per_line * rmode->xfbHeight; x++) game_xfb[x] = 0x00800080; /* negro */
		showing_game = 1;
	}
	bpp = format == 3 ? 4 : 2;
	for(y = 0; y < APP_SCREEN_H; y++){
		const u8 *src = fb + y * stride * bpp;
		u32 *dst = game_xfb + (y0 + y) * words_per_line + x0;
		for(x = 0; x < APP_SCREEN_W; x += 2, src += 2 * bpp){
			int r1, g1, b1, r2, g2, b2;
			psp_pixel(src, format, &r1, &g1, &b1);
			psp_pixel(src + bpp, format, &r2, &g2, &b2);
			int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
			int y2 = ((66 * r2 + 129 * g2 + 25 * b2 + 128) >> 8) + 16;
			int r = (r1 + r2) >> 1, g = (g1 + g2) >> 1, b = (b1 + b2) >> 1;
			int cb = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
			int cr = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
			*dst++ = ((u32)y1 << 24) | ((u32)cb << 16) | ((u32)y2 << 8) | (u32)cr;
		}
	}
	DCFlushRange(game_xfb, words_per_line * rmode->xfbHeight * 4);
	VIDEO_SetNextFramebuffer(game_xfb);
	VIDEO_Flush();
}

static void show_console(void){
	if(!showing_game) return;
	showing_game = 0;
	VIDEO_SetNextFramebuffer(console_xfb);
	VIDEO_Flush();
}

/* --- Bucle principal ---------------------------------------------------- */

static void program_output(const char *text, unsigned len){
	fwrite(text, 1, len, stdout);
}

static void wait_for_exit(void){
	printf("\nPulsa HOME (Wiimote) o START (GameCube) para salir.\n");
	for(;;){
		Pads p = read_pads();
		if((p.down & WPAD_BUTTON_HOME) || (p.down & GC(PAD_BUTTON_START))) break;
		VIDEO_WaitVSync();
	}
}

/* Devuelve 1 si el usuario quiere ejecutar (A), 0 si quiere salir */
static int ask_run(void){
	printf("\nPulsa A para ejecutar, HOME para salir.\n");
	for(;;){
		Pads p = read_pads();
		if((p.down & WPAD_BUTTON_A) || (p.down & GC(PAD_BUTTON_A))) return 1;
		if((p.down & WPAD_BUTTON_HOME) || (p.down & GC(PAD_BUTTON_START))) return 0;
		VIDEO_WaitVSync();
	}
}

static void run_program(void){
	int exited = 0, frames = 0;
	app_set_output(program_output);
	if(app_start()){
		printf("Error: no se pudo iniciar el programa\n");
		return;
	}
	printf("\n--- Ejecutando (HOME para salir) ---\n");
	while(!exited){
		Pads p = read_pads();
		if(wants_exit(&p)) break;
		send_input(&p);
		exited = app_run_frame();
		frames++;
		draw_psp_frame();
	}
	show_console();
	printf("\n--- %s tras %d frames ---\n",
	       exited ? app_exit_reason() : "detenido por el usuario", frames);
}

int main(int argc, char **argv){
	static const char *const default_paths[] = {
		"sd:/apps/wiisp/EBOOT.PBP",
		"usb:/apps/wiisp/EBOOT.PBP",
		"sd:/wiisp/EBOOT.PBP",
		"usb:/wiisp/EBOOT.PBP",
	};
	static char app_dir_path[256], imports_path[256];
	const char *path = NULL, *slash;
	unsigned i;

	init_video();
	WPAD_Init();
	PAD_Init();

	printf("\n\nWIISP - emulador de PSP para Wii (fase 2: interprete + HLE)\n\n");

	if(app_init()){
		wait_for_exit();
		return 1;
	}
	if(!fatInitDefault()){
		printf("Error: no se pudo montar la SD/USB\n");
		wait_for_exit();
		return 1;
	}

	if(argc > 1 && argv[1] && file_exists(argv[1])) path = argv[1];

	/* argv[0] es la ruta del boot.dol, p. ej. "sd:/apps/wiisp/boot.dol" */
	if(!path && argc > 0 && argv[0]){
		slash = strrchr(argv[0], '/');
		int dir_len = slash ? (int)(slash - argv[0]) : -1;
		if(dir_len > 0 && dir_len < (int)sizeof(app_dir_path) - 12){
			snprintf(app_dir_path, sizeof(app_dir_path), "%.*s/EBOOT.PBP", dir_len, argv[0]);
			if(file_exists(app_dir_path)) path = app_dir_path;
		}
	}

	for(i = 0; !path && i < sizeof(default_paths) / sizeof(default_paths[0]); i++)
		if(file_exists(default_paths[i])) path = default_paths[i];

	if(!path){
		printf("No se encontro EBOOT.PBP. Rutas probadas:\n");
		if(app_dir_path[0]) printf("  %s\n", app_dir_path);
		for(i = 0; i < sizeof(default_paths) / sizeof(default_paths[0]); i++)
			printf("  %s\n", default_paths[i]);
		wait_for_exit();
		return 0;
	}

	/* imports.txt va en la misma carpeta que el EBOOT */
	slash = strrchr(path, '/');
	snprintf(imports_path, sizeof(imports_path), "%.*s/imports.txt",
	         slash ? (int)(slash - path) : 0, path);

	if(app_load(path, 8, imports_path) == 0 && ask_run())
		run_program();
	else {
		wait_for_exit();
		return 0;
	}
	wait_for_exit();
	return 0;
}
