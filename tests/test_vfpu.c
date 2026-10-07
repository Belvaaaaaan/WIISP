/**
 * WIISP - tests/test_vfpu.c
 * Pruebas de la VFPU: los caminos rápidos (sin prefijos) deben dar
 * exactamente los mismos bits que el camino general, que es el que validan
 * los pspautotests de cpu/vfpu. Se prueban miles de instrucciones al azar
 * con NaN, infinitos, denormales y ceros con signo.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include <string.h>
#include "cpu/vfpu.h"
#include "test.h"

static u32 rng = 0x12345678u;
static u32 rnd(void){
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static float u2f(u32 u){ float f; memcpy(&f, &u, 4); return f; }
static u32 f2u(float f){ u32 u; memcpy(&u, &f, 4); return u; }

/* Valores de prueba: especiales, comunes y al azar */
static u32 rand_value(void){
	static const u32 special[] = {
		0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x7F800000, 0xFF800000,
		0x7FC00000, 0xFFC00000, 0x7F800001, 0xFFBFFFFF, 0x000116C2, 0x800116C2, 0x00800000,
		0x80800000, 0x007FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x4F000000, 0xCF000000, 0x4EFFFFFF,
		0x3EFFFFFF, 0x3FC00000, 0xC0200000, 0x40100000,
	};
	switch(rnd() % 4){
	case 0: return special[rnd() % (sizeof(special) / sizeof(special[0]))];
	case 1: return rnd();                                         /* cualquier patrón */
	case 2: return f2u(((float)(rnd() % 20001) - 10000.0f) / 16.0f);
	default: return (rnd() & 0x807FFFFFu) | ((100 + rnd() % 60) << 23);   /* magnitudes medias */
	}
}

static void rand_state(VfpuState *s){
	int i;
	vfpu_reset(s);
	for(i = 0; i < 128; i++) s->vi[i] = rand_value();
	s->ctrl[VFPU_CTRL_CC] = rnd() & 0x3F;
}

static const u32 SIZES[4] = { 0, 0x80, 0x8000, 0x8080 };

static u32 rand_op(void){
	u32 regs = (rnd() & 0x7F) | ((rnd() & 0x7F) << 8) | ((rnd() & 0x7F) << 16);
	u32 size = SIZES[rnd() & 3];
	static const u32 v2[] = { 0, 1, 2, 4, 5, 16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 28 };
	switch(rnd() % 12){
	case 0: return 0x60000000u | size | regs;                         /* vadd */
	case 1: return 0x60800000u | size | regs;                         /* vsub */
	case 2: return 0x63800000u | size | regs;                         /* vdiv */
	case 3: return 0x64000000u | size | regs;                         /* vmul */
	case 4: return 0x64800000u | size | regs;                         /* vdot */
	case 5: return 0x65000000u | size | regs;                         /* vscl */
	case 6:                                                           /* vmmul */
		if(rnd() & 1) regs &= 0x1C1C1C;   /* M000..M700: matrices seguidas */
		return 0xF0000000u | size | regs;
	case 7:                                                           /* v(h)tfm2/3/4 */
		if(rnd() & 1) regs &= 0x1C1C7F;   /* matriz M y vector columna */
		if(rnd() & 1) regs &= 0x1F1C7F;
		return 0xF0000000u | ((1 + rnd() % 3) << 23) | size | regs;
	case 8: return 0xD0000000u | (v2[rnd() % 16] << 16) | size | (regs & 0x7F7F);
	case 9: return 0xD0000000u | ((16 + (rnd() & 3)) << 21) | ((rnd() & 31) << 16) | size | (regs & 0x7F7F);  /* vf2i */
	case 10: return 0x6D000000u | ((rnd() & 1) << 23) | size | regs; /* vmin/vmax */
	default: return 0x6C000000u | size | (regs & 0x7F7F00) | (rnd() & 15);  /* vcmp */
	}
}

/* Las definiciones al bit de la PSP, para validar las de la FPU */
static float flush_bits(float f){
	u32 u = f2u(f);
	return (u & 0x7F800000u) == 0 ? u2f(u & 0x80000000u) : f;
}
static float canon_bits(float f){
	u32 u = f2u(f);
	if((u & 0x7FFFFFFFu) > 0x7F800000u) return u2f(0x7F800001u);
	return (u & 0x7F800000u) == 0 ? u2f(u & 0x80000000u) : f;
}

/* vf2i como lo hacía PPSSPP (con floor/ceil y el redondeo IEEE a mano) */
static s32 f2i_ref(float f, int imm, int mode){
	double sv, i, fr;
	if(f != f) return 0x7FFFFFFF;
	sv = (double)(f * (float)(1u << imm));
	if(sv > (double)0x7FFFFFFF) return 0x7FFFFFFF;
	if(sv <= -2147483648.0) return (s32)0x80000000u;
	switch(mode){
	case 17: return f >= 0 ? (s32)floor(sv) : (s32)ceil(sv);
	case 18: return (s32)ceil(sv);
	case 19: return (s32)floor(sv);
	default:
		i = (float)floor(sv);
		fr = sv - i;
		if(fr < 0.5f) return (s32)i;
		if(fr > 0.5f) return (s32)(i + 1.0f);
		return fmod(i, 2.0) == 0.0 ? (s32)i : (s32)(i + 1.0f);
	}
}

