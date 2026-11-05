/**
 * WIISP - tests/test_gxmezcla.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <stdlib.h>
#include "wii/gx_mezcla.h"
#include "test.h"

static uint32_t rnd_mz = 20261105u;
static uint32_t rnd(void){ rnd_mz ^= rnd_mz << 13; rnd_mz ^= rnd_mz >> 17; rnd_mz ^= rnd_mz << 5; return rnd_mz; }

static int psp_ref(int s, int d, int a, int t){
	const int fs = 2 * a, fd = 255 - 2 * a;
	int ts = ((2 * s + 1) * (2 * fs + 1)) >> 10, td = ((2 * d + 1) * (2 * (fd < 0 ? -fd : fd) + 1)) >> 10, r;
	if(fd < 0) td = -td;
	r = ts + td + t;
	return r < 0 ? 0 : r > 255 ? 255 : r;
}

static int arista(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t px, int64_t py){
	return (int)((bx - ax) * (py - ay) - (by - ay) * (px - ax) > 0 ? 1 : (bx - ax) * (py - ay) - (by - ay) * (px - ax) < 0 ? -1 : 0);
}

static int arriba_izquierda(int64_t ax, int64_t ay, int64_t bx, int64_t by, int orient){
	const int64_t dx = (bx - ax) * orient, dy = (by - ay) * orient;
	return (dy == 0 && dx < 0) || dy > 0;
}

static int cubre(const int32_t *x, const int32_t *y, int n, int px, int py){
	const int64_t cx = (int64_t)px * 16 + 8, cy = (int64_t)py * 16 + 8;
	int64_t area = 0;
	int i, orient;
	for(i = 0; i < n; i++) area += (int64_t)x[i] * y[(i + 1) % n] - (int64_t)x[(i + 1) % n] * y[i];
	if(!area) return 0;
	orient = area > 0 ? 1 : -1;
	for(i = 0; i < n; i++){
		const int e = arista(x[i], y[i], x[(i + 1) % n], y[(i + 1) % n], cx, cy) * orient;
		if(e < 0) return 0;
		if(e == 0 && !arriba_izquierda(x[i], y[i], x[(i + 1) % n], y[(i + 1) % n], orient)) return 0;
	}
	return 1;
}

static int comparten_pixel(const int32_t *xa, const int32_t *ya, int na, const int32_t *xb, const int32_t *yb, int nb){
	int px, py;
	for(py = 0; py < 16; py++)
		for(px = 0; px < 16; px++)
			if(cubre(xa, ya, na, px, py) && cubre(xb, yb, nb, px, py)) return 1;
	return 0;
}

void test_gxmezcla(void){
	static const int sd[11] = { 0, 1, 2, 63, 64, 127, 128, 129, 191, 254, 255 };
	static const int tram[4] = { -8, -1, 0, 7 };
	GmPrograma p;
	long malos = 0, alfa_mal = 0, n = 0, falsos = 0, juntos = 0, separados = 0;
	int sw, a, t, i, j, k, sal[4];
	printf("gx_mezcla_doble: la mezcla doble de la PSP en el TEV, como tabla\n");
	CHECK_EQ(gm_psp_termino(0x7F, 2 * 0xFF) + gm_psp_termino(0, 255 - 2 * 0xFF), 0xFE);
	CHECK_EQ(gm_tev(0, 128, 1, 0, GMB_CERO, GMS_1, GMO_ADD, 0, 0, 0), 1);
	CHECK_EQ(gm_tev(0, 128, 1, 200, GMB_CERO, GMS_1, GMO_SUB, 0, 0, 0), 200);
	CHECK_EQ(gm_tev(0, 128, 1, 200, GMB_CERO, GMS_1, GMO_SUB, 0, 1, 1), 199);
	CHECK_EQ(gm_tev(0, 128, 1, 200, GMB_CERO, GMS_1, GMO_SUB, 0, 1, 0), 200);
	CHECK_EQ(gm_tev(0, 128, 128, 0, GMB_CERO, GMS_1, GMO_ADD, 0, 0, 0), 65);
	CHECK_EQ(gm_tev(0, 100, 255, 7, GMB_MAS, GMS_MEDIO, GMO_ADD, 0, 0, 0), 117);
	CHECK_EQ(gm_tev(0, 0, 0, -300, GMB_CERO, GMS_1, GMO_ADD, 1, 0, 0), 0);
	CHECK_EQ(gm_tev(0, 0, 0, 2000, GMB_CERO, GMS_4, GMO_ADD, 0, 0, 0), 1023);
	CHECK_EQ(gm_tev(300, 0, 0, 0, GMB_CERO, GMS_1, GMO_ADD, 0, 0, 0), 44);
	for(i = 0; i < 20000; i++){
		const int s = (int)(rnd() & 255), d = (int)(rnd() & 255), al = (int)(rnd() & 255), tt = (int)(rnd() % 16) - 8;
		if(gm_psp_doble(s, d, al, tt) != psp_ref(s, d, al, tt)) malos++;
	}
	CHECK_EQ(malos, 0);
	for(sw = 0; sw < 2; sw++){
		uint8_t tr[4];
		gm_programa_doble(&p, sw);
		CHECK_EQ(p.n, 13);
		CHECK_EQ(gm_konst_alfa(&p, GMK_K3_A), sw ? 4 : 3);
		CHECK_EQ(gm_konst_alfa(&p, GMK_K0_A), 8);
		CHECK_EQ(gm_konst_alfa(&p, GMK_K1_A), 127);
		CHECK_EQ(gm_konst_color(&p, GMK_K2, 1), 2);
		CHECK_EQ(gm_konst_color(&p, GMK_1_2, 0), 128);
		CHECK_EQ(gm_konst_alfa(&p, GMK_1_4), 64);
		gm_texel_tramado(-8, sw, tr);
		CHECK_EQ(tr[3], 98);
		CHECK_EQ(tr[2], sw ? 2 : 1);
		for(t = 0; t < 4; t++){
			gm_texel_tramado(tram[t], sw, tr);
			for(a = 0; a < 256; a++)
				for(i = 0; i < 11; i++)
					for(j = 0; j < 11; j += 3){
						const int s[3] = { sd[i], sd[i], sd[i] };
						uint8_t c[4] = { (uint8_t)sd[j], (uint8_t)sd[j + 1 < 11 ? j + 1 : 0], (uint8_t)sd[j + 2 < 11 ? j + 2 : 0], 0 };
						gm_ejecutar(&p, sw, s, a, c, tr, sal);
						for(k = 0; k < 3; k++){ n++; if(sal[k] != gm_psp_doble(s[k], c[k], a, tram[t])) malos++; }
						if(sal[3] != a) alfa_mal++;
					}
		}
		for(i = 0; i < 60000; i++){
			const int al = (int)(rnd() & 255), tt = (int)(rnd() % 16) - 8;
			const int s[3] = { (int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255) };
			uint8_t c[4] = { (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd() };
			gm_texel_tramado(tt, sw, tr);
			gm_ejecutar(&p, sw, s, al, c, tr, sal);
			for(k = 0; k < 3; k++){ n++; if(sal[k] != gm_psp_doble(s[k], c[k], al, tt)) malos++; }
			if(sal[3] != al) alfa_mal++;
		}
	}
	CHECK_EQ(malos, 0);
	CHECK_EQ(alfa_mal, 0);
	gm_programa_doble(&p, 0);
	{
		uint8_t tr[4];
		long cruzados = 0;
		gm_texel_tramado(0, 0, tr);
		for(a = 0; a < 256; a++)
			for(i = 0; i < 256; i += 5){
				const int s[3] = { i, 255 - i, i ^ 0x55 };
				uint8_t c[4] = { (uint8_t)(i * 7), (uint8_t)(i * 13 + 5), (uint8_t)(255 - i), 0 };
				gm_ejecutar(&p, 1, s, a, c, tr, sal);
				for(k = 0; k < 3; k++) cruzados += sal[k] != gm_psp_doble(s[k], c[k], a, 0);
			}
		CHECK(cruzados > 0);
		printf("  con la variante equivocada de la resta: %ld distintos\n", cruzados);
	}
	gm_programa_abs(&p);
	{
		uint8_t tr[4] = { 0, 0, 0, 0 };
		long m = 0;
		for(i = 0; i < 256; i++)
			for(j = 0; j < 256; j += 3){
				const int s[3] = { i, 255 - i, i };
				uint8_t c[4] = { (uint8_t)j, (uint8_t)(j + 1 < 256 ? j + 1 : 0), (uint8_t)(j + 2 < 256 ? j + 2 : 0), 9 };
				gm_ejecutar(&p, 0, s, 33, c, tr, sal);
				for(k = 0; k < 3; k++) if(sal[k] != abs(s[k] - c[k])) m++;
				if(sal[3] != 33) m++;
			}
		CHECK_EQ(m, 0);
	}
	printf("  %ld comparaciones de la mezcla doble (las dos variantes de la resta)\n", n);

	{
		const int32_t ax[3] = { 0, 160, 0 }, ay[3] = { 0, 0, 160 }, bx[3] = { 160, 160, 0 }, by[3] = { 0, 160, 160 };
		const int32_t cx[3] = { 16, 176, 16 }, cy[3] = { 16, 16, 176 };
		const int32_t rx[4] = { 0, 160, 160, 0 }, ry[4] = { 0, 0, 160, 160 }, sx[4] = { 160, 320, 320, 160 }, sy[4] = { 0, 0, 160, 160 };
		const int32_t dx[3] = { 0, 160, 320 }, dy[3] = { 0, 0, 0 };
		CHECK_EQ(gm_separados(ax, ay, 3, bx, by, 3), 1);
		CHECK_EQ(gm_separados(ax, ay, 3, cx, cy, 3), 0);
		CHECK_EQ(gm_separados(rx, ry, 4, sx, sy, 4), 1);
		CHECK_EQ(gm_separados(rx, ry, 4, ax, ay, 3), 0);
		CHECK_EQ(gm_separados(dx, dy, 3, ax, ay, 3), 1);
	}
	for(i = 0; i < 30000; i++){
		int32_t xa[4], ya[4], xb[4], yb[4];
		int na = (i & 7) == 0 ? 4 : 3, nb = (i & 15) == 1 ? 4 : 3, sep;
		for(k = 0; k < 3; k++){ xa[k] = (int32_t)(rnd() % 257); ya[k] = (int32_t)(rnd() % 257); }
		if(na == 4){ int32_t x0 = xa[0], y0 = ya[0], x1 = xa[1], y1 = ya[1];
			xa[0] = x0; ya[0] = y0; xa[1] = x1; ya[1] = y0; xa[2] = x1; ya[2] = y1; xa[3] = x0; ya[3] = y1; }
		if(i % 3 == 0){
			xb[0] = xa[1]; yb[0] = ya[1]; xb[1] = xa[2]; yb[1] = ya[2];
			xb[2] = (int32_t)(rnd() % 257); yb[2] = (int32_t)(rnd() % 257);
			nb = 3;
		} else {
			for(k = 0; k < 3; k++){ xb[k] = (int32_t)(rnd() % 257); yb[k] = (int32_t)(rnd() % 257); }
			if(nb == 4){ int32_t x0 = xb[0], y0 = yb[0], x1 = xb[1], y1 = yb[1];
				xb[0] = x0; yb[0] = y0; xb[1] = x1; yb[1] = y0; xb[2] = x1; yb[2] = y1; xb[3] = x0; yb[3] = y1; }
		}
		sep = gm_separados(xa, ya, na, xb, yb, nb);
		if(sep != gm_separados(xb, yb, nb, xa, ya, na)) falsos++;
		if(sep) separados++; else juntos++;
		if(sep && comparten_pixel(xa, ya, na, xb, yb, nb)) falsos++;
	}
	CHECK_EQ(falsos, 0);
	CHECK(separados > 3000 && juntos > 3000);
	printf("  solapes: %ld separados, %ld que se pisan, %ld separados que comparten un pixel\n", separados, juntos, falsos);
}
