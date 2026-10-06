/**
 * WIISP - interp.c
 * Intérprete del Allegrex (ver cpu.h).
 *
 * Modelo de delay slots: cada paso ejecuta la instrucción en cpu.pc y avanza
 * (pc, npc) = (npc, npc + 4). Un salto solo cambia npc, así que la
 * instrucción siguiente (el delay slot) se ejecuta antes de llegar al
 * destino, igual que en el hardware. Un "branch likely" no tomado se salta
 * el delay slot avanzando una vez más.
 *
 * Códigos de operación según la documentación del Allegrex y PPSSPP
 * (Core/MIPS/MIPSTables.cpp), comprobados con pspautotests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include <stdio.h>
#include "cpu/cpu.h"
#include "core/memory.h"

CpuState cpu;
volatile int cpu_stop_requested;
u64 cpu_cycles;
u64 cpu_executed;

#define RS   ((instr >> 21) & 31)
#define RT   ((instr >> 16) & 31)
#define RD   ((instr >> 11) & 31)
#define SA   ((instr >> 6) & 31)
#define FUNC (instr & 63)
#define IMM  ((s32)(s16)(instr & 0xFFFF))
#define UIMM (instr & 0xFFFF)

#define R(n)  cpu.r[n]
#define FS   ((instr >> 11) & 31)
#define FT   ((instr >> 16) & 31)
#define FD   ((instr >> 6) & 31)
#define F(n)  cpu.fpr[n].f
#define FI(n) cpu.fpr[n].u

/* ------------------------------------------------------------------ */
/* Memoria                                                            */
/* ------------------------------------------------------------------ */

static inline u8 *ptr(u32 addr, u32 size, u32 instr){
	u8 *p = mem_ptr(addr, size);
	if(!p) cpu_fault("acceso a memoria invalido", addr, instr);
	return p;
}

static inline const u8 *ptr_r(u32 addr, u32 size, u32 instr){
	const u8 *p = mem_ptr_r(addr, size);
	if(!p) cpu_fault("acceso a memoria invalido", addr, instr);
	return p;
}

static inline u32 ld32(u32 a, u32 i){ const u8 *p = ptr_r(a, 4, i); return p ? rd_le32(p) : 0; }
static inline u32 ld16(u32 a, u32 i){ const u8 *p = ptr_r(a, 2, i); return p ? rd_le16(p) : 0; }
static inline u32 ld8 (u32 a, u32 i){ const u8 *p = ptr_r(a, 1, i); return p ? *p : 0; }
static inline void st32(u32 a, u32 v, u32 i){ u8 *p = ptr(a, 4, i); if(p) wr_le32(p, v); }
static inline void st16(u32 a, u32 v, u32 i){ u8 *p = ptr(a, 2, i); if(p) wr_le16(p, (u16)v); }
static inline void st8 (u32 a, u32 v, u32 i){ u8 *p = ptr(a, 1, i); if(p) *p = (u8)v; }

/* ------------------------------------------------------------------ */
/* Saltos                                                             */
/* ------------------------------------------------------------------ */

/* pc es la dirección de la instrucción de salto */
static inline void branch(int taken, u32 pc, u32 instr){
	if(taken) cpu.npc = pc + 4 + ((u32)IMM << 2);
}

static inline void branch_likely(int taken, u32 pc, u32 instr){
	if(taken) cpu.npc = pc + 4 + ((u32)IMM << 2);
	else { cpu.pc = cpu.npc; cpu.npc += 4; } /* anula el delay slot */
}

/* ------------------------------------------------------------------ */
/* Grupos de instrucciones                                            */
/* ------------------------------------------------------------------ */

