/**
 * WIISP - tests/test_enlace.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo

#include <string.h>
#include "cpu/cpu.h"
#include "cpu/predec.h"
#include "core/memory.h"
#include "core/opt.h"
#include "test.h"

#define E_CODE    0x08A10000u
#define E_SIZE    0x4000u
#define E_STACK   0x08B20000u
#define E_NRUT    8
#define E_NCHUNK  32

static u32 es = 0x2468ACEu;
static u32 ernd(void){ es ^= es << 13; es ^= es >> 17; es ^= es << 5; return es; }
static u32 ern(u32 n){ return ernd() % n; }

static u32 r_(u32 rs, u32 rt, u32 rd, u32 sa, u32 fn){ return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn; }
static u32 i_(u32 op, u32 rs, u32 rt, u32 imm){ return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }

static u32 *ec;
static u32 slots[E_SIZE / 4], nslots;
static u8 slot_ent[E_SIZE / 4];
static int entrada;

static u32 acc(void){ return 2 + ern(11); }
static u32 alu(void){
	switch(ern(6)){
	case 0: case 1: { u32 a = acc(); return i_(0x09, a, a, ern(0x10000)); }
	case 2: return r_(acc(), acc(), acc(), 0, 0x21);
	case 3: return r_(acc(), acc(), acc(), 0, 0x26);
	case 4: { u32 a = acc(); return r_(0, a, a, 1 + ern(3), 0x00); }
	default: { u32 a = acc(); return i_(0x0D, a, a, ern(0x10000)); }
	}
}
static void put_alu(u32 o){ ec[o / 4] = alu(); slot_ent[nslots] = (u8)entrada; slots[nslots++] = o; }
static u32 store_code(void){
	const u32 rt = ern(5) ? 20 + ern(4) : 0;
	u32 k = ern(nslots), t;
	for(t = 0; t < 8 && ern(2) && !slot_ent[k]; t++) k = ern(nslots);
	return i_(0x2B, 30, rt, slots[k]);
}
static void put_delay(u32 o, int stores){
	const u32 k = ern(8);
	if(stores && k < 2 && nslots) ec[o / 4] = store_code();
	else if(k < 4) ec[o / 4] = 0;
	else put_alu(o);
}

static u32 rut_at[E_NRUT], rut_len[E_NRUT];

static u32 gen_rut(u32 i, u32 o){
	const int nest = i + 1 < E_NRUT && ern(3) == 0, frame = nest || ern(4) == 0;
	const u32 o0 = o;
	u32 k, n = 2 + ern(8);
	rut_at[i] = o;
	entrada = 1;
	if(frame){
		ec[o / 4] = i_(0x09, 29, 29, (u32)-8); o += 4;
		ec[o / 4] = i_(0x2B, 29, 31, 0); o += 4;
	}
	for(k = 0; k < n; k++, o += 4){
		if(nest && k == n / 2){
			const u32 j = i + 1 + ern(E_NRUT - i - 1);
			ec[o / 4] = 0x0C000000u | (j << 16) | 0xFFFF;
			entrada = 0;
			o += 4;
			put_delay(o, 1);
			continue;
		}
		if(nslots && ern(4) == 0) ec[o / 4] = store_code();
		else put_alu(o);
	}
	if(frame){
		ec[o / 4] = i_(0x23, 29, 31, 0); o += 4;
		ec[o / 4] = i_(0x09, 29, 29, 8); o += 4;
	}
	ec[o / 4] = r_(31, 0, 0, 0, 0x08); o += 4;
	entrada = 0;
	put_delay(o, 1); o += 4;
	rut_len[i] = o - o0;
	return o;
}

static void fix_calls(u32 from, u32 to){
	u32 o;
	for(o = from; o < to; o += 4){
		const u32 w = ec[o / 4];
		if((w & 0xFF00FFFFu) == 0x0C00FFFFu){
			const u32 j = (w >> 16) & 0xFF, dst = E_CODE + rut_at[j];
			ec[o / 4] = 0x0C000000u | ((dst >> 2) & 0x03FFFFFFu);
		}
	}
}

static void gen_prog(void){
	u32 o, i, iters = 3 + ern(10), loop, ncalls;
	memset(ec, 0, E_SIZE);
	nslots = 0;
	{
		u32 at = 0x200;
		for(i = 0; i < E_NRUT; i++){
			if(ern(3) == 0){
				const u32 z = ((at + 0x3FF) & ~0x3FFu) + 0x400 * ern(2);
				at = z - 4 * (1 + ern(4));
			} else at += 4 * ern(32);
			if(at + 0x80 >= E_SIZE) at = 0x200 + 4 * ern(64);
			{
				u32 j, ok;
				do {
					ok = 1;
					for(j = 0; j < i; j++)
						if(at < rut_at[j] + rut_len[j] + 8 && at + 0x80 > rut_at[j]) ok = 0;
					if(!ok) at = 0x200 + 4 * ern((E_SIZE - 0x300) / 4);
				} while(!ok);
			}
			rut_len[i] = 0x80;
			at = gen_rut(i, at);
		}
		for(i = 0; i < E_NRUT; i++) fix_calls(rut_at[i], rut_at[i] + rut_len[i]);
	}
	o = 0;
	entrada = 0;
	ec[o / 4] = i_(0x09, 0, 27, iters); o += 4;
	loop = o;
	ncalls = 2 + ern(5);
	for(i = 0; i < ncalls; i++){
		const u32 j = ern(E_NRUT), dst = E_CODE + rut_at[j];
		u32 k;
		for(k = ern(3); k; k--, o += 4){
			if(ern(3) == 0) ec[o / 4] = store_code();
			else put_alu(o);
		}
		if(ern(4) == 0){
			ec[o / 4] = i_(0x0F, 0, 25, dst >> 16); o += 4;
			ec[o / 4] = i_(0x0D, 25, 25, dst & 0xFFFF); o += 4;
			ec[o / 4] = r_(25, 0, 31, 0, 0x09); o += 4;
		} else {
			ec[o / 4] = 0x0C000000u | ((dst >> 2) & 0x03FFFFFFu); o += 4;
		}
		put_delay(o, 1); o += 4;
		entrada = 1;
		put_alu(o); o += 4;
		entrada = 0;
	}
	ec[o / 4] = i_(0x09, 27, 27, (u32)-1); o += 4;
	ec[o / 4] = i_(0x07, 27, 0, (u32)((s32)(loop - o - 4) >> 2)); o += 4;
	put_delay(o, 0); o += 4;
	ec[o / 4] = 0x1000FFFFu; o += 4;
	ec[o / 4] = 0;
	for(i = 0; i < E_SIZE / 4; i++) wr_le32(&ec[i], ec[i]);
	mem_note_write(E_CODE, E_SIZE);
}

typedef struct {
	CpuState cpu;
	u64 cycles, executed;
	int stop;
	u32 ret[E_NCHUNK];
	u8 code[E_SIZE], stack[0x1000];
} ESnap;

static void etake(ESnap *s){
	s->cpu = cpu;
	s->cycles = cpu_cycles;
	s->executed = cpu_executed;
	s->stop = cpu_stop_requested;
	memcpy(s->code, mem_ptr(E_CODE, E_SIZE), E_SIZE);
	memcpy(s->stack, mem_ptr(E_STACK, 0x1000), 0x1000);
}
static void eput(const ESnap *s){
	cpu = s->cpu;
	cpu_cycles = s->cycles;
	cpu_executed = s->executed;
	cpu_stop_requested = s->stop;
	memcpy(mem_ptr(E_CODE, E_SIZE), s->code, E_SIZE);
	memcpy(mem_ptr(E_STACK, 0x1000), s->stack, 0x1000);
	mem_note_write(E_CODE, E_SIZE);
}

static int esame(const ESnap *a, const ESnap *b, int prog, int *rep){
	int i, ok = 1;
	for(i = 0; i < 32; i++) if(a->cpu.r[i] != b->cpu.r[i]) ok = 0;
	if(a->cpu.pc != b->cpu.pc || a->cpu.npc != b->cpu.npc) ok = 0;
	if(a->cycles != b->cycles || a->executed != b->executed) ok = 0;
	if(memcmp(a->ret, b->ret, sizeof(a->ret)) || memcmp(a->code, b->code, E_SIZE) || memcmp(a->stack, b->stack, 0x1000)) ok = 0;
	if(!ok && *rep < 8){
		(*rep)++;
		printf("  programa de enlaces %d distinto (pc %08X/%08X, r2 %08X/%08X, ciclos %llu/%llu)\n", prog,
		       a->cpu.pc, b->cpu.pc, a->cpu.r[2], b->cpu.r[2], (unsigned long long)a->cycles, (unsigned long long)b->cycles);
	}
	return ok;
}

static void erun(ESnap *out, const u32 *chunks, int predec){
	int k;
	opt_on[OPT_CPU_SIN_EXTRA] = (u8)predec;
	opt_on[OPT_CPU_PREDEC] = (u8)predec;
	memset(out->ret, 0, sizeof(out->ret));
	for(k = 0; k < E_NCHUNK; k++){
		out->ret[k] = cpu_run(chunks[k]);
		if(cpu_stop_requested) break;
	}
	etake(out);
}

void test_enlace(void){
	static ESnap init, ra, rb;
	const u8 s_extra = opt_on[OPT_CPU_SIN_EXTRA], s_predec = opt_on[OPT_CPU_PREDEC], s_comp = opt_on[OPT_PREDEC_COMPACTO];
	const u8 s_enl = opt_on[OPT_CPU_ENLACE], s_ver = opt_on[OPT_CPU_VERIFICAR];
	int p, rep = 0, k;
	u64 total = 0;
	printf("enlaces entre bloques: bucles que vuelven a pasar por destinos reescritos\n");
	ec = (u32 *)mem_ptr(E_CODE, E_SIZE);
	for(p = 0; p < 3000; p++){
		u32 chunks[E_NCHUNK], i;
		gen_prog();
		memset(&cpu, 0, sizeof(cpu));
		for(i = 2; i <= 12; i++) cpu.r[i] = ernd();
		for(i = 20; i <= 23; i++){ const u32 a = acc(); cpu.r[i] = i_(0x09, a, a, ern(0x10000)); }
		cpu.r[29] = E_STACK + 0x800;
		cpu.r[30] = E_CODE;
		cpu.pc = E_CODE;
		cpu.npc = E_CODE + 4;
		cpu_cycles = ernd();
		cpu_executed = 0;
		cpu_stop_requested = 0;
		for(k = 0; k < E_NCHUNK; k++) chunks[k] = ern(3) ? 1 + ern(12) : 1 + ern(200);
		etake(&init);
		erun(&ra, chunks, 0);
		total += ra.executed;
		for(k = 0; k < 8; k++){
			cpu_predec_tam_pruebas(k & 1 ? 4096 : 0, k & 1 ? 2048 : 0);
			opt_on[OPT_PREDEC_COMPACTO] = (u8)((k >> 1) & 1);
			opt_on[OPT_CPU_ENLACE] = (u8)(k < 4 || (p & 1));
			opt_on[OPT_CPU_VERIFICAR] = (u8)(k == 7 && (p & 6) == 0);
			eput(&init);
			erun(&rb, chunks, 1);
			CHECK(esame(&ra, &rb, p, &rep));
			if(!cpu_predec_enlaces_ok() && rep < 8){ rep++; printf("  programa de enlaces %d: enlace a otro pc\n", p); CHECK(0); }
		}
	}
	cpu_predec_tam_pruebas(0, 0);
	printf("  %d programas, %llu instrucciones comparadas\n", p, (unsigned long long)total);
	opt_on[OPT_CPU_SIN_EXTRA] = s_extra;
	opt_on[OPT_CPU_PREDEC] = s_predec;
	opt_on[OPT_PREDEC_COMPACTO] = s_comp;
	opt_on[OPT_CPU_ENLACE] = s_enl;
	opt_on[OPT_CPU_VERIFICAR] = s_ver;
	cpu_stop_requested = 0;
	memset(mem_ptr(E_CODE, E_SIZE), 0, E_SIZE);
	mem_note_write(E_CODE, E_SIZE);
}
