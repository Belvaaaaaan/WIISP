/**
 * WIISP - tests/test_censo.c
 * El censo de estado (prof_estado): cada comando que cambia de valor tiene
 * que marcar los grupos del estado que lo leen. Llamadas de dibujo con
 * comandos al azar entre medias (por el mismo camino que una lista); un
 * grupo que sale distinto sin estar marcado es un comando que falta en la
 * tabla de ge.c. Con fallos metidos a propósito, se tienen que ver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "core/opt.h"
#include "test.h"

static u32 rs = 20261016u;
static u32 rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f; }
static u32 f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }

static void n_begin(void){}
static void n_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ (void)a; (void)b; (void)c; }
static void n_two(const GeVertex *a, const GeVertex *b){ (void)a; (void)b; }
static void n_point(const GeVertex *a){ (void)a; }
static void n_flush(void){}
static const GeHwRenderer null_hw = { n_begin, n_tri, n_two, n_two, n_two, n_point, n_flush };

#define VBASE 0x08600000u

static void cmd(u32 c, u32 v){ ge_test_execute((c << 24) | (v & 0xFFFFFF)); }

/* Un tipo de vértice sin índices (las direcciones no vienen al caso) */
static u32 random_vtype(void){
	u32 tc = rnd() % 4, col = rnd() % 8, nrm = rnd() % 4, pos = 1 + rnd() % 3, wt = rnd() % 3 ? 0 : 1 + rnd() % 3;
	u32 nw = rnd() % 8, nm = rnd() % 6 ? 0 : rnd() % 3, thr = rnd() % 6 == 0;
	return tc | (col << 2) | (nrm << 5) | (pos << 7) | (wt << 9) | (nw << 14) | (nm << 18) | (thr << 23);
}

static void draw(void){
	u8 *p = mem_ptr(VBASE, 0x4000);
	u32 i;
	for(i = 0; i < 0x4000 && p; i++) p[i] = (u8)rnd();
	ge.vaddr = VBASE;
	ge_draw_prim(GE_PRIM_TRIANGLE_STRIP, 4 + rnd() % 8);
}

/* Estado de partida al azar, con valores de juego en lo que decide el camino */
static void random_state(void){
	int i;
	for(i = 0; i < 256; i++) ge.cmd[i] = rnd() & 0xFFFFFF;
	ge.cmd[GE_VIEWPORTXSCALE] = f24(240.0f); ge.cmd[GE_VIEWPORTYSCALE] = f24(-136.0f);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048.0f); ge.cmd[GE_VIEWPORTYCENTER] = f24(2048.0f);
	ge.cmd[GE_OFFSETX] = (2048 - 240) * 16; ge.cmd[GE_OFFSETY] = (2048 - 136) * 16;
	ge.cmd[GE_SCISSOR1] = 0; ge.cmd[GE_SCISSOR2] = 479 | (271 << 10);
	ge.cmd[GE_REGION1] = 0; ge.cmd[GE_REGION2] = 479 | (271 << 10);
	ge.cmd[GE_LIGHTINGENABLE] = rnd() % 2;
	ge.cmd[GE_TEXTUREMAPENABLE] = rnd() % 4 != 0;
	ge.cmd[GE_CLEARMODE] = rnd() % 8 == 0;
	ge.cmd[GE_TEXMAPMODE] = rnd() % 4;
	ge.cmd[GE_VERTEXTYPE] = random_vtype();
	for(i = 0; i < 12; i++){ ge.world[i] = frnd(-2.0f, 2.0f); ge.view[i] = frnd(-2.0f, 2.0f); ge.tgen[i] = frnd(-2.0f, 2.0f); }
	for(i = 0; i < 16; i++) ge.proj[i] = frnd(-2.0f, 2.0f);
	ge_sucio = GE_SUCIO_TODO;
}

/* Un cambio entre dos llamadas: un comando (a veces con el valor que ya
   tenía), una matriz o un dato de hueso; con focus, sobre todo ese comando */
static int focus = -1;

static const u8 avoid[] = { GE_PRIM, GE_BEZIER, GE_SPLINE, GE_BOUNDINGBOX, GE_JUMP, GE_BJUMP, GE_CALL, GE_RET,
	                           GE_END, GE_SIGNAL, GE_FINISH, GE_TRANSFERSTART, GE_VAP };

static int avoided(u32 c){
	unsigned i;
	for(i = 0; i < sizeof(avoid); i++) if(avoid[i] == c) return 1;
	return c >= GE_VSCX && c <= GE_VSCV;
}

