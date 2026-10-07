/**
 * WIISP - vfpu.c
 * Intérprete de la VFPU del Allegrex.
 *
 * Port del intérprete de PPSSPP (Core/MIPS/InterpreterVFPU.cpp y
 * Core/MIPS/MIPSVFPUUtils.cpp, (c) 2012- PPSSPP Project, GPLv2+), que
 * reproduce el hardware hasta en los efectos raros de los prefijos: cada
 * instrucción decide cómo le afectan los prefijos S, T y D (swizzle,
 * constantes, abs, negado, saturación y máscara de escritura). rcp, rsqrt,
 * sqrt, exp2, log2, sin/cos y asin usan las tablas de segmentos que imitan
 * el interpolador cuadrático de la PSP bit a bit (vfpu_tables.h).
 *
 * Los registros se guardan con las columnas contiguas (VFPU_INDEX).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

/* La PSP no fusiona multiplicaciones y sumas: que GCC tampoco (fmadds) */
#pragma GCC optimize ("fp-contract=off")

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/vfpu.h"
#include "cpu/vfpu_tables.h"
#include "core/memory.h"

typedef enum { V_Single = 1, V_Pair = 2, V_Triple = 3, V_Quad = 4, V_Invalid = -1 } VectorSize;
typedef enum { M_1x1 = 1, M_2x2 = 2, M_3x3 = 3, M_4x4 = 4 } MatrixSize;

/* Constantes de los prefijos (VFPUConst de PPSSPP) */
enum { C_NONE = -1, C_ZERO, C_ONE, C_TWO, C_HALF, C_THREE, C_THIRD, C_FOURTH, C_SIXTH };

#define VD (op & 0x7F)
#define VS ((op >> 8) & 0x7F)
#define VT ((op >> 16) & 0x7F)
#define CTRL vfpu.ctrl

typedef union { float f[4]; u32 u[4]; s32 i[4]; } FloatBits;

VfpuState vfpu;
int vfpu_live;
u64 vfpu_stat[VS_COUNT];
const char *const vfpu_stat_name[VS_COUNT] = {
	"lv.s", "sv.s", "lv.q", "sv.q", "lvl/lvr/svl/svr", "mfv", "mtv", "mfvc/mtvc", "bvf/bvt",
	"vadd", "vsub", "vmul", "vdiv", "vdot", "vscl", "vhdp", "vcrs", "vdet",
	"vmov", "vabs/vneg", "vsat", "vrcp", "vrsq", "vsin/vcos", "vexp2/vlog2",
	"vsqrt", "vasin", "vidt/vzero/vone", "vcmp", "vmin/vmax", "vscmp/vsge/vslt",
	"vcmov", "vf2i", "vi2f", "vcst", "vpfx", "viim/vfim", "vmmul", "vtfm/vhtfm",
	"vmscl", "vcrsp/vqmul", "vmmov", "vmidt/vmzero/vmone", "vrot", "vflush", "vrnd",
	"conversiones", "vsrt/vbfy/vocp/vfad/vavg/vsgn", "vwbn", "vsbn/vsbz/vlgb",
};
#define VSTAT(x) (vfpu_stat_now[x]++)
/* Contadores de 32 bits (un u64 son seis instrucciones en el Broadway); se
   pasan a vfpu_stat en cada frame con vfpu_stats_fold() */
static u32 vfpu_stat_now[VS_COUNT];
u32 vfpu_stat_bv;
#define NOINLINE __attribute__((noinline))

void vfpu_stats_fold(void){
	int i;
	for(i = 0; i < VS_COUNT; i++){
		vfpu_stat[i] += vfpu_stat_now[i];
		vfpu_stat_now[i] = 0;
	}
	vfpu_stat[VS_BV] += vfpu_stat_bv;
	vfpu_stat_bv = 0;
}

static inline u32 f2u(float f){ u32 u; memcpy(&u, &f, 4); return u; }
static inline float u2f(u32 u){ float f; memcpy(&f, &u, 4); return f; }
static inline int is_nan(float f){ return (f2u(f) & 0x7FFFFFFFu) > 0x7F800000u; }
static inline int is_inf(float f){ return (f2u(f) & 0x7FFFFFFFu) == 0x7F800000u; }
static inline int is_nan_or_inf(float f){ return (f2u(f) & 0x7F800000u) == 0x7F800000u; }

/* Los denormales cuentan como cero con signo. Se comprueba con la FPU: en
   el Broadway, pasar un float a un registro entero obliga a guardarlo en
   memoria y volver a leerlo. x * 0 conserva el signo. */
#define VFPU_FLT_MIN 1.17549435e-38f
static inline float flush(float f){
	return fabsf(f) < VFPU_FLT_MIN ? f * 0.0f : f;
}

/* Salida de las sumas y productos: NaN canónico y denormales a cero
   (pspautotests cpu/vfpu/specials). Además deja el mismo NaN en x86 y en
   PowerPC. */
static inline float canon(float f){
	if(__builtin_expect(f != f, 0)) return u2f(0x7F800001u);
	return fabsf(f) < VFPU_FLT_MIN ? f * 0.0f : f;
}

static inline VectorSize vec_size(u32 op){ return (VectorSize)(((op >> 7) & 1) + ((op >> 14) & 2) + 1); }
static inline MatrixSize mtx_size(u32 op){ return (MatrixSize)(((op >> 7) & 1) + ((op >> 14) & 2) + 1); }
static inline int nelem(VectorSize sz){ return sz >= 1 && sz <= 4 ? (int)sz : 1; }

static inline u32 SWIZZLE(int x, int y, int z, int w){ return (u32)(x | (y << 2) | (z << 4) | (w << 6)); }
static inline u32 MASK4(int x, int y, int z, int w){ return (u32)(x | (y << 1) | (z << 2) | (w << 3)); }
#define ANY_SWIZZLE 0xFFu
#define ABS4(x, y, z, w) (MASK4(x, y, z, w) << 8)
#define NEGATE4(x, y, z, w) (MASK4(x, y, z, w) << 16)

static u32 MAKE_CONSTANTS(int x, int y, int z, int w){
	u32 r = 0;
	if(x != C_NONE) r |= (u32)(((x & 3) << 0) | ((x & 4) << 6) | (1 << 12));
	if(y != C_NONE) r |= (u32)(((y & 3) << 2) | ((y & 4) << 7) | (1 << 13));
	if(z != C_NONE) r |= (u32)(((z & 3) << 4) | ((z & 4) << 8) | (1 << 14));
	if(w != C_NONE) r |= (u32)(((w & 3) << 6) | ((w & 4) << 9) | (1 << 15));
	return r;
}

static inline u32 rewrite_prefix(int ctrl, u32 remove, u32 add){ return (CTRL[ctrl] & ~remove) | add; }

static inline u32 write_mask(void){ return (CTRL[VFPU_CTRL_DPREFIX] >> 8) & 0xF; }
static inline int write_masked(int i){ return (CTRL[VFPU_CTRL_DPREFIX] >> (8 + i)) & 1; }

/* Sin prefijos activos (lo normal): S y T dejan pasar y D no hace nada.
   Lo comprueban solo las instrucciones que tienen camino rápido. */
int vfpu_fast_paths = 1;
static inline int prefixes_neutral(void){
	return ((CTRL[VFPU_CTRL_SPREFIX] ^ 0xE4) | (CTRL[VFPU_CTRL_TPREFIX] ^ 0xE4) | CTRL[VFPU_CTRL_DPREFIX]) == 0;
}
#define FAST_OK() (prefixes_neutral() & vfpu_fast_paths)

/* NaN o infinito, con la FPU */
static inline int nan_or_inf_f(float f){ return !(fabsf(f) <= 3.40282347e+38f); }


/* --- Registros ------------------------------------------------------------------------ */

/* Posición en bytes dentro de vfpu.v de cada componente de cada registro,
   precalculada: vidx[tamaño][reg][i] para vectores y
   midx[tamaño][reg][columna * 4 + fila] para matrices. En bytes, cada
   acceso es una carga indexada sin desplazamientos. */
static u16 vidx[5][128][4];
static u16 midx[5][128][16];
/* Matrices 4x4 y vectores de 4 guardados seguidos (M000, C000...): byte de
   inicio en vfpu.v, o -1 si no lo están */
static s16 mcont4[128], vcont4[128];
#define VF(off) (*(float *)((u8 *)vfpu.v + (off)))
#define VI(off) (*(u32 *)((u8 *)vfpu.vi + (off)))

static void build_tables(void){
	int size, reg, i, j;
	for(reg = 0; reg < 128; reg++){
		const int mtx = (reg << 2) & 0x70, col = reg & 3, transpose = (reg >> 5) & 1;
		for(size = 1; size <= 4; size++){
			int row = size == 1 ? 0 : size == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
			int side, mrow, mtr;
			if(size == 1) vidx[1][reg][0] = (u16)(VFPU_INDEX(reg) * 4);
			else for(i = 0; i < size; i++)
				vidx[size][reg][i] = (u16)(4 * (transpose ? mtx + col + ((row + i) & 3) * 4 : mtx + col * 4 + ((row + i) & 3)));
			/* Matrices */
			mtr = transpose;
			switch(size){
			case 1: mtr = 0; mrow = (reg >> 5) & 3; side = 1; break;
			case 2: mrow = (reg >> 5) & 2; side = 2; break;
			case 3: mrow = (reg >> 6) & 1; side = 3; break;
			default: mrow = (reg >> 5) & 2; side = 4; break;
			}
			for(j = 0; j < side; j++)
				for(i = 0; i < side; i++)
					midx[size][reg][j * 4 + i] = (u16)(4 * (mtx + (mtr ? ((mrow + i) & 3) * 4 + ((col + j) & 3)
					                                                     : ((col + j) & 3) * 4 + ((mrow + i) & 3))));
		}
		mcont4[reg] = vcont4[reg] = (s16)midx[4][reg][0];
		for(i = 1; i < 16; i++) if(midx[4][reg][i] != midx[4][reg][0] + 4 * i) mcont4[reg] = -1;
		for(i = 1; i < 4; i++) if(vidx[4][reg][i] != vidx[4][reg][0] + 4 * i) vcont4[reg] = -1;
	}
}

static inline void read_vector(float *rd, VectorSize size, int reg){
	const u16 *ix = vidx[size][reg];
	switch(size){
	case V_Quad: rd[3] = VF(ix[3]); /* fallthrough */
	case V_Triple: rd[2] = VF(ix[2]); /* fallthrough */
	case V_Pair: rd[1] = VF(ix[1]); /* fallthrough */
	default: rd[0] = VF(ix[0]); break;
	}
}

static inline void write_vector(const float *rd, VectorSize size, int reg){
	const u16 *ix = vidx[size][reg];
	const u32 mask = CTRL[VFPU_CTRL_DPREFIX] >> 8;
	int i;
	if(__builtin_expect(!(mask & 0xF), 1)){
		switch(size){
		case V_Quad: VF(ix[3]) = rd[3]; /* fallthrough */
		case V_Triple: VF(ix[2]) = rd[2]; /* fallthrough */
		case V_Pair: VF(ix[1]) = rd[1]; /* fallthrough */
		default: VF(ix[0]) = rd[0]; break;
		}
		return;
	}
	for(i = 0; i < (int)size; i++)
		if(!((mask >> i) & 1)) VF(ix[i]) = rd[i];
}

static void read_matrix(float *rd, MatrixSize size, int reg){
	const u16 *ix = midx[size][reg];
	int i, j;
	if(size == M_4x4){
		for(i = 0; i < 16; i++) rd[i] = VF(ix[i]);
		return;
	}
	for(j = 0; j < (int)size; j++)
		for(i = 0; i < (int)size; i++) rd[j * 4 + i] = VF(ix[j * 4 + i]);
}

static void write_matrix(const float *rd, MatrixSize size, int reg){
	const u16 *ix = midx[size][reg];
	const int side = (int)size;
	int i, j;
	if(size == M_4x4 && !write_mask()){
		for(i = 0; i < 16; i++) VF(ix[i]) = rd[i];
		return;
	}
	/* La máscara solo afecta a la última fila (o columna) */
	for(j = 0; j < side; j++)
		for(i = 0; i < side; i++){
			if(j == side - 1 && write_masked(i)) continue;
			VF(ix[j * 4 + i]) = rd[j * 4 + i];
		}
}

static void vector_regs(u8 regs[4], VectorSize n, int reg){
	int mtx = (reg >> 2) & 7, col = reg & 3, row = 0, length = 0, transpose = (reg >> 5) & 1, i;
	switch(n){
	case V_Single: transpose = 0; row = (reg >> 5) & 3; length = 1; break;
	case V_Pair: row = (reg >> 5) & 2; length = 2; break;
	case V_Triple: row = (reg >> 6) & 1; length = 3; break;
	case V_Quad: row = (reg >> 5) & 2; length = 4; break;
	default: break;
	}
	for(i = 0; i < length; i++){
		int index = mtx * 4;
		if(transpose) index += ((row + i) & 3) + col * 32;
		else index += col + ((row + i) & 3) * 32;
		regs[i] = (u8)index;
	}
}

/* --- Prefijos ---------------------------------------------------------------------------- */

