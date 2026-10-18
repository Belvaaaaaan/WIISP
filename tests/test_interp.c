/**
 * WIISP - tests/test_interp.c
 * El intérprete (cpu_run con cpu_sin_extra=1) y el predecodificado
 * (cpu_predec, cpu/predec.c) contra el de referencia (cpu_run_ref):
 * programas aleatorios con todas las familias de
 * instrucciones (ALU, saltos con y sin likely, enlaces, FPU con el riesgo
 * de bc1x, cargas y almacenamientos en la RAM y sus espejos, el scratchpad
 * y la VRAM, ll/sc, cache, VFPU, mfic/mtic) y programas con fallos
 * (instrucciones y direcciones inválidas). Cada programa se ejecuta en
 * trozos de tamaño aleatorio, los mismos para los dos, y tienen que acabar
 * en el mismo estado exacto: registros, memoria, pc, npc, ciclos,
 * instrucciones ejecutadas, lo que devuelve cada llamada y la petición de
 * parar. Algunos programas se reescriben a sí mismos mientras corren
 * (escriben nops o basura en su propio código): el predecodificado tiene que
 * tirar sus bloques en el acto.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu/cpu.h"
#include "cpu/predec.h"
#include "cpu/vfpu.h"
#include "core/memory.h"
#include "core/opt.h"
#include "test.h"

#define CODE      0x08A00340u   /* cruza una zona de 1 KB del predecodificado */
#define DATA      0x08B00000u   /* r28 apunta a DATA + 0x800 */
#define NPROG     96            /* instrucciones aleatorias por programa */
#define NCHUNKS   24
#define VRAM_SNAP 0x10000u      /* lo que alcanzan los espejos con las bases de la prueba */

static u32 rs_state = 0x1234567u;
static u32 rnd(void){ rs_state ^= rs_state << 13; rs_state ^= rs_state >> 17; rs_state ^= rs_state << 5; return rs_state; }
static u32 rn(u32 n){ return rnd() % n; }
static u32 pick(const u32 *v, u32 n){ return v[rn(n)]; }
#define PICK(a) pick(a, (u32)(sizeof(a) / sizeof(a[0])))

static u32 rtype(u32 op, u32 rs, u32 rt, u32 rd, u32 sa, u32 func){
	return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | func;
}
static u32 itype(u32 op, u32 rs, u32 rt, u32 imm){ return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }

/* r28 (datos en la RAM), r29 (datos por el camino lento), r30 (el código)
   y r31 (vuelta de los enlaces) no se escriben salvo con los enlaces: así
   los saltos por registro y los accesos a memoria caen donde se espera */
static u32 dreg(void){ return rn(8) ? 1 + rn(27) : 0; }
static u32 sreg(void){ return rn(32); }
static u32 boff(u32 i){ return (u32)((s32)rn(NPROG + 2) - (s32)i - 1) & 0xFFFF; }
static u32 imm16(void){
	static const u32 sp[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFF, 0xFFFE };
	return rn(3) ? rn(0x10000) : PICK(sp);
}
static u32 moff(u32 align){ return ((u32)((s32)rn(0x1000) - 0x800)) & ~(align - 1) & 0xFFFF; }
static u32 mbase(void){ return rn(4) ? 28 : 29; }

static u32 rval(void){
	switch(rn(8)){
	case 0: return 0;
	case 1: return 0xFFFFFFFFu;
	case 2: return 0x80000000u;
	case 3: return rn(64);
	case 4: return 0x7FFFFFFFu;
	default: return rnd();
	}
}

static u32 fval(void){
	static const u32 sp[] = {
		0, 0x80000000u, 0x3F800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u,
		0xFFC00000u, 0x00000001u, 0x4F400000u, 0xCF400000u, 0x3F000000u, 0x3FC00000u, 0x40200000u
	};
	float f;
	u32 u;
	switch(rn(4)){
	case 0: return PICK(sp);
	case 1: return rnd();
	default:
		f = ((float)(s32)(rnd() & 0xFFFFF) - 524288.0f) / (float)(1 + rn(1000));
		memcpy(&u, &f, 4);
		return u;
	}
}

/* Una instrucción que falla (solo en los programas con fallos); con r29
   inválido, también un salto ahí (falla al leer la instrucción) */
static u32 gen_fault(u32 i, int bad29){
	static const u32 bad_op[] = { 0x13, 0x1A, 0x1D, 0x1E, 0x27, 0x2C, 0x2D, 0x33, 0x3B };
	static const u32 bad_special[] = { 0x01, 0x05, 0x0E, 0x14, 0x15, 0x1E, 0x28, 0x30, 0x3F };
	if(bad29 && !rn(4)) return rtype(0, 29, 0, 0, 0, 0x08);                           /* jr r29 */
	switch(rn(8)){
	case 0: return rtype(0, sreg(), sreg(), 0, rn(32), 0x0D);                         /* break */
	case 1: return itype(PICK(bad_op), sreg(), sreg(), imm16());
	case 2: return rtype(0, sreg(), sreg(), dreg(), 0, PICK(bad_special));
	case 3: return itype(1, sreg(), 0x04 + rn(4), boff(i));                           /* REGIMM */
	case 4: return rtype(0x11, 0x10, rn(32), rn(32), rn(32), 0x08 + rn(4));            /* FPU S */
	case 5: return rtype(0x11, 0x01, dreg(), rn(32), 0, 0);                            /* COP1 */
	case 6: return rtype(0x1F, sreg(), sreg(), dreg(), rn(2) ? 0x01 : rn(32), rn(2) ? 0x20 : 0x3F); /* BSHFL, SPECIAL3 */
	default: return rtype(0x11, 0x14, rn(32), rn(32), rn(32), 0x21);                   /* FPU W */
	}
}

