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
#include "core/prof.h"   //arreglo
#include "hle/hle.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define BLOQUE_MAX  64
#define TABLA_BITS  14
#define ARENA_BYTES (3u << 20)   /* la de los bloques */   //arreglo
#define FRIO_BYTES  (3u << 19)   /* la de lo que casi nunca se lee (predec_compacto) */   //arreglo

u8 cpu_code_map[(64u << 20) >> PREDEC_ZONA_BITS];

typedef struct {
	const void *h;              /* el código que la ejecuta */
	union {
		struct { u8 d, s, t, sa; } r;   /* registros como desplazamientos en bytes */
		struct { u8 s, t; s16 imm; } i;
		u32 w;
	} a;
} Card;

//arreglo
/* Lo que se mira al ejecutar y al pasar de un bloque a otro: 24 bytes en el
   Wii, así que con el bloque alineado a 32 la cabecera y la primera tarjeta
   llenan una línea de la caché */
typedef struct Block {
	u32 pc;        /* la primera instrucción */
	u32 end;       /* la que sigue a la última */
	u32 target;    /* destino fijo del salto (j, jal, b*) */
	struct Block *succ[2];
	u16 n;         /* instrucciones (0: las ejecuta el intérprete de siempre) */
	u8 alive;
	u8 branch;     /* acaba en un salto y su delay slot */
	Card c[];      /* n tarjetas y la de cierre (su a.w dice dónde está lo frío) */
} Block;

//arreglo
/* Lo que casi nunca se lee: las listas, las zonas y las instrucciones (para
   la memoria lenta, los fallos y cpu_verificar). Con predec_compacto va en
   su propia arena; sin él, justo detrás de las tarjetas (como antes) */
typedef struct {
	Block *hnext, *znext[2];
	u32 z0, z1;    /* zonas de 1 KB que ocupa */
	u32 raw[];
} Frio;

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
	OP_LVS, OP_SVS, OP_LVQ, OP_SVQ, OP_LVQT, OP_SVQT, OP_MFV, OP_MTV, OP_VFPU_V,   //Arreglo
	OP_VADD, OP_VSUB, OP_VMUL, OP_VMOV, OP_VABS, OP_VNEG, OP_VMIN, OP_VMAX, OP_VSCL, OP_VI2F, OP_VF2I, OP_VPFX,   //Arreglo
	OP_END, OP_BEND, OP_BENDD,   //Arreglo
	OP_N
};

static const void *h_tab[OP_N];

//Arreglo
static union {
	Block b;
	u8 sitio[sizeof(Block) + 2 * sizeof(Card)];
} nulo_u __attribute__((aligned(32))) = { .b = { .pc = 0xFFFFFFFFu, .n = 0xFFFF, .alive = 0 } };
#define NULO (&nulo_u.b)

/* --- Arena, tabla y zonas ------------------------------------------------- */

static u8 *arena;
static u32 arena_used;
static Block *tabla[1u << TABLA_BITS];
static Block **zona_head;        /* lista de bloques de cada zona de 1 KB */
static u32 zonas;
static PredecStats st;
//arreglo
static u8 *arena_propia, *frio_arena, *frio_base;   /* frio_base: donde cuenta c[n].a.w */
static u32 frio_used, arena_tam = ARENA_BYTES, frio_tam = FRIO_BYTES, frio_hecha;
static u8 compacto, conf = 0xFF;
static u8 *reserva;              /* la de MEM1 (predec_mem1), si el anfitrión la dio */
static u32 reserva_tam;
static int copia = -1;           /* qué copia del bucle llenó h_tab */
static u8 medir;   //Arreglo
static u8 vt_on, vt_verif, vt_arit;   //Arreglo
static u8 en_on, sc_on;   //Arreglo

#define FRIO(b) ((Frio *)(frio_base + (b)->c[(b)->n].a.w))   //arreglo

static inline u32 slot(u32 pc){ return (pc >> 2) & ((1u << TABLA_BITS) - 1); }

static inline Block *lookup(u32 pc){
	Block *b = tabla[slot(pc)];
	while(b && b->pc != pc) b = FRIO(b)->hnext;   //arreglo
	return b;
}

