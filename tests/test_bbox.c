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
static float f24f(float f){ u32 b; memcpy(&b, &f, 4); b &= ~0xFFu; memcpy(&f, &b, 4); return f; }   //Arreglo

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

static u32 p_escribir(u32 pos, const float (*v)[3], int n){   //Arreglo
	u32 size = pos == 1 ? 8 : pos == 2 ? 12 : 16, a;
	int i, k;
	rv = (rv + 3) & ~3u;
	a = rv;
	for(i = 0; i < n; i++){
		u8 *p = mem_ptr(a + (u32)i * size, size);
		memset(p, 0, size);
		p[0] = (u8)brnd(); p[1] = (u8)brnd(); p[2] = (u8)brnd(); p[3] = 0xFF;
		for(k = 0; k < 3; k++){
			if(pos == 1){
				int q = (int)(v[i][k] * 128.0f);
				q = q < -128 ? -128 : q > 127 ? 127 : q;
				p[4 + k] = (u8)(s8)q;
			} else if(pos == 2){
				int q = (int)(v[i][k] * 32768.0f);
				q = q < -32768 ? -32768 : q > 32767 ? 32767 : q;
				p[4 + 2 * k] = (u8)q;
				p[5 + 2 * k] = (u8)(q >> 8);
			} else {
				u32 b;
				memcpy(&b, &v[i][k], 4);
				p[4 + 4 * k] = (u8)b; p[5 + 4 * k] = (u8)(b >> 8); p[6 + 4 * k] = (u8)(b >> 16); p[7 + 4 * k] = (u8)(b >> 24);
			}
		}
	}
	rv = a + (u32)n * size;
	return a;
}

static void p_leida(u32 pos, const float *v, float *o){   //Arreglo
	int k;
	for(k = 0; k < 3; k++){
		if(pos == 1){ int q = (int)(v[k] * 128.0f); q = q < -128 ? -128 : q > 127 ? 127 : q; o[k] = (float)q / 128.0f; }
		else if(pos == 2){ int q = (int)(v[k] * 32768.0f); q = q < -32768 ? -32768 : q > 32767 ? 32767 : q; o[k] = (float)q / 32768.0f; }
		else o[k] = v[k];
	}
}

static void p_mundo(float tx, float ty, float tz, float esc, float ang){   //Arreglo
	float m[12], c = esc * (float)__builtin_cos(ang), sn = esc * (float)__builtin_sin(ang);
	int k;
	m[0] = c; m[1] = 0.0f; m[2] = -sn;
	m[3] = 0.0f; m[4] = esc; m[5] = 0.0f;
	m[6] = sn; m[7] = 0.0f; m[8] = c;
	m[9] = tx; m[10] = ty; m[11] = tz;
	rcmd(GE_WORLDMATRIXNUMBER, 0);
	for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
}

static float pp_a, pp_b, pp_n, pp_f;   //Arreglo
static int pp_orto;   //Arreglo

static void p_pantalla(int prof){   //Arreglo
	static const float ks[5] = { 1.0f, 1.0f, 1.5f, 2.0f, 3.0f };
	float xs = 240.0f * ks[brn(5)] * (brn(8) ? 1.0f : -1.0f), ys = -136.0f * ks[brn(5)] * (brn(8) ? 1.0f : -1.0f);
	u32 s2;
	int i;
	estado_pantalla(prof);
	ge.cmd[GE_VIEWPORTXSCALE] = f24(xs);
	ge.cmd[GE_VIEWPORTYSCALE] = f24(ys);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048.0f + (float)((int)brn(81) - 40));
	ge.cmd[GE_VIEWPORTYCENTER] = f24(2048.0f + (float)((int)brn(81) - 40));
	ge.cmd[GE_OFFSETX] = (u32)((2048 - 240 + (int)brn(61) - 30) * 16 + (int)brn(16));
	ge.cmd[GE_OFFSETY] = (u32)((2048 - 136 + (int)brn(61) - 30) * 16 + (int)brn(16));
	ge.cmd[GE_SCISSOR1] = brn(3) ? 0 : brn(60) | (brn(40) << 10);
	s2 = (479 - (brn(3) ? 0 : brn(80))) | ((271 - (brn(3) ? 0 : brn(60))) << 10);
	ge.cmd[GE_SCISSOR2] = s2;
	ge.cmd[GE_REGION2] = brn(3) ? s2 : (479 - brn(100)) | ((271 - brn(80)) << 10);
	for(i = 0; i < 12; i++) ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	memset(ge.proj, 0, sizeof(ge.proj));
	pp_orto = brn(5) == 0;
	pp_n = bfr(0.2f, 2.0f);
	pp_f = bfr(30.0f, 150.0f);
	pp_a = bfr(0.7f, 2.0f);
	pp_b = pp_a * bfr(1.2f, 2.0f);
	if(pp_orto){
		pp_a *= 0.1f; pp_b *= 0.1f;
		ge.proj[0] = pp_a; ge.proj[5] = pp_b; ge.proj[10] = -2.0f / (pp_f - pp_n); ge.proj[14] = -(pp_f + pp_n) / (pp_f - pp_n);
		ge.proj[15] = 1.0f;
	} else {
		ge.proj[0] = pp_a; ge.proj[5] = pp_b; ge.proj[10] = -(pp_f + pp_n) / (pp_f - pp_n); ge.proj[11] = -1.0f;
		ge.proj[14] = -2.0f * pp_f * pp_n / (pp_f - pp_n);
	}
}

static void p_ojo(float X, float Y, float w, float *e){   //Arreglo
	float xs = ge_f24(ge.cmd[GE_VIEWPORTXSCALE]), ys = ge_f24(ge.cmd[GE_VIEWPORTYSCALE]);
	float xc = ge_f24(ge.cmd[GE_VIEWPORTXCENTER]), yc = ge_f24(ge.cmd[GE_VIEWPORTYCENTER]);
	float sx = X + (float)(ge.cmd[GE_OFFSETX] & 0xFFFF) / 16.0f, sy = Y + (float)(ge.cmd[GE_OFFSETY] & 0xFFFF) / 16.0f;
	float nx = (sx - xc) / xs, ny = (sy - yc) / ys;
	if(pp_orto){ e[0] = nx / pp_a; e[1] = ny / pp_b; e[2] = -w; }
	else { e[0] = nx * w / pp_a; e[1] = ny * w / pp_b; e[2] = -w; }
}

