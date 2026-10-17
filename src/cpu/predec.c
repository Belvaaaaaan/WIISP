/**
 * WIISP - cpu/predec.c
 * Intérprete predecodificado (ver predec.h).
 *
 * Un bloque son las instrucciones seguidas desde una dirección hasta la
 * primera que salta (con su delay slot), un syscall, una que no tiene
 * tarjeta o BLOQUE_MAX instrucciones. Cada tarjeta repite lo que hace
 * cpu_run (interp.c) con su instrucción; el reloj se descuenta al entrar en
 * el bloque y, si hay que salir antes (una llamada fuera pide parar, la
 * memoria lenta, el bloque se reescribió), la posición de la tarjeta da el
 * pc, el npc y las instrucciones hechas.
 *
 * Las tarjetas viven en una arena que se vacía entera cuando se llena. Cada
 * bloque recuerda sus dos sucesores (el que sigue y el destino del salto),
 * así que casi nunca hace falta buscar en la tabla.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
#include "cpu/predec.h"
#include "cpu/vfpu.h"
#include "core/memory.h"
#include "core/opt.h"
#include "hle/hle.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define BLOQUE_MAX  64
#define TABLA_BITS  14
#define ARENA_BYTES (4u << 20)

u8 cpu_code_map[(64u << 20) >> PREDEC_ZONA_BITS];

typedef struct {
	const void *h;              /* el código que la ejecuta */
	union {
		struct { u8 d, s, t, sa; } r;   /* registros como desplazamientos en bytes */
		struct { u8 s, t; s16 imm; } i;
		u32 w;
	} a;
} Card;

typedef struct Block {
	u32 pc;        /* la primera instrucción */
	u32 end;       /* la que sigue a la última */
	u32 target;    /* destino fijo del salto (j, jal, b*) */
	u16 n;         /* instrucciones (0: las ejecuta el intérprete de siempre) */
	u8 alive;
	u8 branch;     /* acaba en un salto y su delay slot */
	u32 z0, z1;    /* zonas de 1 KB que ocupa */
	struct Block *hnext, *znext[2], *succ[2];
	u32 *raw;      /* las instrucciones (para la VFPU, los fallos y cpu_verificar) */
	Card c[];      /* n tarjetas y la de cierre */
} Block;

enum {
	OP_NOP, OP_SLL, OP_SRL, OP_ROTR, OP_SRA, OP_SLLV, OP_SRLV, OP_ROTRV, OP_SRAV, OP_MOVZ, OP_MOVN,
	OP_MFHI, OP_MTHI, OP_MFLO, OP_MTLO, OP_CLZ, OP_CLO, OP_MULT, OP_MULTU, OP_DIV, OP_DIVU,
	OP_MADD, OP_MADDU, OP_MSUB, OP_MSUBU, OP_ADDU, OP_SUBU, OP_AND, OP_OR, OP_XOR, OP_NOR,
	OP_SLT, OP_SLTU, OP_MAX, OP_MIN,
	OP_ADDIU, OP_SLTI, OP_SLTIU, OP_ANDI, OP_ORI, OP_XORI, OP_LUI,
	OP_EXT, OP_INS, OP_WSBH, OP_WSBW, OP_SEB, OP_SEH, OP_BITREV,
	OP_LB, OP_LH, OP_LW, OP_LBU, OP_LHU, OP_LWL, OP_LWR, OP_LL,
	OP_SB, OP_SH, OP_SW, OP_SWL, OP_SWR, OP_SC, OP_LWC1, OP_SWC1, OP_CACHEWB,
	OP_MFC1, OP_MTC1, OP_CFC1, OP_CTC1, OP_FPU, OP_CVTSW, OP_MFIC, OP_MTIC, OP_VFPU, OP_SYSCALL,
	OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ, OP_BLTZ, OP_BGEZ, OP_BLTZAL, OP_BGEZAL,
	OP_BEQL, OP_BNEL, OP_BLEZL, OP_BGTZL, OP_BLTZL, OP_BGEZL, OP_BLTZALL, OP_BGEZALL,
	OP_J, OP_JAL, OP_JR, OP_JALR, OP_BC1, OP_BV,
	OP_END, OP_BEND,
	OP_N
};

static const void *h_tab[OP_N];

/* --- Arena, tabla y zonas ------------------------------------------------- */

static u8 *arena;
static u32 arena_used;
static Block *tabla[1u << TABLA_BITS];
static Block **zona_head;        /* lista de bloques de cada zona de 1 KB */
static u32 zonas;
static PredecStats st;

static inline u32 slot(u32 pc){ return (pc >> 2) & ((1u << TABLA_BITS) - 1); }

static inline Block *lookup(u32 pc){
	Block *b = tabla[slot(pc)];
	while(b && b->pc != pc) b = b->hnext;
	return b;
}

static void unhash(Block *b){
	Block **pp = &tabla[slot(b->pc)];
	while(*pp && *pp != b) pp = &(*pp)->hnext;
	if(*pp) *pp = b->hnext;
}

static void kill_block(Block *b){
	b->alive = 0;
	unhash(b);
	st.bloques_tirados++;
}

static void kill_zone(u32 z){
	Block *b;
	if(!zona_head || z >= zonas) return;
	b = zona_head[z];
	zona_head[z] = NULL;
	cpu_code_map[z] = 0;
	while(b){
		Block *nx = b->znext[b->z0 == z ? 0 : 1];
		if(b->alive) kill_block(b);
		b = nx;
	}
}

void cpu_predec_store(u32 off, u32 len){
	u32 z = off >> PREDEC_ZONA_BITS, last = (off + (len ? len - 1 : 0)) >> PREDEC_ZONA_BITS;
	st.escrituras_en_codigo++;
	for(; z <= last && z < zonas; z++) if(cpu_code_map[z]) kill_zone(z);
}

void cpu_predec_flush(void){
	if(arena_used) st.vaciados++;
	memset(tabla, 0, sizeof(tabla));
	if(zona_head) memset(zona_head, 0, zonas * sizeof(*zona_head));
	memset(cpu_code_map, 0, sizeof(cpu_code_map));
	arena_used = 0;
}

void cpu_predec_stats(PredecStats *out){
	*out = st;
	out->arena_usada = arena_used;
	out->arena_total = arena ? ARENA_BYTES : 0;
}

