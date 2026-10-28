/**
 * WIISP - cpu/ppc_emit.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#ifndef WIISP_PPC_EMIT_H
#define WIISP_PPC_EMIT_H

#include "core/types.h"

//Arreglo
typedef struct {
	u32 *p, *ini, *fin;
	int lleno;
} Emit;

//Arreglo
static inline void e_ini(Emit *e, u32 *buf, u32 palabras){ e->p = e->ini = buf; e->fin = buf + palabras; e->lleno = 0; }
static inline void e_w(Emit *e, u32 w){ if(e->p < e->fin) *e->p++ = w; else e->lleno = 1; }
static inline u32 e_pos(const Emit *e){ return (u32)(e->p - e->ini); }

//Arreglo
#define PPC_D(op, d, a, imm)       (((u32)(op) << 26) | ((u32)(d) << 21) | ((u32)(a) << 16) | ((u32)(imm) & 0xFFFFu))
#define PPC_X(d, a, b, xo, rc)     ((31u << 26) | ((u32)(d) << 21) | ((u32)(a) << 16) | ((u32)(b) << 11) | ((u32)(xo) << 1) | (u32)(rc))
#define PPC_M(op, s, a, sh, mb, me, rc) (((u32)(op) << 26) | ((u32)(s) << 21) | ((u32)(a) << 16) | ((u32)(sh) << 11) | \
                                         ((u32)(mb) << 6) | ((u32)(me) << 1) | (u32)(rc))

//Arreglo
enum { BO_FALSO = 4, BO_CIERTO = 12, BO_SIEMPRE = 20 };
enum { CR_LT = 0, CR_GT = 1, CR_EQ = 2, CR_SO = 3 };

//Arreglo
static inline void e_addi(Emit *e, int d, int a, s32 imm){ e_w(e, PPC_D(14, d, a, imm)); }
static inline void e_addis(Emit *e, int d, int a, s32 imm){ e_w(e, PPC_D(15, d, a, imm)); }
static inline void e_li(Emit *e, int d, s32 imm){ e_addi(e, d, 0, imm); }
static inline void e_lis(Emit *e, int d, s32 imm){ e_addis(e, d, 0, imm); }
static inline void e_addic(Emit *e, int d, int a, s32 imm){ e_w(e, PPC_D(12, d, a, imm)); }
static inline void e_subfic(Emit *e, int d, int a, s32 imm){ e_w(e, PPC_D(8, d, a, imm)); }
static inline void e_ori(Emit *e, int a, int s, u32 imm){ e_w(e, PPC_D(24, s, a, imm)); }
static inline void e_oris(Emit *e, int a, int s, u32 imm){ e_w(e, PPC_D(25, s, a, imm)); }
static inline void e_xori(Emit *e, int a, int s, u32 imm){ e_w(e, PPC_D(26, s, a, imm)); }
static inline void e_xoris(Emit *e, int a, int s, u32 imm){ e_w(e, PPC_D(27, s, a, imm)); }
static inline void e_andi_(Emit *e, int a, int s, u32 imm){ e_w(e, PPC_D(28, s, a, imm)); }
static inline void e_andis_(Emit *e, int a, int s, u32 imm){ e_w(e, PPC_D(29, s, a, imm)); }
static inline void e_lwz(Emit *e, int d, s32 off, int a){ e_w(e, PPC_D(32, d, a, off)); }
static inline void e_stw(Emit *e, int s, s32 off, int a){ e_w(e, PPC_D(36, s, a, off)); }
static inline void e_lbz(Emit *e, int d, s32 off, int a){ e_w(e, PPC_D(34, d, a, off)); }
static inline void e_cmpwi(Emit *e, int cr, int a, s32 imm){ e_w(e, PPC_D(11, cr << 2, a, imm)); }
static inline void e_cmplwi(Emit *e, int cr, int a, u32 imm){ e_w(e, PPC_D(10, cr << 2, a, imm)); }

//Arreglo
static inline void e_add(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 266, 0)); }
static inline void e_addc(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 10, 0)); }
static inline void e_adde(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 138, 0)); }
static inline void e_subf(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 40, 0)); }
static inline void e_subfc(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 8, 0)); }
static inline void e_subfe(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 136, 0)); }
static inline void e_neg(Emit *e, int d, int a){ e_w(e, PPC_X(d, a, 0, 104, 0)); }
static inline void e_mullw(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 235, 0)); }
static inline void e_mulhw(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 75, 0)); }
static inline void e_mulhwu(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 11, 0)); }
static inline void e_and(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 28, 0)); }
static inline void e_andc(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 60, 0)); }
static inline void e_or(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 444, 0)); }
static inline void e_xor(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 316, 0)); }
static inline void e_nor(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 124, 0)); }
static inline void e_mr(Emit *e, int a, int s){ e_or(e, a, s, s); }
static inline void e_slw(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 24, 0)); }
static inline void e_srw(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 536, 0)); }
static inline void e_sraw(Emit *e, int a, int s, int b){ e_w(e, PPC_X(s, a, b, 792, 0)); }
static inline void e_srawi(Emit *e, int a, int s, int sh){ e_w(e, PPC_X(s, a, sh, 824, 0)); }
static inline void e_cntlzw(Emit *e, int a, int s){ e_w(e, PPC_X(s, a, 0, 26, 0)); }
static inline void e_extsb(Emit *e, int a, int s){ e_w(e, PPC_X(s, a, 0, 954, 0)); }
static inline void e_extsh(Emit *e, int a, int s){ e_w(e, PPC_X(s, a, 0, 922, 0)); }
static inline void e_cmpw(Emit *e, int cr, int a, int b){ e_w(e, PPC_X(cr << 2, a, b, 0, 0)); }
static inline void e_cmplw(Emit *e, int cr, int a, int b){ e_w(e, PPC_X(cr << 2, a, b, 32, 0)); }
static inline void e_lwbrx(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 534, 0)); }
static inline void e_stwbrx(Emit *e, int s, int a, int b){ e_w(e, PPC_X(s, a, b, 662, 0)); }
static inline void e_lhbrx(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 790, 0)); }
static inline void e_sthbrx(Emit *e, int s, int a, int b){ e_w(e, PPC_X(s, a, b, 918, 0)); }
static inline void e_lbzx(Emit *e, int d, int a, int b){ e_w(e, PPC_X(d, a, b, 87, 0)); }
static inline void e_stbx(Emit *e, int s, int a, int b){ e_w(e, PPC_X(s, a, b, 215, 0)); }
static inline void e_dcbst(Emit *e, int a, int b){ e_w(e, PPC_X(0, a, b, 54, 0)); }
static inline void e_icbi(Emit *e, int a, int b){ e_w(e, PPC_X(0, a, b, 982, 0)); }
static inline void e_sync(Emit *e){ e_w(e, PPC_X(0, 0, 0, 598, 0)); }
static inline void e_isync(Emit *e){ e_w(e, 0x4C00012Cu); }
static inline void e_mtspr(Emit *e, int spr, int s){ e_w(e, PPC_X(s, spr & 31, spr >> 5, 467, 0)); }
static inline void e_mfspr(Emit *e, int d, int spr){ e_w(e, PPC_X(d, spr & 31, spr >> 5, 339, 0)); }
static inline void e_mtctr(Emit *e, int s){ e_mtspr(e, 9, s); }
static inline void e_mtlr(Emit *e, int s){ e_mtspr(e, 8, s); }
static inline void e_mflr(Emit *e, int d){ e_mfspr(e, d, 8); }

//Arreglo
static inline void e_rlwinm(Emit *e, int a, int s, int sh, int mb, int me){ e_w(e, PPC_M(21, s, a, sh, mb, me, 0)); }
static inline void e_rlwinm_(Emit *e, int a, int s, int sh, int mb, int me){ e_w(e, PPC_M(21, s, a, sh, mb, me, 1)); }
static inline void e_rlwimi(Emit *e, int a, int s, int sh, int mb, int me){ e_w(e, PPC_M(20, s, a, sh, mb, me, 0)); }
static inline void e_rlwnm(Emit *e, int a, int s, int b, int mb, int me){ e_w(e, PPC_M(23, s, a, b, mb, me, 0)); }

//Arreglo
static inline void e_blr(Emit *e){ e_w(e, 0x4E800020u); }
static inline void e_bctr(Emit *e){ e_w(e, 0x4E800420u); }
static inline void e_bctrl(Emit *e){ e_w(e, 0x4E800421u); }
static inline u32 ppc_b(s32 desp, int lk){ return (18u << 26) | ((u32)desp & 0x03FFFFFCu) | (u32)lk; }
static inline u32 ppc_bc(int bo, int bi, s32 desp){ return (16u << 26) | ((u32)bo << 21) | ((u32)bi << 16) | ((u32)desp & 0xFFFCu); }
static inline void e_b(Emit *e, const u32 *destino){ e_w(e, ppc_b((s32)((const u8 *)destino - (const u8 *)e->p), 0)); }
static inline void e_bl(Emit *e, const void *destino){ e_w(e, ppc_b((s32)((const u8 *)destino - (const u8 *)e->p), 1)); }
static inline u32 *e_bc_hueco(Emit *e, int bo, int bi){
	u32 *w = e->p;
	if(w >= e->fin){ e->lleno = 1; return NULL; }
	e_w(e, ppc_bc(bo, bi, 0));
	return w;
}
static inline void e_parchea_bc(u32 *w, const u32 *destino){
	if(w) *w = (*w & ~0xFFFCu) | ((u32)((const u8 *)destino - (const u8 *)w) & 0xFFFCu);
}
static inline u32 *e_b_hueco(Emit *e){
	u32 *w = e->p;
	if(w >= e->fin){ e->lleno = 1; return NULL; }
	e_w(e, ppc_b(0, 0));
	return w;
}
static inline void e_parchea_b(u32 *w, const u32 *destino){ if(w) *w = ppc_b((s32)((const u8 *)destino - (const u8 *)w), 0); }
static inline int e_alcanza_b(const void *desde, const void *destino){
	const s32 d = (s32)((const u8 *)destino - (const u8 *)desde);
	return d >= -0x02000000 && d < 0x02000000;
}

//Arreglo
static inline void e_carga32(Emit *e, int d, u32 v){
	if((s32)v >= -0x8000 && (s32)v < 0x8000){ e_li(e, d, (s32)v); return; }
	e_lis(e, d, (s32)(v >> 16));
	if(v & 0xFFFF) e_ori(e, d, d, v & 0xFFFF);
}

#endif
