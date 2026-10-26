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
	[OPT_PROF_PMC]       = { "prof_pmc",       1, 0 },
	[OPT_PROF_ESTADO]    = { "prof_estado",    1, 0 },
	[OPT_PROF_TIRONES]   = { "prof_tirones",   1, 0 },
	[OPT_PROF_ES]        = { "prof_es",        1, 0 },
	[OPT_PROF_PMC_L2]    = { "prof_pmc_l2",    1, 0 },   //arreglo
	[OPT_PROF_PMC_CTR]   = { "prof_pmc_ctr",   1, 0 },   //arreglo
	[OPT_PROF_PMC_CARGA] = { "prof_pmc_carga", 1, 0 },   //arreglo
	[OPT_PROF_CPU]       = { "prof_cpu",       1, 0 },   //Arreglo
	[OPT_PROF_BLOQUES]   = { "prof_bloques",   1, 0 },   //Arreglo
	[OPT_PROF_VENTANA_JUEGO] = { "prof_ventana_juego", 1, 0 },   //Arreglo
	[OPT_PROF_VERTICES]  = { "prof_vertices",  1, 0 },   //Arreglo
	[OPT_PROF_BBOX]      = { "prof_bbox",      1, 0 },   //Arreglo
	[OPT_PROF_COMANDOS]  = { "prof_comandos",  1, 0 },   //Arreglo
	[OPT_PROF_HUESOS]    = { "prof_huesos",    1, 0 },   //Arreglo
	[OPT_GE_SKIN_RAPIDO] = { "ge_skin_rapido", 1, 1 },
	[OPT_GE_MTX_CACHE]   = { "ge_mtx_cache",   1, 1 },
	[OPT_GE_COLOR_CONST] = { "ge_color_const", 0, 0 },
	[OPT_GE_TEX_CLUT]    = { "ge_tex_clut",    1, 1 },
	[OPT_GE_RTT_GPU]     = { "ge_rtt_gpu",     1, 1 },
	[OPT_GE_FB_RELLENO]  = { "ge_fb_relleno",  1, 1 },
	[OPT_GE_FB_HUECO]    = { "ge_fb_hueco",    1, 1 },
	[OPT_GE_Z_RANGO]     = { "ge_z_rango",     1, 1 },
	[OPT_GE_VDEC_ESP]    = { "ge_vdec_esp",    1, 1 },
	[OPT_GE_VDEC_PSQ]    = { "ge_vdec_psq",    1, 1 },
	[OPT_GE_PANTALLA_RAPIDA] = { "ge_pantalla_rapida", 1, 1 },   //Arreglo
	[OPT_GE_CMD_PUNTERO] = { "ge_cmd_puntero", 1, 1 },   //Arreglo
	[OPT_GE_CMD_MATRICES] = { "ge_cmd_matrices", 1, 1 },   //Arreglo
	[OPT_GE_CMD_HUESOS] = { "ge_cmd_huesos", 1, 1 },   //Arreglo
	[OPT_GE_VERIF]       = { "ge_verificar",   1, 0 },
	[OPT_GX_ENVIO]       = { "gx_envio_rapido", 1, 1 },
	[OPT_GE_TRI_RAPIDO]  = { "ge_tri_rapido",  1, 1 },
	[OPT_GE_ENSAMBLAR]   = { "ge_ensamblar_rapido", 1, 1 },
	[OPT_GE_BBOX]        = { "ge_bbox",        1, 1 },   //Arreglo
	[OPT_GE_BBOX_RAPIDO] = { "ge_bbox_rapido", 1, 1 },   //Arreglo
	[OPT_GE_BBOX_PANTALLA] = { "ge_bbox_pantalla", 1, 1 },   //Arreglo
	[OPT_GE_BBOX_LLAMADA] = { "ge_bbox_llamada", 1, 1 },   //Arreglo
	[OPT_GE_BBOX_ORDEN] = { "ge_bbox_orden", 1, 1 },   //Arreglo
	[OPT_GE_BBOX_AABB] = { "ge_bbox_aabb", 1, 1 },   //Arreglo
	[OPT_GE_BBOX_GUARDA] = { "ge_bbox_guarda", 1, 1 },   //Arreglo
	[OPT_GE_BBOX_TRIANGULOS] = { "ge_bbox_triangulos", 1, 0 },   //Arreglo
	[OPT_GE_ESTADO_SUCIO] = { "ge_estado_sucio", 1, 1 },
	[OPT_GE_CHAROLAS]    = { "ge_charolas",    1, 1 },   //Arreglo
	[OPT_GE_CHAROLAS_ENVIO] = { "ge_charolas_envio", 1, 1 },   //Arreglo
	[OPT_CPU_SIN_EXTRA]  = { "cpu_sin_extra",  1, 1 },
	[OPT_CPU_PREDEC]     = { "cpu_predec",     1, 1 },
	[OPT_CPU_VERIFICAR]  = { "cpu_verificar",  1, 0 },
	[OPT_PREDEC_COMPACTO] = { "predec_compacto", 1, 1 },   //arreglo
	[OPT_PREDEC_SIN_UNIR] = { "predec_sin_unir", 1, 0 },   //Arreglo
	[OPT_PREDEC_MEM1]    = { "predec_mem1",    1, 1 },   //arreglo
	[OPT_CPU_VFPU_TARJETAS] = { "cpu_vfpu_tarjetas", 1, 1 },   //Arreglo
	[OPT_CPU_VFPU_VERIFICAR] = { "cpu_vfpu_verificar", 1, 0 },   //Arreglo
	[OPT_CPU_VFPU_ARIT]  = { "cpu_vfpu_arit",  1, 1 },   //Arreglo
	[OPT_CPU_ENLACE]     = { "cpu_enlace",     1, 1 },   //Arreglo
	[OPT_CPU_SYSCALL_SLOT] = { "cpu_syscall_slot", 1, 1 },   //Arreglo
	[OPT_CPU_FUSION]     = { "cpu_fusion",     0, 0 },
	[OPT_HLE_RAPIDO]     = { "hle_rapido",     0, 0 },
	[OPT_HLE_REEMPLAZO]  = { "hle_reemplazo",  0, 0 },
	[OPT_MEM_BAT]        = { "mem_bat",        0, 0 },
	[OPT_GX_TRANSFORM]   = { "gx_transform",   0, 0 },
	[OPT_GX_VCACHE]      = { "gx_vcache",      0, 0 },
	[OPT_GX_DLCACHE]     = { "gx_dlcache",     0, 0 },
	[OPT_GX_TLUT]        = { "gx_tlut",        0, 0 },
	[OPT_GX_PARALELO]    = { "gx_paralelo",    1, 1 },
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
	[OPT_REGISTRO_DIFERIDO] = { "registro_diferido", 1, 1 },
	[OPT_REGISTRO_HILO]  = { "registro_hilo",  1, 1 },
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
