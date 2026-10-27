/**
 * WIISP - tests/test_gxtiles.c
 * La escritura de texturas RGBA8 de GX por franjas (wii/gx_tiles.h) tiene
 * que dejar los mismos bytes que la escritura texel a texel, y la
 * decodificación CLUT16/CLUT32 por franjas lo mismo que de una vez.
 *
 * Texturas con paleta desde la copia del EFB (ge_rtt_gpu): los índices CI8
 * sacados de la copia RGBA8 de un búfer 565 y la TLUT 565, expandida como
 * la expande GX, tienen que dar el mismo color que bajar el búfer a la VRAM
 * y decodificarlo como CLUT16 (lo de antes); y la TLUT de un CLUT32 que lee
 * el alfa, el mismo color que decodificar el 8888 con ese alfa.
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

/* Lo que hace GX con una entrada RGB565 de la TLUT: 8 bits repitiendo los
   altos, alfa 255 (en el orden de la PSP, rojo en el byte bajo) */
static u32 gx_expand565(u16 e){
	u32 r = e >> 11, g = (e >> 5) & 63, b = e & 31;
	return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}

static void setup_clut_texture(int fmt, int w, int h, u32 cf){
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_TEXTUREMAPENABLE] = 1;
	ge.cmd[GE_TEXADDR0] = TEX_ADDR & 0xFFFFF0;
	ge.cmd[GE_TEXBUFWIDTH0] = ((TEX_ADDR >> 8) & 0x0F0000) | (u32)w;
	ge.cmd[GE_TEXSIZE0] = (u32)(__builtin_ctz((unsigned)w) | (__builtin_ctz((unsigned)h) << 8));
	ge.cmd[GE_TEXFORMAT] = (u32)fmt;
	ge.cmd[GE_CLUTFORMAT] = cf;
	ge_sucio = GE_SUCIO_TODO;   /* ge.cmd escrito a mano, no por una lista */
	ge_raster_begin();
}