static void unhash(Block *b){
	Block **pp = &tabla[slot(b->pc)];
	while(*pp && *pp != b) pp = &FRIO(*pp)->hnext;   //arreglo
	if(*pp) *pp = FRIO(b)->hnext;   //arreglo
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
		Frio *f = FRIO(b);   //arreglo
		Block *nx = f->znext[f->z0 == z ? 0 : 1];   //arreglo
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
	frio_used = 0;   //arreglo
}

void cpu_predec_stats(PredecStats *out){
	*out = st;
	out->arena_usada = arena_used;
	out->arena_total = arena ? arena_tam : 0;   //arreglo
	//arreglo
	out->frio_usada = compacto ? frio_used : 0;
	out->frio_total = compacto && frio_arena ? frio_tam : 0;
	out->dir_arena = (uintptr_t)arena;
	out->dir_frio = compacto ? (uintptr_t)frio_arena : 0;
	out->dir_tabla = (uintptr_t)tabla;
	out->dir_mapa = (uintptr_t)cpu_code_map;
	out->compacto = compacto;
}

//arreglo
void cpu_predec_reserva(void *p, u32 bytes){
	reserva = (u8 *)p;
	reserva_tam = bytes;
	conf = 0xFF;
}

//arreglo
void cpu_predec_tam_pruebas(u32 bloques, u32 frio){
	arena_tam = bloques ? bloques : ARENA_BYTES;
	frio_tam = frio ? frio : FRIO_BYTES;
	conf = 0xFF;
}

//arreglo
/* Para las pruebas (ver predec.h) */
int cpu_predec_tarjetas_ok(void){
	u32 i, k, j;
	Block *b;
	if(copia != (opt_on[OPT_PREDEC_SIN_UNIR] | (opt_on[OPT_PROF_CPU] | opt_on[OPT_PROF_BLOQUES]) << 1)) return 0;   //Arreglo
	for(i = 0; i < (1u << TABLA_BITS); i++)
		for(b = tabla[i]; b; b = FRIO(b)->hnext)
			for(k = 0; k <= b->n; k++){
				for(j = 0; j < OP_N && b->c[k].h != h_tab[j]; j++) ;
				if(j == OP_N) return 0;
			}
	return 1;
}

//Arreglo
int cpu_predec_enlaces_ok(void){
	u32 i, k;
	Block *b;
	for(i = 0; i < (1u << TABLA_BITS); i++)
		for(b = tabla[i]; b; b = FRIO(b)->hnext){
			if(b->c[b->n].h != h_tab[OP_BENDD]) continue;
			for(k = 0; k < 2; k++){
				const Block *s = b->succ[k];
				if(s == NULO) continue;
				if((const u8 *)s < arena || (const u8 *)s >= arena + arena_used) return 0;
				if(s->pc != (k ? b->target : b->end)) return 0;
			}
		}
	return NULO->alive == 0 && NULO->pc == 0xFFFFFFFFu;
}

