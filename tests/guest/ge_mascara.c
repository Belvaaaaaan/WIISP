/**
 * WIISP - tests/guest/ge_mascara.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include "guest.h"

typedef unsigned short u16;
typedef short s16;

int sceGeListEnQueue(const void *list, void *stall, int cbid, void *arg);
int sceGeDrawSync(int mode);
int sceKernelDcacheWritebackAll(void);

static u32 borrar_v[6] __attribute__((aligned(16)));
static s16 v[5][6] __attribute__((aligned(16)));
static u32 list[512] __attribute__((aligned(16)));
static u32 *lp;

static void cmd(u32 c, u32 arg){ *lp++ = (c << 24) | (arg & 0xFFFFFF); }

static void sprite(int k, int x0, int x1){
	u32 a = (u32)v[k];
	v[k][0] = (s16)x0; v[k][1] = 0; v[k][2] = 0;
	v[k][3] = (s16)x1; v[k][4] = 64; v[k][5] = 0;
	cmd(0x10, (a >> 8) & 0x0F0000);
	cmd(0x01, a);
	cmd(0x04, (6 << 16) | 2);
}

static void build_list(void){
	u32 bv = (u32)borrar_v;
	lp = list;
	cmd(0x9C, 0x158000); cmd(0x9D, 64); cmd(0xD2, 2);
	cmd(0x9E, 0x100000); cmd(0x9F, 64);
	cmd(0xD4, 0); cmd(0xD5, 63 | (63 << 10));
	cmd(0x15, 0); cmd(0x16, 63 | (63 << 10));
	cmd(0x4C, 0); cmd(0x4D, 0);
	borrar_v[0] = 0; borrar_v[1] = 0; borrar_v[2] = 0;
	borrar_v[3] = 0; borrar_v[4] = 64 | (64 << 16); borrar_v[5] = 0;
	cmd(0xD3, 1 | (3 << 8));
	cmd(0x10, (bv >> 8) & 0x0F0000);
	cmd(0x12, (7 << 2) | (2 << 7) | (1 << 23));
	cmd(0x01, bv);
	cmd(0x04, (6 << 16) | 2);
	cmd(0xD3, 0);
	cmd(0x17, 0); cmd(0x1E, 0); cmd(0x20, 0); cmd(0x21, 0); cmd(0x22, 0); cmd(0x23, 0); cmd(0x24, 0);
	cmd(0x12, (2 << 7) | (1 << 23));
	cmd(0xE9, 0xFF);
	cmd(0x58, 0xFF);
	cmd(0x55, 0xFFFFFF); cmd(0xE8, 0xFFFFEF); sprite(0, 0, 32);
	cmd(0x55, 0xFFFFFF); cmd(0xE8, 0xFFFFDF); sprite(1, 16, 48);
	cmd(0x55, 0x000000); cmd(0xE8, 0xFFFFEF); sprite(2, 0, 8);
	cmd(0x55, 0xFFFFFF); cmd(0xE8, 0xF0F0F0); sprite(3, 0, 64);
	cmd(0x55, 0x00FF00); cmd(0xE8, 0x0FFF0F); sprite(4, 40, 64);
	cmd(0x0F, 0);
	cmd(0x0C, 0);
}

int main(void){
	const volatile u16 *fb = (const volatile u16 *)0x44158000u;
	u32 bits[16], i, b;
	char fila[65];
	build_list();
	sceKernelDcacheWritebackAll();
	sceGeListEnQueue(list, 0, -1, 0);
	sceGeDrawSync(0);
	for(b = 0; b < 16; b++) bits[b] = 0;
	for(i = 0; i < 64 * 64; i++)
		for(b = 0; b < 16; b++) bits[b] += (fb[i] >> b) & 1;
	for(i = 0; i < 64; i++) fila[i] = "0123456789ABCDEF"[fb[i] & 15];
	fila[64] = 0;
	printf_("ge_mascara: fila 0, rojo: %s\n", fila);
	for(i = 0; i < 64; i++) fila[i] = "0123456789ABCDEF"[(fb[i] >> 4) & 15];
	printf_("ge_mascara: fila 0, verde: %s\n", fila);
	printf_("ge_mascara: texeles con cada bit:");
	for(b = 0; b < 16; b++) printf_(" %d", (int)bits[b]);
	printf_("\n");
	return 0;
}
