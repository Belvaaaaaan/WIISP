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

static u32 bs = 0xB0C5u;
static u32 brnd(void){ bs ^= bs << 13; bs ^= bs >> 17; bs ^= bs << 5; return bs; }
static u32 brn(u32 n){ return brnd() % n; }
static float bfr(float lo, float hi){ return lo + (hi - lo) * (float)(brnd() & 0xFFFFFF) / 16777215.0f; }

#define R_VERT 0x08B80000u
#define R_IDX  0x08B70000u
#define R_TEX  0x08B00000u
#define R_LIST 0x08C00000u
#define R_DATA 0x08D00000u

static void estado_pantalla(int profundidad){
	int i;
	memset(ge.cmd, 0, sizeof(ge.cmd));
	ge.cmd[GE_VIEWPORTXSCALE] = f24(240.0f); ge.cmd[GE_VIEWPORTYSCALE] = f24(-136.0f); ge.cmd[GE_VIEWPORTZSCALE] = f24(-32000.0f);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048.0f); ge.cmd[GE_VIEWPORTYCENTER] = f24(2048.0f); ge.cmd[GE_VIEWPORTZCENTER] = f24(32000.0f);
	ge.cmd[GE_OFFSETX] = (2048 - 240) * 16; ge.cmd[GE_OFFSETY] = (2048 - 136) * 16;
	ge.cmd[GE_SCISSOR2] = 479 | (271 << 10); ge.cmd[GE_REGION2] = 479 | (271 << 10);
	ge.cmd[GE_MINZ] = 0; ge.cmd[GE_MAXZ] = 0xFFFF;
	ge.cmd[GE_DEPTHCLAMPENABLE] = (u32)profundidad;
	ge.cmd[GE_FRAMEBUFPTR] = 0; ge.cmd[GE_FRAMEBUFWIDTH] = 512; ge.cmd[GE_FRAMEBUFPIXFORMAT] = 3;
	ge.cmd[GE_ZBUFPTR] = 0x110000; ge.cmd[GE_ZBUFWIDTH] = 512;
	ge.cmd[GE_TEXTUREMAPENABLE] = 1; ge.cmd[GE_TEXADDR0] = R_TEX & 0xFFFFFF; ge.cmd[GE_TEXBUFWIDTH0] = 16 | ((R_TEX >> 24) << 16);
	ge.cmd[GE_TEXSIZE0] = 4 | (4 << 8); ge.cmd[GE_TEXFORMAT] = 3; ge.cmd[GE_TEXFUNC] = 0; ge.cmd[GE_TEXFILTER] = 0;
	ge.cmd[GE_TEXSCALEU] = f24(1.0f); ge.cmd[GE_TEXSCALEV] = f24(1.0f);
	ge.cmd[GE_SHADEMODE] = 1;
	ge.cmd[GE_MATERIALAMBIENT] = 0xFFFFFF; ge.cmd[GE_MATERIALALPHA] = 0xFF;
	memset(ge.world, 0, sizeof(ge.world)); memset(ge.view, 0, sizeof(ge.view)); memset(ge.proj, 0, sizeof(ge.proj));
	for(i = 0; i < 12; i++) ge.world[i] = ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	ge.view[11] = -5.0f;
	ge.proj[0] = 1.5f; ge.proj[5] = 2.0f; ge.proj[10] = -51.0f / 49.0f; ge.proj[11] = -1.0f; ge.proj[14] = -100.0f / 49.0f;
	ge.offset = 0;
	ge.vaddr = R_VERT;
	ge.iaddr = R_IDX;
}

static u32 vt_azar(void){
	u32 tc = brn(4), col = brn(5) ? 4 + brn(4) : 0, nrm = brn(4), pos = 1 + brn(3), wt = brn(4) ? 0 : 1 + brn(3);
	u32 nw = brn(8), nm = brn(5) ? 0 : brn(3), idx = brn(4) ? 0 : 1 + brn(2), thr = brn(8) == 0;
	return tc | (col << 2) | (nrm << 5) | (pos << 7) | (wt << 9) | (idx << 11) | (nw << 14) | (nm << 18) | (thr << 23);
}

