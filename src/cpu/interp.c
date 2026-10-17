/**
 * WIISP - interp.c
 * Intérprete del Allegrex (ver cpu.h).
 *
 * Modelo de delay slots: cada paso ejecuta la instrucción en pc y avanza
 * (pc, npc) = (npc, npc + 4). Un salto solo cambia npc, así que la
 * instrucción siguiente (el delay slot) se ejecuta antes de llegar al
 * destino, igual que en el hardware. Un "branch likely" no tomado se salta
 * el delay slot avanzando una vez más.
 *
 * Códigos de operación según la documentación del Allegrex, comprobados con
 * pspautotests.
 *
 * Coste por instrucción (optimización 2.1 de la hoja de ruta, cpu_sin_extra)
 * ---------------------------------------------------------------------------
 * Lo que no es la instrucción en sí se hace lo menos posible:
 *   - pc, npc, el puntero a la RAM y su tamaño viven en variables locales
 *     (registros del Broadway) y no en memoria;
 *   - los ciclos y las instrucciones ejecutadas se cuentan con un contador
 *     local de 32 bits que baja hasta 0, en vez de sumar a dos contadores
 *     de 64 bits en memoria en cada instrucción;
 *   - la instrucción se lee de la RAM directamente (una comparación y una
 *     carga con los bytes al revés); el resto de la memoria va por un camino
 *     lento fuera de línea;
 *   - todo va en una sola función y los switch de los grupos más usados
 *     tienen un caso para cada valor, así el salto por tabla no comprueba
 *     el rango.
 * El estado global (cpu.pc, cpu.npc, cpu_cycles, cpu_executed) se pone al día
 * solo antes de salir al HLE (syscall), a la VFPU, a los caminos lentos de
 * memoria y a los fallos, y cpu_stop_requested solo se mira después de esas
 * llamadas, que son las únicas que pueden pedir parar. Así el resultado es el
 * mismo ciclo a ciclo que comprobando todo en cada instrucción, que es lo que
 * hace cpu_run_ref (interp_ref.c) y lo que se usa con cpu_sin_extra=0.
 *
 * En el Wii, un addiu pasa de 54 instrucciones del Broadway (20 accesos a
 * memoria) a 24 (5 accesos), y un lw de unas 66 a 29.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include <stdio.h>
#include "cpu/cpu.h"
#include "cpu/vfpu.h"
#include "cpu/predec.h"
#include "core/memory.h"
#include "core/opt.h"

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

#define R(n)  r[n]
#define FS   ((instr >> 11) & 31)
#define FT   ((instr >> 16) & 31)
#define FD   ((instr >> 6) & 31)
#define F(n)  cpu.fpr[n].f
#define FI(n) cpu.fpr[n].u

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------------ */
/* Caminos lentos (fuera de línea)                                    */
/* ------------------------------------------------------------------ */

/* Todo lo que no es la RAM principal: VRAM (con sus ganchos), scratchpad
   y direcciones inválidas (fallo). cpu.pc ya está al día. */
__attribute__((noinline)) u32 cpu_slow_load(u32 addr, u32 size, u32 instr){
	const u8 *p = mem_ptr_r_slow(addr, size);
	if(!p){
		cpu_fault("acceso a memoria invalido", addr, instr);
		return 0;
	}
	return size == 4 ? rd_le32(p) : size == 2 ? rd_le16(p) : *p;
}

__attribute__((noinline)) void cpu_slow_store(u32 addr, u32 size, u32 v, u32 instr){
	u8 *p = mem_ptr_slow(addr, size);
	if(!p){
		cpu_fault("acceso a memoria invalido", addr, instr);
		return;
	}
	if(size == 4) wr_le32(p, v);
	else if(size == 2) wr_le16(p, (u16)v);
	else *p = (u8)v;
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
   0xFFC00000; PowerPC ya coincide). Si una entrada era NaN, se propaga el
   de la primera que lo sea, silencioso: así no depende del orden en que el
   compilador ponga los operandos (con dos NaN distintos, x86 y PowerPC
   devuelven el del primero que les toque). */
static inline void set_arith(u32 fd, float res, float a, float b){
	union { float f; u32 u; } ua, ub;
	if(!isnan(res)){ F(fd) = res; return; }
	ua.f = a;
	ub.f = b;
	if(isnan(a)) FI(fd) = ua.u | 0x00400000u;
	else if(isnan(b)) FI(fd) = ub.u | 0x00400000u;
	else FI(fd) = 0x7FC00000u;
}