static void p_borde_xy(int borde, float d, float *X, float *Y){   //Arreglo
	float x1 = (float)(ge.cmd[GE_SCISSOR1] & 0x3FF), y1 = (float)((ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF);
	float x2 = (float)((ge.cmd[GE_SCISSOR2] & 0x3FF) < (ge.cmd[GE_REGION2] & 0x3FF) ? (ge.cmd[GE_SCISSOR2] & 0x3FF) : (ge.cmd[GE_REGION2] & 0x3FF)) + 1.0f;
	float y2 = (float)(((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF) < ((ge.cmd[GE_REGION2] >> 10) & 0x3FF) ? ((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF) : ((ge.cmd[GE_REGION2] >> 10) & 0x3FF)) + 1.0f;
	*X = bfr(x1, x2);
	*Y = bfr(y1, y2);
	if(borde == 0) *X = x1 - d;
	else if(borde == 1) *X = x2 + d;
	else if(borde == 2) *Y = y1 - d;
	else if(borde == 3) *Y = y2 + d;
	else { *X = (borde & 1) ? x2 + d : x1 - d; *Y = (borde & 2) ? y2 + d : y1 - d; }
}

static void p_llamadas(const float *clo, const float *chi, float *mn, float *mx, int extremos){   //Arreglo
	static const u32 pr[3] = { GE_PRIM_TRIANGLES, GE_PRIM_TRIANGLE_STRIP, GE_PRIM_TRIANGLE_FAN };
	float v[24][3], r[3];
	u32 pos = 1 + brn(3), a;
	int n = 3 + (int)brn(16), i, k;
	for(i = 0; i < n; i++)
		for(k = 0; k < 3; k++) v[i][k] = bfr(clo[k], chi[k]);
	if(extremos)
		for(k = 0; k < 3; k++){ v[0][k] = clo[k]; v[1][k] = chi[k]; }
	for(i = 0; i < n; i++){
		p_leida(pos, v[i], r);
		for(k = 0; k < 3; k++){ if(r[k] < mn[k]) mn[k] = r[k]; if(r[k] > mx[k]) mx[k] = r[k]; }
	}
	a = p_escribir(pos, (const float (*)[3])v, n);
	r_dibujo((7u << 2) | (pos << 7), pr[brn(3)], a, n);
}

static u32 p_lista(int objetos){   //Arreglo
	int o;
	rl = R_LIST;
	rv = R_DATA;
	for(o = 0; o < objetos; o++){
		float e[3], cen[3], rad, mn[3], mx[3], clo[3], chi[3], X, Y, w;
		int tipo = (int)brn(3), d, nd = 1 + (int)brn(4), k;
		u32 caja, salto, cuerpo_ini;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		if(tipo == 0){
			w = pp_orto ? bfr(pp_n, pp_f) : bfr(pp_n * 0.6f, pp_f * 0.9f);
			p_borde_xy((int)brn(8), bfr(-20.0f, 80.0f), &X, &Y);
			p_ojo(X, Y, w, e);
			rad = (pp_orto ? 1.0f : w) * bfr(0.005f, 0.3f) / pp_a;
		} else if(tipo == 1){
			e[0] = (brn(2) ? 1.0f : -1.0f) * bfr(0.0f, 8.0f);
			e[1] = bfr(-3.0f, 3.0f);
			e[2] = bfr(-2.0f, 1.5f);
			rad = bfr(0.3f, 4.0f);
		} else {
			w = pp_orto ? bfr(pp_n, pp_f) : bfr(3.0f, 25.0f);
			p_borde_xy(8 + (int)brn(4), bfr(-200.0f, 0.0f), &X, &Y);
			p_ojo(X, Y, w, e);
			rad = (pp_orto ? 1.0f : w) * bfr(0.2f, 1.0f) / pp_a;
		}
		for(k = 0; k < 3; k++) cen[k] = e[k];
		p_mundo(cen[0], cen[1], cen[2], rad, bfr(0.0f, 6.28f));
		{
			u32 l0 = rl;
			rl += 9 * 4;
			cuerpo_ini = rl;
			rl = l0;
		}
		rl = cuerpo_ini;
		for(d = 0; d < nd; d++){
			float sub = tipo == 2 ? bfr(0.05f, 0.4f) : 1.0f;
			for(k = 0; k < 3; k++){
				float c0 = bfr(-1.0f + sub, 1.0f - sub);
				clo[k] = c0 - sub * bfr(0.2f, 1.0f);
				chi[k] = c0 + sub * bfr(0.2f, 1.0f);
				if(clo[k] < -1.0f) clo[k] = -1.0f;
				if(chi[k] > 0.99f) chi[k] = 0.99f;
			}
			p_llamadas(clo, chi, mn, mx, (int)brn(2));
		}
		if(brn(6) == 0){
			int e2 = (int)brn(3);
			mx[e2] = mn[e2] + (mx[e2] - mn[e2]) * 0.5f;
		} else if(brn(3) == 0)
			for(k = 0; k < 3; k++){ float g = (mx[k] - mn[k]) * bfr(0.0f, 0.5f); mn[k] -= g; mx[k] += g; }
		{
			u32 fin = rl, l1;
			caja = r_caja(mn, mx);
			l1 = cuerpo_ini - 9 * 4;
			rl = l1;
			rcmd(GE_VERTEXTYPE, 0x180);
			rbase(caja);
			rcmd(GE_VADDR, caja & 0xFFFFFF);
			rcmd(GE_BOUNDINGBOX, 8);
			salto = rl + 4;
			rbase(rl);
			rl += 4;
			while(rl < cuerpo_ini) rcmd(GE_NOP, 0);
			rl = fin;
			mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
		}
		if(!brn(5)){
			float l2[3] = { -0.3f, -0.3f, -0.3f }, h2[3] = { 0.3f, 0.3f, 0.3f }, m2[3], x2[3];
			u32 vt = (7u << 2) | (3u << 7), a = r_vertices(vt, 2 + (int)brn(5), l2, h2, m2, x2, 1);
			r_dibujo(vt, brn(2) ? GE_PRIM_CONTINUE : GE_PRIM_TRIANGLE_STRIP, a, 2 + (int)brn(4));
		}
	}
	return rl;
}

static void bbox_pantalla_azar(void){   //Arreglo
	static u8 vram_ref[0x200000];
	u32 lista, ronda, distintas = 0, prims_mal = 0, verif = 0, verif_mal = 0, cache_mal = 0, i;
	u32 pant = 0, dudosas = 0, cruzan = 0, salt = 0, ll_salt = 0, ll_prob = 0, orden_mal = 0, guarda = 0, g_salt = 0;   //Arreglo
	static GeStats st1;   //Arreglo
	u8 *tex = mem_ptr(R_TEX, 16 * 16 * 4), *vram = mem_ptr(0x04000000, 0x200000);
	printf("Cajas exactas en pantalla (ge_bbox_pantalla, ge_bbox_llamada): listas al azar alrededor de la pantalla y de la camara\n");
	for(i = 0; i < 16 * 16 * 4; i++) tex[i] = (u8)brnd();
	ge_hw = NULL;
	for(ronda = 0; ronda < 1200; ronda++){
		GeState ini;
		u32 prims0 = 0, drawn0 = 0;
		u8 rapido1 = 0;   //Arreglo
		int modo, prof = (int)brn(2), combo = (int)(ronda % 4);
		p_pantalla(prof);
		if(brn(2)){ ge.cmd[GE_CULLFACEENABLE] = 1; ge.cmd[GE_CULL] = brn(2); }
		memcpy(&ini, &ge, sizeof(ini));
		lista = p_lista(4 + (int)brn(12));
		for(modo = 0; modo < 5; modo++){   //Arreglo
			ge_fast_math = (int)((ronda >> 2) & 1);
			memcpy(&ge, &ini, sizeof(ge));
			ge_vertex_reset();
			ge_raster_reset();
			memset(vram, 0x5A, 0x200000);
			memset(&ge_stats, 0, sizeof(ge_stats));
			opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
			opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
			opt_on[OPT_GE_BBOX_RAPIDO] = (u8)(brn(3) != 0);
			opt_on[OPT_GE_BBOX_PANTALLA] = (u8)(combo != 1 && combo != 3);
			opt_on[OPT_GE_BBOX_LLAMADA] = (u8)(combo < 2);
			opt_on[OPT_GE_BBOX_ORDEN] = opt_on[OPT_GE_BBOX_AABB] = (u8)(modo == 4 || (((ronda >> 3) & 1) ^ (modo == 3)));   //Arreglo
			opt_on[OPT_GE_BBOX_GUARDA] = opt_on[OPT_GE_BBOX_TRIANGULOS] = (u8)(modo == 2 || modo == 4);   //Arreglo
			if(modo == 3) opt_on[OPT_GE_BBOX_RAPIDO] = rapido1;   //Arreglo
			ge_test_run_list(R_LIST, lista, NULL);
			if(modo == 3){   //Arreglo
				if(memcmp(&st1, &ge_stats, sizeof(st1)) && orden_mal++ < 8) printf("  ronda %u: cuentas distintas con ge_bbox_orden y ge_bbox_aabb\n", (unsigned)ronda);   //Arreglo
				if(memcmp(vram_ref, vram, sizeof(vram_ref)) && distintas++ < 8) printf("  ronda %u modo 3: imagen distinta\n", (unsigned)ronda);   //Arreglo
				continue;   //Arreglo
			}   //Arreglo
			if(modo == 0){
				memcpy(vram_ref, vram, sizeof(vram_ref));
				prims0 = ge_stats.primitives;
				drawn0 = ge_stats.tris_drawn;
				continue;
			}
			if(ge_stats.primitives != prims0 || ge_stats.tris_drawn != drawn0){
				if(prims_mal++ < 8) printf("  ronda %u modo %d: primitivas %u/%u\n", (unsigned)ronda, modo, (unsigned)ge_stats.primitives, (unsigned)prims0);
			}
			if(modo == 1){
				st1 = ge_stats;   //Arreglo
				rapido1 = opt_on[OPT_GE_BBOX_RAPIDO];   //Arreglo
				pant += ge_stats.bx_pantalla; dudosas += ge_stats.bx_dudosas; cruzan += ge_stats.bx_cruzan;
				salt += ge_stats.bx_saltadas; ll_salt += ge_stats.bx_ll_saltadas; ll_prob += ge_stats.bx_ll_probadas;
			} else if(modo == 4){ guarda += ge_stats.bx_guarda + ge_stats.bx_ll_guarda + ge_stats.bx_ll_tri; g_salt += ge_stats.bx_saltadas; }   //Arreglo
			else { verif += ge_stats.bx_verif; verif_mal += ge_stats.bx_verif_bad; cache_mal += ge_stats.bx_cache_mal; }   //Arreglo
			if(memcmp(vram_ref, vram, sizeof(vram_ref))){
				if(distintas++ < 8) printf("  ronda %u modo %d: imagen distinta\n", (unsigned)ronda, modo);
			}
		}
	}
	opt_on[OPT_GE_VERIF] = 0;
	opt_on[OPT_GE_BBOX_GUARDA] = opt_on[OPT_GE_BBOX_TRIANGULOS] = 1;   //Arreglo
	printf("  %u listas: cajas fuera de la pantalla %u, dudosas %u (%u cruzan la camara); "
	       "%u llamadas saltadas (%u por su caja, de %u probadas); %u imagenes y %u cuentas de primitivas distintas; "
	       "con ge_verificar %u comprobadas, %u distintas\n", (unsigned)ronda, (unsigned)pant, (unsigned)dudosas,
	       (unsigned)cruzan, (unsigned)salt, (unsigned)ll_salt, (unsigned)ll_prob, (unsigned)distintas, (unsigned)prims_mal,
	       (unsigned)verif, (unsigned)verif_mal);
	CHECK_EQ(distintas, 0u);
	CHECK_EQ(prims_mal, 0u);
	CHECK_EQ(verif_mal, 0u);
	CHECK_EQ(cache_mal, 0u);
	printf("  con y sin ge_bbox_orden y ge_bbox_aabb: %u rondas con cuentas distintas; con ge_bbox_guarda y ge_bbox_triangulos %u cajas y llamadas mas fuera, "
	       "%u llamadas saltadas\n", (unsigned)orden_mal, (unsigned)guarda, (unsigned)g_salt);   //Arreglo
	CHECK_EQ(orden_mal, 0u);   //Arreglo
	CHECK(pant > 10);
	CHECK(cruzan > 200);
	CHECK(ll_salt > 200);
	CHECK(verif > 500);
}

static void p_llamada_f(const float *lo, const float *hi, float *mn, float *mx){   //Arreglo
	static const u32 pr[3] = { GE_PRIM_TRIANGLES, GE_PRIM_TRIANGLE_STRIP, GE_PRIM_TRIANGLE_FAN };
	float v[24][3];
	int n = 3 + (int)brn(16), i, k;
	for(i = 0; i < n; i++)
		for(k = 0; k < 3; k++){
			v[i][k] = i < 2 ? (i ? hi[k] : lo[k]) : bfr(lo[k], hi[k]);
			if(v[i][k] < mn[k]) mn[k] = v[i][k];
			if(v[i][k] > mx[k]) mx[k] = v[i][k];
		}
	r_dibujo((7u << 2) | (3u << 7), pr[brn(3)], p_escribir(3, (const float (*)[3])v, n), n);
}

static float p_ndc_max(int eje){   //Arreglo
	float X1, Y1, X2, Y2, e1[3], e2[3];
	p_borde_xy(0, 0.0f, &X1, &Y1);
	p_borde_xy(1, 0.0f, &X2, &Y2);
	if(eje){ p_borde_xy(2, 0.0f, &X1, &Y1); p_borde_xy(3, 0.0f, &X2, &Y2); }
	p_ojo(X1, Y1, 1.0f, e1);
	p_ojo(X2, Y2, 1.0f, e2);
	e1[eje] = __builtin_fabsf(e1[eje]);
	e2[eje] = __builtin_fabsf(e2[eje]);
	return e1[eje] > e2[eje] ? e1[eje] : e2[eje];
}

static void bbox_pantalla_cruza(void){   //Arreglo
	static u8 vram_ref[0x200000];
	u8 *vram = mem_ptr(0x04000000, 0x200000);
	u32 ronda, mal = 0, distintas = 0, salt = 0, verif = 0, verif_mal = 0, rondas_pant = 0, rondas_prof = 0, visibles = 0;
	printf("Cajas exactas en pantalla: cajas finas en diagonal que pasan por la camara (la prueba de siempre las da por visibles)\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 800; ronda++){
		GeState ini;
		float mn[3], mx[3], lado = (ronda & 1) ? 1.0f : -1.0f, r, n, largo, del, ax[3], ay[3], az[3], m[12];
		u32 caja, salto, prims0 = 0, cuerpo, fin;
		int prof = (int)((ronda >> 1) & 1), eje = (int)((ronda >> 2) & 1), modo, k, d;
		p_pantalla(prof);
		pp_orto = 0;
		memset(ge.proj, 0, sizeof(ge.proj));
		ge.proj[0] = pp_a; ge.proj[5] = pp_b; ge.proj[10] = -(pp_f + pp_n) / (pp_f - pp_n); ge.proj[11] = -1.0f;
		ge.proj[14] = -2.0f * pp_f * pp_n / (pp_f - pp_n);
		n = pp_n;
		r = p_ndc_max(eje) * bfr(1.6f, 3.0f);
		largo = bfr(3.0f, 15.0f);
		del = n * bfr(0.002f, 0.02f);
		memset(ax, 0, sizeof(ax)); memset(ay, 0, sizeof(ay)); memset(az, 0, sizeof(az));
		ax[eje] = lado * largo * r / (float)__builtin_sqrt(1.0f + r * r);
		ax[2] = largo / (float)__builtin_sqrt(1.0f + r * r);
		ay[1 - eje] = del;
		az[eje] = del / (float)__builtin_sqrt(1.0f + r * r);
		az[2] = -lado * del * r / (float)__builtin_sqrt(1.0f + r * r);
		for(k = 0; k < 3; k++){ m[k] = ax[k]; m[3 + k] = ay[k]; m[6 + k] = az[k]; m[9 + k] = 0.0f; }
		rl = R_LIST;
		rv = R_DATA;
		rcmd(GE_WORLDMATRIXNUMBER, 0);
		for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(d = 0; d < 1 + (int)brn(3); d++){
			float lo[3], hi[3];
			lo[0] = bfr(-1.0f, -0.3f); hi[0] = bfr(0.3f, 1.0f);
			lo[1] = bfr(-1.0f, 0.0f); hi[1] = bfr(0.0f, 1.0f);
			lo[2] = bfr(-1.0f, 0.0f); hi[2] = bfr(0.0f, 1.0f);
			p_llamada_f(lo, hi, mn, mx);
		}
		fin = rl;
		caja = r_caja(mn, mx);
		rl = cuerpo - 6 * 4;
		rcmd(GE_VERTEXTYPE, 0x180);
		rbase(caja);
		rcmd(GE_VADDR, caja & 0xFFFFFF);
		rcmd(GE_BOUNDINGBOX, 8);
		salto = rl + 4;
		rbase(rl);
		rl = fin;
		mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
		memcpy(&ini, &ge, sizeof(ini));
		rondas_prof += (u32)prof;
		for(modo = 0; modo < 3; modo++){
			ge_fast_math = (int)((ronda >> 3) & 1);
			memcpy(&ge, &ini, sizeof(ge));
			ge_vertex_reset();
			ge_raster_reset();
			memset(vram, 0x5A, 0x200000);
			memset(&ge_stats, 0, sizeof(ge_stats));
			opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
			opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
			opt_on[OPT_GE_BBOX_RAPIDO] = 1;
			opt_on[OPT_GE_BBOX_PANTALLA] = 1;
			opt_on[OPT_GE_BBOX_LLAMADA] = 0;
			ge_test_run_list(R_LIST, rl, NULL);
			if(modo == 0){ prims0 = ge_stats.primitives; memcpy(vram_ref, vram, sizeof(vram_ref)); continue; }
			if(ge_stats.primitives != prims0 && mal++ < 8)
				printf("  ronda %u modo %d: primitivas %u/%u\n", (unsigned)ronda, modo, (unsigned)ge_stats.primitives, (unsigned)prims0);
			if(memcmp(vram_ref, vram, sizeof(vram_ref)) && distintas++ < 8) printf("  ronda %u modo %d: imagen distinta\n", (unsigned)ronda, modo);
			if(modo == 1){
				visibles += ge_stats.bx_cajas;
				salt += ge_stats.bx_saltadas;
				rondas_pant += prof && ge_stats.bx_pantalla != 0;
			} else { verif += ge_stats.bx_verif; verif_mal += ge_stats.bx_verif_bad; }
		}
	}
	opt_on[OPT_GE_VERIF] = 0;
	printf("  %u cajas (%u con recorte de profundidad, %u visibles para la prueba de siempre): fuera de la pantalla en %u, "
	       "%u llamadas saltadas; %u imagenes y %u cuentas distintas; con ge_verificar %u comprobadas, %u distintas\n", (unsigned)ronda,
	       (unsigned)rondas_prof, (unsigned)visibles, (unsigned)rondas_pant, (unsigned)salt, (unsigned)distintas, (unsigned)mal,
	       (unsigned)verif, (unsigned)verif_mal);
	CHECK_EQ(mal, 0u);
	CHECK_EQ(distintas, 0u);
	CHECK_EQ(verif_mal, 0u);
	CHECK(rondas_pant * 10 > rondas_prof * 8);
	CHECK(verif > 300);
}

static void bbox_pantalla_bordes(void){   //Arreglo
	static const int ds[16] = { -48, -20, -17, -16, -15, -8, -1, 0, 1, 8, 15, 16, 17, 20, 48, 160 };
	u32 ronda, mal = 0, saltadas = 0, lejos = 0, lejos_salt = 0, dibujados = 0;
	printf("Cajas exactas en pantalla: un vertice a pocos 1/16 de pixel de cada borde del scissor\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 400; ronda++){
		int borde = (int)(ronda % 4), di, prof = (int)brn(2), tras = (int)((ronda >> 2) & 1);
		p_pantalla(prof);
		for(di = 0; di < 16; di++){
			GeState ini;
			float X, Y, e[3][3], w = pp_orto ? bfr(pp_n + 1.0f, pp_f - 1.0f) : bfr(pp_n * 2.0f, pp_f * 0.5f), d = (float)ds[di] / 16.0f;
			float mn[3], mx[3], v[3][3];
			u32 a, caja, salto, prims0 = 0;
			int k, j, modo;
			for(j = 0; j < 3; j++){
				p_borde_xy(borde, j ? d + bfr(2.0f, 40.0f) : d, &X, &Y);
				if(j){
					if(borde < 2) Y += (float)(j == 1 ? -30 : 30);
					else X += (float)(j == 1 ? -30 : 30);
				}
				p_ojo(X + 1.0f / 32.0f, Y + 1.0f / 32.0f, w, e[j]);
			}
			rl = R_LIST;
			rv = R_DATA;
			{
				float m[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
				if(tras) for(k = 0; k < 3; k++) m[9 + k] = f24f(e[0][k]);
				rcmd(GE_WORLDMATRIXNUMBER, 0);
				for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
				for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
				for(j = 0; j < 3; j++)
					for(k = 0; k < 3; k++){
						v[j][k] = e[j][k] - m[9 + k];
						if(v[j][k] < mn[k]) mn[k] = v[j][k];
						if(v[j][k] > mx[k]) mx[k] = v[j][k];
					}
				if(!tras){
					float c[3];
					p_borde_xy(0, -200.0f, &X, &Y);
					p_ojo(X, Y, w, c);
					for(k = 0; k < 3; k++){ if(c[k] < mn[k]) mn[k] = c[k]; if(c[k] > mx[k]) mx[k] = c[k]; }
				}
			}
			a = p_escribir(3, (const float (*)[3])v, 3);
			caja = r_caja(mn, mx);
			rcmd(GE_VERTEXTYPE, 0x180);
			rbase(caja);
			rcmd(GE_VADDR, caja & 0xFFFFFF);
			rcmd(GE_BOUNDINGBOX, 8);
			salto = rl + 4;
			rbase(rl);
			rl += 4;
			r_dibujo((7u << 2) | (3u << 7), GE_PRIM_TRIANGLES, a, 3);
			mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
			memcpy(&ini, &ge, sizeof(ini));
			for(modo = 0; modo < 3; modo++){
				ge_fast_math = (int)(ronda & 1);
				memcpy(&ge, &ini, sizeof(ge));
				ge_vertex_reset();
				ge_raster_reset();
				memset(&ge_stats, 0, sizeof(ge_stats));
				opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
				opt_on[OPT_GE_VERIF] = 0;
				opt_on[OPT_GE_BBOX_RAPIDO] = (u8)((ronda >> 3) & 1);
				opt_on[OPT_GE_BBOX_PANTALLA] = 1;
				opt_on[OPT_GE_BBOX_LLAMADA] = (u8)(modo == 2);
				ge_test_run_list(R_LIST, rl, NULL);
				if(modo == 0){ prims0 = ge_stats.primitives; dibujados += prims0 != 0; continue; }
				if(ge_stats.primitives != prims0 && mal++ < 8)
					printf("  ronda %u borde %d a %d/16: primitivas %u/%u\n", (unsigned)ronda, borde, ds[di], (unsigned)ge_stats.primitives, (unsigned)prims0);
				saltadas += ge_stats.bx_saltadas;
				if(ds[di] == 160 && modo == 2 && !tras){ lejos++; lejos_salt += ge_stats.bx_saltadas; }
			}
		}
	}
	printf("  %u triangulos (%u dibujados): %u saltados, %u cuentas distintas; a 10 pixeles fuera, %u de %u saltados por su caja\n",
	       (unsigned)(ronda * 16), (unsigned)dibujados, (unsigned)saltadas, (unsigned)mal, (unsigned)lejos_salt, (unsigned)lejos);
	CHECK_EQ(mal, 0u);
	CHECK(dibujados > 1000);
	CHECK(saltadas > 1000);
	CHECK(lejos_salt * 10 > lejos * 9);
}

typedef struct { u32 rondas, mal, distintas, probadas, pant, salt, ll_salt, verif, verif_mal, dibujo0, cache_mal, orden_mal, guarda, g_salt, g_prob, nada, nada_ll, nada_s1, nada_s4, tri, t_prob; } PRes;   //Arreglo

static int p_correr(const char *que, u32 ronda, u32 fin, int llamada, PRes *r){   //Arreglo
	static u8 vram_ref[0x200000];
	u8 *vram = mem_ptr(0x04000000, 0x200000);
	GeState ini;
	static GeStats st1;   //Arreglo
	u32 prims0 = 0;
	int modo, salt = 0, nuevo = (int)(((ronda * 2654435761u) >> 30) & 1);   //Arreglo
	memcpy(&ini, &ge, sizeof(ini));
	r->rondas++;
	for(modo = 0; modo < 5; modo++){   //Arreglo
		memcpy(&ge, &ini, sizeof(ge));
		ge_vertex_reset();
		ge_raster_reset();
		memset(vram, 0x5A, 0x200000);
		memset(&ge_stats, 0, sizeof(ge_stats));
		opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
		opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
		opt_on[OPT_GE_BBOX_RAPIDO] = (u8)((ronda * 2654435761u) >> 31);
		opt_on[OPT_GE_BBOX_PANTALLA] = 1;
		opt_on[OPT_GE_BBOX_LLAMADA] = (u8)llamada;
		opt_on[OPT_GE_BBOX_ORDEN] = opt_on[OPT_GE_BBOX_AABB] = (u8)(modo == 4 || (nuevo ^ (modo == 3)));   //Arreglo
		opt_on[OPT_GE_BBOX_GUARDA] = opt_on[OPT_GE_BBOX_TRIANGULOS] = (u8)(modo == 2 || modo == 4);   //Arreglo
		ge_test_run_list(R_LIST, fin, NULL);
		if(modo == 3){   //Arreglo
			if(memcmp(&st1, &ge_stats, sizeof(st1)) && r->orden_mal++ < 8) printf("  %s ronda %u: cuentas distintas con ge_bbox_orden y ge_bbox_aabb\n", que, (unsigned)ronda);   //Arreglo
			if(memcmp(vram_ref, vram, sizeof(vram_ref)) && r->distintas++ < 8) printf("  %s ronda %u modo 3: imagen distinta\n", que, (unsigned)ronda);   //Arreglo
			continue;   //Arreglo
		}   //Arreglo
		if(modo == 0){
			prims0 = ge_stats.primitives;
			r->dibujo0 += prims0 != 0;
			r->nada += prims0 == 0;   //Arreglo
			r->nada_ll += prims0 == 0 ? ge_stats.draws : 0;   //Arreglo
			memcpy(vram_ref, vram, sizeof(vram_ref));
			continue;
		}
		if(ge_stats.primitives != prims0 && r->mal++ < 8)
			printf("  %s ronda %u modo %d: primitivas %u/%u\n", que, (unsigned)ronda, modo, (unsigned)ge_stats.primitives, (unsigned)prims0);
		if(memcmp(vram_ref, vram, sizeof(vram_ref)) && r->distintas++ < 8) printf("  %s ronda %u modo %d: imagen distinta\n", que, (unsigned)ronda, modo);
		if(modo == 1 && !prims0) r->nada_s1 += ge_stats.bx_saltadas;   //Arreglo
		if(modo == 4 && !prims0) r->nada_s4 += ge_stats.bx_saltadas;   //Arreglo
		if(modo == 1){
			st1 = ge_stats;   //Arreglo
			r->probadas += ge_stats.bx_cajas != 0;
			r->pant += ge_stats.bx_pantalla != 0;
			r->salt += ge_stats.bx_saltadas;
			r->ll_salt += ge_stats.bx_ll_saltadas;
			salt = ge_stats.bx_saltadas != 0;
		} else if(modo == 4){   //Arreglo
			r->guarda += ge_stats.bx_guarda + ge_stats.bx_ll_guarda;   //Arreglo
			r->g_salt += ge_stats.bx_saltadas;   //Arreglo
			r->g_prob += ge_stats.bx_g_probadas;   //Arreglo
			r->tri += ge_stats.bx_ll_tri;   //Arreglo
			r->t_prob += ge_stats.bx_t_probadas;   //Arreglo
		} else { r->verif += ge_stats.bx_verif; r->verif_mal += ge_stats.bx_verif_bad; r->cache_mal += ge_stats.bx_cache_mal; }   //Arreglo
	}
	opt_on[OPT_GE_VERIF] = 0;
	opt_on[OPT_GE_BBOX_GUARDA] = opt_on[OPT_GE_BBOX_TRIANGULOS] = 1;   //Arreglo
	ge = ini;
	return salt;
}

static void p_perspectiva(void){   //Arreglo
	pp_orto = 0;
	memset(ge.proj, 0, sizeof(ge.proj));
	ge.proj[0] = pp_a; ge.proj[5] = pp_b; ge.proj[10] = -(pp_f + pp_n) / (pp_f - pp_n); ge.proj[11] = -1.0f;
	ge.proj[14] = -2.0f * pp_f * pp_n / (pp_f - pp_n);
}

static void p_mundo_m(const float *m){   //Arreglo
	int k;
	rcmd(GE_WORLDMATRIXNUMBER, 0);
	for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
}

static u32 p_objeto(u32 cuerpo, const float *mn, const float *mx){   //Arreglo
	u32 fin = rl, caja = r_caja(mn, mx), salto;
	rl = cuerpo - 6 * 4;
	rcmd(GE_VERTEXTYPE, 0x180);
	rbase(caja);
	rcmd(GE_VADDR, caja & 0xFFFFFF);
	rcmd(GE_BOUNDINGBOX, 8);
	salto = rl + 4;
	rbase(rl);
	mem_write32(salto, (GE_BJUMP << 24) | (fin & 0xFFFFFF));
	rl = fin;
	return fin;
}

static void p_pantalla_fina(int prof){   //Arreglo
	int x1 = 2 + (int)brn(4), y1 = 2 + (int)brn(4), x2 = x1 + 4 + (int)brn(2), y2 = y1 + 4 + (int)brn(2), i;
	float ox = (float)(16 + brn(32)), oy = (float)(16 + brn(32));
	estado_pantalla(prof);
	ge.cmd[GE_OFFSETX] = (u32)ox;
	ge.cmd[GE_OFFSETY] = (u32)oy;
	ge.cmd[GE_SCISSOR1] = (u32)(x1 | (y1 << 10));
	ge.cmd[GE_SCISSOR2] = (u32)(x2 | (y2 << 10));
	ge.cmd[GE_REGION2] = brn(2) ? ge.cmd[GE_SCISSOR2] : (u32)((x2 + (int)brn(3) - 1) | ((y2 + (int)brn(3) - 1) << 10));
	ge.cmd[GE_VIEWPORTXSCALE] = f24(bfr(6.0f, 10.0f) * (brn(2) ? 1.0f : -1.0f));
	ge.cmd[GE_VIEWPORTYSCALE] = f24(bfr(6.0f, 10.0f) * (brn(2) ? 1.0f : -1.0f));
	ge.cmd[GE_VIEWPORTXCENTER] = f24((float)(x1 + x2 + 1) * 0.5f + ox / 16.0f + bfr(-0.5f, 0.5f));
	ge.cmd[GE_VIEWPORTYCENTER] = f24((float)(y1 + y2 + 1) * 0.5f + oy / 16.0f + bfr(-0.5f, 0.5f));
	for(i = 0; i < 12; i++) ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	pp_n = bfr(0.2f, 2.0f);
	pp_f = bfr(30.0f, 150.0f);
	pp_a = bfr(0.7f, 2.0f);
	pp_b = pp_a * bfr(1.2f, 2.0f);
	p_perspectiva();
	if(brn(4) == 0){
		pp_orto = 1;
		pp_a *= 0.1f; pp_b *= 0.1f;
		memset(ge.proj, 0, sizeof(ge.proj));
		ge.proj[0] = pp_a; ge.proj[5] = pp_b; ge.proj[10] = -2.0f / (pp_f - pp_n); ge.proj[14] = -(pp_f + pp_n) / (pp_f - pp_n);
		ge.proj[15] = 1.0f;
	}
}

static void p_cambio_pantalla(int borde, int tipo){   //Arreglo
	const int ex = borde < 2, sg = (borde & 1) ? -1 : 1;
	const u32 c = ge.cmd[GE_SCISSOR1], s2 = ge.cmd[GE_SCISSOR2], r2 = ge.cmd[GE_REGION2], sh = ex ? 0 : 10;
	switch(tipo){
	case 0:
		if(ex) rcmd(GE_OFFSETX, ge.cmd[GE_OFFSETX] - (u32)(sg * 16));
		else rcmd(GE_OFFSETY, ge.cmd[GE_OFFSETY] - (u32)(sg * 16));
		break;
	case 1:
		if(sg > 0) rcmd(GE_SCISSOR1, c - (1u << sh));
		else { rcmd(GE_SCISSOR2, s2 + (1u << sh)); rcmd(GE_REGION2, r2 + (1u << sh)); }
		break;
	case 2:
		if(ex) rcmd(GE_VIEWPORTXCENTER, f24(ge_f24(ge.cmd[GE_VIEWPORTXCENTER]) + (float)sg));
		else rcmd(GE_VIEWPORTYCENTER, f24(ge_f24(ge.cmd[GE_VIEWPORTYCENTER]) + (float)sg));
		break;
	case 3:
		if(ex) rcmd(GE_VIEWPORTXSCALE, f24(ge_f24(ge.cmd[GE_VIEWPORTXSCALE]) * 0.7f));
		else rcmd(GE_VIEWPORTYSCALE, f24(ge_f24(ge.cmd[GE_VIEWPORTYSCALE]) * 0.7f));
		break;
	default:
		rcmd(GE_DEPTHCLAMPENABLE, ge.cmd[GE_DEPTHCLAMPENABLE] ^ 1);
		break;
	}
}

static void bbox_pantalla_fino(void){   //Arreglo
	static const int ds[6] = { -2, -1, 0, 1, 2, 4 };
	PRes r;
	u32 ronda, primero = 0, primero_salt = 0, cambio_n[5] = { 0 }, cambio_vis[5] = { 0 }, dentro_salt = 0;
	int j;
	memset(&r, 0, sizeof(r));
	printf("Cajas exactas en pantalla: umbral exacto, con el centro del viewport junto al borde (margen < 1/32 de pixel)\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 2400; ronda++){
		int borde = (int)(ronda % 4), di = (int)((ronda / 4) % 6), prof = (int)brn(2), cambio = brn(2) ? (int)brn(5) : -1, k, s;
		int pri = (borde & 1) ? 0 : 1;
		float X, Y, e[3][3], mn[3], mx[3], d = (float)ds[di] / 16.0f, w;
		u32 cuerpo, a;
		p_pantalla_fina(prof);
		w = pp_orto ? bfr(pp_n + 1.0f, pp_f - 1.0f) : bfr(pp_n * 2.0f, pp_f * 0.5f);
		for(j = 0; j < 3; j++){
			float dj = j ? d + bfr(0.2f, 0.6f) : d;
			p_borde_xy(borde, dj, &X, &Y);
			if(j){
				if(borde < 2) Y += (j == 1 ? -1.0f : 1.0f) * bfr(0.5f, 1.5f);
				else X += (j == 1 ? -1.0f : 1.0f) * bfr(0.5f, 1.5f);
			}
			p_ojo(X + 1.0f / 32.0f, Y + 1.0f / 32.0f, w, e[j]);
		}
		rl = R_LIST;
		rv = R_DATA;
		p_mundo_m((const float[12]){ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 });
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(j = 0; j < 3; j++)
			for(k = 0; k < 3; k++){ if(e[j][k] < mn[k]) mn[k] = e[j][k]; if(e[j][k] > mx[k]) mx[k] = e[j][k]; }
		a = p_escribir(3, (const float (*)[3])e, 3);
		if(cambio >= 0) p_cambio_pantalla(borde, cambio);
		r_dibujo((7u << 2) | (3u << 7), GE_PRIM_TRIANGLES, a, 3);
		p_objeto(cuerpo, mn, mx);
		ge_fast_math = (int)((ronda >> 3) & 1);
		{
			u32 d0 = r.dibujo0, p0 = r.probadas;
			s = p_correr("fino", ronda, rl, (int)brn(2), &r);
			if(cambio >= 0){ cambio_n[cambio]++; cambio_vis[cambio] += r.dibujo0 != d0; }
			else if(ds[di] == pri && r.probadas != p0){ primero++; primero_salt += (u32)s; }
			if(cambio < 0 && ds[di] < pri) dentro_salt += (u32)s;
		}
	}
	printf("  %u triangulos (%u probados por la caja, %u dibujados): a 1/32 de pixel fuera, %u de %u saltados; por dentro, %u saltados; "
	       "cambiando la pantalla entre la caja y la llamada (offset, scissor, centro, escala, recorte) %u/%u, %u/%u, %u/%u, %u/%u, %u/%u dibujan; "
	       "%u cuentas y %u imagenes distintas; con ge_verificar %u comprobadas, %u distintas\n",
	       (unsigned)r.rondas, (unsigned)r.probadas, (unsigned)r.dibujo0, (unsigned)primero_salt, (unsigned)primero, (unsigned)dentro_salt,
	       (unsigned)cambio_vis[0], (unsigned)cambio_n[0], (unsigned)cambio_vis[1], (unsigned)cambio_n[1], (unsigned)cambio_vis[2], (unsigned)cambio_n[2],
	       (unsigned)cambio_vis[3], (unsigned)cambio_n[3], (unsigned)cambio_vis[4], (unsigned)cambio_n[4],
	       (unsigned)r.mal, (unsigned)r.distintas, (unsigned)r.verif, (unsigned)r.verif_mal);
	CHECK_EQ(r.mal, 0u);
	CHECK_EQ(r.distintas, 0u);
	CHECK_EQ(r.verif_mal, 0u);
	CHECK_EQ(r.cache_mal, 0u);
	printf("  con y sin ge_bbox_orden y ge_bbox_aabb: %u rondas con cuentas distintas\n", (unsigned)r.orden_mal);   //Arreglo
	CHECK_EQ(r.orden_mal, 0u);   //Arreglo
	CHECK_EQ(dentro_salt, 0u);
	CHECK(primero > 100);
	CHECK(primero_salt * 10 > primero * 9);
	for(j = 0; j < 4; j++) CHECK(cambio_vis[j] * 2 > cambio_n[j]);
}

static void bbox_pantalla_cercano(void){   //Arreglo
	PRes rp, rs, rc;
	u32 ronda;
	memset(&rp, 0, sizeof(rp)); memset(&rs, 0, sizeof(rs)); memset(&rc, 0, sizeof(rc));
	printf("Cajas exactas en pantalla: cajas en diagonal que cruzan el plano cercano, dentro de la pantalla solo por detras de el\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 1200; ronda++){
		float lado = (ronda & 1) ? 1.0f : -1.0f, n, lim, w0, w1, q0, q1, al, be, p0[3], p1[3], u[3], m[12], mn[3], mx[3], pn, del, lu;
		int prof = (int)((ronda >> 1) & 1), eje = (int)((ronda >> 2) & 1), cambio = prof && brn(2), k, d;
		u32 cuerpo;
		p_pantalla(prof);
		p_perspectiva();
		ge.cmd[GE_VIEWPORTZSCALE] = f24(-bfr(5000.0f, 9000.0f));
		ge.cmd[GE_VIEWPORTZCENTER] = f24(bfr(15000.0f, 20000.0f));
		n = pp_n;
		lim = p_ndc_max(eje);
		w0 = n * bfr(0.35f, 0.8f);
		w1 = n * bfr(1.5f, 4.0f);
		q0 = lim * bfr(-0.3f, 0.3f);
		q1 = lado * lim * bfr(1.15f, 1.6f);
		al = (q0 - q1) / (1.0f / w0 - 1.0f / n);
		be = q1 - al / n;
		memset(p0, 0, sizeof(p0)); memset(p1, 0, sizeof(p1));
		p0[eje] = al + be * w0; p0[2] = -w0;
		p1[eje] = al + be * w1; p1[2] = -w1;
		for(k = 0; k < 3; k++){ u[k] = (p1[k] - p0[k]) * 0.5f; m[9 + k] = (p0[k] + p1[k]) * 0.5f; }
		lu = (float)__builtin_sqrt(u[eje] * u[eje] + u[2] * u[2]);
		del = n * bfr(0.002f, 0.01f);
		for(k = 0; k < 3; k++){ m[k] = u[k]; m[3 + k] = 0.0f; m[6 + k] = 0.0f; }
		m[3 + (1 - eje)] = del;
		m[6 + eje] = u[2] / lu * del;
		m[6 + 2] = -u[eje] / lu * del;
		pn = 2.0f * (n - w0) / (w1 - w0) - 1.0f;
		rl = R_LIST;
		rv = R_DATA;
		p_mundo_m(m);
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		if(cambio) rcmd(GE_DEPTHCLAMPENABLE, 0);
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(d = 0; d < 1 + (int)brn(3); d++){
			float lo[3], hi[3];
			lo[0] = bfr(-1.0f, pn - 0.1f); hi[0] = bfr(pn + 0.1f, 1.0f);
			lo[1] = bfr(-1.0f, 0.0f); hi[1] = bfr(0.0f, 1.0f);
			lo[2] = bfr(-1.0f, 0.0f); hi[2] = bfr(0.0f, 1.0f);
			p_llamada_f(lo, hi, mn, mx);
		}
		p_objeto(cuerpo, mn, mx);
		ge_fast_math = (int)((ronda >> 3) & 1);
		p_correr("cercano", ronda, rl, (int)brn(2), cambio ? &rc : prof ? &rp : &rs);
	}
	printf("  con recorte de profundidad: %u cajas (%u probadas), fuera de la pantalla %u, %u llamadas saltadas; sin recorte: %u cajas, "
	       "%u dibujan algo, %u saltadas; quitando el recorte entre la caja y la llamada: %u cajas, %u dibujan algo, %u saltadas; "
	       "%u cuentas y %u imagenes distintas; con ge_verificar %u comprobadas, %u distintas\n",
	       (unsigned)rp.rondas, (unsigned)rp.probadas, (unsigned)rp.pant, (unsigned)rp.salt, (unsigned)rs.rondas, (unsigned)rs.dibujo0,
	       (unsigned)rs.salt, (unsigned)rc.rondas, (unsigned)rc.dibujo0, (unsigned)rc.salt, (unsigned)(rp.mal + rs.mal + rc.mal),
	       (unsigned)(rp.distintas + rs.distintas + rc.distintas), (unsigned)(rp.verif + rs.verif + rc.verif),
	       (unsigned)(rp.verif_mal + rs.verif_mal + rc.verif_mal));
	CHECK_EQ(rp.mal + rs.mal + rc.mal, 0u);
	CHECK_EQ(rp.distintas + rs.distintas + rc.distintas, 0u);
	CHECK_EQ(rp.verif_mal + rs.verif_mal + rc.verif_mal, 0u);
	CHECK_EQ(rp.cache_mal + rs.cache_mal + rc.cache_mal, 0u);
	printf("  con y sin ge_bbox_orden y ge_bbox_aabb: %u rondas con cuentas distintas\n", (unsigned)(rp.orden_mal + rs.orden_mal + rc.orden_mal));   //Arreglo
	CHECK_EQ(rp.orden_mal + rs.orden_mal + rc.orden_mal, 0u);   //Arreglo
	CHECK(rp.probadas * 10 > rp.rondas * 6);
	CHECK(rp.pant * 10 > rp.probadas * 9);
	CHECK(rs.dibujo0 * 2 > rs.rondas);
	CHECK(rc.dibujo0 * 2 > rc.rondas);
}

static void p_recta(const float *pa, const float *pb, float del, float *m){   //Arreglo
	float u[3], lu, e1[3], e2[3], ax[3] = { 0.0f, 0.0f, 1.0f }, le;
	int k;
	for(k = 0; k < 3; k++){ u[k] = (pb[k] - pa[k]) * 0.5f; m[9 + k] = (pa[k] + pb[k]) * 0.5f; }
	lu = (float)__builtin_sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	if(__builtin_fabsf(u[2]) > 0.9f * lu){ ax[0] = 1.0f; ax[2] = 0.0f; }
	e1[0] = u[1] * ax[2] - u[2] * ax[1]; e1[1] = u[2] * ax[0] - u[0] * ax[2]; e1[2] = u[0] * ax[1] - u[1] * ax[0];
	le = (float)__builtin_sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
	for(k = 0; k < 3; k++) e1[k] /= le;
	e2[0] = (u[1] * e1[2] - u[2] * e1[1]) / lu; e2[1] = (u[2] * e1[0] - u[0] * e1[2]) / lu; e2[2] = (u[0] * e1[1] - u[1] * e1[0]) / lu;
	for(k = 0; k < 3; k++){ m[k] = u[k]; m[3 + k] = e1[k] * del; m[6 + k] = e2[k] * del; }
}

static void p_ojo_sx(float sx, float sy, float w, float *e){   //Arreglo
	float xs = ge_f24(ge.cmd[GE_VIEWPORTXSCALE]), ys = ge_f24(ge.cmd[GE_VIEWPORTYSCALE]);
	float xc = ge_f24(ge.cmd[GE_VIEWPORTXCENTER]), yc = ge_f24(ge.cmd[GE_VIEWPORTYCENTER]);
	e[0] = (sx - xc) / xs * w / pp_a;
	e[1] = (sy - yc) / ys * w / pp_b;
	e[2] = -w;
}

static void bbox_triangulos_limites(void){   //Arreglo
	static const float ds[12] = { -48.0f, -20.0f, -3.0f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 3.0f, 20.0f, 48.0f, 2560.0f };
	u32 ronda, mal = 0, dibujados = 0, saltadas = 0, lejos = 0, lejos_salt = 0, probadas = 0;
	printf("Repaso por triangulos (ge_bbox_triangulos): un vertice junto al limite de la banda, del rango z, del plano cercano o de w = 0\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 1600; ronda++){
		const int tipo = (int)(ronda % 8);
		int prof = tipo == 6 ? 1 : tipo == 4 || tipo == 5 ? 0 : (int)((ronda >> 3) & 1), di, j, k, modo;
		float zs, zc, X0, Y0, sx1, sx2, sy1, sy2;
		p_pantalla(prof);
		p_perspectiva();
		zs = ge_f24(ge.cmd[GE_VIEWPORTZSCALE]); zc = ge_f24(ge.cmd[GE_VIEWPORTZCENTER]);
		X0 = (float)(ge.cmd[GE_OFFSETX] & 0xFFFF) / 16.0f; Y0 = (float)(ge.cmd[GE_OFFSETY] & 0xFFFF) / 16.0f;
		sx1 = X0 + (float)(ge.cmd[GE_SCISSOR1] & 0x3FF) + 4.0f; sx2 = X0 + (float)(ge.cmd[GE_SCISSOR2] & 0x3FF) - 4.0f;
		sy1 = Y0 + (float)((ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF) + 4.0f; sy2 = Y0 + (float)((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF) - 4.0f;
		if(sx2 > X0 + (float)(ge.cmd[GE_REGION2] & 0x3FF) - 4.0f) sx2 = X0 + (float)(ge.cmd[GE_REGION2] & 0x3FF) - 4.0f;
		if(sy2 > Y0 + (float)((ge.cmd[GE_REGION2] >> 10) & 0x3FF) - 4.0f) sy2 = Y0 + (float)((ge.cmd[GE_REGION2] >> 10) & 0x3FF) - 4.0f;
		for(di = 0; di < 12; di++){
			GeState ini;
			float v[3][3], mn[3], mx[3], d = ds[di], w0 = bfr(pp_n * 1.5f, pp_f * 0.5f);
			u32 a, caja, salto, prims0 = 0;
			for(j = 1; j < 3; j++) p_ojo_sx(bfr(sx1, sx2), bfr(sy1, sy2), bfr(pp_n * 1.5f, pp_f * 0.5f), v[j]);
			if(tipo < 4){
				const float lim = (tipo & 1) ? 4095.0f + 15.5f / 16.0f : 0.0f, sig = (tipo & 1) ? 1.0f : -1.0f;
				if(tipo < 2) p_ojo_sx(lim + sig * d / 16.0f, bfr(sy1, sy2), w0, v[0]);
				else p_ojo_sx(bfr(sx1, sx2), lim + sig * d / 16.0f, w0, v[0]);
			} else if(tipo < 6){
				const float sz = tipo == 4 ? -d : 65536.0f + d, q = (sz - zc) / zs;
				const float ze = -ge.proj[14] / (ge.proj[10] + q);
				p_ojo_sx(bfr(sx1, sx2), bfr(sy1, sy2), -ze, v[0]);
			} else if(tipo == 6){
				const float dl = d * (1.0f / 4096.0f) * w0, ze = (dl - ge.proj[14]) / (ge.proj[10] - 1.0f);
				p_ojo_sx(bfr(sx1, sx2), bfr(sy1, sy2), -ze, v[0]);
			} else {
				const float wz = d * (1.0f / 2048.0f) * pp_n;
				v[0][0] = bfr(-1.0f, 1.0f); v[0][1] = bfr(-1.0f, 1.0f); v[0][2] = -wz;
			}
			for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
			for(j = 0; j < 3; j++)
				for(k = 0; k < 3; k++){ if(v[j][k] < mn[k]) mn[k] = v[j][k]; if(v[j][k] > mx[k]) mx[k] = v[j][k]; }
			rl = R_LIST;
			rv = R_DATA;
			{
				static const float m[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
				rcmd(GE_WORLDMATRIXNUMBER, 0);
				for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
			}
			a = p_escribir(3, (const float (*)[3])v, 3);
			caja = r_caja(mn, mx);
			rcmd(GE_VERTEXTYPE, 0x180);
			rbase(caja);
			rcmd(GE_VADDR, caja & 0xFFFFFF);
			rcmd(GE_BOUNDINGBOX, 8);
			salto = rl + 4;
			rbase(rl);
			rl += 4;
			r_dibujo((7u << 2) | (3u << 7), brn(2) ? GE_PRIM_TRIANGLES : GE_PRIM_TRIANGLE_STRIP, a, 3);
			mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
			memcpy(&ini, &ge, sizeof(ini));
			for(modo = 0; modo < 3; modo++){
				ge_fast_math = (int)((ronda >> 4) & 1);
				memcpy(&ge, &ini, sizeof(ge));
				ge_vertex_reset();
				ge_raster_reset();
				memset(&ge_stats, 0, sizeof(ge_stats));
				opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
				opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
				opt_on[OPT_GE_BBOX_RAPIDO] = (u8)((ronda >> 5) & 1);
				opt_on[OPT_GE_BBOX_PANTALLA] = opt_on[OPT_GE_BBOX_LLAMADA] = opt_on[OPT_GE_BBOX_ORDEN] = 1;
				opt_on[OPT_GE_BBOX_GUARDA] = opt_on[OPT_GE_BBOX_TRIANGULOS] = 1;
				ge_test_run_list(R_LIST, rl, NULL);
				if(modo == 0){ prims0 = ge_stats.primitives; dibujados += prims0 != 0; continue; }
				if((ge_stats.primitives != prims0 || ge_stats.bx_verif_bad) && mal++ < 8)
					printf("  ronda %u tipo %d a %g: primitivas %u/%u, verificacion %u\n", (unsigned)ronda, tipo, (double)d,
					       (unsigned)ge_stats.primitives, (unsigned)prims0, (unsigned)ge_stats.bx_verif_bad);
				if(modo == 1){
					saltadas += ge_stats.bx_ll_tri;
					probadas += ge_stats.bx_t_probadas;
					if(di == 11 && tipo < 6){ lejos++; lejos_salt += ge_stats.bx_saltadas; }
				}
			}
			opt_on[OPT_GE_VERIF] = 0;
		}
	}
	printf("  %u triangulos (%u dibujados): %u probados por sus triangulos, %u saltados; %u cuentas distintas; "
	       "con el vertice a 2560 fuera, %u de %u saltados\n", (unsigned)(ronda * 12), (unsigned)dibujados, (unsigned)probadas,
	       (unsigned)saltadas, (unsigned)mal, (unsigned)lejos_salt, (unsigned)lejos);
	CHECK_EQ(mal, 0u);
	CHECK(dibujados > 4000);
	CHECK(saltadas > 1000);
	CHECK(lejos_salt * 10 > lejos * 9);
}

static void bbox_guarda_z(void){   //Arreglo
	static u8 vram_ref[0x200000];
	u8 *vram = mem_ptr(0x04000000, 0x200000);
	u32 ronda, mal = 0, cajas = 0, llamadas = 0, cambio_dib = 0, cambio_salt = 0, nada = 0;
	printf("Banda de guarda (ge_bbox_guarda): sin recorte, con una escala z que deja fuera de su rango lo mas cercano del frustum, "
	       "y la escala z cambiada entre la caja y las llamadas\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 400; ronda++){
		GeState ini;
		float v[3][3], mn[3], mx[3], w = pp_n, cx, cy;
		u32 a, caja, salto, prims0 = 0;
		int j, k, modo, cambio = (int)(ronda & 1);
		p_pantalla(0);
		p_perspectiva();
		ge.cmd[GE_VIEWPORTZSCALE] = f24(-60000.0f);
		ge.cmd[GE_VIEWPORTZCENTER] = f24(32000.0f);
		{
			const float q = bfr(-0.95f, -0.65f);
			w = ge.proj[14] / (ge.proj[10] + q);
		}
		cx = bfr(-0.3f, 0.3f); cy = bfr(-0.3f, 0.3f);
		for(j = 0; j < 3; j++){
			v[j][0] = (cx + (j == 1 ? 0.2f : 0.0f)) * w / pp_a;
			v[j][1] = (cy + (j == 2 ? 0.2f : 0.0f)) * w / pp_b;
			v[j][2] = -w * bfr(0.995f, 1.005f);
		}
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(j = 0; j < 3; j++)
			for(k = 0; k < 3; k++){ if(v[j][k] < mn[k]) mn[k] = v[j][k]; if(v[j][k] > mx[k]) mx[k] = v[j][k]; }
		rl = R_LIST;
		rv = R_DATA;
		{
			static const float m[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
			rcmd(GE_WORLDMATRIXNUMBER, 0);
			for(k = 0; k < 12; k++) rcmd(GE_WORLDMATRIXDATA, f24(m[k]));
		}
		a = p_escribir(3, (const float (*)[3])v, 3);
		caja = r_caja(mn, mx);
		rcmd(GE_VERTEXTYPE, 0x180);
		rbase(caja);
		rcmd(GE_VADDR, caja & 0xFFFFFF);
		rcmd(GE_BOUNDINGBOX, 8);
		salto = rl + 4;
		rbase(rl);
		rl += 4;
		if(cambio){
			rcmd(GE_VIEWPORTZSCALE, f24(-32000.0f));
			rcmd(GE_VIEWPORTZCENTER, f24(32000.0f));
		}
		r_dibujo((7u << 2) | (3u << 7), GE_PRIM_TRIANGLES, a, 3);
		mem_write32(salto, (GE_BJUMP << 24) | (rl & 0xFFFFFF));
		memcpy(&ini, &ge, sizeof(ini));
		for(modo = 0; modo < 3; modo++){
			ge_fast_math = (int)((ronda >> 1) & 1);
			memcpy(&ge, &ini, sizeof(ge));
			ge_vertex_reset();
			ge_raster_reset();
			memset(vram, 0x5A, 0x200000);
			memset(&ge_stats, 0, sizeof(ge_stats));
			opt_on[OPT_GE_BBOX] = (u8)(modo != 0);
			opt_on[OPT_GE_VERIF] = (u8)(modo == 2);
			opt_on[OPT_GE_BBOX_RAPIDO] = (u8)((ronda >> 2) & 1);
			opt_on[OPT_GE_BBOX_PANTALLA] = opt_on[OPT_GE_BBOX_ORDEN] = opt_on[OPT_GE_BBOX_GUARDA] = 1;
			opt_on[OPT_GE_BBOX_LLAMADA] = (u8)((ronda >> 3) & 1);
			opt_on[OPT_GE_BBOX_TRIANGULOS] = 0;
			ge_test_run_list(R_LIST, rl, NULL);
			if(modo == 0){
				prims0 = ge_stats.primitives;
				memcpy(vram_ref, vram, sizeof(vram_ref));
				nada += !cambio && !prims0;
				cambio_dib += cambio && prims0;
				continue;
			}
			if((ge_stats.primitives != prims0 || ge_stats.bx_verif_bad || memcmp(vram_ref, vram, sizeof(vram_ref))) && mal++ < 8)
				printf("  ronda %u modo %d: primitivas %u/%u\n", (unsigned)ronda, modo, (unsigned)ge_stats.primitives, (unsigned)prims0);
			if(modo == 1){
				cajas += ge_stats.bx_guarda;
				llamadas += !cambio && ge_stats.bx_saltadas;
				cambio_salt += cambio && ge_stats.bx_saltadas;
			}
		}
		opt_on[OPT_GE_VERIF] = 0;
	}
	opt_on[OPT_GE_BBOX_TRIANGULOS] = 1;
	printf("  %u objetos sin dibujar nada: %u cajas fuera por la banda, %u con sus llamadas saltadas; con la escala z cambiada, "
	       "%u dibujan y %u se saltan; %u cuentas e imagenes distintas\n", (unsigned)nada, (unsigned)cajas, (unsigned)llamadas,
	       (unsigned)cambio_dib, (unsigned)cambio_salt, (unsigned)mal);
	CHECK_EQ(mal, 0u);
	CHECK(nada > 150);
	CHECK(llamadas * 10 > nada * 9);
	CHECK(cambio_dib > 150);
	CHECK_EQ(cambio_salt, 0u);
}

static void bbox_guarda_vuelta(void){   //Arreglo
	PRes rp, rs;
	u32 ronda;
	memset(&rp, 0, sizeof(rp)); memset(&rs, 0, sizeof(rs));
	printf("Banda de guarda (ge_bbox_guarda): objetos largos desde muy fuera de la banda hasta cerca de la pantalla\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 1200; ronda++){
		float pa[3], pb[3], m[12], v[24][3], mn[3], mx[3], sxa, sya, sxb, syb, L, R, T, B, X0, Y0, xc, yc, del;
		int prof = (int)(ronda & 1), lado = (int)brn(4), nv = 4 + 2 * (int)brn(7), i, k;
		u32 cuerpo;
		p_pantalla(prof);
		p_perspectiva();
		xc = ge_f24(ge.cmd[GE_VIEWPORTXCENTER]); yc = ge_f24(ge.cmd[GE_VIEWPORTYCENTER]);
		X0 = (float)(ge.cmd[GE_OFFSETX] & 0xFFFF) / 16.0f; Y0 = (float)(ge.cmd[GE_OFFSETY] & 0xFFFF) / 16.0f;
		L = X0 + (float)(ge.cmd[GE_SCISSOR1] & 0x3FF); R = X0 + (float)(ge.cmd[GE_SCISSOR2] & 0x3FF);
		T = Y0 + (float)((ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF); B = Y0 + (float)((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF);
		if(lado < 2){ sxa = xc + (lado ? 1.0f : -1.0f) * bfr(3000.0f, 9000.0f); sya = bfr(T, B); }
		else { sya = yc + (lado == 3 ? 1.0f : -1.0f) * bfr(3000.0f, 9000.0f); sxa = bfr(L, R); }
		sxb = bfr(L - 400.0f, R + 400.0f);
		syb = bfr(T - 400.0f, B + 400.0f);
		p_ojo_sx(sxa, sya, bfr(pp_n * 1.5f, pp_f * 0.5f), pa);
		p_ojo_sx(sxb, syb, bfr(pp_n * 1.5f, pp_f * 0.5f), pb);
		del = bfr(0.0005f, 0.004f) * (float)__builtin_sqrt(pb[0] * pb[0] + pb[1] * pb[1] + pb[2] * pb[2]);
		p_recta(pa, pb, del, m);
		rl = R_LIST;
		rv = R_DATA;
		p_mundo_m(m);
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(i = 0; i < nv; i++){
			v[i][0] = -1.0f + 2.0f * (float)(i / 2) / (float)(nv / 2 - 1);
			v[i][1] = (i & 1) ? 0.5f : -0.5f;
			v[i][2] = 0.0f;
			for(k = 0; k < 3; k++){ if(v[i][k] < mn[k]) mn[k] = v[i][k]; if(v[i][k] > mx[k]) mx[k] = v[i][k]; }
		}
		r_dibujo((7u << 2) | (3u << 7), GE_PRIM_TRIANGLE_STRIP, p_escribir(3, (const float (*)[3])v, nv), nv);
		p_objeto(cuerpo, mn, mx);
		ge_fast_math = (int)((ronda >> 1) & 1);
		p_correr("vuelta", ronda, rl, (int)((ronda >> 2) & 1), prof ? &rp : &rs);
	}
	printf("  con recorte: %u objetos (%u dibujan algo), %u llamadas que no dibujan nada: saltadas %u sin ge_bbox_guarda ni ge_bbox_triangulos "
	       "y %u con ellos (%u por la banda, %u por sus triangulos); sin recorte: %u objetos (%u dibujan algo), %u llamadas que no dibujan "
	       "nada: saltadas %u y %u (%u por la banda, %u por sus triangulos); %u cuentas y %u imagenes distintas\n",
	       (unsigned)rp.rondas, (unsigned)rp.dibujo0, (unsigned)rp.nada_ll, (unsigned)rp.nada_s1, (unsigned)rp.nada_s4, (unsigned)rp.guarda,
	       (unsigned)rp.tri, (unsigned)rs.rondas, (unsigned)rs.dibujo0, (unsigned)rs.nada_ll, (unsigned)rs.nada_s1, (unsigned)rs.nada_s4,
	       (unsigned)rs.guarda, (unsigned)rs.tri, (unsigned)(rp.mal + rs.mal), (unsigned)(rp.distintas + rs.distintas));
	CHECK_EQ(rp.mal + rs.mal, 0u);
	CHECK_EQ(rp.distintas + rs.distintas, 0u);
	CHECK_EQ(rp.verif_mal + rs.verif_mal, 0u);
	CHECK_EQ(rp.orden_mal + rs.orden_mal, 0u);
	CHECK(rp.dibujo0 > 100 && rs.dibujo0 > 100);
}

static void bbox_guarda_azar(void){   //Arreglo
	PRes rp, rs;
	u32 ronda;
	memset(&rp, 0, sizeof(rp)); memset(&rs, 0, sizeof(rs));
	printf("Cajas exactas con la banda de guarda (ge_bbox_guarda): llamadas que cruzan el plano de la camara a su alrededor\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 1600; ronda++){
		float cen[3], mn[3], mx[3], esc = bfr(0.2f, 6.0f), v[24][3];
		int prof = (int)(ronda & 1), tipo = (int)brn(4), nc = 1 + (int)brn(4), d, i, k;
		u32 cuerpo;
		p_pantalla(prof);
		p_perspectiva();
		if(tipo == 0){ cen[0] = (brn(2) ? 1.0f : -1.0f) * bfr(0.2f, 5.0f); cen[1] = bfr(-2.0f, 2.0f); }
		else if(tipo == 1){ cen[0] = bfr(-2.0f, 2.0f); cen[1] = (brn(2) ? 1.0f : -1.0f) * bfr(0.2f, 5.0f); }
		else { cen[0] = bfr(-4.0f, 4.0f); cen[1] = bfr(-4.0f, 4.0f); }
		cen[2] = tipo == 3 ? bfr(0.0f, 4.0f) : bfr(-4.0f, 2.0f);
		rl = R_LIST;
		rv = R_DATA;
		p_mundo(cen[0], cen[1], cen[2], esc, bfr(0.0f, 6.28f));
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(d = 0; d < nc; d++){
			static const u32 pr[3] = { GE_PRIM_TRIANGLES, GE_PRIM_TRIANGLE_STRIP, GE_PRIM_TRIANGLE_FAN };
			float c0[3], r0 = bfr(0.05f, 0.5f), q[3];
			u32 pos = 1 + brn(3);
			int nv = 3 + (int)brn(14);
			for(k = 0; k < 3; k++) c0[k] = bfr(-1.0f + r0, 1.0f - r0);
			for(i = 0; i < nv; i++){
				for(k = 0; k < 3; k++) v[i][k] = c0[k] + r0 * bfr(-1.0f, 1.0f);
				p_leida(pos, v[i], q);
				for(k = 0; k < 3; k++){ if(q[k] < mn[k]) mn[k] = q[k]; if(q[k] > mx[k]) mx[k] = q[k]; }
			}
			r_dibujo((7u << 2) | (pos << 7), pr[brn(3)], p_escribir(pos, (const float (*)[3])v, nv), nv);
		}
		p_objeto(cuerpo, mn, mx);
		ge_fast_math = (int)((ronda >> 1) & 1);
		p_correr("guarda", ronda, rl, (int)((ronda >> 2) & 1), prof ? &rp : &rs);
	}
	printf("  con recorte de profundidad: %u objetos (%u dibujan algo); de los que no dibujan nada, %u llamadas calculadas: saltadas %u sin "
	       "ge_bbox_guarda ni ge_bbox_triangulos y %u con ellos (%u por la banda de %u probadas, %u por sus triangulos de %u probadas)\n",
	       (unsigned)rp.rondas, (unsigned)rp.dibujo0, (unsigned)rp.nada_ll, (unsigned)rp.nada_s1, (unsigned)rp.nada_s4, (unsigned)rp.guarda,
	       (unsigned)rp.g_prob, (unsigned)rp.tri, (unsigned)rp.t_prob);
	printf("  sin recorte: %u objetos (%u dibujan algo); de los que no dibujan nada, %u llamadas calculadas: saltadas %u sin ge_bbox_guarda ni "
	       "ge_bbox_triangulos y %u con ellos (%u por la banda de %u probadas, %u por sus triangulos de %u probadas); %u cuentas y %u imagenes "
	       "distintas; con ge_verificar %u comprobadas, %u distintas\n",
	       (unsigned)rs.rondas, (unsigned)rs.dibujo0, (unsigned)rs.nada_ll, (unsigned)rs.nada_s1, (unsigned)rs.nada_s4, (unsigned)rs.guarda,
	       (unsigned)rs.g_prob, (unsigned)rs.tri, (unsigned)rs.t_prob, (unsigned)(rp.mal + rs.mal), (unsigned)(rp.distintas + rs.distintas),
	       (unsigned)(rp.verif + rs.verif), (unsigned)(rp.verif_mal + rs.verif_mal));
	CHECK_EQ(rp.mal + rs.mal, 0u);
	CHECK_EQ(rp.distintas + rs.distintas, 0u);
	CHECK_EQ(rp.verif_mal + rs.verif_mal, 0u);
	CHECK_EQ(rp.cache_mal + rs.cache_mal, 0u);
	CHECK_EQ(rp.orden_mal + rs.orden_mal, 0u);
	CHECK(rs.guarda >= 4);   //Arreglo
	CHECK(rp.tri + rs.tri >= 50);   //Arreglo
}

static void bbox_cerca_camara(void){   //Arreglo
	PRes r;
	u32 ronda;
	memset(&r, 0, sizeof(r));
	printf("Cajas exactas: detras de la camara a menos del margen (traslaciones grandes que se cancelan)\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 400; ronda++){
		float T = bfr(200.0f, 2000.0f), e = bfr(0.05f, 0.5f), d = e + bfr(0.02f, 0.4f), m[12], v[24][3], mn[3], mx[3];
		int prof = (ronda & 3) == 3, nv = 3 + (int)brn(16), i, k;
		u32 cuerpo;
		p_pantalla(prof);
		p_perspectiva();
		ge.view[9] = 0.0f; ge.view[10] = 0.0f; ge.view[11] = -T;
		m[0] = bfr(1.0f, 5.0f); m[1] = 0.0f; m[2] = 0.0f;
		m[3] = 0.0f; m[4] = bfr(1.0f, 5.0f); m[5] = 0.0f;
		m[6] = 0.0f; m[7] = 0.0f; m[8] = e;
		m[9] = bfr(-0.3f, 0.3f); m[10] = bfr(-0.3f, 0.3f); m[11] = T + d;
		rl = R_LIST;
		rv = R_DATA;
		p_mundo_m(m);
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(i = 0; i < nv; i++)
			for(k = 0; k < 3; k++){
				v[i][k] = i < 2 && k < 2 ? (i ? 1.0f : -1.0f) : bfr(-1.0f, 1.0f);
				if(v[i][k] < mn[k]) mn[k] = v[i][k];
				if(v[i][k] > mx[k]) mx[k] = v[i][k];
			}
		{
			static const u32 pr[3] = { GE_PRIM_TRIANGLES, GE_PRIM_TRIANGLE_STRIP, GE_PRIM_TRIANGLE_FAN };
			r_dibujo((7u << 2) | (3u << 7), pr[brn(3)], p_escribir(3, (const float (*)[3])v, nv), nv);
		}
		p_objeto(cuerpo, mn, mx);
		ge_fast_math = (int)((ronda >> 1) & 1);
		p_correr("cerca", ronda, rl, (int)((ronda >> 2) & 1), &r);
	}
	printf("  %u cajas (%u probadas, %u dibujan algo), %u llamadas saltadas (%u por su caja); %u cuentas y %u imagenes distintas; "
	       "con ge_verificar %u comprobadas, %u distintas; con y sin ge_bbox_orden y ge_bbox_aabb: %u rondas con cuentas distintas\n",
	       (unsigned)r.rondas, (unsigned)r.probadas, (unsigned)r.dibujo0, (unsigned)r.salt, (unsigned)r.ll_salt, (unsigned)r.mal,
	       (unsigned)r.distintas, (unsigned)r.verif, (unsigned)r.verif_mal, (unsigned)r.orden_mal);
	CHECK_EQ(r.mal, 0u);
	CHECK_EQ(r.distintas, 0u);
	CHECK_EQ(r.verif_mal, 0u);
	CHECK_EQ(r.cache_mal, 0u);
	CHECK_EQ(r.orden_mal, 0u);
	CHECK(r.probadas * 10 > r.rondas * 6);
}

static void bbox_pantalla_rara(void){   //Arreglo
	PRes rp, rs;
	u32 ronda;
	memset(&rp, 0, sizeof(rp)); memset(&rs, 0, sizeof(rs));
	printf("Cajas exactas en pantalla: proyecciones con w negativa delante del plano cercano\n");
	ge_hw = NULL;
	for(ronda = 0; ronda < 1200; ronda++){
		float sg = (ronda & 1) ? 1.0f : -1.0f, lim, limy, ea, wc, W, W3, A, B, qm, ry, al, mn[3], mx[3], v[24][3], wa, wb, pa[3], pb[3], m[12];
		int prof = ((ronda >> 1) & 3) != 0, eje = (int)((ronda >> 3) & 1), nv = 3 + (int)brn(16), i, k;
		u32 cuerpo;
		do {
			p_pantalla(prof);
			p_perspectiva();
			lim = p_ndc_max(eje);
			limy = p_ndc_max(1 - eje);
			ea = eje ? pp_b : pp_a;
		} while(lim * ea > 0.6f || lim * ea < 0.2f || limy * (eje ? pp_a : pp_b) > 0.9f);
		wc = bfr(1.0f, 3.0f);
		B = -lim * bfr(1.3f, 1.5f);
		qm = lim * bfr(-0.3f, 0.3f);
		ry = bfr(-0.3f, 0.3f);
		A = wc * (B - qm);
		W = -A / (0.95f / ea + B) * bfr(1.1f, 1.5f);
		W3 = wc * bfr(8.0f, 10.0f);
		al = bfr(0.8f, 0.97f);
		ge.proj[10] = al;
		ge.proj[14] = (1.0f - al) * wc;
		wa = 1.15f * W;
		wb = -1.2f * W3;
		pa[eje] = sg * (A + B * wa); pa[1 - eje] = wa * limy * ry; pa[2] = -wa;
		pb[eje] = sg * (A + B * wb); pb[1 - eje] = wb * limy * ry; pb[2] = -wb;
		p_recta(pa, pb, W * bfr(0.001f, 0.005f), m);
		rl = R_LIST;
		rv = R_DATA;
		p_mundo_m(m);
		cuerpo = rl + 6 * 4;
		rl = cuerpo;
		for(k = 0; k < 3; k++){ mn[k] = 1e30f; mx[k] = -1e30f; }
		for(i = 0; i < nv; i++){
			int atras = i < 2 ? i : (int)brn(2);
			float w = atras ? -W3 * bfr(1.0f, 1.2f) : W * bfr(1.0f, 1.15f);
			v[i][0] = 2.0f * (w - wa) / (wb - wa) - 1.0f;
			v[i][1] = bfr(-1.0f, 1.0f);
			v[i][2] = bfr(-1.0f, 1.0f);
			for(k = 0; k < 3; k++){ if(v[i][k] < mn[k]) mn[k] = v[i][k]; if(v[i][k] > mx[k]) mx[k] = v[i][k]; }
		}
		{
			static const u32 pr[3] = { GE_PRIM_TRIANGLES, GE_PRIM_TRIANGLE_STRIP, GE_PRIM_TRIANGLE_FAN };
			r_dibujo((7u << 2) | (3u << 7), pr[brn(3)], p_escribir(3, (const float (*)[3])v, nv), nv);
		}
		p_objeto(cuerpo, mn, mx);
		ge_fast_math = (int)((ronda >> 4) & 1);
		p_correr("rara", ronda, rl, (int)brn(2), prof ? &rp : &rs);
	}
	printf("  con recorte de profundidad, lo de detras de la camara cortado por el plano cae en la pantalla: %u cajas (%u probadas), "
	       "%u dibujan algo, %u saltadas; sin recorte: %u cajas (%u probadas), fuera de la pantalla %u, %u saltadas; "
	       "%u cuentas y %u imagenes distintas; con ge_verificar %u comprobadas, %u distintas\n",
	       (unsigned)rp.rondas, (unsigned)rp.probadas, (unsigned)rp.dibujo0, (unsigned)rp.salt, (unsigned)rs.rondas, (unsigned)rs.probadas,
	       (unsigned)rs.pant, (unsigned)rs.salt, (unsigned)(rp.mal + rs.mal), (unsigned)(rp.distintas + rs.distintas),
	       (unsigned)(rp.verif + rs.verif), (unsigned)(rp.verif_mal + rs.verif_mal));
	CHECK_EQ(rp.mal + rs.mal, 0u);
	CHECK_EQ(rp.distintas + rs.distintas, 0u);
	CHECK_EQ(rp.verif_mal + rs.verif_mal, 0u);
	CHECK_EQ(rp.cache_mal + rs.cache_mal, 0u);
	printf("  con y sin ge_bbox_orden y ge_bbox_aabb: %u rondas con cuentas distintas\n", (unsigned)(rp.orden_mal + rs.orden_mal));   //Arreglo
	CHECK_EQ(rp.orden_mal + rs.orden_mal, 0u);   //Arreglo
	CHECK(rp.probadas * 10 > rp.rondas * 9);
	CHECK(rp.dibujo0 * 10 > rp.rondas * 9);
	CHECK(rs.probadas * 2 > rs.rondas);
	CHECK(rs.pant * 10 > rs.probadas * 9);
	CHECK(rs.verif > 100);
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
	opt_on[OPT_GE_BBOX_PANTALLA] = opt_on[OPT_GE_BBOX_LLAMADA] = 0;   //Arreglo
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
		memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
		opt_on[OPT_PROF_BBOX] = 1;   //Arreglo
		opt_on[OPT_GE_BBOX_PANTALLA] = opt_on[OPT_GE_BBOX_LLAMADA] = 1;   //Arreglo
		ge_test_run_list(LISTA, lp, NULL);   //Arreglo
		printf("  con ge_bbox_pantalla y ge_bbox_llamada: %u llamadas saltadas por su caja; sin dibujar %u (clases %u/%u/%u/%u/%u)\n",
		       (unsigned)ge_stats.bx_ll_saltadas, (unsigned)ge_stats.ob_nada, (unsigned)ge_stats.ob_nada_clase[0], (unsigned)ge_stats.ob_nada_clase[1],
		       (unsigned)ge_stats.ob_nada_clase[2], (unsigned)ge_stats.ob_nada_clase[3], (unsigned)ge_stats.ob_nada_clase[4]);   //Arreglo
		CHECK_EQ(ge_stats.tris_drawn, drawn);   //Arreglo
		CHECK_EQ(ge_stats.primitives, prims);   //Arreglo
		CHECK_EQ(ge_stats.ob_n, 4u);   //Arreglo
		CHECK_EQ(ge_stats.ob_nada, 2u);   //Arreglo
		CHECK_EQ(ge_stats.ob_nada_clase[0] + ge_stats.ob_nada_clase[1] + ge_stats.ob_nada_clase[2] + ge_stats.ob_nada_clase[3] +
		         ge_stats.ob_nada_clase[4], ge_stats.ob_nada);   //Arreglo
		CHECK(ge_stats.bx_ll_saltadas >= 3);   //Arreglo
		CHECK(ge_stats.bx_ll_cajas >= ge_stats.bx_ll_saltadas);   //Arreglo
		CHECK(ge_stats.bx_ll_leidos >= 3 * ge_stats.bx_ll_cajas);   //Arreglo
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
	bbox_pantalla_azar();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_pantalla_bordes();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_pantalla_cruza();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_pantalla_fino();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_pantalla_cercano();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_pantalla_rara();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_cerca_camara();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_guarda_azar();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_triangulos_limites();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_guarda_z();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	bbox_guarda_vuelta();   //Arreglo
	memset(&ge_stats, 0, sizeof(ge_stats));   //Arreglo
	memcpy(opt_on, saved_opt, sizeof(saved_opt));
	ge_hw = saved_hw;
	ge_fast_math = saved_fast;
	ge_vertex_reset();
	ge_raster_reset();
}