static void bbox_rapida_azar(void){
	u32 ronda, distintas = 0, probadas = 0, visibles = 0;
	printf("Cajas: prueba rapida (ge_bbox_rapido) contra la de siempre\n");
	for(ronda = 0; ronda < 2000; ronda++){
		u8 *v = mem_ptr(R_VERT, 0x8000), *ix = mem_ptr(R_IDX, 0x1000);
		u32 vt = vt_azar(), count[4], vis[2][4], va[2][4], ia[2][4], i, cambio[4];
		float nuevo[4];
		int k, j;
		GeState ini;
		estado_pantalla((int)brn(2));
		for(i = 0; i < 0x8000; i++) v[i] = (u8)brnd();
		for(i = 0; i < 0x1000; i++) ix[i] = (u8)brn(64);
		if(((vt >> 7) & 3) == 3 && brn(2)){
			for(i = 0; i < 0x8000; i += 4){
				float f = bfr(-2.0f, 2.0f);
				u32 b;
				memcpy(&b, &f, 4);
				v[i] = (u8)b; v[i + 1] = (u8)(b >> 8); v[i + 2] = (u8)(b >> 16); v[i + 3] = (u8)(b >> 24);
			}
		}
		for(k = 0; k < 12; k++){ ge.world[k] = brn(3) ? bfr(-3.0f, 3.0f) : (float)(int)brn(3); ge.view[k] = brn(4) ? ge.view[k] : bfr(-3.0f, 3.0f); }
		for(k = 0; k < 16; k++) if(!brn(4)) ge.proj[k] = bfr(-3.0f, 3.0f);
		for(k = 0; k < 96; k++) ge.bone[k / 12][k % 12] = bfr(-2.0f, 2.0f);
		for(k = 0; k < 8; k++) ge.cmd[GE_MORPHWEIGHT0 + k] = f24(bfr(0.0f, 1.0f));
		if(!brn(6)) ge.cmd[GE_SCISSOR1] = brn(400) | (brn(200) << 10);
		if(!brn(8)) ge.cmd[GE_OFFSETX] = brn(0x10000);
		if(!brn(10)) ge.cmd[GE_VIEWPORTXSCALE] = f24(bfr(-500.0f, 500.0f));
		if(!brn(50)) ge.world[brn(12)] = brn(2) ? 1.0f / 0.0f : 0.0f / 0.0f;
		ge.cmd[GE_VERTEXTYPE] = vt;
		ge.vaddr = R_VERT + (brn(3) ? brn(64) * 4 : brn(64));
		ge.iaddr = R_IDX + brn(64);
		for(j = 0; j < 4; j++){
			count[j] = brn(20) ? 1 + brn(16) : (brn(2) ? 0x100 + brn(20) : 0x200 + brn(20));
			cambio[j] = brn(5);
			nuevo[j] = bfr(-3.0f, 3.0f);
		}
		memcpy(&ini, &ge, sizeof(ini));
		for(k = 0; k < 2; k++){
			memcpy(&ge, &ini, sizeof(ge));
			ge_vertex_reset();
			opt_on[OPT_GE_BBOX_RAPIDO] = (u8)k;
			opt_on[OPT_GE_VERIF] = (u8)(k && (ronda & 1));
			for(j = 0; j < 4; j++){
				if(cambio[j] == 1) ge.world[j * 3] = nuevo[j];
				else if(cambio[j] == 2) ge.view[j * 3 + 1] = nuevo[j];
				else if(cambio[j] == 3) ge.proj[j * 4 + 2] = nuevo[j];
				ge.vaddr = ini.vaddr + (u32)j * 64;
				ge_bounding_box(count[j]);
				vis[k][j] = (u32)ge.bbox_visible;
				va[k][j] = ge.vaddr;
				ia[k][j] = ge.iaddr;
			}
		}
		for(j = 0; j < 4; j++){
			probadas++;
			visibles += vis[1][j];
			if(vis[0][j] != vis[1][j] || va[0][j] != va[1][j] || ia[0][j] != ia[1][j]){
				if(distintas++ < 8)
					printf("  ronda %u caja %d distinta: tipo 0x%06X, %u vertices, visible %u/%u\n", (unsigned)ronda, j, (unsigned)vt,
					       (unsigned)count[j], (unsigned)vis[0][j], (unsigned)vis[1][j]);
			}
		}
	}
	opt_on[OPT_GE_VERIF] = 0;
	printf("  %u cajas (%u visibles): %u distintas; con ge_verificar %u comparadas, %u distintas\n", (unsigned)probadas,
	       (unsigned)visibles, (unsigned)distintas, (unsigned)ge_stats.bbv_cajas, (unsigned)ge_stats.bbv_malas);
	CHECK_EQ(distintas, 0u);
	CHECK_EQ(ge_stats.bbv_malas, 0u);
	CHECK(ge_stats.bbv_cajas > 2000);
	CHECK(visibles > 300 && visibles < probadas - 300);
}

static u32 rl, rv;

static void rcmd(u32 c, u32 a){ mem_write32(rl, (c << 24) | (a & 0xFFFFFF)); rl += 4; }

static void rbase(u32 a){ rcmd(GE_BASE, (a >> 8) & 0x0F0000); }

