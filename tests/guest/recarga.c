/**
 * WIISP - tests/guest/recarga.c
 * Cargar el mismo programa otra vez en el mismo proceso (--veces 2, como
 * en el autotest del Wii o al volver al menú) tiene que dar lo mismo que la
 * primera vez. Al empezar se mira lo que un programa anterior podía dejar:
 * un DMA en curso (el reloj vuelve a 0), el reloj de la CPU y del bus, y la
 * última uv, que usa un vértice sin uv con la textura puesta. Al terminar
 * se deja todo eso cambiado a propósito.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

typedef unsigned short u16;
typedef unsigned char u8;

int sceGeListEnQueue(const void *list, void *stall, int cbid, void *arg);
int sceGeDrawSync(int mode);
int sceDisplaySetFrameBuf(void *top, int width, int fmt, int sync);
int sceKernelDcacheWritebackAll(void);
int sceDmacMemcpy(void *dst, const void *src, u32 n);
int sceDmacTryMemcpy(void *dst, const void *src, u32 n);
int scePowerSetClockFrequency(int pll, int cpu, int bus);
int scePowerGetCpuClockFrequencyInt(void);
int scePowerGetBusClockFrequencyInt(void);

static u32 tex[16] __attribute__((aligned(16)));   /* 4x4, 8888 */
static u8 sin_uv[3 * 12] __attribute__((aligned(16)));   /* color, posición */
static u8 con_uv[3 * 16] __attribute__((aligned(16)));   /* uv, color, posición */
static u32 list[128] __attribute__((aligned(16)));
static u32 *lp;
static u8 dma_a[4096] __attribute__((aligned(64))), dma_b[4096] __attribute__((aligned(64)));

static void cmd(u32 c, u32 arg){ *lp++ = (c << 24) | (arg & 0xFFFFFF); }
static void put16(u8 *p, int v){ p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v){ put16(p, (int)(v & 0xFFFF)); put16(p + 2, (int)(v >> 16)); }

static void vertex(u8 *v, int uv, int x, int y){
	if(uv >= 0){ put16(v, uv); put16(v + 2, uv); v += 4; }
	put32(v, 0xFFFFFFFFu);
	put16(v + 4, x); put16(v + 6, y); put16(v + 8, 0);
}

/* Un triángulo texturizado en coordenadas de pantalla; con uv < 0, sin uv */
static void draw(int uv, int x){
	u8 *v = uv >= 0 ? con_uv : sin_uv;
	int size = uv >= 0 ? 16 : 12;
	u32 vt = (7 << 2) | (2 << 7) | (1 << 23) | (uv >= 0 ? 2 : 0);
	vertex(v, uv, x, 10);
	vertex(v + size, uv, x + 40, 10);
	vertex(v + 2 * size, uv, x, 50);
	lp = list;
	cmd(0x9C, 0); cmd(0x9D, 512); cmd(0xD2, 3);
	cmd(0xD4, 0); cmd(0xD5, 479 | (271 << 10));
	cmd(0x15, 0); cmd(0x16, 479 | (271 << 10));
	cmd(0x23, 0); cmd(0x1D, 0);
	cmd(0x1E, 1);                                       /* textura */
	cmd(0xA0, (u32)tex & 0xFFFFFF); cmd(0xA8, (((u32)tex >> 8) & 0x0F0000) | 4);
	cmd(0xB8, (2 << 8) | 2);                            /* 4x4 */
	cmd(0xC2, 0); cmd(0xC3, 3);                         /* 8888 */
	cmd(0xC6, 0); cmd(0xC7, 0);                         /* sin filtro, repetir */
	cmd(0xC9, 3 | (1 << 8));                            /* reemplazar */
	cmd(0xCB, 0);                                       /* TEXFLUSH */
	cmd(0x12, vt);
	cmd(0x10, (((u32)v) >> 8) & 0x0F0000);
	cmd(0x01, (u32)v);
	cmd(0x04, (3 << 16) | 3);
	cmd(0x0F, 0);
	cmd(0x0C, 0);
	sceKernelDcacheWritebackAll();
	sceGeListEnQueue(list, 0, -1, 0);
	sceGeDrawSync(0);
}

static u32 pixel(int x, int y){ return *(volatile u32 *)(0x44000000u + ((u32)y * 512u + (u32)x) * 4u); }

int main(void){
	int i;
	for(i = 0; i < 16; i++) tex[i] = 0xFF101010u + (u32)(i * 0x0F1123);
	sceDisplaySetFrameBuf((void *)0x44000000u, 512, 3, 1);

	printf_("recarga: DMA %08x, reloj %d/%d\n", sceDmacTryMemcpy(dma_b, dma_a, 64),
	        scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
	draw(-1, 20);
	printf_("recarga: sin uv %08x\n", pixel(25, 15));

	/* Lo que queda para la vez siguiente */
	draw(3, 100);
	printf_("recarga: con uv %08x\n", pixel(105, 15));
	scePowerSetClockFrequency(333, 333, 166);
	sceDmacMemcpy(dma_b, dma_a, sizeof(dma_a));
	return 0;
}