//arreglo
/* Las arenas según predec_compacto y predec_mem1; si cambian, todo fuera */
static int predec_config(void){
	u8 want = (u8)(opt_on[OPT_PREDEC_COMPACTO] | (opt_on[OPT_PREDEC_MEM1] && reserva ? 2 : 0) |
	               (opt_on[OPT_CPU_VFPU_TARJETAS] ? 4 : 0) | (opt_on[OPT_CPU_VFPU_VERIFICAR] ? 8 : 0) |
	               (opt_on[OPT_CPU_VFPU_ARIT] ? 16 : 0) | (opt_on[OPT_CPU_ENLACE] ? 32 : 0) |
	               (opt_on[OPT_CPU_SYSCALL_SLOT] ? 64 : 0));   //Arreglo
	if(likely(want == conf)) return arena != NULL;
	conf = want;
	vt_on = (want >> 2) & 1;   //Arreglo
	vt_verif = (want >> 3) & 1;   //Arreglo
	vt_arit = (want >> 4) & 1;   //Arreglo
	en_on = (want >> 5) & 1;   //Arreglo
	sc_on = (want >> 6) & 1;   //Arreglo
	if(!zona_head){
		zonas = psp_mem.ram_size >> PREDEC_ZONA_BITS;
		if(zonas > sizeof(cpu_code_map)) return 0;
		zona_head = calloc(zonas, sizeof(*zona_head));
	}
	free(arena_propia);
	arena_propia = NULL;
	if((want & 2) && reserva_tam >= arena_tam) arena = reserva;
	else {
		arena_propia = malloc(arena_tam + 31);
		arena = arena_propia ? (u8 *)(((uintptr_t)arena_propia + 31) & ~(uintptr_t)31) : NULL;
	}
	compacto = want & 1;
	if(compacto && (!frio_arena || frio_hecha != frio_tam)){
		free(frio_arena);
		frio_arena = malloc(frio_tam);
		frio_hecha = frio_arena ? frio_tam : 0;
	}
	frio_base = compacto ? frio_arena : arena;
	cpu_predec_flush();
	if(!arena || !zona_head || (compacto && !frio_arena)){ arena = NULL; conf = 0xFF; return 0; }
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

//Arreglo
static int decode_vfpu(u32 w, Card *cd){
	const u32 op = w >> 26, rs = RS_(w), rt = RT_(w);
	switch(op){
	case 0x32: case 0x3A:
		card_i(cd, op == 0x32 ? OP_LVS : OP_SVS, rs, 0, w & 0xFFFC);
		cd->a.i.t = (u8)VFPU_INDEX(rt | ((w & 3) << 5));
		return op == 0x32 ? OP_LVS : OP_SVS;
	case 0x36: case 0x3E: {
		const int k = (w & 1) ? (op == 0x36 ? OP_LVQT : OP_SVQT) : (op == 0x36 ? OP_LVQ : OP_SVQ);
		card_i(cd, k, rs, 0, w & 0xFFFC);
		cd->a.i.t = (u8)(((rt << 2) & 0x70) + ((w & 1) ? (rt & 3) : (rt & 3) * 4));
		return k;
	}
	case 0x12:
		if(w & 0x80) return 0;
		if(rs == 3 && rt){ card_i(cd, OP_MFV, 0, rt, (u32)VFPU_INDEX(w & 0x7F) * 4); return OP_MFV; }
		if(rs == 7){ card_i(cd, OP_MTV, rt, 0, (u32)VFPU_INDEX(w & 0x7F) * 4); return OP_MTV; }
		return 0;
	}
	if(vt_arit){   //Arreglo
		int k = 0;   //Arreglo
		const u32 sub = (w >> 23) & 7, sub21 = (w >> 21) & 0x1F, sub16 = (w >> 16) & 0x1F;   //Arreglo
		switch(op){   //Arreglo
		case 0x18: k = sub == 0 ? OP_VADD : sub == 1 ? OP_VSUB : 0; break;   //Arreglo
		case 0x19: k = sub == 0 ? OP_VMUL : sub == 2 ? OP_VSCL : 0; break;   //Arreglo
		case 0x1B: k = sub == 2 ? OP_VMIN : sub == 3 ? OP_VMAX : 0; break;   //Arreglo
		case 0x34:   //Arreglo
			if(sub21 == 0) k = sub16 == 0 ? OP_VMOV : sub16 == 1 ? OP_VABS : sub16 == 2 ? OP_VNEG : 0;   //Arreglo
			else if(sub21 >= 16 && sub21 <= 19) k = OP_VF2I;   //Arreglo
			else if(sub21 == 20) k = OP_VI2F;   //Arreglo
			break;   //Arreglo
		case 0x37: k = sub < 6 ? OP_VPFX : 0; break;   //Arreglo
		}   //Arreglo
		if(k){ card_w(cd, k, w); return k; }   //Arreglo
	}   //Arreglo
	return 0;
}

//Arreglo
static int vfpu_card(u32 w, Card *cd){
	if(!vt_on || !decode_vfpu(w, cd)) return 0;
	if(vt_verif) card_w(cd, OP_VFPU_V, w);
	return 1;
}

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
				card_w(cd, OP_FPU, w);   //arreglo
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
		if(vfpu_card(w, cd)) return K_NORMAL;   //Arreglo
		card_w(cd, OP_VFPU, w);   //arreglo
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
		if(vfpu_card(w, cd)) return K_NORMAL;   //Arreglo
		card_w(cd, OP_VFPU, w);   //arreglo
		return K_NORMAL;
	default: return K_PARA;
	}
}

