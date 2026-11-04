/**
 * WIISP - tests/test_rastreo_fb.c
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

#define RF_VB 0x08700000u
#define RF_IB 0x08710000u

static void rf_begin(void){}
static void rf_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ (void)a; (void)b; (void)c; }
static void rf_two(const GeVertex *a, const GeVertex *b){ (void)a; (void)b; }
static void rf_point(const GeVertex *a){ (void)a; }
static void rf_flush(void){}
static const GeHwRenderer rf_hw = { rf_begin, rf_tri, rf_two, rf_two, rf_two, rf_point, rf_flush, NULL };

static u32 rf_donde(u32 off, u32 len, char *texto, size_t n){
	(void)off; (void)len;
	if(texto && n) snprintf(texto, n, "prueba");
	return 1u | ((2u | (1u << 3)) << 4);
}

static void rf_estado_4444(void){
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_FRAMEBUFPTR] = 0x158000;
	ge.cmd[GE_FRAMEBUFWIDTH] = 64;
	ge.cmd[GE_FRAMEBUFPIXFORMAT] = GE_FMT_4444;
	ge.cmd[GE_SCISSOR2] = 63 | (63u << 10);
	ge.cmd[GE_MASKALPHA] = 0xFF;
	ge.cmd[GE_MASKRGB] = 0xFFFFEF;
}

static void rf_estado_lectura(u32 shift){
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_FRAMEBUFPTR] = 0x000000;
	ge.cmd[GE_FRAMEBUFWIDTH] = 512;
	ge.cmd[GE_FRAMEBUFPIXFORMAT] = GE_FMT_8888;
	ge.cmd[GE_SCISSOR2] = 479 | (271u << 10);
	ge.cmd[GE_TEXTUREMAPENABLE] = 1;
	ge.cmd[GE_TEXADDR0] = 0x158000;
	ge.cmd[GE_TEXBUFWIDTH0] = (0x04u << 16) | 64;
	ge.cmd[GE_TEXFORMAT] = GE_TFMT_CLUT16;
	ge.cmd[GE_TEXSIZE0] = 6 | (6u << 8);
	ge.cmd[GE_CLUTFORMAT] = GE_FMT_8888 | (shift << 2) | (0x01u << 8);
}

static int rf_cuenta(const char *ruta, const char *texto){
	FILE *f = fopen(ruta, "r");
	char l[1200];
	int n = 0;
	if(!f) return -1;
	while(fgets(l, sizeof(l), f)) if(strstr(l, texto)) n++;
	fclose(f);
	return n;
}

void test_rastreo_fb(void){
	static u32 cmd0[256];
	static u8 vram0[0x2000];
	const u8 guardada = opt_on[OPT_PROF_RASTREO_FB];
	const GeHwRenderer *hw0 = ge_hw;
	u32 (*donde0)(u32, u32, char *, size_t) = ge_rastreo_fb_donde;
	const u32 vaddr0 = ge.vaddr, iaddr0 = ge.iaddr;
	RfDatos d;
	u32 i, x, y;
	u8 *vb, *ib;
	printf("prof_rastreo_fb: escrituras y lecturas de un bufer vigilado\n");
	memcpy(cmd0, ge.cmd, sizeof(cmd0));
	memcpy(vram0, psp_mem.vram + 0x158000, sizeof(vram0));
	hle_set_log_screen(0);
	ge_hw = NULL;
	ge_rastreo_fb_donde = NULL;
	opt_on[OPT_PROF_RASTREO_FB] = 1;

	ge_rastreo_fb_fijar(0, 0);
	ge_rastreo_fb_reiniciar();
	rf_estado_4444();
	CHECK_EQ(ge_rastreo_fb_activo(), 0);
	CHECK_EQ(ge_rastreo_fb_interesa(), 0);
	ge_rastreo_fb_fijar(0x04158000u, 0);
	CHECK_EQ(ge_rastreo_fb_activo(), 1);
	CHECK_EQ(ge_rastreo_fb_largo(), 0x2000u);
	CHECK_EQ(ge_rastreo_fb_toca(0x158000, 1), 1);
	CHECK_EQ(ge_rastreo_fb_toca(0x159FFF, 1), 1);
	CHECK_EQ(ge_rastreo_fb_toca(0x15A000, 4), 0);
	CHECK_EQ(ge_rastreo_fb_toca(0x157FFC, 4), 0);
	CHECK_EQ(ge_rastreo_fb_toca(0x157FFC, 5), 1);
	CHECK_EQ(ge_rastreo_fb_interesa(), 1);
	ge.cmd[GE_FRAMEBUFPTR] = 0x150000;
	ge.cmd[GE_FRAMEBUFWIDTH] = 512;
	ge.cmd[GE_SCISSOR2] = 479 | (32u << 10);
	CHECK_EQ(ge_rastreo_fb_interesa(), 1);
	ge.cmd[GE_SCISSOR2] = 479 | (31u << 10);
	CHECK_EQ(ge_rastreo_fb_interesa(), 0);
	opt_on[OPT_PROF_RASTREO_FB] = 0;
	rf_estado_4444();
	CHECK_EQ(ge_rastreo_fb_interesa(), 0);
	opt_on[OPT_PROF_RASTREO_FB] = 1;

	ge_rastreo_fb_reiniciar();
	rf_estado_4444();
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0xFF00F000u, 0xFF00FFFFu, 1);
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 6, 1, 0xFF000F00u, 0xFFFF0F00u, 1);
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.n_escr, 1u);
	CHECK_EQ(d.escr_col_y, 0xFF000000u);
	CHECK_EQ(d.escr_col_o, 0xFFFFFFFFu);
	ge.cmd[GE_MASKRGB] = 0xFFFFDF;
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0xFF000000u, 0xFFFFFFFFu, 1);
	ge.cmd[GE_LOGICOPENABLE] = 1;
	ge.cmd[GE_LOGICOP] = 7;
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0xFF000000u, 0xFFFFFFFFu, 1);
	ge.cmd[GE_LOGICOP] = 6;
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0xFF000000u, 0xFFFFFFFFu, 1);
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0x80000001u, 0x80000003u, 1);
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.escrituras, 6u);
	CHECK_EQ(d.n_escr, 4u);
	CHECK_EQ(d.escr_col_y, 0xFF000000u);
	CHECK_EQ(d.escr_col_o, 0xFFFFFFFFu);
	CHECK_EQ(d.escr_mrgb, 0xFFFFEFu);
	CHECK_EQ(d.lecturas, 0u);

	vb = mem_ptr(RF_VB, 0x100);
	ib = mem_ptr(RF_IB, 0x10);
	CHECK(vb != NULL && ib != NULL);
	if(vb && ib){
		static const u32 col[4] = { 0xFF0000F0u, 0x8000FF10u, 0x01020304u, 0xFFFFFFFFu };
		ge_rastreo_fb_reiniciar();
		rf_estado_4444();
		ge.cmd[GE_VERTEXTYPE] = (7u << 2) | (2u << 7) | (1u << 23);
		for(i = 0; i < 4; i++){
			memset(vb + i * 12, 0, 12);
			wr_le32(vb + i * 12, col[i]);
			wr_le16(vb + i * 12 + 4, (u16)(i * 8));
			wr_le16(vb + i * 12 + 6, (u16)(i * 4));
		}
		ge.vaddr = RF_VB;
		ge_hw = &rf_hw;
		ge_draw_prim(GE_PRIM_TRIANGLES, 3);
		ge_rastreo_fb_datos(&d);
		CHECK_EQ(d.escrituras, 1u);
		CHECK_EQ(d.escr_col_y, col[0] & col[1] & col[2]);
		CHECK_EQ(d.escr_col_o, col[0] | col[1] | col[2]);
		ge_rastreo_fb_reiniciar();
		ib[0] = 3; ib[1] = 3; ib[2] = 1;
		ge.cmd[GE_VERTEXTYPE] = (7u << 2) | (2u << 7) | (1u << 11) | (1u << 23);
		ge.vaddr = RF_VB;
		ge.iaddr = RF_IB;
		ge_draw_prim(GE_PRIM_TRIANGLES, 3);
		ge_rastreo_fb_datos(&d);
		CHECK_EQ(d.escr_col_y, col[3] & col[1]);
		CHECK_EQ(d.escr_col_o, col[3] | col[1]);
		ge_rastreo_fb_reiniciar();
		ge.cmd[GE_VERTEXTYPE] = (2u << 7) | (1u << 23);
		ge.cmd[GE_MATERIALAMBIENT] = 0x123456;
		ge.cmd[GE_MATERIALALPHA] = 0x78;
		ge.vaddr = RF_VB;
		ge_draw_prim(GE_PRIM_TRIANGLES, 3);
		ge_rastreo_fb_datos(&d);
		CHECK_EQ(d.escr_col_y, 0x78123456u);
		CHECK_EQ(d.escr_col_o, 0x78123456u);
		ge_rastreo_fb_reiniciar();
		opt_on[OPT_PROF_RASTREO_FB] = 0;
		ge.vaddr = RF_VB;
		ge_draw_prim(GE_PRIM_TRIANGLES, 3);
		opt_on[OPT_PROF_RASTREO_FB] = 1;
		ge_rastreo_fb_datos(&d);
		CHECK_EQ(d.escrituras, 0u);
		ge_hw = NULL;
	}

	for(y = 0; y < 64; y++)
		for(x = 0; x < 64; x++){
			u32 v = 0xF000u;
			if(x < 16) v |= 1;
			if(y < 8) v |= 2;
			if(x == 0 && y == 0) v |= 8;
			wr_le16(psp_mem.vram + 0x158000 + 2 * (y * 64 + x), (u16)v);
		}
	ge_rastreo_fb_reiniciar();
	rf_estado_lectura(0);
	CHECK_EQ(ge_rastreo_fb_interesa(), 2);
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 2, 0, 0, 0);
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.lecturas, 1u);
	CHECK_EQ(d.n_lect, 1u);
	CHECK_EQ(d.lect_muestras, 1u);
	CHECK_EQ(d.lect_bit0, 16u * 64u);
	CHECK_EQ(d.lect_bit1, 8u * 64u);
	CHECK_EQ(d.lect_soft, 1u);

	ge_rastreo_fb_reiniciar();
	ge_hw = &rf_hw;
	ge_rastreo_fb_donde = rf_donde;
	rf_estado_lectura(1);
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 2, 0, 0, 0);
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.lect_muestras, 0u);
	CHECK_EQ(d.lect_soft, 0u);
	ge_rastreo_fb_gx(RG_BAJAR, 0, "prueba");
	ge_rastreo_fb_contenido("prueba");
	ge_rastreo_fb_camino(RF_DECODIFICADA);
	ge_rastreo_fb_contenido("prueba");
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.lect_muestras, 1u);
	CHECK_EQ(d.lect_bit1, 8u * 64u);
	CHECK_EQ(d.bajadas_con, 1u);
	CHECK_EQ(d.bajadas_sin, 0u);
	ge_rastreo_fb_gx(RG_BAJAR, 4, "prueba");
	rf_estado_4444();
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0, 0, 1);
	ge_rastreo_fb_gx(RG_BAJAR, 4, "prueba");
	ge_rastreo_fb_gx(RG_BAJAR_FILAS, -1, "prueba");
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.bajadas_con, 1u);
	CHECK_EQ(d.bajadas_sin, 2u);
	ge_rastreo_fb_transferencia(0x04158000u, 0x04000000u, 64, 64, 2);
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.transferencias, 1u);

	hle_set_log_file("rastreo_prueba.log");
	ge_rastreo_fb_reiniciar();
	ge_rastreo_fb_traza(10);
	ge_rastreo_fb_cuadro(9);
	rf_estado_4444();
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0, 0, 1);
	ge_rastreo_fb_cuadro(10);
	for(i = 0; i < 3; i++) ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0, 0, 1);
	ge.cmd[GE_MASKRGB] = 0xFFFFDF;
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0, 0, 1);
	ge_hw = NULL;
	rf_estado_lectura(0);
	for(i = 0; i < 6; i++) ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 2, 0, 0, 0);
	ge_rastreo_fb_cuadro(11);
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 2, 0, 0, 0);
	ge_rastreo_fb_cuadro(12);
	ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 2, 0, 0, 0);
	ge_rastreo_fb_informe();
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.escrituras, 0u);
	CHECK_EQ(d.n_escr, 0u);
	CHECK_EQ(d.n_lect, 0u);
	hle_set_log_file(NULL);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "c9 "), 0);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "mascara RGB FFFFEF alfa FF"), 2);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "(x3)"), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "camino: VRAM (software)"), 7);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c10 contenido (software, al leer): textura 04158000, 4096 texeles en el rango; texeles con cada bit (0 en adelante): 1024 512 0 1 0 0 0 0 0 0 0 0 4096 4096 4096 4096"), 6);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c10 mapa "), 4);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c11 mapa "), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c12 "), 0);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c10 m1 07 3333333333333333222222"), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c10 m1 00 B333333333333333222222"), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "] c10 m1 08 1111111111111111000000"), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "GX: ignora la mascara parcial"), 4);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "(cuadra: si)"), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "Rastreo, escritura 2 "), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "Rastreo, lectura 1 (8 dibujos"), 1);
	CHECK_EQ(rf_cuenta("rastreo_prueba.log", "software 8; contenido en 8 lecturas"), 1);
	remove("rastreo_prueba.log");
	ge_rastreo_fb_traza(0);

	ge_rastreo_fb_reiniciar();
	rf_estado_4444();
	for(i = 0; i < 70; i++){
		ge.cmd[GE_MASKRGB] = i;
		ge_rastreo_fb_dibujo(GE_PRIM_TRIANGLES, 3, 1, 0, 0, 1);
	}
	ge_rastreo_fb_datos(&d);
	CHECK_EQ(d.n_escr, 64u);
	ge_rastreo_fb_informe();
	ge_rastreo_fb_datos(&d);
	CHECK(d.cuadra);

	ge_rastreo_fb_fijar(0, 0);
	ge_rastreo_fb_reiniciar();
	ge_hw = hw0;
	ge_rastreo_fb_donde = donde0;
	ge.vaddr = vaddr0;
	ge.iaddr = iaddr0;
	memcpy(ge.cmd, cmd0, sizeof(cmd0));
	memcpy(psp_mem.vram + 0x158000, vram0, sizeof(vram0));
	opt_on[OPT_PROF_RASTREO_FB] = guardada;
	hle_set_log_screen(1);
}