static int predec_init(void){
	zonas = psp_mem.ram_size >> PREDEC_ZONA_BITS;
	if(zonas > sizeof(cpu_code_map)) return 0;
	arena = malloc(ARENA_BYTES);
	zona_head = calloc(zonas, sizeof(*zona_head));
	if(!arena || !zona_head){
		free(arena); free(zona_head);
		arena = NULL; zona_head = NULL;
		return 0;
	}
	cpu_predec_flush();
	st.vaciados = 0;
	return 1;
}

/* --- Traducir ----------------------------------------------------------- */

enum { K_NORMAL, K_SALTO, K_SYSCALL, K_PARA };

#define RS_(w)  (((w) >> 21) & 31)
#define RT_(w)  (((w) >> 16) & 31)
#define RD_(w)  (((w) >> 11) & 31)
#define SA_(w)  (((w) >> 6) & 31)
#define FN_(w)  ((w) & 63)

static void card_r(Card *cd, int op, u32 d, u32 s, u32 t, u32 sa){
	cd->h = h_tab[op];
	cd->a.r.d = (u8)(d * 4); cd->a.r.s = (u8)(s * 4); cd->a.r.t = (u8)(t * 4); cd->a.r.sa = (u8)sa;
}
static void card_i(Card *cd, int op, u32 s, u32 t, u32 imm){
	cd->h = h_tab[op];
	cd->a.i.s = (u8)(s * 4); cd->a.i.t = (u8)(t * 4); cd->a.i.imm = (s16)(u16)imm;
}
static void card_w(Card *cd, int op, u32 w){ cd->h = h_tab[op]; cd->a.w = w; }
/* Los cuatro bytes tal cual (posiciones, tamaños, bits) */
static void card_b(Card *cd, int op, u32 d, u32 s, u32 t, u32 sa){
	cd->h = h_tab[op];
	cd->a.r.d = (u8)d; cd->a.r.s = (u8)(s * 4); cd->a.r.t = (u8)(t * 4); cd->a.r.sa = (u8)sa;
}
static int nop(Card *cd){ card_w(cd, OP_NOP, 0); return K_NORMAL; }