static void op_special(u32 instr, u32 pc){
	u32 rs = R(RS), rt = R(RT);
	switch(FUNC){
	case 0x00: R(RD) = rt << SA; break;                       /* sll */
	case 0x02:                                                 /* srl / rotr */
		if(RS & 1) R(RD) = SA ? (rt >> SA) | (rt << (32 - SA)) : rt;
		else R(RD) = rt >> SA;
		break;
	case 0x03: R(RD) = (u32)((s32)rt >> SA); break;           /* sra */
	case 0x04: R(RD) = rt << (rs & 31); break;                 /* sllv */
	case 0x06: {                                               /* srlv / rotrv */
		u32 s = rs & 31;
		if(SA & 1) R(RD) = s ? (rt >> s) | (rt << (32 - s)) : rt;
		else R(RD) = rt >> s;
		break;
	}
	case 0x07: R(RD) = (u32)((s32)rt >> (rs & 31)); break;    /* srav */
	case 0x08: cpu.npc = rs; break;                            /* jr */
	case 0x09: cpu.npc = rs; R(RD) = pc + 8; break;            /* jalr */
	case 0x0A: if(rt == 0) R(RD) = rs; break;                  /* movz */
	case 0x0B: if(rt != 0) R(RD) = rs; break;                  /* movn */
	case 0x0C: cpu.llbit = 0; hle_syscall((instr >> 6) & 0xFFFFF); break; /* syscall */
	case 0x0D: cpu_fault("break", pc, instr); break;           /* break */
	case 0x0F: break;                                          /* sync */
	case 0x10: R(RD) = cpu.hi; break;                          /* mfhi */
	case 0x11: cpu.hi = rs; break;                             /* mthi */
	case 0x12: R(RD) = cpu.lo; break;                          /* mflo */
	case 0x13: cpu.lo = rs; break;                             /* mtlo */
	case 0x16: R(RD) = rs ? (u32)__builtin_clz(rs) : 32; break;   /* clz */
	case 0x17: R(RD) = ~rs ? (u32)__builtin_clz(~rs) : 32; break; /* clo */
	case 0x18: {                                               /* mult */
		s64 r = (s64)(s32)rs * (s32)rt;
		cpu.lo = (u32)r; cpu.hi = (u32)((u64)r >> 32);
		break;
	}
	case 0x19: {                                               /* multu */
		u64 r = (u64)rs * rt;
		cpu.lo = (u32)r; cpu.hi = (u32)(r >> 32);
		break;
	}
	case 0x1A: {                                               /* div */
		s32 a = (s32)rs, b = (s32)rt;
		if(a == (s32)0x80000000 && b == -1){ cpu.lo = 0x80000000u; cpu.hi = 0; }
		else if(b != 0){ cpu.lo = (u32)(a / b); cpu.hi = (u32)(a % b); }
		else { cpu.lo = a < 0 ? 1 : 0xFFFFFFFFu; cpu.hi = (u32)a; }
		break;
	}
	case 0x1B:                                                 /* divu */
		if(rt != 0){ cpu.lo = rs / rt; cpu.hi = rs % rt; }
		else { cpu.lo = rs <= 0xFFFF ? 0xFFFF : 0xFFFFFFFFu; cpu.hi = rs; }
		break;
	case 0x1C: case 0x1D: case 0x2E: case 0x2F: {              /* madd(u) / msub(u) */
		u64 acc = ((u64)cpu.hi << 32) | cpu.lo;
		u64 prod = (FUNC & 1) ? (u64)rs * rt : (u64)((s64)(s32)rs * (s32)rt);
		acc = (FUNC & 2) ? acc - prod : acc + prod;
		cpu.lo = (u32)acc; cpu.hi = (u32)(acc >> 32);
		break;
	}
	case 0x20: case 0x21: R(RD) = rs + rt; break;              /* add(u) */
	case 0x22: case 0x23: R(RD) = rs - rt; break;              /* sub(u) */
	case 0x24: R(RD) = rs & rt; break;
	case 0x25: R(RD) = rs | rt; break;
	case 0x26: R(RD) = rs ^ rt; break;
	case 0x27: R(RD) = ~(rs | rt); break;
	case 0x2A: R(RD) = (s32)rs < (s32)rt; break;               /* slt */
	case 0x2B: R(RD) = rs < rt; break;                         /* sltu */
	case 0x2C: R(RD) = (s32)rs > (s32)rt ? rs : rt; break;     /* max */
	case 0x2D: R(RD) = (s32)rs < (s32)rt ? rs : rt; break;     /* min */
	default: cpu_fault("instruccion SPECIAL desconocida", pc, instr);
	}
}