/* Programas que se reescriben: escrituras en su propio código (r30) */
static int smc;

/* Escribe una o dos instrucciones en code[i]; devuelve cuántas */
static u32 gen(u32 *code, u32 i, int faults, int bad29){
	static const u32 alu3[] = { 0x04, 0x06, 0x07, 0x0A, 0x0B, 0x16, 0x17, 0x20, 0x21, 0x22, 0x23,
	                            0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2C, 0x2D };
	static const u32 hilo[] = { 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x2E, 0x2F };
	static const u32 regimm[] = { 0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13 };
	static const u32 bops[] = { 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17 };
	static const u32 fpu_s[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0E, 0x0F, 0x24 };
	static const u32 bshfl[] = { 0x02, 0x03, 0x10, 0x14, 0x18 };
	static const u32 loads[] = { 0x20, 0x21, 0x23, 0x24, 0x25 };
	static const u32 stores[] = { 0x28, 0x29, 0x2B };
	static const u32 unal[] = { 0x22, 0x26, 0x2A, 0x2E };
	static const u32 cacheop[] = { 0x1A, 0x1B, 0x14, 0x08 };
	u32 f, op;

	if(faults && !rn(48)){ code[i] = gen_fault(i, bad29); return 1; }
	if(smc && !rn(10)){
		/* Un nop (sw de $zero) o basura (otro registro) en una instrucción
		   del programa, delante o detrás */
		u32 to = rn(NPROG + 2);
		if(faults && !rn(3)) code[i] = rn(2) ? itype(0x3A, 30, rn(32), to * 4 + rn(4)) : itype(0x3E, 30, rn(32), (to & ~3u) * 4 + rn(2));   //Arreglo
		else if(rn(4)) code[i] = itype(0x2B, 30, rn(3) ? 0 : sreg(), to * 4);   //Arreglo
		else code[i] = itype(rn(2) ? 0x28 : 0x29, 30, sreg(), to * 4 + rn(2) * (rn(2) ? 1 : 2));
		return 1;
	}
	switch(rn(24)){
	case 0:
		f = PICK(alu3);
		code[i] = rtype(0, sreg(), sreg(), dreg(), f == 0x06 ? rn(2) : 0, f);
		break;
	case 1:                                                   /* sll, srl/rotr, sra */
		f = rn(3); f = f ? f + 1 : 0;
		code[i] = rtype(0, f == 2 ? rn(2) : 0, sreg(), dreg(), rn(32), f);
		break;
	case 2:
		code[i] = rtype(0, sreg(), sreg(), dreg(), 0, PICK(hilo));
		break;
	case 3:                                                   /* jr, jalr, sync */
		switch(rn(3)){
		case 0: code[i] = rtype(0, 30 + rn(2), 0, 0, 0, 0x08); break;
		case 1: code[i] = rtype(0, 30 + rn(2), 0, rn(2) ? 31 : dreg(), 0, 0x09); break;
		default: code[i] = rtype(0, 0, 0, 0, 0, 0x0F);
		}
		break;
	case 4: code[i] = itype(1, sreg(), PICK(regimm), boff(i)); break;
	case 5: code[i] = ((2 + rn(2)) << 26) | ((CODE >> 2) + rn(NPROG + 2)) ; break;  /* j, jal */
	case 6: code[i] = itype(PICK(bops), sreg(), sreg(), boff(i)); break;
	case 7: code[i] = itype(0x08 + rn(8), sreg(), dreg(), imm16()); break;
	case 8:                                                   /* mfc1, cfc1, mtc1, ctc1 */
		switch(rn(4)){
		case 0: code[i] = rtype(0x11, 0x00, dreg(), rn(32), 0, 0); break;
		case 1: code[i] = rtype(0x11, 0x02, dreg(), rn(3) ? (rn(2) ? 31 : 0) : rn(32), 0, 0); break;
		case 2: code[i] = rtype(0x11, 0x04, sreg(), rn(32), 0, 0); break;
		default: code[i] = rtype(0x11, 0x06, sreg(), rn(4) ? 31 : rn(32), 0, 0);
		}
		break;
	case 9:                                                   /* formato S y W */
		if(rn(6)) code[i] = rtype(0x11, 0x10, rn(32), rn(32), rn(32), rn(3) ? PICK(fpu_s) : 0x30 + rn(16));
		else code[i] = rtype(0x11, 0x14, 0, rn(32), rn(32), 0x20);
		break;
	case 10:                                                  /* c.cond y bc1x justo detrás */
		code[i] = rtype(0x11, 0x10, rn(32), rn(32), 0, 0x30 + rn(16));
		if(i + 1 >= NPROG) return 1;
		code[i + 1] = itype(0x11, 0x08, rn(4), boff(i + 1));
		return 2;
	case 11: code[i] = itype(0x11, 0x08, rn(4), boff(i)); break;  /* bc1x */
	case 12:                                                  /* mfic, mtic */
		code[i] = rn(2) ? rtype(0x1C, 0, dreg(), 0, 0, 0x24) : rtype(0x1C, 0, sreg(), 0, 0, 0x26);
		break;
	case 13:                                                  /* ext, ins, bshfl */
		switch(rn(3)){
		case 0: code[i] = rtype(0x1F, sreg(), dreg(), rn(32), rn(32), 0x00); break;
		case 1: code[i] = rtype(0x1F, sreg(), dreg(), rn(32), rn(32), 0x04); break;
		default: code[i] = rtype(0x1F, 0, sreg(), dreg(), PICK(bshfl), 0x20);
		}
		break;
	case 14:
		op = PICK(loads);
		code[i] = itype(op, mbase(), dreg(), moff(op == 0x23 ? 4 : op == 0x21 || op == 0x25 ? 2 : 1));
		break;
	case 15:
		op = PICK(stores);
		code[i] = itype(op, mbase(), sreg(), moff(op == 0x2B ? 4 : op == 0x29 ? 2 : 1));
		break;
	case 16:                                                  /* lwl, lwr, swl, swr */
		op = PICK(unal);
		code[i] = itype(op, mbase(), (op & 8) ? sreg() : dreg(), moff(1));
		break;
	case 17: code[i] = itype(rn(2) ? 0x30 : 0x38, mbase(), dreg(), moff(4)); break;   /* ll, sc */
	case 18: code[i] = itype(rn(2) ? 0x31 : 0x39, mbase(), rn(32), moff(4)); break;   /* lwc1, swc1 */
	case 19: code[i] = itype(0x2F, 28, PICK(cacheop), moff(1)); break;
	case 20: {                                                /* lv.s, sv.s */
		u32 vt = rn(128);
		switch(rn(4)){   //Arreglo
		case 0:   //Arreglo
			code[i] = itype(rn(2) ? 0x32 : 0x3A, mbase(), vt & 31, moff(4) | (vt >> 5));
			break;   //Arreglo
		case 1:   //Arreglo
			code[i] = itype(rn(2) ? 0x36 : 0x3E, mbase(), vt & 31, (faults && !rn(4) ? moff(4) : moff(16)) | (rn(2) << 1) | ((vt >> 5) & 1));   //Arreglo
			break;   //Arreglo
		case 2:   //Arreglo
			if(rn(2)) code[i] = rtype(0x12, 3, dreg(), 0, 0, 0) | (rn(6) ? vt : 128 + rn(20));   //Arreglo
			else code[i] = rtype(0x12, 7, sreg(), 0, 0, 0) | (rn(6) ? vt : 128 + rn(20));   //Arreglo
			break;   //Arreglo
		default:   //Arreglo
			code[i] = 0xDE000000u | (rn(3) ? 0 : rn(16) << 8) | rn(256);   //Arreglo
		}   //Arreglo
		break;
	}
	case 21: {                                                /* vadd, vsub, vcmp */
		u32 sz = (rn(2) << 15) | (rn(2) << 7);
		if(rn(3)) code[i] = (0x18u << 26) | (rn(2) << 23) | (rn(128) << 16) | sz | (rn(128) << 8) | rn(128);
		else code[i] = (0x1Bu << 26) | (rn(128) << 16) | sz | (rn(128) << 8) | rn(16);
		break;
	}
	case 22: code[i] = itype(0x12, 0x08, (rn(8) << 2) | rn(4), boff(i)); break;  /* bvf, bvt, bvfl, bvtl */
	default: code[i] = (0x10u << 26) | (rnd() & 0x03FFFFFFu); break;              /* cop0: nada */
	}
	return 1;
}

