/**
 * WIISP - wii/gx_mezcla.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#ifndef WIISP_GX_MEZCLA_H
#define WIISP_GX_MEZCLA_H

#include <stdint.h>

enum { GMT_NADA, GMT_COPIA, GMT_TRAMADO };
enum { GMC_CPREV, GMC_APREV, GMC_C0, GMC_A0, GMC_C1, GMC_A1, GMC_C2, GMC_A2, GMC_TEXC, GMC_TEXA, GMC_RASC, GMC_RASA,
       GMC_ONE, GMC_HALF, GMC_KONST, GMC_ZERO };
enum { GMA_APREV, GMA_A0, GMA_A1, GMA_A2, GMA_TEXA, GMA_RASA, GMA_KONST, GMA_ZERO };
enum { GMO_ADD = 0, GMO_SUB = 1, GMO_CMP_GT = 14, GMO_CMP_EQ = 15 };
enum { GMB_CERO, GMB_MAS, GMB_MENOS };
enum { GMS_1, GMS_2, GMS_4, GMS_MEDIO };
enum { GMR_PREV, GMR_REG0, GMR_REG1, GMR_REG2 };
enum { GMK_1 = 0x00, GMK_1_2 = 0x04, GMK_1_4 = 0x06, GMK_K2 = 0x0E, GMK_K0_A = 0x1C, GMK_K1_A = 0x1D, GMK_K3_A = 0x1F };
enum { GMCH_R, GMCH_G, GMCH_B, GMCH_A };

typedef struct {
	uint8_t a, b, c, d, op, bias, escala, satura, reg;
} GmComb;

typedef struct {
	uint8_t tex, swap;
	GmComb col, alf;
	uint8_t kc, ka;
} GmEtapa;

typedef struct {
	const GmEtapa *e;
	int n;
	uint8_t konst[4][4];
	uint8_t konst_usa[4][4];
	uint8_t swap[4][4];
	int tramado;
} GmPrograma;

#define GM_PASA(r) { GMA_ZERO, GMA_ZERO, GMA_ZERO, GMA_APREV, GMO_ADD, GMB_CERO, GMS_1, 0, r }
#define GM_PASA_SAT(r) { GMA_ZERO, GMA_ZERO, GMA_ZERO, GMA_APREV, GMO_ADD, GMB_CERO, GMS_1, 1, r }

static const GmEtapa gm_etapas_abs[3] = {
	{ GMT_COPIA, 0, { GMC_TEXC, GMC_ZERO, GMC_ZERO, GMC_CPREV, GMO_SUB, GMB_CERO, GMS_1, 1, GMR_REG0 }, GM_PASA_SAT(GMR_REG0), 0x0C, GMK_K0_A },
	{ GMT_COPIA, 0, { GMC_CPREV, GMC_ZERO, GMC_ZERO, GMC_TEXC, GMO_SUB, GMB_CERO, GMS_1, 1, GMR_PREV }, GM_PASA_SAT(GMR_PREV), 0x0C, GMK_K0_A },
	{ GMT_NADA, 0, { GMC_C0, GMC_ZERO, GMC_ZERO, GMC_CPREV, GMO_ADD, GMB_CERO, GMS_1, 1, GMR_PREV }, GM_PASA_SAT(GMR_PREV), 0x0C, GMK_K0_A },
};

static const GmEtapa gm_etapas_doble[13] = {
	{ GMT_NADA, 0, { GMC_ZERO, GMC_CPREV, GMC_ONE, GMC_ONE, GMO_SUB, GMB_CERO, GMS_1, 0, GMR_REG0 },
	               { GMA_ZERO, GMA_APREV, GMA_KONST, GMA_ZERO, GMO_ADD, GMB_CERO, GMS_MEDIO, 0, GMR_REG0 }, GMK_1, GMK_K0_A },
	{ GMT_NADA, 0, { GMC_ZERO, GMC_ZERO, GMC_ZERO, GMC_APREV, GMO_ADD, GMB_CERO, GMS_4, 0, GMR_REG2 },
	               { GMA_ZERO, GMA_A0, GMA_KONST, GMA_APREV, GMO_ADD, GMB_CERO, GMS_4, 0, GMR_REG0 }, GMK_1, GMK_1_4 },
	{ GMT_TRAMADO, 1, { GMC_KONST, GMC_C2, GMC_TEXC, GMC_C2, GMO_CMP_GT, GMB_CERO, GMS_1, 0, GMR_REG2 },
	                  { GMA_ZERO, GMA_A0, GMA_KONST, GMA_TEXA, GMO_ADD, GMB_MAS, GMS_1, 0, GMR_REG0 }, GMK_1_2, GMK_1 },
	{ GMT_TRAMADO, 2, { GMC_ZERO, GMC_CPREV, GMC_ONE, GMC_A0, GMO_ADD, GMB_CERO, GMS_1, 0, GMR_REG1 },
	                  { GMA_APREV, GMA_KONST, GMA_TEXA, GMA_ZERO, GMO_CMP_GT, GMB_CERO, GMS_1, 0, GMR_REG0 }, GMK_1, GMK_K1_A },
	{ GMT_NADA, 0, { GMC_C0, GMC_CPREV, GMC_C2, GMC_C1, GMO_ADD, GMB_CERO, GMS_MEDIO, 0, GMR_REG1 },
	               { GMA_ZERO, GMA_ZERO, GMA_ZERO, GMA_APREV, GMO_ADD, GMB_CERO, GMS_2, 0, GMR_REG1 }, GMK_1, GMK_1 },
	{ GMT_TRAMADO, 2, { GMC_ZERO, GMC_A0, GMC_KONST, GMC_C2, GMO_ADD, GMB_CERO, GMS_1, 0, GMR_REG2 },
	                  { GMA_A1, GMA_KONST, GMA_TEXA, GMA_ZERO, GMO_CMP_GT, GMB_CERO, GMS_1, 0, GMR_REG1 }, GMK_K2, GMK_K1_A },
	{ GMT_NADA, 0, { GMC_ZERO, GMC_CPREV, GMC_A1, GMC_C1, GMO_ADD, GMB_CERO, GMS_MEDIO, 0, GMR_REG1 },
	               { GMA_ZERO, GMA_APREV, GMA_KONST, GMA_A0, GMO_SUB, GMB_CERO, GMS_4, 0, GMR_REG2 }, GMK_1, GMK_1 },
	{ GMT_TRAMADO, 3, { GMC_ZERO, GMC_C0, GMC_A0, GMC_C1, GMO_SUB, GMB_CERO, GMS_1, 0, GMR_REG1 },
	                  { GMA_TEXA, GMA_KONST, GMA_A0, GMA_A2, GMO_ADD, GMB_CERO, GMS_1, 0, GMR_REG2 }, GMK_1, GMK_K3_A },
	{ GMT_COPIA, 0, { GMC_ZERO, GMC_TEXC, GMC_ONE, GMC_ONE, GMO_SUB, GMB_CERO, GMS_1, 0, GMR_REG0 }, GM_PASA(GMR_PREV), GMK_1, GMK_1 },
	{ GMT_COPIA, 0, { GMC_TEXC, GMC_C0, GMC_A1, GMC_A2, GMO_ADD, GMB_CERO, GMS_1, 0, GMR_PREV }, GM_PASA(GMR_PREV), GMK_1, GMK_1 },
	{ GMT_COPIA, 0, { GMC_TEXC, GMC_C0, GMC_C2, GMC_CPREV, GMO_ADD, GMB_CERO, GMS_MEDIO, 0, GMR_PREV }, GM_PASA(GMR_PREV), GMK_1, GMK_1 },
	{ GMT_COPIA, 0, { GMC_TEXC, GMC_C0, GMC_A0, GMC_CPREV, GMO_ADD, GMB_CERO, GMS_MEDIO, 0, GMR_PREV }, GM_PASA(GMR_PREV), GMK_1, GMK_1 },
	{ GMT_NADA, 0, { GMC_ZERO, GMC_CPREV, GMC_ONE, GMC_C1, GMO_ADD, GMB_MENOS, GMS_1, 1, GMR_PREV }, GM_PASA_SAT(GMR_PREV), GMK_1, GMK_1 },
};

static inline void gm_programa_abs(GmPrograma *p){
	int i, j;
	p->e = gm_etapas_abs;
	p->n = 3;
	for(i = 0; i < 4; i++) for(j = 0; j < 4; j++){ p->konst[i][j] = 0; p->konst_usa[i][j] = 0; p->swap[i][j] = (uint8_t)j; }
	p->tramado = 0;
}

static inline void gm_programa_doble(GmPrograma *p, int swsub){
	static const uint8_t sw[4][4] = { { GMCH_R, GMCH_G, GMCH_B, GMCH_A }, { GMCH_G, GMCH_G, GMCH_G, GMCH_A },
	                                  { GMCH_R, GMCH_G, GMCH_B, GMCH_R }, { GMCH_R, GMCH_G, GMCH_B, GMCH_B } };
	int i, j;
	p->e = gm_etapas_doble;
	p->n = 13;
	for(i = 0; i < 4; i++) for(j = 0; j < 4; j++){ p->konst[i][j] = 0; p->konst_usa[i][j] = 0; p->swap[i][j] = sw[i][j]; }
	p->konst[0][3] = 8; p->konst_usa[0][3] = 1;
	p->konst[1][3] = 127; p->konst_usa[1][3] = 1;
	p->konst[2][0] = p->konst[2][1] = p->konst[2][2] = 2; p->konst_usa[2][0] = p->konst_usa[2][1] = p->konst_usa[2][2] = 1;
	p->konst[3][3] = (uint8_t)(swsub ? 4 : 3); p->konst_usa[3][3] = 1;
	p->tramado = 1;
}

static inline void gm_texel_tramado(int d, int swsub, uint8_t *rgba){
	rgba[0] = 255; rgba[1] = 1; rgba[2] = (uint8_t)(swsub ? 2 : 1); rgba[3] = (uint8_t)(4 * d + 130);
}

static inline int gm_tev(int a, int b, int c, int d, int bias, int escala, int op, int satura, int alfa, int swsub){
	const int ls = escala == GMS_2 ? 1 : escala == GMS_4 ? 2 : 0, rs = escala == GMS_MEDIO ? 1 : 0;
	const int bv = bias == GMB_MAS ? 128 : bias == GMB_MENOS ? -128 : 0;
	int r, t;
	a &= 255; b &= 255; c &= 255; c += c >> 7;
	t = (a * (256 - c) + b * c) << ls;
	if(op == GMO_SUB && alfa && swsub){
		t += escala == GMS_MEDIO ? 0 : 127;
		r = ((d + bv) << ls) + ((-t) >> 8);
	} else {
		t = (t + (escala == GMS_MEDIO ? 0 : op == GMO_SUB ? 127 : 128)) >> 8;
		r = ((d + bv) << ls) + (op == GMO_SUB ? -t : t);
	}
	r >>= rs;
	if(satura){ if(r < 0) r = 0; if(r > 255) r = 255; }
	else { if(r < -1024) r = -1024; if(r > 1023) r = 1023; }
	return r;
}

static inline int gm_comparar(int a, int b, int c, int d, int igual, int satura){
	int r;
	a &= 255; b &= 255; c &= 255;
	r = d + ((igual ? a == b : a > b) ? c : 0);
	if(satura){ if(r < 0) r = 0; if(r > 255) r = 255; }
	else { if(r < -1024) r = -1024; if(r > 1023) r = 1023; }
	return r;
}

static inline int gm_konst_fija(int sel){
	static const int f[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };
	return sel < 8 ? f[sel] : 0;
}

static inline int gm_konst_color(const GmPrograma *p, int sel, int canal){
	if(sel < 8) return gm_konst_fija(sel);
	if(sel >= 0x0C && sel <= 0x0F) return p->konst[sel - 0x0C][canal];
	if(sel >= 0x10) return p->konst[(sel - 0x10) & 3][(sel - 0x10) >> 2];
	return 0;
}

static inline int gm_konst_alfa(const GmPrograma *p, int sel){
	if(sel < 8) return gm_konst_fija(sel);
	if(sel >= 0x10) return p->konst[(sel - 0x10) & 3][(sel - 0x10) >> 2];
	return 0;
}

static inline void gm_ejecutar(const GmPrograma *p, int swsub, const int *s, int alfa, const uint8_t *copia, const uint8_t *tramado, int *sal){
	int reg[4][4], i, k;
	for(k = 0; k < 3; k++) reg[0][k] = s[k];
	reg[0][3] = alfa;
	for(i = 1; i < 4; i++) for(k = 0; k < 4; k++) reg[i][k] = 0;
	for(i = 0; i < p->n; i++){
		const GmEtapa *e = &p->e[i];
		const uint8_t *tex = e->tex == GMT_COPIA ? copia : e->tex == GMT_TRAMADO ? tramado : NULL;
		int t[4] = { 0, 0, 0, 0 }, nc[3], na, ent[4];
		if(tex) for(k = 0; k < 4; k++) t[k] = tex[p->swap[e->swap][k]];
		for(k = 0; k < 3; k++){
			const uint8_t sel[4] = { e->col.a, e->col.b, e->col.c, e->col.d };
			int j;
			for(j = 0; j < 4; j++){
				switch(sel[j]){
				case GMC_CPREV: ent[j] = reg[0][k]; break;
				case GMC_APREV: ent[j] = reg[0][3]; break;
				case GMC_C0: ent[j] = reg[1][k]; break;
				case GMC_A0: ent[j] = reg[1][3]; break;
				case GMC_C1: ent[j] = reg[2][k]; break;
				case GMC_A1: ent[j] = reg[2][3]; break;
				case GMC_C2: ent[j] = reg[3][k]; break;
				case GMC_A2: ent[j] = reg[3][3]; break;
				case GMC_TEXC: ent[j] = t[k]; break;
				case GMC_TEXA: ent[j] = t[3]; break;
				case GMC_ONE: ent[j] = 255; break;
				case GMC_HALF: ent[j] = 128; break;
				case GMC_KONST: ent[j] = gm_konst_color(p, e->kc, k); break;
				default: ent[j] = 0; break;
				}
			}
			if(e->col.op >= GMO_CMP_GT) nc[k] = gm_comparar(ent[0], ent[1], ent[2], ent[3], e->col.op == GMO_CMP_EQ, e->col.satura);
			else nc[k] = gm_tev(ent[0], ent[1], ent[2], ent[3], e->col.bias, e->col.escala, e->col.op, e->col.satura, 0, swsub);
		}
		{
			const uint8_t sel[4] = { e->alf.a, e->alf.b, e->alf.c, e->alf.d };
			int j;
			for(j = 0; j < 4; j++){
				switch(sel[j]){
				case GMA_APREV: ent[j] = reg[0][3]; break;
				case GMA_A0: ent[j] = reg[1][3]; break;
				case GMA_A1: ent[j] = reg[2][3]; break;
				case GMA_A2: ent[j] = reg[3][3]; break;
				case GMA_TEXA: ent[j] = t[3]; break;
				case GMA_KONST: ent[j] = gm_konst_alfa(p, e->ka); break;
				default: ent[j] = 0; break;
				}
			}
			if(e->alf.op >= GMO_CMP_GT) na = gm_comparar(ent[0], ent[1], ent[2], ent[3], e->alf.op == GMO_CMP_EQ, e->alf.satura);
			else na = gm_tev(ent[0], ent[1], ent[2], ent[3], e->alf.bias, e->alf.escala, e->alf.op, e->alf.satura, 1, swsub);
		}
		for(k = 0; k < 3; k++) reg[e->col.reg][k] = nc[k];
		reg[e->alf.reg][3] = na;
	}
	for(k = 0; k < 4; k++) sal[k] = reg[0][k] & 255;
}

static inline int gm_psp_termino(int c, int f){
	const int t = ((2 * c + 1) * (2 * (f < 0 ? -f : f) + 1)) >> 10;
	return f < 0 ? -t : t;
}

static inline int gm_psp_doble(int s, int d, int alfa, int tramado){
	const int r = gm_psp_termino(s, 2 * alfa) + gm_psp_termino(d, 255 - 2 * alfa) + tramado;
	return r < 0 ? 0 : r > 255 ? 255 : r;
}

static inline int gm_separados(const int32_t *xa, const int32_t *ya, int na, const int32_t *xb, const int32_t *yb, int nb){
	int pol, i, j;
	for(pol = 0; pol < 2; pol++){
		const int32_t *px = pol ? xb : xa, *py = pol ? yb : ya;
		const int n = pol ? nb : na;
		for(i = 0; i < n; i++){
			const int64_t nx = -(int64_t)(py[(i + 1) % n] - py[i]), ny = (int64_t)(px[(i + 1) % n] - px[i]);
			int64_t mina = INT64_MAX, maxa = INT64_MIN, minb = INT64_MAX, maxb = INT64_MIN;
			for(j = 0; j < na; j++){ const int64_t v = xa[j] * nx + ya[j] * ny; if(v < mina) mina = v; if(v > maxa) maxa = v; }
			for(j = 0; j < nb; j++){ const int64_t v = xb[j] * nx + yb[j] * ny; if(v < minb) minb = v; if(v > maxb) maxb = v; }
			if(maxa <= minb || maxb <= mina) return 1;
		}
	}
	return 0;
}

static inline int gm_rect_copia(const int32_t *caja, int lim_x, int lim_y, int al_x, int al_y, int *r){
	int x0 = (int)(caja[0] >> 4) - 1, y0 = (int)(caja[1] >> 4) - 1, x1 = (int)(caja[2] >> 4) + 2, y1 = (int)(caja[3] >> 4) + 2;
	if(x0 < 0) x0 = 0;
	if(y0 < 0) y0 = 0;
	x0 &= ~(al_x - 1);
	y0 &= ~(al_y - 1);
	x1 = (x1 + al_x - 1) & ~(al_x - 1);
	y1 = (y1 + al_y - 1) & ~(al_y - 1);
	if(x1 > lim_x) x1 = lim_x;
	if(y1 > lim_y) y1 = lim_y;
	if(x1 <= x0 || y1 <= y0) return 0;
	r[0] = x0; r[1] = y0; r[2] = x1 - x0; r[3] = y1 - y0;
	return 1;
}

static inline uint32_t gm_desplazamiento(int ancho, int tile_w, int tile_bytes, int x, int y){
	return (uint32_t)((y / 4) * ((ancho + tile_w - 1) / tile_w) + x / tile_w) * (uint32_t)tile_bytes;
}

#endif