//arreglo
/* Con predec_compacto, el bloque alineado a 32 en su arena y lo frío en la
   otra; sin él, lo frío justo detrás de las tarjetas y todo alineado a 8 */
static Block *alloc_block(u32 n, Frio **fp){
	u32 hot = (u32)(sizeof(Block) + (n + 1) * sizeof(Card));
	u32 cold = (u32)(sizeof(Frio) + n * 4 + 7) & ~7u;
	Block *b;
	Frio *f;
	if(compacto){
		hot = (hot + 31) & ~31u;
		if(arena_used + hot > arena_tam || frio_used + cold > frio_tam) cpu_predec_flush();
		b = (Block *)(arena + arena_used);
		f = (Frio *)(frio_arena + frio_used);
		b->c[n].a.w = frio_used;
		arena_used += hot;
		frio_used += cold;
	} else {
		hot = (hot + 7) & ~7u;
		if(arena_used + hot + cold > arena_tam) cpu_predec_flush();
		b = (Block *)(arena + arena_used);
		f = (Frio *)(arena + arena_used + hot);
		b->c[n].a.w = arena_used + hot;
		arena_used += hot + cold;
	}
	memset(b, 0, sizeof(*b));
	b->succ[0] = b->succ[1] = NULO;   //Arreglo
	memset(f, 0, sizeof(*f));
	*fp = f;
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
	Frio *f;   //arreglo

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
			{   //Arreglo
				const int kd = decode(wd, pc + n * 4 + 4, &cd, &td);   //Arreglo
				if(kd != K_NORMAL && !(kd == K_SYSCALL && sc_on)) break;   //Arreglo
			}   //Arreglo
			target = t;
			tmp[n + 1] = cd;
			raw[n + 1] = wd;
			n += 2;
			branch = 1;
			break;
		}
		n++;
	}

	b = alloc_block(n, &f);   //arreglo
	b->pc = pc;
	b->n = (u16)n;
	b->end = pc + n * 4;
	b->target = target;
	b->branch = (u8)branch;
	b->alive = 1;
	memcpy(b->c, tmp, n * sizeof(Card));
	memcpy(f->raw, raw, n * 4);   //arreglo
	/* La de cierre (un bloque que acaba en syscall no llega a ella) */
	if(en_on){   //Arreglo
		const int ind = branch && (tmp[n - 2].h == h_tab[OP_JR] || tmp[n - 2].h == h_tab[OP_JALR]);   //Arreglo
		b->c[n].h = h_tab[branch ? (ind ? OP_BEND : OP_BENDD) : OP_END];   //Arreglo
		if(branch && b->c[n - 1].h == h_tab[OP_NOP]) b->c[n - 1].h = b->c[n].h;   //Arreglo
	} else b->c[n].h = h_tab[branch ? OP_BEND : OP_END];   //Arreglo
	//arreglo
	f->z0 = o0 >> PREDEC_ZONA_BITS;
	f->z1 = (o0 + (n ? n * 4 - 1 : 0)) >> PREDEC_ZONA_BITS;
	f->hnext = tabla[slot(pc)];
	tabla[slot(pc)] = b;
	f->znext[0] = zona_head[f->z0];
	zona_head[f->z0] = b;
	cpu_code_map[f->z0] = 1;
	if(f->z1 != f->z0){
		f->znext[1] = zona_head[f->z1];
		zona_head[f->z1] = b;
		cpu_code_map[f->z1] = 1;
	}
	st.bloques_hechos++;
	return b;
}