static void test_clut_gpu(void){
	static u8 tile[64 * 32 * 4] __attribute__((aligned(32)));
	static u8 ci[64 * 32] __attribute__((aligned(32)));
	static u32 whole[64 * 32];
	static u16 tlut[256];
	int caso;
	printf("Texturas con paleta desde la copia del EFB (CI8 + TLUT)\n");
	/* CLUT16 sobre un búfer 565: índices desde la copia RGBA8 */
	for(caso = 0; caso < 200; caso++){
		int tw = 64, th = 32, sw = 4 * (1 + (int)(rnd() % 16)), h = 4 * (1 + (int)(rnd() % 8)), x, y, i, bad = 0;
		/* Paleta 565 (bits 0-1 = 0); desplazamiento, máscara y offset al azar */
		u32 cf = ((rnd() % 32) << 2) | ((rnd() & 0xFF) << 8) | ((rnd() % 32) << 16), shift, mask, offset;
		u8 *vram = mem_ptr(TEX_ADDR, (u32)tw * (u32)th * 2);
		const u32 *pal;
		if(caso < 20) cf = (5u << 2) | (0xFFu << 8);   /* el de DBZ */
		for(i = 0; i < (int)sizeof(tile); i++) tile[i] = (u8)rnd();
		for(i = 0; i < (int)sizeof(ge.clut); i++) ge.clut[i] = (u8)rnd();
		/* Lo de antes: la bajada convierte la copia a 565 en la VRAM */
		memset(vram, 0, (size_t)tw * (size_t)th * 2);
		for(y = 0; y < h; y++)
			for(x = 0; x < sw; x++){
				const u8 *t = gx_tile_ptr(tile, sw, x, y);
				u32 v = (u32)(t[1] >> 3) | ((u32)(t[32] >> 2) << 5) | ((u32)(t[33] >> 3) << 11);
				vram[(y * tw + x) * 2] = (u8)v;
				vram[(y * tw + x) * 2 + 1] = (u8)(v >> 8);
			}
		setup_clut_texture(GE_TFMT_CLUT16, tw, th, cf);
		ge_texture_decode(0, whole, tw, th, tw);
		/* Lo nuevo */
		ge_clut16_32_prepare();
		pal = ge_clut16_32_info(&shift, &mask, &offset);
		memset(ci, 0xEE, sizeof(ci));
		gx_ci8_from_rgba8_565(ci, tw, tile, sw, h, shift, mask);
		for(i = 0; i < 256; i++) tlut[i] = gx_tlut565(pal[(u32)i | offset]);
		for(y = 0; y < h; y++)
			for(x = 0; x < tw; x++){
				u8 idx = ci[(((y >> 2) * (tw >> 3) + (x >> 3)) << 5) + ((y & 3) << 3) + (x & 7)];
				if(x >= sw){ bad += idx != 0; continue; }
				bad += gx_expand565(tlut[idx]) != whole[y * tw + x];
			}
		CHECK_EQ(bad, 0);
	}
	/* CLUT32 que lee el alfa (desplazamiento >= 24): TLUT por valor de alfa */
	for(caso = 0; caso < 200; caso++){
		int tw = 16, th = 16, x, y, i, bad = 0;
		u32 cf = ((24 + rnd() % 8) << 2) | ((rnd() & 0xFF) << 8) | ((rnd() % 32) << 16), shift, mask, offset;
		u8 *vram = mem_ptr(TEX_ADDR, (u32)tw * (u32)th * 4);
		const u32 *pal;
		if(caso < 20) cf = (24u << 2) | (0x7Fu << 8);   /* el de DBZ */
		for(i = 0; i < tw * th * 4; i++) vram[i] = (u8)rnd();
		for(i = 0; i < (int)sizeof(ge.clut); i++) ge.clut[i] = (u8)rnd();
		setup_clut_texture(GE_TFMT_CLUT32, tw, th, cf);
		ge_texture_decode(0, whole, tw, th, tw);
		ge_clut16_32_prepare();
		pal = ge_clut16_32_info(&shift, &mask, &offset);
		for(i = 0; i < 256; i++) tlut[i] = gx_tlut565(pal[((((u32)i << 24) >> shift) & mask) | offset]);
		for(y = 0; y < th; y++)
			for(x = 0; x < tw; x++)
				bad += gx_expand565(tlut[vram[(y * tw + x) * 4 + 3]]) != whole[y * tw + x];
		CHECK_EQ(bad, 0);
	}
}

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
		ge_sucio = GE_SUCIO_TODO;   /* ge.cmd escrito a mano, no por una lista */
		ge_raster_begin();
		ge_texture_decode(0, whole, w, h, w);
		ge_clut16_32_prepare();
		for(y = 0; y < h; y += 4){
			ge_clut16_32_rows(tex + (u32)y * pitch, pitch, w, 4, band, w);
			memcpy(bands + y * w, band, (size_t)w * 4 * 4);
		}
		CHECK(!memcmp(whole, bands, sizeof(whole)));
	}
	test_clut_gpu();
	{   //Arreglo
		static u8 tile[64 * 32 * 4] __attribute__((aligned(32)));
		static u8 c1[128 * 32] __attribute__((aligned(32))), c2[128 * 32] __attribute__((aligned(32)));
		static const unsigned mascaras[] = { 0xFF, 0x01, 0x03, 0x7F, 0x80, 0x1F };
		unsigned shift, mi, malos = 0, casos = 0;
		printf("Indices CI8 por palabras (ge_ci8_rapido) contra los de siempre\n");
		for(shift = 0; shift < 32; shift++)
			for(mi = 0; mi < 7; mi++){
				int sw = 4 * (1 + (int)(rnd() % 16)), h = 4 * (1 + (int)(rnd() % 8)), dw = 8 * (((sw + 7) >> 3) + (int)(rnd() % 3)), i;
				unsigned mask = mi < 6 ? mascaras[mi] : (rnd() & 0xFF);
				for(i = 0; i < (int)sizeof(tile); i++) tile[i] = (u8)rnd();
				memset(c1, 0xEE, sizeof(c1));
				memset(c2, 0xEE, sizeof(c2));
				gx_ci8_from_rgba8_565(c1, dw, tile, sw, h, shift, mask);
				gx_ci8_from_rgba8_565_palabras(c2, dw, tile, sw, h, shift, mask);
				malos += memcmp(c1, c2, sizeof(c1)) != 0;
				casos++;
			}
		printf("  %u casos (32 desplazamientos x 7 mascaras, anchos y destinos al azar): %u distintos\n", casos, malos);
		CHECK_EQ(malos, 0u);
	}
}
