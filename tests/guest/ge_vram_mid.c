/**
 * WIISP - tests/guest/ge_vram_mid.c
 * Vértices leídos de la VRAM en medio de una llamada de dibujo: la primera
 * llamada dibuja arriba del framebuffer (8888 al inicio de la VRAM) y la
 * segunda lee sus vértices de las filas 240-250 del mismo framebuffer.
 * Cada vértice leído baja el búfer de la GPU a la VRAM a mitad de la
 * llamada, y el triángulo siguiente tiene que volver a marcarlo como
 * dibujado (en el renderizador de GX: tri_listo con ge_estado_sucio). Al
 * final la CPU lee un píxel dibujado, que solo baja el búfer si quedó
 * marcado. Con tools/banco/build_cli_gx.sh, la huella de GX con y sin
 * ge_estado_sucio tiene que ser la misma.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

typedef unsigned short u16;
typedef unsigned char u8;

int sceGeListEnQueue(const void *list, void *stall, int cbid, void *arg);
int sceGeDrawSync(int mode);
int sceDisplaySetFrameBuf(void *top, int width, int fmt, int sync);
int sceDisplayWaitVblankStart(void);
int sceKernelDcacheWritebackAll(void);

#define FRAMES 30
#define NTRI   12
/* Vértice en 2D: color 8888 y posición de 16 bits (12 bytes) */
#define VSIZE  12

static u8 top_v[3 * VSIZE] __attribute__((aligned(16)));
static u32 list[512] __attribute__((aligned(16)));
static u32 *lp;

static void cmd(u32 c, u32 arg){ *lp++ = (c << 24) | (arg & 0xFFFFFF); }

static void vertex(u8 *v, u32 color, int x, int y){
	v[0] = (u8)color; v[1] = (u8)(color >> 8); v[2] = (u8)(color >> 16); v[3] = (u8)(color >> 24);
	v[4] = (u8)x; v[5] = (u8)(x >> 8); v[6] = (u8)y; v[7] = (u8)(y >> 8); v[8] = 0; v[9] = 0;
	v[10] = v[11] = 0;
}

/* Los vértices de la segunda llamada, en la VRAM: fila 240 del framebuffer
   (stride 512, 4 bytes por píxel) */
static u8 *vram_verts(void){ return (u8 *)(0x44000000u + 240u * 512u * 4u); }

static void build(void){
	u8 *vv = vram_verts();
	int t;
	u32 vt = (7 << 2) | (2 << 7) | (1 << 23);
	vertex(top_v, 0xFF2040C0u, 10, 10);
	vertex(top_v + VSIZE, 0xFF2040C0u, 200, 10);
	vertex(top_v + 2 * VSIZE, 0xFF2040C0u, 10, 120);
	for(t = 0; t < NTRI; t++){
		int x = 20 + t * 30;
		vertex(vv + (t * 3) * VSIZE, 0xFF00FF00u + (u32)t, x, 130);
		vertex(vv + (t * 3 + 1) * VSIZE, 0xFF00FF00u + (u32)t, x + 25, 130);
		vertex(vv + (t * 3 + 2) * VSIZE, 0xFF00FF00u + (u32)t, x, 200);
	}
	lp = list;
	cmd(0x9C, 0); cmd(0x9D, 512); cmd(0xD2, 3);
	cmd(0xD4, 0); cmd(0xD5, 479 | (271 << 10));
	cmd(0x15, 0); cmd(0x16, 479 | (271 << 10));
	cmd(0x23, 0); cmd(0x1E, 0); cmd(0x1D, 0);
	cmd(0x12, vt);
	cmd(0x10, (((u32)top_v) >> 8) & 0x0F0000);
	cmd(0x01, (u32)top_v);
	cmd(0x04, (3 << 16) | 3);
	cmd(0x10, (((u32)vv) >> 8) & 0x0F0000);
	cmd(0x01, (u32)vv);
	cmd(0x04, (3 << 16) | (NTRI * 3));
	cmd(0x0F, 0);
	cmd(0x0C, 0);
}

int main(void){
	int f;
	u32 sum = 0;
	sceDisplaySetFrameBuf((void *)0x44000000u, 512, 3, 1);
	for(f = 0; f < FRAMES; f++){
		volatile u32 *px = (volatile u32 *)(0x44000000u + (150u * 512u + 30u + (u32)f) * 4u);
		build();
		sceKernelDcacheWritebackAll();
		sceGeListEnQueue(list, 0, -1, 0);
		sceGeDrawSync(0);
		sum = sum * 31u + *px;
		sceDisplayWaitVblankStart();
	}
	printf_("ge_vram_mid: %d cuadros, %08x\n", f, sum);
	return 0;
}