/* cpu_verificar: ¿siguen en la RAM las instrucciones del bloque? */
static int verif_ok(const Block *b){
	const u8 *p = psp_mem.ram + ((b->pc & PSP_ADDR_MASK) - PSP_RAM_BASE);
	const u32 *raw = FRIO(b)->raw;   //arreglo
	u32 i;
	for(i = 0; i < b->n; i++){
		if(rd_le32(p + i * 4) != raw[i]){   //arreglo
			if(st.verif_viejos < 16)
				hle_log_quiet("[PREDEC] bloque viejo en %08X: tenia %08X y en la RAM hay %08X (falta un aviso de escritura)\n",
				              b->pc + i * 4, raw[i], rd_le32(p + i * 4));   //arreglo
			st.verif_viejos++;
			return 0;
		}
	}
	return 1;
}

/* --- Ejecutar ------------------------------------------------------------- */

//arreglo
/* La memoria lenta, contada y marcada para el muestreo */
static u32 lenta_load(u32 addr, u32 size, u32 instr){
	u32 v;
	if(!medir) return cpu_slow_load(addr, size, instr);   //Arreglo
	st.lentas++;
	prof_cpu_phase = CPF_LENTA;
	v = cpu_slow_load(addr, size, instr);
	prof_cpu_phase = CPF_TARJETAS;
	return v;
}
static void lenta_store(u32 addr, u32 size, u32 v, u32 instr){
	if(!medir){ cpu_slow_store(addr, size, v, instr); return; }   //Arreglo
	st.lentas++;
	prof_cpu_phase = CPF_LENTA;
	cpu_slow_store(addr, size, v, instr);
	prof_cpu_phase = CPF_TARJETAS;
}

#define VFR(off) (*(float *)((u8 *)vfpu.v + (off)))   //Arreglo

//Arreglo
static inline __attribute__((always_inline)) int vfpu_arit(int k, u32 w){
	const int n = (int)(((w >> 7) & 1) + ((w >> 14) & 2) + 1);
	const u16 *xs = vfpu_vidx[n][(w >> 8) & 0x7F], *xd = vfpu_vidx[n][w & 0x7F];
	union { float f[4]; u32 u[4]; s32 i[4]; } s, d;
	int i;
	if(k == OP_VPFX){
		const int r = (int)((w >> 24) & 3);
		vfpu.ctrl[VFPU_CTRL_SPREFIX + r] = r == VFPU_CTRL_DPREFIX ? w & 0xFFF : w & 0xFFFFF;
		return 1;
	}
	if(((vfpu.ctrl[VFPU_CTRL_SPREFIX] ^ 0xE4) | (vfpu.ctrl[VFPU_CTRL_TPREFIX] ^ 0xE4) | vfpu.ctrl[VFPU_CTRL_DPREFIX]) || !vfpu_fast_paths)
		return 0;
	switch(k){
	case OP_VADD: case OP_VSUB: case OP_VMUL: {
		const u16 *xt = vfpu_vidx[n][(w >> 16) & 0x7F];
		for(i = 0; i < n; i++){
			const float a = vfpu_flush(VFR(xs[i])), b = vfpu_flush(VFR(xt[i]));
			d.f[i] = vfpu_canon(k == OP_VADD ? a + b : k == OP_VSUB ? a - b : a * b);
		}
		break;
	}
	case OP_VMOV: for(i = 0; i < n; i++) d.f[i] = VFR(xs[i]); break;
	case OP_VABS: for(i = 0; i < n; i++) d.f[i] = fabsf(VFR(xs[i])); break;
	case OP_VNEG: for(i = 0; i < n; i++) d.f[i] = -VFR(xs[i]); break;
	case OP_VMIN: case OP_VMAX: {
		const u16 *xt = vfpu_vidx[n][(w >> 16) & 0x7F];
		for(i = 0; i < n; i++)
			d.u[i] = vfpu_minmax_bits(VFR(xs[i]), VFR(xt[i]), *(const u32 *)((const u8 *)vfpu.vi + xs[i]),
			                          *(const u32 *)((const u8 *)vfpu.vi + xt[i]), k == OP_VMAX);
		break;
	}
	case OP_VSCL: {
		const float m = vfpu.v[VFPU_INDEX((w >> 16) & 0x7F)];
		for(i = 0; i < n; i++) d.f[i] = VFR(xs[i]) * m;
		break;
	}
	case OP_VI2F: {
		const float m = 1.0f / (float)(1u << ((w >> 16) & 0x1F));
		for(i = 0; i < n; i++) s.f[i] = VFR(xs[i]);
		for(i = 0; i < n; i++) d.f[i] = (float)s.i[i] * m;
		break;
	}
	case OP_VF2I: {
		const float m = (float)(1u << ((w >> 16) & 0x1F));
		const int mode = (int)((w >> 21) & 0x1F);
		for(i = 0; i < n; i++) s.f[i] = VFR(xs[i]);
		for(i = 0; i < n; i++) d.u[i] = vfpu_f2i_lane(s.f[i], m, mode);
		break;
	}
	default: return 0;
	}
	for(i = n - 1; i >= 0; i--) VFR(xd[i]) = d.f[i];
	return 1;
}

