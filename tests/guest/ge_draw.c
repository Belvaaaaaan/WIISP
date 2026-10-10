/**
 * WIISP - tests/guest/ge_draw.c
 * Un dibujo 3D típico para medir el GE: 8 mallas de 32x32 vértices con
 * índices, luz y textura con paleta (dos texturas que se alternan) y un
 * borrado con sprite, durante 120 cuadros. Cada cuadro la CPU cambia una
 * fila de una textura y vacía la caché de datos (como un juego).
 *
 * No hay coma flotante en tiempo de ejecución (sin libgcc): los float del
 * GE van como constantes en hexadecimal.
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
int sceKernelDcacheWritebackRange(const void *p, u32 size);
int sceKernelDcacheWritebackAll(void);

#define GRID   32
#define NVERT  (GRID * GRID)
#define NIDX   ((GRID - 1) * (GRID - 1) * 6)
#define VSIZE  14    /* uv 16 bits, normal 8 bits, posición 16 bits */
#define FRAMES 120

static u8 verts[NVERT * VSIZE] __attribute__((aligned(16)));
static u16 idx[NIDX] __attribute__((aligned(16)));
static u8 tex[2][64 * 64] __attribute__((aligned(16)));
static u32 clut[256] __attribute__((aligned(64)));
static u32 clear_v[6] __attribute__((aligned(16)));
static u32 list[4096] __attribute__((aligned(16)));
static u32 *lp;

static void cmd(u32 c, u32 arg){ *lp++ = (c << 24) | (arg & 0xFFFFFF); }
static void cmdf(u32 c, u32 bits){ cmd(c, bits >> 8); }

/* Matriz de mundo 4x3: escala s y traslación (x, y, z) */
static void world(u32 s, u32 x, u32 y, u32 z){
	cmd(0x3A, 0);
	cmdf(0x3B, s); cmdf(0x3B, 0); cmdf(0x3B, 0);
	cmdf(0x3B, 0); cmdf(0x3B, s); cmdf(0x3B, 0);
	cmdf(0x3B, 0); cmdf(0x3B, 0); cmdf(0x3B, s);
	cmdf(0x3B, x); cmdf(0x3B, y); cmdf(0x3B, z);
}

static void texture(const u8 *t){
	u32 a = (u32)t;
	cmd(0xA0, a);
	cmd(0xA8, ((a >> 8) & 0x0F0000) | 64);
	cmd(0xB8, (6 << 8) | 6);
	cmd(0xCB, 0);
}

static void build_mesh(void){
	int i, j, n = 0;
	for(j = 0; j < GRID; j++)
		for(i = 0; i < GRID; i++){
			u8 *v = verts + (j * GRID + i) * VSIZE;
			u32 u = (u32)i * 1024, w = (u32)j * 1024;
			int x = (i - GRID / 2) * 2048, y = (j - GRID / 2) * 2048;
			v[0] = (u8)u; v[1] = (u8)(u >> 8); v[2] = (u8)w; v[3] = (u8)(w >> 8);
			v[4] = 0; v[5] = 0; v[6] = 127;                        /* normal (0, 0, 1) */
			v[8] = (u8)x; v[9] = (u8)(x >> 8);
			v[10] = (u8)y; v[11] = (u8)(y >> 8);
			v[12] = 0; v[13] = 0;
		}
	for(j = 0; j < GRID - 1; j++)
		for(i = 0; i < GRID - 1; i++){
			u16 a = (u16)(j * GRID + i), b = (u16)(a + 1), c = (u16)(a + GRID), d = (u16)(c + 1);
			idx[n++] = a; idx[n++] = b; idx[n++] = c;
			idx[n++] = b; idx[n++] = d; idx[n++] = c;
		}
}

static void build_textures(void){
	int i;
	for(i = 0; i < 64 * 64; i++){
		tex[0][i] = (u8)(((i >> 3) ^ (i >> 9)) & 1 ? 200 : 40);
		tex[1][i] = (u8)(i * 7);
	}
	for(i = 0; i < 256; i++) clut[i] = 0xFF000000u | ((u32)i << 16) | ((u32)(255 - i) << 8) | (u32)(i / 2);
}

