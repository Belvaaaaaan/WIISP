/**
 * WIISP - tests/test_listas.c
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

static u32 ls = 0x5EED2110u;
static u32 lrnd(void){ ls ^= ls << 13; ls ^= ls >> 17; ls ^= ls << 5; return ls; }
static u32 lrn(u32 n){ return lrnd() % n; }

static void n_begin(void){}
static void n_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ (void)a; (void)b; (void)c; }
static void n_two(const GeVertex *a, const GeVertex *b){ (void)a; (void)b; }
static void n_point(const GeVertex *a){ (void)a; }
static void n_flush(void){}
static const GeHwRenderer null_hw = { n_begin, n_tri, n_two, n_two, n_two, n_point, n_flush, NULL };   //Arreglo

#define L_VERT 0x08700000u

static const u32 zonas[] = {
	0x08A00000u, 0x08B40000u, 0x09F00000u, 0x48A80000u, 0x88B80000u,
	0x04000000u, 0x04100000u, 0x04220000u, 0x04440000u, 0x04660000u, 0x44080000u,
	0x00010000u, 0x00012000u
};
static const u32 bordes[] = { 0x0A000000u, 0x04800000u, 0x00014000u };

static const u8 evitar[] = { GE_PRIM, GE_BEZIER, GE_SPLINE, GE_BOUNDINGBOX, GE_JUMP, GE_BJUMP, GE_CALL, GE_RET,
	                            GE_END, GE_SIGNAL, GE_FINISH, GE_TRANSFERSTART, GE_VAP, GE_BASE, GE_OFFSETADDR,
	                            GE_ORIGIN, GE_VADDR, GE_IADDR, GE_VERTEXTYPE };

static u32 lp;

static void lcmd(u32 c, u32 a){ mem_write32(lp, (c << 24) | (a & 0xFFFFFF)); lp += 4; }

static void base_de(u32 dest){
	lcmd(GE_OFFSETADDR, 0);
	lcmd(GE_BASE, ((dest & 0x0FFFFFFFu) >> 8) & 0x0F0000);
}

static int evitado(u32 c){
	unsigned i;
	for(i = 0; i < sizeof(evitar); i++) if(evitar[i] == c) return 1;
	return c >= GE_VSCX && c <= GE_VSCV;
}

static void comandos(int n){
	int i;
	for(i = 0; i < n; i++){
		u32 k = lrn(24), c;
		if(k == 0){
			u32 tc = lrn(4), col = lrn(8), nrm = lrn(4), pos = 1 + lrn(3), wt = lrn(3) ? 0 : 1 + lrn(3), nw = lrn(8);
			lcmd(GE_VERTEXTYPE, tc | (col << 2) | (nrm << 5) | (pos << 7) | (wt << 9) | (nw << 14) | ((lrn(4) == 0) << 23));
			lcmd(GE_BASE, (L_VERT >> 8) & 0x0F0000);
			lcmd(GE_VADDR, (L_VERT + lrn(0x8000)) & 0xFFFFFC);
			lcmd(GE_PRIM, (lrn(7) << 16) | lrn(7));
		} else if(k == 1){
			lcmd(GE_VERTEXTYPE, 0x180 | lrn(4));
			lcmd(GE_BASE, (L_VERT >> 8) & 0x0F0000);
			lcmd(GE_VADDR, (L_VERT + lrn(0x8000)) & 0xFFFFFC);
			lcmd(GE_BOUNDINGBOX, lrn(9));
		} else if(k == 2) lcmd(GE_ORIGIN, lrnd());
		else if(k == 3) lcmd(GE_OFFSETADDR, lrnd());
		else if(k == 4) lcmd(GE_NOP, lrnd());
		else if(k == 5){
			u32 which = lrn(5), numc[5] = { GE_WORLDMATRIXNUMBER, GE_VIEWMATRIXNUMBER, GE_PROJMATRIXNUMBER, GE_TGENMATRIXNUMBER,
			                                 GE_BONEMATRIXNUMBER };
			int j, m = 1 + (int)lrn(12);
			lcmd(numc[which], lrn(which == 4 ? 96 : 16));
			for(j = 0; j < m; j++) lcmd(numc[which] + 1, lrnd());
		} else if(k == 6){
			u32 skip = 1 + lrn(4), j;
			u32 dest = lp + 4 * 5 + 4 * skip;
			base_de(dest);
			lcmd(GE_BOUNDINGBOX, 0);
			lcmd(GE_BJUMP, dest & 0xFFFFFC);
			lcmd(GE_NOP, 0);
			for(j = 0; j < skip; j++) lcmd(lrn(2) ? GE_FINISH : GE_SIGNAL, lrnd());
		} else {
			do c = lrnd() & 0xFF; while(evitado(c));
			lcmd(c, lrn(3) ? lrnd() : ge.cmd[c]);
		}
	}
}

typedef struct {
	GeState g;
	u32 st, pc, commands, tris, prims, verts, bb;
} Foto;

static u32 sucio_ini = GE_SUCIO_TODO;

static void correr(const GeState *ini, u32 pc, u32 end, int puntero, Foto *f){
	memcpy(&ge, ini, sizeof(ge));
	ge_sucio = sucio_ini;
	ge_vertex_reset();
	memset(&ge_stats, 0, sizeof(ge_stats));
	opt_on[OPT_GE_CMD_PUNTERO] = (u8)puntero;
	f->st = (u32)ge_test_run_list(pc, end, &f->pc);
	memcpy(&f->g, &ge, sizeof(ge));
	f->commands = ge_stats.commands;
	f->tris = ge_stats.tris;
	f->prims = ge_stats.primitives;
	f->verts = ge_stats.vertices;
	f->bb = ge_stats.bb_sin_probar;
}

void test_listas(void){
	const GeHwRenderer *saved_hw = ge_hw;
	int saved_fast = ge_fast_math;
	u8 saved_opt[OPT_N];
	static GeState ini;
	u32 ronda, distintas = 0, errores = 0, bordes_n = 0, cmds = 0, saltos = 0, verif = 0, verif_mal = 0, mverif = 0, mverif_mal = 0;
	u32 hverif = 0, hverif_mal = 0;   //Arreglo
	u8 *v;
	int i;

	printf("Listas del GE: leer los comandos con un puntero (ge_cmd_puntero) contra mem_read32\n");
	memcpy(saved_opt, opt_on, sizeof(saved_opt));
	ge_hw = &null_hw;
	ge_fast_math = 1;
	opt_on[OPT_PROF_BBOX] = 0;
	opt_on[OPT_PROF_ESTADO] = 0;
	opt_on[OPT_GE_VERIF] = 0;
	opt_on[OPT_GE_BBOX] = 0;
	v = mem_ptr(L_VERT, 0x9000);
	for(i = 0; i < 0x9000; i++) v[i] = (u8)lrnd();

	for(ronda = 0; ronda < 600; ronda++){
		u32 seg[5], pc0, end = 0, z;
		int n = 1 + (int)lrn(4), s, borde = lrn(4) == 0;
		Foto a, b;
		for(s = 0; s < n; s++){
			int k, rep;
			u32 zi[5];
			do {
				zi[s] = lrn(sizeof(zonas) / sizeof(zonas[0]));
				for(rep = 0, k = 0; k < s; k++) rep |= zi[k] == zi[s];
			} while(rep);
			z = zonas[zi[s]] + (zi[s] >= 11 ? lrn(0x400) : lrn(0x1000)) * 4;
			seg[s] = z;
		}
		for(s = 0; s < n; s++){
			lp = seg[s];
			comandos(4 + (int)lrn(40));
			if(s + 1 < n){
				u32 dest = seg[s + 1] & 0x0FFFFFFFu;
				if(lrn(3) == 0){
					u32 sub = (0x08C00000u + lrn(0x1000) * 16), ret_lp;
					base_de(sub);
					lcmd(GE_CALL, sub & 0xFFFFFC);
					ret_lp = lp;
					lp = sub;
					comandos(2 + (int)lrn(10));
					lcmd(GE_RET, 0);
					lp = ret_lp;
				}
				base_de(dest);
				if(lrn(2)){ lcmd(GE_BOUNDINGBOX, 0); lcmd(GE_BJUMP, dest & 0xFFFFFC); }
				else lcmd(GE_JUMP, dest & 0xFFFFFC);
				saltos++;
			}
		}
		end = 0x08D00000u + lrn(0x1000) * 4;
		if(!borde){
			base_de(end);
			lcmd(GE_JUMP, end & 0xFFFFFC);
		} else {
			u32 b0 = bordes[lrn(sizeof(bordes) / sizeof(bordes[0]))];
			u32 dest = b0 - 4 * (1 + lrn(6));
			base_de(dest);
			lcmd(GE_JUMP, dest & 0xFFFFFC);
			lp = dest;
			while(lp < b0) lcmd(GE_NOP, lrnd());
			end = 0xFFFFFFF0u;
			bordes_n++;
		}
		pc0 = seg[0];
		for(i = 0; i < 256; i++) ini.cmd[i] = lrnd() & 0xFFFFFF;
		ini.cmd[GE_VIEWPORTXSCALE] = 0x437000; ini.cmd[GE_VIEWPORTYSCALE] = 0xC30800;
		ini.cmd[GE_VIEWPORTXCENTER] = 0x450000; ini.cmd[GE_VIEWPORTYCENTER] = 0x450000;
		ini.cmd[GE_OFFSETX] = (2048 - 240) * 16; ini.cmd[GE_OFFSETY] = (2048 - 136) * 16;
		ini.cmd[GE_SCISSOR1] = 0; ini.cmd[GE_SCISSOR2] = 479 | (271 << 10);
		ini.cmd[GE_REGION1] = 0; ini.cmd[GE_REGION2] = 479 | (271 << 10);
		for(i = 0; i < 12; i++){ ini.world[i] = (float)(int)lrn(5) - 2.0f; ini.view[i] = (float)(int)lrn(5) - 2.0f; ini.tgen[i] = 0.5f; }
		for(i = 0; i < 16; i++) ini.proj[i] = (float)(int)lrn(5) - 2.0f;
		for(i = 0; i < 96; i++) ini.bone[i / 12][i % 12] = (float)(int)lrn(5) - 2.0f;
		ini.offset = 0;
		ini.vaddr = L_VERT;
		ini.iaddr = L_VERT;
		ini.bbox_visible = (int)lrn(2);
		correr(&ini, pc0, end, 0, &a);
		opt_on[OPT_GE_VERIF] = (u8)(ronda & 1);
		correr(&ini, pc0, end, 1, &b);
		verif += ge_stats.cp_verif;
		verif_mal += ge_stats.cp_verif_bad;
		mverif += ge_stats.cm_verif;
		mverif_mal += ge_stats.cm_verif_bad;
		hverif += ge_stats.ch_verif;   //Arreglo
		hverif_mal += ge_stats.ch_verif_bad;   //Arreglo
		opt_on[OPT_GE_VERIF] = 0;
		cmds += a.commands;
		errores += a.st == 4;
		if(memcmp(&a, &b, sizeof(a))){
			if(distintas++ < 8)
				printf("  ronda %u distinta: estado %u/%u, pc %08X/%08X, comandos %u/%u, triangulos %u/%u\n", (unsigned)ronda,
				       (unsigned)a.st, (unsigned)b.st, (unsigned)a.pc, (unsigned)b.pc, (unsigned)a.commands,
				       (unsigned)b.commands, (unsigned)a.tris, (unsigned)b.tris);
		}
	}
	printf("  600 listas (%u saltos entre zonas, %u hasta el borde de la memoria, %u terminan con error), %u comandos: %u distintas\n",
	       (unsigned)saltos, (unsigned)bordes_n, (unsigned)errores, (unsigned)cmds, (unsigned)distintas);
	printf("  con ge_verificar: %u comandos comparados con execute, %u distintos; %u de matrices, %u distintos\n", (unsigned)verif,
	       (unsigned)verif_mal, (unsigned)mverif, (unsigned)mverif_mal);
	CHECK_EQ(distintas, 0u);
	CHECK_EQ(verif_mal, 0u);
	CHECK(verif > 5000);
	CHECK_EQ(mverif_mal, 0u);
	CHECK(mverif > 1000);
	printf("  con ge_verificar: %u rachas de huesos comparadas con execute, %u distintas\n", (unsigned)hverif, (unsigned)hverif_mal);   //Arreglo
	CHECK_EQ(hverif_mal, 0u);   //Arreglo
	CHECK(hverif > 50);   //Arreglo
	CHECK(errores >= bordes_n);
	CHECK(cmds > 20000);

	{
		u32 c, simples = 0, malos = 0, rep;
		sucio_ini = 0;
		for(c = 0; c < 256; c++){
			if(ge_test_cmd_con_caso(c)) continue;
			simples++;
			for(rep = 0; rep < 8; rep++){
				Foto a, b;
				u32 sa, sb;
				for(i = 0; i < 256; i++) ini.cmd[i] = lrnd() & 0xFFFFFF;
				if(rep & 1) ini.cmd[c] = 0x123456;
				lp = 0x08D40000u + lrn(0x400) * 4;
				lcmd(c, rep & 1 ? 0x123456 : lrnd());
				correr(&ini, lp - 4, lp, 0, &a);
				sa = ge_sucio;
				correr(&ini, lp - 4, lp, 1, &b);
				sb = ge_sucio;
				if(memcmp(&a, &b, sizeof(a)) || sa != sb){
					if(malos++ < 8) printf("  comando 0x%02X distinto\n", (unsigned)c);
				}
			}
		}
		sucio_ini = GE_SUCIO_TODO;
		printf("  %u comandos sin caso propio, 8 veces cada uno: %u distintos\n", (unsigned)simples, (unsigned)malos);
		CHECK_EQ(simples, 229u);
		CHECK_EQ(malos, 0u);
	}
	{
		u32 c, mats = 0, malos = 0, rep;
		sucio_ini = 0;
		for(c = 0; c < 256; c++){
			if(!ge_test_cmd_matriz(c)) continue;
			mats++;
			for(rep = 0; rep < 64; rep++){
				Foto a, b;
				u32 sa, sb, k;
				for(i = 0; i < 256; i++) ini.cmd[i] = lrnd() & 0xFFFFFF;
				for(k = 0; k < 12; k++){ ini.world[k] = (float)(int)lrn(3); ini.view[k] = (float)(int)lrn(3); ini.tgen[k] = (float)(int)lrn(3); }
				for(k = 0; k < 16; k++) ini.proj[k] = (float)(int)lrn(3);
				for(k = 0; k < 96; k++) ini.bone[k / 12][k % 12] = (float)(int)lrn(3);
				if(rep & 1) ini.cmd[c - 1] = lrn(rep & 2 ? 0x80 : 0x14);
				lp = 0x08D40000u + lrn(0x400) * 4;
				lcmd(c, rep & 4 ? ((u32)(int)lrn(3) == 0 ? 0 : 0x3F8000u) : lrnd());
				opt_on[OPT_GE_CMD_MATRICES] = 1;
				correr(&ini, lp - 4, lp, 0, &a);
				sa = ge_sucio;
				correr(&ini, lp - 4, lp, 1, &b);
				sb = ge_sucio;
				if(memcmp(&a, &b, sizeof(a)) || sa != sb){
					if(malos++ < 8) printf("  matriz 0x%02X distinta\n", (unsigned)c);
				}
			}
		}
		sucio_ini = GE_SUCIO_TODO;
		printf("  %u comandos de matrices por el atajo, 64 veces cada uno: %u distintos\n", (unsigned)mats, (unsigned)malos);
		CHECK_EQ(mats, 10u);
		CHECK_EQ(malos, 0u);
	}

	{   //Arreglo
		u32 rep, malos = 0, rachas = 0, rcmds = 0, verif_n = 0, verif_mal = 0, dentro = 0, borde = 0;
		printf("Rachas de huesos (ge_cmd_huesos) contra un comando cada vez\n");
		sucio_ini = 0;
		opt_on[OPT_GE_CMD_MATRICES] = 1;
		for(rep = 0; rep < 3000; rep++){
			Foto a, b, c;
			u32 sa, sb, sc, k, base, ini_r, len, end, caso = rep % 4, mezcla = lrn(3);
			for(i = 0; i < 256; i++) ini.cmd[i] = lrnd() & 0xFFFFFF;
			k = lrn(5);
			ini.cmd[GE_BONEMATRIXNUMBER] = k == 0 ? lrn(96) : k == 1 ? 90 + lrn(38) : k == 2 ? 0xFFFFF0u + lrn(16) : k == 3 ? lrnd() : lrnd() & 0xFFFFFF;
			for(k = 0; k < 96; k++) ini.bone[k / 12][k % 12] = (float)(int)lrn(3);
			len = 1 + (lrn(4) ? lrn(40) : lrn(300));
			if(caso == 3){
				base = 0x0A000000u - 4 * (len + 4 + lrn(4));
				if(lrn(2)) base = (base & 0x0FFFFFFFu) | 0x40000000u;
			} else {
				base = 0x08D60000u + lrn(0x1000) * 4;
				if(lrn(4) == 0) base |= 0x40000000u;
			}
			lp = base;
			for(k = lrn(4); k > 0; k--) lcmd(GE_NOP, lrnd());
			ini_r = lp;
			for(k = 0; k < len; k++){
				u32 tipo = mezcla == 0 ? (k % 13 == 0) : mezcla == 1 ? lrn(8) == 0 : lrn(2), a;
				if(tipo){
					if(lrn(3)){ a = lrn(96); a += 12 * lrn(3); } else a = lrnd();
					lcmd(GE_BONEMATRIXNUMBER, a);
				} else {
					if(lrn(4)) a = lrn(3) ? 0x3F8000u : 0; else a = lrnd();
					lcmd(GE_BONEMATRIXDATA, a);
				}
			}
			if(caso == 3){
				while((lp & 0x0FFFFFFFu) < 0x0A000000u){
					u32 cc = lrn(4) ? GE_BONEMATRIXDATA : GE_NOP;
					lcmd(cc, lrnd());
				}
				if(lrn(2)) end = 0xFFFFFFF0u;
				else { end = ini_r + 4 * lrn(len); end += 1 + lrn(3); }
				borde++;
			} else {
				for(k = lrn(3); k > 0; k--){
					u32 cc;
					do cc = lrnd() & 0xFF; while(evitado(cc) || cc == GE_BONEMATRIXNUMBER || cc == GE_BONEMATRIXDATA);
					lcmd(cc, lrnd());
				}
				end = lp;
				if(caso == 2){ end = ini_r + 4 * lrn(len + 1); dentro++; }
			}
			opt_on[OPT_GE_CMD_HUESOS] = 0;
			correr(&ini, base, end, 1, &a);
			sa = ge_sucio;
			opt_on[OPT_GE_CMD_HUESOS] = 1;
			opt_on[OPT_GE_VERIF] = (u8)(rep % 3 == 2);
			correr(&ini, base, end, 1, &b);
			sb = ge_sucio;
			rachas += ge_stats.ch_rachas;
			rcmds += ge_stats.ch_cmds;
			verif_n += ge_stats.ch_verif;
			verif_mal += ge_stats.ch_verif_bad;
			opt_on[OPT_GE_VERIF] = 0;
			correr(&ini, base, end, 0, &c);
			sc = ge_sucio;
			if(memcmp(&a, &b, sizeof(a)) || memcmp(&a, &c, sizeof(a)) || sa != sb || sa != sc){
				if(malos++ < 8)
					printf("  lista %u distinta (caso %u): estado %u/%u/%u, pc %08X/%08X/%08X, comandos %u/%u/%u\n", (unsigned)rep, (unsigned)caso,
					       (unsigned)a.st, (unsigned)b.st, (unsigned)c.st, (unsigned)a.pc, (unsigned)b.pc, (unsigned)c.pc,
					       (unsigned)a.commands, (unsigned)b.commands, (unsigned)c.commands);
			}
		}
		sucio_ini = GE_SUCIO_TODO;
		printf("  3000 listas (%u con la parada dentro de la racha, %u hasta el borde de la memoria): %u distintas; "
		       "%u rachas con %u comandos por el atajo; con ge_verificar %u comparadas, %u distintas\n", (unsigned)dentro, (unsigned)borde,
		       (unsigned)malos, (unsigned)rachas, (unsigned)rcmds, (unsigned)verif_n, (unsigned)verif_mal);
		CHECK_EQ(malos, 0u);
		CHECK_EQ(verif_mal, 0u);
		CHECK(rachas > 1500);
		{
			u32 modo, vistos[4] = { 0, 0, 0, 0 };
			opt_on[OPT_GE_CMD_HUESOS] = 1;
			opt_on[OPT_GE_VERIF] = 1;
			for(modo = 1; modo <= 3; modo++){
				Foto f;
				for(i = 0; i < 256; i++) ini.cmd[i] = lrnd() & 0xFFFFFF;
				lp = 0x08D60000u;
				lcmd(GE_BONEMATRIXNUMBER, 0);
				for(i = 0; i < 12; i++) lcmd(GE_BONEMATRIXDATA, 0x3F8000u);
				lcmd(GE_NOP, 0);
				ge_test_romper_racha((int)modo);
				correr(&ini, 0x08D60000u, lp, 1, &f);
				ge_test_romper_racha(0);
				vistos[modo] = ge_stats.ch_verif_bad;
			}
			opt_on[OPT_GE_VERIF] = 0;
			printf("  con ge_verificar y el atajo estropeado a proposito: huesos %u, numero %u, comando actual %u rachas distintas\n",
			       (unsigned)vistos[1], (unsigned)vistos[2], (unsigned)vistos[3]);
			CHECK_EQ(vistos[1], 1u);
			CHECK_EQ(vistos[2], 1u);
			CHECK_EQ(vistos[3], 1u);
		}
		CHECK(rcmds > 40000);
		CHECK(verif_n > 500);
	}

	{   //Arreglo
		static const u32 m[12] = { 0x3F8000u, 0, 0, 0, 0x3F8000u, 0, 0, 0, 0x3F8000u, 0x3F0000u, 0x3E8000u, 0 };
		u32 k, j, datos[2];
		printf("Censo de los huesos (prof_huesos)\n");
		for(k = 0; k < 2; k++){
			ge_vertex_reset();
			memset(ge.bone, 0, sizeof(ge.bone));
			ge.cmd[GE_VERTEXTYPE] = 0;
			opt_on[OPT_GE_CMD_PUNTERO] = 1;
			opt_on[OPT_GE_CMD_MATRICES] = 1;
			opt_on[OPT_PROF_HUESOS] = (u8)(k == 0);
			memset(&ge_stats, 0, sizeof(ge_stats));
			lp = 0x08D40000u;
			for(i = 0; i < 2; i++){
				lcmd(GE_BONEMATRIXNUMBER, 0);
				for(j = 0; j < 24; j++) lcmd(GE_BONEMATRIXDATA, m[j % 12]);
				lcmd(GE_NOP, 0);
			}
			lcmd(GE_BONEMATRIXNUMBER, 30);
			for(j = 0; j < 5; j++) lcmd(GE_BONEMATRIXDATA, m[j]);
			lcmd(GE_VERTEXTYPE, (3u << 7) | (2u << 9) | (1u << 14));
			lcmd(GE_BASE, (L_VERT >> 8) & 0x0F0000);
			lcmd(GE_VADDR, L_VERT & 0xFFFFFC);
			lcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 0);
			lcmd(GE_PRIM, (GE_PRIM_TRIANGLES << 16) | 3);
			lcmd(GE_NOP, 0);
			ge_test_run_list(0x08D40000u, lp, NULL);
			datos[k] = ge_stats.hu_datos;
			if(k) break;
			printf("  %u datos (%u iguales), %u matrices completas (%u iguales), %u rachas (%u datos, %u de 12 o mas), %u llamadas con huesos\n",
			       (unsigned)ge_stats.hu_datos, (unsigned)ge_stats.hu_iguales, (unsigned)ge_stats.hu_mat, (unsigned)ge_stats.hu_mat_iguales,
			       (unsigned)ge_stats.hu_rachas, (unsigned)ge_stats.hu_racha_datos, (unsigned)ge_stats.hu_rachas_largas,
			       (unsigned)ge_stats.hu_llamadas);
			CHECK_EQ(ge_stats.hu_datos, 53u);
			CHECK_EQ(ge_stats.hu_iguales, 41u);
			CHECK_EQ(ge_stats.hu_mat, 4u);
			CHECK_EQ(ge_stats.hu_mat_iguales, 2u);
			CHECK_EQ(ge_stats.hu_rachas, 3u);
			CHECK_EQ(ge_stats.hu_racha_datos, 53u);
			CHECK_EQ(ge_stats.hu_rachas_largas, 2u);
			CHECK_EQ(ge_stats.hu_llamadas, 1u);
			CHECK_EQ(ge.bone[2][6], 1.0f);
			CHECK_EQ(ge.bone[1][9], 0.5f);
		}
		CHECK_EQ(datos[1], 0u);
		opt_on[OPT_PROF_HUESOS] = 0;
	}

	memcpy(opt_on, saved_opt, sizeof(saved_opt));
	memset(&ge_stats, 0, sizeof(ge_stats));
	ge_hw = saved_hw;
	ge_fast_math = saved_fast;
	ge_vertex_reset();
}