static void rw32(u32 b){ mem_write32(rv, b); rv += 4; }

static u32 r_vertices(u32 vt, int n, const float *lo, const float *hi, float *mn, float *mx, int coherentes){
	u32 a, tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, size = 0, al = 1;
	int i, k;
	static const u8 tcs[4] = { 0, 2, 4, 8 }, cols[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }, nrms[4] = { 0, 3, 6, 12 }, poss[4] = { 3, 3, 6, 12 };
	static const u8 tca[4] = { 1, 1, 2, 4 }, nra[4] = { 1, 1, 2, 4 }, poa[4] = { 1, 1, 2, 4 };
	u32 off_tc, off_col, off_nrm, off_pos;
	size = 0;
	if((vt >> 9) & 3){
		static const u8 wts[4] = { 0, 1, 2, 4 };
		size = wts[(vt >> 9) & 3] * (((vt >> 14) & 7) + 1);
		al = wts[(vt >> 9) & 3];
	}
	if(tc){ size = (size + tca[tc] - 1) & ~(u32)(tca[tc] - 1); off_tc = size; size += tcs[tc]; if(tca[tc] > al) al = tca[tc]; } else off_tc = 0;
	if(col){ size = (size + cols[col] - 1) & ~(u32)(cols[col] - 1); off_col = size; size += cols[col]; if(cols[col] > al) al = cols[col]; } else off_col = 0;
	if(nrm){ size = (size + nra[nrm] - 1) & ~(u32)(nra[nrm] - 1); off_nrm = size; size += nrms[nrm]; if(nra[nrm] > al) al = nra[nrm]; } else off_nrm = 0;
	size = (size + poa[pos] - 1) & ~(u32)(poa[pos] - 1); off_pos = size; size += poss[pos]; if(poa[pos] > al) al = poa[pos];
	size = (size + al - 1) & ~(al - 1);
	rv = (rv + 3) & ~3u;
	a = rv;
	for(i = 0; i < 3; i++){ mn[i] = 1e30f; mx[i] = -1e30f; }
	for(i = 0; i < n; i++){
		u8 *p = mem_ptr(a + (u32)i * size, size);
		for(k = 0; k < (int)size; k++) p[k] = (u8)brnd();
		if(tc == 1){ p[off_tc] = (u8)brn(256); p[off_tc + 1] = (u8)brn(256); }
		for(k = 0; k < 3; k++){
			float f = coherentes ? bfr(lo[k], hi[k]) : bfr(-1.0f, 1.0f), d;
			if(pos == 1){ int q = (int)(f * 127.0f); p[off_pos + k] = (u8)(s8)q; d = (float)q / 128.0f; }
			else if(pos == 2){ int q = (int)(f * 32767.0f); p[off_pos + 2 * k] = (u8)q; p[off_pos + 2 * k + 1] = (u8)(q >> 8); d = (float)q / 32768.0f; }
			else { u32 b; memcpy(&b, &f, 4); p[off_pos + 4 * k] = (u8)b; p[off_pos + 4 * k + 1] = (u8)(b >> 8); p[off_pos + 4 * k + 2] = (u8)(b >> 16); p[off_pos + 4 * k + 3] = (u8)(b >> 24); d = f; }
			if(d < mn[k]) mn[k] = d;
			if(d > mx[k]) mx[k] = d;
		}
		(void)off_col; (void)off_nrm;
	}
	rv = a + (u32)n * size;
	return a;
}

static u32 r_caja(const float *mn, const float *mx){
	u32 a, m = brn(8);
	int j, k;
	rv = (rv + 3) & ~3u;
	a = rv;
	for(j = 0; j < 8; j++)
		for(k = 0; k < 3; k++){
			float f = ((u32)j ^ m) >> k & 1 ? mx[k] : mn[k];
			u32 b;
			memcpy(&b, &f, 4);
			rw32(b);
		}
	return a;
}

static void r_dibujo(u32 vt, u32 prim, u32 a, int n){
	rcmd(GE_VERTEXTYPE, vt);
	rbase(a);
	rcmd(GE_VADDR, a & 0xFFFFFF);
	rcmd(GE_PRIM, (prim << 16) | (u32)n);
}

static void r_mundo(float tx, float ty, float tz, float esc){
	int k;
	float m[12] = { esc, 0, 0, 0, esc, 0, 0, 0, esc, tx, ty, tz };
	float ang = bfr(0.0f, 6.28f), c = esc * (float)__builtin_cos(ang), s = esc * (float)__builtin_sin(ang);
	m[0] = c; m[2] = -s; m[6] = s; m[8] = c;
	rcmd(GE_WORLDMATRIXNUMBER, 0);
	for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
}