static void build_list(void){
	static const u32 xs[4] = { 0xC02CCCCDu, 0xBF666666u, 0x3F666666u, 0x402CCCCDu };   /* -2.7 -0.9 0.9 2.7 */
	static const u32 ys[2] = { 0xBF666666u, 0x3F666666u };
	u32 va = (u32)verts, ia = (u32)idx, ca = (u32)clut, cv = (u32)clear_v;
	int k;
	lp = list;
	/* Destino: framebuffer 8888 en el inicio de la VRAM y profundidad detrás */
	cmd(0x9C, 0); cmd(0x9D, 512); cmd(0xD2, 3);
	cmd(0x9E, 0x88000); cmd(0x9F, 512);
	cmdf(0x42, 0x43700000u); cmdf(0x43, 0xC3080000u); cmdf(0x44, 0xC6FFFF00u);   /* 240 -136 -32767.5 */
	cmdf(0x45, 0x45000000u); cmdf(0x46, 0x45000000u); cmdf(0x47, 0x46FFFF00u);   /* 2048 2048 32767.5 */
	cmd(0x4C, (2048 - 240) << 4); cmd(0x4D, (2048 - 136) << 4);
	cmd(0xD4, 0); cmd(0xD5, 479 | (271 << 10));
	cmd(0x15, 0); cmd(0x16, 479 | (271 << 10));
	cmd(0xD6, 0); cmd(0xD7, 65535);

	/* Borrar con un sprite en 2D */
	clear_v[0] = 0xFF302010u; clear_v[1] = 0; clear_v[2] = 0;
	clear_v[3] = 0xFF302010u; clear_v[4] = 480 | (272 << 16); clear_v[5] = 0;
	cmd(0xD3, 1 | (7 << 8));
	cmd(0x10, (cv >> 8) & 0x0F0000);
	cmd(0x12, (7 << 2) | (2 << 7) | (1 << 23));
	cmd(0x01, cv);
	cmd(0x04, (6 << 16) | 2);
	cmd(0xD3, 0);

	/* Cámara */
	cmd(0x3E, 0);
	cmdf(0x3F, 0x3F7B22D1u); cmdf(0x3F, 0); cmdf(0x3F, 0); cmdf(0x3F, 0);
	cmdf(0x3F, 0); cmdf(0x3F, 0x3FDDB22Du); cmdf(0x3F, 0); cmdf(0x3F, 0);
	cmdf(0x3F, 0); cmdf(0x3F, 0); cmdf(0x3F, 0xBF814952u); cmdf(0x3F, 0xBF800000u);
	cmdf(0x3F, 0); cmdf(0x3F, 0); cmdf(0x3F, 0xBF80A4D3u); cmdf(0x3F, 0);
	cmd(0x3C, 0);
	cmdf(0x3D, 0x3F800000u); cmdf(0x3D, 0); cmdf(0x3D, 0);
	cmdf(0x3D, 0); cmdf(0x3D, 0x3F800000u); cmdf(0x3D, 0);
	cmdf(0x3D, 0); cmdf(0x3D, 0); cmdf(0x3D, 0x3F800000u);
	cmdf(0x3D, 0); cmdf(0x3D, 0); cmdf(0x3D, 0);

	/* Luz direccional y material */
	cmd(0x17, 1); cmd(0x18, 1);
	cmd(0x5F, 0);
	cmdf(0x63, 0xBF13B646u); cmdf(0x64, 0xBF13B646u); cmdf(0x65, 0x3F800000u);
	cmd(0x8F, 0x202020); cmd(0x90, 0xFFFFFF); cmd(0x91, 0);
	cmd(0x53, 0); cmd(0x55, 0x404040); cmd(0x56, 0xFFFFFF); cmd(0x5C, 0x202020); cmd(0x5D, 0xFF);

	/* Textura con paleta de 256 colores */
	cmd(0xB0, ca); cmd(0xB1, (ca >> 8) & 0x0F0000);
	cmd(0xC5, 3 | (0xFF << 8)); cmd(0xC4, 32);
	cmd(0xC2, 0); cmd(0xC3, 5); cmd(0xC6, 0); cmd(0xC7, 0); cmd(0xC9, 0);
	cmdf(0x48, 0x3F800000u); cmdf(0x49, 0x3F800000u); cmdf(0x4A, 0); cmdf(0x4B, 0);
	cmd(0x1E, 1);

	cmd(0x23, 1); cmd(0xDE, 7);
	cmd(0x1D, 1); cmd(0x9B, 0);
	cmd(0x10, (va >> 8) & 0x0F0000);
	cmd(0x12, 2 | (1 << 5) | (2 << 7) | (2 << 11));
	for(k = 0; k < 8; k++){
		texture(tex[k & 1]);
		world(0x3F4CCCCDu, xs[k & 3], ys[k >> 2], 0xC0800000u);
		cmd(0x01, va);
		cmd(0x02, ia);
		cmd(0x04, (3 << 16) | NIDX);
	}
	cmd(0x0F, 0);
	cmd(0x0C, 0);
}

int main(void){
	int f;
	build_mesh();
	build_textures();
	build_list();
	sceKernelDcacheWritebackAll();
	sceDisplaySetFrameBuf((void *)0x44000000u, 512, 3, 1);
	for(f = 0; f < FRAMES; f++){
		/* Una fila nueva de la primera textura, como haría un juego */
		u8 *row = tex[0] + (f & 63) * 64;
		int i;
		for(i = 0; i < 64; i++) row[i] = (u8)(row[i] + 17);
		sceKernelDcacheWritebackRange(row, 64);
		sceGeListEnQueue(list, 0, -1, 0);
		sceGeDrawSync(0);
		sceDisplayWaitVblankStart();
	}
	printf_("ge_draw: %d cuadros\n", f);
	return 0;
}
