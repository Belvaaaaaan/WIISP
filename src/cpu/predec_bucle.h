/**
 * WIISP - cpu/predec_bucle.h
 * El bucle del intérprete predecodificado (ver predec.c). predec.c lo
 * incluye dos veces: tal cual y compilado sin unir los finales de las
 * tarjetas (predec_sin_unir), con PREDEC_BUCLE (el nombre),
 * PREDEC_COPIA (0 o 1) y PREDEC_ATRIB (los atributos) ya definidos.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//arreglo

PREDEC_ATRIB static u32 PREDEC_BUCLE(u32 max_cycles){   //arreglo
	const u64 start = cpu_cycles;
	const u64 end = start + max_cycles;
	u8 *const ram = psp_mem.ram;
	const u32 lim4 = psp_mem.ram_size - 3, lim2 = psp_mem.ram_size - 1, lim1 = psp_mem.ram_size;
	u8 *const r8 = (u8 *)cpu.r;
	u8 *const f8 = (u8 *)cpu.fpr;
	const int verif = opt_on[OPT_CPU_VERIFICAR];
#if PREDEC_MEDIR   //Arreglo
	const int bq = opt_on[OPT_PROF_BLOQUES];   //Arreglo
#define FASE(f) (prof_cpu_phase = (f))   //Arreglo
#define CAIDA(m) do { if(bq) caida((m), pc, lim4); } while(0)   //Arreglo
#define CONTAR(x) ((void)(x))   //Arreglo
#else   //Arreglo
#define FASE(f) ((void)0)   //Arreglo
#define CAIDA(m) ((void)0)   //Arreglo
#define CONTAR(x) ((void)0)   //Arreglo
#endif   //Arreglo
	u32 pc, npc, left, seg, bt = 0;
	const Card *c = NULL;
	Block *cblk = NULL, *nb, *nbx = NULO;   //Arreglo

	if(unlikely(copia != PREDEC_COPIA)){   //arreglo
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
			&&L_LVS, &&L_SVS, &&L_LVQ, &&L_SVQ, &&L_LVQT, &&L_SVQT, &&L_MFV, &&L_MTV, &&L_VFPU_V,   //Arreglo
			&&L_VADD, &&L_VSUB, &&L_VMUL, &&L_VMOV, &&L_VABS, &&L_VNEG, &&L_VMIN, &&L_VMAX, &&L_VSCL,   //Arreglo
			&&L_VI2F, &&L_VF2I, &&L_VPFX,   //Arreglo
			&&L_END, &&L_BEND, &&L_BENDD   //Arreglo
		};
		memcpy(h_tab, labels, sizeof(h_tab));
		cpu_predec_flush();   //arreglo
		copia = PREDEC_COPIA;   //arreglo
	}
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
#define RAW()  (FRIO(cblk)->raw[IDX()])   //arreglo
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
			SYNC_AT(); dst = lenta_load(a_, size, RAW()); tail; \
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
			SYNC_AT(); lenta_store(a_, size, v, RAW()); tail; \
			if(unlikely(cpu_stop_requested)) goto x_after; \
		} \
	} while(0)
#define ST32(a, v, tail) STX(a, v, 4, lim4, wr_le32(ram + o_, v), tail)
#define ST16(a, v, tail) STX(a, v, 2, lim2, wr_le16(ram + o_, (u16)(v)), tail)
#define ST8(a, v, tail)  STX(a, v, 1, lim1, ram[o_] = (u8)(v), tail)

/* Saltos: bt es a dónde va después del delay slot */
#define BRANCH(name, cond) OP(name, { if(cond){ bt = cblk->target; nbx = cblk->succ[1]; } else { bt = cblk->end; nbx = cblk->succ[0]; } })   //Arreglo
#define BRANCH_LIKELY(name, cond) L_##name: { \
		if(cond){ const void *n_ = c[1].h; bt = cblk->target; nbx = cblk->succ[1]; c++; goto *n_; } \
		goto x_likely_nt; \
	}   //Arreglo

