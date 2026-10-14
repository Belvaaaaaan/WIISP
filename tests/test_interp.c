/**
 * WIISP - tests/test_interp.c
 * El intérprete (cpu_run con cpu_sin_extra=1) contra el de referencia
 * (cpu_run_ref): programas aleatorios con todas las familias de
 * instrucciones (ALU, saltos con y sin likely, enlaces, FPU con el riesgo
 * de bc1x, cargas y almacenamientos en la RAM y sus espejos, el scratchpad
 * y la VRAM, ll/sc, cache, VFPU, mfic/mtic) y programas con fallos
 * (instrucciones y direcciones inválidas). Cada programa se ejecuta en
 * trozos de tamaño aleatorio, los mismos para los dos, y tienen que acabar
 * en el mismo estado exacto: registros, memoria, pc, npc, ciclos,
 * instrucciones ejecutadas, lo que devuelve cada llamada y la petición de
 * parar.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu/cpu.h"
#include "cpu/vfpu.h"
#include "core/memory.h"
#include "core/opt.h"
#include "test.h"

#define CODE      0x08A00000u
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
		code[i] = itype(rn(2) ? 0x32 : 0x3A, mbase(), vt & 31, moff(4) | (vt >> 5));
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
	u64 cycles, executed;
	int stop, intr;
	u32 ret[NCHUNKS];
	u8 data[0x2000], scratch[PSP_SCRATCH_SIZE], vram[VRAM_SNAP];
} Snap;

static void take(Snap *s){
	s->cpu = cpu;
	s->vfpu = vfpu;
	s->cycles = cpu_cycles;
	s->executed = cpu_executed;
	s->stop = cpu_stop_requested;
	s->intr = hle_get_intr_enabled();
	memcpy(s->data, mem_ptr(DATA, sizeof(s->data)), sizeof(s->data));
	memcpy(s->scratch, psp_mem.scratch, sizeof(s->scratch));
	memcpy(s->vram, psp_mem.vram, sizeof(s->vram));
}

static void put(const Snap *s){
	cpu = s->cpu;
	vfpu = s->vfpu;
	cpu_cycles = s->cycles;
	cpu_executed = s->executed;
	hle_set_intr_enabled(s->intr);
	cpu_stop_requested = s->stop;
	memcpy(mem_ptr(DATA, sizeof(s->data)), s->data, sizeof(s->data));
	memcpy(psp_mem.scratch, s->scratch, sizeof(s->scratch));
	memcpy(psp_mem.vram, s->vram, sizeof(s->vram));
}

static void run_chunks(Snap *out, const u32 *chunks, int ref){
	int k;
	opt_on[OPT_CPU_SIN_EXTRA] = (u8)!ref;
	memset(out->ret, 0, sizeof(out->ret));
	vfpu_live = 1;
	for(k = 0; k < NCHUNKS; k++){
		out->ret[k] = cpu_run(chunks[k]);
		if(cpu_stop_requested) break;
	}
	take(out);
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
	DIF(a->cycles != b->cycles, "ciclos %llu != %llu", (unsigned long long)a->cycles, (unsigned long long)b->cycles);
	DIF(a->executed != b->executed, "ejecutadas %llu != %llu", (unsigned long long)a->executed, (unsigned long long)b->executed);
	DIF(a->stop != b->stop || a->intr != b->intr, "parar %d/%d, interrupciones %d/%d", a->stop, b->stop, a->intr, b->intr);
	DIF(memcmp(a->ret, b->ret, sizeof(a->ret)) != 0, "lo que devuelve cpu_run");
	DIF(memcmp(a->data, b->data, sizeof(a->data)) != 0, "RAM");
	DIF(memcmp(a->scratch, b->scratch, sizeof(a->scratch)) != 0, "scratchpad");
	DIF(memcmp(a->vram, b->vram, sizeof(a->vram)) != 0, "VRAM");
#undef DIF
	return ok;
}

static void one_program(int prog, int faults, Snap *init, Snap *ra, Snap *rb, u64 *total){
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

	take(init);
	run_chunks(ra, chunks, 1);
	put(init);
	run_chunks(rb, chunks, 0);
	*total += ra->executed - init->executed;
	CHECK(same(ra, rb, prog));
}

void test_interp(void){
	static Snap init, ra, rb;
	const u8 saved_opt = opt_on[OPT_CPU_SIN_EXTRA];
	const int saved_intr = hle_get_intr_enabled(), saved_live = vfpu_live;
	u64 total = 0;
	int p, err_fd;
	printf("interprete contra el de referencia\n");

	for(p = 0; p < 3000; p++) one_program(p, 0, &init, &ra, &rb, &total);
	/* Con fallos: cpu_fault avisa por stderr, que aquí no interesa */
	fflush(stderr);
	err_fd = dup(2);
	if(err_fd >= 0){
		int null_fd = open("/dev/null", O_WRONLY);
		if(null_fd >= 0){ dup2(null_fd, 2); close(null_fd); }
	}
	for(; p < 3400; p++) one_program(p, 1, &init, &ra, &rb, &total);
	if(err_fd >= 0){
		fflush(stderr);
		dup2(err_fd, 2);
		close(err_fd);
	}
	printf("  %d programas, %llu instrucciones comparadas\n", p, (unsigned long long)total);

	opt_on[OPT_CPU_SIN_EXTRA] = saved_opt;
	hle_set_intr_enabled(saved_intr);
	vfpu_live = saved_live;
	cpu_stop_requested = 0;
	memset(mem_ptr(CODE, (NPROG + 80) * 4), 0, (NPROG + 80) * 4);
}
