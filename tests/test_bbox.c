/**
 * WIISP - tests/test_bbox.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo

#include <string.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "core/opt.h"
#include "test.h"

static u32 f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }

static void n_begin(void){}
static void n_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ (void)a; (void)b; (void)c; }
static void n_two(const GeVertex *a, const GeVertex *b){ (void)a; (void)b; }
static void n_point(const GeVertex *a){ (void)a; }
static void n_flush(void){}
static const GeHwRenderer null_hw = { n_begin, n_tri, n_two, n_two, n_two, n_point, n_flush };

#define LISTA 0x08A00000u
#define VERT  0x08900000u
#define VT_POS 0x180u
#define VT_HUESOS (0x180u | (3u << 9))

static u32 lp, vp;

static void cmd(u32 c, u32 a){ mem_write32(lp, (c << 24) | (a & 0xFFFFFF)); lp += 4; }

static void flt(float f){ u32 b; memcpy(&b, &f, 4); mem_write32(vp, b); vp += 4; }

static u32 caja(float x0, float x1){
	u32 a = vp;
	int i;
	for(i = 0; i < 8; i++){
		flt(i & 1 ? x1 : x0);
		flt(i & 2 ? 0.5f : -0.5f);
		flt(i & 4 ? 0.5f : -0.5f);
	}
	return a;
}

static u32 triangulo(float dx, int huesos){
	static const float p[3][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } };
	u32 a = vp;
	int i;
	for(i = 0; i < 3; i++){
		if(huesos) flt(1.0f);
		flt(p[i][0] + dx);
		flt(p[i][1]);
		flt(0.0f);
	}
	return a;
}

static void dibujo(u32 vt, u32 v){
	cmd(GE_VERTEXTYPE, vt);
	cmd(GE_BASE, (v >> 8) & 0x0F0000);
	cmd(GE_VADDR, v & 0xFFFFFF);
	cmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
}

static u32 objeto(u32 c){
	cmd(GE_VERTEXTYPE, VT_POS);
	cmd(GE_BASE, (c >> 8) & 0x0F0000);
	cmd(GE_VADDR, c & 0xFFFFFF);
	cmd(GE_BOUNDINGBOX, 8);
	cmd(GE_BASE, (LISTA >> 8) & 0x0F0000);
	lp += 4;
	return lp - 4;
}

static void fin_objeto(u32 salto){
	mem_write32(salto, (GE_BJUMP << 24) | (lp & 0xFFFFFF));
}

void test_bbox(void){
	const GeHwRenderer *saved_hw = ge_hw;
	int saved_fast = ge_fast_math;
	u8 saved_opt[OPT_N];
	u32 dentro, fuera, s;
	int i;

	printf("Censo de cajas (prof_bbox)\n");
	memcpy(saved_opt, opt_on, sizeof(saved_opt));
	ge_hw = &null_hw;
	ge_fast_math = 1;
	ge_vertex_reset();
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_VIEWPORTXSCALE] = f24(240.0f); ge.cmd[GE_VIEWPORTYSCALE] = f24(-136.0f); ge.cmd[GE_VIEWPORTZSCALE] = f24(-32000.0f);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048.0f); ge.cmd[GE_VIEWPORTYCENTER] = f24(2048.0f); ge.cmd[GE_VIEWPORTZCENTER] = f24(32000.0f);
	ge.cmd[GE_OFFSETX] = (2048 - 240) * 16; ge.cmd[GE_OFFSETY] = (2048 - 136) * 16;
	ge.cmd[GE_SCISSOR2] = 479 | (271 << 10); ge.cmd[GE_REGION2] = 479 | (271 << 10);
	ge.cmd[GE_MINZ] = 0; ge.cmd[GE_MAXZ] = 0xFFFF;
	memset(ge.world, 0, sizeof(ge.world)); memset(ge.view, 0, sizeof(ge.view)); memset(ge.proj, 0, sizeof(ge.proj));
	for(i = 0; i < 12; i++) ge.world[i] = ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	for(i = 0; i < 12; i++) ge.bone[0][i] = (i % 4 == 0) ? 1.0f : 0.0f;
	ge.view[11] = -5.0f;
	ge.proj[0] = 1.5f; ge.proj[5] = 2.0f; ge.proj[10] = -1.0f; ge.proj[11] = -1.0f; ge.proj[14] = -1.0f;
	ge_sucio = GE_SUCIO_TODO;
	ge.offset = 0;

	lp = LISTA;
	vp = VERT;
	dentro = caja(-0.5f, 0.5f);
	fuera = caja(20.0f, 21.0f);
	s = objeto(dentro);
	dibujo(VT_POS, triangulo(0.0f, 0));
	dibujo(VT_POS, triangulo(20.0f, 0));
	fin_objeto(s);
	s = objeto(fuera);
	dibujo(VT_POS, triangulo(20.0f, 0));
	dibujo(VT_HUESOS, triangulo(20.0f, 1));
	dibujo(VT_POS, triangulo(20.0f, 0));
	fin_objeto(s);
	s = objeto(dentro);
	dibujo(VT_POS, triangulo(20.0f, 0));
	dibujo(VT_POS, triangulo(30.0f, 0));
	fin_objeto(s);
	s = objeto(dentro);
	dibujo(VT_HUESOS, triangulo(20.0f, 1));
	fin_objeto(s);
	s = objeto(dentro);
	fin_objeto(s);
	dibujo(VT_POS, triangulo(0.0f, 0));
	dibujo(VT_POS, triangulo(20.0f, 0));
	dibujo(VT_HUESOS, triangulo(-20.0f, 1));
	cmd(GE_BOUNDINGBOX, 0);
	s = lp; lp += 4;
	dibujo(VT_POS, triangulo(0.0f, 0));
	fin_objeto(s);

	memset(&ge_stats, 0, sizeof(ge_stats));
	opt_on[OPT_PROF_BBOX] = 1;
	ge_test_run_list(LISTA, lp, NULL);
	cmd(GE_BOUNDINGBOX, 0);
	ge_test_run_list(lp - 4, lp, NULL);

	printf("  BBOX %u (visibles %u, 0 vertices %u), BJUMP %u (tomados %u: %u llamadas, %u vertices, %u con huesos)\n",
	       (unsigned)ge_stats.bb_cmds, (unsigned)ge_stats.bb_visibles, (unsigned)ge_stats.bb_count0, (unsigned)ge_stats.bj_total,
	       (unsigned)ge_stats.bj_tomados, (unsigned)ge_stats.bj_llamadas, (unsigned)ge_stats.bj_vertices, (unsigned)ge_stats.bj_huesos);
	printf("  objetos %u (vacios %u, %u llamadas), sin dibujar %u (fuera %u, %u llamadas, con huesos %u); "
	       "sin caja %u llamadas, sin dibujar %u (fuera %u, con huesos %u)\n",
	       (unsigned)ge_stats.ob_n, (unsigned)ge_stats.ob_vacios, (unsigned)ge_stats.ob_llamadas, (unsigned)ge_stats.ob_nada,
	       (unsigned)ge_stats.ob_fuera, (unsigned)ge_stats.ob_nada_llamadas, (unsigned)ge_stats.ob_nada_huesos,
	       (unsigned)ge_stats.sc_llamadas, (unsigned)ge_stats.sc_nada, (unsigned)ge_stats.sc_fuera,
	       (unsigned)ge_stats.sc_nada_huesos);
	CHECK_EQ(ge_stats.bb_cmds, 7u);
	CHECK_EQ(ge_stats.bb_visibles, 4u);
	CHECK_EQ(ge_stats.bb_count0, 2u);
	CHECK_EQ(ge_stats.bb_sin_probar, 0u);
	CHECK_EQ(ge_stats.bj_total, 6u);
	CHECK_EQ(ge_stats.bj_sin_caja, 0u);
	CHECK_EQ(ge_stats.bj_tomados, 2u);
	CHECK_EQ(ge_stats.bj_llamadas, 4u);
	CHECK_EQ(ge_stats.bj_vertices, 12u);
	CHECK_EQ(ge_stats.bj_huesos, 3u);
	CHECK_EQ(ge_stats.ob_n, 4u);
	CHECK_EQ(ge_stats.ob_vacios, 1u);
	CHECK_EQ(ge_stats.ob_llamadas, 5u);
	CHECK_EQ(ge_stats.ob_vertices, 15u);
	CHECK_EQ(ge_stats.ob_nada, 2u);
	CHECK_EQ(ge_stats.ob_fuera, 2u);
	CHECK_EQ(ge_stats.ob_nada_llamadas, 3u);
	CHECK_EQ(ge_stats.ob_nada_vertices, 9u);
	CHECK_EQ(ge_stats.ob_nada_huesos, 1u);
	CHECK_EQ(ge_stats.ob_nada_huesos_vertices, 3u);
	CHECK_EQ(ge_stats.sc_llamadas, 3u);
	CHECK_EQ(ge_stats.sc_vertices, 9u);
	CHECK_EQ(ge_stats.sc_nada, 2u);
	CHECK_EQ(ge_stats.sc_fuera, 2u);
	CHECK_EQ(ge_stats.sc_fuera_vertices, 6u);
	CHECK_EQ(ge_stats.sc_nada_huesos, 1u);
	CHECK_EQ(ge_stats.sc_nada_huesos_vertices, 3u);
	CHECK_EQ(ge_stats.tris, 8u);
	CHECK_EQ(ge_stats.tris_drawn, 2u);

	{
		u32 tris = ge_stats.tris, drawn = ge_stats.tris_drawn, prims = ge_stats.primitives;
		memset(&ge_stats, 0, sizeof(ge_stats));
		opt_on[OPT_PROF_BBOX] = 0;
		ge_test_run_list(LISTA, lp, NULL);
		CHECK_EQ(ge_stats.tris, tris);
		CHECK_EQ(ge_stats.tris_drawn, drawn);
		CHECK_EQ(ge_stats.primitives, prims);
		CHECK_EQ(ge_stats.ob_n + ge_stats.sc_llamadas + ge_stats.bb_cmds, 0u);
	}

	memset(&ge_stats, 0, sizeof(ge_stats));
	memcpy(opt_on, saved_opt, sizeof(saved_opt));
	ge_hw = saved_hw;
	ge_fast_math = saved_fast;
	ge_vertex_reset();
}
