/**
 * WIISP - tests/test_efectos.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "core/memory.h"
#include "core/opt.h"
#include "hle/hle.h"
#include "test.h"

static long tam_archivo(const char *ruta){
	FILE *f = fopen(ruta, "rb");
	long n;
	if(!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

static int pixel_bmp(const char *ruta, int w, int h, int x, int y){
	FILE *f = fopen(ruta, "rb");
	u8 p[3];
	const long paso = ((long)w * 3 + 3) & ~3L;
	int r = -1;
	if(!f) return -1;
	if(!fseek(f, 54 + (long)(h - 1 - y) * paso + (long)x * 3, SEEK_SET) && fread(p, 1, 3, f) == 3) r = p[0];
	fclose(f);
	return r;
}

void test_efectos(void){
	static u32 cmd0[256];
	static u8 clut0[2048], vram0[256];
	static const char *const sufijos[4] = { "fuente", "alfa", "indices", "paleta" };
	const u8 guardada = opt_on[OPT_PROF_EFECTOS];
	EfectosDatos d;
	u32 i, x, y, archivos0;
	char ruta[64];
	printf("prof_efectos: censo de stencil y de texturas leidas de la VRAM\n");
	memcpy(cmd0, ge.cmd, sizeof(cmd0));
	memcpy(clut0, ge.clut, sizeof(clut0));
	memcpy(vram0, psp_mem.vram, sizeof(vram0));
	hle_set_log_screen(0);
	ge_efectos_informe();
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_FRAMEBUFPTR] = 0x088000;
	ge.cmd[GE_FRAMEBUFPIXFORMAT] = GE_FMT_8888;
	ge.cmd[GE_STENCILTESTENABLE] = 1;
	ge.cmd[GE_STENCILTEST] = CMP_ALWAYS | (0x40u << 8) | (0xFFu << 16);
	ge.cmd[GE_STENCILOP] = SOP_KEEP | (SOP_KEEP << 8) | ((u32)SOP_REPLACE << 16);
	ge.cmd[GE_MASKALPHA] = 0;
	for(i = 0; i < 3; i++) ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge.cmd[GE_STENCILOP] = (u32)SOP_ZERO << 16;
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge.cmd[GE_STENCILTEST] = CMP_EQUAL | (0x40u << 8) | (0xFFu << 16);
	ge.cmd[GE_STENCILOP] = (u32)SOP_INCR << 16;
	ge_efectos_dibujo(GE_PRIM_TRIANGLES, 3);
	ge_efectos_datos(&d);
	CHECK_EQ(d.dibujos, 5u);
	CHECK_EQ(d.n_escr, 3u);
	CHECK_EQ(d.escr_dibujos, 3u);
	CHECK_EQ(d.escr_vertices, 6u);
	CHECK_EQ(d.n_lect, 0u);
	for(y = 0; y < 8; y++)
		for(x = 0; x < 8; x++){
			const u32 a = y == 0 ? x + 1 : 0x80;
			wr_le32(psp_mem.vram + 4 * (y * 8 + x), (a << 24) | 0x102030u);
		}
	for(i = 0; i < 128; i++) wr_le32(ge.clut + 4 * i, i * 0x010101u);
	ge.cmd[GE_TEXTUREMAPENABLE] = 1;
	ge.cmd[GE_TEXADDR0] = 0;
	ge.cmd[GE_TEXBUFWIDTH0] = (0x04u << 16) | 8;
	ge.cmd[GE_TEXFORMAT] = GE_TFMT_CLUT32;
	ge.cmd[GE_TEXSIZE0] = 3 | (3 << 8);
	ge.cmd[GE_CLUTFORMAT] = GE_FMT_8888 | (24u << 2) | (0x7Fu << 8);
	archivos0 = d.archivos;
	ge_efectos_volcado("ef_prueba", 7);
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge_efectos_volcado(NULL, 0);
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge_efectos_datos(&d);
	CHECK_EQ(d.volcados, 1u);
	CHECK_EQ(d.archivos - archivos0, 4u);
	CHECK_EQ(d.vol_top_idx, 0u);
	CHECK_EQ(d.vol_top_n, 56u);
	CHECK_EQ(d.n_lect, 1u);
	CHECK_EQ(d.lect_dibujos, 3u);
	for(i = 0; i < 4; i++){
		snprintf(ruta, sizeof(ruta), "ef_prueba.ef7.1.%s.bmp", sufijos[i]);
		CHECK_EQ(tam_archivo(ruta), 54 + 8 * 24);
	}
	CHECK_EQ(pixel_bmp("ef_prueba.ef7.1.alfa.bmp", 8, 8, 2, 0), 3);
	CHECK_EQ(pixel_bmp("ef_prueba.ef7.1.alfa.bmp", 8, 8, 2, 5), 0x80);
	CHECK_EQ(pixel_bmp("ef_prueba.ef7.1.indices.bmp", 8, 8, 2, 0), 3 * 255 / 127);
	CHECK_EQ(pixel_bmp("ef_prueba.ef7.1.indices.bmp", 8, 8, 2, 5), 0);
	CHECK_EQ(pixel_bmp("ef_prueba.ef7.1.paleta.bmp", 8, 8, 2, 0), 3);
	CHECK_EQ(pixel_bmp("ef_prueba.ef7.1.fuente.bmp", 8, 8, 4, 4), 0x10);
	for(i = 0; i < 4; i++){
		snprintf(ruta, sizeof(ruta), "ef_prueba.ef7.1.%s.bmp", sufijos[i]);
		remove(ruta);
	}
	archivos0 = d.archivos;
	ge_efectos_volcado("ef_prueba", 20);
	ge_efectos_cuadro_fin(21);
	ge_efectos_cuadro_fin(22);
	ge_efectos_datos(&d);
	CHECK_EQ(d.volcados, 0u);
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge.cmd[GE_CLUTADDR] = 0x000780;
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge_efectos_cuadro_fin(23);
	ge_efectos_datos(&d);
	CHECK_EQ(d.volcados, 2u);
	CHECK_EQ(d.archivos - archivos0, 8u);
	CHECK_EQ(tam_archivo("ef_prueba.ef22.1.fuente.bmp"), 54 + 8 * 24);
	CHECK_EQ(tam_archivo("ef_prueba.ef22.2.paleta.bmp"), 54 + 8 * 24);
	CHECK_EQ(tam_archivo("ef_prueba.ef23.3.paleta.bmp"), -1);
	ge_efectos_cuadro_fin(20 + 1800);
	ge.cmd[GE_CLUTADDR] = 0x000800;
	ge_efectos_dibujo(GE_PRIM_RECTANGLES, 2);
	ge_efectos_datos(&d);
	CHECK_EQ(d.volcados, 2u);
	CHECK_EQ(d.archivos - archivos0, 8u);
	for(i = 0; i < 8; i++){
		snprintf(ruta, sizeof(ruta), "ef_prueba.ef22.%u.%s.bmp", (unsigned)(1 + i / 4), sufijos[i % 4]);
		remove(ruta);
	}
	for(i = 0; i < 600; i++){
		ge.cmd[GE_STENCILTEST] = CMP_ALWAYS | ((i & 0xFFu) << 8) | (0xFFu << 16);
		ge.cmd[GE_ZTEST] = i >> 8;
		ge_efectos_dibujo(GE_PRIM_TRIANGLES, 3);
	}
	ge_efectos_datos(&d);
	CHECK_EQ(d.n_escr, 512u - 1u);
	ge_efectos_informe();
	ge_efectos_datos(&d);
	CHECK(d.cuadra);
	CHECK_EQ(d.dibujos, 0u);
	ge.cmd[GE_STENCILTEST] = CMP_ALWAYS | (0xFFu << 16);
	ge.cmd[GE_ZTEST] = 0;
	ge_efectos_dibujo(GE_PRIM_TRIANGLES, 3);
	ge_efectos_datos(&d);
	CHECK_EQ(d.n_escr, 1u);
	CHECK_EQ(d.escr_dibujos, 1u);
	ge_efectos_informe();
	memcpy(ge.cmd, cmd0, sizeof(cmd0));
	memcpy(ge.clut, clut0, sizeof(clut0));
	memcpy(psp_mem.vram, vram0, sizeof(vram0));
	opt_on[OPT_PROF_EFECTOS] = guardada;
	hle_set_log_screen(1);
}
