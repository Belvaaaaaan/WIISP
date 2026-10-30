/**
 * WIISP - wii/gx_stencil.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#ifndef WIISP_GX_STENCIL_H
#define WIISP_GX_STENCIL_H

#include <stdint.h>
#include <string.h>

typedef struct {
	uint64_t usados;
	uint8_t val[64];
	uint8_t cod[256];
	uint8_t dec[256];
	unsigned perdidos, raros;
} StCod;

static inline uint8_t st_enc(int c){ return (uint8_t)((c << 2) | (c >> 4)); }

static inline void st_vaciar(StCod *t){ t->usados = 0; t->perdidos = 0; t->raros = 0; }

static inline void st_empezar(StCod *t){
	int i;
	memset(t->cod, 0, sizeof(t->cod));
	for(i = 0; i < 256; i++) t->dec[i] = (uint8_t)i;
}

static inline void st_poner(StCod *t, int c, int v){
	int i;
	if(!t->usados) st_empezar(t);
	t->usados |= (uint64_t)1 << c;
	t->val[c] = (uint8_t)v;
	t->cod[v] = (uint8_t)(c + 1);
	for(i = 0; i < 4; i++) t->dec[(c << 2) | i] = (uint8_t)v;
	if(c != v >> 2) t->raros++;
}

static inline int st_codigo(StCod *t, int v){
	int n = (v & 0xFF) >> 2, c = -1, d;
	v &= 0xFF;
	if(t->usados && t->cod[v]){
		c = t->cod[v] - 1;
		return c == n ? v : st_enc(c);
	}
	for(d = 0; d < 64 && c < 0; d++){
		if(n + d < 64 && !((t->usados >> (n + d)) & 1)) c = n + d;
		else if(n - d >= 0 && !((t->usados >> (n - d)) & 1)) c = n - d;
	}
	if(c < 0){ t->perdidos++; return -1; }
	st_poner(t, c, v);
	return c == n ? v : st_enc(c);
}

static inline void st_cerrar(StCod *t){
	if(!t->usados) st_empezar(t);
	t->usados = ~(uint64_t)0;
}

static inline void st_lleno(StCod *t){
	st_vaciar(t);
	st_cerrar(t);
}

static inline int st_monotono(StCod *t, const uint8_t *hay){
	int v, k = 0, total = 0, prev = -1, c;
	for(v = 0; v < 256; v++) total += hay[v] != 0;
	st_vaciar(t);
	if(total > 64){ st_lleno(t); return 0; }
	for(v = 0; v < 256; v++){
		if(!hay[v]) continue;
		c = v >> 2;
		if(c <= prev) c = prev + 1;
		if(c > 64 - (total - k)) c = 64 - (total - k);
		st_poner(t, c, v);
		prev = c;
		k++;
	}
	return 1;
}

static inline const uint8_t *st_tabla(const StCod *t, const uint8_t *ident){ return t->usados ? t->dec : ident; }

#endif
