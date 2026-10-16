/**
 * WIISP - tests/test_vdec.c
 * El camino de los vértices del GE con y sin un interruptor (ge_vdec_esp):
 * llamadas de dibujo al azar (formatos, índices, huesos, morph, through,
 * luces, niebla, generación de uv, descarte de caras, recorte de
 * profundidad) en tandas de tres, para que también cuente lo que cada
 * llamada deja a la siguiente (la última uv y la última normal). Todo lo
 * que llega al renderizador por hardware tiene que ser igual bit a bit, y
 * los contadores del GE también.
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

static u32 rs = 20261015u;
static u32 rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f; }
static u32 f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }

/* --- Renderizador que lo apunta todo ---------------------------------------- */

static u32 *cap;
static size_t cap_n, cap_max;

static void put32(u32 v){
	if(cap_n == cap_max){
		size_t n = cap_max ? cap_max * 2 : 1 << 16;
		u32 *p = realloc(cap, n * sizeof(u32));
		if(!p) return;
		cap = p;
		cap_max = n;
	}
	cap[cap_n++] = v;
}

static void putf(float f){ u32 u; memcpy(&u, &f, 4); put32(u); }

static void put_v(const GeVertex *v){
	putf(v->s); putf(v->t); putf(v->q); putf(v->clipw);
	put32(v->color0); put32(v->color1);
	put32((u32)v->x); put32((u32)v->y); put32(v->z);
	putf(v->fogdepth);
}

static void c_begin(void){ put32(0xB0u); }
static void c_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ put32(0x77u); put_v(a); put_v(b); put_v(c); }
static void c_rect(const GeVertex *a, const GeVertex *b){ put32(0x66u); put_v(a); put_v(b); }
static void c_clear(const GeVertex *a, const GeVertex *b){ put32(0x55u); put_v(a); put_v(b); }
static void c_line(const GeVertex *a, const GeVertex *b){ put32(0x44u); put_v(a); put_v(b); }
static void c_point(const GeVertex *a){ put32(0x33u); put_v(a); }
static void c_flush(void){}

static const GeHwRenderer cap_hw = { c_begin, c_tri, c_rect, c_clear, c_line, c_point, c_flush };

/* --- Estado y vértices al azar ------------------------------------------- */

#define VBASE 0x08900000u
#define IBASE 0x08A00000u

typedef struct {
	u32 cmd[256];
	float world[12], view[12], proj[16], tgen[12], bone[8][12];
	u32 vt[3], prim[3], count[3];
} Case;

static const u32 csz[4] = { 0, 1, 2, 4 };

static void put_comp(u8 *p, int fmt, float v, int sign){
	if(fmt == 3 && rnd() % 64 == 0){
		u32 x = rnd();   /* de vez en cuando bits cualquiera (NaN, infinitos) */
		memcpy(p, &x, 4);
		return;
	}
	switch(fmt){
	case 1: p[0] = (u8)(sign ? (s8)(int)(v * 127.0f) : (u8)(int)(v * 255.0f)); break;
	case 2: { u16 x = (u16)(sign ? (s16)(int)(v * 32767.0f) : (u16)(int)(v * 65535.0f)); p[0] = (u8)x; p[1] = (u8)(x >> 8); break; }
	case 3: { u32 x; memcpy(&x, &v, 4); p[0] = (u8)x; p[1] = (u8)(x >> 8); p[2] = (u8)(x >> 16); p[3] = (u8)(x >> 24); break; }
	default: break;
	}
}

static u32 random_vtype(void){
	u32 tc = rnd() % 4, col = rnd() % 8, nrm = rnd() % 4, pos = 1 + rnd() % 3, wt = rnd() % 3 ? 0 : 1 + rnd() % 3;
	u32 idx = rnd() % 3 ? 0 : 1 + rnd() % 2, nw = rnd() % 8, nm = rnd() % 10 ? 0 : rnd() % 3, thr = rnd() % 8 == 0;
	if(col && col < 4 && rnd() % 2) col = 7;
	return tc | (col << 2) | (nrm << 5) | (pos << 7) | (wt << 9) | (idx << 11) | (nw << 14) | (nm << 18) | (thr << 23);
}

