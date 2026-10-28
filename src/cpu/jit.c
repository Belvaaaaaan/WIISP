/**
 * WIISP - cpu/jit.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/jit.h"
#include "cpu/cpu.h"
#include "cpu/ppc_emit.h"
#include "cpu/predec.h"
#include "core/memory.h"
#include "hle/hle.h"
#if JIT_PPC && !defined(GEKKO)
#include <sys/mman.h>
#endif
#if defined(GEKKO)
#include <malloc.h>
#endif

//Arreglo
#define JIT_ARENA (4u << 20)
#define JIT_MAX_N 64
#define NCACHE 5

//Arreglo
enum { P_CTX = 3, P_RAM = 4, P_MAPA = 5, P_T0 = 0, P_T1 = 11, P_T2 = 12, P_LOG = 31 };
static const u8 cache_ppc[NCACHE] = { 6, 7, 8, 9, 10 };

//Arreglo
enum {
	J_NO, J_NOP, J_SLL, J_SRL, J_ROTR, J_SRA, J_SLLV, J_SRLV, J_ROTRV, J_SRAV, J_MOVZ, J_MOVN,
	J_MFHI, J_MTHI, J_MFLO, J_MTLO, J_CLZ, J_CLO, J_MULT, J_MULTU, J_MADD, J_MADDU, J_MSUB, J_MSUBU,
	J_ADDU, J_SUBU, J_AND, J_OR, J_XOR, J_NOR, J_SLT, J_SLTU, J_MAX, J_MIN,
	J_ADDIU, J_SLTI, J_SLTIU, J_ANDI, J_ORI, J_XORI, J_LUI, J_EXT, J_INS, J_SEB, J_SEH,
	J_LB, J_LH, J_LW, J_LBU, J_LHU, J_SB, J_SH, J_SW,
	J_BEQ, J_BNE, J_BLEZ, J_BGTZ, J_BLTZ, J_BGEZ, J_J, J_JAL, J_JR, J_JALR
};

//Arreglo
#define HI 32
#define LO 33
#define BIT(r) (1ull << (r))

//Arreglo
typedef struct {
	u8 op, d, s, t, sa, tam;
	u32 imm, destino;
	u64 lee, esc;
} Ins;

//Arreglo
static void analizar(u32 w, u32 ipc, Ins *x){
	const u32 rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
	const u32 imm = w & 0xFFFF, simm = (u32)(s32)(s16)imm;
	memset(x, 0, sizeof(*x));
	x->op = J_NO;
	switch(w >> 26){
	case 0x00:
		switch(fn){
		case 0x00: if(!rd){ x->op = J_NOP; return; } x->op = J_SLL; x->d = (u8)rd; x->t = (u8)rt; x->sa = (u8)sa; break;
		case 0x02:
			if(!rd){ x->op = J_NOP; return; }
			x->d = (u8)rd; x->t = (u8)rt;
			if((rs & 1) && sa){ x->op = J_ROTR; x->sa = (u8)sa; }
			else { x->op = J_SRL; x->sa = (u8)((rs & 1) ? 0 : sa); }
			break;
		case 0x03: if(!rd){ x->op = J_NOP; return; } x->op = J_SRA; x->d = (u8)rd; x->t = (u8)rt; x->sa = (u8)sa; break;
		case 0x04: if(!rd){ x->op = J_NOP; return; } x->op = J_SLLV; x->d = (u8)rd; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x06: if(!rd){ x->op = J_NOP; return; } x->op = (sa & 1) ? J_ROTRV : J_SRLV; x->d = (u8)rd; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x07: if(!rd){ x->op = J_NOP; return; } x->op = J_SRAV; x->d = (u8)rd; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x08: x->op = J_JR; x->s = (u8)rs; break;
		case 0x09: x->op = rd ? J_JALR : J_JR; x->d = (u8)rd; x->s = (u8)rs; break;
		case 0x0A: if(!rd){ x->op = J_NOP; return; } x->op = J_MOVZ; x->d = (u8)rd; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x0B: if(!rd){ x->op = J_NOP; return; } x->op = J_MOVN; x->d = (u8)rd; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x0F: x->op = J_NOP; return;
		case 0x10: if(!rd){ x->op = J_NOP; return; } x->op = J_MFHI; x->d = (u8)rd; break;
		case 0x11: x->op = J_MTHI; x->s = (u8)rs; break;
		case 0x12: if(!rd){ x->op = J_NOP; return; } x->op = J_MFLO; x->d = (u8)rd; break;
		case 0x13: x->op = J_MTLO; x->s = (u8)rs; break;
		case 0x16: if(!rd){ x->op = J_NOP; return; } x->op = J_CLZ; x->d = (u8)rd; x->s = (u8)rs; break;
		case 0x17: if(!rd){ x->op = J_NOP; return; } x->op = J_CLO; x->d = (u8)rd; x->s = (u8)rs; break;
		case 0x18: x->op = J_MULT; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x19: x->op = J_MULTU; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x1C: x->op = J_MADD; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x1D: x->op = J_MADDU; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x2E: x->op = J_MSUB; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x2F: x->op = J_MSUBU; x->s = (u8)rs; x->t = (u8)rt; break;
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
		case 0x2A: case 0x2B: case 0x2C: case 0x2D: {
			static const u8 ops[14] = { J_ADDU, J_ADDU, J_SUBU, J_SUBU, J_AND, J_OR, J_XOR, J_NOR, J_NO, J_NO, J_SLT, J_SLTU, J_MAX, J_MIN };
			if(!rd){ x->op = J_NOP; return; }
			x->op = ops[fn - 0x20]; x->d = (u8)rd; x->s = (u8)rs; x->t = (u8)rt;
			break;
		}
		default: return;
		}
		break;
	case 0x01:
		if(rt == 0x00){ x->op = J_BLTZ; x->s = (u8)rs; x->destino = ipc + 4 + (simm << 2); }
		else if(rt == 0x01){ x->op = J_BGEZ; x->s = (u8)rs; x->destino = ipc + 4 + (simm << 2); }
		else return;
		break;
	case 0x02: case 0x03:
		x->op = (w >> 26) == 0x02 ? J_J : J_JAL;
		x->destino = (ipc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
		break;
	case 0x04: x->op = J_BEQ; x->s = (u8)rs; x->t = (u8)rt; x->destino = ipc + 4 + (simm << 2); break;
	case 0x05: x->op = J_BNE; x->s = (u8)rs; x->t = (u8)rt; x->destino = ipc + 4 + (simm << 2); break;
	case 0x06: x->op = J_BLEZ; x->s = (u8)rs; x->destino = ipc + 4 + (simm << 2); break;
	case 0x07: x->op = J_BGTZ; x->s = (u8)rs; x->destino = ipc + 4 + (simm << 2); break;
	case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
		static const u8 ops[8] = { J_ADDIU, J_ADDIU, J_SLTI, J_SLTIU, J_ANDI, J_ORI, J_XORI, J_LUI };
		if(!rt){ x->op = J_NOP; return; }
		x->op = ops[(w >> 26) - 0x08]; x->d = (u8)rt; x->s = (u8)rs;
		x->imm = (x->op == J_ANDI || x->op == J_ORI || x->op == J_XORI || x->op == J_LUI) ? imm : simm;
		if(x->op == J_LUI) x->s = 0;
		break;
	}
	case 0x10: x->op = J_NOP; return;
	case 0x1C: if(fn == 0x24 || fn == 0x26) return; x->op = J_NOP; return;
	case 0x1F:
		if(fn == 0x00){ if(!rt){ x->op = J_NOP; return; } x->op = J_EXT; x->d = (u8)rt; x->s = (u8)rs; x->sa = (u8)sa; x->tam = (u8)(rd + 1); break; }
		if(fn == 0x04){
			u32 size = rd + 1 - sa;
			if(!rt){ x->op = J_NOP; return; }
			x->op = J_INS; x->d = (u8)rt; x->s = (u8)rs; x->sa = (u8)sa; x->tam = (u8)(size >= 32 ? 32 : size);
			break;
		}
		if(fn == 0x20){
			if(!rd){ if(sa == 0x02 || sa == 0x03 || sa == 0x10 || sa == 0x14 || sa == 0x18) x->op = J_NOP; return; }
			if(sa == 0x10){ x->op = J_SEB; x->d = (u8)rd; x->t = (u8)rt; break; }
			if(sa == 0x18){ x->op = J_SEH; x->d = (u8)rd; x->t = (u8)rt; break; }
		}
		return;
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: {
		static const u8 ops[6] = { J_LB, J_LH, J_NO, J_LW, J_LBU, J_LHU };
		if(!rt) return;
		x->op = ops[(w >> 26) - 0x20]; x->d = (u8)rt; x->s = (u8)rs; x->imm = simm;
		break;
	}
	case 0x28: x->op = J_SB; x->s = (u8)rs; x->t = (u8)rt; x->imm = simm; break;
	case 0x29: x->op = J_SH; x->s = (u8)rs; x->t = (u8)rt; x->imm = simm; break;
	case 0x2B: x->op = J_SW; x->s = (u8)rs; x->t = (u8)rt; x->imm = simm; break;
	case 0x2F: if(rt == 0x1A || rt == 0x1B) return; x->op = J_NOP; return;
	default: return;
	}
	switch(x->op){
	case J_SLL: case J_SRL: case J_ROTR: case J_SRA: case J_SEB: case J_SEH:
		x->lee = BIT(x->t); x->esc = BIT(x->d); break;
	case J_MOVZ: case J_MOVN:
		x->lee = BIT(x->s) | BIT(x->t) | BIT(x->d); x->esc = BIT(x->d); break;
	case J_MFHI: x->lee = BIT(HI); x->esc = BIT(x->d); break;
	case J_MFLO: x->lee = BIT(LO); x->esc = BIT(x->d); break;
	case J_MTHI: x->lee = BIT(x->s); x->esc = BIT(HI); break;
	case J_MTLO: x->lee = BIT(x->s); x->esc = BIT(LO); break;
	case J_MULT: case J_MULTU: x->lee = BIT(x->s) | BIT(x->t); x->esc = BIT(HI) | BIT(LO); break;
	case J_MADD: case J_MADDU: case J_MSUB: case J_MSUBU:
		x->lee = BIT(x->s) | BIT(x->t) | BIT(HI) | BIT(LO); x->esc = BIT(HI) | BIT(LO); break;
	case J_CLZ: case J_CLO: case J_ADDIU: case J_SLTI: case J_SLTIU: case J_ANDI: case J_ORI: case J_XORI: case J_EXT:
	case J_LB: case J_LH: case J_LW: case J_LBU: case J_LHU:
		x->lee = BIT(x->s); x->esc = BIT(x->d); break;
	case J_LUI: x->esc = BIT(x->d); break;
	case J_INS: x->lee = BIT(x->s) | BIT(x->d); x->esc = BIT(x->d); break;
	case J_SB: case J_SH: case J_SW: case J_BEQ: case J_BNE: x->lee = BIT(x->s) | BIT(x->t); break;
	case J_BLEZ: case J_BGTZ: case J_BLTZ: case J_BGEZ: case J_JR: x->lee = BIT(x->s); break;
	case J_JAL: x->esc = BIT(31); break;
	case J_JALR: x->lee = BIT(x->s); x->esc = BIT(x->d); break;
	case J_J: break;
	default: x->lee = BIT(x->s) | BIT(x->t); x->esc = BIT(x->d); break;
	}
	x->lee &= ~1ull;
	x->esc &= ~1ull;
}

//Arreglo
void jit_uso_regs(u32 w, u64 *lee, u64 *esc){
	const u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fn = w & 63;
	Ins x;
	analizar(w, 0, &x);
	*lee = x.lee;
	*esc = x.esc;
	if(x.op != J_NO) return;
	switch(op){
	case 0x00:
		if(fn == 0x1A || fn == 0x1B){ *lee = BIT(rs) | BIT(rt); *esc = BIT(HI) | BIT(LO); }
		break;
	case 0x01:
		*lee = BIT(rs);
		if(rt & 0x10) *esc = BIT(31);
		break;
	case 0x14: case 0x15: *lee = BIT(rs) | BIT(rt); break;
	case 0x16: case 0x17: *lee = BIT(rs); break;
	case 0x11:
		if(rs == 0x00 || rs == 0x02) *esc = BIT(rt);
		else if(rs == 0x04 || rs == 0x06) *lee = BIT(rt);
		break;
	case 0x12:
		if(rs == 3) *esc = BIT(rt);
		else if(rs == 7) *lee = BIT(rt);
		break;
	case 0x1C:
		if(fn == 0x24) *esc = BIT(rt);
		else if(fn == 0x26) *lee = BIT(rt);
		break;
	case 0x1F:
		if(fn == 0x20){ *lee = BIT(rt); *esc = BIT(rd); }
		break;
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x30: *lee = BIT(rs); *esc = BIT(rt); break;
	case 0x22: case 0x26: *lee = BIT(rs) | BIT(rt); *esc = BIT(rt); break;
	case 0x2A: case 0x2E: *lee = BIT(rs) | BIT(rt); break;
	case 0x38: *lee = BIT(rs) | BIT(rt); *esc = BIT(rt); break;
	case 0x2F: case 0x31: case 0x39: case 0x32: case 0x3A: case 0x36: case 0x3E: case 0x35: case 0x3D: *lee = BIT(rs); break;
	}
	*lee &= ~1ull;
	*esc &= ~1ull;
}

//Arreglo
static int es_salto(int op){ return op >= J_BEQ; }

//Arreglo
u32 jit_prefijo(const u32 *raw, u32 n, int branch){
	u32 k;
	Ins x;
	if(n > JIT_MAX_N) n = JIT_MAX_N;
	for(k = 0; k < n; k++){
		analizar(raw[k], 0, &x);
		if(x.op == J_NO) break;
		if(es_salto(x.op) != (branch && k == n - 2)) break;
	}
	return k;
}

//Arreglo
static u8 *arena, *reserva;
#if JIT_PPC
static u8 *arena_propia;
static int pedida;
#endif
static u32 usado, tam = JIT_ARENA, reserva_tam, tam_hecho;
static int lleno;
static JitStats st;
int jit_prueba_error;
void *(*jit_pedir_arena)(u32 bytes);

//Arreglo
void jit_reserva(void *p, u32 bytes){ reserva = (u8 *)p; reserva_tam = bytes; }
void jit_tam_pruebas(u32 bytes){ tam = bytes ? bytes : JIT_ARENA; }
int jit_lleno(void){ return lleno; }
void jit_stats(JitStats *out){ *out = st; }
const void *jit_arena_dir(void){ return arena; }
u32 jit_arena_usada(void){ return usado; }
u32 jit_arena_total(void){ return arena ? tam_hecho : 0; }

//Arreglo
void jit_vaciar(void){
	if(usado) st.vaciados++;
	usado = 0;
	lleno = 0;
}

//Arreglo
static int arena_lista(void){
	if(arena && tam_hecho == tam) return 1;
#if JIT_PPC
	if(!reserva && jit_pedir_arena && !pedida){
		pedida = 1;
		reserva = (u8 *)jit_pedir_arena(JIT_ARENA);
		reserva_tam = reserva ? JIT_ARENA : 0;
	}
	if(reserva && reserva_tam >= tam){ arena = reserva; tam_hecho = tam; usado = 0; return 1; }
#if defined(GEKKO)
	(void)arena_propia;
	return 0;
#else
	if(arena_propia) munmap(arena_propia, tam_hecho);
	arena_propia = mmap(NULL, tam, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(arena_propia == MAP_FAILED) arena_propia = NULL;
	arena = arena_propia;
	tam_hecho = arena ? tam : 0;
	usado = 0;
	return arena != NULL;
#endif
#else
	return 0;
#endif
}

//Arreglo
void *jit_arena_lista(u32 *bytes){
	const int ok = arena_lista();
	*bytes = ok ? tam_hecho : 0;
	return ok ? arena : NULL;
}

//Arreglo
void jit_publicar(void *p, u32 bytes){
#if JIT_PPC
	u8 *a = (u8 *)((uintptr_t)p & ~(uintptr_t)31), *e = (u8 *)p + bytes, *q;
	for(q = a; q < e; q += 32) __asm__ __volatile__("dcbst 0,%0" :: "r"(q) : "memory");
	__asm__ __volatile__("sync" ::: "memory");
	for(q = a; q < e; q += 32) __asm__ __volatile__("icbi 0,%0" :: "r"(q) : "memory");
	__asm__ __volatile__("sync\n\tisync" ::: "memory");
#else
	(void)p; (void)bytes;
#endif
}

//Arreglo
static u32 jit_log[JIT_MAX_N * 3];
static u32 *jit_log_pos;

//Arreglo
typedef struct {
	Emit e;
	s8 en[34];
	u8 sucio;
	u8 verificar;
	u8 hb;
	struct { u32 *w; u8 k, sucio; } sal[2 * JIT_MAX_N + 2];
	int nsal;
} Comp;

//Arreglo
static inline s32 off_reg(int r){ return r == HI ? 128 : r == LO ? 132 : 4 * r; }
#define OFF_PC 136
typedef char jit_off_ok[offsetof(CpuState, hi) == 128 && offsetof(CpuState, lo) == 132 && offsetof(CpuState, pc) == OFF_PC ? 1 : -1];

//Arreglo
static int lee(Comp *c, int r, int tmp){
	if(r == 0){ e_li(&c->e, tmp, 0); return tmp; }
	if(c->en[r] >= 0) return cache_ppc[(int)c->en[r]];
	e_lwz(&c->e, tmp, off_reg(r), P_CTX);
	return tmp;
}

//Arreglo
static int dst(Comp *c, int r){ return c->en[r] >= 0 ? cache_ppc[(int)c->en[r]] : P_T0; }

//Arreglo
static void fin(Comp *c, int r, int reg){
	if(c->en[r] >= 0){
		const int cr = cache_ppc[(int)c->en[r]];
		if(reg != cr) e_mr(&c->e, cr, reg);
		c->sucio |= (u8)(1u << c->en[r]);
	} else e_stw(&c->e, reg, off_reg(r), P_CTX);
}

//Arreglo
static void salida(Comp *c, int bo, int bi, u32 k){
	c->sal[c->nsal].w = e_bc_hueco(&c->e, bo, bi);
	c->sal[c->nsal].k = (u8)k;
	c->sal[c->nsal].sucio = c->sucio;
	c->nsal++;
}

//Arreglo
static void ha_lo(u32 a, s32 *ha, s32 *lo){ *ha = (s32)((a + 0x8000u) >> 16); *lo = (s32)(s16)(a & 0xFFFFu); }

//Arreglo
static void salir(Comp *c, u8 sucio, u32 valor){
	int r;
	for(r = 1; r < 34; r++)
		if(c->en[r] >= 0 && ((sucio >> c->en[r]) & 1)) e_stw(&c->e, cache_ppc[(int)c->en[r]], off_reg(r), P_CTX);
	if(c->verificar){
		s32 ha, lo;
		ha_lo((u32)(uintptr_t)&jit_log_pos, &ha, &lo);
		e_lis(&c->e, P_T2, ha);
		e_stw(&c->e, P_LOG, lo, P_T2);
		e_lwz(&c->e, P_LOG, 8, 1);
		e_addi(&c->e, 1, 1, 16);
	}
	e_li(&c->e, 3, (s32)valor);
	e_blr(&c->e);
}

//Arreglo
static void direccion(Comp *c, const Ins *x, u32 k, int t){
	const int b = lee(c, x->s, P_T1);
	if(x->imm){
		e_addi(&c->e, P_T2, b, (s32)x->imm);
		e_addis(&c->e, P_T2, P_T2, -0x0800);
	} else e_addis(&c->e, P_T2, b, -0x0800);
	e_rlwinm(&c->e, P_T2, P_T2, 0, 2, 31);
	if(t == 4) e_rlwinm_(&c->e, P_T0, P_T2, 30, 0, c->hb + 1);
	else if(t == 2) e_rlwinm_(&c->e, P_T0, P_T2, 31, 0, c->hb);
	else e_rlwinm_(&c->e, P_T0, P_T2, 0, 0, c->hb - 1);
	salida(c, BO_FALSO, CR_EQ, k);
}

//Arreglo
static void guardar(Comp *c, const Ins *x, u32 k, int t){
	int v;
	direccion(c, x, k, t);
	e_rlwinm(&c->e, P_T0, P_T2, 32 - 10, 10, 31);
	e_lbzx(&c->e, P_T0, P_MAPA, P_T0);
	e_cmpwi(&c->e, 0, P_T0, 0);
	salida(c, BO_FALSO, CR_EQ, k);
	v = lee(c, x->t, P_T1);
	if(c->verificar){
		if(t == 4) e_lwbrx(&c->e, P_T0, P_RAM, P_T2);
		else if(t == 2) e_lhbrx(&c->e, P_T0, P_RAM, P_T2);
		else e_lbzx(&c->e, P_T0, P_RAM, P_T2);
		e_stw(&c->e, P_T2, 0, P_LOG);
		e_stw(&c->e, P_T0, 4, P_LOG);
		e_li(&c->e, P_T0, t);
		e_stw(&c->e, P_T0, 8, P_LOG);
		e_addi(&c->e, P_LOG, P_LOG, 12);
	}
	if(t == 4) e_stwbrx(&c->e, v, P_RAM, P_T2);
	else if(t == 2) e_sthbrx(&c->e, v, P_RAM, P_T2);
	else e_stbx(&c->e, v, P_RAM, P_T2);
}

//Arreglo
static void slt(Comp *c, int o, int a, int b, int con_signo){
	if(con_signo){
		e_xoris(&c->e, P_T0, a, 0x8000);
		e_xoris(&c->e, P_T2, b, 0x8000);
		e_subfc(&c->e, P_T1, P_T2, P_T0);
		e_subfe(&c->e, o, P_T1, P_T1);
	} else {
		e_subfc(&c->e, P_T0, b, a);
		e_subfe(&c->e, o, P_T0, P_T0);
	}
	e_neg(&c->e, o, o);
}

//Arreglo
static void salto_condicional(Comp *c, int bo, int bi, u32 fin_bloque, u32 destino){
	u32 *h;
	e_carga32(&c->e, P_T0, fin_bloque);
	h = e_bc_hueco(&c->e, bo, bi);
	e_carga32(&c->e, P_T0, destino);
	e_parchea_bc(h, c->e.p);
	e_stw(&c->e, P_T0, OFF_PC, P_CTX);
}

//Arreglo
static void traducir(Comp *c, const Ins *x, u32 k, u32 fin_bloque){
	int a, b, o;
	switch(x->op){
	case J_NOP: break;
	case J_SLL: case J_SRL: case J_ROTR: case J_SRA: case J_SEB: case J_SEH:
		b = lee(c, x->t, P_T2);
		o = dst(c, x->d);
		if(x->op == J_SEB) e_extsb(&c->e, o, b);
		else if(x->op == J_SEH) e_extsh(&c->e, o, b);
		else if(x->op == J_SRA) e_srawi(&c->e, o, b, x->sa);
		else if(!x->sa) e_mr(&c->e, o, b);
		else if(x->op == J_SLL) e_rlwinm(&c->e, o, b, x->sa, 0, 31 - x->sa);
		else if(x->op == J_SRL) e_rlwinm(&c->e, o, b, 32 - x->sa, x->sa, 31);
		else e_rlwinm(&c->e, o, b, 32 - x->sa, 0, 31);
		fin(c, x->d, o);
		break;
	case J_SLLV: case J_SRLV: case J_SRAV: case J_ROTRV:
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		o = dst(c, x->d);
		if(x->op == J_ROTRV){
			e_neg(&c->e, P_T1, a);
			e_rlwnm(&c->e, o, b, P_T1, 0, 31);
		} else {
			e_rlwinm(&c->e, P_T1, a, 0, 27, 31);
			if(x->op == J_SLLV) e_slw(&c->e, o, b, P_T1);
			else if(x->op == J_SRLV) e_srw(&c->e, o, b, P_T1);
			else e_sraw(&c->e, o, b, P_T1);
		}
		fin(c, x->d, o);
		break;
	case J_MOVZ: case J_MOVN: {
		u32 *h;
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		e_cmpwi(&c->e, 0, b, 0);
		h = e_bc_hueco(&c->e, x->op == J_MOVZ ? BO_FALSO : BO_CIERTO, CR_EQ);
		fin(c, x->d, a);
		e_parchea_bc(h, c->e.p);
		break;
	}
	case J_MFHI: fin(c, x->d, lee(c, HI, P_T1)); break;
	case J_MFLO: fin(c, x->d, lee(c, LO, P_T1)); break;
	case J_MTHI: fin(c, HI, lee(c, x->s, P_T1)); break;
	case J_MTLO: fin(c, LO, lee(c, x->s, P_T1)); break;
	case J_CLZ: case J_CLO:
		a = lee(c, x->s, P_T1);
		o = dst(c, x->d);
		if(x->op == J_CLO){ e_nor(&c->e, P_T1, a, a); a = P_T1; }
		e_cntlzw(&c->e, o, a);
		fin(c, x->d, o);
		break;
	case J_MULT: case J_MULTU:
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		e_mullw(&c->e, P_T0, a, b);
		fin(c, LO, P_T0);
		if(x->op == J_MULT) e_mulhw(&c->e, P_T0, a, b);
		else e_mulhwu(&c->e, P_T0, a, b);
		fin(c, HI, P_T0);
		break;
	case J_MADD: case J_MADDU: case J_MSUB: case J_MSUBU: {
		int l, h;
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		e_mullw(&c->e, P_T0, a, b);
		if(x->op == J_MADD || x->op == J_MSUB) e_mulhw(&c->e, P_T1, a, b);
		else e_mulhwu(&c->e, P_T1, a, b);
		l = lee(c, LO, P_T2);
		if(x->op == J_MADD || x->op == J_MADDU) e_addc(&c->e, P_T0, l, P_T0);
		else e_subfc(&c->e, P_T0, P_T0, l);
		fin(c, LO, P_T0);
		h = lee(c, HI, P_T2);
		if(x->op == J_MADD || x->op == J_MADDU) e_adde(&c->e, P_T1, h, P_T1);
		else e_subfe(&c->e, P_T1, P_T1, h);
		fin(c, HI, P_T1);
		break;
	}
	case J_ADDU: case J_SUBU: case J_AND: case J_OR: case J_XOR: case J_NOR:
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		o = dst(c, x->d);
		if(x->op == J_ADDU){ if(jit_prueba_error) e_subf(&c->e, o, b, a); else e_add(&c->e, o, a, b); }
		else if(x->op == J_SUBU) e_subf(&c->e, o, b, a);
		else if(x->op == J_AND) e_and(&c->e, o, a, b);
		else if(x->op == J_OR) e_or(&c->e, o, a, b);
		else if(x->op == J_XOR) e_xor(&c->e, o, a, b);
		else e_nor(&c->e, o, a, b);
		fin(c, x->d, o);
		break;
	case J_SLT: case J_SLTU:
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		o = dst(c, x->d);
		slt(c, o, a, b, x->op == J_SLT);
		fin(c, x->d, o);
		break;
	case J_MAX: case J_MIN: {
		u32 *h;
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		e_cmpw(&c->e, 0, a, b);
		e_mr(&c->e, P_T0, a);
		h = e_bc_hueco(&c->e, BO_CIERTO, x->op == J_MAX ? CR_GT : CR_LT);
		e_mr(&c->e, P_T0, b);
		e_parchea_bc(h, c->e.p);
		fin(c, x->d, P_T0);
		break;
	}
	case J_ADDIU:
		o = dst(c, x->d);
		if(!x->s) e_carga32(&c->e, o, x->imm);
		else e_addi(&c->e, o, lee(c, x->s, P_T1), (s32)x->imm);
		fin(c, x->d, o);
		break;
	case J_SLTI: case J_SLTIU:
		a = lee(c, x->s, P_T1);
		o = dst(c, x->d);
		if(x->op == J_SLTI){
			e_xoris(&c->e, P_T0, a, 0x8000);
			e_carga32(&c->e, P_T2, x->imm ^ 0x80000000u);
			e_subfc(&c->e, P_T1, P_T2, P_T0);
			e_subfe(&c->e, o, P_T1, P_T1);
		} else {
			e_carga32(&c->e, P_T2, x->imm);
			e_subfc(&c->e, P_T0, P_T2, a);
			e_subfe(&c->e, o, P_T0, P_T0);
		}
		e_neg(&c->e, o, o);
		fin(c, x->d, o);
		break;
	case J_ANDI: case J_ORI: case J_XORI:
		a = lee(c, x->s, P_T1);
		o = dst(c, x->d);
		if(x->op == J_ANDI) e_andi_(&c->e, o, a, x->imm);
		else if(x->op == J_ORI) e_ori(&c->e, o, a, x->imm);
		else e_xori(&c->e, o, a, x->imm);
		fin(c, x->d, o);
		break;
	case J_LUI:
		o = dst(c, x->d);
		e_lis(&c->e, o, (s32)x->imm);
		fin(c, x->d, o);
		break;
	case J_EXT: {
		const u32 pos = x->sa, t = x->tam < 32 - pos ? x->tam : 32 - pos;
		a = lee(c, x->s, P_T1);
		o = dst(c, x->d);
		e_rlwinm(&c->e, o, a, (int)((32 - pos) & 31), (int)(32 - t), 31);
		fin(c, x->d, o);
		break;
	}
	case J_INS: {
		const u32 pos = x->sa, top = (pos + x->tam < 32 ? pos + x->tam : 32) - 1;
		int v;
		if(!x->tam) break;
		a = lee(c, x->s, P_T1);
		v = lee(c, x->d, P_T0);
		e_rlwimi(&c->e, v, a, (int)pos, (int)(31 - top), (int)(31 - pos));
		fin(c, x->d, v);
		break;
	}
	case J_LB: case J_LH: case J_LW: case J_LBU: case J_LHU: {
		const int t = x->op == J_LW ? 4 : (x->op == J_LH || x->op == J_LHU) ? 2 : 1;
		direccion(c, x, k, t);
		o = dst(c, x->d);
		if(t == 4) e_lwbrx(&c->e, o, P_RAM, P_T2);
		else if(t == 2){ e_lhbrx(&c->e, o, P_RAM, P_T2); if(x->op == J_LH) e_extsh(&c->e, o, o); }
		else { e_lbzx(&c->e, o, P_RAM, P_T2); if(x->op == J_LB) e_extsb(&c->e, o, o); }
		fin(c, x->d, o);
		break;
	}
	case J_SB: guardar(c, x, k, 1); break;
	case J_SH: guardar(c, x, k, 2); break;
	case J_SW: guardar(c, x, k, 4); break;
	case J_BEQ: case J_BNE:
		a = lee(c, x->s, P_T1);
		b = lee(c, x->t, P_T2);
		e_cmpw(&c->e, 0, a, b);
		salto_condicional(c, x->op == J_BEQ ? BO_FALSO : BO_CIERTO, CR_EQ, fin_bloque, x->destino);
		break;
	case J_BLEZ: case J_BGTZ: case J_BLTZ: case J_BGEZ:
		a = lee(c, x->s, P_T1);
		e_cmpwi(&c->e, 0, a, 0);
		if(x->op == J_BLEZ) salto_condicional(c, BO_CIERTO, CR_GT, fin_bloque, x->destino);
		else if(x->op == J_BGTZ) salto_condicional(c, BO_FALSO, CR_GT, fin_bloque, x->destino);
		else if(x->op == J_BLTZ) salto_condicional(c, BO_FALSO, CR_LT, fin_bloque, x->destino);
		else salto_condicional(c, BO_CIERTO, CR_LT, fin_bloque, x->destino);
		break;
	case J_J: case J_JAL:
		if(x->op == J_JAL){ e_carga32(&c->e, P_T0, fin_bloque); fin(c, 31, P_T0); }
		e_carga32(&c->e, P_T0, x->destino);
		e_stw(&c->e, P_T0, OFF_PC, P_CTX);
		break;
	case J_JR: case J_JALR:
		a = lee(c, x->s, P_T1);
		e_stw(&c->e, a, OFF_PC, P_CTX);
		if(x->op == J_JALR){ e_carga32(&c->e, P_T0, fin_bloque); fin(c, x->d, P_T0); }
		break;
	}
}

//Arreglo
static void elegir_cache(Comp *c, const Ins *x, u32 k){
	u32 cuenta[34], i, j;
	int r, n = 0;
	memset(cuenta, 0, sizeof(cuenta));
	memset(c->en, -1, sizeof(c->en));
	for(i = 0; i < k; i++)
		for(r = 1; r < 34; r++) cuenta[r] += (u32)(((x[i].lee >> r) & 1) + ((x[i].esc >> r) & 1));
	for(j = 0; j < NCACHE; j++){
		int mejor = 0;
		for(r = 1; r < 34; r++) if(c->en[r] < 0 && cuenta[r] >= 2 && (!mejor || cuenta[r] > cuenta[mejor])) mejor = r;
		if(!mejor) break;
		c->en[mejor] = (s8)n++;
	}
}

//Arreglo
static Ins x_[JIT_MAX_N];
static Comp c_;

//Arreglo
static u32 generar(u32 *ini, u32 cap, u32 pc, u32 n, u32 k, int branch, int verificar, u32 hb){
	Comp *const c = &c_;
	const Ins *const x = x_;
	const u32 fin_bloque = pc + 4 * n;
	u32 i;
	int r;
	e_ini(&c->e, ini, cap);
	c->sucio = 0;
	c->verificar = (u8)verificar;
	c->hb = (u8)hb;
	c->nsal = 0;
	elegir_cache(c, x, k);
	if(verificar){
		s32 ha, lo;
		ha_lo((u32)(uintptr_t)&jit_log_pos, &ha, &lo);
		e_w(&c->e, PPC_D(37, 1, 1, -16));
		e_stw(&c->e, P_LOG, 8, 1);
		e_lis(&c->e, P_T2, ha);
		e_lwz(&c->e, P_LOG, lo, P_T2);
	}
	for(r = 1; r < 34; r++){
		u64 visto = 0;
		if(c->en[r] < 0) continue;
		for(i = 0; i < k && !visto; i++){
			if((x[i].lee >> r) & 1){ e_lwz(&c->e, cache_ppc[(int)c->en[r]], off_reg(r), P_CTX); visto = 1; }
			else if((x[i].esc >> r) & 1) visto = 1;
		}
	}
	for(i = 0; i < k; i++) traducir(c, &x[i], i, fin_bloque);
	if(k == n){
		if(!branch){
			e_carga32(&c->e, P_T0, fin_bloque);
			e_stw(&c->e, P_T0, OFF_PC, P_CTX);
		}
		salir(c, c->sucio, JIT_HECHO);
	} else salir(c, c->sucio, k);
	for(i = 0; i < (u32)c->nsal && !c->e.lleno; i++){
		u32 j, *aqui;
		if(!c->sal[i].w) continue;
		aqui = c->e.p;
		salir(c, c->sal[i].sucio, c->sal[i].k);
		for(j = i; j < (u32)c->nsal; j++)
			if(c->sal[j].w && c->sal[j].k == c->sal[i].k){ if(!c->e.lleno) e_parchea_bc(c->sal[j].w, aqui); if(j != i) c->sal[j].w = NULL; }
		c->sal[i].w = NULL;
	}
	return c->e.lleno ? 0 : e_pos(&c->e);
}

//Arreglo
JitFn jit_compilar(u32 pc, const u32 *raw, u32 n, int branch, int verificar, u32 *cubre){
	u32 k, i, *ini, palabras, sz = psp_mem.ram_size;
	*cubre = 0;
	if(!JIT_PPC || n > JIT_MAX_N || (sz != PSP_RAM_SIZE_32MB && sz != PSP_RAM_SIZE_64MB)) return NULL;
	if(!arena_lista()) return NULL;
	for(i = 0; i < n; i++) analizar(raw[i], pc + 4 * i, &x_[i]);
	k = jit_prefijo(raw, n, branch);
	if(!k){ st.sin_nada++; return NULL; }
	usado = (usado + 31) & ~31u;
	if(usado >= tam_hecho){ lleno = 1; st.llenos++; return NULL; }
	ini = (u32 *)(arena + usado);
	palabras = generar(ini, (tam_hecho - usado) / 4, pc, n, k, branch, verificar, sz == PSP_RAM_SIZE_32MB ? 7 : 6);
	if(!palabras){ lleno = 1; st.llenos++; return NULL; }
	jit_publicar(ini, palabras * 4);
	usado += palabras * 4;
	st.compilados++;
	st.instr_compiladas += k;
	st.palabras += palabras;
	*cubre = k;
	return (JitFn)(uintptr_t)ini;
}

//Arreglo
u32 jit_tam_estimado(u32 pc, const u32 *raw, u32 n, int branch, u32 *cubre){
	static u32 *buf;
	u32 k, i;
	*cubre = 0;
	if(!buf) buf = malloc(4096 * 4);
	if(!buf) return 0;
	if(n > JIT_MAX_N) n = JIT_MAX_N;
	for(i = 0; i < n; i++) analizar(raw[i], pc + 4 * i, &x_[i]);
	*cubre = k = jit_prefijo(raw, n, branch);
	return k ? generar(buf, 4096, pc, n, k, branch, 0, psp_mem.ram_size == PSP_RAM_SIZE_64MB ? 6 : 7) : 0;
}

//Arreglo
static u32 leer_mem(u32 o, u32 t){
	const u8 *p = psp_mem.ram + o;
	return t == 4 ? rd_le32(p) : t == 2 ? rd_le16(p) : p[0];
}

//Arreglo
static void escribir_mem(u32 o, u32 t, u32 v){
	u8 *p = psp_mem.ram + o;
	if(t == 4) wr_le32(p, v);
	else if(t == 2) wr_le16(p, (u16)v);
	else p[0] = (u8)v;
}

//Arreglo
u32 jit_verificar_bloque(JitFn f, u32 pc, u32 n){
	static u32 r0[32], rn[32], fin_nat[JIT_MAX_N];
	u32 hi0 = cpu.hi, lo0 = cpu.lo, hin, lon, pcn, k, i, nlog;
	u64 ciclos, hechas;
	int mal = 0;
	memcpy(r0, cpu.r, sizeof(r0));
	jit_log_pos = jit_log;
	k = f((u8 *)cpu.r, psp_mem.ram, cpu_code_map);
	if(k != JIT_HECHO){ st.verif_parciales++; return k; }
	st.verif_bloques++;
	nlog = (u32)(jit_log_pos - jit_log) / 3;
	memcpy(rn, cpu.r, sizeof(rn));
	hin = cpu.hi; lon = cpu.lo; pcn = cpu.pc;
	for(i = 0; i < nlog; i++) fin_nat[i] = leer_mem(jit_log[3 * i], jit_log[3 * i + 2]);
	for(i = nlog; i-- > 0;) escribir_mem(jit_log[3 * i], jit_log[3 * i + 2], jit_log[3 * i + 1]);
	memcpy(cpu.r, r0, sizeof(r0));
	cpu.hi = hi0; cpu.lo = lo0;
	ciclos = cpu_cycles; hechas = cpu_executed;
	cpu.pc = pc; cpu.npc = pc + 4;
	cpu_run_interp(n);
	cpu_cycles = ciclos; cpu_executed = hechas;
	for(i = 0; i < 32; i++) if(rn[i] != cpu.r[i]) mal = 1;
	if(hin != cpu.hi || lon != cpu.lo || pcn != cpu.pc) mal = 1;
	for(i = 0; i < nlog; i++) if(leer_mem(jit_log[3 * i], jit_log[3 * i + 2]) != fin_nat[i]) mal = 1;
	if(mal){
		if(st.verif_mal < 16){
			char l[300];
			size_t len = (size_t)snprintf(l, sizeof(l), "[JIT] bloque %08X (%u instrucciones) distinto del interprete:", (unsigned)pc, (unsigned)n);
			for(i = 0; i < 32 && len + 40 < sizeof(l); i++)
				if(rn[i] != cpu.r[i]) len += (size_t)snprintf(l + len, sizeof(l) - len, " r%u %08X/%08X", (unsigned)i, (unsigned)rn[i], (unsigned)cpu.r[i]);
			if(hin != cpu.hi || lon != cpu.lo) len += (size_t)snprintf(l + len, sizeof(l) - len, " hi/lo");
			if(pcn != cpu.pc && len + 30 < sizeof(l)) len += (size_t)snprintf(l + len, sizeof(l) - len, " pc %08X/%08X", (unsigned)pcn, (unsigned)cpu.pc);
			hle_log_quiet("%s (nativo/interprete)\n", l);
		}
		st.verif_mal++;
	}
	return JIT_HECHO;
}
