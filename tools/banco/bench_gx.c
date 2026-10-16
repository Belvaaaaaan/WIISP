/* Banco: tiras de 20 vértices como las de DBZ pasando por el renderizador de
   GX real (gx_ge.c compilado con devkitPPC) con libogc de mentira */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "core/opt.h"
void gx_ge_init(void);
void gx_ge_enable(int on);
static u32 f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }
static u32 rs = 1;
static u32 rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(int argc, char **argv){
	u8 *ram = calloc(1, PSP_RAM_SIZE_32MB), *vram = calloc(1, PSP_VRAM_SIZE), *scr = calloc(1, PSP_SCRATCH_SIZE);
	int i, k, reps = argc > 1 ? atoi(argv[1]) : 200;
	u32 vt = argc > 2 ? (u32)strtoul(argv[2], NULL, 16) : 0x11A;
	u8 *p;
	mem_init(ram, PSP_RAM_SIZE_32MB, vram, scr);
	opt_reset();
	for(i = 3; i < argc; i++) opt_parse(argv[i]);
	gx_ge_init();
	gx_ge_enable(1);
	ge.cmd[GE_VIEWPORTXSCALE] = f24(240); ge.cmd[GE_VIEWPORTYSCALE] = f24(-136); ge.cmd[GE_VIEWPORTZSCALE] = f24(-32000);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048); ge.cmd[GE_VIEWPORTYCENTER] = f24(2048); ge.cmd[GE_VIEWPORTZCENTER] = f24(32000);
	ge.cmd[GE_OFFSETX] = (2048 - 240) * 16; ge.cmd[GE_OFFSETY] = (2048 - 136) * 16;
	ge.cmd[GE_TEXSCALEU] = f24(1); ge.cmd[GE_TEXSCALEV] = f24(1);
	ge.cmd[GE_TEXTUREMAPENABLE] = 1; ge.cmd[GE_SHADEMODE] = 1;
	ge.cmd[GE_SCISSOR2] = 479 | (271 << 10); ge.cmd[GE_REGION2] = 479 | (271 << 10);
	ge.cmd[GE_FRAMEBUFPTR] = 0; ge.cmd[GE_FRAMEBUFWIDTH] = 512; ge.cmd[GE_FRAMEBUFPIXFORMAT] = 3;
	ge.cmd[GE_ZBUFPTR] = 0x110000; ge.cmd[GE_ZBUFWIDTH] = 512;
	ge.cmd[GE_ZTESTENABLE] = 1; ge.cmd[GE_ZTEST] = 6; ge.cmd[GE_MASKRGB] = 0; ge.cmd[GE_MASKALPHA] = 0;
	ge.cmd[GE_TEXADDR0] = 0x900000; ge.cmd[GE_TEXBUFWIDTH0] = 64 | (0x08 << 16); ge.cmd[GE_TEXSIZE0] = 6 | (6 << 8);
	ge.cmd[GE_TEXFORMAT] = 3; ge.cmd[GE_TEXFUNC] = 0; ge.cmd[GE_TEXFILTER] = 1 | (1 << 8);
	ge.cmd[GE_VERTEXTYPE] = vt;
	for(i = 0; i < 12; i++) ge.world[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	for(i = 0; i < 12; i++) ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	ge.view[11] = -5.0f;
	ge.proj[0] = 1.5f; ge.proj[5] = 2.0f; ge.proj[10] = -1.0f; ge.proj[11] = -1.0f; ge.proj[14] = -1.0f;
	for(k = 0; k < 8; k++) for(i = 0; i < 12; i++) ge.bone[k][i] = (i % 4 == 0) ? 1.0f : 0.01f * (float)i;
	p = mem_ptr(0x08900000, 0x10000);
	for(i = 0; i < 0x10000; i++) p[i] = (u8)(rnd() & 0x3F);
	for(i = 0; i < reps; i++){
		ge.vaddr = 0x08900000 + (u32)(i % 16) * 0x400;
		ge_draw_prim(GE_PRIM_TRIANGLE_STRIP, 20);
	}
	printf("%u vertices, %u triangulos, %u primitivas\n", ge_stats.vertices, ge_stats.tris, ge_stats.primitives);
	return 0;
}
