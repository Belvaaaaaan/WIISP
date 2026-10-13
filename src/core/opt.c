/**
 * WIISP - core/opt.c
 * Interruptores de optimización (ver opt.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/opt.h"

/* Al implementar una optimización: lista = 1 y su valor por defecto */
const OptInfo opt_info[OPT_N] = {
	[OPT_PROF_ESPERA]    = { "prof_espera",    0, 0 },
	[OPT_PROF_PMC]       = { "prof_pmc",       0, 0 },
	[OPT_GE_SKIN_RAPIDO] = { "ge_skin_rapido", 0, 0 },
	[OPT_GE_MTX_CACHE]   = { "ge_mtx_cache",   0, 0 },
	[OPT_GE_COLOR_CONST] = { "ge_color_const", 0, 0 },
	[OPT_GE_RTT_GPU]     = { "ge_rtt_gpu",     0, 0 },
	[OPT_GE_VDEC_ESP]    = { "ge_vdec_esp",    0, 0 },
	[OPT_GE_VDEC_PSQ]    = { "ge_vdec_psq",    0, 0 },
	[OPT_GE_BBOX]        = { "ge_bbox",        0, 0 },
	[OPT_GE_CHAROLAS]    = { "ge_charolas",    0, 0 },
	[OPT_CPU_SIN_EXTRA]  = { "cpu_sin_extra",  0, 0 },
	[OPT_CPU_PREDEC]     = { "cpu_predec",     0, 0 },
	[OPT_CPU_FUSION]     = { "cpu_fusion",     0, 0 },
	[OPT_HLE_RAPIDO]     = { "hle_rapido",     0, 0 },
	[OPT_HLE_REEMPLAZO]  = { "hle_reemplazo",  0, 0 },
	[OPT_MEM_BAT]        = { "mem_bat",        0, 0 },
	[OPT_GX_TRANSFORM]   = { "gx_transform",   0, 0 },
	[OPT_GX_VCACHE]      = { "gx_vcache",      0, 0 },
	[OPT_GX_DLCACHE]     = { "gx_dlcache",     0, 0 },
	[OPT_GX_TLUT]        = { "gx_tlut",        0, 0 },
	[OPT_GX_PARALELO]    = { "gx_paralelo",    0, 0 },
	[OPT_JIT]            = { "jit",            0, 0 },
	[OPT_JIT_ENLACE]     = { "jit_enlace",     0, 0 },
	[OPT_JIT_LOCKSTEP]   = { "jit_lockstep",   0, 0 },
	[OPT_JIT_NATIVO]     = { "jit_nativo",     0, 0 },
	[OPT_JIT_REGS]       = { "jit_regs",       0, 0 },
	[OPT_CACHE_LOCK]     = { "cache_lock",     0, 0 },
	[OPT_JIT_IDLE]       = { "jit_idle",       0, 0 },
	[OPT_JIT_FASTMEM]    = { "jit_fastmem",    0, 0 },
	[OPT_JIT_SD]         = { "jit_sd",         0, 0 },
	[OPT_DSP_SALIDA]     = { "dsp_salida",     0, 0 },
	[OPT_DSP_SAS]        = { "dsp_sas",        0, 0 },
};

u8 opt_on[OPT_N];

void opt_reset(void){
	int i;
	for(i = 0; i < OPT_N; i++) opt_on[i] = opt_info[i].lista && opt_info[i].por_defecto;
}

int opt_find(const char *nombre){
	int i;
	for(i = 0; i < OPT_N; i++)
		if(!strcmp(opt_info[i].nombre, nombre)) return i;
	return -1;
}

int opt_set(const char *nombre, int on){
	int i = opt_find(nombre);
	if(i < 0) return -1;
	if(!opt_info[i].lista) return 1;
	opt_on[i] = on ? 1 : 0;
	return 0;
}

int opt_parse(const char *texto){
	char nombre[32];
	size_t n = 0;
	const char *p = texto;
	char *end;
	long v;
	while(*p == ' ' || *p == '\t') p++;
	while(*p && *p != '=' && *p != ' ' && *p != '\t' && n + 1 < sizeof(nombre)) nombre[n++] = *p++;
	nombre[n] = 0;
	while(*p == ' ' || *p == '\t' || *p == '=') p++;
	if(!n || !*p) return -1;
	v = strtol(p, &end, 10);
	if(end == p || (v != 0 && v != 1)) return -1;
	while(*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
	if(*end) return -1;
	return opt_set(nombre, (int)v);
}

void opt_describe(char *buf, size_t size){
	size_t len = 0;
	int i, any = 0;
	if(!size) return;
	buf[0] = 0;
	for(i = 0; i < OPT_N; i++){
		int w;
		if(!opt_info[i].lista) continue;
		w = snprintf(buf + len, size - len, "%s%s=%d", any ? " " : "", opt_info[i].nombre, opt_on[i]);
		if(w < 0 || (size_t)w >= size - len) break;
		len += (size_t)w;
		any = 1;
	}
	if(!any) snprintf(buf, size, "ninguna implementada todavia");
}