static void op_regimm(u32 instr, u32 pc){
	s32 rs = (s32)R(RS);
	switch(RT){
	case 0x00: branch(rs < 0, pc, instr); break;               /* bltz */
	case 0x01: branch(rs >= 0, pc, instr); break;              /* bgez */
	case 0x02: branch_likely(rs < 0, pc, instr); break;        /* bltzl */
	case 0x03: branch_likely(rs >= 0, pc, instr); break;       /* bgezl */
	case 0x10: R(R_RA) = pc + 8; branch(rs < 0, pc, instr); break;          /* bltzal */
	case 0x11: R(R_RA) = pc + 8; branch(rs >= 0, pc, instr); break;         /* bgezal */
	case 0x12: R(R_RA) = pc + 8; branch_likely(rs < 0, pc, instr); break;   /* bltzall */
	case 0x13: R(R_RA) = pc + 8; branch_likely(rs >= 0, pc, instr); break;  /* bgezall */
	default: cpu_fault("instruccion REGIMM desconocida", pc, instr);
	}
}

static void op_special3(u32 instr, u32 pc){
	u32 rs = R(RS), rt = R(RT);
	switch(FUNC){
	case 0x00: {                                               /* ext */
		u32 pos = SA, size = RD + 1;
		u32 mask = size >= 32 ? 0xFFFFFFFFu : (1u << size) - 1;
		R(RT) = (rs >> pos) & mask;
		break;
	}
	case 0x04: {                                               /* ins */
		u32 pos = SA, size = RD + 1 - pos;
		u32 mask = (size >= 32 ? 0xFFFFFFFFu : (1u << size) - 1) << pos;
		R(RT) = (rt & ~mask) | ((rs << pos) & mask);
		break;
	}
	case 0x20:                                                 /* bshfl */
		switch(SA){
		case 0x02: R(RD) = ((rt & 0x00FF00FFu) << 8) | ((rt >> 8) & 0x00FF00FFu); break; /* wsbh */
		case 0x03: R(RD) = __builtin_bswap32(rt); break;                                 /* wsbw */
		case 0x10: R(RD) = (u32)(s32)(s8)rt; break;                                      /* seb */
		case 0x14: {                                                                     /* bitrev */
			u32 v = rt, r = 0, i;
			for(i = 0; i < 32; i++, v >>= 1) r = (r << 1) | (v & 1);
			R(RD) = r;
			break;
		}
		case 0x18: R(RD) = (u32)(s32)(s16)rt; break;                                     /* seh */
		default: cpu_fault("instruccion BSHFL desconocida", pc, instr);
		}
		break;
	default: cpu_fault("instruccion SPECIAL3 desconocida", pc, instr);
	}
}

/* ------------------------------------------------------------------ */
/* FPU (COP1)                                                         */
/* ------------------------------------------------------------------ */

/* Conversión a entero con saturación (como el hardware) */
static u32 float_to_int(float v, int mode){
	double d;
	if(isnan(v)) return 0x7FFFFFFFu;
	switch(mode){
	case 0:  d = nearbyint(v); break; /* al más cercano (par) */
	case 1:  d = trunc(v); break;
	case 2:  d = ceil(v); break;
	default: d = floor(v); break;
	}
	if(d >= 2147483647.0) return 0x7FFFFFFFu;
	if(d <= -2147483648.0) return 0x80000000u;
	return (u32)(s32)d;
}

/* Un NaN generado por la FPU de la PSP es siempre 0x7FC00000 (x86 da
   0xFFC00000; PowerPC ya coincide). Si una entrada era NaN, se propaga. */
static inline void set_arith(u32 fd, float r, float a, float b){
	F(fd) = r;
	if(isnan(r) && !isnan(a) && !isnan(b)) FI(fd) = 0x7FC00000u;
}

