/**
 * WIISP - wii/main.c
 * Frontend del Wii: consola de texto, SD/USB y Wiimote/mando de GameCube.
 *
 * Paso 1 del plan: carga un EBOOT.PBP y muestra su información. Ruta:
 *   - el primer argumento si el Homebrew Channel nos pasa uno, o
 *   - sd:/wiisp/EBOOT.PBP, o usb:/wiisp/EBOOT.PBP
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gccore.h>
#include <fat.h>
#include <wiiuse/wpad.h>
#include "frontend/app.h"

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

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
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(0);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static int file_exists(const char *path){
	return access(path, F_OK) == 0;
}

static void wait_for_exit(void){
	printf("\nPulsa HOME (Wiimote) o START (GameCube) para salir.\n");
	for(;;){
		WPAD_ScanPads();
		PAD_ScanPads();
		if(WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
		if(PAD_ButtonsDown(0) & PAD_BUTTON_START) break;
		VIDEO_WaitVSync();
	}
}

int main(int argc, char **argv){
	static const char *const default_paths[] = {
		"sd:/wiisp/EBOOT.PBP",
		"usb:/wiisp/EBOOT.PBP",
	};
	const char *path = NULL;
	unsigned i;

	init_video();
	WPAD_Init();
	PAD_Init();

	printf("\n\nWIISP - emulador de PSP para Wii (paso 1: cargador)\n\n");

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
	for(i = 0; !path && i < sizeof(default_paths) / sizeof(default_paths[0]); i++)
		if(file_exists(default_paths[i])) path = default_paths[i];

	if(path) app_load(path, 12);
	else printf("No se encontro EBOOT.PBP. Copialo a sd:/wiisp/EBOOT.PBP\n");

	wait_for_exit();
	return 0;
}