static u32 r_lista(int objetos){
	int o;
	rl = R_LIST;
	rv = R_DATA;
	for(o = 0; o < objetos; o++){
		float lo[3], hi[3], mn[3], mx[3], cmn[3], cmx[3];
		int d, ndib = 1 + (int)brn(3), mentira = brn(8) == 0, k;
		u32 caja, salto;
		u32 donde = brn(16);
		if(donde >= 12 && donde < 14){
			int e, borde = (int)brn(6);
			float tx = 0.0f, ty = 0.0f, tz = -15.0f + 5.0f;
			float lo2[3] = { -1.0f, -1.0f, -1.0f }, hi2[3] = { 1.0f, 1.0f, 1.0f };
			u32 vt = (1 + brn(3)) | (7u << 2) | ((1 + brn(3)) << 7), a;
			int nv = 6 + (int)brn(10);
			float esc = 2.0f, j = 1.0f;
			if(borde < 4 && brn(2)){ esc = 0.3f; j = 0.4f; }
			if(borde == 0) tx = -15.0f / 1.5f + bfr(-j, j);
			else if(borde == 1) tx = 15.0f / 1.5f + bfr(-j, j);
			else if(borde == 2) ty = -15.0f / 2.0f + bfr(-j, j);
			else if(borde == 3) ty = 15.0f / 2.0f + bfr(-j, j);
			else if(borde == 4) tz = 5.0f - 1.0f + bfr(-1.5f, 1.5f);
			else tz = 5.0f - 50.0f + bfr(-1.5f, 1.5f);
			rcmd(GE_WORLDMATRIXNUMBER, 0);
			for(e = 0; e < 12; e++) rcmd(GE_WORLDMATRIXDATA, f24(e == 0 || e == 4 || e == 8 ? esc : e == 9 ? tx : e == 10 ? ty : e == 11 ? tz : 0.0f));
			a = r_vertices(vt, nv, lo2, hi2, mn, mx, 1);
			caja = r_caja(mn, mx);
			rcmd(GE_VERTEXTYPE, 0x180);
			rbase(caja);
			rcmd(GE_VADDR, caja & 0xFFFFFF);
			rcmd(GE_BOUNDINGBOX, 8);
			salto = rl + 4;
			rbase(rl);
			rl += 4;
			r_dibujo(vt, GE_PRIM_TRIANGLE_STRIP, a, nv);
			mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
			continue;
		}
		if(donde >= 10 && donde < 12){
			int e;
			float lo2[3] = { -1.0f, -1.0f, -1.0f }, hi2[3] = { 1.0f, 1.0f, 1.0f };
			u32 vt = (1 + brn(3)) | (7u << 2) | ((1 + brn(3)) << 7), a;
			int nv = 6 + (int)brn(10);
			rcmd(GE_WORLDMATRIXNUMBER, 0);
			for(e = 0; e < 12; e++) rcmd(GE_WORLDMATRIXDATA, f24(e == 0 || e == 4 ? 8.0f : e == 8 ? 15.0f : e == 11 ? -50.0f : 0.0f));
			a = r_vertices(vt, nv, lo2, hi2, mn, mx, 1);
			mx[2] = -0.2f;
			caja = r_caja(mn, mx);
			rcmd(GE_VERTEXTYPE, 0x180);
			rbase(caja);
			rcmd(GE_VADDR, caja & 0xFFFFFF);
			rcmd(GE_BOUNDINGBOX, 8);
			salto = rl + 4;
			rbase(rl);
			rl += 4;
			r_dibujo(vt, GE_PRIM_TRIANGLE_STRIP, a, nv);
			mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
			continue;
		}
		if(donde < 4) r_mundo(bfr(-4.0f, 4.0f), bfr(-3.0f, 3.0f), 5.0f + bfr(3.0f, 20.0f), bfr(0.2f, 1.5f));
		else if(donde < 7) r_mundo(bfr(-20.0f, 20.0f), bfr(-15.0f, 15.0f), 5.0f - bfr(52.0f, 80.0f), bfr(0.2f, 1.5f));
		else if(donde >= 14){
			float esc = bfr(8.0f, 12.0f);
			rcmd(GE_WORLDMATRIXNUMBER, 0);
			for(k = 0; k < 12; k++)
				rcmd(GE_WORLDMATRIXDATA, f24(k == 0 || k == 4 || k == 8 ? esc : k == 9 || k == 10 ? bfr(-1.0f, 1.0f) : k == 11 ? 7.0f + esc + bfr(0.0f, 1.0f) : 0.0f));
		}
		else {
			float r = bfr(3.0f, 30.0f), th = bfr(0.0f, 6.28f), ph = bfr(-1.2f, 1.2f);
			r_mundo(r * (float)__builtin_cos(th) * (float)__builtin_cos(ph), r * (float)__builtin_sin(ph),
			        5.0f + r * (float)__builtin_sin(th) * (float)__builtin_cos(ph), bfr(0.2f, 1.5f));
		}
		for(k = 0; k < 3; k++){ lo[k] = bfr(-1.0f, 0.0f); hi[k] = bfr(0.0f, 1.0f); cmn[k] = 1e30f; cmx[k] = -1e30f; }
		if(donde >= 14) for(k = 0; k < 3; k++){ lo[k] = -1.0f; hi[k] = 1.0f; }
		{
			u32 vts[3], dirs[3], prims[3];
			int ns[3];
			for(d = 0; d < ndib; d++){
				static const u32 pr[8] = { 3, 4, 5, 4, 4, 3, 0, 6 };
				vts[d] = (brn(4) ? 1 + brn(3) : 0) | ((brn(3) ? 4 + brn(4) : 0) << 2) | (brn(3) << 5) | ((1 + brn(3)) << 7);
				if(!brn(10)) vts[d] |= (1u << 9);
				prims[d] = pr[brn(8)];
				ns[d] = 1 + (int)brn(14);
				dirs[d] = r_vertices(vts[d], ns[d], lo, hi, mn, mx, 1);
				for(k = 0; k < 3; k++){ if(mn[k] < cmn[k]) cmn[k] = mn[k]; if(mx[k] > cmx[k]) cmx[k] = mx[k]; }
			}
			if(mentira){ int e = (int)brn(3); cmx[e] = cmn[e] + (cmx[e] - cmn[e]) * 0.5f; }
			caja = r_caja(cmn, cmx);
			rcmd(GE_VERTEXTYPE, 0x180);
			rbase(caja);
			rcmd(GE_VADDR, caja & 0xFFFFFF);
			rcmd(GE_BOUNDINGBOX, 8);
			salto = rl + 4;
			rbase(rl);
			rl += 4;
			for(d = 0; d < ndib; d++) r_dibujo(vts[d], prims[d], dirs[d], ns[d]);
			mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
		}
		if(!brn(4)){
			u32 vt = (1 + brn(3)) | (7u << 2) | (3u << 7), a;
			float l2[3] = { -0.3f, -0.3f, -0.3f }, h2[3] = { 0.3f, 0.3f, 0.3f };
			if(brn(2)) vt &= ~3u;
			r_mundo(bfr(-1.0f, 1.0f), bfr(-1.0f, 1.0f), bfr(-2.0f, 1.0f), 1.0f);
			a = r_vertices(vt, 2 + (int)brn(5), l2, h2, mn, mx, 1);
			k = (int)brn(6);
			if(k == 0) r_dibujo(vt, GE_PRIM_TRIANGLES, a, 0);
			else if(k == 1) r_dibujo(vt & ~(3u << 7), GE_PRIM_TRIANGLES, a, 3);
			r_dibujo(vt, brn(2) ? GE_PRIM_CONTINUE : GE_PRIM_TRIANGLE_STRIP, a, 2 + (int)brn(4));
		}
	}
	return rl;
}