static void op_cop1_s(u32 instr, u32 pc){
	float fs = F(FS), ft = F(FT);
	switch(FUNC){
	case 0x00: set_arith(FD, fs + ft, fs, ft); break;
	case 0x01: set_arith(FD, fs - ft, fs, ft); break;
	case 0x02: set_arith(FD, fs * ft, fs, ft); break;
	case 0x03: set_arith(FD, fs / ft, fs, ft); break;
	case 0x04: set_arith(FD, sqrtf(fs), fs, 0); break;
	case 0x05: FI(FD) = FI(FS) & 0x7FFFFFFFu; break;          /* abs */
	case 0x06: FI(FD) = FI(FS); break;                         /* mov */
	case 0x07: FI(FD) = FI(FS) ^ 0x80000000u; break;          /* neg */
	case 0x0C: FI(FD) = float_to_int(fs, 0); break;            /* round.w */
	case 0x0D: FI(FD) = float_to_int(fs, 1); break;            /* trunc.w */
	case 0x0E: FI(FD) = float_to_int(fs, 2); break;            /* ceil.w */
	case 0x0F: FI(FD) = float_to_int(fs, 3); break;            /* floor.w */
	case 0x24: FI(FD) = float_to_int(fs, cpu.fcr31 & 3); break; /* cvt.w.s */
	default:
		if(FUNC >= 0x30){                                      /* c.cond.s */
			u32 c = FUNC & 7;
			int unordered = isnan(fs) || isnan(ft);
			int res = ((c & 1) && unordered) ||
			          ((c & 2) && !unordered && fs == ft) ||
			          ((c & 4) && !unordered && fs < ft);
			/* Riesgo de pipeline: un bc1x justo después todavía ve la
			   condición anterior (pspautotests cpu/fpu/fpu_branch_hazard) */
			cpu.fcc_old = (cpu.fcr31 & FCR31_COND) != 0;
			cpu.fcc_hazard_pc = pc + 4;
			if(res) cpu.fcr31 |= FCR31_COND;
			else cpu.fcr31 &= ~FCR31_COND;
		} else cpu_fault("instruccion FPU desconocida", pc, instr);
	}
}

static void op_cop1(u32 instr, u32 pc){
	switch(RS){
	case 0x00: R(RT) = FI(FS); break;                          /* mfc1 */
	case 0x02:                                                 /* cfc1 */
		R(RT) = FS == 31 ? cpu.fcr31 : FS == 0 ? FCR0_VALUE : 0;
		break;
	case 0x04: FI(FS) = R(RT); break;                          /* mtc1 */
	case 0x06:                                                 /* ctc1 */
		if(FS == 31){
			cpu.fcr31 = R(RT) & FCR31_WRITABLE;
			cpu.fcc_hazard_pc = 0;
		}
		break;
	case 0x08: {                                               /* bc1f/t/fl/tl */
		int cond = (cpu.fcr31 & FCR31_COND) != 0;
		if(pc == cpu.fcc_hazard_pc) cond = cpu.fcc_old;
		cpu.fcc_hazard_pc = 0;
		int taken = (RT & 1) ? cond : !cond;
		if(RT & 2) branch_likely(taken, pc, instr);
		else branch(taken, pc, instr);
		break;
	}
	case 0x10: op_cop1_s(instr, pc); break;                    /* formato S */
	case 0x14:                                                 /* formato W */
		if(FUNC == 0x20) F(FD) = (float)(s32)FI(FS);          /* cvt.s.w */
		else cpu_fault("instruccion FPU (W) desconocida", pc, instr);
		break;
	default: cpu_fault("instruccion COP1 desconocida", pc, instr);
	}
}

/* ------------------------------------------------------------------ */
/* Bucle principal                                                    */
/* ------------------------------------------------------------------ */