/* La tarjeta de una instrucción; *target, el destino de un salto fijo */
static int decode(u32 w, u32 ipc, Card *cd, u32 *target){
	const u32 rs = RS_(w), rt = RT_(w), rd = RD_(w), sa = SA_(w), fn = FN_(w), imm = w & 0xFFFF;
	*target = ipc + 4 + ((u32)(s32)(s16)imm << 2);
	switch(w >> 26){
	case 0x00:
		switch(fn){
		case 0x00: if(!rd) return nop(cd); card_r(cd, OP_SLL, rd, 0, rt, sa); return K_NORMAL;
		case 0x02:
			if(!rd) return nop(cd);
			if((rs & 1) && sa) card_r(cd, OP_ROTR, rd, 0, rt, sa);
			else card_r(cd, OP_SRL, rd, 0, rt, (rs & 1) ? 0 : sa);
			return K_NORMAL;
		case 0x03: if(!rd) return nop(cd); card_r(cd, OP_SRA, rd, 0, rt, sa); return K_NORMAL;
		case 0x04: if(!rd) return nop(cd); card_r(cd, OP_SLLV, rd, rs, rt, 0); return K_NORMAL;
		case 0x06: if(!rd) return nop(cd); card_r(cd, (sa & 1) ? OP_ROTRV : OP_SRLV, rd, rs, rt, 0); return K_NORMAL;
		case 0x07: if(!rd) return nop(cd); card_r(cd, OP_SRAV, rd, rs, rt, 0); return K_NORMAL;
		case 0x08: card_r(cd, OP_JR, 0, rs, 0, 0); return K_SALTO;
		case 0x09: card_r(cd, rd ? OP_JALR : OP_JR, rd, rs, 0, 0); return K_SALTO;
		case 0x0A: if(!rd) return nop(cd); card_r(cd, OP_MOVZ, rd, rs, rt, 0); return K_NORMAL;
		case 0x0B: if(!rd) return nop(cd); card_r(cd, OP_MOVN, rd, rs, rt, 0); return K_NORMAL;
		case 0x0C: card_w(cd, OP_SYSCALL, (w >> 6) & 0xFFFFF); return K_SYSCALL;
		case 0x0F: return nop(cd);
		case 0x10: if(!rd) return nop(cd); card_r(cd, OP_MFHI, rd, 0, 0, 0); return K_NORMAL;
		case 0x11: card_r(cd, OP_MTHI, 0, rs, 0, 0); return K_NORMAL;
		case 0x12: if(!rd) return nop(cd); card_r(cd, OP_MFLO, rd, 0, 0, 0); return K_NORMAL;
		case 0x13: card_r(cd, OP_MTLO, 0, rs, 0, 0); return K_NORMAL;
		case 0x16: if(!rd) return nop(cd); card_r(cd, OP_CLZ, rd, rs, 0, 0); return K_NORMAL;
		case 0x17: if(!rd) return nop(cd); card_r(cd, OP_CLO, rd, rs, 0, 0); return K_NORMAL;
		case 0x18: card_r(cd, OP_MULT, 0, rs, rt, 0); return K_NORMAL;
		case 0x19: card_r(cd, OP_MULTU, 0, rs, rt, 0); return K_NORMAL;
		case 0x1A: card_r(cd, OP_DIV, 0, rs, rt, 0); return K_NORMAL;
		case 0x1B: card_r(cd, OP_DIVU, 0, rs, rt, 0); return K_NORMAL;
		case 0x1C: card_r(cd, OP_MADD, 0, rs, rt, 0); return K_NORMAL;
		case 0x1D: card_r(cd, OP_MADDU, 0, rs, rt, 0); return K_NORMAL;
		case 0x2E: card_r(cd, OP_MSUB, 0, rs, rt, 0); return K_NORMAL;
		case 0x2F: card_r(cd, OP_MSUBU, 0, rs, rt, 0); return K_NORMAL;
		case 0x20: case 0x21: if(!rd) return nop(cd); card_r(cd, OP_ADDU, rd, rs, rt, 0); return K_NORMAL;
		case 0x22: case 0x23: if(!rd) return nop(cd); card_r(cd, OP_SUBU, rd, rs, rt, 0); return K_NORMAL;
		case 0x24: if(!rd) return nop(cd); card_r(cd, OP_AND, rd, rs, rt, 0); return K_NORMAL;
		case 0x25: if(!rd) return nop(cd); card_r(cd, OP_OR, rd, rs, rt, 0); return K_NORMAL;
		case 0x26: if(!rd) return nop(cd); card_r(cd, OP_XOR, rd, rs, rt, 0); return K_NORMAL;
		case 0x27: if(!rd) return nop(cd); card_r(cd, OP_NOR, rd, rs, rt, 0); return K_NORMAL;
		case 0x2A: if(!rd) return nop(cd); card_r(cd, OP_SLT, rd, rs, rt, 0); return K_NORMAL;
		case 0x2B: if(!rd) return nop(cd); card_r(cd, OP_SLTU, rd, rs, rt, 0); return K_NORMAL;
		case 0x2C: if(!rd) return nop(cd); card_r(cd, OP_MAX, rd, rs, rt, 0); return K_NORMAL;
		case 0x2D: if(!rd) return nop(cd); card_r(cd, OP_MIN, rd, rs, rt, 0); return K_NORMAL;
		default: return K_PARA;   /* break y las que no existen: fallan */
		}
	case 0x01: {
		static const u8 ops[32] = {
			[0x00] = OP_BLTZ, [0x01] = OP_BGEZ, [0x02] = OP_BLTZL, [0x03] = OP_BGEZL,
			[0x10] = OP_BLTZAL, [0x11] = OP_BGEZAL, [0x12] = OP_BLTZALL, [0x13] = OP_BGEZALL
		};
		if(rt > 0x13 || (rt > 0x03 && rt < 0x10)) return K_PARA;
		card_r(cd, ops[rt], 0, rs, 0, 0);
		return K_SALTO;
	}
	case 0x02: case 0x03:
		*target = (ipc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
		card_w(cd, (w >> 26) == 0x02 ? OP_J : OP_JAL, 0);
		return K_SALTO;
	case 0x04: card_r(cd, OP_BEQ, 0, rs, rt, 0); return K_SALTO;
	case 0x05: card_r(cd, OP_BNE, 0, rs, rt, 0); return K_SALTO;
	case 0x06: card_r(cd, OP_BLEZ, 0, rs, 0, 0); return K_SALTO;
	case 0x07: card_r(cd, OP_BGTZ, 0, rs, 0, 0); return K_SALTO;
	case 0x14: card_r(cd, OP_BEQL, 0, rs, rt, 0); return K_SALTO;
	case 0x15: card_r(cd, OP_BNEL, 0, rs, rt, 0); return K_SALTO;
	case 0x16: card_r(cd, OP_BLEZL, 0, rs, 0, 0); return K_SALTO;
	case 0x17: card_r(cd, OP_BGTZL, 0, rs, 0, 0); return K_SALTO;
	case 0x08: case 0x09: if(!rt) return nop(cd); card_i(cd, OP_ADDIU, rs, rt, imm); return K_NORMAL;
	case 0x0A: if(!rt) return nop(cd); card_i(cd, OP_SLTI, rs, rt, imm); return K_NORMAL;
	case 0x0B: if(!rt) return nop(cd); card_i(cd, OP_SLTIU, rs, rt, imm); return K_NORMAL;
	case 0x0C: if(!rt) return nop(cd); card_i(cd, OP_ANDI, rs, rt, imm); return K_NORMAL;
	case 0x0D: if(!rt) return nop(cd); card_i(cd, OP_ORI, rs, rt, imm); return K_NORMAL;
	case 0x0E: if(!rt) return nop(cd); card_i(cd, OP_XORI, rs, rt, imm); return K_NORMAL;
	case 0x0F: if(!rt) return nop(cd); card_i(cd, OP_LUI, 0, rt, imm); return K_NORMAL;
	case 0x10: return nop(cd);
	case 0x11:
		switch(rs){
		case 0x00: if(!rt) return nop(cd); card_r(cd, OP_MFC1, 0, rd, rt, 0); return K_NORMAL;   /* s = fs */
		case 0x02:
			if(!rt) return nop(cd);
			if(rd == 31){ card_r(cd, OP_CFC1, 0, 0, rt, 0); return K_NORMAL; }
			card_i(cd, OP_ORI, 0, rt, rd == 0 ? FCR0_VALUE : 0);
			return K_NORMAL;
		case 0x04: card_r(cd, OP_MTC1, rd, 0, rt, 0); return K_NORMAL;   /* d = fs */
		case 0x06: if(rd != 31) return nop(cd); card_r(cd, OP_CTC1, 0, 0, rt, 0); return K_NORMAL;
		case 0x08: card_r(cd, OP_BC1, 0, 0, 0, rt & 3); return K_SALTO;
		case 0x10:
			if(fn <= 0x07 || (fn >= 0x0C && fn <= 0x0F) || fn == 0x24 || fn >= 0x30){
				card_w(cd, OP_FPU, 0);
				return K_NORMAL;
			}
			return K_PARA;
		case 0x14:
			if(fn != 0x20) return K_PARA;
			card_r(cd, OP_CVTSW, sa, rd, 0, 0);   /* d = fd, s = fs */
			return K_NORMAL;
		default: return K_PARA;
		}
	case 0x12:
		if(rs == 8){ card_b(cd, OP_BV, (w >> 18) & 7, 0, 0, rt & 3); return K_SALTO; }
		card_w(cd, OP_VFPU, 0);
		return K_NORMAL;
	case 0x1C:
		if(fn == 0x24){ if(!rt) return nop(cd); card_r(cd, OP_MFIC, 0, 0, rt, 0); return K_NORMAL; }
		if(fn == 0x26){ card_r(cd, OP_MTIC, 0, 0, rt, 0); return K_NORMAL; }
		return nop(cd);
	case 0x1F:
		switch(fn){
		case 0x00:
			if(!rt) return nop(cd);
			card_b(cd, OP_EXT, sa, rs, rt, rd + 1);   /* d = posición, sa = tamaño */
			return K_NORMAL;
		case 0x04: {
			u32 size = rd + 1 - sa;
			if(!rt) return nop(cd);
			card_b(cd, OP_INS, sa, rs, rt, size >= 32 ? 32 : size);
			return K_NORMAL;
		}
		case 0x20:
			if(!rd){
				if(sa == 0x02 || sa == 0x03 || sa == 0x10 || sa == 0x14 || sa == 0x18) return nop(cd);
				return K_PARA;
			}
			switch(sa){
			case 0x02: card_r(cd, OP_WSBH, rd, 0, rt, 0); return K_NORMAL;
			case 0x03: card_r(cd, OP_WSBW, rd, 0, rt, 0); return K_NORMAL;
			case 0x10: card_r(cd, OP_SEB, rd, 0, rt, 0); return K_NORMAL;
			case 0x14: card_r(cd, OP_BITREV, rd, 0, rt, 0); return K_NORMAL;
			case 0x18: card_r(cd, OP_SEH, rd, 0, rt, 0); return K_NORMAL;
			default: return K_PARA;
			}
		default: return K_PARA;
		}
	/* Cargas a $zero y sc con $zero: los ejecuta el intérprete (la
	   tarjeta nunca escribe $zero) */
	case 0x20: if(!rt) return K_PARA; card_i(cd, OP_LB, rs, rt, imm); return K_NORMAL;
	case 0x21: if(!rt) return K_PARA; card_i(cd, OP_LH, rs, rt, imm); return K_NORMAL;
	case 0x22: if(!rt) return K_PARA; card_i(cd, OP_LWL, rs, rt, imm); return K_NORMAL;
	case 0x23: if(!rt) return K_PARA; card_i(cd, OP_LW, rs, rt, imm); return K_NORMAL;
	case 0x24: if(!rt) return K_PARA; card_i(cd, OP_LBU, rs, rt, imm); return K_NORMAL;
	case 0x25: if(!rt) return K_PARA; card_i(cd, OP_LHU, rs, rt, imm); return K_NORMAL;
	case 0x26: if(!rt) return K_PARA; card_i(cd, OP_LWR, rs, rt, imm); return K_NORMAL;
	case 0x30: if(!rt) return K_PARA; card_i(cd, OP_LL, rs, rt, imm); return K_NORMAL;
	case 0x28: card_i(cd, OP_SB, rs, rt, imm); return K_NORMAL;
	case 0x29: card_i(cd, OP_SH, rs, rt, imm); return K_NORMAL;
	case 0x2A: card_i(cd, OP_SWL, rs, rt, imm); return K_NORMAL;
	case 0x2B: card_i(cd, OP_SW, rs, rt, imm); return K_NORMAL;
	case 0x2E: card_i(cd, OP_SWR, rs, rt, imm); return K_NORMAL;
	case 0x38: if(!rt) return K_PARA; card_i(cd, OP_SC, rs, rt, imm); return K_NORMAL;
	case 0x2F:
		if(rt == 0x1A || rt == 0x1B){ card_i(cd, OP_CACHEWB, rs, 0, imm); return K_NORMAL; }
		return nop(cd);
	case 0x31: card_i(cd, OP_LWC1, rs, rt, imm); return K_NORMAL;   /* t = ft */
	case 0x39: card_i(cd, OP_SWC1, rs, rt, imm); return K_NORMAL;
	case 0x18: case 0x19: case 0x1B: case 0x32: case 0x34: case 0x35: case 0x36:
	case 0x37: case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
		card_w(cd, OP_VFPU, 0);
		return K_NORMAL;
	default: return K_PARA;
	}
}

static Block *alloc_block(u32 n){
	u32 bytes = (u32)(sizeof(Block) + (n + 1) * sizeof(Card) + n * 4 + 7) & ~7u;
	Block *b;
	if(arena_used + bytes > ARENA_BYTES) cpu_predec_flush();
	b = (Block *)(arena + arena_used);
	arena_used += bytes;
	memset(b, 0, sizeof(*b));
	b->raw = (u32 *)(b->c + n + 1);
	return b;
}

/* Traduce el bloque que empieza en pc (en la RAM y alineado). Un bloque
   con n = 0 dice que ahí empieza algo que ejecuta el intérprete. */
static Block *build(u32 pc, u32 lim4){
	static Card tmp[BLOQUE_MAX + 2];
	static u32 raw[BLOQUE_MAX + 2];
	const u8 *ram = psp_mem.ram;
	u32 n = 0, target = 0, o0 = (pc & PSP_ADDR_MASK) - PSP_RAM_BASE, t;
	int branch = 0, kind;
	Block *b;

	while(n < BLOQUE_MAX){
		u32 o = o0 + n * 4, w;
		if(o >= lim4) break;
		w = rd_le32(ram + o);
		kind = decode(w, pc + n * 4, &tmp[n], &t);
		if(kind == K_PARA) break;
		raw[n] = w;
		if(kind == K_SYSCALL){ n++; break; }
		if(kind == K_SALTO){
			/* Con su delay slot, que tiene que ser una instrucción normal */
			u32 wd, td;
			Card cd;
			if(o + 4 >= lim4) break;
			wd = rd_le32(ram + o + 4);
			if(decode(wd, pc + n * 4 + 4, &cd, &td) != K_NORMAL) break;
			target = t;
			tmp[n + 1] = cd;
			raw[n + 1] = wd;
			n += 2;
			branch = 1;
			break;
		}
		n++;
	}

	b = alloc_block(n);
	b->pc = pc;
	b->n = (u16)n;
	b->end = pc + n * 4;
	b->target = target;
	b->branch = (u8)branch;
	b->alive = 1;
	memcpy(b->c, tmp, n * sizeof(Card));
	memcpy(b->raw, raw, n * 4);
	/* La de cierre (un bloque que acaba en syscall no llega a ella) */
	b->c[n].h = h_tab[branch ? OP_BEND : OP_END];
	b->z0 = o0 >> PREDEC_ZONA_BITS;
	b->z1 = (o0 + (n ? n * 4 - 1 : 0)) >> PREDEC_ZONA_BITS;
	b->hnext = tabla[slot(pc)];
	tabla[slot(pc)] = b;
	b->znext[0] = zona_head[b->z0];
	zona_head[b->z0] = b;
	cpu_code_map[b->z0] = 1;
	if(b->z1 != b->z0){
		b->znext[1] = zona_head[b->z1];
		zona_head[b->z1] = b;
		cpu_code_map[b->z1] = 1;
	}
	st.bloques_hechos++;
	return b;
}

/* cpu_verificar: ¿siguen en la RAM las instrucciones del bloque? */
static int verif_ok(const Block *b){
	const u8 *p = psp_mem.ram + ((b->pc & PSP_ADDR_MASK) - PSP_RAM_BASE);
	u32 i;
	for(i = 0; i < b->n; i++){
		if(rd_le32(p + i * 4) != b->raw[i]){
			if(st.verif_viejos < 16)
				hle_log_quiet("[PREDEC] bloque viejo en %08X: tenia %08X y en la RAM hay %08X (falta un aviso de escritura)\n",
				              b->pc + i * 4, b->raw[i], rd_le32(p + i * 4));
			st.verif_viejos++;
			return 0;
		}
	}
	return 1;
}

/* --- Ejecutar ------------------------------------------------------------- */

u32 cpu_run_predec(u32 max_cycles){
	const u64 start = cpu_cycles;
	const u64 end = start + max_cycles;
	u8 *const ram = psp_mem.ram;
	const u32 lim4 = psp_mem.ram_size - 3, lim2 = psp_mem.ram_size - 1, lim1 = psp_mem.ram_size;
	u8 *const r8 = (u8 *)cpu.r;
	u8 *const f8 = (u8 *)cpu.fpr;
	const int verif = opt_on[OPT_CPU_VERIFICAR];
	u32 pc, npc, left, seg, bt = 0;
	const Card *c = NULL;
	Block *cblk = NULL, *nb;

	if(unlikely(!h_tab[OP_NOP])){
		static const void *const labels[OP_N] = {
			&&L_NOP, &&L_SLL, &&L_SRL, &&L_ROTR, &&L_SRA, &&L_SLLV, &&L_SRLV, &&L_ROTRV, &&L_SRAV, &&L_MOVZ, &&L_MOVN,
			&&L_MFHI, &&L_MTHI, &&L_MFLO, &&L_MTLO, &&L_CLZ, &&L_CLO, &&L_MULT, &&L_MULTU, &&L_DIV, &&L_DIVU,
			&&L_MADD, &&L_MADDU, &&L_MSUB, &&L_MSUBU, &&L_ADDU, &&L_SUBU, &&L_AND, &&L_OR, &&L_XOR, &&L_NOR,
			&&L_SLT, &&L_SLTU, &&L_MAX, &&L_MIN,
			&&L_ADDIU, &&L_SLTI, &&L_SLTIU, &&L_ANDI, &&L_ORI, &&L_XORI, &&L_LUI,
			&&L_EXT, &&L_INS, &&L_WSBH, &&L_WSBW, &&L_SEB, &&L_SEH, &&L_BITREV,
			&&L_LB, &&L_LH, &&L_LW, &&L_LBU, &&L_LHU, &&L_LWL, &&L_LWR, &&L_LL,
			&&L_SB, &&L_SH, &&L_SW, &&L_SWL, &&L_SWR, &&L_SC, &&L_LWC1, &&L_SWC1, &&L_CACHEWB,
			&&L_MFC1, &&L_MTC1, &&L_CFC1, &&L_CTC1, &&L_FPU, &&L_CVTSW, &&L_MFIC, &&L_MTIC, &&L_VFPU, &&L_SYSCALL,
			&&L_BEQ, &&L_BNE, &&L_BLEZ, &&L_BGTZ, &&L_BLTZ, &&L_BGEZ, &&L_BLTZAL, &&L_BGEZAL,
			&&L_BEQL, &&L_BNEL, &&L_BLEZL, &&L_BGTZL, &&L_BLTZL, &&L_BGEZL, &&L_BLTZALL, &&L_BGEZALL,
			&&L_J, &&L_JAL, &&L_JR, &&L_JALR, &&L_BC1, &&L_BV,
			&&L_END, &&L_BEND
		};
		memcpy(h_tab, labels, sizeof(h_tab));
	}
	if(cpu_stop_requested || !max_cycles) return 0;
	if(unlikely(!arena) && !predec_init()) return cpu_run_interp(max_cycles);
	pc = cpu.pc;
	npc = cpu.npc;
	left = seg = max_cycles;

/* Registros: los operandos de las tarjetas son desplazamientos en bytes */
#define G(o)   (*(u32 *)(r8 + (o)))
#define FR(o)  (*(u32 *)(f8 + (o)))
#define D      (c->a.r.d)
#define S      (c->a.r.s)
#define T      (c->a.r.t)
#define SAF    (c->a.r.sa)
#define IS     (c->a.i.s)
#define IT     (c->a.i.t)
#define SIMM   ((u32)(s32)c->a.i.imm)
#define ZIMM   ((u32)(u16)c->a.i.imm)
/* Lo del bloque que solo hace falta al salir o llamar fuera */
#define CB     (cblk->c)
#define BN     ((u32)cblk->n)
#define IDX()  ((u32)(c - CB))
#define RAW()  (cblk->raw[IDX()])
#define IPC()  (cblk->pc + 4u * IDX())
/* La siguiente tarjeta: su código se carga antes del cuerpo de esta */
#define OP(name, ...) L_##name: { const void *n_ = c[1].h; __VA_ARGS__; c++; goto *n_; }

/* El estado global, como lo deja cpu_run antes de llamar fuera durante la
   instrucción de esta tarjeta (en el delay slot, pc y npc ya son el destino) */
#define SYNC_AT() do { \
		u32 i_ = IDX(), cur_ = left + BN - i_; \
		if(cblk->branch && i_ == BN - 1){ cpu.pc = bt; cpu.npc = bt + 4; } \
		else { cpu.pc = cblk->pc + 4 * i_ + 4; cpu.npc = cpu.pc + 4; } \
		cpu_cycles = end - cur_; \
		cpu_executed += seg - cur_; \
		seg = cur_; \
	} while(0)

#define RAM_OFF(a) (((a) & PSP_ADDR_MASK) - PSP_RAM_BASE)
#define LDX(dst, a, size, lim, rd, tail) do { \
		u32 a_ = (a), o_ = RAM_OFF(a_); \
		if(likely(o_ < (lim))){ dst = rd; tail; } \
		else { \
			SYNC_AT(); dst = cpu_slow_load(a_, size, RAW()); tail; \
			if(unlikely(cpu_stop_requested)) goto x_after; \
		} \
	} while(0)