static void bbox_saltos_render(void){
	static u8 vram_ref[0x200000];
	u32 lista, ronda, distintas = 0, saltadas = 0, fuera = 0, detras = 0, verif = 0, verif_mal = 0, i;
	u8 *tex = mem_ptr(R_TEX, 16 * 16 * 4), *vram = mem_ptr(0x04000000, 0x200000);
	printf("Cajas exactas (ge_bbox): mismas imagenes saltandose las llamadas de cajas enteras fuera\n");
	for(i = 0; i < 16 * 16 * 4; i++) tex[i] = (u8)brnd();
	ge_hw = NULL;
	for(ronda = 0; ronda < 240; ronda++){
		GeState ini;
		int modo, prof = (int)brn(2);
		estado_pantalla(prof);
		if(brn(2)){ ge.cmd[GE_CULLFACEENABLE] = 1; ge.cmd[GE_CULL] = brn(2); }
		memcpy(&ini, &ge, sizeof(ini));
		lista = r_lista(6 + (int)brn(10));
		for(modo = 0; modo < 3; modo++){
			ge_fast_math = (int)(ronda & 1);
			memcpy(&ge, &ini, sizeof(ge));
			ge_vertex_reset();
			ge_raster_reset();
			memset(vram, 0x5A, 0x200000);
			memset(&ge_stats, 0, sizeof(ge_stats));
			opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
			opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
			opt_on[OPT_GE_BBOX_RAPIDO] = (u8)(ronda % 3 != 0);
			ge_test_run_list(R_LIST, lista, NULL);
			if(modo == 0){ memcpy(vram_ref, vram, sizeof(vram_ref)); continue; }
			if(modo == 1){ saltadas += ge_stats.bx_saltadas; fuera += ge_stats.bx_fuera; detras += ge_stats.bx_detras;
			}
			else { verif += ge_stats.bx_verif; verif_mal += ge_stats.bx_verif_bad; }
			if(memcmp(vram_ref, vram, sizeof(vram_ref))){
				if(distintas++ < 8) printf("  ronda %u modo %d: imagen distinta\n", (unsigned)ronda, modo);
			}
		}
	}
	opt_on[OPT_GE_VERIF] = 0;
	printf("  %u listas: %u cajas enteras fuera (%u detras de la camara), %u llamadas saltadas; %u imagenes distintas; "
	       "con ge_verificar %u comprobadas, %u distintas\n", (unsigned)ronda, (unsigned)fuera, (unsigned)detras, (unsigned)saltadas,
	       (unsigned)distintas, (unsigned)verif, (unsigned)verif_mal);
	CHECK_EQ(distintas, 0u);
	CHECK_EQ(verif_mal, 0u);
	CHECK(saltadas > 100);
	CHECK(verif > 100);
	CHECK(detras > 50);
}