/* Formato S. Devuelve 0 si la instrucción no existe. */
int cpu_cop1_s(u32 instr, u32 ipc){
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
			cpu.fcc_hazard_pc = ipc + 4;
			if(res) cpu.fcr31 |= FCR31_COND;
			else cpu.fcr31 &= ~FCR31_COND;
		} else return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Bucle principal                                                    */
/* ------------------------------------------------------------------ */

/* cpu_cycles avanza con cada instrucción para que el HLE vea la hora
   exacta; si una llamada del HLE ejecuta código del juego (o adelanta el
   reloj), ese tiempo también cuenta para el límite.

   Contabilidad local: `left` son las instrucciones que quedan hasta `end`
   contando la actual; `seg` es lo que valía `left` la última vez que se
   pusieron al día cpu_cycles y cpu_executed (seg - left = ejecutadas desde
   entonces). */
u32 cpu_run(u32 max_cycles){
	if(opt_on[OPT_CPU_PREDEC]) return cpu_run_predec(max_cycles);
	return cpu_run_interp(max_cycles);
}

u32 cpu_run_interp(u32 max_cycles){
	const u64 start = cpu_cycles;
	u64 end = start + max_cycles;
	u8 *const ram = psp_mem.ram;
	const u32 lim4 = psp_mem.ram_size - 3, lim2 = psp_mem.ram_size - 1, lim1 = psp_mem.ram_size;
	u32 *const r = cpu.r;
	u32 pc, npc, left, seg;

	if(!opt_on[OPT_CPU_SIN_EXTRA]) return cpu_run_ref(max_cycles);
	if(cpu_stop_requested || !max_cycles) return 0;
	pc = cpu.pc;
	npc = cpu.npc;
	left = seg = max_cycles;

/* Antes de llamar fuera (HLE, VFPU, memoria lenta, fallos) el estado global
   se pone al día: quien está fuera puede leer pc, el reloj o las
   instrucciones ejecutadas (los informes de un fallo, por ejemplo) */
#define SYNC() (cpu.pc = pc, cpu.npc = npc, cpu_cycles = end - left, \
                cpu_executed += seg - left, seg = left)
/* Tras una llamada que no toca el reloj ni cambia de hilo (todas salvo los
   syscalls): si pidió parar, esta instrucción es la última */
#define RESUME() do { \
		if(unlikely(cpu_stop_requested)){ end = cpu_cycles + 1; left = seg = 1; } \
	} while(0)
#define FAULT(what, addr) do { SYNC(); cpu_fault(what, addr, instr); RESUME(); } while(0)

/* Memoria: la RAM principal en línea, el resto fuera */
#define RAM_OFF(a) (((a) & PSP_ADDR_MASK) - PSP_RAM_BASE)
#define LD(dst, a, size, lim, rd) do { \
		u32 a_ = (a), o_ = RAM_OFF(a_); \
		if(likely(o_ < (lim))) dst = rd; \
		else { SYNC(); dst = cpu_slow_load(a_, size, instr); RESUME(); } \
	} while(0)
#define LD32(dst, a) LD(dst, a, 4, lim4, rd_le32(ram + o_))
#define LD16(dst, a) LD(dst, a, 2, lim2, rd_le16(ram + o_))
#define LD8(dst, a)  LD(dst, a, 1, lim1, ram[o_])
/* Una escritura donde hay código predecodificado lo tira (cpu/predec.h) */
#define ST(a, v, size, lim, wr) do { \
		u32 a_ = (a), o_ = RAM_OFF(a_); \
		if(likely(o_ < (lim))){ \
			wr; \
			if(unlikely(cpu_code_map[o_ >> PREDEC_ZONA_BITS])) cpu_predec_store(o_, size); \
		} \
		else { SYNC(); cpu_slow_store(a_, size, v, instr); RESUME(); } \
	} while(0)
#define ST32(a, v) ST(a, v, 4, lim4, wr_le32(ram + o_, v))
#define ST16(a, v) ST(a, v, 2, lim2, wr_le16(ram + o_, (u16)(v)))
#define ST8(a, v)  ST(a, v, 1, lim1, ram[o_] = (u8)(v))