/* Igual al bit, salvo qué NaN sale cuando hay dos NaN de entrada: eso lo
   decide el orden en que el compilador pone los operandos (en x86 gana el
   primero) y no lo fija ni la PSP ni PPSSPP en estas operaciones */
static int same_state(const VfpuState *a, const VfpuState *b){
	int i;
	for(i = 0; i < 128; i++){
		u32 x = a->vi[i], y = b->vi[i];
		if(x == y) continue;
		if((x & 0x7FFFFFFFu) > 0x7F800000u && (y & 0x7FFFFFFFu) > 0x7F800000u) continue;
		return 0;
	}
	return !memcmp(a->ctrl, b->ctrl, sizeof(a->ctrl));
}

void test_vfpu(void){
	static VfpuState start, fast;
	int k, bad = 0, i;
	printf("VFPU: caminos rápidos contra el general\n");
	vfpu_live = 1;
	for(k = 0; k < 200000; k++){
		u32 op = rand_op();
		rand_state(&start);
		vfpu = start;
		vfpu_fast_paths = 1;
		vfpu_exec(op, 0);
		fast = vfpu;
		vfpu = start;
		vfpu_fast_paths = 0;
		vfpu_exec(op, 0);
		if(!same_state(&fast, &vfpu)){
			if(bad++ < 5){
				int r;
				printf("  distinto: op %08X\n", op);
				for(r = 0; r < 128; r++)
					if(fast.vi[r] != vfpu.vi[r])
						printf("    v[%d]: rapido %08X general %08X (antes %08X)\n", r, fast.vi[r], vfpu.vi[r], start.vi[r]);
				for(r = 0; r < 16; r++)
					if(fast.ctrl[r] != vfpu.ctrl[r]) printf("    ctrl[%d]: %08X %08X\n", r, fast.ctrl[r], vfpu.ctrl[r]);
			}
		}
	}
	vfpu_fast_paths = 1;
	CHECK_EQ(bad, 0);

	/* vadd/vsub/vmul/vdiv.s contra la definición al bit */
	bad = 0;
	for(k = 0; k < 100000; k++){
		static const u32 base[4] = { 0x60000000u, 0x60800000u, 0x64000000u, 0x63800000u };
		int kind = k & 3;
		float a = u2f(rand_value()), b = u2f(rand_value()), r, e;
		vfpu_reset(&vfpu);
		vfpu.v[0] = a;
		vfpu.v[1] = b;
		/* S000 = S000 op S010 (v[0] y v[1]) */
		vfpu_exec(base[kind] | (0x20u << 16), 0);
		a = flush_bits(a);
		b = flush_bits(b);
		e = canon_bits(kind == 0 ? a + b : kind == 1 ? a - b : kind == 2 ? a * b : a / b);
		r = vfpu.v[0];
		if(f2u(r) != f2u(e) && bad++ < 5) printf("  op %d: %08X, %08X -> %08X (esperado %08X)\n",
		                                         kind, f2u(a), f2u(b), f2u(r), f2u(e));
	}
	CHECK_EQ(bad, 0);

	/* vf2i contra la versión con floor/ceil */
	bad = 0;
	for(k = 0; k < 100000; k++){
		int mode = 16 + (k & 3), imm = (int)(rnd() & 31);
		u32 bits = (k & 4) ? rand_value() : f2u(((float)(rnd() % 4001) - 2000.0f) / 4.0f);
		vfpu_reset(&vfpu);
		vfpu.vi[0] = bits;
		vfpu_exec(0xD0000000u | ((u32)mode << 21) | ((u32)imm << 16), 0);   /* vf2iX.s S000, S000 */
		if((s32)vfpu.vi[0] != f2i_ref(u2f(bits), imm, mode) && bad++ < 5)
			printf("  vf2i modo %d imm %d: %08X -> %08X (esperado %08X)\n", mode, imm, bits,
			       vfpu.vi[0], (u32)f2i_ref(u2f(bits), imm, mode));
	}
	CHECK_EQ(bad, 0);

	/* Registros: la tabla de índices contra la fórmula de PPSSPP */
	bad = 0;
	for(i = 0; i < 128; i++){
		float q[4];
		int lane;
		vfpu_reset(&vfpu);
		for(k = 0; k < 128; k++) vfpu.v[k] = (float)k;
		vfpu_exec(0xD0000000u | 0x8080 | ((u32)i << 8), 0);   /* vmov.q S000.., reg i */
		for(lane = 0; lane < 4; lane++){
			int row = (i >> 5) & 2, mtx = (i << 2) & 0x70, col = i & 3;
			int idx = (i >> 5) & 1 ? mtx + col + ((row + lane) & 3) * 4 : mtx + col * 4 + ((row + lane) & 3);
			q[lane] = vfpu.v[VFPU_INDEX(lane << 5)];   /* C000 */
			if(q[lane] != (float)idx) bad++;
		}
	}
	CHECK_EQ(bad, 0);
}