static u32 r_centro(u32 vt, u32 prim, int n){
	float lo[3] = { -0.3f, -0.3f, -0.3f }, hi[3] = { 0.3f, 0.3f, 0.3f }, mn[3], mx[3];
	u32 a = r_vertices(vt, n, lo, hi, mn, mx, 1);
	r_dibujo(vt, prim, a, n);
	return a;
}

static u32 r_pico(int variante){
	float lo[3] = { -1.0f, -1.0f, -1.0f }, hi[3] = { 1.0f, 1.0f, 1.0f }, mn[3], mx[3];
	u32 vt = 1 | (7u << 2) | (3u << 7), a, caja, salto, ix;
	int e;
	rl = R_LIST;
	rv = R_DATA;
	r_mundo(0.0f, 0.0f, 0.0f, 1.0f);
	r_centro(vt, GE_PRIM_TRIANGLE_STRIP, 4);
	rcmd(GE_WORLDMATRIXNUMBER, 0);
	for(e = 0; e < 12; e++) rcmd(GE_WORLDMATRIXDATA, f24(e == 0 || e == 4 || e == 8 ? 1.0f : e == 11 ? -55.0f : 0.0f));
	a = r_vertices(vt, 6, lo, hi, mn, mx, 1);
	caja = r_caja(mn, mx);
	rcmd(GE_VERTEXTYPE, 0x180);
	rbase(caja);
	rcmd(GE_VADDR, caja & 0xFFFFFF);
	rcmd(GE_BOUNDINGBOX, 8);
	salto = rl + 4;
	rbase(rl);
	rl += 4;
	r_dibujo(vt, GE_PRIM_TRIANGLE_STRIP, a, 6);
	mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
	r_mundo(0.0f, 0.0f, 0.0f, 1.0f);
	rv = (rv + 3) & ~3u;
	ix = rv;
	for(e = 0; e < 4; e++) mem_write8(ix + (u32)e, (u8)e);
	rv += 4;
	switch(variante){
	case 1: r_dibujo(vt, GE_PRIM_TRIANGLES, a, 0); break;
	case 2: r_dibujo(vt & ~(3u << 7), GE_PRIM_TRIANGLES, a, 3); break;
	case 3: rcmd(GE_VERTEXTYPE, vt); rcmd(GE_BASE, 0); rcmd(GE_VADDR, 0x100); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); break;
	case 4: r_centro(vt, GE_PRIM_TRIANGLES, 3); break;
	case 5: r_dibujo(vt, GE_PRIM_TRIANGLES, a, 0); r_centro(vt, GE_PRIM_TRIANGLE_STRIP, 4); break;
	case 6: case 7:
		a = r_vertices(vt, 4, lo, hi, mn, mx, 0);
		rcmd(GE_VERTEXTYPE, vt | (1u << 11));
		rbase(a);
		rcmd(GE_VADDR, a & 0xFFFFFF);
		if(variante == 6){ rbase(ix); rcmd(GE_IADDR, ix & 0xFFFFFF); }
		else { rcmd(GE_BASE, 0); rcmd(GE_IADDR, 0x100); }
		rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
		break;
	default: break;
	}
	r_centro(vt, GE_PRIM_CONTINUE, 3);
	return rl;
}