static void apply_prefix_st(float *r, u32 data, VectorSize size, float invalid){
	static const float constants[8] = { 0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f };
	float orig[4];
	int n, i;
	if(data == 0xE4) return;
	n = nelem(size);
	orig[0] = orig[1] = orig[2] = orig[3] = invalid;
	for(i = 0; i < n; i++) orig[i] = r[i];
	for(i = 0; i < n; i++){
		int regnum = (data >> (i * 2)) & 3, abs_ = (data >> (8 + i)) & 1;
		int negate = (data >> (16 + i)) & 1, constant = (data >> (12 + i)) & 1;
		u32 bits;
		if(!constant){
			r[i] = orig[regnum];
			if(abs_) r[i] = u2f(f2u(r[i]) & 0x7FFFFFFFu);
		} else r[i] = constants[regnum + (abs_ << 2)];
		if(negate){ bits = f2u(r[i]) ^ 0x80000000u; r[i] = u2f(bits); }
	}
}

static inline void swizzle_s(float *v, VectorSize size, float invalid){ apply_prefix_st(v, CTRL[VFPU_CTRL_SPREFIX], size, invalid); }
static inline void swizzle_t(float *v, VectorSize size, float invalid){ apply_prefix_st(v, CTRL[VFPU_CTRL_TPREFIX], size, invalid); }

static inline float clampf(float v, float lo, float hi){
	/* NaN se conserva y -0.0 pasa a +0.0 si lo es +0.0 */
	return v >= hi ? hi : (v <= lo ? lo : v);
}

static void apply_prefix_d(float *v, VectorSize size, int only_write_mask){
	u32 data = CTRL[VFPU_CTRL_DPREFIX];
	int n, i;
	if(!data || only_write_mask) return;
	n = nelem(size);
	for(i = 0; i < n; i++){
		int sat = (data >> (i * 2)) & 3;
		if(sat == 1) v[i] = clampf(v[i], 0.0f, 1.0f);
		else if(sat == 3) v[i] = clampf(v[i], -1.0f, 1.0f);
	}
}

static int last_lane_swizzle_invalid(int ctrl){
	const u32 prefix = CTRL[ctrl];
	return (prefix & 3) != 0 && (prefix & (1 << 12)) == 0;
}

/* Un swizzle fuera del vector deja el resultado en 0 (en algunas operaciones) */
static void retain_invalid_swizzle_st(float *d, VectorSize sz){
	u32 sp = CTRL[VFPU_CTRL_SPREFIX], tp = CTRL[VFPU_CTRL_TPREFIX];
	int n = nelem(sz), i;
	if(sp == 0xE4 && tp == 0xE4) return;
	for(i = 0; i < n; i++){
		int ss = (sp >> (i + i)) & 3, st = (tp >> (i + i)) & 3;
		int cs = (sp >> (12 + i)) & 1, ct = (tp >> (12 + i)) & 1;
		if((ss >= n && !cs) || (st >= n && !ct)) d[i] = 0.0f;
	}
}

/* El prefijo D solo se aplica al último elemento (máscara y saturación) */
static void dprefix_last_only(int n){
	u32 lastmask = (CTRL[VFPU_CTRL_DPREFIX] & (1 << 8)) << (n - 1);
	u32 lastsat = (CTRL[VFPU_CTRL_DPREFIX] & 3) << (n + n - 2);
	CTRL[VFPU_CTRL_DPREFIX] = lastmask | lastsat;
}

static inline void eat_prefixes(void){
	CTRL[VFPU_CTRL_SPREFIX] = 0xE4;
	CTRL[VFPU_CTRL_TPREFIX] = 0xE4;
	CTRL[VFPU_CTRL_DPREFIX] = 0;
}

/* --- Funciones exactas (interpolador cuadrático de la PSP) ---------------------------- */

static inline int clz32_nonzero(u32 v){ return __builtin_clz(v); }

static inline s32 vf_square(u32 x2){
	const s32 t = abs((s32)(x2 >> 6) - 512);
	return (t * t + 255) >> 8;
}

static inline s32 vf_interp(const VfpuSegment *seg, u32 x2){
	return seg->c0 + (s32)(((s64)seg->m * x2) >> 17) + ((seg->n * vf_square(x2)) >> 9);
}

static inline u32 vf_interp_bits(const VfpuSegment *segments, u32 index){
	const VfpuSegment *seg = &segments[index >> 16];
	return (((u32)(seg->e - 1) << 23) + (u32)vf_interp(seg, index & 0xFFFF)) & ~3u;
}

static inline u32 vf_sin_fixed(u32 arg){
	const VfpuSegment *seg;
	u32 y, v;
	if(arg == 0u) return 0u;
	if(arg == 0x00800000u) return 0x10000000u;
	y = 0x00800000u - arg;
	seg = &vfpu_sin_segments[y >> 16];
	v = (u32)vf_interp(seg, y & 0xFFFF) & ~3u;
	return seg->e >= 122 ? v << (seg->e - 122) : v >> (122 - seg->e);
}

static inline int vf_sin_reduce(u32 bits, u32 *angle, int *odd){
	const u32 exponent = (bits >> 23) & 0xFFu;
	u32 significand = (bits & 0x007FFFFFu) | 0x00800000u;
	if(exponent == 0xFFu) return 0;
	if(exponent < 0x7Fu){
		if(exponent < 0x7Fu - 23u) significand = 0u;
		else significand >>= (0x7F - exponent);
	} else if(exponent > 0x7Fu){
		if(exponent - 0x7Fu >= 25u && exponent - 0x7Fu < 32u) significand = 0u;
		else if((exponent & 0x9Fu) == 0x9Fu) significand = 0u;
		else significand <<= ((exponent - 0x7Fu) & 31);
	}
	*odd = (significand >> 24) & 1;
	*angle = significand & 0x00FFFFFFu;
	return 1;
}

static inline int clz64_nonzero(u64 v){
	return (v >> 32) != 0 ? clz32_nonzero((u32)(v >> 32)) : 32 + clz32_nonzero((u32)v);
}

static inline u32 fixed_to_bits(u64 v, int frac_bits, int negative){
	const u32 sign = negative ? 0x80000000u : 0u;
	int top;
	u64 significand;
	if(v == 0) return sign;
	top = 63 - clz64_nonzero(v);
	significand = top <= 23 ? v << (23 - top) : v >> (top - 23);
	return sign + ((u32)(top - frac_bits + 127) << 23) + ((u32)significand & 0x007FFFFFu);
}

static inline u32 sin_from_reduced(u32 angle, int negate){
	if(angle > 0x00800000u) angle = 0x01000000u - angle;
	return fixed_to_bits(vf_sin_fixed(angle), 28, negate);
}

static inline u32 cos_from_reduced(u32 angle, int negate){
	if(angle >= 0x00800000u){
		angle = 0x01000000u - angle;
		negate = !negate;
	}
	return fixed_to_bits(vf_sin_fixed(0x00800000u - angle), 28, negate);
}

float vfpu_sin(float x){
	u32 bits = f2u(x), angle;
	int odd;
	if(!vf_sin_reduce(bits, &angle, &odd)) return u2f((bits & 0x80000000u) ^ 0x7F800001u);
	return u2f(sin_from_reduced(angle, (int)(bits >> 31) != odd));
}

float vfpu_cos(float x){
	u32 bits = f2u(x), angle;
	int odd;
	if(!vf_sin_reduce(bits, &angle, &odd)) return u2f(0x7F800001u);
	return u2f(cos_from_reduced(angle, odd));
}

static void vfpu_sincos(float a, float *s, float *c){
	u32 bits = f2u(a), angle;
	int odd;
	if(!vf_sin_reduce(bits, &angle, &odd)){
		*s = u2f((bits & 0x80000000u) ^ 0x7F800001u);
		*c = u2f(0x7F800001u);
	} else {
		*s = u2f(sin_from_reduced(angle, (int)(bits >> 31) != odd));
		*c = u2f(cos_from_reduced(angle, odd));
	}
}

/* Los caminos rápidos de rcp, rsqrt y sqrt (VFPUFastSegment de PPSSPP) */
static inline u32 fast_interp(const VfpuSegment *segments, int bias, u32 w, u32 exponent){
	const VfpuSegment *seg = &segments[(w >> 16) & 0x7F];
	const u32 k = (u32)seg->c0 + ((u32)(seg->e - 1 + bias) << 23);
	const u32 x2 = w & 0xFFFF;
	const u32 linear = (u32)(((s64)seg->m * x2) >> 17);
	const u32 square = (u32)((seg->n * vf_square(x2)) >> 9);
	return (k + exponent + linear + square) & ~3u;
}

static inline int rcp_is_fast(u32 bits){ return (bits << 1) - 0x01000000u <= 0xFC000000u; }
static inline int sqrt_is_fast(u32 bits){ return bits - 0x00800000u < 0x7F000000u; }

float vfpu_sqrt(float x){
	u32 bits = f2u(x);
	if(sqrt_is_fast(bits)){
		const u32 w = (bits + 0x00800000u) >> 1;
		bits = fast_interp(vfpu_sqrt_segments, -64, w, w & 0x7F800000u);
	} else if((bits & 0x7FFFFFFFu) < 0x00800000u) bits = 0;
	else bits = bits == 0x7F800000u ? 0x7F800000u : 0x7F800001u;
	return u2f(bits);
}

float vfpu_rsqrt(float x){
	u32 bits = f2u(x);
	if(sqrt_is_fast(bits)){
		const u32 w = (bits + 0x00800000u) >> 1;
		bits = fast_interp(vfpu_rsqrt_segments, 64, w, 0u - (w & 0x7F800000u));
	} else if((bits & 0x7FFFFFFFu) < 0x00800000u) bits = (bits & 0x80000000u) | 0x7F800000u;
	else bits = (bits >> 31) ? 0xFF800001u : (bits > 0x7F800000u ? 0x7F800001u : 0u);
	return u2f(bits);
}

float vfpu_rcp(float x){
	u32 bits = f2u(x);
	if(rcp_is_fast(bits)) bits = fast_interp(vfpu_rcp_segments, 127, bits, 0u - (bits & 0xFF800000u));
	else {
		const u32 a = bits & 0x7FFFFFFFu;
		bits = (bits & 0x80000000u) ^ (a < 0x00800000u ? 0x7F800000u : (a > 0x7F800000u ? 0x7F800001u : 0u));
	}
	return u2f(bits);
}

static inline u32 asin_fixed(u32 x){
	const VfpuSegment *seg;
	u32 v;
	if(x == 0u) return 0u;
	if(x == 1u << 23) return 1u << 30;
	seg = &vfpu_asin_segments[x >> 16];
	v = (u32)vf_interp(seg, x & 0xFFFF) & ((x >> 16) == 0 ? ~1u : ~3u);
	return seg->e >= 120 ? v << (seg->e - 120) : v >> (120 - seg->e);
}

float vfpu_asin(float x){
	const u32 bits = f2u(x), sign = bits & 0x80000000u, a = bits & 0x7FFFFFFFu;
	u32 result;
	if(a > 0x3F800000u) result = 0x7F800001u ^ sign;
	else {
		const int e = (int)(a >> 23);
		const u32 significand = (a & 0x007FFFFFu) | 0x00800000u;
		const u32 fixed = e < 104 ? 0 : significand >> (127 - e);
		result = fixed_to_bits(asin_fixed(fixed), 30, sign != 0);
	}
	return u2f(result);
}

static u32 exp2_bits(u32 bits){
	const u32 a = bits & 0x7FFFFFFFu;
	const int negative = (bits >> 31) != 0;
	int e;
	u32 significand, frac, sig_bits;
	s32 fixed;
	if(a <= 0x007FFFFFu) return 0x3F800000u;
	if(a > 0x7F800000u) return 0x7F800001u;
	if(negative && a >= 0x42FC0000u) return 0;
	if(!negative && a >= 0x43000000u) return 0x7F800000u;
	e = (int)(a >> 23);
	significand = (a & 0x007FFFFFu) | 0x00800000u;
	fixed = e >= 127 ? (s32)(significand << (e - 127)) : (e < 104 ? 0 : (s32)(significand >> (127 - e)));
	if(negative) fixed = -fixed - 1;
	frac = (u32)fixed & 0x007FFFFFu;
	sig_bits = frac == 0 ? 0 : vf_interp_bits(vfpu_exp2_segments, frac) - 0x3F800000u;
	return 0x3F800000u + ((u32)fixed & 0xFF800000u) + sig_bits;
}

float vfpu_exp2(float x){ return u2f(exp2_bits(f2u(x))); }
static float vfpu_rexp2(float x){ return u2f(exp2_bits(f2u(x) ^ 0x80000000u)); }