static inline void step(void){
	u32 pc = cpu.pc;
	u32 instr = ld32(pc, 0);
	u32 rs, addr;

	cpu.pc = cpu.npc;
	cpu.npc += 4;

	switch(instr >> 26){
	case 0x00: op_special(instr, pc); break;
	case 0x01: op_regimm(instr, pc); break;
	case 0x02: cpu.npc = (pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2); break; /* j */
	case 0x03:                                                                     /* jal */
		R(R_RA) = pc + 8;
		cpu.npc = (pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
		break;
	case 0x04: branch(R(RS) == R(RT), pc, instr); break;              /* beq */
	case 0x05: branch(R(RS) != R(RT), pc, instr); break;              /* bne */
	case 0x06: branch((s32)R(RS) <= 0, pc, instr); break;             /* blez */
	case 0x07: branch((s32)R(RS) > 0, pc, instr); break;              /* bgtz */
	case 0x08: case 0x09: R(RT) = R(RS) + (u32)IMM; break;            /* addi(u) */
	case 0x0A: R(RT) = (s32)R(RS) < IMM; break;                       /* slti */
	case 0x0B: R(RT) = R(RS) < (u32)IMM; break;                       /* sltiu */
	case 0x0C: R(RT) = R(RS) & UIMM; break;
	case 0x0D: R(RT) = R(RS) | UIMM; break;
	case 0x0E: R(RT) = R(RS) ^ UIMM; break;
	case 0x0F: R(RT) = UIMM << 16; break;                             /* lui */
	case 0x10: break;                                  /* cop0: sin uso en modo usuario */
	case 0x11: op_cop1(instr, pc); break;
	case 0x14: branch_likely(R(RS) == R(RT), pc, instr); break;       /* beql */
	case 0x15: branch_likely(R(RS) != R(RT), pc, instr); break;       /* bnel */
	case 0x16: branch_likely((s32)R(RS) <= 0, pc, instr); break;      /* blezl */
	case 0x17: branch_likely((s32)R(RS) > 0, pc, instr); break;       /* bgtzl */
	case 0x1C:                                         /* special2: halt, mfic, mtic */
		if(FUNC == 0x24) R(RT) = (u32)hle_get_intr_enabled();
		else if(FUNC == 0x26) hle_set_intr_enabled(R(RT) & 1);
		break;
	case 0x1F: op_special3(instr, pc); break;
	case 0x20: R(RT) = (u32)(s32)(s8)ld8(R(RS) + IMM, instr); break;   /* lb */
	case 0x21: R(RT) = (u32)(s32)(s16)ld16(R(RS) + IMM, instr); break; /* lh */
	case 0x23: R(RT) = ld32(R(RS) + IMM, instr); break;                /* lw */
	case 0x24: R(RT) = ld8(R(RS) + IMM, instr); break;                 /* lbu */
	case 0x25: R(RT) = ld16(R(RS) + IMM, instr); break;                /* lhu */
	case 0x22: case 0x26: {                                            /* lwl / lwr */
		addr = R(RS) + IMM;
		u32 sh = (addr & 3) * 8, mem = ld32(addr & ~3u, instr), rt = R(RT);
		if((instr >> 26) == 0x22) R(RT) = (rt & (0x00FFFFFFu >> sh)) | (mem << (24 - sh));
		else R(RT) = (rt & ~(0xFFFFFFFFu >> sh)) | (mem >> sh);
		break;
	}
	case 0x28: st8(R(RS) + IMM, R(RT), instr); break;                  /* sb */
	case 0x29: st16(R(RS) + IMM, R(RT), instr); break;                 /* sh */
	case 0x2B: st32(R(RS) + IMM, R(RT), instr); break;                 /* sw */
	case 0x2A: case 0x2E: {                                            /* swl / swr */
		addr = R(RS) + IMM;
		u32 sh = (addr & 3) * 8, mem = ld32(addr & ~3u, instr), rt = R(RT);
		if((instr >> 26) == 0x2A) mem = (mem & ~(0xFFFFFFFFu >> (24 - sh))) | (rt >> (24 - sh));
		else mem = (mem & ~(0xFFFFFFFFu << sh)) | (rt << sh);
		st32(addr & ~3u, mem, instr);
		break;
	}
	case 0x2F: break;                                                  /* cache */
	case 0x30: R(RT) = ld32(R(RS) + IMM, instr); cpu.llbit = 1; break; /* ll */
	case 0x38:                                                         /* sc */
		rs = R(RS);
		if(cpu.llbit) st32(rs + IMM, R(RT), instr);
		/* En la PSP, sc no borra el bit de enlace: solo lo hacen los
		   syscalls y las interrupciones (pspautotests cpu/lsu/llsc) */
		R(RT) = (u32)cpu.llbit;
		break;
	case 0x31: FI(FT) = ld32(R(RS) + IMM, instr); break;               /* lwc1 */
	case 0x39: st32(R(RS) + IMM, FI(FT), instr); break;                /* swc1 */
	default:
		cpu_fault("instruccion desconocida (VFPU aun no implementada)", pc, instr);
	}
	cpu.r[0] = 0;
}

/* cpu_cycles avanza con cada instrucción para que el HLE vea la hora
   exacta; si una llamada del HLE ejecuta código del juego (o adelanta el
   reloj), ese tiempo también cuenta para el límite. */
u32 cpu_run(u32 max_cycles){
	u64 start = cpu_cycles, end = cpu_cycles + max_cycles;
	while(cpu_cycles < end && !cpu_stop_requested){
		step();
		cpu_cycles++;
		cpu_executed++;
	}
	return (u32)(cpu_cycles - start);
}