static void bbox_saltos_pico(void){
	static u8 vram_ref[0x200000];
	static const u32 ligeras[8] = { 0, 0, 0, 0, 1, 1, 1, 0 };
	u8 *vram = mem_ptr(0x04000000, 0x200000);
	u32 lista, distintas = 0, malas = 0;
	int variante, mr, modo;
	printf("Cajas exactas: lo que deja una llamada saltada antes de una continuacion\n");
	for(variante = 0; variante < 8; variante++)
		for(mr = 0; mr < 2; mr++){
			GeState ini;
			estado_pantalla(0);
			memcpy(&ini, &ge, sizeof(ini));
			lista = r_pico(variante);
			for(modo = 0; modo < 3; modo++){
				ge_fast_math = mr;
				memcpy(&ge, &ini, sizeof(ge));
				ge_vertex_reset();
				ge_raster_reset();
				memset(vram, 0x5A, 0x200000);
				memset(&ge_stats, 0, sizeof(ge_stats));
				opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
				opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
				ge_test_run_list(R_LIST, lista, NULL);
				if(modo == 0){ memcpy(vram_ref, vram, sizeof(vram_ref)); continue; }
				if(modo == 1 && (ge_stats.bx_saltadas != 1 || ge_stats.bx_ligeras != ligeras[variante])){
					if(malas++ < 8) printf("  variante %d: %u saltadas, %u ligeras\n", variante, (unsigned)ge_stats.bx_saltadas,
					                       (unsigned)ge_stats.bx_ligeras);
				}
				if(modo == 2 && (ge_stats.bx_verif != 1 || ge_stats.bx_verif_bad)) malas++;
				if(memcmp(vram_ref, vram, sizeof(vram_ref)) && distintas++ < 8)
					printf("  variante %d modo %d: imagen distinta\n", variante, modo);
			}
			if(!variante && !mr){
				u32 i, n = 0;
				for(i = 0; i < sizeof(vram_ref); i += 4) n += memcmp(vram_ref + i, "\x5A\x5A\x5A\x5A", 4) != 0;
				CHECK(n > 20);
			}
		}
	opt_on[OPT_GE_VERIF] = 0;
	printf("  8 variantes en exacto y MR: %u imagenes distintas, %u cuentas distintas\n", (unsigned)distintas, (unsigned)malas);
	CHECK_EQ(distintas, 0u);
	CHECK_EQ(malas, 0u);
}

static void p_fin(u32 vt){
	rcmd(GE_VERTEXTYPE, vt);
	rbase(R_VERT);
	rcmd(GE_VADDR, R_VERT & 0xFFFFFF);
	rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
}

static void p_salto(u32 c, u32 dir){
	rbase(dir);
	rcmd(c, dir & 0xFFFFFF);
}

