/**
 * WIISP - tests/guest/guion_teclado.c
 * El teclado en pantalla dentro de un guion: el juego lo abre tres veces y
 * con guion_teclado.guion recibe, en orden, un nombre, una cancelación y un
 * texto con comillas y almohadilla. Imprime lo que recibió cada vez.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

int sceUtilityOskInitStart(void *params);
int sceUtilityOskShutdownStart(void);
int sceUtilityOskUpdate(int n);
int sceUtilityOskGetStatus(void);
int sceDisplayWaitVblankStart(void);

typedef unsigned short u16;

static u32 params[0x40 / 4];
static u32 campo[52 / 4];
static u16 desc[] = { 'N', 'o', 'm', 'b', 'r', 'e', 0 };
static u16 inicial[] = { 'G', 'o', 'k', 'u', 0 };
static u16 salida[32];

static void pedir(int vez){
	int i, st;
	char texto[32];
	for(i = 0; i < 0x40 / 4; i++) params[i] = 0;
	for(i = 0; i < 52 / 4; i++) campo[i] = 0;
	for(i = 0; i < 32; i++) salida[i] = 0xFFFF;
	params[0] = 0x40;                 /* tamaño */
	params[0x30 / 4] = 1;             /* un campo */
	params[0x34 / 4] = (u32)campo;
	campo[20 / 4] = 1;                /* una línea */
	campo[28 / 4] = (u32)desc;
	campo[32 / 4] = (u32)inicial;
	campo[40 / 4] = (u32)salida;
	campo[48 / 4] = 20;               /* hasta 19 caracteres */
	sceUtilityOskInitStart(params);
	do {
		sceDisplayWaitVblankStart();
		sceUtilityOskUpdate(1);
		st = sceUtilityOskGetStatus();
	} while(st != 3);
	sceUtilityOskShutdownStart();
	while(sceUtilityOskGetStatus() != 0) sceDisplayWaitVblankStart();
	for(i = 0; i < 31 && salida[i] && salida[i] != 0xFFFF; i++) texto[i] = (char)salida[i];
	texto[i] = 0;
	printf_("vez %d: resultado %d, texto \"%s\"\n", vez, (int)campo[44 / 4], texto);
}

int main(void){
	pedir(1);
	pedir(2);
	pedir(3);
	pedir(4);   /* el guion ya no tiene más: se acepta el inicial */
	sceKernelExitGame();
	return 0;
}
