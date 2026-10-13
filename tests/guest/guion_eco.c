/**
 * WIISP - tests/guest/guion_eco.c
 * Escena de prueba para los guiones de botones (frontend/guion.h): lee el
 * mando en cada vblank e imprime cada cambio con su número de cuadro, y la
 * hora de la PSP en un cuadro fijo. Con guion_eco.guion, la salida dice si
 * cada línea del guion llega al juego en el cuadro que toca y si la hora
 * sale de los ciclos emulados (siempre la misma) y no del reloj del
 * anfitrión. No termina sola: la para la línea "fin" del guion.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

int sceCtrlReadBufferPositive(void *buf, int n);
int sceCtrlSetSamplingMode(int mode);
int sceDisplayWaitVblankStart(void);
int sceRtcGetCurrentTick(void *tick);

typedef struct {
	u32 timestamp, buttons;
	unsigned char lx, ly, rsrv[6];
} Pad;

static Pad pad;
static u32 mezcla[256];   /* para que la RAM cambie según lo pulsado */

int main(void){
	u32 cuadro, botones = 0xFFFFFFFFu, lx = 0, ly = 0, h = 1;
	sceCtrlSetSamplingMode(1);
	for(cuadro = 1;; cuadro++){
		sceDisplayWaitVblankStart();
		sceCtrlReadBufferPositive(&pad, 1);
		if(pad.buttons != botones || pad.lx != lx || pad.ly != ly){
			botones = pad.buttons;
			lx = pad.lx;
			ly = pad.ly;
			printf_("cuadro %d: botones %08x stick %d %d\n", cuadro, botones, lx, ly);
		}
		h = h * 1103515245u + 12345u + botones;
		mezcla[cuadro & 255] ^= h;
		if(cuadro == 100){
			u32 tick[2];
			sceRtcGetCurrentTick(tick);
			printf_("cuadro 100: hora %08x%08x\n", tick[1], tick[0]);
		}
	}
	return 0;
}
