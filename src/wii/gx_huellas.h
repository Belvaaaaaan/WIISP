/**
 * WIISP - wii/gx_huellas.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#ifndef WIISP_GX_HUELLAS_H
#define WIISP_GX_HUELLAS_H

#include <stdint.h>

#define HZ_CASILLAS 16384
#define HZ_PRUEBAS 16

typedef struct {
	uint32_t clave[HZ_CASILLAS], rango[HZ_CASILLAS], epoca[HZ_CASILLAS];
	uint8_t primeros[HZ_CASILLAS], rangos_varios[HZ_CASILLAS];
} HzTabla;

enum { HZ_SIN_PAREJA, HZ_IGUAL, HZ_OTRO_RANGO, HZ_ROTADO, HZ_ROTADO_OTRO_RANGO };

static inline int hz_menor(const int32_t *a, const int32_t *b){
	if(a[1] != b[1]) return a[1] < b[1];
	if(a[0] != b[0]) return a[0] < b[0];
	return a[2] < b[2];
}

static inline int hz_canonico(const int32_t *x, const int32_t *y, const int32_t *z){
	const int32_t v[3][3] = { { x[0], y[0], z[0] }, { x[1], y[1], z[1] }, { x[2], y[2], z[2] } };
	int k = hz_menor(v[1], v[0]) ? 1 : 0;
	if(hz_menor(v[2], v[k])) k = 2;
	return k;
}

static inline uint32_t hz_huella(const int32_t *x, const int32_t *y, const int32_t *z, uint32_t superficie, int *primero){
	int32_t v[3][3];
	int orden[3] = { 0, 1, 2 }, i, j;
	uint32_t h = 2166136261u ^ superficie;
	for(i = 0; i < 3; i++){ v[i][0] = x[i]; v[i][1] = y[i]; v[i][2] = z[i]; }
	for(i = 1; i < 3; i++)
		for(j = i; j > 0 && hz_menor(v[orden[j]], v[orden[j - 1]]); j--){ const int t = orden[j]; orden[j] = orden[j - 1]; orden[j - 1] = t; }
	for(i = 0; i < 3; i++)
		for(j = 0; j < 3; j++){ h ^= (uint32_t)v[orden[i]][j]; h *= 16777619u; h ^= h >> 15; }
	for(i = 0; i < 3; i++) if(orden[i] == 0) *primero = i;
	return h ? h : 1u;
}

static inline int hz_casilla(const HzTabla *t, uint32_t clave, uint32_t epoca, int *libre){
	uint32_t i = (clave * 2654435761u) >> 18;
	int k;
	*libre = -1;
	for(k = 0; k < HZ_PRUEBAS; k++, i = (i + 1) & (HZ_CASILLAS - 1)){
		if(t->epoca[i] != epoca){ if(*libre < 0) *libre = (int)i; return -1; }
		if(t->clave[i] == clave) return (int)i;
	}
	return -1;
}

static inline int hz_poner(HzTabla *t, uint32_t clave, int primero, uint32_t rango, uint32_t epoca){
	int libre, i = hz_casilla(t, clave, epoca, &libre);
	if(i < 0){
		if(libre < 0) return 0;
		i = libre;
		t->clave[i] = clave;
		t->epoca[i] = epoca;
		t->rango[i] = rango;
		t->primeros[i] = 0;
		t->rangos_varios[i] = 0;
	}
	t->primeros[i] |= (uint8_t)(1u << primero);
	if(t->rango[i] != rango) t->rangos_varios[i] = 1;
	return 1;
}

static inline int hz_buscar(const HzTabla *t, uint32_t clave, int primero, uint32_t rango, uint32_t epoca){
	int libre, i = hz_casilla(t, clave, epoca, &libre);
	int mismo_rango;
	if(i < 0) return HZ_SIN_PAREJA;
	mismo_rango = t->rango[i] == rango && !t->rangos_varios[i];
	if(t->primeros[i] & (1u << primero)) return mismo_rango ? HZ_IGUAL : HZ_OTRO_RANGO;
	return mismo_rango ? HZ_ROTADO : HZ_ROTADO_OTRO_RANGO;
}

#endif