//Arreglo
static inline __attribute__((always_inline)) int vfpu_tarjeta(int k, const Card *c, u8 *r8, u8 *ram, u32 lim4){
	u32 a, o, *v;
	if(unlikely(!vfpu_live)) return 0;
	if(k >= OP_VADD) return vfpu_arit(k, c->a.w);   //Arreglo
	if(k == OP_MFV){ *(u32 *)(r8 + c->a.i.t) = *(const u32 *)((const u8 *)vfpu.vi + (u16)c->a.i.imm); return 1; }
	if(k == OP_MTV){ *(u32 *)((u8 *)vfpu.vi + (u16)c->a.i.imm) = *(const u32 *)(r8 + c->a.i.s); return 1; }
	a = *(const u32 *)(r8 + c->a.i.s) + (u32)(s32)c->a.i.imm;
	o = (a & PSP_ADDR_MASK) - PSP_RAM_BASE;
	switch(k){
	case OP_LVS:
		if((a & 3) || o >= lim4) return 0;
		vfpu.vi[c->a.i.t] = rd_le32(ram + o);
		return 1;
	case OP_SVS:
		if((a & 3) || o >= lim4) return 0;
		wr_le32(ram + o, vfpu.vi[c->a.i.t]);
		if(unlikely(cpu_code_map[o >> PREDEC_ZONA_BITS])){ cpu_predec_store(o, 4); return 2; }
		return 1;
	case OP_LVQ: case OP_LVQT: {
		const u32 p = k == OP_LVQ ? 1 : 4;
		if((a & 15) || o >= lim4 - 12 || (vfpu.ctrl[VFPU_CTRL_DPREFIX] & 0xF00)) return 0;
		v = vfpu.vi + c->a.i.t;
		v[0] = rd_le32(ram + o);
		v[p] = rd_le32(ram + o + 4);
		v[2 * p] = rd_le32(ram + o + 8);
		v[3 * p] = rd_le32(ram + o + 12);
		return 1;
	}
	case OP_SVQ: case OP_SVQT: {
		const u32 p = k == OP_SVQ ? 1 : 4;
		if((a & 15) || o >= lim4 - 12) return 0;
		v = vfpu.vi + c->a.i.t;
		wr_le32(ram + o, v[0]);
		wr_le32(ram + o + 4, v[p]);
		wr_le32(ram + o + 8, v[2 * p]);
		wr_le32(ram + o + 12, v[3 * p]);
		if(unlikely(cpu_code_map[o >> PREDEC_ZONA_BITS])){ cpu_predec_store(o, 16); return 2; }
		return 1;
	}
	}
	return 0;
}