/* Buscar (o traducir) el bloque de pc */
dispatch:
	FASE(CPF_BUSCAR);   //Arreglo
	if(unlikely(!left)) goto done;
	if(npc != pc + 4 || (pc & 3) || RAM_OFF(pc) >= lim4){ CAIDA(npc != pc + 4 ? CAIDA_DELAY : CAIDA_FUERA); goto step; }   //Arreglo
	nb = lookup(pc);
	if(!nb) nb = build(pc, lim4);
	st.busquedas++;
	if(unlikely(!nb->n || left < nb->n)){ CAIDA(nb->n ? CAIDA_NO_CABE : CAIDA_SIN_TARJETA); goto step; }   //Arreglo
	if(unlikely(verif) && !verif_ok(nb)){
		kill_block(nb);
		nb = build(pc, lim4);
		if(!nb->n || left < nb->n){ CAIDA(nb->n ? CAIDA_NO_CABE : CAIDA_SIN_TARJETA); goto step; }   //Arreglo
	}
	FASE(CPF_TARJETAS);   //Arreglo
enter:
#if PREDEC_MEDIR   //Arreglo
	if(bq){ st.bq_bloques++; st.bq_instr += nb->n; st.bq_tam[tam_bloque(nb->n)]++; }   //Arreglo
#endif   //Arreglo
	cblk = nb;
	c = nb->c;
	left -= nb->n;
	goto *c->h;

/* El final del bloque: al siguiente por el enlace guardado (los bloques
   enlazados nunca tienen n = 0, así que left >= n también dice left > 0).
   Con cpu_verificar, siempre por la búsqueda, que es donde se comprueba. */
link_end:
	nb = cblk->succ[0];
	if(likely(nb && nb->alive && left >= nb->n && !verif)){ CONTAR(bq && st.bq_enlace[0][0]++); goto enter; }   //Arreglo
	npc = pc + 4;
	if(!left) goto done;
	CONTAR(bq && st.bq_enlace[0][1]++);   //Arreglo
	nb = lookup(pc);
	if(nb && nb->n) cblk->succ[0] = nb;
	goto dispatch;