static u32 log2_bits(u32 bits){
	s32 exponent, p, n, c0, square;
	u32 index, x2;
	int d;
	s64 step, m, y, frac, sum;
	const VfpuSegment *seg;
	if((bits & 0x7FFFFFFFu) <= 0x007FFFFFu) return 0xFF800000u;
	if(bits & 0x80000000u) return 0x7F800001u;
	if((bits >> 23) == 255u) return 0x7F800000u + ((bits & 0x007FFFFFu) != 0);
	exponent = (s32)(bits >> 23) - 127;
	index = bits & 0x007FFFFFu;
	d = exponent < 0 ? 7 : exponent < 2 ? 0 : 31 - clz32_nonzero((u32)exponent);
	p = 1 << d;
	step = (s64)(4 << d) << 17;
	seg = &vfpu_log2_segments[index >> 16];
	x2 = index & 0xFFFF;
	n = seg->n < 0 ? -(-seg->n & ~(p - 1)) : (seg->n & ~(p - 1));
	c0 = (seg->c0 + 2 * (seg->n - n)) & ~(p - 1);
	square = ((n * vf_square(x2)) >> 9) & ~(p - 1);
	m = seg->m & ~((4 << d) - 1);
	y = ((s64)(c0 + square) << 17) + m * x2;
	frac = exponent >= 0 ? (y & ~(step - 1)) : -(-y & ~(step - 1));
	sum = ((s64)exponent << 41) + frac;
	return fixed_to_bits((u64)(sum < 0 ? -sum : sum), 41, exponent < 0);
}

float vfpu_log2(float x){ return u2f(log2_bits(f2u(x))); }

/* --- Medio float, generador aleatorio ---------------------------------------------------- */

static u32 vfpu_h2f(u16 h){
	u32 sign = (u32)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
	if(exp == 0) return sign;
	if(exp == 31) return sign | 0x7F800000u | mant;
	return sign | ((exp + 112) << 23) | (mant << 13);
}

static u16 vfpu_f2h(u32 f){
	u16 sign = (u16)((f >> 16) & 0x8000);
	u32 exp = (f >> 23) & 0xFF, mant = f & 0x7FFFFF;
	if(exp == 255) return (u16)(sign | 0x7C00 | (mant & 0x3FF));
	if(exp < 113) return sign;
	if(exp >= 143) return (u16)(sign | 0x7C00);
	return (u16)(sign | ((exp - 112) << 10) | (mant >> 13));
}

static float float16_to_float32(u16 l){
	u32 sign = (l >> 15) & 1, fraction = l & 0x3FF;
	int exponent = (l >> 10) & 0x1F;
	if(exponent == 0x1F) return u2f((sign << 31) | (255u << 23) | fraction);
	if(exponent == 0 && fraction == 0) return sign ? -0.0f : 0.0f;
	if(exponent == 0){
		do { fraction <<= 1; exponent--; } while(!(fraction & 0x400));
		fraction &= 0x3FF;
	}
	return u2f((sign << 31) | ((u32)(exponent + 112) << 23) | (fraction << 13));
}

static void vrnd_init(u32 seed, u32 *rcx){
	int i;
	for(i = 0; i < 8; ++i)
		rcx[i] = 0x3F800000u | ((seed >> ((i / 4) * 16)) & 0xFFFFu) | (((seed >> (4 * i)) & 0xF) << 16);
}

static u32 vrnd_generate(u32 *rcx){
	u32 A = (rcx[0] & 0xFFFFu) | (rcx[4] << 16), B = (rcx[1] & 0xFFFFu) | (rcx[5] << 16);
	u32 C = (rcx[2] & 0xFFFFu) | (rcx[6] << 16), D = (rcx[3] & 0xFFFFu) | (rcx[7] << 16);
	u32 E = (((rcx[0] >> 16) & 0xF) << 0) | (((rcx[1] >> 16) & 0xF) << 4) | (((rcx[2] >> 16) & 0xF) << 8) |
	        (((rcx[3] >> 16) & 0xF) << 12) | (((rcx[4] >> 16) & 0xF) << 16) | (((rcx[5] >> 16) & 0xF) << 20) |
	        (((rcx[6] >> 16) & 0xF) << 24) | (((rcx[7] >> 16) & 0xF) << 28);
	u32 t;
	A = 69069u * A + 1u;
	B ^= B << 13;
	B ^= B >> 17;
	B ^= B << 5;
	t = 2u * D + C + E;
	E = (u32)(((u64)C + (u64)(D >> 1) + (u64)E) >> 32);
	C = D;
	D = t;
	rcx[0] = 0x3F800000u | (((E >> 0) & 0xF) << 16) | (A & 0xFFFFu);
	rcx[1] = 0x3F800000u | (((E >> 4) & 0xF) << 16) | (B & 0xFFFFu);
	rcx[2] = 0x3F800000u | (((E >> 8) & 0xF) << 16) | (C & 0xFFFFu);
	rcx[3] = 0x3F800000u | (((E >> 12) & 0xF) << 16) | (D & 0xFFFFu);
	rcx[4] = 0x3F800000u | (((E >> 16) & 0xF) << 16) | (A >> 16);
	rcx[5] = 0x3F800000u | (((E >> 20) & 0xF) << 16) | (B >> 16);
	rcx[6] = 0x3F800000u | (((E >> 24) & 0xF) << 16) | (C >> 16);
	rcx[7] = 0x3F800000u | (((E >> 28) & 0xF) << 16) | (D >> 16);
	return A + B + D;
}

/* --- Control ---------------------------------------------------------------------------- */

static int ctrl_mask(int reg, u32 *mask){
	switch(reg){
	case VFPU_CTRL_SPREFIX: case VFPU_CTRL_TPREFIX: *mask = 0x000FFFFF; return 1;
	case VFPU_CTRL_DPREFIX: *mask = 0x00000FFF; return 1;
	case VFPU_CTRL_CC: *mask = 0x0000003F; return 1;
	case VFPU_CTRL_INF4: *mask = 0xFFFFFFFF; return 1;
	case VFPU_CTRL_RSV5: case VFPU_CTRL_RSV6: case VFPU_CTRL_REV: return 0;   /* solo lectura */
	default:
		if(reg >= VFPU_CTRL_RCX0 && reg <= VFPU_CTRL_RCX7){ *mask = 0x000FFFFF; return 1; }
		return 0;
	}
}

static u32 ctrl_set_bits(int reg){
	return reg >= VFPU_CTRL_RCX0 && reg <= VFPU_CTRL_RCX7 ? 0x3F800000u : 0;
}

void vfpu_reset(VfpuState *st){
	static int tables_ready;
	int i;
	if(!tables_ready){ build_tables(); tables_ready = 1; }
	/* Un hilo nuevo empieza con los registros a NaN */
	for(i = 0; i < 128; i++) st->vi[i] = 0x7F800001u;
	memset(st->ctrl, 0, sizeof(st->ctrl));
	st->ctrl[VFPU_CTRL_SPREFIX] = 0xE4;
	st->ctrl[VFPU_CTRL_TPREFIX] = 0xE4;
	st->ctrl[VFPU_CTRL_DPREFIX] = 0;
	st->ctrl[VFPU_CTRL_CC] = 0x3F;
	st->ctrl[VFPU_CTRL_REV] = 0x7772CEABu;
	st->ctrl[VFPU_CTRL_RCX0] = 0x3F800001u;
	st->ctrl[VFPU_CTRL_RCX1] = 0x3F800002u;
	st->ctrl[VFPU_CTRL_RCX2] = 0x3F800004u;
	st->ctrl[VFPU_CTRL_RCX3] = 0x3F800008u;
	st->ctrl[VFPU_CTRL_RCX4] = 0x3F800000u;
	st->ctrl[VFPU_CTRL_RCX5] = 0x3F800000u;
	st->ctrl[VFPU_CTRL_RCX6] = 0x3F800000u;
	st->ctrl[VFPU_CTRL_RCX7] = 0x3F800000u;
}

/* --- Memoria ---------------------------------------------------------------------------- */

static int valid4(u32 addr, u32 pc, u32 op){
	if((addr & 3) || !mem_valid(addr, 4)){
		cpu_fault("acceso a memoria invalido (VFPU)", addr, op);
		(void)pc;
		return 0;
	}
	return 1;
}

/* lv.s / sv.s (sin máscara de escritura, como en la PSP) */
static NOINLINE void op_sv(u32 op, u32 pc){
	const int vt = ((op >> 16) & 0x1F) | ((op & 3) << 5);
	const u32 addr = cpu.r[(op >> 21) & 0x1F] + (u32)(s32)(s16)(op & 0xFFFC);
	if((op >> 26) == 0x32){
		const u8 *p = (addr & 3) ? NULL : mem_ptr_r(addr, 4);
		if(!p){ valid4(addr, pc, op); return; }
		vfpu.vi[VFPU_INDEX(vt)] = rd_le32(p);
	} else {
		u8 *p = (addr & 3) ? NULL : mem_ptr(addr, 4);
		if(!p){ valid4(addr, pc, op); return; }
		wr_le32(p, vfpu.vi[VFPU_INDEX(vt)]);
	}
}

static NOINLINE void op_svq(u32 op, u32 pc){
	const int vt = ((op >> 16) & 0x1F) | ((op & 1) << 5);
	const u32 addr = cpu.r[(op >> 21) & 0x1F] + (u32)(s32)(s16)(op & 0xFFFC);
	FloatBits d;
	int i;
	switch(op >> 26){
	case 0x36: {   /* lv.q: un solo acceso a la memoria emulada */
		const u8 *p;
		if(addr & 0xF){ cpu_fault("lv.q desalineado", addr, op); return; }
		p = mem_ptr_r(addr, 16);
		if(!p){ valid4(addr, pc, op); return; }
		d.u[0] = rd_le32(p);
		d.u[1] = rd_le32(p + 4);
		d.u[2] = rd_le32(p + 8);
		d.u[3] = rd_le32(p + 12);
		write_vector(d.f, V_Quad, vt);
		break;
	}
	case 0x3E: {   /* sv.q */
		u8 *p;
		if(addr & 0xF){ cpu_fault("sv.q desalineado", addr, op); return; }
		p = mem_ptr(addr, 16);
		if(!p){ valid4(addr, pc, op); return; }
		read_vector(d.f, V_Quad, vt);
		wr_le32(p, d.u[0]);
		wr_le32(p + 4, d.u[1]);
		wr_le32(p + 8, d.u[2]);
		wr_le32(p + 12, d.u[3]);
		break;
	}
	case 0x35: {   /* lvl.q / lvr.q */
		int offset = (addr >> 2) & 3;
		if(!valid4(addr, pc, op)) return;
		read_vector(d.f, V_Quad, vt);
		if(!(op & 2)) for(i = 0; i < offset + 1; i++) d.u[3 - i] = mem_read32(addr - 4 * (u32)i);
		else for(i = 0; i < (3 - offset) + 1; i++) d.u[i] = mem_read32(addr + 4 * (u32)i);
		write_vector(d.f, V_Quad, vt);
		break;
	}
	case 0x3D: {   /* svl.q / svr.q */
		int offset = (addr >> 2) & 3;
		if(!valid4(addr, pc, op)) return;
		read_vector(d.f, V_Quad, vt);
		if(!(op & 2)) for(i = 0; i < offset + 1; i++) mem_write32(addr - 4 * (u32)i, d.u[3 - i]);
		else for(i = 0; i < (3 - offset) + 1; i++) mem_write32(addr + 4 * (u32)i, d.u[i]);
		break;
	}
	}
}

/* --- COP2: mfv/mtv ---------------------------------------------------------------------- */

static NOINLINE void op_mftv(u32 op){
	int imm = op & 0xFF, rt = (op >> 16) & 0x1F;
	u32 mask;
	switch((op >> 21) & 0x1F){
	case 3:   /* mfv / mfvc (rt = 0 sirve de barrera) */
		if(rt){
			if(imm < 128) cpu.r[rt] = vfpu.vi[VFPU_INDEX(imm)];
			else if(imm < 128 + VFPU_CTRL_MAX) cpu.r[rt] = CTRL[imm - 128];
		}
		break;
	case 7:   /* mtv / mtvc */
		if(imm < 128) vfpu.vi[VFPU_INDEX(imm)] = cpu.r[rt];
		else if(imm < 128 + VFPU_CTRL_MAX && ctrl_mask(imm - 128, &mask))
			CTRL[imm - 128] = (cpu.r[rt] & mask) | ctrl_set_bits(imm - 128);
		break;
	}
}

static NOINLINE void op_vmfvc(u32 op){
	int imm = (op >> 8) & 0x7F;
	vfpu.vi[VFPU_INDEX(VD)] = imm < VFPU_CTRL_MAX ? CTRL[imm] : 0;
}

static NOINLINE void op_vmtvc(u32 op){
	int imm = op & 0x7F;
	u32 mask;
	if(imm < VFPU_CTRL_MAX && ctrl_mask(imm, &mask))
		CTRL[imm] = (vfpu.vi[VFPU_INDEX(VS)] & mask) | ctrl_set_bits(imm);
}

/* --- Operaciones -------------------------------------------------------------------------- */

static NOINLINE void op_vpfx(u32 op){
	u32 data = op & 0x000FFFFF;
	int regnum = (op >> 24) & 3;
	if(regnum == VFPU_CTRL_DPREFIX) data &= 0x00000FFF;
	CTRL[VFPU_CTRL_SPREFIX + regnum] = data;
}