static void bbox_pico_directo(void){
	const u32 vt = 1 | (7u << 2) | (3u << 7), sin_pos = 1 | (7u << 2), lejos = R_LIST + 0x10000;
	const u32 x = R_LIST + 0x1000;
	u32 malos = 0, casos = 0, i;
	int caso, con_stall;
	printf("Cajas exactas: lo que mira ge_prim_datos_sobran por delante en la lista\n");
	memset(mem_ptr(R_VERT, 0x100), 0, 0x100);
	for(caso = 0; caso < 34; caso++)
		for(con_stall = 0; con_stall < 2; con_stall++){
			int esperado = -1, r;
			u32 stall = con_stall ? lejos : 0, tras = R_VERT;
			memset(mem_ptr(R_LIST, 0x2000), 0, 0x2000);
			memset(ge.cmd, 0, sizeof(ge.cmd));
			ge.cmd[GE_VERTEXTYPE] = vt;
			ge.offset = 0;
			ge.iaddr = R_IDX;
			rl = R_LIST;
			rcmd(GE_PRIM, (GE_PRIM_TRIANGLE_STRIP << 16) | 6);
			switch(caso){
			case 0: rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); esperado = 1; break;
			case 1: rcmd(GE_PRIM, (GE_PRIM_CONTINUE << 16) | 3); p_fin(vt); esperado = 0; break;
			case 2: rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 0); rcmd(GE_PRIM, (GE_PRIM_CONTINUE << 16) | 3); p_fin(vt); esperado = 0; break;
			case 3: rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 0); rcmd(GE_PRIM, (GE_PRIM_TRIANGLE_STRIP << 16) | 4); esperado = 1; break;
			case 4: rcmd(GE_VERTEXTYPE, sin_pos); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); p_fin(vt); esperado = 0; break;
			case 5: rcmd(GE_VERTEXTYPE, sin_pos); rcmd(GE_VERTEXTYPE, vt); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); esperado = 1; break;
			case 6: rcmd(GE_BASE, 0); rcmd(GE_VADDR, 0x100); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); p_fin(vt); esperado = 0; break;
			case 7: rcmd(GE_BOUNDINGBOX, 8); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); p_fin(vt); esperado = 0; break;
			case 8: rcmd(GE_BOUNDINGBOX, 8); p_fin(vt); esperado = 1; break;
			case 9: rcmd(GE_VERTEXTYPE, vt | (1u << 11)); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); esperado = 1; break;
			case 10: rcmd(GE_VERTEXTYPE, vt | (1u << 11)); rcmd(GE_BASE, 0); rcmd(GE_IADDR, 0x100); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
			         p_fin(vt); esperado = 0; break;
			case 11: rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); tras = 0x100; esperado = 0; break;
			case 12: rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); tras = R_VERT + 1; esperado = 1; break;
			case 13: p_salto(GE_JUMP, x); p_fin(sin_pos); rl = x; rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); esperado = !con_stall; break;
			case 14: p_salto(GE_JUMP, x); p_fin(vt); rl = x; rcmd(GE_PRIM, (GE_PRIM_CONTINUE << 16) | 3); p_fin(vt); esperado = 0; break;
			case 15: p_salto(GE_BJUMP, x); p_fin(vt); rl = x; p_fin(vt); esperado = !con_stall; break;
			case 16: p_salto(GE_BJUMP, x); rcmd(GE_PRIM, (GE_PRIM_CONTINUE << 16) | 3); p_fin(vt); rl = x; p_fin(vt); esperado = 0; break;
			case 17: p_salto(GE_BJUMP, x); p_fin(vt); rl = x; rcmd(GE_PRIM, (GE_PRIM_CONTINUE << 16) | 3); p_fin(vt); esperado = 0; break;
			case 18: case 19:
				for(i = 0; i < (u32)(caso == 18 ? 4 : 5); i++){ p_salto(GE_JUMP, x + i * 0x100); rl = x + i * 0x100; }
				p_fin(vt);
				esperado = caso == 18 && !con_stall;
				break;
			case 20: case 21:
				for(i = 0; i < (u32)(caso == 20 ? 47 : 48); i++) rcmd(GE_NOP, 0);
				rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
				esperado = caso == 20;
				break;
			case 22: rcmd(GE_CALL, 0); p_fin(vt); esperado = 0; break;
			case 23: rcmd(GE_RET, 0); p_fin(vt); esperado = 0; break;
			case 24: rcmd(GE_END, 0); p_fin(vt); esperado = 0; break;
			case 25: rcmd(GE_FINISH, 0); p_fin(vt); esperado = 0; break;
			case 26: rcmd(GE_SIGNAL, 0); p_fin(vt); esperado = 0; break;
			case 27: rcmd(GE_BEZIER, 0x101); p_fin(vt); esperado = 0; break;
			case 28: rcmd(GE_SPLINE, 0x101); p_fin(vt); esperado = 0; break;
			case 29: rcmd(GE_VSCX, 0); p_fin(vt); esperado = 0; break;
			case 30: rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); stall = R_LIST + 4; esperado = 0; break;
			case 31: rcmd(GE_OFFSETADDR, (R_VERT >> 8) & 0xFFFFFF); rcmd(GE_BASE, 0); rcmd(GE_VADDR, 0); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
			         esperado = 1; break;
			case 32: rcmd(GE_BASE, 0); rcmd(GE_ORIGIN, 0); rcmd(GE_VADDR, 0x10); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3); esperado = 1; break;
			case 33: rcmd(GE_BASE, 0); rcmd(GE_OFFSETADDR, 0); rcmd(GE_VADDR, 0x10); rcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
			         p_fin(vt); esperado = 0; break;
			}
			r = ge_test_pico(R_LIST, stall, tras);
			casos++;
			if(r != esperado && malos++ < 8) printf("  caso %d (stall %d): %d, tenia que dar %d\n", caso, con_stall, r, esperado);
		}
	printf("  %u casos: %u distintos de lo esperado\n", (unsigned)casos, (unsigned)malos);
	CHECK_EQ(malos, 0u);
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
	opt_on[OPT_PROF_BBOX] = 0;
	bbox_rapida_azar();
	memset(&ge_stats, 0, sizeof(ge_stats));
	bbox_saltos_render();
	memset(&ge_stats, 0, sizeof(ge_stats));
	bbox_saltos_pico();
	bbox_pico_directo();
	memset(&ge_stats, 0, sizeof(ge_stats));
	memcpy(opt_on, saved_opt, sizeof(saved_opt));
	ge_hw = saved_hw;
	ge_fast_math = saved_fast;
	ge_vertex_reset();
	ge_raster_reset();
}