link:
	{
		const int k = pc != cblk->end;
		nb = cblk->succ[k];
		if(likely(nb && nb->pc == pc && nb->alive && left >= nb->n && !verif)){ CONTAR(bq && st.bq_enlace[k][0]++); goto enter; }   //Arreglo
		npc = pc + 4;
		if(!left) goto done;
#if PREDEC_MEDIR   //Arreglo
		if(bq){   //Arreglo
			st.bq_enlace[k][1]++;   //Arreglo
			if(CB[BN - 2].h == h_tab[OP_JR] || CB[BN - 2].h == h_tab[OP_JALR]) st.bq_jr_sin_enlace++;   //Arreglo
		}   //Arreglo
#endif   //Arreglo
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
	FASE(CPF_PASO);   //Arreglo
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

x_vfpu:   //Arreglo
	SYNC_AT();   //Arreglo
	CONTAR(st.vfpu++);   //Arreglo
	FASE(CPF_VFPU);   //Arreglo
	vfpu_exec(RAW(), IPC());   //Arreglo
	FASE(CPF_TARJETAS);   //Arreglo
	if(unlikely(cpu_stop_requested || !cblk->alive)) goto x_after;   //Arreglo
	{ const void *n_ = c[1].h; c++; goto *n_; }   //Arreglo

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
	FASE(CPF_TARJETAS);   //Arreglo
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
		else { SYNC_AT(); mem = lenta_load(a_, 4, RAW()); chk = cpu_stop_requested; } \
		mem = merge; \
		if(likely(o_ < lim4)){ \
			wr_le32(ram + o_, mem); \
			if(unlikely(cpu_code_map[o_ >> PREDEC_ZONA_BITS])){ cpu_predec_store(o_, 4); chk |= !cblk->alive; } \
		} else { SYNC_AT(); lenta_store(a_, 4, mem, RAW()); chk |= cpu_stop_requested; } \
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
	OP(FPU, {   //arreglo
		CONTAR(st.fpu++);   //Arreglo
		FASE(CPF_FPU);   //Arreglo
		cpu_cop1_s(compacto ? c->a.w : RAW(), IPC());
		FASE(CPF_TARJETAS);   //Arreglo
	})
	OP(CVTSW, { float f = (float)(s32)FR(S); memcpy(f8 + D, &f, 4); })
	OP(MFIC, G(T) = (u32)hle_get_intr_enabled())
	OP(MTIC, {
		SYNC_AT();
		hle_set_intr_enabled(G(T) & 1);
		if(unlikely(cpu_stop_requested)) goto x_after;
	})
	OP(VFPU, {
		SYNC_AT();
		CONTAR(st.vfpu++);   //Arreglo
		FASE(CPF_VFPU);   //Arreglo
		vfpu_exec(compacto ? c->a.w : RAW(), IPC());   //arreglo
		FASE(CPF_TARJETAS);   //Arreglo
		if(unlikely(cpu_stop_requested || !cblk->alive)) goto x_after;
	})
#define VTARJ(name, vs) L_##name: { \
		const void *n_ = c[1].h; \
		const int r_ = vfpu_tarjeta(OP_##name, c, r8, ram, lim4); \
		if(unlikely(!r_)) goto x_vfpu; \
		vfpu_stat_now[vs]++; \
		CONTAR(st.vfpu_tarjetas++); \
		if(r_ == 2 && !cblk->alive) goto x_after; \
		c++; \
		goto *n_; \
	}   //Arreglo
	VTARJ(LVS, VS_LV)   //Arreglo
	VTARJ(SVS, VS_SV)   //Arreglo
	VTARJ(LVQ, VS_LVQ)   //Arreglo
	VTARJ(SVQ, VS_SVQ)   //Arreglo
	VTARJ(LVQT, VS_LVQ)   //Arreglo
	VTARJ(SVQT, VS_SVQ)   //Arreglo
	VTARJ(MFV, VS_MFV)   //Arreglo
	VTARJ(MTV, VS_MTV)   //Arreglo
	VTARJ(VADD, VS_VADD)   //Arreglo
	VTARJ(VSUB, VS_VSUB)   //Arreglo
	VTARJ(VMUL, VS_VMUL)   //Arreglo
	VTARJ(VMOV, VS_VMOV)   //Arreglo
	VTARJ(VABS, VS_VABS_NEG)   //Arreglo
	VTARJ(VNEG, VS_VABS_NEG)   //Arreglo
	VTARJ(VMIN, VS_VMIN_MAX)   //Arreglo
	VTARJ(VMAX, VS_VMIN_MAX)   //Arreglo
	VTARJ(VSCL, VS_VSCL)   //Arreglo
	VTARJ(VI2F, VS_VI2F)   //Arreglo
	VTARJ(VF2I, VS_VF2I)   //Arreglo
	VTARJ(VPFX, VS_VPFX)   //Arreglo
#undef VTARJ   //Arreglo
	L_VFPU_V: {   //Arreglo
		SYNC_AT();   //Arreglo
		FASE(CPF_VFPU);   //Arreglo
		vfpu_verificar_una(c->a.w, IPC(), lim4);   //Arreglo
		FASE(CPF_TARJETAS);   //Arreglo
		if(unlikely(cpu_stop_requested || !cblk->alive)) goto x_after;   //Arreglo
		{ const void *n_ = c[1].h; c++; goto *n_; }   //Arreglo
	}   //Arreglo
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
	OP(BLTZAL, { s32 v = (s32)G(S); G(31 * 4) = cblk->end; if(v < 0){ bt = cblk->target; nbx = cblk->succ[1]; } else { bt = cblk->end; nbx = cblk->succ[0]; } })   //Arreglo
	OP(BGEZAL, { s32 v = (s32)G(S); G(31 * 4) = cblk->end; if(v >= 0){ bt = cblk->target; nbx = cblk->succ[1]; } else { bt = cblk->end; nbx = cblk->succ[0]; } })   //Arreglo
	BRANCH_LIKELY(BEQL, G(S) == G(T))
	BRANCH_LIKELY(BNEL, G(S) != G(T))
	BRANCH_LIKELY(BLEZL, (s32)G(S) <= 0)
	BRANCH_LIKELY(BGTZL, (s32)G(S) > 0)
	BRANCH_LIKELY(BLTZL, (s32)G(S) < 0)
	BRANCH_LIKELY(BGEZL, (s32)G(S) >= 0)
	L_BLTZALL: { s32 v = (s32)G(S); G(31 * 4) = cblk->end; if(v < 0){ const void *n_ = c[1].h; bt = cblk->target; nbx = cblk->succ[1]; c++; goto *n_; } goto x_likely_nt; }   //Arreglo
	L_BGEZALL: { s32 v = (s32)G(S); G(31 * 4) = cblk->end; if(v >= 0){ const void *n_ = c[1].h; bt = cblk->target; nbx = cblk->succ[1]; c++; goto *n_; } goto x_likely_nt; }   //Arreglo
	OP(J, { bt = cblk->target; nbx = cblk->succ[1]; })   //Arreglo
	OP(JAL, { G(31 * 4) = cblk->end; bt = cblk->target; nbx = cblk->succ[1]; })   //Arreglo
	OP(JR, bt = G(S))
	OP(JALR, { u32 v = G(S); G(D) = cblk->end; bt = v; })
	L_BC1: {
		int cond = (cpu.fcr31 & FCR31_COND) != 0, taken;
		if(cblk->end - 8 == cpu.fcc_hazard_pc) cond = cpu.fcc_old;
		cpu.fcc_hazard_pc = 0;
		taken = (SAF & 1) ? cond : !cond;
		if(SAF & 2){
			if(taken){ const void *n_ = c[1].h; bt = cblk->target; nbx = cblk->succ[1]; c++; goto *n_; }   //Arreglo
			goto x_likely_nt;
		}
		{ const void *n_ = c[1].h; if(taken){ bt = cblk->target; nbx = cblk->succ[1]; } else { bt = cblk->end; nbx = cblk->succ[0]; } c++; goto *n_; }   //Arreglo
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
			nbx = cblk->succ[1];   //Arreglo
		} else if(taken){ bt = cblk->target; nbx = cblk->succ[1]; } else { bt = cblk->end; nbx = cblk->succ[0]; }   //Arreglo
		if(unlikely(chk)) goto x_after;
		{ const void *n_ = c[1].h; c++; goto *n_; }
	}

	L_END:
		pc = cblk->end;
		goto link_end;
	L_BEND:
		pc = bt;
		goto link;
	L_BENDD: {   //Arreglo
		Block *const nb_ = nbx;
		const void *const h_ = nb_->c[0].h;
		const u32 n_ = nb_->n;
		pc = bt;
		if(unlikely(verif) && nb_->alive && !verif_ok(nb_)) kill_block(nb_);
		if(likely(nb_->alive && left >= n_)){
#if PREDEC_MEDIR
			(void)h_;
			CONTAR(bq && st.bq_enlace[pc != cblk->end][0]++);
			nb = nb_;
			goto enter;
#else
			cblk = nb_;
			c = nb_->c;
			left -= n_;
			goto *h_;
#endif
		}
		goto link;
	}

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
#undef FASE   //Arreglo
#undef CAIDA   //Arreglo
#undef CONTAR   //Arreglo
}