static NOINLINE void op_vmatrix_init(u32 op){
	static const float idt[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	static const float zero[16] = { 0 };
	static const float one[16] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
	int vd = VD;
	MatrixSize sz = mtx_size(op);
	const float *m;
	switch((op >> 16) & 0xF){
	case 3: m = idt; break;
	case 6: m = zero; break;
	case 7: m = one; break;
	default: eat_prefixes(); return;
	}
	if(CTRL[VFPU_CTRL_SPREFIX] & 0xF0F00){
		/* El prefijo S genera constantes, solo en la última fila */
		float prefixed[16];
		int off = (int)sz - 1;
		u32 add = 0;
		memcpy(prefixed, m, sizeof(prefixed));
		switch((op >> 16) & 0xF){
		case 3: add = MAKE_CONSTANTS(off == 0 ? C_ONE : C_ZERO, off == 1 ? C_ONE : C_ZERO, off == 2 ? C_ONE : C_ZERO, off == 3 ? C_ONE : C_ZERO); break;
		case 6: add = MAKE_CONSTANTS(C_ZERO, C_ZERO, C_ZERO, C_ZERO); break;
		case 7: add = MAKE_CONSTANTS(C_ONE, C_ONE, C_ONE, C_ONE); break;
		}
		apply_prefix_st(&prefixed[off * 4], rewrite_prefix(VFPU_CTRL_SPREFIX, ANY_SWIZZLE, add), V_Quad, 0.0f);
		write_matrix(prefixed, sz, vd);
	} else write_matrix(m, sz, vd);
	eat_prefixes();
}

static NOINLINE void op_vvector_init(u32 op){
	VectorSize sz = vec_size(op);
	float d[4];
	int c = ((op >> 16) & 0xF) == 7 ? C_ONE : C_ZERO;
	apply_prefix_st(d, rewrite_prefix(VFPU_CTRL_SPREFIX, ANY_SWIZZLE, MAKE_CONSTANTS(c, c, c, c)), sz, 0.0f);
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_viim(u32 op){
	float f[1];
	int type = (op >> 23) & 7;
	if(type == 6) f[0] = (float)(s16)(op & 0xFFFF);
	else f[0] = float16_to_float32((u16)(op & 0xFFFF));
	apply_prefix_d(f, V_Single, 0);
	write_vector(f, V_Single, VT);
	eat_prefixes();
}

static NOINLINE void op_vidt(u32 op){
	int vd = VD;
	VectorSize sz = vec_size(op);
	float f[4];
	int offmask = sz == V_Quad || sz == V_Triple ? 3 : 1, off = vd & offmask;
	u32 add = MAKE_CONSTANTS(off == (0 & offmask) ? C_ONE : C_ZERO, off == (1 & offmask) ? C_ONE : C_ZERO,
	                         off == (2 & offmask) ? C_ONE : C_ZERO, off == (3 & offmask) ? C_ONE : C_ZERO);
	apply_prefix_st(f, rewrite_prefix(VFPU_CTRL_SPREFIX, ANY_SWIZZLE, add), sz, 0.0f);
	apply_prefix_d(f, sz, 0);
	write_vector(f, sz, vd);
	eat_prefixes();
}

/* d[a * 4 + b] = sum_c S[b * 4 + c] * T[a * 4 + c], con el mismo orden de
   sumas que el camino general; S y T seguidos en memoria */
static void vmmul4_cont(const float *S, const float *T, float *d){
	int a, b;
	for(b = 0; b < 4; b++){
		const float s0 = S[b * 4], s1 = S[b * 4 + 1], s2 = S[b * 4 + 2], s3 = S[b * 4 + 3];
		for(a = 0; a < 4; a++)
			d[a * 4 + b] = (((0.0f + s0 * T[a * 4]) + s1 * T[a * 4 + 1]) + s2 * T[a * 4 + 2]) + s3 * T[a * 4 + 3];
	}
}

static void vmmul4_fast(u32 op){
	if(mcont4[VS] >= 0 && mcont4[VT] >= 0 && mcont4[VD] >= 0){
		const int md = mcont4[VD];
		if(md != mcont4[VS] && md != mcont4[VT]){
			/* Sin solape (cada matriz son 64 bytes alineados): directo */
			vmmul4_cont(&VF(mcont4[VS]), &VF(mcont4[VT]), &VF(md));
		} else {
			float d[16];
			int k;
			vmmul4_cont(&VF(mcont4[VS]), &VF(mcont4[VT]), d);
			for(k = 0; k < 16; k++) VF(md + 4 * k) = d[k];
		}
		return;
	}
	/* Lo más común: 4x4 sin prefijos. Mismo orden de sumas para cada
	   elemento; los cuatro de una columna van a la vez. */
	const u16 *xs = midx[4][VS], *xt = midx[4][VT], *xd = midx[4][VD];
	float s[16], d[16];
	int a, k;
	for(k = 0; k < 16; k++) s[k] = VF(xs[k]);
	for(a = 0; a < 4; a++){
		const float t0 = VF(xt[a * 4]), t1 = VF(xt[a * 4 + 1]);
		const float t2 = VF(xt[a * 4 + 2]), t3 = VF(xt[a * 4 + 3]);
		float d0 = 0.0f + s[0] * t0, d1 = 0.0f + s[4] * t0, d2 = 0.0f + s[8] * t0, d3 = 0.0f + s[12] * t0;
		d0 += s[1] * t1; d1 += s[5] * t1; d2 += s[9] * t1; d3 += s[13] * t1;
		d0 += s[2] * t2; d1 += s[6] * t2; d2 += s[10] * t2; d3 += s[14] * t2;
		d0 += s[3] * t3; d1 += s[7] * t3; d2 += s[11] * t3; d3 += s[15] * t3;
		d[a * 4] = d0; d[a * 4 + 1] = d1; d[a * 4 + 2] = d2; d[a * 4 + 3] = d3;
	}
	for(k = 0; k < 16; k++) VF(xd[k]) = d[k];
}

static NOINLINE void vmmul_general(u32 op){
	MatrixSize sz = mtx_size(op);
	int n = (int)sz, a, b, c;
	float s[16] = { 0 }, t[16] = { 0 }, d[16];
	read_matrix(s, sz, VS);
	read_matrix(t, sz, VT);
	for(a = 0; a < n; a++)
		for(b = 0; b < n; b++){
			float sum = 0.0f;
			if(a == n - 1 && b == n - 1){
				/* S y T se aplican al último producto */
				swizzle_s(&s[b * 4], V_Quad, 0.0f);
				swizzle_t(&t[a * 4], V_Quad, 0.0f);
				for(c = 0; c < 4; c++) sum += s[b * 4 + c] * t[a * 4 + c];
			} else for(c = 0; c < n; c++) sum += s[b * 4 + c] * t[a * 4 + c];
			d[a * 4 + b] = sum;
		}
	dprefix_last_only(n);
	apply_prefix_d(&d[4 * (n - 1)], V_Quad, 0);
	write_matrix(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vmmul(u32 op){
	if(mtx_size(op) == M_4x4 && FAST_OK()) vmmul4_fast(op);
	else vmmul_general(op);
}

static NOINLINE void op_vmscl(u32 op){
	float s[16] = { 0 }, t[4] = { 0 }, d[16];
	MatrixSize sz = mtx_size(op);
	int n = (int)sz, a, b, vt = VT, tlane = (vt >> 5) & 3;
	read_matrix(s, sz, VS);
	read_vector(t, V_Single, vt);
	for(a = 0; a < n - 1; a++)
		for(b = 0; b < n; b++) d[a * 4 + b] = s[a * 4 + b] * t[0];
	swizzle_s(&s[(n - 1) * 4], V_Quad, 0.0f);
	t[tlane] = t[0];
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, SWIZZLE(tlane, tlane, tlane, tlane)), V_Quad, 0.0f);
	for(b = 0; b < n; b++) d[(n - 1) * 4 + b] = s[(n - 1) * 4 + b] * t[b];
	apply_prefix_d(&d[(n - 1) * 4], V_Quad, 0);
	write_matrix(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vmmov(u32 op){
	float s[16] = { 0 };
	MatrixSize sz = mtx_size(op);
	int off = (int)sz - 1;
	read_matrix(s, sz, VS);
	swizzle_s(&s[off * 4], V_Quad, 0.0f);
	apply_prefix_d(&s[off * 4], V_Quad, 0);
	write_matrix(s, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vflush(u32 op){
	/* Todo 0xFC... es un nop, pero solo 0xFFFF0000 conserva los prefijos */
	if((op & 0xFFFF0000u) != 0xFFFF0000u) eat_prefixes();
}

/* Sin prefijos, ninguno de los ajustes del camino general hace nada */
static void vv2op_fast(u32 op, int optype){
	const VectorSize sz = vec_size(op);
	const int n = (int)sz;
	float s[4], d[4];
	int i;
	read_vector(s, sz, VS);
	switch(optype){
	case 0: write_vector(s, sz, VD); return;
	case 1: for(i = 0; i < n; i++) d[i] = fabsf(s[i]); break;
	case 2: for(i = 0; i < n; i++) d[i] = -s[i]; break;
	case 16: for(i = 0; i < n; i++) d[i] = vfpu_rcp(s[i]); break;
	case 17: for(i = 0; i < n; i++) d[i] = vfpu_rsqrt(s[i]); break;
	case 18: for(i = 0; i < n; i++) d[i] = vfpu_sin(s[i]); break;
	case 19: for(i = 0; i < n; i++) d[i] = vfpu_cos(s[i]); break;
	default: for(i = 0; i < n; i++) d[i] = vfpu_sqrt(s[i]); break;
	}
	write_vector(d, sz, VD);
}

static NOINLINE void vv2op_general(u32 op){
	float s[4], d[4];
	int optype = (op >> 16) & 0x1F, i;
	VectorSize sz = vec_size(op);
	int n = nelem(sz);
	read_vector(s, sz, VS);
	switch(optype){
	case 1: apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, 0, ABS4(1, 1, 1, 1)), sz, 0.0f); break;
	case 2: apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, 0, NEGATE4(1, 1, 1, 1)), sz, 0.0f); break;
	case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
		swizzle_s(&s[n - 1], V_Single, INFINITY);
		break;
	case 24: case 26:
		apply_prefix_st(&s[n - 1], rewrite_prefix(VFPU_CTRL_SPREFIX, NEGATE4(1, 0, 0, 0), 0), V_Single, -INFINITY);
		break;
	case 28:
		apply_prefix_st(&s[n - 1], rewrite_prefix(VFPU_CTRL_SPREFIX, NEGATE4(1, 0, 0, 0), 0), V_Single, INFINITY);
		break;
	default: swizzle_s(s, sz, 0.0f); break;
	}
	for(i = 0; i < n; i++){
		switch(optype){
		case 0: case 1: case 2: d[i] = s[i]; break;
		case 4: d[i] = s[i] <= 0 ? 0 : (s[i] > 1.0f ? 1.0f : s[i]); break;          /* vsat0 */
		case 5: d[i] = s[i] < -1.0f ? -1.0f : (s[i] > 1.0f ? 1.0f : s[i]); break;   /* vsat1 */
		case 16: d[i] = vfpu_rcp(s[i]); break;
		case 17: d[i] = vfpu_rsqrt(s[i]); break;
		case 18: d[i] = vfpu_sin(s[i]); break;
		case 19: d[i] = vfpu_cos(s[i]); break;
		case 20: d[i] = vfpu_exp2(s[i]); break;
		case 21: d[i] = vfpu_log2(s[i]); break;
		case 22: d[i] = vfpu_sqrt(s[i]); break;
		case 23: d[i] = vfpu_asin(s[i]); break;
		case 24: d[i] = -vfpu_rcp(s[i]); break;
		case 26: d[i] = -vfpu_sin(s[i]); break;
		case 28: d[i] = vfpu_rexp2(s[i]); break;
		default: d[i] = s[i]; break;
		}
	}
	switch(optype){
	case 5: apply_prefix_d(d, sz, 1); break;
	case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 26: case 28:
		if(last_lane_swizzle_invalid(VFPU_CTRL_SPREFIX)) d[n - 1] = 0.0f;
		dprefix_last_only(n);
		apply_prefix_d(d, sz, 0);
		break;
	default: apply_prefix_d(d, sz, 0); break;
	}
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vv2op(u32 op){
	const int optype = (op >> 16) & 0x1F;
	if(FAST_OK()){
		switch(optype){
		case 0: case 1: case 2: case 16: case 17: case 18: case 19: case 22:
			vv2op_fast(op, optype);
			return;
		}
	}
	vv2op_general(op);
}

static NOINLINE void op_vocp(u32 op){
	float s[4], t[4], d[4];
	VectorSize sz = vec_size(op);
	int i;
	read_vector(s, sz, VS);
	apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, 0, NEGATE4(1, 1, 1, 1)), sz, 0.0f);
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, MAKE_CONSTANTS(C_ONE, C_ONE, C_ONE, C_ONE)), sz, 0.0f);
	for(i = 0; i < nelem(sz); i++) d[i] = canon(t[i] + s[i]);
	retain_invalid_swizzle_st(d, sz);
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static float nanclamp01(float f){
	f = f <= 0.0f ? 0.0f : f;   /* nanmax */
	return f >= 1.0f ? 1.0f : f;
}

static NOINLINE void op_vsocp(u32 op){
	float s[4], t[4], d[4];
	VectorSize sz = vec_size(op), out = sz == V_Single ? V_Pair : sz == V_Pair ? V_Quad : V_Quad;
	read_vector(s, sz, VS);
	apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, ANY_SWIZZLE | NEGATE4(1, 1, 1, 1), SWIZZLE(0, 0, 1, 1) | NEGATE4(1, 0, 1, 0)), out, 0.0f);
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, MAKE_CONSTANTS(C_ONE, C_ZERO, C_ONE, C_ZERO)), out, 0.0f);
	d[0] = canon(nanclamp01(t[0] + s[0]));
	d[1] = canon(nanclamp01(t[1] + s[1]));
	if(out == V_Quad){
		d[2] = canon(nanclamp01(t[2] + s[2]));
		d[3] = canon(nanclamp01(t[3] + s[3]));
	}
	apply_prefix_d(d, sz, 1);
	write_vector(d, out, VD);
	eat_prefixes();
}