#define LD32(dst, a, tail) LDX(dst, a, 4, lim4, rd_le32(ram + o_), tail)
#define LD16(dst, a, tail) LDX(dst, a, 2, lim2, rd_le16(ram + o_), tail)
#define LD8(dst, a, tail)  LDX(dst, a, 1, lim1, ram[o_], tail)
/* Una escritura en una zona con bloques los tira; si tira este, se sale
   al acabar la instrucción */
#define STX(a, v, size, lim, wr, tail) do { \
		u32 a_ = (a), o_ = RAM_OFF(a_); \
		if(likely(o_ < (lim))){ \
			wr; \
			if(unlikely(cpu_code_map[o_ >> PREDEC_ZONA_BITS])){ \
				cpu_predec_store(o_, size); tail; \
				if(!cblk->alive) goto x_after; \
			} else { tail; } \
		} else { \
			SYNC_AT(); cpu_slow_store(a_, size, v, RAW()); tail; \
			if(unlikely(cpu_stop_requested)) goto x_after; \
		} \
	} while(0)
#define ST32(a, v, tail) STX(a, v, 4, lim4, wr_le32(ram + o_, v), tail)
#define ST16(a, v, tail) STX(a, v, 2, lim2, wr_le16(ram + o_, (u16)(v)), tail)
#define ST8(a, v, tail)  STX(a, v, 1, lim1, ram[o_] = (u8)(v), tail)

