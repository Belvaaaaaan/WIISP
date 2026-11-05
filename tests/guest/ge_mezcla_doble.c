/**
 * WIISP - tests/guest/ge_mezcla_doble.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include "guest.h"

typedef unsigned short u16;

int sceGeListEnQueue(const void *list, void *stall, int cbid, void *arg);
int sceGeDrawSync(int mode);
int sceKernelDcacheWritebackAll(void);

typedef struct { u32 color; u16 x, y, z, pad; } Vert;

static Vert borrar_v[2] __attribute__((aligned(16)));
static Vert v[24] __attribute__((aligned(16)));
static u32 list[256] __attribute__((aligned(16)));
static u32 *lp;

static void cmd(u32 c, u32 arg){ *lp++ = (c << 24) | (arg & 0xFFFFFF); }

static void poner(Vert *p, u32 color, int x, int y){ p->color = color; p->x = (u16)x; p->y = (u16)y; p->z = 0; p->pad = 0; }

static void build_list(void){
	static const u32 alfas[8] = { 0, 32, 64, 127, 128, 160, 200, 255 };
	u32 bv = (u32)borrar_v, va = (u32)v;
	int i;
	lp = list;
	cmd(0x9C, 0x000000); cmd(0x9D, 64); cmd(0xD2, 3);
	cmd(0x9E, 0x100000); cmd(0x9F, 64);
	cmd(0xD4, 0); cmd(0xD5, 63 | (63 << 10));
	cmd(0x15, 0); cmd(0x16, 63 | (63 << 10));
	cmd(0x4C, 0); cmd(0x4D, 0);
	poner(&borrar_v[0], 0xFF206090u, 0, 0);
	poner(&borrar_v[1], 0xFF206090u, 64, 64);
	cmd(0xD3, 1 | (7 << 8));
	cmd(0x10, (bv >> 8) & 0x0F0000);
	cmd(0x12, (7 << 2) | (2 << 7) | (1 << 23));
	cmd(0x01, bv);
	cmd(0x04, (6 << 16) | 2);
	cmd(0xD3, 0);
	cmd(0x17, 0); cmd(0x1E, 0); cmd(0x20, 0); cmd(0x22, 0); cmd(0x23, 0); cmd(0x24, 0);
	cmd(0x21, 1);
	cmd(0xDF, 6 | (7 << 4));
	cmd(0xE8, 0); cmd(0xE9, 0xFF);
	for(i = 0; i < 8; i++){
		poner(&v[2 * i], (alfas[i] << 24) | 0x3264C8u, i * 8, 0);
		poner(&v[2 * i + 1], (alfas[i] << 24) | 0x3264C8u, i * 8 + 8, 32);
	}
	poner(&v[16], 0x9050A0F0u, 0, 32); poner(&v[17], 0x9050A0F0u, 40, 64);
	poner(&v[18], 0x60F02040u, 24, 40); poner(&v[19], 0x60F02040u, 64, 64);
	cmd(0x10, (va >> 8) & 0x0F0000);
	cmd(0x12, (7 << 2) | (2 << 7) | (1 << 23));
	cmd(0x01, va);
	cmd(0x04, (6 << 16) | 16);
	cmd(0x04, (6 << 16) | 4);
	cmd(0x0F, 0);
	cmd(0x0C, 0);
}

int main(void){
	const volatile u32 *fb = (const volatile u32 *)0x44000000u;
	static const int puntos[6][2] = { { 4, 50 }, { 30, 36 }, { 30, 50 }, { 50, 50 }, { 50, 36 }, { 60, 60 } };
	int i;
	build_list();
	sceKernelDcacheWritebackAll();
	sceGeListEnQueue(list, 0, -1, 0);
	sceGeDrawSync(0);
	printf_("ge_mezcla_doble: fila 4:");
	for(i = 0; i < 8; i++) printf_(" %06X", (unsigned)(fb[4 * 64 + i * 8 + 4] & 0xFFFFFF));
	printf_("\n");
	printf_("ge_mezcla_doble: sprites que se pisan:");
	for(i = 0; i < 6; i++) printf_(" %06X", (unsigned)(fb[puntos[i][1] * 64 + puntos[i][0]] & 0xFFFFFF));
	printf_("\n");
	return 0;
}