static NOINLINE void op_vsgn(u32 op){
	float s[4], t[4], d[4];
	VectorSize sz = vec_size(op);
	int n = nelem(sz), i;
	read_vector(s, sz, VS);
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, MAKE_CONSTANTS(C_ZERO, C_ZERO, C_ZERO, C_ZERO)), sz, 0.0f);
	if(n < 4) memcpy(&s[n], &t[n], sizeof(float) * (size_t)(4 - n));
	swizzle_s(s, V_Quad, 0.0f);
	for(i = 0; i < n; i++){
		u32 val = f2u(s[i] - t[i]);
		if((val & 0x7F800000u) == 0) d[i] = 0.0f;
		else d[i] = (val >> 31) == 0 ? 1.0f : -1.0f;
	}
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

/* s ya está en (-2^31, 2^31): sin floor/ceil, que en el Broadway son
   llamadas lentas (no tiene instrucción de redondeo) */
static inline s32 f2i_mode(double sv, int mode){
	const s32 t = (s32)sv;   /* hacia cero */
	s32 fl;
	double fr;
	switch(mode){
	case 17: return t;                        /* vf2iz */
	case 18: return t + ((double)t < sv);     /* vf2iu */
	case 19: return t - ((double)t > sv);     /* vf2id */
	default:                                  /* vf2in: al par más cercano */
		fl = t - ((double)t > sv);
		fr = sv - (double)fl;
		if(fr < 0.5) return fl;
		if(fr > 0.5) return fl + 1;
		return fl + (fl & 1);
	}
}

static NOINLINE void op_vf2i(u32 op){
	FloatBits s, d;
	int imm = (op >> 16) & 0x1F, i, mode = (op >> 21) & 0x1F;
	float mult = (float)(1u << imm);
	VectorSize sz = vec_size(op);
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	for(i = 0; i < nelem(sz); i++){
		double sv;
		if(s.f[i] != s.f[i]){ d.i[i] = 0x7FFFFFFF; continue; }
		sv = (double)(s.f[i] * mult);
		if(sv > (double)0x7FFFFFFF) d.i[i] = 0x7FFFFFFF;
		else if(sv <= -2147483648.0) d.u[i] = 0x80000000u;
		else d.i[i] = f2i_mode(sv, mode);
	}
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vi2f(u32 op){
	FloatBits s;
	float d[4];
	int imm = (op >> 16) & 0x1F, i;
	float mult = 1.0f / (float)(1u << imm);
	VectorSize sz = vec_size(op);
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	for(i = 0; i < nelem(sz); i++) d[i] = (float)s.i[i] * mult;
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vh2f(u32 op){
	FloatBits s, d;
	VectorSize sz = vec_size(op), out;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	if(sz == V_Single){
		out = V_Pair;
		d.u[0] = vfpu_h2f((u16)(s.u[0] & 0xFFFF));
		d.u[1] = vfpu_h2f((u16)(s.u[0] >> 16));
	} else {
		out = V_Quad;
		d.u[0] = vfpu_h2f((u16)(s.u[0] & 0xFFFF));
		d.u[1] = vfpu_h2f((u16)(s.u[0] >> 16));
		d.u[2] = vfpu_h2f((u16)(s.u[1] & 0xFFFF));
		d.u[3] = vfpu_h2f((u16)(s.u[1] >> 16));
	}
	apply_prefix_d(d.f, out, 0);
	write_vector(d.f, out, VD);
	eat_prefixes();
}

static NOINLINE void op_vf2h(u32 op){
	FloatBits s, d;
	VectorSize sz = vec_size(op), out;
	memset(&s, 0, sizeof(s));
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, V_Quad, 0.0f);
	retain_invalid_swizzle_st(s.f, V_Quad);
	if(sz == V_Single || sz == V_Pair){
		out = V_Single;
		d.u[0] = vfpu_f2h(s.u[0]) | ((u32)vfpu_f2h(s.u[1]) << 16);
	} else {
		out = V_Pair;
		d.u[0] = vfpu_f2h(s.u[0]) | ((u32)vfpu_f2h(s.u[1]) << 16);
		d.u[1] = vfpu_f2h(s.u[2]) | ((u32)vfpu_f2h(s.u[3]) << 16);
	}
	apply_prefix_d(d.f, out, 0);
	write_vector(d.f, out, VD);
	eat_prefixes();
}

static NOINLINE void op_vx2i(u32 op){
	FloatBits s, d;
	VectorSize sz = vec_size(op), oz = sz;
	int i;
	memset(&d, 0, sizeof(d));
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	switch((op >> 16) & 3){
	case 0: {   /* vuc2i */
		u32 value = s.u[0];
		for(i = 0; i < 4; i++){ d.u[i] = (u32)((value & 0xFF) * 0x01010101u) >> 1; value >>= 8; }
		oz = V_Quad;
		break;
	}
	case 1: {   /* vc2i */
		u32 value = s.u[0];
		d.u[0] = (value & 0xFF) << 24;
		d.u[1] = (value & 0xFF00) << 16;
		d.u[2] = (value & 0xFF0000) << 8;
		d.u[3] = value & 0xFF000000u;
		oz = V_Quad;
		break;
	}
	case 2:     /* vus2i */
	case 3: {   /* vs2i */
		int us = ((op >> 16) & 3) == 2;
		oz = V_Pair;
		if(sz == V_Quad || sz == V_Triple) sz = V_Pair;
		if(sz == V_Pair) oz = V_Quad;
		for(i = 0; i < nelem(sz); i++){
			u32 value = s.u[i];
			if(us){ d.u[i * 2] = (value & 0xFFFF) << 15; d.u[i * 2 + 1] = (value & 0xFFFF0000u) >> 1; }
			else { d.u[i * 2] = (value & 0xFFFF) << 16; d.u[i * 2 + 1] = value & 0xFFFF0000u; }
		}
		break;
	}
	}
	apply_prefix_d(d.f, oz, 0);
	write_vector(d.f, oz, VD);
	eat_prefixes();
}

static NOINLINE void op_vi2x(u32 op){
	FloatBits s, d;
	const VectorSize sz = vec_size(op);
	VectorSize oz = V_Single;
	int i;
	memset(&s, 0, sizeof(s));
	memset(&d, 0, sizeof(d));
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, V_Quad, 0.0f);
	switch((op >> 16) & 3){
	case 0:   /* vi2uc */
		for(i = 0; i < 4; i++){
			int v = s.i[i];
			if(v < 0) v = 0;
			v >>= 23;
			d.u[0] |= ((u32)v & 0xFF) << (i * 8);
		}
		break;
	case 1:   /* vi2c */
		for(i = 0; i < 4; i++) d.u[0] |= (s.u[i] >> 24) << (i * 8);
		break;
	case 2: case 3: {   /* vi2us / vi2s */
		int elems = (nelem(sz) + 1) / 2;
		for(i = 0; i < elems; i++){
			if(((op >> 16) & 3) == 2){
				int lo = s.i[i * 2], hi = s.i[i * 2 + 1];
				if(lo < 0) lo = 0;
				if(hi < 0) hi = 0;
				d.u[i] = (u32)(lo >> 15) | ((u32)(hi >> 15) << 16);
			} else d.u[i] = (s.u[i * 2] >> 16) | ((s.u[i * 2 + 1] >> 16) << 16);
		}
		oz = sz == V_Quad || sz == V_Triple ? V_Pair : V_Single;
		break;
	}
	}
	apply_prefix_d(d.f, oz, 0);
	write_vector(d.f, oz, VD);
	eat_prefixes();
}

static NOINLINE void op_color_conv(u32 op){
	FloatBits s, ov;
	VectorSize isz = vec_size(op);
	u16 colors[4];
	int i;
	read_vector(s.f, V_Quad, VS);
	swizzle_s(s.f, V_Quad, 0.0f);
	for(i = 0; i < 4; i++){
		u32 in = s.u[i];
		u16 col = 0;
		switch((op >> 16) & 3){
		case 1: col = (u16)((((in >> 24) & 0xFF) >> 4) << 12 | (((in >> 16) & 0xFF) >> 4) << 8 | (((in >> 8) & 0xFF) >> 4) << 4 | ((in & 0xFF) >> 4)); break;
		case 2: col = (u16)((((in >> 24) & 0xFF) >> 7) << 15 | (((in >> 16) & 0xFF) >> 3) << 10 | (((in >> 8) & 0xFF) >> 3) << 5 | ((in & 0xFF) >> 3)); break;
		case 3: col = (u16)((((in >> 16) & 0xFF) >> 3) << 11 | (((in >> 8) & 0xFF) >> 2) << 5 | ((in & 0xFF) >> 3)); break;
		}
		colors[i] = col;
	}
	ov.u[0] = (u32)colors[0] | ((u32)colors[1] << 16);
	ov.u[1] = (u32)colors[2] | ((u32)colors[3] << 16);
	apply_prefix_d(ov.f, V_Pair, 0);
	write_vector(ov.f, isz == V_Single ? V_Single : V_Pair, VD);
	eat_prefixes();
}

static NOINLINE void op_vdot(u32 op){
	const int neutral = FAST_OK();
	float s[4] = { 0 }, t[4] = { 0 }, d = 0.0f;
	VectorSize sz = vec_size(op);
	int i;
	if(neutral){
		/* Lo que falta hasta 4 suma 0 * 0 = +0, que convierte -0 en +0 */
		const u16 *xs = vidx[sz][VS], *xt = vidx[sz][VT];
		d = 0.0f + VF(xs[0]) * VF(xt[0]);
		for(i = 1; i < (int)sz; i++) d += VF(xs[i]) * VF(xt[i]);
		for(; i < 4; i++) d += 0.0f;
		VF(vidx[1][VD][0]) = d;
		return;
	}
	read_vector(s, sz, VS);
	read_vector(t, sz, VT);
	swizzle_s(s, V_Quad, 0.0f);
	swizzle_t(t, V_Quad, 0.0f);
	for(i = 0; i < 4; i++) d += s[i] * t[i];
	apply_prefix_d(&d, V_Single, 0);
	write_vector(&d, V_Single, VD);
	eat_prefixes();
}

static NOINLINE void op_vhdp(u32 op){
	float s[4] = { 0 }, t[4] = { 0 }, d, sum = 0.0f;
	VectorSize sz = vec_size(op);
	u32 remove, add;
	int i;
	read_vector(s, sz, VS);
	read_vector(t, sz, VT);
	swizzle_t(t, V_Quad, 0.0f);
	/* S fuerza la constante 1 en el último elemento */
	switch(sz){
	case V_Quad: remove = SWIZZLE(0, 0, 0, 3); add = MAKE_CONSTANTS(C_NONE, C_NONE, C_NONE, C_ONE); break;
	case V_Triple: remove = SWIZZLE(0, 0, 3, 0); add = MAKE_CONSTANTS(C_NONE, C_NONE, C_ONE, C_NONE); break;
	case V_Pair: remove = SWIZZLE(0, 3, 0, 0); add = MAKE_CONSTANTS(C_NONE, C_ONE, C_NONE, C_NONE); break;
	default: remove = SWIZZLE(3, 0, 0, 0); add = MAKE_CONSTANTS(C_ONE, C_NONE, C_NONE, C_NONE); break;
	}
	apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, remove, add), V_Quad, 0.0f);
	for(i = 0; i < 4; i++) sum += s[i] * t[i];
	d = is_nan(sum) ? fabsf(sum) : sum;
	apply_prefix_d(&d, V_Single, 0);
	write_vector(&d, V_Single, VD);
	eat_prefixes();
}

static NOINLINE void op_vbfy(u32 op){
	float s[4] = { 0 }, t[4] = { 0 }, d[4];
	VectorSize sz = vec_size(op);
	read_vector(s, sz, VS);
	read_vector(t, sz, VS);
	if(op & 0x10000){   /* vbfy2 */
		apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, 0, NEGATE4(0, 0, 1, 1)), sz, 0.0f);
		apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, SWIZZLE(2, 3, 0, 1)), sz, 0.0f);
	} else {            /* vbfy1 */
		apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, 0, NEGATE4(0, 1, 0, 1)), sz, 0.0f);
		apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, SWIZZLE(1, 0, 3, 2)), sz, 0.0f);
	}
	d[0] = canon(flush(s[0]) + flush(t[0]));
	d[1] = canon(flush(s[1]) + flush(t[1]));
	d[2] = canon(flush(s[2]) + flush(t[2]));
	d[3] = canon(flush(s[3]) + flush(t[3]));
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

/* Orden de vsrt*: -NaN < -inf < reales < inf < NaN, con los denormales a 0 */
static inline s32 order_key(float f){
	u32 u = f2u(f), mag = u & 0x7FFFFFFFu;
	if(mag < 0x00800000u) mag = 0;
	return (u & 0x80000000u) ? -(s32)mag : (s32)mag;
}
static inline float vmin_(float a, float b){ return order_key(a) < order_key(b) ? a : b; }
static inline float vmax_(float a, float b){ return order_key(a) > order_key(b) ? a : b; }