/* Saltos: bt es a dónde va después del delay slot */
#define BRANCH(name, cond) OP(name, { bt = (cond) ? cblk->target : cblk->end; })
#define BRANCH_LIKELY(name, cond) L_##name: { \
		if(cond){ const void *n_ = c[1].h; bt = cblk->target; c++; goto *n_; } \
		goto x_likely_nt; \
	}

/* Buscar (o traducir) el bloque de pc */
dispatch:
	if(unlikely(!left)) goto done;
	if(npc != pc + 4 || (pc & 3) || RAM_OFF(pc) >= lim4) goto step;
	nb = lookup(pc);
	if(!nb) nb = build(pc, lim4);
	st.busquedas++;
	if(unlikely(!nb->n || left < nb->n)) goto step;
	if(unlikely(verif) && !verif_ok(nb)){
		kill_block(nb);
		nb = build(pc, lim4);
		if(!nb->n || left < nb->n) goto step;
	}
enter:
	cblk = nb;
	c = nb->c;
	left -= nb->n;
	goto *c->h;

/* El final del bloque: al siguiente por el enlace guardado (los bloques
   enlazados nunca tienen n = 0, así que left >= n también dice left > 0).
   Con cpu_verificar, siempre por la búsqueda, que es donde se comprueba. */
link_end:
	nb = cblk->succ[0];
	if(likely(nb && nb->alive && left >= nb->n && !verif)) goto enter;
	npc = pc + 4;
	if(!left) goto done;
	nb = lookup(pc);
	if(nb && nb->n) cblk->succ[0] = nb;
	goto dispatch;