typedef struct {
	CpuState cpu;
	VfpuState vfpu;
	u64 vstat[VS_COUNT];   //Arreglo
	int vivo;   //Arreglo
	u64 cycles, executed;
	int stop, intr;
	u32 fault_pc, fault_npc, faults;
	u32 ret[NCHUNKS];
	u8 data[0x2000], scratch[PSP_SCRATCH_SIZE], vram[VRAM_SNAP];
	u8 code[(NPROG + 80) * 4];
} Snap;

static void take(Snap *s){
	s->cpu = cpu;
	s->vfpu = vfpu;
	s->cycles = cpu_cycles;
	s->executed = cpu_executed;
	s->stop = cpu_stop_requested;
	s->intr = hle_get_intr_enabled();
	s->fault_pc = cpu_fault_pc;
	s->fault_npc = cpu_fault_npc;
	s->faults = cpu_fault_count;
	memcpy(s->data, mem_ptr(DATA, sizeof(s->data)), sizeof(s->data));
	memcpy(s->scratch, psp_mem.scratch, sizeof(s->scratch));
	memcpy(s->vram, psp_mem.vram, sizeof(s->vram));
	memcpy(s->code, mem_ptr(CODE, sizeof(s->code)), sizeof(s->code));
}

static void put(const Snap *s){
	cpu = s->cpu;
	vfpu = s->vfpu;
	cpu_cycles = s->cycles;
	cpu_executed = s->executed;
	hle_set_intr_enabled(s->intr);
	cpu_stop_requested = s->stop;
	cpu_fault_pc = s->fault_pc;
	cpu_fault_npc = s->fault_npc;
	cpu_fault_count = s->faults;
	memcpy(mem_ptr(DATA, sizeof(s->data)), s->data, sizeof(s->data));
	memcpy(psp_mem.scratch, s->scratch, sizeof(s->scratch));
	memcpy(psp_mem.vram, s->vram, sizeof(s->vram));
	/* El código vuelve a ser el de antes: aviso a granel, como al cargar */
	memcpy(mem_ptr(CODE, sizeof(s->code)), s->code, sizeof(s->code));
	mem_note_write(CODE, sizeof(s->code));
}

/* modo: 0 el de referencia, 1 el rápido, 2 el predecodificado */
static int vivo = 1;   //Arreglo