static NOINLINE void op_vsrt(u32 op, int which){
	float s[4], t[4], d[4];
	VectorSize sz = vec_size(op);
	u32 add = (which == 1 || which == 3) ? SWIZZLE(1, 0, 3, 2) : SWIZZLE(3, 2, 1, 0);
	read_vector(s, sz, VS);
	swizzle_s(s, sz, 0.0f);
	read_vector(t, sz, VS);
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, SWIZZLE(3, 3, 3, 3), add), sz, 0.0f);
	switch(which){
	case 1: d[0] = vmin_(t[0], s[0]); d[1] = vmax_(s[1], t[1]); d[2] = vmin_(t[2], s[2]); d[3] = vmax_(s[3], t[3]); break;
	case 2: d[0] = vmin_(t[0], s[0]); d[1] = vmin_(t[1], s[1]); d[2] = vmax_(s[2], t[2]); d[3] = vmax_(s[3], t[3]); break;
	case 3: d[0] = vmax_(s[0], t[0]); d[1] = vmin_(t[1], s[1]); d[2] = vmax_(s[2], t[2]); d[3] = vmin_(t[3], s[3]); break;
	default: d[0] = vmax_(s[0], t[0]); d[1] = vmax_(s[1], t[1]); d[2] = vmin_(t[2], s[2]); d[3] = vmin_(t[3], s[3]); break;
	}
	retain_invalid_swizzle_st(d, sz);
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vcrs(u32 op){
	float s[4] = { 0 }, t[4] = { 0 }, d[4];
	VectorSize sz = vec_size(op);
	read_vector(s, sz, VS);
	read_vector(t, sz, VT);
	apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, SWIZZLE(3, 3, 3, 0), SWIZZLE(1, 2, 0, 0)), sz, 0.0f);
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, SWIZZLE(3, 3, 3, 0), SWIZZLE(2, 0, 1, 0)), sz, 0.0f);
	d[0] = s[0] * t[0];
	d[1] = s[1] * t[1];
	d[2] = s[2] * t[2];
	d[3] = s[3] * t[3];
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vdet(u32 op){
	float s[4] = { 0 }, t[4] = { 0 }, d[4];
	VectorSize sz = vec_size(op);
	read_vector(s, sz, VS);
	swizzle_s(s, V_Quad, 0.0f);
	read_vector(t, sz, VT);
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, SWIZZLE(3, 3, 0, 0), SWIZZLE(1, 0, 0, 0)), V_Quad, 0.0f);
	d[0] = s[0] * t[0] - s[1] * t[1];
	d[0] += s[2] * t[2] + s[3] * t[3];
	apply_prefix_d(d, V_Single, 0);
	write_vector(d, V_Single, VD);
	eat_prefixes();
}

static NOINLINE void op_vfad_vavg(u32 op, int avg){
	float s[4] = { 0 }, t[4] = { 0 }, d = 0.0f;
	VectorSize sz = vec_size(op);
	u32 remove = ANY_SWIZZLE, add;
	int i;
	read_vector(s, sz, VS);
	swizzle_s(s, V_Quad, 0.0f);
	if(!avg) add = MAKE_CONSTANTS(C_ONE, C_ONE, C_ONE, C_ONE);
	else {
		int c = sz == V_Single ? C_ZERO : sz == V_Pair ? C_HALF : sz == V_Triple ? C_THIRD : C_FOURTH;
		remove |= ABS4(1, 1, 1, 1);
		add = MAKE_CONSTANTS(c, c, c, c);
	}
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, remove, add), V_Quad, 0.0f);
	for(i = 0; i < 4; i++) d += s[i] * t[i];
	d = canon(d);
	apply_prefix_d(&d, V_Single, 0);
	write_vector(&d, V_Single, VD);
	eat_prefixes();
}

static NOINLINE void op_vscl(u32 op){
	const int neutral = FAST_OK();
	float s[4], t[4], d[4];
	VectorSize sz = vec_size(op);
	int vt = VT, tlane = (vt >> 5) & 3, n = nelem(sz), i;
	if(neutral){
		const u16 *xs = vidx[sz][VS];
		const float k = vfpu.v[VFPU_INDEX(vt)];
		for(i = 0; i < n; i++) d[i] = VF(xs[i]) * k;
		write_vector(d, sz, VD);
		return;
	}
	read_vector(s, sz, VS);
	swizzle_s(s, sz, 0.0f);
	t[tlane] = vfpu.v[VFPU_INDEX(vt)];
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, ANY_SWIZZLE, SWIZZLE(tlane, tlane, tlane, tlane)), V_Quad, 0.0f);
	for(i = 0; i < n; i++) d[i] = s[i] * t[i];
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vrnds(u32 op){
	FloatBits seed;
	seed.u[0] = vfpu.vi[VFPU_INDEX(VD)];
	swizzle_s(seed.f, V_Single, 0.0f);
	vrnd_init(seed.u[0], CTRL + VFPU_CTRL_RCX0);
	eat_prefixes();
}

static NOINLINE void op_vrndx(u32 op){
	FloatBits d;
	VectorSize sz = vec_size(op);
	int n = nelem(sz), i;
	/* Los valores se escriben al revés */
	for(i = n - 1; i >= 0; i--){
		switch((op >> 16) & 0x1F){
		case 1: d.u[i] = vrnd_generate(CTRL + VFPU_CTRL_RCX0); break;
		case 2: d.u[i] = 0x3F800000u | (vrnd_generate(CTRL + VFPU_CTRL_RCX0) & 0x007FFFFFu); break;
		default: d.u[i] = 0x40000000u | (vrnd_generate(CTRL + VFPU_CTRL_RCX0) & 0x007FFFFFu); break;
		}
	}
	dprefix_last_only(n);
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vrot(u32 op){
	float d[4] = { 0 }, sine, cosine;
	int vd = VD, vs = VS, imm = (op >> 16) & 0x1F, i;
	VectorSize sz = vec_size(op);
	int neg_sin = (imm & 0x10) != 0, sine_lane = (imm >> 2) & 3, cosine_lane = imm & 3;
	u32 dremove;
	if(CTRL[VFPU_CTRL_SPREFIX] == 0x000E4){
		vfpu_sincos(vfpu.v[VFPU_INDEX(vs)], &sine, &cosine);
		if(neg_sin) sine = -sine;
	} else {
		float s[4] = { 0 };
		read_vector(s, V_Single, vs);
		apply_prefix_st(s, rewrite_prefix(VFPU_CTRL_SPREFIX, NEGATE4(1, 0, 0, 0), NEGATE4(0, 0, 0, 0)), V_Single, 0.0f);
		cosine = vfpu_cos(vfpu.v[VFPU_INDEX(vs)]);
		sine = vfpu_sin(s[0]);
		if(neg_sin) sine = -sine;
		retain_invalid_swizzle_st(&sine, V_Single);
	}
	if(sine_lane == cosine_lane) for(i = 0; i < 4; i++) d[i] = sine;
	else d[sine_lane] = sine;
	if(((vd >> 2) & 7) == ((vs >> 2) & 7)){
		u8 dregs[4] = { 0 };
		int n = nelem(sz), written = 0;
		vector_regs(dregs, sz, vd);
		for(i = 0; i < n; i++)
			if(vs == dregs[i]){ d[cosine_lane] = vfpu_cos(d[i]); written = 1; break; }
		if(!written) d[cosine_lane] = cosine;
	} else d[cosine_lane] = cosine;
	dremove = (3u << (cosine_lane * 2)) | (1u << (8 + cosine_lane));
	CTRL[VFPU_CTRL_DPREFIX] &= 0xFFFFFu ^ dremove;
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, vd);
	eat_prefixes();
}

static void vtfm_fast(u32 op, int ins, int n){
	const int side = ins + 1, tn = n < side ? n : side;
	if(ins == 3 && n == 4 && mcont4[VS] >= 0 && vcont4[VT] >= 0){
		/* vtfm4.q con la matriz y el vector seguidos: lo más común */
		const float *S = &VF(mcont4[VS]), *T = &VF(vcont4[VT]);
		const float t0 = T[0], t1 = T[1], t2 = T[2], t3 = T[3];
		float d[4];
		int i;
		for(i = 0; i < 4; i++){
			float acc = S[i * 4] * t0;
			acc += S[i * 4 + 1] * t1;
			acc += S[i * 4 + 2] * t2;
			acc += S[i * 4 + 3] * t3;
			d[i] = acc;
		}
		write_vector(d, V_Quad, VD);
		return;
	}
	const u16 *xs = midx[side][VS], *xt = vidx[side][VT], *xd = vidx[side][VD];
	float t[4], d[4], acc;
	int i, k;
	for(k = 0; k < 4; k++) t[k] = k < side ? VF(xt[k]) : 0.0f;
	for(i = 0; i < ins; i++){
		const u16 *row = &xs[i * 4];
		acc = VF(row[0]) * t[0];
		for(k = 1; k < tn; k++) acc += VF(row[k]) * t[k];
		if(ins >= n) acc += VF(row[ins]);   /* vhtfm */
		d[i] = acc;
	}
	/* Última fila: lo que falta de T vale 0 (y 1 en vhtfm) */
	for(k = n; k < 4; k++) t[k] = (ins >= n && k == ins) ? 1.0f : 0.0f;
	{
		const u16 *row = &xs[ins * 4];
		float s1 = side > 1 ? VF(row[1]) : 0.0f, s2 = side > 2 ? VF(row[2]) : 0.0f;
		float s3 = side > 3 ? VF(row[3]) : 0.0f;
		acc = VF(row[0]) * t[0];
		acc += s1 * t[1];
		acc += s2 * t[2];
		acc += s3 * t[3];
		d[ins] = acc;
	}
	for(i = 0; i < side; i++) VF(xd[i]) = d[i];
}

static NOINLINE void vtfm_general(u32 op){
	int ins = (op >> 23) & 3, n = nelem(vec_size(op)), tn, i, k;
	float s[16] = { 0 }, t[4] = { 0 };
	FloatBits d;
	VectorSize sz = (VectorSize)(ins + 1);
	MatrixSize msz = (MatrixSize)(ins + 1);
	int cx = C_NONE, cy = n < 2 ? C_ZERO : C_NONE, cz = n < 3 ? C_ZERO : C_NONE, cw = n < 4 ? C_ZERO : C_NONE;
	tn = n < ins + 1 ? n : ins + 1;
	read_matrix(s, msz, VS);
	read_vector(t, sz, VT);
	for(i = 0; i < ins; i++){
		d.f[i] = s[i * 4] * t[0];
		for(k = 1; k < tn; k++) d.f[i] += s[i * 4 + k] * t[k];
		if(ins >= n) d.f[i] += s[i * 4 + ins];   /* vhtfm: la última columna por 1 */
	}
	/* S y T solo se aplican a la última fila; T pone las constantes 0/1 */
	swizzle_s(&s[ins * 4], V_Quad, 0.0f);
	if(ins >= n){
		if(ins == 1) cy = C_ONE;
		else if(ins == 2) cz = C_ONE;
		else if(ins == 3) cw = C_ONE;
	}
	apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, SWIZZLE(0, n < 2 ? 3 : 0, n < 3 ? 3 : 0, n < 4 ? 3 : 0),
	                                  MAKE_CONSTANTS(cx, cy, cz, cw)), V_Quad, 0.0f);
	d.f[ins] = s[ins * 4] * t[0];
	for(k = 1; k < 4; k++) d.f[ins] += s[ins * 4 + k] * t[k];
	{
		u32 lastmask = (CTRL[VFPU_CTRL_DPREFIX] & (1 << 8)) << ins;
		u32 lastsat = (CTRL[VFPU_CTRL_DPREFIX] & 3) << (ins + ins);
		CTRL[VFPU_CTRL_DPREFIX] = lastmask | lastsat;
	}
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vtfm(u32 op){
	if(FAST_OK()) vtfm_fast(op, (op >> 23) & 3, nelem(vec_size(op)));
	else vtfm_general(op);
}

static NOINLINE void op_vcst(u32 op){
	static float cst[32];
	static int ready;
	VectorSize sz = vec_size(op);
	float c, temp[4];
	if(!ready){
		/* Comprobadas en una PSP (PPSSPP) */
		cst[1] = 3.40282346638528859812e+38f;   /* FLT_MAX */
		cst[2] = sqrtf(2.0f);
		cst[3] = sqrtf(0.5f);
		cst[4] = 2.0f / sqrtf(3.14159265358979323846f);
		cst[5] = 2.0f / 3.14159265358979323846f;
		cst[6] = 1.0f / 3.14159265358979323846f;
		cst[7] = 3.14159265358979323846f / 4;
		cst[8] = 3.14159265358979323846f / 2;
		cst[9] = 3.14159265358979323846f;
		cst[10] = 2.71828182845904523536f;
		cst[11] = 1.44269504088896340736f;
		cst[12] = 0.434294481903251827651f;
		cst[13] = 0.693147180559945309417f;
		cst[14] = 2.30258509299404568402f;
		cst[15] = 2 * 3.14159265358979323846f;
		cst[16] = 3.14159265358979323846f / 6;
		cst[17] = log10f(2.0f);
		cst[18] = logf(10.0f) / logf(2.0f);
		cst[19] = sqrtf(3.0f) / 2.0f;
		ready = 1;
	}
	c = cst[(op >> 16) & 0x1F];
	temp[0] = temp[1] = temp[2] = temp[3] = c;
	apply_prefix_d(temp, sz, 0);
	write_vector(temp, sz, VD);
	eat_prefixes();
}