link:
	{
		const int k = pc != cblk->end;
		nb = cblk->succ[k];
		if(likely(nb && nb->pc == pc && nb->alive && left >= nb->n && !verif)) goto enter;
		npc = pc + 4;
		if(!left) goto done;
		nb = lookup(pc);
		if(nb && nb->n) cblk->succ[k] = nb;
	}
	goto dispatch;

/* Una instrucción con el intérprete de siempre */
step:
	cpu.pc = pc;
	cpu.npc = npc;
	cpu_cycles = end - left;
	cpu_executed += seg - left;
	seg = left;
	st.pasos++;
	cpu_run_interp(1);
	pc = cpu.pc;
	npc = cpu.npc;
	if(cpu_stop_requested || cpu_cycles >= end) goto ret;
	left = seg = (u32)(end - cpu_cycles);
	goto dispatch;

/* Se sale del bloque tras la instrucción de la tarjeta c (pidió parar o
   se reescribió el bloque) */
x_after: {
		u32 i_ = IDX();
		if(cblk->branch && i_ == BN - 1){ pc = bt; npc = bt + 4; }
		else if(cblk->branch && i_ == BN - 2){ pc = cblk->pc + 4 * i_ + 4; npc = bt; }
		else { pc = cblk->pc + 4 * i_ + 4; npc = pc + 4; }
		left += BN - (i_ + 1);
	}
block_exit:
	if(cpu_stop_requested){
		cpu.pc = pc;
		cpu.npc = npc;
		cpu_cycles = end - left;
		cpu_executed += seg - left;
		goto ret;
	}
	goto dispatch;

/* Branch likely no tomado: el delay slot no se ejecuta ni cuenta */
x_likely_nt:
	pc = cblk->end;
	left++;
	goto link_end;

after_call:
	/* Vuelta del HLE, como en cpu_run */
	pc = cpu.pc;
	npc = cpu.npc;
	cpu.r[0] = 0;
	cpu_cycles++;
	cpu_executed++;
	if(cpu_stop_requested || cpu_cycles >= end) goto ret;
	left = seg = (u32)(end - cpu_cycles);
	goto dispatch;

done:
	cpu.pc = pc;
	cpu.npc = npc;
	cpu_cycles = end;
	cpu_executed += seg;
