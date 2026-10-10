/**
 * WIISP - tests/guest/ctrl_read.c
 * sceCtrlReadBufferPositive tiene que devolver cuántas muestras copió
 * también cuando espera al vblank (GTA tiraba los botones si devolvía 0).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

int sceCtrlReadBufferPositive(void *buf, int n);
int sceCtrlSetSamplingMode(int mode);
int sceDisplayWaitVblankStart(void);

static u32 pad[4 * 4];

int main(void){
	int i;
	sceCtrlSetSamplingMode(1);
	for(i = 0; i < 3; i++){
		int r = sceCtrlReadBufferPositive(pad, 1);   /* sin muestra nueva: espera */
		printf_("lectura seguida: %d\n", r);
	}
	sceDisplayWaitVblankStart();
	printf_("tras un vblank: %d\n", sceCtrlReadBufferPositive(pad, 4));
	sceDisplayWaitVblankStart();
	sceDisplayWaitVblankStart();
	printf_("tras dos vblanks, pidiendo 4: %d\n", sceCtrlReadBufferPositive(pad, 4));
	printf_("pidiendo 65: %08x\n", sceCtrlReadBufferPositive(pad, 65));
	return 0;
}