enum { VC_FL, VC_EQ, VC_LT, VC_LE, VC_TR, VC_NE, VC_GE, VC_GT, VC_EZ, VC_EN, VC_EI, VC_ES, VC_NZ, VC_NN, VC_NI, VC_NS };

static NOINLINE void vcmp_general(u32 op){
	const int neutral = FAST_OK();
	int cond = op & 0xF, i, cc = 0, or_val = 0, and_val = 1, affected = (1 << 4) | (1 << 5);
	VectorSize sz = vec_size(op);
	int n = nelem(sz);
	float s[4], t[4];
	if(neutral){
		const u16 *xs = vidx[sz][VS], *xt = vidx[sz][VT];
		for(i = 0; i < n; i++){ s[i] = VF(xs[i]); t[i] = VF(xt[i]); }
	} else {
		read_vector(s, sz, VS);
		swizzle_s(s, sz, 0.0f);
		read_vector(t, sz, VT);
		swizzle_t(t, sz, 0.0f);
	}
	if(cond == VC_GT || cond == VC_LT || cond == VC_GE || cond == VC_LE || cond == VC_EQ || cond == VC_NE){
		/* Las comparaciones normales: un bucle por condición */
		for(i = 0; i < n; i++){ s[i] = flush(s[i]); t[i] = flush(t[i]); }
		switch(cond){
		case VC_EQ: for(i = 0; i < n; i++) cc |= (s[i] == t[i]) << i; break;
		case VC_LT: for(i = 0; i < n; i++) cc |= (s[i] < t[i]) << i; break;
		case VC_LE: for(i = 0; i < n; i++) cc |= (s[i] <= t[i]) << i; break;
		case VC_NE: for(i = 0; i < n; i++) cc |= (s[i] != t[i]) << i; break;
		case VC_GE: for(i = 0; i < n; i++) cc |= (s[i] >= t[i]) << i; break;
		default: for(i = 0; i < n; i++) cc |= (s[i] > t[i]) << i; break;
		}
		affected |= (1 << n) - 1;
		or_val = cc != 0;
		and_val = cc == (1 << n) - 1;
		CTRL[VFPU_CTRL_CC] = (CTRL[VFPU_CTRL_CC] & ~(u32)affected) | ((u32)(cc | (or_val << 4) | (and_val << 5)) & (u32)affected);
		eat_prefixes();
		return;
	}
	for(i = 0; i < n; i++){
		int c;
		s[i] = flush(s[i]);
		t[i] = flush(t[i]);
		switch(cond){
		case VC_FL: c = 0; break;
		case VC_EQ: c = s[i] == t[i]; break;
		case VC_LT: c = s[i] < t[i]; break;
		case VC_LE: c = s[i] <= t[i]; break;
		case VC_TR: c = 1; break;
		case VC_NE: c = s[i] != t[i]; break;
		case VC_GE: c = s[i] >= t[i]; break;
		case VC_GT: c = s[i] > t[i]; break;
		case VC_EZ: c = s[i] == 0.0f || s[i] == -0.0f; break;
		case VC_EN: c = is_nan(s[i]); break;
		case VC_EI: c = is_inf(s[i]); break;
		case VC_ES: c = is_nan_or_inf(s[i]); break;
		case VC_NZ: c = s[i] != 0; break;
		case VC_NN: c = !is_nan(s[i]); break;
		case VC_NI: c = !is_inf(s[i]); break;
		default: c = !is_nan_or_inf(s[i]); break;
		}
		cc |= c << i;
		or_val |= c;
		and_val &= c;
		affected |= 1 << i;
	}
	CTRL[VFPU_CTRL_CC] = (CTRL[VFPU_CTRL_CC] & ~(u32)affected) | ((u32)(cc | (or_val << 4) | (and_val << 5)) & (u32)affected);
	eat_prefixes();
}

/* Sin prefijos y con una comparación normal (lo habitual) */
static void vcmp_fast(u32 op, int cond){
	const VectorSize sz = vec_size(op);
	const int n = (int)sz, all = (1 << n) - 1;
	const u16 *xs = vidx[sz][VS], *xt = vidx[sz][VT];
	float s[4], t[4];
	int i, cc = 0;
	const u32 affected = (u32)(all | 0x30);
	for(i = 0; i < n; i++){ s[i] = flush(VF(xs[i])); t[i] = flush(VF(xt[i])); }
	switch(cond){
	case VC_EQ: for(i = 0; i < n; i++) cc |= (s[i] == t[i]) << i; break;
	case VC_LT: for(i = 0; i < n; i++) cc |= (s[i] < t[i]) << i; break;
	case VC_LE: for(i = 0; i < n; i++) cc |= (s[i] <= t[i]) << i; break;
	case VC_NE: for(i = 0; i < n; i++) cc |= (s[i] != t[i]) << i; break;
	case VC_GE: for(i = 0; i < n; i++) cc |= (s[i] >= t[i]) << i; break;
	default: for(i = 0; i < n; i++) cc |= (s[i] > t[i]) << i; break;
	}
	cc |= ((cc != 0) << 4) | ((cc == all) << 5);
	CTRL[VFPU_CTRL_CC] = (CTRL[VFPU_CTRL_CC] & ~affected) | ((u32)cc & affected);
}

static NOINLINE void op_vcmp(u32 op){
	const int cond = op & 0xF;
	if((cond == VC_EQ || cond == VC_LT || cond == VC_LE || cond == VC_NE || cond == VC_GE || cond == VC_GT) && FAST_OK())
		vcmp_fast(op, cond);
	else vcmp_general(op);
}

