/**
 * WIISP - tests/test_gxtiles.c
 * La escritura de texturas RGBA8 de GX por franjas (wii/gx_tiles.h) tiene
 * que dejar los mismos bytes que la escritura texel a texel, y la
 * decodificación CLUT16/CLUT32 por franjas lo mismo que de una vez.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include <string.h>
#include "wii/gx_tiles.h"
#include "core/memory.h"
#include "gpu/ge_internal.h"
#include "test.h"

#define TEX_ADDR 0x08A00000u

static u32 rnd_state = 777;
static u32 rnd(void){ rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

void test_gxtiles(void){
	static u32 lin[512 * 64], band[4 * 512];
	static u8 a[512 * 64 * 4], b[512 * 64 * 4];
	int caso;
	printf("Texturas de GX por franjas\n");
	for(caso = 0; caso < 60; caso++){
		int w = 4 << (rnd() % 8), h = 4 << (rnd() % 5), x, y;
		if(w > 512) w = 512;
		if(h > 64) h = 64;
		for(x = 0; x < w * h; x++) lin[x] = rnd() ^ (rnd() << 16);
		memset(a, 0xAA, sizeof(a));
		memset(b, 0x55, sizeof(b));
		for(y = 0; y < h; y++)
			for(x = 0; x < w; x++) gx_put_rgba8(gx_tile_ptr(a, w, x, y), lin[y * w + x]);
		for(y = 0; y < h; y += 4) gx_tile_band_rgba8(b, w, y, lin + y * w, w);
		CHECK(!memcmp(a, b, (size_t)w * (size_t)h * 4));
	}
	/* CLUT16/CLUT32 por franjas (como en el Wii) contra de una vez */
	for(caso = 0; caso < 100; caso++){
		int fmt = caso & 1 ? GE_TFMT_CLUT32 : GE_TFMT_CLUT16, bpp = fmt == GE_TFMT_CLUT32 ? 4 : 2;
		int w = 64, h = 32, y, i;
		u32 pitch = (u32)w * (u32)bpp, cf = (rnd() % 4) | ((rnd() % 32) << 2) | ((rnd() & 0xFF) << 8) | ((rnd() % 32) << 16);
		u8 *tex = mem_ptr(TEX_ADDR, pitch * (u32)h);
		static u32 whole[64 * 32], bands[64 * 32];
		for(i = 0; i < (int)(pitch * (u32)h); i++) tex[i] = (u8)rnd();
		for(i = 0; i < (int)sizeof(ge.clut); i++) ge.clut[i] = (u8)rnd();
		memset(ge.cmd, 0, sizeof(ge.cmd));
		ge.cmd[GE_TEXTUREMAPENABLE] = 1;
		ge.cmd[GE_TEXADDR0] = TEX_ADDR & 0xFFFFF0;
		ge.cmd[GE_TEXBUFWIDTH0] = ((TEX_ADDR >> 8) & 0x0F0000) | (u32)w;
		ge.cmd[GE_TEXSIZE0] = 6 | (5 << 8);
		ge.cmd[GE_TEXFORMAT] = (u32)fmt;
		ge.cmd[GE_CLUTFORMAT] = cf;
		ge_raster_begin();
		ge_texture_decode(0, whole, w, h, w);
		ge_clut16_32_prepare();
		for(y = 0; y < h; y += 4){
			ge_clut16_32_rows(tex + (u32)y * pitch, pitch, w, 4, band, w);
			memcpy(bands + y * w, band, (size_t)w * 4 * 4);
		}
		CHECK(!memcmp(whole, bands, sizeof(whole)));
	}
}