ret:
	return (u32)(cpu_cycles - start);

	/* --- Las tarjetas ------------------------------------------------- */

	OP(NOP, (void)0)
	OP(SLL, G(D) = G(T) << SAF)
	OP(SRL, G(D) = G(T) >> SAF)
	OP(ROTR, { u32 v = G(T); G(D) = (v >> SAF) | (v << (32 - SAF)); })
	OP(SRA, G(D) = (u32)((s32)G(T) >> SAF))
	OP(SLLV, G(D) = G(T) << (G(S) & 31))
	OP(SRLV, G(D) = G(T) >> (G(S) & 31))
	OP(ROTRV, { u32 v = G(T), sh = G(S) & 31; G(D) = sh ? (v >> sh) | (v << (32 - sh)) : v; })
	OP(SRAV, G(D) = (u32)((s32)G(T) >> (G(S) & 31)))
	OP(MOVZ, if(G(T) == 0) G(D) = G(S))
	OP(MOVN, if(G(T) != 0) G(D) = G(S))
	OP(MFHI, G(D) = cpu.hi)
	OP(MTHI, cpu.hi = G(S))
	OP(MFLO, G(D) = cpu.lo)
	OP(MTLO, cpu.lo = G(S))
	OP(CLZ, { u32 v = G(S); G(D) = v ? (u32)__builtin_clz(v) : 32; })
	OP(CLO, { u32 v = ~G(S); G(D) = v ? (u32)__builtin_clz(v) : 32; })
	OP(MULT, { s64 m = (s64)(s32)G(S) * (s32)G(T); cpu.lo = (u32)m; cpu.hi = (u32)((u64)m >> 32); })
	OP(MULTU, { u64 m = (u64)G(S) * G(T); cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); })
	OP(DIV, {
		s32 a = (s32)G(S), b = (s32)G(T);
		if(a == (s32)0x80000000 && b == -1){ cpu.lo = 0x80000000u; cpu.hi = 0; }
		else if(b != 0){ cpu.lo = (u32)(a / b); cpu.hi = (u32)(a % b); }
		else { cpu.lo = a < 0 ? 1 : 0xFFFFFFFFu; cpu.hi = (u32)a; }
	})
	OP(DIVU, {
		u32 a = G(S), b = G(T);
		if(b != 0){ cpu.lo = a / b; cpu.hi = a % b; }
		else { cpu.lo = a <= 0xFFFF ? 0xFFFF : 0xFFFFFFFFu; cpu.hi = a; }
	})
	OP(MADD, { u64 acc = ((u64)cpu.hi << 32) | cpu.lo; acc += (u64)((s64)(s32)G(S) * (s32)G(T)); cpu.lo = (u32)acc; cpu.hi = (u32)(acc >> 32); })
	OP(MADDU, { u64 acc = ((u64)cpu.hi << 32) | cpu.lo; acc += (u64)G(S) * G(T); cpu.lo = (u32)acc; cpu.hi = (u32)(acc >> 32); })
	OP(MSUB, { u64 acc = ((u64)cpu.hi << 32) | cpu.lo; acc -= (u64)((s64)(s32)G(S) * (s32)G(T)); cpu.lo = (u32)acc; cpu.hi = (u32)(acc >> 32); })
	OP(MSUBU, { u64 acc = ((u64)cpu.hi << 32) | cpu.lo; acc -= (u64)G(S) * G(T); cpu.lo = (u32)acc; cpu.hi = (u32)(acc >> 32); })
	OP(ADDU, G(D) = G(S) + G(T))
	OP(SUBU, G(D) = G(S) - G(T))
	OP(AND, G(D) = G(S) & G(T))
	OP(OR, G(D) = G(S) | G(T))
	OP(XOR, G(D) = G(S) ^ G(T))
	OP(NOR, G(D) = ~(G(S) | G(T)))
	OP(SLT, G(D) = (s32)G(S) < (s32)G(T))
	OP(SLTU, G(D) = G(S) < G(T))
	OP(MAX, { u32 a = G(S), b = G(T); G(D) = (s32)a > (s32)b ? a : b; })
	OP(MIN, { u32 a = G(S), b = G(T); G(D) = (s32)a < (s32)b ? a : b; })

	OP(ADDIU, G(IT) = G(IS) + SIMM)
	OP(SLTI, G(IT) = (s32)G(IS) < (s32)SIMM)
	OP(SLTIU, G(IT) = G(IS) < SIMM)
	OP(ANDI, G(IT) = G(IS) & ZIMM)
	OP(ORI, G(IT) = G(IS) | ZIMM)
	OP(XORI, G(IT) = G(IS) ^ ZIMM)
	OP(LUI, G(IT) = ZIMM << 16)

	OP(EXT, {
		u32 size = SAF, mask = size >= 32 ? 0xFFFFFFFFu : (1u << size) - 1;
		G(T) = (G(S) >> D) & mask;
	})
	OP(INS, {
		u32 size = SAF, pos = D;
		u32 mask = (size >= 32 ? 0xFFFFFFFFu : (1u << size) - 1) << pos;
		G(T) = (G(T) & ~mask) | ((G(S) << pos) & mask);
	})
	OP(WSBH, { u32 v = G(T); G(D) = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); })
	OP(WSBW, G(D) = __builtin_bswap32(G(T)))
	OP(SEB, G(D) = (u32)(s32)(s8)G(T))
	OP(SEH, G(D) = (u32)(s32)(s16)G(T))
	OP(BITREV, { u32 v = G(T), b = 0, i; for(i = 0; i < 32; i++, v >>= 1) b = (b << 1) | (v & 1); G(D) = b; })

	OP(LB, { u32 v; LD8(v, G(IS) + SIMM, G(IT) = (u32)(s32)(s8)v); })
	OP(LH, { u32 v; LD16(v, G(IS) + SIMM, G(IT) = (u32)(s32)(s16)v); })
	OP(LW, { u32 v; LD32(v, G(IS) + SIMM, G(IT) = v); })
	OP(LBU, { u32 v; LD8(v, G(IS) + SIMM, G(IT) = v); })
	OP(LHU, { u32 v; LD16(v, G(IS) + SIMM, G(IT) = v); })
	OP(LWL, {
		u32 addr = G(IS) + SIMM, sh = (addr & 3) * 8, mem, rt = G(IT);
		LD32(mem, addr & ~3u, G(IT) = (rt & (0x00FFFFFFu >> sh)) | (mem << (24 - sh)));
	})
	OP(LWR, {
		u32 addr = G(IS) + SIMM, sh = (addr & 3) * 8, mem, rt = G(IT);
		LD32(mem, addr & ~3u, G(IT) = (rt & ~(0xFFFFFFFFu >> sh)) | (mem >> sh));
	})
	OP(LL, { u32 v; LD32(v, G(IS) + SIMM, { G(IT) = v; cpu.llbit = 1; }); })
	OP(SB, ST8(G(IS) + SIMM, G(IT), (void)0))
	OP(SH, ST16(G(IS) + SIMM, G(IT), (void)0))
	OP(SW, ST32(G(IS) + SIMM, G(IT), (void)0))
	/* swl/swr: leer y escribir; si la lectura pide parar, la escritura se
	   hace igual (como en cpu_run) y se sale después */