static NOINLINE void vminmax_general(u32 op){
	FloatBits s, t, d;
	VectorSize sz = vec_size(op);
	int n = nelem(sz), i, is_max = ((op >> 23) & 3) == 3;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	read_vector(t.f, sz, VT);
	swizzle_t(t.f, sz, 0.0f);
	for(i = 0; i < n; i++){
		if(nan_or_inf_f(s.f[i]) || nan_or_inf_f(t.f[i])){
			/* -NaN < -inf < reales < inf < NaN (más mantisa, más lejos del 0) */
			int both_neg = s.i[i] < 0 && t.i[i] < 0;
			int take_max = is_max ? !both_neg : both_neg;
			if(take_max) d.i[i] = t.i[i] > s.i[i] ? t.i[i] : s.i[i];
			else d.i[i] = t.i[i] < s.i[i] ? t.i[i] : s.i[i];
		} else {
			/* Los denormales comparan como cero y en empate gana t */
			float fs = flush(s.f[i]), ft = flush(t.f[i]);
			if(is_max) d.f[i] = ft < fs ? s.f[i] : t.f[i];
			else d.f[i] = fs < ft ? s.f[i] : t.f[i];
		}
	}
	retain_invalid_swizzle_st(d.f, sz);
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static void vminmax_fast(u32 op){
	const VectorSize sz = vec_size(op);
	const int n = (int)sz, is_max = ((op >> 23) & 3) == 3;
	const u16 *xs = vidx[sz][VS], *xt = vidx[sz][VT];
	FloatBits d;
	int i;
	for(i = 0; i < n; i++){
		const float a = VF(xs[i]), b = VF(xt[i]);
		if(nan_or_inf_f(a) || nan_or_inf_f(b)){
			/* -NaN < -inf < reales < inf < NaN, con los bits */
			const s32 sa = (s32)VI(xs[i]), sb = (s32)VI(xt[i]);
			const int both_neg = sa < 0 && sb < 0, take_max = is_max ? !both_neg : both_neg;
			if(take_max) d.i[i] = sb > sa ? sb : sa;
			else d.i[i] = sb < sa ? sb : sa;
		} else {
			/* Los denormales comparan como cero y en empate gana t */
			const float fa = flush(a), fb = flush(b);
			if(is_max) d.f[i] = fb < fa ? a : b;
			else d.f[i] = fa < fb ? a : b;
		}
	}
	write_vector(d.f, sz, VD);
}

static NOINLINE void op_vminmax(u32 op){
	if(FAST_OK()) vminmax_fast(op);
	else vminmax_general(op);
}

static NOINLINE void op_vscmp(u32 op){
	FloatBits s, t, d;
	VectorSize sz = vec_size(op);
	int n = nelem(sz), i;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	read_vector(t.f, sz, VT);
	swizzle_t(t.f, sz, 0.0f);
	for(i = 0; i < n; i++){
		float a = s.f[i] - t.f[i];
		if(is_nan(a)){
			int sm = (int)(s.u[i] & 0x7FFFFFFFu), tm = (int)(t.u[i] & 0x7FFFFFFFu);
			int b = (s.i[i] < 0 ? -sm : sm) - (t.i[i] < 0 ? -tm : tm);
			d.f[i] = (float)((0 < b) - (b < 0));
		} else d.f[i] = (float)((0.0f < a) - (a < 0.0f));
	}
	retain_invalid_swizzle_st(d.f, sz);
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vsge_vslt(u32 op, int lt){
	float s[4], t[4], d[4];
	VectorSize sz = vec_size(op);
	int n = nelem(sz), i;
	read_vector(s, sz, VS);
	swizzle_s(s, sz, 0.0f);
	read_vector(t, sz, VT);
	swizzle_t(t, sz, 0.0f);
	for(i = 0; i < n; i++){
		if(is_nan(s[i]) || is_nan(t[i])) d[i] = 0.0f;
		else d[i] = (lt ? s[i] < t[i] : s[i] >= t[i]) ? 1.0f : 0.0f;
	}
	retain_invalid_swizzle_st(d, sz);
	apply_prefix_d(d, sz, 1);
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vcmov(u32 op){
	int tf = (op >> 19) & 1, imm3 = (op >> 16) & 7, i;
	VectorSize sz = vec_size(op);
	int n = nelem(sz);
	float s[4], d[4];
	u32 cc = CTRL[VFPU_CTRL_CC];
	read_vector(s, sz, VS);
	swizzle_s(s, sz, 0.0f);
	/* D se lee (como T) y el prefijo T se le aplica */
	read_vector(d, sz, VD);
	swizzle_t(d, sz, 0.0f);
	if(imm3 < 6){
		if((int)((cc >> imm3) & 1) == !tf) for(i = 0; i < n; i++) d[i] = s[i];
	} else if(imm3 == 6){
		for(i = 0; i < n; i++) if((int)((cc >> i) & 1) == !tf) d[i] = s[i];
	}
	apply_prefix_d(d, sz, 0);
	write_vector(d, sz, VD);
	eat_prefixes();
}

/* vadd, vsub, vdiv (VFPU0) y vmul (VFPU1) */
/* Sin prefijos: directo de los registros; las comprobaciones de denormales
   y NaN son comparaciones de la FPU que casi nunca saltan */
static void vecdo3_fast(u32 op, int optype){
	const VectorSize sz = vec_size(op);
	const int n = (int)sz;
	const u16 *xs = vidx[sz][VS], *xt = vidx[sz][VT];
	float d[4];
	int i;
	switch(optype){
	case 0: for(i = 0; i < n; i++) d[i] = canon(flush(VF(xs[i])) + flush(VF(xt[i]))); break;
	case 1: for(i = 0; i < n; i++) d[i] = canon(flush(VF(xs[i])) - flush(VF(xt[i]))); break;
	case 7: for(i = 0; i < n; i++) d[i] = canon(flush(VF(xs[i])) / flush(VF(xt[i]))); break;
	default: for(i = 0; i < n; i++) d[i] = canon(flush(VF(xs[i])) * flush(VF(xt[i]))); break;
	}
	write_vector(d, sz, VD);
}

/* Los caminos generales van aparte para que el rápido no pague su prólogo */
static NOINLINE void vecdo3_general(u32 op, int optype){
	float s[4], t[4];
	FloatBits d;
	VectorSize sz = vec_size(op);
	int n = nelem(sz), i;
	read_vector(s, sz, VS);
	read_vector(t, sz, VT);
	if(optype != 7){
		swizzle_s(s, sz, 0.0f);
		swizzle_t(t, sz, 0.0f);
	} else {
		/* vdiv: el prefijo x se aplica al último elemento */
		swizzle_s(&s[n - 1], V_Single, -INFINITY);
		swizzle_t(&t[n - 1], V_Single, -INFINITY);
	}
	/* Las entradas denormales cuentan como cero y el NaN sale canónico */
	for(i = 0; i < n; i++){
		float a = flush(s[i]), b = flush(t[i]);
		switch(optype){
		case 0: d.f[i] = canon(a + b); break;
		case 1: d.f[i] = canon(a - b); break;
		case 7: d.f[i] = canon(a / b); break;
		default: d.f[i] = canon(a * b); break;
		}
	}
	if(optype == 7){
		if(last_lane_swizzle_invalid(VFPU_CTRL_SPREFIX) || last_lane_swizzle_invalid(VFPU_CTRL_TPREFIX)) d.f[n - 1] = 0.0f;
		dprefix_last_only(n);
		apply_prefix_d(d.f, sz, 0);
	} else {
		retain_invalid_swizzle_st(d.f, sz);
		apply_prefix_d(d.f, sz, 0);
	}
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vecdo3(u32 op, int optype){
	if(FAST_OK()) vecdo3_fast(op, optype);
	else vecdo3_general(op, optype);
}

static NOINLINE void op_crossquat(u32 op){
	float s[4] = { 0 }, t[4] = { 0 }, d[4] = { 0 };
	VectorSize sz = vec_size(op);
	int n = nelem(sz);
	u32 remove = ANY_SWIZZLE | NEGATE4(1, 1, 1, 1);
	read_vector(s, sz, VS);
	read_vector(t, sz, VT);
	switch(sz){
	case V_Triple:   /* vcrsp.t */
		d[0] = s[1] * t[2] - s[2] * t[1];
		d[1] = s[2] * t[0] - s[0] * t[2];
		apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, remove, SWIZZLE(1, 0, 3, 2) | NEGATE4(0, 1, 0, 0)), V_Quad, 0.0f);
		swizzle_s(s, V_Quad, 0.0f);
		d[2] = s[0] * t[0] + s[1] * t[1] + s[2] * t[2] + s[3] * t[3];
		break;
	case V_Quad:     /* vqmul.q */
		d[0] = s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0];
		d[1] = -s[0] * t[2] + s[1] * t[3] + s[2] * t[0] + s[3] * t[1];
		d[2] = s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2];
		apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, remove, SWIZZLE(0, 1, 2, 3) | NEGATE4(1, 1, 1, 0)), V_Quad, 0.0f);
		swizzle_s(s, sz, 0.0f);
		d[3] = s[0] * t[0] + s[1] * t[1] + s[2] * t[2] + s[3] * t[3];
		break;
	case V_Pair:
		d[0] = 0;
		apply_prefix_st(t, rewrite_prefix(VFPU_CTRL_TPREFIX, remove, SWIZZLE(0, 0, 0, 0)), V_Quad, 0.0f);
		swizzle_s(s, V_Quad, 0.0f);
		d[1] = s[2] * t[2];
		break;
	default:
		d[0] = 0;
		break;
	}
	if(sz != V_Single){
		dprefix_last_only(n);
		apply_prefix_d(d, sz, 0);
	} else CTRL[VFPU_CTRL_DPREFIX] = 0;
	write_vector(d, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vlgb(u32 op){
	FloatBits d, s;
	VectorSize sz = vec_size(op);
	int exp, i;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	exp = (int)((s.u[0] & 0x7F800000u) >> 23);
	if(exp == 0xFF) d.f[0] = s.f[0];
	else if(exp == 0) d.f[0] = -INFINITY;
	else d.f[0] = (float)(exp - 127);
	for(i = 1; i < nelem(sz); ++i) d.u[i] = s.u[i];
	retain_invalid_swizzle_st(d.f, sz);
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vwbn(u32 op){
	FloatBits d, s;
	VectorSize sz = vec_size(op);
	u8 exp = (u8)((op >> 16) & 0xFF);
	u32 sigbit, prev_exp, mantissa;
	int i;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	sigbit = s.u[0] & 0x80000000u;
	prev_exp = (s.u[0] & 0x7F800000u) >> 23;
	mantissa = (s.u[0] & 0x007FFFFFu) | 0x00800000u;
	if(prev_exp != 0xFF && prev_exp != 0){
		if(exp > prev_exp) mantissa >>= (exp - prev_exp) & 0xF;
		else mantissa <<= (prev_exp - exp) & 0xF;
		d.u[0] = sigbit | (mantissa & 0x007FFFFFu) | ((u32)exp << 23);
	} else d.u[0] = s.u[0] | ((u32)exp << 23);
	for(i = 1; i < nelem(sz); ++i) d.u[i] = s.u[i];
	retain_invalid_swizzle_st(d.f, sz);
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vsbn(u32 op){
	FloatBits d, s, t;
	VectorSize sz = vec_size(op);
	u8 exp;
	u32 prev;
	int i;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	read_vector(t.f, sz, VT);
	swizzle_t(t.f, sz, 0.0f);
	exp = (u8)(127 + t.i[0]);
	prev = s.u[0] & 0x7F800000u;
	if(prev != 0 && prev != 0x7F800000u) d.u[0] = (s.u[0] & ~0x7F800000u) | ((u32)exp << 23);
	else d.u[0] = s.u[0];
	for(i = 1; i < nelem(sz); ++i) d.u[i] = s.u[i];
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

static NOINLINE void op_vsbz(u32 op){
	FloatBits d, s;
	VectorSize sz = vec_size(op);
	int i;
	read_vector(s.f, sz, VS);
	swizzle_s(s.f, sz, 0.0f);
	if(is_nan(s.f[0]) || (s.u[0] & 0x7F800000u) == 0) d.u[0] = s.u[0];
	else d.u[0] = (127u << 23) | (s.u[0] & 0x007FFFFFu);
	for(i = 1; i < nelem(sz); ++i) d.u[i] = s.u[i];
	apply_prefix_d(d.f, sz, 0);
	write_vector(d.f, sz, VD);
	eat_prefixes();
}

/* --- Decodificación ------------------------------------------------------------------------ */

static void invalid(u32 op, u32 pc){ cpu_fault("instruccion VFPU desconocida", pc, op); }

static NOINLINE void vfpu4(u32 op, u32 pc){
	switch((op >> 16) & 0x1F){
	case 0: VSTAT(VS_VMOV); op_vv2op(op); break;
	case 1: case 2: VSTAT(VS_VABS_NEG); op_vv2op(op); break;
	case 4: case 5: VSTAT(VS_VSAT); op_vv2op(op); break;
	case 16: case 24: VSTAT(VS_VRCP); op_vv2op(op); break;
	case 17: VSTAT(VS_VRSQ); op_vv2op(op); break;
	case 18: case 19: case 26: VSTAT(VS_VSIN_COS); op_vv2op(op); break;
	case 20: case 21: case 28: VSTAT(VS_VEXP_LOG); op_vv2op(op); break;
	case 22: VSTAT(VS_VSQRT); op_vv2op(op); break;
	case 23: VSTAT(VS_VASIN); op_vv2op(op); break;
	case 3: VSTAT(VS_VIDT_ZERO_ONE); op_vidt(op); break;
	case 6: case 7: VSTAT(VS_VIDT_ZERO_ONE); op_vvector_init(op); break;
	default: invalid(op, pc); break;
	}
}

static NOINLINE void vfpu7(u32 op, u32 pc){
	switch((op >> 16) & 0x1F){
	case 0: VSTAT(VS_VRND); op_vrnds(op); break;
	case 1: case 2: case 3: VSTAT(VS_VRND); op_vrndx(op); break;
	case 18: VSTAT(VS_CONV); op_vf2h(op); break;
	case 19: VSTAT(VS_CONV); op_vh2f(op); break;
	case 22: VSTAT(VS_SBN_SBZ_LGB); op_vsbz(op); break;
	case 23: VSTAT(VS_SBN_SBZ_LGB); op_vlgb(op); break;
	case 24: case 25: case 26: case 27: VSTAT(VS_CONV); op_vx2i(op); break;
	case 28: case 29: case 30: case 31: VSTAT(VS_CONV); op_vi2x(op); break;
	default: invalid(op, pc); break;
	}
}

static NOINLINE void vfpu9(u32 op, u32 pc){
	switch((op >> 16) & 0x1F){
	case 0: VSTAT(VS_VFPU9); op_vsrt(op, 1); break;
	case 1: VSTAT(VS_VFPU9); op_vsrt(op, 2); break;
	case 2: case 3: VSTAT(VS_VFPU9); op_vbfy(op); break;
	case 4: VSTAT(VS_VFPU9); op_vocp(op); break;
	case 5: VSTAT(VS_VFPU9); op_vsocp(op); break;
	case 6: VSTAT(VS_VFPU9); op_vfad_vavg(op, 0); break;
	case 7: VSTAT(VS_VFPU9); op_vfad_vavg(op, 1); break;
	case 8: VSTAT(VS_VFPU9); op_vsrt(op, 3); break;
	case 9: VSTAT(VS_VFPU9); op_vsrt(op, 4); break;
	case 10: VSTAT(VS_VFPU9); op_vsgn(op); break;
	case 16: VSTAT(VS_MFVC_MTVC); op_vmfvc(op); break;
	case 17: VSTAT(VS_MFVC_MTVC); op_vmtvc(op); break;
	case 25: case 26: case 27: VSTAT(VS_CONV); op_color_conv(op); break;
	default: invalid(op, pc); break;
	}
}

void vfpu_exec(u32 op, u32 pc){
	vfpu_ensure();
	switch(op >> 26){
	case 0x36: case 0x3E: VSTAT((op >> 26) == 0x36 ? VS_LVQ : VS_SVQ); op_svq(op, pc); break;
	case 0x32: case 0x3A: VSTAT((op >> 26) == 0x32 ? VS_LV : VS_SV); op_sv(op, pc); break;
	case 0x35: case 0x3D: VSTAT(VS_LVSVLR); op_svq(op, pc); break;
	case 0x12:   /* COP2 (las ramas bvf/bvt las lleva el intérprete) */
		switch((op >> 21) & 0x1F){
		case 3: VSTAT((op & 0x80) ? VS_MFVC_MTVC : VS_MFV); op_mftv(op); break;
		case 7: VSTAT((op & 0x80) ? VS_MFVC_MTVC : VS_MTV); op_mftv(op); break;
		default: invalid(op, pc); break;
		}
		break;
	case 0x18:   /* VFPU0 */
		switch((op >> 23) & 7){
		case 0: VSTAT(VS_VADD); op_vecdo3(op, 0); break;
		case 1: VSTAT(VS_VSUB); op_vecdo3(op, 1); break;
		case 2: VSTAT(VS_SBN_SBZ_LGB); op_vsbn(op); break;
		case 7: VSTAT(VS_VDIV); op_vecdo3(op, 7); break;
		default: invalid(op, pc); break;
		}
		break;
	case 0x19:   /* VFPU1 */
		switch((op >> 23) & 7){
		case 0: VSTAT(VS_VMUL); op_vecdo3(op, 8); break;
		case 1: VSTAT(VS_VDOT); op_vdot(op); break;
		case 2: VSTAT(VS_VSCL); op_vscl(op); break;
		case 4: VSTAT(VS_VHDP); op_vhdp(op); break;
		case 5: VSTAT(VS_VCRS); op_vcrs(op); break;
		case 6: VSTAT(VS_VDET); op_vdet(op); break;
		default: invalid(op, pc); break;
		}
		break;
	case 0x1B:   /* VFPU3 */
		switch((op >> 23) & 7){
		case 0: VSTAT(VS_VCMP); op_vcmp(op); break;
		case 2: case 3: VSTAT(VS_VMIN_MAX); op_vminmax(op); break;
		case 5: VSTAT(VS_VSCMP_SGE_SLT); op_vscmp(op); break;
		case 6: VSTAT(VS_VSCMP_SGE_SLT); op_vsge_vslt(op, 0); break;
		case 7: VSTAT(VS_VSCMP_SGE_SLT); op_vsge_vslt(op, 1); break;
		default: invalid(op, pc); break;
		}
		break;
	case 0x34: {   /* VFPU4Jump */
		u32 sub = (op >> 21) & 0x1F;
		if(sub == 0) vfpu4(op, pc);
		else if(sub == 1) vfpu7(op, pc);
		else if(sub == 2) vfpu9(op, pc);
		else if(sub == 3){ VSTAT(VS_VCST); op_vcst(op); }
		else if(sub >= 16 && sub <= 19){ VSTAT(VS_VF2I); op_vf2i(op); }
		else if(sub == 20){ VSTAT(VS_VI2F); op_vi2f(op); }
		else if(sub == 21){ VSTAT(VS_VCMOV); op_vcmov(op); }
		else if(sub >= 24){ VSTAT(VS_VWBN); op_vwbn(op); }
		else invalid(op, pc);
		break;
	}
	case 0x37:   /* VFPU5 */
		if(((op >> 23) & 7) < 6){ VSTAT(VS_VPFX); op_vpfx(op); }
		else { VSTAT(VS_VIIM); op_viim(op); }
		break;
	case 0x3C: {   /* VFPU6 */
		u32 sub = (op >> 21) & 0x1F;
		if(sub <= 3){ VSTAT(VS_VMMUL); op_vmmul(op); }
		else if(sub <= 15){ VSTAT(VS_VTFM); op_vtfm(op); }
		else if(sub <= 19){ VSTAT(VS_VMSCL); op_vmscl(op); }
		else if(sub <= 23){ VSTAT(VS_VCRSP_QMUL); op_crossquat(op); }
		else if(sub == 28){
			switch((op >> 16) & 0xF){
			case 0: VSTAT(VS_VMMOV); op_vmmov(op); break;
			case 3: case 6: case 7: VSTAT(VS_VMINIT); op_vmatrix_init(op); break;
			default: invalid(op, pc); break;
			}
		} else if(sub == 29){ VSTAT(VS_VROT); op_vrot(op); }
		else invalid(op, pc);
		break;
	}
	case 0x3F: VSTAT(VS_VFLUSH); op_vflush(op); break;
	default: invalid(op, pc); break;
	}
}