/* Tamaño de un vértice como vformat_setup */
static u32 vsize(u32 vt, u32 *off_w, u32 *off_tc, u32 *off_col, u32 *off_nrm, u32 *off_pos, u32 *one){
	static const u8 tc_size[4] = { 0, 2, 4, 8 }, tc_align[4] = { 0, 1, 2, 4 };
	static const u8 col_size[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
	static const u8 nrm_size[4] = { 0, 3, 6, 12 }, nrm_align[4] = { 0, 1, 2, 4 };
	static const u8 pos_size[4] = { 3, 3, 6, 12 }, pos_align[4] = { 1, 1, 2, 4 };
	u32 tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
	u32 nw = ((vt >> 14) & 7) + 1, nm = ((vt >> 18) & 7) + 1, size = 0, big = 0;
#define AL(n, a) ((a) ? (((n) + (a) - 1) & ~((a) - 1)) : 0)
	*off_w = 0;
	if(wt){ size += csz[wt] * nw; if(csz[wt] > big) big = csz[wt]; }
	if(tc){ size = AL(size, tc_align[tc]); *off_tc = size; size += tc_size[tc]; if(tc_align[tc] > big) big = tc_align[tc]; }
	if(col){ size = AL(size, col_size[col]); *off_col = size; size += col_size[col]; if(col_size[col] > big) big = col_size[col]; }
	if(nrm){ size = AL(size, nrm_align[nrm]); *off_nrm = size; size += nrm_size[nrm]; if(nrm_align[nrm] > big) big = nrm_align[nrm]; }
	size = AL(size, pos_align[pos]); *off_pos = size; size += pos_size[pos]; if(pos_align[pos] > big) big = pos_align[pos];
	*one = AL(size, big);
#undef AL
	return *one * nm;
}

static void fill_vertices(u32 vt, u32 n, u32 addr){
	u32 ow = 0, ot = 0, oc = 0, on = 0, op = 0, one, total = vsize(vt, &ow, &ot, &oc, &on, &op, &one);
	u32 tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
	u32 nw = ((vt >> 14) & 7) + 1, nm = ((vt >> 18) & 7) + 1, i, k, m, thr = (vt >> 23) & 1;
	u8 *base = mem_ptr(addr, total * n);
	if(!base) return;
	for(i = 0; i < total * n; i++) base[i] = (u8)rnd();
	for(i = 0; i < n; i++)
		for(m = 0; m < nm; m++){
			u8 *p = base + i * total + m * one;
			for(k = 0; k < nw && wt; k++) put_comp(p + ow + k * csz[wt], (int)wt, frnd(0.0f, 1.0f), 0);
			for(k = 0; k < 2 && tc; k++) put_comp(p + ot + k * csz[tc], (int)tc, thr ? frnd(0.0f, 256.0f) : frnd(0.0f, 1.0f), 0);
			for(k = 0; k < 3 && nrm; k++) put_comp(p + on + k * csz[nrm], (int)nrm, frnd(-1.0f, 1.0f), 1);
			for(k = 0; k < 3; k++){
				float v = thr ? (k == 2 ? frnd(0.0f, 60000.0f) : frnd(-20.0f, 500.0f)) : frnd(-1.0f, 1.0f) * (pos == 3 ? 12.0f : 1.0f);
				put_comp(p + op + k * csz[pos == 0 ? 1 : pos], (int)pos, v, 1);
			}
			(void)col; (void)oc;
		}
}

static void random_case(Case *c){
	int i, k;
	memset(c, 0, sizeof(*c));
	for(i = 0; i < 256; i++) c->cmd[i] = rnd() & 0xFFFFFF;
	/* Lo que decide el camino, con valores de juego */
	c->cmd[GE_VIEWPORTXSCALE] = f24(240.0f);
	c->cmd[GE_VIEWPORTYSCALE] = f24(-136.0f);
	c->cmd[GE_VIEWPORTZSCALE] = f24(frnd(-32000.0f, 32000.0f));
	c->cmd[GE_VIEWPORTXCENTER] = f24(2048.0f);
	c->cmd[GE_VIEWPORTYCENTER] = f24(2048.0f);
	c->cmd[GE_VIEWPORTZCENTER] = f24(frnd(0.0f, 65535.0f));
	c->cmd[GE_OFFSETX] = (2048 - 240) * 16;
	c->cmd[GE_OFFSETY] = (2048 - 136) * 16;
	c->cmd[GE_TEXSCALEU] = f24(frnd(-2.0f, 2.0f));
	c->cmd[GE_TEXSCALEV] = f24(frnd(-2.0f, 2.0f));
	c->cmd[GE_TEXOFFSETU] = f24(frnd(-1.0f, 1.0f));
	c->cmd[GE_TEXOFFSETV] = f24(frnd(-1.0f, 1.0f));
	c->cmd[GE_TEXMAPMODE] = (rnd() % 4) | ((rnd() % 4) << 8);
	c->cmd[GE_TEXSHADELS] = (rnd() % 4) | ((rnd() % 4) << 8);
	c->cmd[GE_LIGHTINGENABLE] = rnd() % 4 == 0;
	for(i = 0; i < 4; i++) c->cmd[GE_LIGHTENABLE0 + i] = rnd() % 2;
	for(i = GE_LX0; i < GE_LAC0; i++) c->cmd[i] = f24(frnd(-10.0f, 10.0f));
	for(i = 0; i < 4; i++) c->cmd[GE_LIGHTTYPE0 + i] = (rnd() % 3) | ((rnd() % 3) << 8);
	c->cmd[GE_FOGENABLE] = rnd() % 3 == 0;
	c->cmd[GE_FOG1] = f24(frnd(0.0f, 50.0f));
	c->cmd[GE_FOG2] = f24(frnd(-1.0f, 1.0f));
	c->cmd[GE_CULLFACEENABLE] = rnd() % 2;
	c->cmd[GE_CULL] = rnd() % 2;
	c->cmd[GE_DEPTHCLAMPENABLE] = rnd() % 2;
	c->cmd[GE_REVERSENORMAL] = rnd() % 4 == 0;
	c->cmd[GE_SHADEMODE] = rnd() % 2;
	c->cmd[GE_CLEARMODE] = rnd() % 16 == 0;
	c->cmd[GE_TEXTUREMAPENABLE] = rnd() % 2;
	c->cmd[GE_ANTIALIASENABLE] = 0;
	c->cmd[GE_SCISSOR1] = 0;
	c->cmd[GE_SCISSOR2] = 479 | (271 << 10);
	c->cmd[GE_REGION1] = 0;
	c->cmd[GE_REGION2] = 479 | (271 << 10);
	c->cmd[GE_BASE] = 0;
	for(i = 0; i < 8; i++) c->cmd[GE_MORPHWEIGHT0 + i] = f24(frnd(-0.5f, 1.0f));
	/* Matrices: un objeto delante de la cámara, con partes detrás y fuera */
	for(i = 0; i < 12; i++) c->world[i] = i < 9 ? frnd(-1.5f, 1.5f) : frnd(-6.0f, 6.0f);
	for(i = 0; i < 12; i++) c->view[i] = i < 9 ? (i % 4 == 0 ? 1.0f : 0.0f) + frnd(-0.3f, 0.3f) : (i == 11 ? frnd(-30.0f, 2.0f) : frnd(-3.0f, 3.0f));
	memset(c->proj, 0, sizeof(c->proj));
	c->proj[0] = frnd(1.0f, 2.5f); c->proj[5] = frnd(1.5f, 3.0f);
	c->proj[10] = -1.0f - frnd(0.0f, 0.1f); c->proj[11] = -1.0f; c->proj[14] = -frnd(0.5f, 2.0f);
	if(rnd() % 8 == 0) for(i = 0; i < 16; i++) c->proj[i] += frnd(-0.2f, 0.2f);
	for(i = 0; i < 12; i++) c->tgen[i] = frnd(-2.0f, 2.0f);
	for(k = 0; k < 8; k++) for(i = 0; i < 12; i++) c->bone[k][i] = i < 9 ? frnd(-1.2f, 1.2f) : frnd(-5.0f, 5.0f);
	for(k = 0; k < 3; k++){
		c->vt[k] = random_vtype();
		c->prim[k] = rnd() % 16 == 0 ? GE_PRIM_CONTINUE : rnd() % 4 ? GE_PRIM_TRIANGLE_STRIP + (rnd() % 2) * (rnd() % 2 ? -1 : 1) : rnd() % 7;
		c->count[k] = 1 + rnd() % 48;
	}
}

/* Los vértices y los índices de la tanda, en la RAM */
static void write_case_data(const Case *c){
	int k;
	for(k = 0; k < 3; k++){
		u32 vt = c->vt[k], n = c->count[k], i, nv = 1 + rnd() % 40;
		u32 idx = (vt >> 11) & 3;
		if(idx){
			u8 *ip = mem_ptr(IBASE + (u32)k * 0x10000u, n * 4);
			int repeat = rnd() % 2;
			for(i = 0; i < n && ip; i++){
				u32 v = repeat ? rnd() % nv : (i + rnd() % 3) % nv;
				if(idx == 1) ip[i] = (u8)v;
				else { ip[2 * i] = (u8)v; ip[2 * i + 1] = (u8)(v >> 8); }
			}
			fill_vertices(vt, nv, VBASE + (u32)k * 0x40000u);
		} else fill_vertices(vt, n, VBASE + (u32)k * 0x40000u);
	}
}

static void run_case(const Case *c, int opt, GeStats *st){
	int k;
	ge_vertex_test_reset();
	memcpy(ge.cmd, c->cmd, sizeof(ge.cmd));
	memcpy(ge.world, c->world, sizeof(ge.world));
	memcpy(ge.view, c->view, sizeof(ge.view));
	memcpy(ge.proj, c->proj, sizeof(ge.proj));
	memcpy(ge.tgen, c->tgen, sizeof(ge.tgen));
	memcpy(ge.bone, c->bone, sizeof(ge.bone));
	opt_on[OPT_GE_VDEC_ESP] = (u8)opt;
	opt_on[OPT_GE_TRI_RAPIDO] = (u8)opt;
	memset(&ge_stats, 0, sizeof(ge_stats));
	cap_n = 0;
	for(k = 0; k < 3; k++){
		ge.cmd[GE_VERTEXTYPE] = c->vt[k];
		ge.vaddr = VBASE + (u32)k * 0x40000u;
		ge.iaddr = IBASE + (u32)k * 0x10000u;
		put32(0xD0u + (u32)k);
		ge_draw_prim(c->prim[k], c->count[k]);
	}
	*st = ge_stats;
}

void test_vdec(void){
	const GeHwRenderer *saved_hw = ge_hw;
	int saved_fast = ge_fast_math, saved_opt = opt_on[OPT_GE_VDEC_ESP], saved_skin = opt_on[OPT_GE_SKIN_RAPIDO];
	int saved_tri = opt_on[OPT_GE_TRI_RAPIDO];
	int n, bad = 0, total_prims = 0;
	u32 golden = 2166136261u;   /* lo que sale con los interruptores apagados */
	Case c;
	u32 *a = NULL;
	size_t an = 0;

	printf("Vertices del GE: camino por lotes contra uno a uno\n");
	ge_hw = &cap_hw;
	ge_fast_math = 1;
	opt_on[OPT_GE_SKIN_RAPIDO] = 1;
	for(n = 0; n < 3000; n++){
		GeStats sa, sb;
		random_case(&c);
		write_case_data(&c);
		run_case(&c, 0, &sa);
		free(a);
		a = malloc(cap_n * sizeof(u32) + 4);
		if(!a) break;
		memcpy(a, cap, cap_n * sizeof(u32));
		an = cap_n;
		for(size_t i = 0; i < an; i++){ golden ^= a[i]; golden *= 16777619u; }
		run_case(&c, 1, &sb);
		total_prims += (int)sb.primitives;
		if(an != cap_n || memcmp(a, cap, an * sizeof(u32)) || sa.vertices != sb.vertices || sa.vertex_reads != sb.vertex_reads ||
		   sa.tris != sb.tris || sa.tris_drawn != sb.tris_drawn || sa.vertices_skinned != sb.vertices_skinned ||
		   sa.primitives != sb.primitives){
			if(bad++ < 5){
				size_t i = 0;
				while(i < an && i < cap_n && a[i] == cap[i]) i++;
				printf("  caso %d: vt %06X %06X %06X prim %u %u %u n %u %u %u: distinto en la palabra %u de %u/%u\n", n,
				       (unsigned)c.vt[0], (unsigned)c.vt[1], (unsigned)c.vt[2], (unsigned)c.prim[0], (unsigned)c.prim[1],
				       (unsigned)c.prim[2], (unsigned)c.count[0], (unsigned)c.count[1], (unsigned)c.count[2],
				       (unsigned)i, (unsigned)an, (unsigned)cap_n);
			}
		}
	}
	CHECK_EQ(bad, 0);
	CHECK(total_prims > 3000);
	/* El modo de verificación del Wii no encuentra nada aquí */
	{
		char rep[200];
		ge_vdec_verif_report(rep, sizeof(rep));
		opt_on[OPT_GE_VDEC_VERIF] = 1;
		for(n = 0; n < 300; n++){
			GeStats sv;
			random_case(&c);
			write_case_data(&c);
			run_case(&c, 1, &sv);
		}
		opt_on[OPT_GE_VDEC_VERIF] = 0;
		ge_vdec_verif_report(rep, sizeof(rep));
		CHECK(strstr(rep, " 0 distintos") != NULL);
		CHECK(strstr(rep, " 0 comparados") == NULL);
		printf("  %s\n", rep);
		n = 3000;
	}   /* que de verdad se dibuje */
	printf("  %d tandas, %d primitivas comparadas (huella %08X)\n", n, total_prims, (unsigned)golden);
	free(a);
	ge_hw = saved_hw;
	ge_fast_math = saved_fast;
	opt_on[OPT_GE_VDEC_ESP] = (u8)saved_opt;
	opt_on[OPT_GE_SKIN_RAPIDO] = (u8)saved_skin;
	opt_on[OPT_GE_TRI_RAPIDO] = (u8)saved_tri;
	ge_vertex_test_reset();
}