//Arreglo
static void vfpu_verificar_una(u32 w, u32 pc, u32 lim4){
	static VfpuState v0, v1;
	u32 r0[32], r1[32], a, o = 0, n = 0;
	u8 m0[16], m1[16];
	Card t;
	const int k = decode_vfpu(w, &t);
	int res;
	if(k == OP_SVS || k == OP_SVQ || k == OP_SVQT){
		n = k == OP_SVS ? 4 : 16;
		a = cpu.r[RS_(w)] + (u32)(s32)(s16)(w & 0xFFFC);
		o = (a & PSP_ADDR_MASK) - PSP_RAM_BASE;
		if((a & (n - 1)) || o >= lim4 + 4 - n) n = 0;
		if(n) memcpy(m0, psp_mem.ram + o, n);
	}
	v0 = vfpu;
	memcpy(r0, cpu.r, sizeof(r0));
	res = k ? vfpu_tarjeta(k, &t, (u8 *)cpu.r, psp_mem.ram, lim4) : 0;
	if(res){
		v1 = vfpu;
		memcpy(r1, cpu.r, sizeof(r1));
		if(n){ memcpy(m1, psp_mem.ram + o, n); memcpy(psp_mem.ram + o, m0, n); }
		vfpu = v0;
		memcpy(cpu.r, r0, sizeof(r0));
	}
	vfpu_exec(w, pc);
	if(!res) return;
	st.vfpu_verif++;
	if(memcmp(&v1, &vfpu, sizeof(v1)) || memcmp(r1, cpu.r, sizeof(r1)) || (n && memcmp(m1, psp_mem.ram + o, n))){
		if(st.vfpu_verif_mal < 16)
			hle_log_quiet("[PREDEC] tarjeta VFPU distinta de vfpu_exec en %08X (%08X)\n", pc, w);
		st.vfpu_verif_mal++;
	}
}

//Arreglo
static u32 ras_pila[32], ras_cima, ras_hondo;

//Arreglo
static void ras_push(u32 vuelta){
	ras_pila[ras_cima++ & 31] = vuelta;
	if(ras_hondo < 32) ras_hondo++;
}

//Arreglo
static int ras_pop(u32 pc){
	if(!ras_hondo) return 0;
	ras_hondo--;
	return ras_pila[--ras_cima & 31] == pc;
}

//Arreglo
static u32 caida_clave(u32 w){
	const u32 op = w >> 26;
	if(op == 0x00) return 64 + FN_(w);
	if(op == 0x1F) return 128 + FN_(w);
	if(op == 0x11) return 192 + RS_(w);
	if(op == 0x01) return 224 + RT_(w);
	return op;
}

//Arreglo
static void caida(int motivo, u32 pc, u32 lim4){
	const u32 o = (pc & PSP_ADDR_MASK) - PSP_RAM_BASE;
	st.bq_caidas[motivo]++;
	if(!(pc & 3) && o < lim4) st.bq_caida_op[caida_clave(rd_le32(psp_mem.ram + o))]++;
}

//Arreglo
static inline u32 tam_bloque(u32 n){
	return n <= 1 ? 0 : n > 64 ? 6 : (u32)(32 - __builtin_clz(n - 1));   //Arreglo
}

//Arreglo
void cpu_predec_caida_nombre(u32 k, char *buf, u32 size){
	static const char *const prim[64] = {
		[0x02] = "j", [0x03] = "jal", [0x04] = "beq", [0x05] = "bne", [0x06] = "blez", [0x07] = "bgtz",
		[0x08] = "addi", [0x09] = "addiu", [0x0A] = "slti", [0x0B] = "sltiu", [0x0C] = "andi", [0x0D] = "ori",
		[0x0E] = "xori", [0x0F] = "lui", [0x10] = "cop0", [0x12] = "vfpu/bv", [0x14] = "beql", [0x15] = "bnel",
		[0x16] = "blezl", [0x17] = "bgtzl", [0x18] = "vfpu0", [0x19] = "vfpu1", [0x1B] = "vfpu3", [0x1C] = "special2",
		[0x20] = "lb", [0x21] = "lh", [0x22] = "lwl", [0x23] = "lw", [0x24] = "lbu", [0x25] = "lhu", [0x26] = "lwr",
		[0x28] = "sb", [0x29] = "sh", [0x2A] = "swl", [0x2B] = "sw", [0x2E] = "swr", [0x2F] = "cache",
		[0x30] = "ll", [0x31] = "lwc1", [0x32] = "lv.s", [0x34] = "vfpu4", [0x35] = "vfpu5", [0x36] = "lv.q",
		[0x37] = "vfpu6", [0x38] = "sc", [0x39] = "swc1", [0x3A] = "sv.s", [0x3C] = "vfpu7", [0x3D] = "lvl/lvr",
		[0x3E] = "sv.q", [0x3F] = "vflush"
	};
	static const char *const spec[64] = {
		[0x08] = "jr", [0x09] = "jalr", [0x0C] = "syscall", [0x0D] = "break", [0x0F] = "sync"
	};
	static const char *const regimm[4] = { "bltz", "bgez", "bltzl", "bgezl" };
	const char *nm = NULL;
	if(k < 64) nm = prim[k];
	else if(k < 128) nm = spec[k - 64];
	else if(k >= 224 && k - 224 < 4) nm = regimm[k - 224];
	if(nm) snprintf(buf, size, "%s", nm);
	else if(k < 64) snprintf(buf, size, "op%02X", (unsigned)k);
	else if(k < 128) snprintf(buf, size, "special.%02X", (unsigned)(k - 64));
	else if(k < 192) snprintf(buf, size, "special3.%02X", (unsigned)(k - 128));
	else if(k < 224) snprintf(buf, size, "cop1.rs%02X", (unsigned)(k - 192));
	else snprintf(buf, size, "regimm.%02X", (unsigned)(k - 224));
}