/* Saltos: ipc es la dirección de la instrucción de salto */
#define BRANCH(cond) do { if(cond) npc = ipc + 4 + ((u32)IMM << 2); } while(0)
#define BRANCH_LIKELY(cond) do { \
		if(cond) npc = ipc + 4 + ((u32)IMM << 2); \
		else { pc = npc; npc += 4; }   /* anula el delay slot */ \
	} while(0)

	for(;;){
		const u32 ipc = pc;
		u32 instr, o = RAM_OFF(ipc);
		if(likely(o < lim4)) instr = rd_le32(ram + o);
		else { SYNC(); instr = cpu_slow_load(ipc, 4, 0); RESUME(); }

		pc = npc;
		npc += 4;

		switch(instr >> 26){
		case 0x00: {                                                   /* SPECIAL */
			const u32 rs = R(RS), rt = R(RT);
			switch(FUNC){
			case 0x00: R(RD) = rt << SA; break;                       /* sll */
			case 0x02:                                                 /* srl / rotr */
				if(RS & 1) R(RD) = SA ? (rt >> SA) | (rt << (32 - SA)) : rt;
				else R(RD) = rt >> SA;
				break;
			case 0x03: R(RD) = (u32)((s32)rt >> SA); break;           /* sra */
			case 0x04: R(RD) = rt << (rs & 31); break;                 /* sllv */
			case 0x06: {                                               /* srlv / rotrv */
				u32 sh = rs & 31;
				if(SA & 1) R(RD) = sh ? (rt >> sh) | (rt << (32 - sh)) : rt;
				else R(RD) = rt >> sh;
				break;
			}
			case 0x07: R(RD) = (u32)((s32)rt >> (rs & 31)); break;    /* srav */
			case 0x08: npc = rs; break;                                /* jr */
			case 0x09: npc = rs; R(RD) = ipc + 8; break;               /* jalr */
			case 0x0A: if(rt == 0) R(RD) = rs; break;                  /* movz */
			case 0x0B: if(rt != 0) R(RD) = rs; break;                  /* movn */
			case 0x0C:                                                 /* syscall */
				/* El HLE ve cpu.pc en la instrucción siguiente y el reloj
				   sin contar esta; puede cambiar de hilo, ejecutar código
				   del juego o adelantar el reloj */
				cpu.llbit = 0;
				SYNC();
				hle_syscall((instr >> 6) & 0xFFFFF);
				goto after_call;
			case 0x0D: FAULT("break", ipc); break;                     /* break */
			case 0x0F: break;                                          /* sync */
			case 0x10: R(RD) = cpu.hi; break;                          /* mfhi */
			case 0x11: cpu.hi = rs; break;                             /* mthi */
			case 0x12: R(RD) = cpu.lo; break;                          /* mflo */
			case 0x13: cpu.lo = rs; break;                             /* mtlo */
			case 0x16: R(RD) = rs ? (u32)__builtin_clz(rs) : 32; break;   /* clz */
			case 0x17: R(RD) = ~rs ? (u32)__builtin_clz(~rs) : 32; break; /* clo */
			case 0x18: {                                               /* mult */
				s64 m = (s64)(s32)rs * (s32)rt;
				cpu.lo = (u32)m; cpu.hi = (u32)((u64)m >> 32);
				break;
			}
			case 0x19: {                                               /* multu */
				u64 m = (u64)rs * rt;
				cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32);
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
			case 0x01: case 0x05: case 0x0E: case 0x14: case 0x15: case 0x1E:
			case 0x1F: case 0x28: case 0x29: case 0x30 ... 0x3F:
				FAULT("instruccion SPECIAL desconocida", ipc);
			}
			break;
		}
		case 0x01: {                                                   /* REGIMM */
			const s32 rs = (s32)R(RS);
			switch(RT){
			case 0x00: BRANCH(rs < 0); break;                          /* bltz */
			case 0x01: BRANCH(rs >= 0); break;                         /* bgez */
			case 0x02: BRANCH_LIKELY(rs < 0); break;                   /* bltzl */
			case 0x03: BRANCH_LIKELY(rs >= 0); break;                  /* bgezl */
			case 0x10: R(R_RA) = ipc + 8; BRANCH(rs < 0); break;       /* bltzal */
			case 0x11: R(R_RA) = ipc + 8; BRANCH(rs >= 0); break;      /* bgezal */
			case 0x12: R(R_RA) = ipc + 8; BRANCH_LIKELY(rs < 0); break;   /* bltzall */
			case 0x13: R(R_RA) = ipc + 8; BRANCH_LIKELY(rs >= 0); break;  /* bgezall */
			case 0x04 ... 0x0F: case 0x14 ... 0x1F:
				FAULT("instruccion REGIMM desconocida", ipc);
			}
			break;
		}
		case 0x02: npc = (ipc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2); break; /* j */
		case 0x03:                                                                    /* jal */
			R(R_RA) = ipc + 8;
			npc = (ipc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
			break;
		case 0x04: BRANCH(R(RS) == R(RT)); break;                      /* beq */
		case 0x05: BRANCH(R(RS) != R(RT)); break;                      /* bne */
		case 0x06: BRANCH((s32)R(RS) <= 0); break;                     /* blez */
		case 0x07: BRANCH((s32)R(RS) > 0); break;                      /* bgtz */
		case 0x08: case 0x09: R(RT) = R(RS) + (u32)IMM; break;         /* addi(u) */
		case 0x0A: R(RT) = (s32)R(RS) < IMM; break;                    /* slti */
		case 0x0B: R(RT) = R(RS) < (u32)IMM; break;                    /* sltiu */
		case 0x0C: R(RT) = R(RS) & UIMM; break;
		case 0x0D: R(RT) = R(RS) | UIMM; break;
		case 0x0E: R(RT) = R(RS) ^ UIMM; break;
		case 0x0F: R(RT) = UIMM << 16; break;                          /* lui */
		case 0x10: break;                                  /* cop0: sin uso en modo usuario */
		case 0x11:                                                     /* COP1 */
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
				int cond = (cpu.fcr31 & FCR31_COND) != 0, taken;
				if(ipc == cpu.fcc_hazard_pc) cond = cpu.fcc_old;
				cpu.fcc_hazard_pc = 0;
				taken = (RT & 1) ? cond : !cond;
				if(RT & 2) BRANCH_LIKELY(taken);
				else BRANCH(taken);
				break;
			}
			case 0x10:                                                 /* formato S */
				if(!cpu_cop1_s(instr, ipc)) FAULT("instruccion FPU desconocida", ipc);
				break;
			case 0x14:                                                 /* formato W */
				if(FUNC == 0x20) F(FD) = (float)(s32)FI(FS);          /* cvt.s.w */
				else FAULT("instruccion FPU (W) desconocida", ipc);
				break;
			default: FAULT("instruccion COP1 desconocida", ipc);
			}
			break;
		case 0x14: BRANCH_LIKELY(R(RS) == R(RT)); break;               /* beql */
		case 0x15: BRANCH_LIKELY(R(RS) != R(RT)); break;               /* bnel */
		case 0x16: BRANCH_LIKELY((s32)R(RS) <= 0); break;              /* blezl */
		case 0x17: BRANCH_LIKELY((s32)R(RS) > 0); break;               /* bgtzl */
		case 0x1C:                                         /* special2: halt, mfic, mtic */
			if(FUNC == 0x24) R(RT) = (u32)hle_get_intr_enabled();
			else if(FUNC == 0x26){
				/* Puede pedir parar para atender interrupciones pendientes */
				SYNC();
				hle_set_intr_enabled(R(RT) & 1);
				RESUME();
			}
			break;
		case 0x1F: {                                                   /* SPECIAL3 */
			const u32 rs = R(RS), rt = R(RT);
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
					u32 v = rt, b = 0, i;
					for(i = 0; i < 32; i++, v >>= 1) b = (b << 1) | (v & 1);
					R(RD) = b;
					break;
				}
				case 0x18: R(RD) = (u32)(s32)(s16)rt; break;                                     /* seh */
				default: FAULT("instruccion BSHFL desconocida", ipc);
				}
				break;
			default: FAULT("instruccion SPECIAL3 desconocida", ipc);
			}
			break;
		}
		case 0x20: { u32 v; LD8(v, R(RS) + IMM); R(RT) = (u32)(s32)(s8)v; break; }     /* lb */
		case 0x21: { u32 v; LD16(v, R(RS) + IMM); R(RT) = (u32)(s32)(s16)v; break; }   /* lh */
		case 0x23: { u32 v; LD32(v, R(RS) + IMM); R(RT) = v; break; }                  /* lw */
		case 0x24: { u32 v; LD8(v, R(RS) + IMM); R(RT) = v; break; }                   /* lbu */
		case 0x25: { u32 v; LD16(v, R(RS) + IMM); R(RT) = v; break; }                  /* lhu */
		case 0x22: case 0x26: {                                            /* lwl / lwr */
			u32 addr = R(RS) + IMM, sh = (addr & 3) * 8, mem, rt = R(RT);
			LD32(mem, addr & ~3u);
			if((instr >> 26) == 0x22) R(RT) = (rt & (0x00FFFFFFu >> sh)) | (mem << (24 - sh));
			else R(RT) = (rt & ~(0xFFFFFFFFu >> sh)) | (mem >> sh);
			break;
		}
		case 0x28: ST8(R(RS) + IMM, R(RT)); break;                         /* sb */
		case 0x29: ST16(R(RS) + IMM, R(RT)); break;                        /* sh */
		case 0x2B: ST32(R(RS) + IMM, R(RT)); break;                        /* sw */
		case 0x2A: case 0x2E: {                                            /* swl / swr */
			u32 addr = R(RS) + IMM, sh = (addr & 3) * 8, mem, rt = R(RT);
			LD32(mem, addr & ~3u);
			if((instr >> 26) == 0x2A) mem = (mem & ~(0xFFFFFFFFu >> (24 - sh))) | (rt >> (24 - sh));
			else mem = (mem & ~(0xFFFFFFFFu << sh)) | (rt << sh);
			ST32(addr & ~3u, mem);
			break;
		}
		case 0x2F:                                                         /* cache */
			/* Hit Writeback (Invalidate) D: el juego escribió esa línea de 64
			   bytes con la CPU (avisa a las cachés de texturas) */
			if(RT == 0x1A || RT == 0x1B) mem_note_write((R(RS) + IMM) & ~63u, 64);
			break;
		case 0x30: { u32 v; LD32(v, R(RS) + IMM); R(RT) = v; cpu.llbit = 1; break; } /* ll */
		case 0x38: {                                                       /* sc */
			const u32 base = R(RS);
			if(cpu.llbit) ST32(base + IMM, R(RT));
			/* En la PSP, sc no borra el bit de enlace: solo lo hacen los
			   syscalls y las interrupciones (pspautotests cpu/lsu/llsc) */
			R(RT) = (u32)cpu.llbit;
			break;
		}
		case 0x31: { u32 v; LD32(v, R(RS) + IMM); FI(FT) = v; break; }    /* lwc1 */
		case 0x39: ST32(R(RS) + IMM, FI(FT)); break;                       /* swc1 */
		case 0x12:                                                         /* COP2 */
			if(RS == 8){                                   /* bvf, bvt, bvfl, bvtl */
				int val;
				if(unlikely(!vfpu_live)){ SYNC(); hle_vfpu_load(); RESUME(); }
				vfpu_stat_bv++;
				val = (vfpu.ctrl[VFPU_CTRL_CC] >> ((instr >> 18) & 7)) & 1;
				if(RT & 2) BRANCH_LIKELY(val == (int)(RT & 1));
				else BRANCH(val == (int)(RT & 1));
			} else {
				SYNC();
				vfpu_exec(instr, ipc);
				RESUME();
			}
			break;
		case 0x18: case 0x19: case 0x1B: case 0x32: case 0x34: case 0x35: case 0x36:
		case 0x37: case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			SYNC();
			vfpu_exec(instr, ipc);
			RESUME();
			break;
		/* Los 64 valores tienen su caso: así el salto por tabla no
		   necesita comprobar el rango */
		case 0x13: case 0x1A: case 0x1D: case 0x1E: case 0x27: case 0x2C:
		case 0x2D: case 0x33: case 0x3B:
			FAULT("instruccion desconocida", ipc);
		}
		r[0] = 0;
		if(likely(--left)) continue;
		break;

	after_call:
		/* Vuelta del HLE: el estado puede ser otro (cambio de hilo, código
		   del juego ejecutado, reloj adelantado). Cuenta esta instrucción y
		   sigue si queda tiempo y nadie pidió parar. */
		pc = cpu.pc;
		npc = cpu.npc;
		r[0] = 0;
		cpu_cycles++;
		cpu_executed++;
		if(cpu_stop_requested || cpu_cycles >= end) return (u32)(cpu_cycles - start);
		left = seg = (u32)(end - cpu_cycles);
	}
	cpu.pc = pc;
	cpu.npc = npc;
	cpu_cycles = end;
	cpu_executed += seg;
	return (u32)(cpu_cycles - start);

#undef SYNC
#undef RESUME
#undef FAULT
#undef RAM_OFF
#undef LD
#undef LD32
#undef LD16
#undef LD8
#undef ST
#undef ST32
#undef ST16
#undef ST8
#undef BRANCH
#undef BRANCH_LIKELY
}