static void run_chunks(Snap *out, const u32 *chunks, int modo){
	int k;
	u64 v0[VS_COUNT];   //Arreglo
	opt_on[OPT_CPU_SIN_EXTRA] = (u8)(modo != 0);
	opt_on[OPT_CPU_PREDEC] = (u8)(modo == 2);
	memset(out->ret, 0, sizeof(out->ret));
	vfpu_live = vivo;   //Arreglo
	vfpu_stats_fold();   //Arreglo
	memcpy(v0, vfpu_stat, sizeof(v0));   //Arreglo
	for(k = 0; k < NCHUNKS; k++){
		out->ret[k] = cpu_run(chunks[k]);
		if(cpu_stop_requested) break;
	}
	take(out);
	vfpu_stats_fold();   //Arreglo
	for(k = 0; k < VS_COUNT; k++) out->vstat[k] = vfpu_stat[k] - v0[k];   //Arreglo
	out->vivo = vfpu_live;   //Arreglo
}

static int report;

static int same(const Snap *a, const Snap *b, int prog){
	int i, ok = 1;
#define DIF(cond, ...) do { if(cond){ ok = 0; if(report < 12){ report++; printf("  programa %d: ", prog); printf(__VA_ARGS__); printf("\n"); } } } while(0)
	for(i = 0; i < 32; i++) DIF(a->cpu.r[i] != b->cpu.r[i], "r%d %08X != %08X", i, a->cpu.r[i], b->cpu.r[i]);
	for(i = 0; i < 32; i++) DIF(a->cpu.fpr[i].u != b->cpu.fpr[i].u, "f%d %08X != %08X", i, a->cpu.fpr[i].u, b->cpu.fpr[i].u);
	DIF(a->cpu.hi != b->cpu.hi || a->cpu.lo != b->cpu.lo, "hi/lo");
	DIF(a->cpu.pc != b->cpu.pc || a->cpu.npc != b->cpu.npc, "pc %08X/%08X != %08X/%08X", a->cpu.pc, a->cpu.npc, b->cpu.pc, b->cpu.npc);
	DIF(a->cpu.fcr31 != b->cpu.fcr31 || a->cpu.llbit != b->cpu.llbit, "fcr31/llbit");
	DIF(a->cpu.fcc_hazard_pc != b->cpu.fcc_hazard_pc || a->cpu.fcc_old != b->cpu.fcc_old, "riesgo de bc1x");
	DIF(memcmp(&a->vfpu, &b->vfpu, sizeof(a->vfpu)) != 0, "VFPU");
	DIF(memcmp(a->vstat, b->vstat, sizeof(a->vstat)) != 0, "recuentos de la VFPU");   //Arreglo
	DIF(a->vivo != b->vivo, "vfpu_live %d != %d", a->vivo, b->vivo);   //Arreglo
	DIF(a->cycles != b->cycles, "ciclos %llu != %llu", (unsigned long long)a->cycles, (unsigned long long)b->cycles);
	DIF(a->executed != b->executed, "ejecutadas %llu != %llu", (unsigned long long)a->executed, (unsigned long long)b->executed);
	DIF(a->stop != b->stop || a->intr != b->intr, "parar %d/%d, interrupciones %d/%d", a->stop, b->stop, a->intr, b->intr);
	DIF(memcmp(a->ret, b->ret, sizeof(a->ret)) != 0, "lo que devuelve cpu_run");
	DIF(a->faults != b->faults || a->fault_pc != b->fault_pc || a->fault_npc != b->fault_npc,
	    "fallos %u en %08X/%08X != %u en %08X/%08X", a->faults, a->fault_pc, a->fault_npc, b->faults, b->fault_pc, b->fault_npc);
	DIF(memcmp(a->data, b->data, sizeof(a->data)) != 0, "RAM");
	DIF(memcmp(a->scratch, b->scratch, sizeof(a->scratch)) != 0, "scratchpad");
	DIF(memcmp(a->vram, b->vram, sizeof(a->vram)) != 0, "VRAM");
	DIF(memcmp(a->code, b->code, sizeof(a->code)) != 0, "codigo");
#undef DIF
	return ok;
}

