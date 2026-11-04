/**
 * WIISP - tests/test_brightcensus.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "core/memory.h"
#include "core/opt.h"
#include "hle/hle.h"
#include "test.h"

static long bc_tam(const char *ruta){
	FILE *f = fopen(ruta, "rb");
	long n;
	if(!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

static u8 *bc_cargar(const char *ruta, long *n){
	FILE *f = fopen(ruta, "rb");
	u8 *d;
	*n = bc_tam(ruta);
	if(!f || *n <= 0){ if(f) fclose(f); return NULL; }
	d = malloc((size_t)*n);
	if(d && fread(d, 1, (size_t)*n, f) != (size_t)*n){ free(d); d = NULL; }
	fclose(f);
	return d;
}

void test_brightcensus(void){
	static u32 cmd0[256];
	static u8 vram0[0x2000];
	const u8 guardada = opt_on[OPT_TEST_BRIGHTNESSCENSUS];
	const GeHwRenderer *hw0 = ge_hw;
	GeVertex va, vb, vc;
	const GeVertex *tri[3] = { &va, &vb, &vc };
	u8 *d;
	long n, p;
	u32 i, x, y, regs = 0, cambios[8], campos0[8], campos1[8];
	float rango[10];
	printf("test_brightnesscensus: registro de cada dibujo con su miniatura\n");
	memcpy(cmd0, ge.cmd, sizeof(cmd0));
	memcpy(vram0, psp_mem.vram + 0x158000, sizeof(vram0));
	hle_set_log_screen(0);
	ge_hw = NULL;
	remove("bc_prueba.dibujos.bin");
	opt_on[OPT_TEST_BRIGHTNESSCENSUS] = 1;
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_FRAMEBUFPTR] = 0x158000;
	ge.cmd[GE_FRAMEBUFWIDTH] = 64;
	ge.cmd[GE_FRAMEBUFPIXFORMAT] = GE_FMT_4444;
	ge.cmd[GE_SCISSOR2] = 63 | (63u << 10);
	ge.cmd[GE_MASKRGB] = 0xFFFFEF;
	for(y = 0; y < 64; y++)
		for(x = 0; x < 64; x++) wr_le16(psp_mem.vram + 0x158000 + 2 * (y * 64 + x), (u16)(x < 16 ? 0xF00F : 0xF000));
	ge_brightcensus_fijar(10);
	ge_brightcensus_cuadro(9, "bc_prueba");
	CHECK_EQ(ge_bc_on, 0);
	CHECK_EQ(bc_tam("bc_prueba.dibujos.bin"), -1);
	ge_brightcensus_cuadro(10, "bc_prueba");
	CHECK_EQ(ge_bc_on, 1);
	memset(&va, 0, sizeof(va)); memset(&vb, 0, sizeof(vb)); memset(&vc, 0, sizeof(vc));
	va.s = -2558.0f; vb.s = 3170.0f; vc.s = 1.0f; va.t = 5.0f; vb.t = -7.0f; va.q = vb.q = vc.q = 1.0f;
	va.x = 0; va.y = 16; vb.x = 1024; vb.y = 32; vc.x = 64; vc.y = 1008;
	ge_brightcensus_prim(tri, 3);
	ge_brightcensus_dibujo_fin(GE_PRIM_TRIANGLES);
	for(y = 0; y < 16; y++) wr_le16(psp_mem.vram + 0x158000 + 2 * (y * 64 + 20), 0xF00F);
	ge_brightcensus_dibujo_fin(GE_PRIM_TRIANGLES);
	ge_brightcensus_cuadro(11, "bc_prueba");
	ge.cmd[GE_TEXTUREMAPENABLE] = 1;
	ge_brightcensus_dibujo_fin(GE_PRIM_RECTANGLES);
	ge_brightcensus_cuadro(13, "bc_prueba");
	ge_brightcensus_dibujo_fin(GE_PRIM_RECTANGLES);
	CHECK_EQ(ge_bc_on, 1);
	ge_brightcensus_cuadro(14, "bc_prueba");
	CHECK_EQ(ge_bc_on, 0);
	ge_brightcensus_dibujo_fin(GE_PRIM_RECTANGLES);
	d = bc_cargar("bc_prueba.dibujos.bin", &n);
	CHECK(d != NULL);
	if(d){
		CHECK(!memcmp(d, "WIISPBC1", 8));
		CHECK_EQ(rd_le32(d + 8), 1u);
		CHECK_EQ(rd_le32(d + 12), 16u);
		CHECK_EQ(rd_le32(d + 16), 32u);
		CHECK_EQ(rd_le32(d + 20), 0u);
		p = 24;
		while(p + 12 <= n && regs < 8){
			CHECK_EQ(rd_le32(d + p), 0x44494255u);
			CHECK_EQ(rd_le32(d + p + 4), 4u | (4u << 16));
			cambios[regs] = rd_le32(d + p + 8);
			campos0[regs] = rd_le32(d + p + 12);
			campos1[regs] = rd_le32(d + p + 16);
			if(regs == 0){
				for(i = 0; i < 10; i++){ u32 b = rd_le32(d + p + 12 + 128 + 4 * i); memcpy(&rango[i], &b, 4); }
				CHECK_EQ(rd_le32(d + p + 12 + 4 * 9), 0xFFFFEFu);
				CHECK_EQ(rd_le32(d + p + 12 + 4 * 31), 1u);
				CHECK(rango[0] == -2558.0f && rango[5] == 3170.0f && rango[6] == 5.0f && rango[1] == -7.0f);
				CHECK(rango[3] == 0.0f && rango[8] == 1024.0f && rango[9] == 1008.0f);
				CHECK_EQ(d[p + 180], 0); CHECK_EQ(d[p + 181], 0);
				CHECK_EQ(d[p + 182], 0xFF); CHECK_EQ(d[p + 183], 0); CHECK_EQ(d[p + 184], 0); CHECK_EQ(d[p + 185], 0xFF);
				CHECK_EQ(d[p + 188], 0); CHECK_EQ(d[p + 189], 0);
			}
			if(regs == 1){
				CHECK_EQ(rd_le32(d + p + 12 + 4 * 31), 0u);
				CHECK_EQ(d[p + 180], 1); CHECK_EQ(d[p + 181], 0);
				CHECK_EQ(d[p + 182], 0x10);
			}
			p += 12 + 128 + 40 + 6 * (long)cambios[regs];
			regs++;
		}
		CHECK_EQ(p, n);
		CHECK_EQ(regs, 4u);
		CHECK_EQ(cambios[0], 16u);
		CHECK_EQ(cambios[1], 1u);
		CHECK_EQ(cambios[2], 0u);
		CHECK_EQ(cambios[3], 0u);
		CHECK_EQ(campos0[0], 10u); CHECK_EQ(campos1[0], 0u);
		CHECK_EQ(campos0[1], 10u); CHECK_EQ(campos1[1], 1u);
		CHECK_EQ(campos0[2], 11u); CHECK_EQ(campos1[2], 0u);
		CHECK_EQ(campos0[3], 13u); CHECK_EQ(campos1[3], 0u);
		free(d);
	}
	remove("bc_prueba.dibujos.bin");
	opt_on[OPT_TEST_BRIGHTNESSCENSUS] = 0;
	ge_brightcensus_cuadro(10, "bc_prueba");
	CHECK_EQ(ge_bc_on, 0);
	CHECK_EQ(bc_tam("bc_prueba.dibujos.bin"), -1);
	ge_brightcensus_fijar(0);
	ge_brightcensus_cerrar();
	ge_hw = hw0;
	memcpy(ge.cmd, cmd0, sizeof(cmd0));
	memcpy(psp_mem.vram + 0x158000, vram0, sizeof(vram0));
	opt_on[OPT_TEST_BRIGHTNESSCENSUS] = guardada;
	hle_set_log_screen(1);
}
