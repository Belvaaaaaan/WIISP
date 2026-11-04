/**
 * WIISP - wii/gx_mascara.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#ifndef WIISP_GX_MASCARA_H
#define WIISP_GX_MASCARA_H

#include <stdint.h>

enum { GM_NORMAL, GM_NADA, GM_OR, GM_AND, GM_NO_COLOR, GM_NO_ALFA, GM_NO_BITS, GM_NO_RGBA6, GM_N };

static inline int gm_bits(int fmt, int canal){
	static const uint8_t b[4][3] = { { 5, 6, 5 }, { 5, 5, 5 }, { 4, 4, 4 }, { 8, 8, 8 } };
	return b[fmt & 3][canal];
}

static inline uint32_t gm_escribibles(int fmt, uint32_t mascara){
	uint32_t w = 0;
	int c;
	for(c = 0; c < 3; c++){
		const int b = gm_bits(fmt, c), s = 8 - b;
		const uint32_t m = ((mascara >> (8 * c)) & 0xFFu) >> s, wn = ~m & ((1u << b) - 1u);
		w |= (wn << s) << (8 * c);
	}
	return w;
}

static inline int gm_calcular(int fmt, uint32_t mascara, uint32_t color, int constante, int escribe_alfa, int rgba6, uint32_t *konst){
	const uint32_t w = gm_escribibles(fmt, mascara), lleno = gm_escribibles(fmt, 0), v = color & w;
	*konst = 0;
	if(w == lleno) return GM_NORMAL;
	if(!w) return GM_NADA;
	if(!constante) return GM_NO_COLOR;
	if(escribe_alfa) return GM_NO_ALFA;
	if(rgba6 && (w & 0x030303u)) return GM_NO_RGBA6;
	if(v == w){ *konst = w; return GM_OR; }
	if(!v){ *konst = ~w & 0xFFFFFFu; return GM_AND; }
	return GM_NO_BITS;
}

#endif