static void one_program(int prog, int faults, Snap *init, Snap *ra, Snap *rb, Snap *rc, u64 *total){
	static const u32 slow_bases[] = { 0x00010800u, 0x04000800u, 0x04200800u, 0x48B00800u, 0x88B00800u, 0x04600800u };
	static const u32 bad_bases[] = { 0x0A000000u, 0x00014000u, 0x04800000u, 0x00000800u };
	u32 *code = (u32 *)mem_ptr(CODE, (NPROG + 80) * 4);
	u32 chunks[NCHUNKS], i, k;
	const int bad29 = faults && rn(2);

	for(i = 0; i < NPROG; ) i += gen(code, i, faults, bad29);
	code[NPROG] = 0x1000FFFFu;              /* beq r0, r0, . */
	code[NPROG + 1] = 0;
	for(i = NPROG + 2; i < NPROG + 78; i++) code[i] = 0;   /* jr r31 tras el final: nops */
	code[NPROG + 78] = 0x1000FFFFu;
	code[NPROG + 79] = 0;
	for(i = 0; i < NPROG + 80; i++) wr_le32(&code[i], code[i]);
	mem_note_write(CODE, (NPROG + 80) * 4);   /* como un programa cargado */

	memset(&cpu, 0, sizeof(cpu));
	for(i = 1; i < 28; i++) cpu.r[i] = rval();
	cpu.r[28] = DATA + 0x800;
	cpu.r[29] = bad29 ? PICK(bad_bases) : PICK(slow_bases);
	cpu.r[30] = CODE;
	cpu.r[31] = CODE + 4 * rn(NPROG + 2);
	for(i = 0; i < 32; i++) cpu.fpr[i].u = fval();
	cpu.hi = rval(); cpu.lo = rval();
	cpu.fcr31 = rnd() & FCR31_WRITABLE;
	cpu.llbit = (int)rn(2);
	cpu.fcc_old = (int)rn(2);
	cpu.pc = CODE + 4 * rn(4);
	cpu.npc = cpu.pc + 4;
	vfpu_reset(&vfpu);
	for(i = 0; i < 128; i++) vfpu.vi[i] = fval();
	vfpu.ctrl[VFPU_CTRL_CC] = rn(64);
	cpu_cycles = rnd();
	cpu_executed = rnd();
	cpu_stop_requested = 0;
	hle_set_intr_enabled((int)rn(2));
	{
		u8 *d = mem_ptr(DATA, sizeof(init->data));
		for(i = 0; i < sizeof(init->data); i++) d[i] = (u8)rnd();
		for(i = 0; i < PSP_SCRATCH_SIZE; i++) psp_mem.scratch[i] = (u8)rnd();
		for(i = 0; i < VRAM_SNAP; i++) psp_mem.vram[i] = (u8)rnd();
	}
	for(k = 0; k < NCHUNKS; k++)
		chunks[k] = !rn(16) ? 0 : rn(3) ? 1 + rn(8) : 1 + rn(160);

	vivo = prog % 5 != 0;   //Arreglo
	take(init);
	run_chunks(ra, chunks, 0);
	put(init);
	run_chunks(rb, chunks, 1);
	*total += ra->executed - init->executed;
	CHECK(same(ra, rb, prog));
	//arreglo
	/* El predecodificado con los dos repartos de la memoria y las dos
	   copias del bucle; la mitad, con arenas diminutas (se vacían a
	   menudo, también a mitad de un programa; en una de cada cuatro se
	   llena antes la de lo frío) */
	if(prog % 4 == 0) cpu_predec_tam_pruebas(4096, 2048);
	else if(prog % 4 == 1) cpu_predec_tam_pruebas(16384, 1024);   /* se llena antes lo frío */
	else cpu_predec_tam_pruebas(0, 0);
	for(k = 0; k < 12; k++){   //Arreglo
		opt_on[OPT_PREDEC_COMPACTO] = (u8)(k & 1);
		opt_on[OPT_PREDEC_SIN_UNIR] = (u8)((k >> 1) & 1);   //Arreglo
		opt_on[OPT_PROF_CPU] = (u8)((k ^ prog) & 1 & (k >> 2 | prog >> 2));   //Arreglo
		opt_on[OPT_PROF_BLOQUES] = (u8)(((k >> 1) ^ prog) & 1 & (prog >> 3));   //Arreglo
		opt_on[OPT_CPU_VFPU_TARJETAS] = (u8)(k >> 2 != 1);   //Arreglo
		opt_on[OPT_CPU_VFPU_VERIFICAR] = (u8)(k >> 2 == 2);   //Arreglo
		put(init);
		run_chunks(rc, chunks, 2);
		CHECK(same(ra, rc, prog));
	}
}

