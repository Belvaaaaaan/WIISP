/**
 * WIISP - tests/test_texclut.c
 * El decodificador rápido de texturas CLUT16/CLUT32 (ge_tex_clut) tiene que
 * dar exactamente lo mismo que el genérico (ge_texture_decode) con cualquier
 * formato de paleta, desplazamiento, máscara y offset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include <string.h>
#include "core/memory.h"
#include "gpu/ge_internal.h"
#include "test.h"

#define TEX_ADDR 0x08A00000u

static u32 rnd_state = 12345;
static u32 rnd(void){ rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

void test_texclut(void){
	static u32 a[64 * 64], b[64 * 64];
	int caso, fallos_antes = failures;
	printf("Texturas CLUT16/CLUT32 rapidas contra el decodificador generico\n");
	for(caso = 0; caso < 400; caso++){
		int fmt = caso & 1 ? GE_TFMT_CLUT32 : GE_TFMT_CLUT16, bpp = fmt == GE_TFMT_CLUT32 ? 4 : 2;
		int ws = 3 + (int)(rnd() % 4), hs = 3 + (int)(rnd() % 4), w = 1 << ws, h = 1 << hs, i;
		u32 clutfmt = rnd() % 4, shift, mask, offset, pitch = (u32)w * (u32)bpp;
		u8 *tex = mem_ptr(TEX_ADDR, pitch * (u32)h);
		/* La mitad de los casos sin transformar el índice */
		if(caso % 4 < 2){ shift = 0; mask = 0xFF; offset = 0; }
		else { shift = rnd() % 32; mask = rnd() & 0xFF; offset = rnd() % 32; }
		for(i = 0; i < (int)(pitch * (u32)h); i++) tex[i] = (u8)rnd();
		for(i = 0; i < (int)sizeof(ge.clut); i++) ge.clut[i] = (u8)rnd();
		memset(ge.cmd, 0, sizeof(ge.cmd));
		ge.cmd[GE_TEXTUREMAPENABLE] = 1;
		ge.cmd[GE_TEXADDR0] = TEX_ADDR & 0xFFFFF0;
		ge.cmd[GE_TEXBUFWIDTH0] = ((TEX_ADDR >> 8) & 0x0F0000) | (u32)w;
		ge.cmd[GE_TEXSIZE0] = (u32)ws | ((u32)hs << 8);
		ge.cmd[GE_TEXFORMAT] = (u32)fmt;
		ge.cmd[GE_CLUTFORMAT] = clutfmt | (shift << 2) | (mask << 8) | (offset << 16);
		ge_sucio = GE_SUCIO_TODO;   /* ge.cmd escrito a mano, no por una lista */
		ge_raster_begin();
		CHECK_EQ(ge_raster_state()->texfmt, fmt);
		CHECK_EQ(ge_raster_state()->clut_fmt, clutfmt);
		ge_texture_decode(0, a, w, h, w);
		ge_texture_decode_clut16_32(tex, pitch, w, h, b, w);
		CHECK(!memcmp(a, b, (size_t)w * (size_t)h * 4));
		if(failures != fallos_antes){
			printf("  caso %d: %s %dx%d paleta %u shift %u mask %02X offset %u\n", caso,
			       fmt == GE_TFMT_CLUT32 ? "CLUT32" : "CLUT16", w, h, clutfmt, shift, mask, offset);
			break;
		}
	}
}