static void random_change(void){
	u32 c, k = rnd() % 16;
	if(k == 0){
		u32 which = rnd() % 4, numc[4] = { GE_WORLDMATRIXNUMBER, GE_VIEWMATRIXNUMBER, GE_PROJMATRIXNUMBER, GE_TGENMATRIXNUMBER };
		cmd(numc[which], rnd() % 12);
		cmd(numc[which] + 1, rnd() % 2 ? f24(frnd(-2.0f, 2.0f)) : 0);
		return;
	}
	if(k == 1){
		cmd(GE_BONEMATRIXNUMBER, rnd() % 96);
		cmd(GE_BONEMATRIXDATA, f24(frnd(-2.0f, 2.0f)));
		return;
	}
	if(k == 2){ cmd(GE_VERTEXTYPE, random_vtype()); return; }
	if(focus >= 0 && k < 10){ cmd((u32)focus, rnd() % 2 ? ge.cmd[focus] ^ 1u : rnd()); return; }
	do c = rnd() & 0xFF; while(avoided(c));
	switch(rnd() % 4){
	case 0: cmd(c, ge.cmd[c]); break;              /* el mismo valor */
	case 1: cmd(c, ge.cmd[c] ^ (1u << (rnd() % 24))); break;
	case 2: cmd(c, ge.cmd[c] ^ 1u); break;
	default: cmd(c, rnd()); break;
	}
}

/* Devuelve los errores (V, T y R) de rondas llamadas con cambios al azar */
static void run(int rounds, u32 *ev, u32 *et, u32 *er, u32 *lim, u32 *dist){
	int n, k;
	memset(&ge_stats, 0, sizeof(ge_stats));
	for(n = 0; n < rounds; n++){
		random_state();
		draw();
		for(k = 0; k < 6; k++){
			int j, m = rnd() % 4;
			for(j = 0; j < m; j++) random_change();
			/* Un tipo de vértice que dibuje (el azar puede dejar uno sin posición) */
			if(!((ge.cmd[GE_VERTEXTYPE] >> 7) & 3) || ((ge.cmd[GE_VERTEXTYPE] >> 11) & 3)) cmd(GE_VERTEXTYPE, random_vtype());
			draw();
		}
	}
	*ev = ge_stats.cs_v_error; *et = ge_stats.cs_t_error; *er = ge_stats.cs_r_error;
	*lim = ge_stats.cs_v_limpio + ge_stats.cs_t_limpio + ge_stats.cs_r_limpio;
	*dist = (ge_stats.cs_vt - ge_stats.cs_v_igual) + (ge_stats.cs_vt - ge_stats.cs_t_igual) + (ge_stats.cs_r - ge_stats.cs_r_igual);
}

void test_censo(void){
	const GeHwRenderer *saved_hw = ge_hw;
	int saved_fast = ge_fast_math;
	u8 saved_opt[OPT_N];
	u32 ev, et, er, lim, dist, old;
	static const struct { u32 cmd; const char *grupo; } mut[] = {
		{ GE_TEXFUNC, "R" }, { GE_ZTEST, "R" }, { GE_LIGHTENABLE0, "T" }, { GE_FOG1, "T" },
		{ GE_MORPHWEIGHT0, "V" }, { GE_TEXSCALEU, "V" }
	};
	unsigned i;

	printf("Censo de estado: grupos marcados por cada comando\n");
	memcpy(saved_opt, opt_on, sizeof(saved_opt));
	ge_hw = &null_hw;
	ge_fast_math = 1;
	opt_on[OPT_PROF_ESTADO] = 1;
	ge_vertex_test_reset();
	run(400, &ev, &et, &er, &lim, &dist);
	printf("  %u comparaciones limpias, %u distintas; errores V %u, T %u, R %u\n", (unsigned)lim, (unsigned)dist,
	       (unsigned)ev, (unsigned)et, (unsigned)er);
	CHECK_EQ(ev, 0u);
	CHECK_EQ(et, 0u);
	CHECK_EQ(er, 0u);
	CHECK(lim > 1000);
	CHECK(dist > 1000);
	/* Cada comando, uno a uno */
	{
		u32 tv = 0, tt = 0, tr = 0, c;
		for(c = 0; c < 256; c++){
			if(avoided(c)) continue;
			focus = (int)c;
			run(12, &ev, &et, &er, &lim, &dist);
			if(ev + et + er) printf("  comando 0x%02X: errores V %u, T %u, R %u\n", (unsigned)c, (unsigned)ev, (unsigned)et, (unsigned)er);
			tv += ev; tt += et; tr += er;
		}
		focus = -1;
		printf("  cada comando por separado: errores V %u, T %u, R %u\n", (unsigned)tv, (unsigned)tt, (unsigned)tr);
		CHECK_EQ(tv + tt + tr, 0u);
	}
	/* Con un comando de menos en la tabla, el censo lo tiene que ver */
	for(i = 0; i < sizeof(mut) / sizeof(mut[0]); i++){
		old = ge_test_census_group(mut[i].cmd, 0);
		focus = (int)mut[i].cmd;
		run(300, &ev, &et, &er, &lim, &dist);
		focus = -1;
		ge_test_census_group(mut[i].cmd, old);
		printf("  sin el comando 0x%02X en %s: errores V %u, T %u, R %u\n", (unsigned)mut[i].cmd, mut[i].grupo,
		       (unsigned)ev, (unsigned)et, (unsigned)er);
		CHECK((mut[i].grupo[0] == 'V' ? ev : mut[i].grupo[0] == 'T' ? et : er) > 0);
	}
	memcpy(opt_on, saved_opt, sizeof(saved_opt));
	ge_fast_math = saved_fast;
	ge_hw = saved_hw;
	ge_vertex_test_reset();
}