void test_interp(void){
	static Snap init, ra, rb, rc;
	const u8 saved_opt = opt_on[OPT_CPU_SIN_EXTRA], saved_predec = opt_on[OPT_CPU_PREDEC];
	const u8 saved_comp = opt_on[OPT_PREDEC_COMPACTO], saved_su = opt_on[OPT_PREDEC_SIN_UNIR];   //arreglo
	const u8 saved_pc = opt_on[OPT_PROF_CPU], saved_pb = opt_on[OPT_PROF_BLOQUES];   //Arreglo
	const u8 saved_vt = opt_on[OPT_CPU_VFPU_TARJETAS], saved_vv = opt_on[OPT_CPU_VFPU_VERIFICAR];   //Arreglo
	const int saved_intr = hle_get_intr_enabled(), saved_live = vfpu_live;
	u64 total = 0;
	int p, err_fd, k;   //arreglo
	printf("interprete y predecodificado contra el de referencia\n");
	hle_vfpu_load();   //Arreglo

	for(p = 0; p < 3000; p++) one_program(p, 0, &init, &ra, &rb, &rc, &total);
	/* Con fallos: cpu_fault avisa por stderr, que aquí no interesa */
	fflush(stderr);
	err_fd = dup(2);
	if(err_fd >= 0){
		int null_fd = open("/dev/null", O_WRONLY);
		if(null_fd >= 0){ dup2(null_fd, 2); close(null_fd); }
	}
	for(; p < 3400; p++) one_program(p, 1, &init, &ra, &rb, &rc, &total);
	/* Que se reescriben: con nops y, con fallos, con basura */
	smc = 1;
	for(; p < 5400; p++) one_program(p, p >= 4900, &init, &ra, &rb, &rc, &total);
	smc = 0;
	if(err_fd >= 0){
		fflush(stderr);
		dup2(err_fd, 2);
		close(err_fd);
	}
	printf("  %d programas, %llu instrucciones comparadas\n", p, (unsigned long long)total);
	{
		PredecStats ps;
		cpu_predec_stats(&ps);
		printf("  predecodificado: %llu bloques hechos, %llu tirados, %llu busquedas, %llu instrucciones sueltas, "
		       "%llu escrituras en codigo\n", (unsigned long long)ps.bloques_hechos, (unsigned long long)ps.bloques_tirados,
		       (unsigned long long)ps.busquedas, (unsigned long long)ps.pasos, (unsigned long long)ps.escrituras_en_codigo);
		CHECK(ps.busquedas > 100000);
		CHECK(ps.bloques_tirados > 1000);
		CHECK(ps.vaciados > 1000);   //arreglo
		CHECK(ps.vfpu_tarjetas > 10000);   //Arreglo
		CHECK(ps.vfpu > 10000);   //Arreglo
		CHECK(ps.vfpu_verif > 10000);   //Arreglo
		CHECK_EQ(ps.vfpu_verif_mal, 0u);   //Arreglo
		printf("  VFPU: %llu en tarjetas propias, %llu por vfpu_exec, %llu verificadas\n", (unsigned long long)ps.vfpu_tarjetas,
		       (unsigned long long)ps.vfpu, (unsigned long long)ps.vfpu_verif);   //Arreglo
		printf("  %llu vaciados (arenas diminutas)\n", (unsigned long long)ps.vaciados);   //arreglo
	}
	cpu_predec_tam_pruebas(0, 0);   //arreglo

	/* cpu_verificar: código cambiado sin avisar (lo que pasaría si faltara
	   un aviso de escritura): el bloque viejo se ve al entrar y se rehace */
	{
		u32 *code = (u32 *)mem_ptr(CODE, 16);
		PredecStats a, b;
		const u8 saved_verif = opt_on[OPT_CPU_VERIFICAR];
		opt_on[OPT_CPU_SIN_EXTRA] = 1;
		opt_on[OPT_CPU_PREDEC] = 1;
		wr_le32(&code[0], itype(0x09, 2, 2, 1));        /* addiu r2, r2, 1 */
		wr_le32(&code[1], itype(0x04, 0, 0, 0xFFFE));   /* beq r0, r0, -2 */
		wr_le32(&code[2], 0);
		mem_note_write(CODE, 12);
		memset(&cpu, 0, sizeof(cpu));
		cpu.pc = CODE;
		cpu.npc = CODE + 4;
		cpu_stop_requested = 0;
		cpu_run(30);                                   /* 10 vueltas: r2 = 10 */
		CHECK_EQ(cpu.r[2], 10u);
		wr_le32(&code[0], itype(0x09, 2, 2, 100));      /* sin aviso */
		opt_on[OPT_CPU_VERIFICAR] = 1;
		cpu_predec_stats(&a);
		cpu_run(30);
		cpu_predec_stats(&b);
		CHECK_EQ(cpu.r[2], 1010u);
		CHECK(b.verif_viejos > a.verif_viejos);
		opt_on[OPT_CPU_VERIFICAR] = saved_verif;
	}
	/* Un enlace a un bloque de otra zona que se reescribió: A (al final de
	   una zona de 1 KB) salta a B (al principio de la siguiente); escribir
	   en B tira solo su zona, y A ya no puede usar el enlace que guardaba */
	{
		const u32 A = 0x08A003F0u, B = 0x08A00400u;
		cpu_predec_flush();
		mem_write32(A, itype(0x09, 2, 2, 1));                      /* addiu r2, r2, 1 */
		mem_write32(A + 4, (2u << 26) | (B >> 2));                 /* j B */
		mem_write32(A + 8, 0);
		mem_write32(B, itype(0x09, 3, 3, 1));                      /* addiu r3, r3, 1 */
		mem_write32(B + 4, (2u << 26) | (A >> 2));                 /* j A */
		mem_write32(B + 8, 0);
		memset(&cpu, 0, sizeof(cpu));
		cpu.pc = A;
		cpu.npc = A + 4;
		cpu_run(60);                                               /* 10 vueltas */
		CHECK_EQ(cpu.r[3], 10u);
		mem_write32(B, itype(0x09, 3, 3, 100));                     /* addiu r3, r3, 100 */
		cpu_run(60);
		CHECK_EQ(cpu.r[3], 1010u);
		CHECK_EQ(cpu.r[2], 20u);
	}
	/* Lo mismo cayendo al bloque siguiente (un branch likely no tomado al
	   final de una zona) */
	{
		const u32 A = 0x08A00BF8u, B = 0x08A00C00u;
		cpu_predec_flush();
		mem_write32(A, itype(0x15, 0, 0, 2));                      /* bnel r0, r0: nunca */
		mem_write32(A + 4, 0);
		mem_write32(B, itype(0x09, 5, 5, 1));                      /* addiu r5, r5, 1 */
		mem_write32(B + 4, (2u << 26) | (A >> 2));                 /* j A */
		mem_write32(B + 8, 0);
		memset(&cpu, 0, sizeof(cpu));
		cpu.pc = A;
		cpu.npc = A + 4;
		cpu_run(40);                                               /* 10 vueltas de 4 */
		CHECK_EQ(cpu.r[5], 10u);
		mem_write32(B, itype(0x09, 5, 5, 100));
		cpu_run(40);
		CHECK_EQ(cpu.r[5], 1010u);
	}
	//arreglo
	/* Cambiar de copia del bucle o de reparto de la memoria sin tocar el
	   código: los bloques hechos con la otra no pueden seguir */
	{
		const u32 P = 0x08A00900u;
		static const u8 su[10] = { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1 }, co[10] = { 1, 1, 0, 0, 1, 0, 0, 1, 1, 1 };   //Arreglo
		static const u8 me[10] = { 0, 0, 0, 0, 0, 0, 1, 1, 0, 1 };   //Arreglo
		cpu_predec_flush();
		mem_write32(P, itype(0x09, 6, 6, 1));                      /* addiu r6, r6, 1 */
		mem_write32(P + 4, (2u << 26) | (P >> 2));                 /* j P */
		mem_write32(P + 8, 0);
		memset(&cpu, 0, sizeof(cpu));
		cpu.pc = P;
		cpu.npc = P + 4;
		for(k = 0; k < 10; k++){   //Arreglo
			opt_on[OPT_PREDEC_SIN_UNIR] = su[k];
			opt_on[OPT_PREDEC_COMPACTO] = co[k];
			opt_on[OPT_PROF_CPU] = me[k];   //Arreglo
			cpu_run(30);                                           /* 10 vueltas */
			CHECK_EQ(cpu.r[6], 10u * (k + 1));
			CHECK(cpu_predec_tarjetas_ok());
		}
	}
	opt_on[OPT_PROF_CPU] = 0;   //Arreglo
	//Arreglo
	{
		const u32 P = 0x08A00A00u;
		PredecStats a, b;
		cpu_predec_flush();
		mem_write32(P, itype(0x09, 7, 7, 1));
		mem_write32(P + 4, itype(0x09, 8, 8, 1));
		mem_write32(P + 8, (2u << 26) | (P >> 2));
		mem_write32(P + 12, 0);
		mem_write32(P + 16, (2u << 26) | (P >> 2));
		mem_write32(P + 20, (2u << 26) | (P >> 2));
		memset(&cpu, 0, sizeof(cpu));
		cpu.pc = P;
		cpu.npc = P + 4;
		opt_on[OPT_PROF_BLOQUES] = 1;
		cpu_predec_stats(&a);
		cpu_run(42);
		cpu_predec_stats(&b);
		CHECK_EQ(cpu.r[7], 11u);
		CHECK_EQ(b.bq_bloques - a.bq_bloques, 10u);
		CHECK_EQ(b.bq_instr - a.bq_instr, 40u);
		CHECK_EQ(b.bq_tam[2] - a.bq_tam[2], 10u);
		CHECK_EQ(b.bq_enlace[1][0] - a.bq_enlace[1][0], 8u);
		CHECK_EQ(b.bq_enlace[1][1] - a.bq_enlace[1][1], 2u);
		CHECK_EQ(b.bq_caidas[CAIDA_NO_CABE] - a.bq_caidas[CAIDA_NO_CABE], 2u);
		CHECK_EQ(b.bq_caida_op[0x09] - a.bq_caida_op[0x09], 2u);
		cpu.pc = P + 16;
		cpu.npc = P + 20;
		cpu_predec_stats(&a);
		cpu_run(1);
		cpu_predec_stats(&b);
		CHECK_EQ(b.bq_caidas[CAIDA_SIN_TARJETA] - a.bq_caidas[CAIDA_SIN_TARJETA], 1u);
		CHECK_EQ(b.bq_caida_op[0x02] - a.bq_caida_op[0x02], 1u);
		cpu_run(1);
		cpu_predec_stats(&a);
		CHECK_EQ(a.bq_caidas[CAIDA_DELAY] - b.bq_caidas[CAIDA_DELAY], 1u);
		CHECK_EQ(cpu.pc, P);
		opt_on[OPT_PROF_BLOQUES] = 0;
		cpu.pc = P;
		cpu.npc = P + 4;
		cpu_predec_stats(&a);
		cpu_run(40);
		cpu_predec_stats(&b);
		CHECK_EQ(b.bq_bloques, a.bq_bloques);
		CHECK_EQ(b.bq_caidas[CAIDA_NO_CABE], a.bq_caidas[CAIDA_NO_CABE]);
	}
	opt_on[OPT_CPU_VFPU_VERIFICAR] = 0;   //Arreglo
	vfpu_reset(&vfpu);   //Arreglo
	cpu_stop_requested = 0;   //Arreglo
	//Arreglo
	{
		const u32 P = 0x08A01000u;
		u32 r5[2], r6[2], m;
		PredecStats a, b;
		u32 *d = (u32 *)mem_ptr(DATA, 32);
		for(m = 0; m < 2; m++){
			opt_on[OPT_CPU_VFPU_TARJETAS] = (u8)m;
			opt_on[OPT_PROF_CPU] = 1;
			cpu_predec_flush();
			wr_le32(&d[0], itype(0x09, 0, 5, 7));
			wr_le32(&d[1], itype(0x09, 0, 6, 9));
			wr_le32(&d[2], 0);
			wr_le32(&d[3], 0);
			mem_write32(P, itype(0x36, 28, 0, 0));
			mem_write32(P + 4, itype(0x3E, 30, 0, 16));
			mem_write32(P + 8, 0);
			mem_write32(P + 12, 0);
			mem_write32(P + 16, itype(0x09, 0, 5, 1));
			mem_write32(P + 20, itype(0x09, 0, 6, 2));
			mem_write32(P + 24, 0);
			mem_write32(P + 28, 0);
			mem_write32(P + 32, 0x1000FFFFu);
			mem_write32(P + 36, 0);
			memset(&cpu, 0, sizeof(cpu));
			cpu.r[28] = DATA;
			cpu.r[30] = P;
			cpu.pc = P;
			cpu.npc = P + 4;
			vfpu_live = 1;
			cpu_predec_stats(&a);
			cpu_run(12);
			cpu_predec_stats(&b);
			r5[m] = cpu.r[5];
			r6[m] = cpu.r[6];
			CHECK_EQ(b.vfpu_tarjetas - a.vfpu_tarjetas, m ? 2u : 0u);
		}
		CHECK_EQ(r5[1], 7u);
		CHECK_EQ(r6[1], 9u);
		for(m = 0; m < 2; m++){
			opt_on[OPT_CPU_VFPU_TARJETAS] = (u8)m;
			cpu_predec_flush();
			wr_le32(&d[4], itype(0x09, 0, 7, 5));
			mem_write32(P, itype(0x32, 28, 0, 16));
			mem_write32(P + 4, itype(0x3A, 30, 0, 12));
			mem_write32(P + 8, 0);
			mem_write32(P + 12, itype(0x09, 0, 7, 1));
			mem_write32(P + 16, 0x1000FFFFu);
			mem_write32(P + 20, 0);
			memset(&cpu, 0, sizeof(cpu));
			cpu.r[28] = DATA;
			cpu.r[30] = P;
			cpu.pc = P;
			cpu.npc = P + 4;
			cpu_run(6);
			CHECK_EQ(cpu.r[7], 5u);
		}
		CHECK_EQ(r5[0], r5[1]);
		CHECK_EQ(r6[0], r6[1]);
	}
	//Arreglo
	{
		const u32 P = 0x08A01100u;
		u32 res[2][4], m, i;
		PredecStats a, b;
		u32 *d = (u32 *)mem_ptr(DATA, 32);
		for(m = 0; m < 2; m++){
			opt_on[OPT_CPU_VFPU_TARJETAS] = (u8)m;
			opt_on[OPT_PROF_CPU] = 1;
			cpu_predec_flush();
			for(i = 0; i < 4; i++){ wr_le32(&d[i], 0x3F800000u + i); vfpu.vi[i] = 0x40000000u + i; vfpu.vi[16 + i] = 0x40400000u + i; }
			vfpu.ctrl[VFPU_CTRL_DPREFIX] = 0x500;
			mem_write32(P, itype(0x36, 28, 0, 0));
			mem_write32(P + 4, itype(0x36, 28, 4, 0));
			mem_write32(P + 8, 0x1000FFFFu);
			mem_write32(P + 12, 0);
			memset(&cpu, 0, sizeof(cpu));
			cpu.r[28] = DATA;
			cpu.pc = P;
			cpu.npc = P + 4;
			vfpu_live = 0;
			cpu_predec_stats(&a);
			cpu_run(4);
			cpu_predec_stats(&b);
			CHECK(vfpu_live);
			CHECK_EQ(b.vfpu - a.vfpu, 2u);
			CHECK_EQ(b.vfpu_tarjetas, a.vfpu_tarjetas);
			for(i = 0; i < 4; i++) res[m][i] = vfpu.vi[i];
			CHECK_EQ(vfpu.vi[16], 0x40400000u);
			CHECK_EQ(vfpu.vi[17], 0x3F800001u);
			CHECK_EQ(vfpu.vi[18], 0x40400002u);
			CHECK_EQ(vfpu.vi[19], 0x3F800003u);
			vfpu.ctrl[VFPU_CTRL_DPREFIX] = 0;
			cpu.pc = P;
			cpu.npc = P + 4;
			cpu_predec_stats(&a);
			cpu_run(4);
			cpu_predec_stats(&b);
			CHECK_EQ(b.vfpu - a.vfpu, m ? 0u : 2u);
			CHECK_EQ(b.vfpu_tarjetas - a.vfpu_tarjetas, m ? 2u : 0u);
			CHECK_EQ(vfpu.vi[16], 0x3F800000u);
			CHECK_EQ(vfpu.vi[18], 0x3F800002u);
			cpu.pc = P;
			cpu.npc = P + 4;
			vfpu_live = 0;
			cpu_predec_stats(&a);
			cpu_run(4);
			cpu_predec_stats(&b);
			CHECK(vfpu_live);
			CHECK_EQ(b.vfpu - a.vfpu, m ? 1u : 2u);
			CHECK_EQ(b.vfpu_tarjetas - a.vfpu_tarjetas, m ? 1u : 0u);
		}
		for(i = 0; i < 4; i++) CHECK_EQ(res[0][i], res[1][i]);
		CHECK_EQ(res[1][0], 0x40000000u);
		CHECK_EQ(res[1][1], 0x3F800001u);
	}
	opt_on[OPT_PROF_CPU] = 0;   //Arreglo
	/* bltzal ra: la condición usa el ra de antes de enlazar */
	{
		const u32 P = 0x08A00800u;
		mem_write32(P, itype(1, 31, 0x10, 2));                     /* bltzal r31, +2 */
		mem_write32(P + 4, 0);
		mem_write32(P + 8, itype(0x09, 0, 4, 1));                   /* addiu r4, r0, 1 */
		mem_write32(P + 12, itype(0x09, 0, 4, 2));                  /* addiu r4, r0, 2 */
		memset(&cpu, 0, sizeof(cpu));
		cpu.r[31] = 0x80000000u;
		cpu.pc = P;
		cpu.npc = P + 4;
		cpu_run(3);
		CHECK_EQ(cpu.r[4], 2u);
		CHECK_EQ(cpu.r[31], P + 8);
	}

	opt_on[OPT_CPU_SIN_EXTRA] = saved_opt;
	opt_on[OPT_CPU_PREDEC] = saved_predec;
	opt_on[OPT_PREDEC_COMPACTO] = saved_comp;   //arreglo
	opt_on[OPT_PREDEC_SIN_UNIR] = saved_su;   //arreglo
	opt_on[OPT_PROF_CPU] = saved_pc;   //Arreglo
	opt_on[OPT_PROF_BLOQUES] = saved_pb;   //Arreglo
	opt_on[OPT_CPU_VFPU_TARJETAS] = saved_vt;   //Arreglo
	opt_on[OPT_CPU_VFPU_VERIFICAR] = saved_vv;   //Arreglo
	hle_set_intr_enabled(saved_intr);
	vfpu_live = saved_live;
	cpu_stop_requested = 0;
	memset(mem_ptr(CODE, (NPROG + 80) * 4), 0, (NPROG + 80) * 4);
}