#define SWLR(name, merge) L_##name: { \
		const void *n_ = c[1].h; \
		u32 addr = G(IS) + SIMM, sh = (addr & 3) * 8, mem, rt = G(IT), a_ = addr & ~3u, o_ = RAM_OFF(a_); \
		int chk = 0; \
		if(likely(o_ < lim4)) mem = rd_le32(ram + o_); \
		else { SYNC_AT(); mem = cpu_slow_load(a_, 4, RAW()); chk = cpu_stop_requested; } \
		mem = merge; \
		if(likely(o_ < lim4)){ \
			wr_le32(ram + o_, mem); \
			if(unlikely(cpu_code_map[o_ >> PREDEC_ZONA_BITS])){ cpu_predec_store(o_, 4); chk |= !cblk->alive; } \
		} else { SYNC_AT(); cpu_slow_store(a_, 4, mem, RAW()); chk |= cpu_stop_requested; } \
		if(unlikely(chk)) goto x_after; \
		c++; \
		goto *n_; \
	}
	SWLR(SWL, (mem & ~(0xFFFFFFFFu >> (24 - sh))) | (rt >> (24 - sh)))
	SWLR(SWR, (mem & ~(0xFFFFFFFFu << sh)) | (rt << sh))
#undef SWLR
	OP(SC, {
		const u32 base = G(IS);
		if(cpu.llbit) ST32(base + SIMM, G(IT), G(IT) = (u32)cpu.llbit);
		else G(IT) = (u32)cpu.llbit;
	})
	OP(LWC1, { u32 v; LD32(v, G(IS) + SIMM, FR(IT) = v); })
	OP(SWC1, ST32(G(IS) + SIMM, FR(IT), (void)0))
	OP(CACHEWB, {
		mem_note_write((G(IS) + SIMM) & ~63u, 64);
		if(unlikely(!cblk->alive)) goto x_after;
	})

	OP(MFC1, G(T) = FR(S))
	OP(MTC1, FR(D) = G(T))
	OP(CFC1, G(T) = cpu.fcr31)
	OP(CTC1, { cpu.fcr31 = G(T) & FCR31_WRITABLE; cpu.fcc_hazard_pc = 0; })
	OP(FPU, cpu_cop1_s(RAW(), IPC()))
	OP(CVTSW, { float f = (float)(s32)FR(S); memcpy(f8 + D, &f, 4); })
	OP(MFIC, G(T) = (u32)hle_get_intr_enabled())
	OP(MTIC, {
		SYNC_AT();
		hle_set_intr_enabled(G(T) & 1);
		if(unlikely(cpu_stop_requested)) goto x_after;
	})
	OP(VFPU, {
		SYNC_AT();
		vfpu_exec(RAW(), IPC());
		if(unlikely(cpu_stop_requested || !cblk->alive)) goto x_after;
	})
	L_SYSCALL:
		cpu.llbit = 0;
		SYNC_AT();
		hle_syscall(c->a.w);
		goto after_call;

	BRANCH(BEQ, G(S) == G(T))
	BRANCH(BNE, G(S) != G(T))
	BRANCH(BLEZ, (s32)G(S) <= 0)
	BRANCH(BGTZ, (s32)G(S) > 0)
	BRANCH(BLTZ, (s32)G(S) < 0)
	BRANCH(BGEZ, (s32)G(S) >= 0)
	OP(BLTZAL, { s32 v = (s32)G(S); G(31 * 4) = cblk->end; bt = v < 0 ? cblk->target : cblk->end; })
	OP(BGEZAL, { s32 v = (s32)G(S); G(31 * 4) = cblk->end; bt = v >= 0 ? cblk->target : cblk->end; })
	BRANCH_LIKELY(BEQL, G(S) == G(T))
	BRANCH_LIKELY(BNEL, G(S) != G(T))
	BRANCH_LIKELY(BLEZL, (s32)G(S) <= 0)
	BRANCH_LIKELY(BGTZL, (s32)G(S) > 0)
	BRANCH_LIKELY(BLTZL, (s32)G(S) < 0)
	BRANCH_LIKELY(BGEZL, (s32)G(S) >= 0)
	L_BLTZALL: { s32 v = (s32)G(S); G(31 * 4) = cblk->end; if(v < 0){ const void *n_ = c[1].h; bt = cblk->target; c++; goto *n_; } goto x_likely_nt; }
	L_BGEZALL: { s32 v = (s32)G(S); G(31 * 4) = cblk->end; if(v >= 0){ const void *n_ = c[1].h; bt = cblk->target; c++; goto *n_; } goto x_likely_nt; }
	OP(J, bt = cblk->target)
	OP(JAL, { G(31 * 4) = cblk->end; bt = cblk->target; })
	OP(JR, bt = G(S))
	OP(JALR, { u32 v = G(S); G(D) = cblk->end; bt = v; })
	L_BC1: {
		int cond = (cpu.fcr31 & FCR31_COND) != 0, taken;
		if(cblk->end - 8 == cpu.fcc_hazard_pc) cond = cpu.fcc_old;
		cpu.fcc_hazard_pc = 0;
		taken = (SAF & 1) ? cond : !cond;
		if(SAF & 2){
			if(taken){ const void *n_ = c[1].h; bt = cblk->target; c++; goto *n_; }
			goto x_likely_nt;
		}
		{ const void *n_ = c[1].h; bt = taken ? cblk->target : cblk->end; c++; goto *n_; }
	}
	L_BV: {
		int val, taken, chk = 0;
		if(unlikely(!vfpu_live)){ SYNC_AT(); hle_vfpu_load(); chk = cpu_stop_requested; }
		vfpu_stat_bv++;
		val = (vfpu.ctrl[VFPU_CTRL_CC] >> D) & 1;
		taken = val == (int)(SAF & 1);
		if(SAF & 2){
			if(!taken){
				if(unlikely(chk)){ pc = cblk->end; npc = pc + 4; left++; goto block_exit; }
				goto x_likely_nt;
			}
			bt = cblk->target;
		} else bt = taken ? cblk->target : cblk->end;
		if(unlikely(chk)) goto x_after;
		{ const void *n_ = c[1].h; c++; goto *n_; }
	}

	L_END:
		pc = cblk->end;
		goto link_end;
	L_BEND:
		pc = bt;
		goto link;

#undef G
#undef FR
#undef D
#undef S
#undef T
#undef SAF
#undef IS
#undef IT
#undef SIMM
#undef ZIMM
#undef CB
#undef BN
#undef IDX
#undef RAW
#undef IPC
#undef OP
#undef SYNC_AT
#undef RAM_OFF
#undef LDX
#undef LD32
#undef LD16
#undef LD8
#undef STX
#undef ST32
#undef ST16
#undef ST8
#undef BRANCH
#undef BRANCH_LIKELY
}
