/**
 * WIISP - vfpu.h
 * La VFPU del Allegrex: 128 registros float organizados en 8 matrices 4x4,
 * con prefijos de swizzle/saturación y operaciones vectoriales y de
 * matrices. La usan casi todos los juegos para su matemática 3D.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_VFPU_H
#define WIISP_VFPU_H

#include <math.h>   //Arreglo
#include <string.h>   //Arreglo
#include "cpu/cpu.h"

/* Registros de control (mfvc/mtvc 128 + n) */
enum {
	VFPU_CTRL_SPREFIX = 0, VFPU_CTRL_TPREFIX, VFPU_CTRL_DPREFIX, VFPU_CTRL_CC,
	VFPU_CTRL_INF4, VFPU_CTRL_RSV5, VFPU_CTRL_RSV6, VFPU_CTRL_REV,
	VFPU_CTRL_RCX0, VFPU_CTRL_RCX1, VFPU_CTRL_RCX2, VFPU_CTRL_RCX3,
	VFPU_CTRL_RCX4, VFPU_CTRL_RCX5, VFPU_CTRL_RCX6, VFPU_CTRL_RCX7,
	VFPU_CTRL_MAX
};

/* Índice en vfpu.v[] del registro de 7 bits (matriz, columna, fila): las
   columnas quedan contiguas, como en PPSSPP */
#define VFPU_INDEX(reg) (((((reg) >> 2) & 7) << 4) | (((reg) & 3) << 2) | (((reg) >> 5) & 3))

/* Los registros VFPU con los que se ejecuta ahora */
extern VfpuState vfpu;

/* Cambio de hilo perezoso: la mayoría de los hilos no usan la VFPU, así
   que el kernel no copia sus registros en cada cambio. Si vfpu_live es 0,
   `vfpu` puede ser de otro hilo y la primera instrucción VFPU llama a
   hle_vfpu_load() (lo implementa el kernel) para traer los del actual. */
extern int vfpu_live;
void hle_vfpu_load(void);
static inline void vfpu_ensure(void){ if(__builtin_expect(!vfpu_live, 0)) hle_vfpu_load(); }

/* 0 desactiva los caminos rápidos (sin prefijos) para compararlos con el
   camino general en las pruebas */
extern int vfpu_fast_paths;

/* Estado inicial de la VFPU de un hilo */
void vfpu_reset(VfpuState *s);

/* Ejecuta una instrucción de la VFPU (COP2 salvo las ramas, VFPU0-7,
   lv/sv). cpu.pc ya apunta a la siguiente. */
void vfpu_exec(u32 op, u32 pc);

/* Estadísticas para wiisp.log: cuántas veces se ejecuta cada tipo de
   instrucción, para saber qué conviene acelerar en cada juego */
enum {
	VS_LV, VS_SV, VS_LVQ, VS_SVQ, VS_LVSVLR, VS_MFV, VS_MTV, VS_MFVC_MTVC, VS_BV,
	VS_VADD, VS_VSUB, VS_VMUL, VS_VDIV, VS_VDOT, VS_VSCL, VS_VHDP, VS_VCRS, VS_VDET,
	VS_VMOV, VS_VABS_NEG, VS_VSAT, VS_VRCP, VS_VRSQ, VS_VSIN_COS, VS_VEXP_LOG,
	VS_VSQRT, VS_VASIN, VS_VIDT_ZERO_ONE, VS_VCMP, VS_VMIN_MAX, VS_VSCMP_SGE_SLT,
	VS_VCMOV, VS_VF2I, VS_VI2F, VS_VCST, VS_VPFX, VS_VIIM, VS_VMMUL, VS_VTFM,
	VS_VMSCL, VS_VCRSP_QMUL, VS_VMMOV, VS_VMINIT, VS_VROT, VS_VFLUSH, VS_VRND,
	VS_CONV, VS_VFPU9, VS_VWBN, VS_SBN_SBZ_LGB,
	VS_COUNT
};
extern u64 vfpu_stat[VS_COUNT];
extern const char *const vfpu_stat_name[VS_COUNT];
void vfpu_stats_fold(void);   /* pasa los contadores rápidos a vfpu_stat */
extern u32 vfpu_stat_bv;      /* bvf/bvt, los cuenta el intérprete */
extern u32 vfpu_stat_now[VS_COUNT];   //Arreglo
extern int vfpu_clase;   //Arreglo
extern u16 vfpu_vidx[5][128][4];   //Arreglo

//Arreglo
static inline float vfpu_flush(float f){
	return fabsf(f) < 1.17549435e-38f ? f * 0.0f : f;
}

//Arreglo
static inline float vfpu_canon(float f){
	if(__builtin_expect(f != f, 0)){
		const u32 u = 0x7F800001u;
		float n;
		memcpy(&n, &u, 4);
		return n;
	}
	return fabsf(f) < 1.17549435e-38f ? f * 0.0f : f;
}

//Arreglo
static inline int vfpu_nan_or_inf(float f){ return !(fabsf(f) <= 3.40282347e+38f); }

//Arreglo
static inline u32 vfpu_minmax_bits(float a, float b, u32 ua, u32 ub, int is_max){
	if(vfpu_nan_or_inf(a) || vfpu_nan_or_inf(b)){
		const s32 sa = (s32)ua, sb = (s32)ub;
		const int both_neg = sa < 0 && sb < 0, take_max = is_max ? !both_neg : both_neg;
		if(take_max) return (u32)(sb > sa ? sb : sa);
		return (u32)(sb < sa ? sb : sa);
	}
	{
		const float fa = vfpu_flush(a), fb = vfpu_flush(b);
		if(is_max) return fb < fa ? ua : ub;
		return fa < fb ? ua : ub;
	}
}

//Arreglo
static inline s32 vfpu_f2i_mode(double sv, int mode){
	const s32 t = (s32)sv;
	s32 fl;
	double fr;
	switch(mode){
	case 17: return t;
	case 18: return t + ((double)t < sv);
	case 19: return t - ((double)t > sv);
	default:
		fl = t - ((double)t > sv);
		fr = sv - (double)fl;
		if(fr < 0.5) return fl;
		if(fr > 0.5) return fl + 1;
		return fl + (fl & 1);
	}
}

//Arreglo
static inline u32 vfpu_f2i_lane(float s, float mult, int mode){
	double sv;
	if(s != s) return 0x7FFFFFFF;
	sv = (double)(s * mult);
	if(sv > (double)0x7FFFFFFF) return 0x7FFFFFFF;
	if(sv <= -2147483648.0) return 0x80000000u;
	return (u32)vfpu_f2i_mode(sv, mode);
}

/* Funciones de la VFPU, exactas al hardware (para pruebas) */
float vfpu_sin(float x);
float vfpu_cos(float x);
float vfpu_asin(float x);
float vfpu_rcp(float x);
float vfpu_rsqrt(float x);
float vfpu_sqrt(float x);
float vfpu_exp2(float x);
float vfpu_log2(float x);

#endif