//arreglo
/* El bucle dos veces: el de siempre y uno sin unir los finales de las
   tarjetas (-fno-crossjumping): así cada tarjeta acaba en su propio mtctr
   y bctr (predec_sin_unir). Sin GCSE (-fno-gcse, lo que recomienda GCC
   con computed goto) cada tarjeta volvía a calcular direcciones: un 21 %
   más de instrucciones en el banco de qemu. */
#define PREDEC_BUCLE run_unido
#define PREDEC_COPIA 0
#define PREDEC_MEDIR 0   //Arreglo
#define PREDEC_ATRIB __attribute__((optimize("no-branch-count-reg")))   //Arreglo
#include "cpu/predec_bucle.h"
#undef PREDEC_BUCLE
#undef PREDEC_COPIA
#undef PREDEC_MEDIR   //Arreglo
#undef PREDEC_ATRIB
#define PREDEC_BUCLE run_sin_unir
#define PREDEC_COPIA 1
#define PREDEC_MEDIR 0   //Arreglo
#define PREDEC_ATRIB __attribute__((optimize("no-crossjumping", "no-branch-count-reg")))   //Arreglo
#include "cpu/predec_bucle.h"
#undef PREDEC_BUCLE
#undef PREDEC_COPIA
#undef PREDEC_MEDIR   //Arreglo
#undef PREDEC_ATRIB
//Arreglo
#define PREDEC_BUCLE run_unido_medir
#define PREDEC_COPIA 2
#define PREDEC_MEDIR 1
#define PREDEC_ATRIB __attribute__((optimize("no-branch-count-reg")))   //Arreglo
#include "cpu/predec_bucle.h"
#undef PREDEC_BUCLE
#undef PREDEC_COPIA
#undef PREDEC_MEDIR
#undef PREDEC_ATRIB
#define PREDEC_BUCLE run_sin_unir_medir
#define PREDEC_COPIA 3
#define PREDEC_MEDIR 1
#define PREDEC_ATRIB __attribute__((optimize("no-crossjumping", "no-branch-count-reg")))   //Arreglo
#include "cpu/predec_bucle.h"
#undef PREDEC_BUCLE
#undef PREDEC_COPIA
#undef PREDEC_MEDIR
#undef PREDEC_ATRIB

//arreglo
u32 cpu_run_predec(u32 max_cycles){
	if(cpu_stop_requested || !max_cycles) return 0;
	if(unlikely(!predec_config())) return cpu_run_interp(max_cycles);
	medir = opt_on[OPT_PROF_CPU] | opt_on[OPT_PROF_BLOQUES];   //Arreglo
	if(medir) return opt_on[OPT_PREDEC_SIN_UNIR] ? run_sin_unir_medir(max_cycles) : run_unido_medir(max_cycles);   //Arreglo
	return opt_on[OPT_PREDEC_SIN_UNIR] ? run_sin_unir(max_cycles) : run_unido(max_cycles);
}
