/**
 * WIISP - tests/test_jit.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo

#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
#include "cpu/jit.h"
#include "cpu/ppc_emit.h"
#include "cpu/predec.h"
#include "core/memory.h"
#include "core/opt.h"
#include "test.h"

#define JC 0x08A40000u
#define JD 0x08A50000u
#define JD_TAM 0x1000u

static u32 js = 0x51ED270Bu;
static u32 jr_(void){ js ^= js << 13; js ^= js >> 17; js ^= js << 5; return js; }
static u32 jrn(u32 n){ return jr_() % n; }

static void test_emisor(void){
	static u32 buf[128], esperado[128];
	Emit e;
	u32 n = 0, i, *h1, *h2, *h3, *h4;
	e_ini(&e, buf, 128);
		e_addi(&e,3,4,-5); esperado[n++] = 0x3864fffbu;
		e_addis(&e,3,4,0x1234); esperado[n++] = 0x3c641234u;
		e_li(&e,7,100); esperado[n++] = 0x38e00064u;
		e_lis(&e,7,-1); esperado[n++] = 0x3ce0ffffu;
		e_addic(&e,5,6,-1); esperado[n++] = 0x30a6ffffu;
		e_subfic(&e,5,6,9); esperado[n++] = 0x20a60009u;
		e_ori(&e,3,4,0xFFFF); esperado[n++] = 0x6083ffffu;
		e_oris(&e,3,4,0x8000); esperado[n++] = 0x64838000u;
		e_xori(&e,3,4,1); esperado[n++] = 0x68830001u;
		e_xoris(&e,3,4,0x8000); esperado[n++] = 0x6c838000u;
		e_andi_(&e,3,4,0x1F); esperado[n++] = 0x7083001fu;
		e_andis_(&e,0,12,0xFE00); esperado[n++] = 0x7580fe00u;
		e_lwz(&e,6,136,3); esperado[n++] = 0x80c30088u;
		e_stw(&e,6,-4,3); esperado[n++] = 0x90c3fffcu;
		e_lbz(&e,0,0,5); esperado[n++] = 0x88050000u;
		e_cmpwi(&e,1,7,0); esperado[n++] = 0x2c870000u;
		e_cmpwi(&e,0,7,-1); esperado[n++] = 0x2c07ffffu;
		e_cmplwi(&e,1,7,64); esperado[n++] = 0x28870040u;
		e_add(&e,3,4,5); esperado[n++] = 0x7c642a14u;
		e_addc(&e,3,4,5); esperado[n++] = 0x7c642814u;
		e_adde(&e,3,4,5); esperado[n++] = 0x7c642914u;
		e_subf(&e,3,4,5); esperado[n++] = 0x7c642850u;
		e_subfc(&e,3,4,5); esperado[n++] = 0x7c642810u;
		e_subfe(&e,3,4,5); esperado[n++] = 0x7c642910u;
		e_neg(&e,3,4); esperado[n++] = 0x7c6400d0u;
		e_mullw(&e,3,4,5); esperado[n++] = 0x7c6429d6u;
		e_mulhw(&e,3,4,5); esperado[n++] = 0x7c642896u;
		e_mulhwu(&e,3,4,5); esperado[n++] = 0x7c642816u;
		e_and(&e,3,4,5); esperado[n++] = 0x7c832838u;
		e_andc(&e,3,4,5); esperado[n++] = 0x7c832878u;
		e_or(&e,3,4,5); esperado[n++] = 0x7c832b78u;
		e_xor(&e,3,4,5); esperado[n++] = 0x7c832a78u;
		e_nor(&e,3,4,5); esperado[n++] = 0x7c8328f8u;
		e_mr(&e,9,10); esperado[n++] = 0x7d495378u;
		e_slw(&e,3,4,5); esperado[n++] = 0x7c832830u;
		e_srw(&e,3,4,5); esperado[n++] = 0x7c832c30u;
		e_sraw(&e,3,4,5); esperado[n++] = 0x7c832e30u;
		e_srawi(&e,3,4,31); esperado[n++] = 0x7c83fe70u;
		e_cntlzw(&e,3,4); esperado[n++] = 0x7c830034u;
		e_extsb(&e,3,4); esperado[n++] = 0x7c830774u;
		e_extsh(&e,3,4); esperado[n++] = 0x7c830734u;
		e_cmpw(&e,1,3,4); esperado[n++] = 0x7c832000u;
		e_cmplw(&e,0,3,4); esperado[n++] = 0x7c032040u;
		e_lwbrx(&e,6,4,12); esperado[n++] = 0x7cc4642cu;
		e_stwbrx(&e,6,4,12); esperado[n++] = 0x7cc4652cu;
		e_lhbrx(&e,6,4,12); esperado[n++] = 0x7cc4662cu;
		e_sthbrx(&e,6,4,12); esperado[n++] = 0x7cc4672cu;
		e_lbzx(&e,6,4,12); esperado[n++] = 0x7cc460aeu;
		e_stbx(&e,6,4,12); esperado[n++] = 0x7cc461aeu;
		e_dcbst(&e,0,3); esperado[n++] = 0x7c00186cu;
		e_icbi(&e,0,3); esperado[n++] = 0x7c001facu;
		e_sync(&e); esperado[n++] = 0x7c0004acu;
		e_isync(&e); esperado[n++] = 0x4c00012cu;
		e_mtctr(&e,12); esperado[n++] = 0x7d8903a6u;
		e_mtlr(&e,0); esperado[n++] = 0x7c0803a6u;
		e_mflr(&e,0); esperado[n++] = 0x7c0802a6u;
		e_mfspr(&e,3,953); esperado[n++] = 0x7c79eaa6u;
		e_mtspr(&e,1011,4); esperado[n++] = 0x7c93fba6u;
		e_rlwinm(&e,12,12,0,2,31); esperado[n++] = 0x558c00beu;
		e_rlwinm_(&e,0,12,30,0,8); esperado[n++] = 0x5580f011u;
		e_rlwimi(&e,3,4,5,10,20); esperado[n++] = 0x50832aa8u;
		e_rlwnm(&e,3,4,5,0,31); esperado[n++] = 0x5c83283eu;
		e_blr(&e); esperado[n++] = 0x4e800020u;
		e_bctr(&e); esperado[n++] = 0x4e800420u;
		e_bctrl(&e); esperado[n++] = 0x4e800421u;
	CHECK_EQ(e_pos(&e), n);
	for(i = 0; i < n; i++) if(buf[i] != esperado[i]){ printf("  instruccion %u: %08X, se esperaba %08X\n", (unsigned)i, (unsigned)buf[i], (unsigned)esperado[i]); CHECK(0); }
	e_ini(&e, buf, 128);
	h1 = e_bc_hueco(&e, BO_FALSO, 4 * 1 + CR_EQ);
	h2 = e_bc_hueco(&e, BO_CIERTO, 4 * 0 + CR_EQ);
	e_b(&e, buf);
	e_parchea_bc(h1, e.p);
	e_parchea_bc(h2, e.p);
	e_bl(&e, buf);
	e_w(&e, ppc_bc(BO_FALSO, 4 * 0 + CR_LT, (s32)((u8 *)buf - (u8 *)e.p)));
	h3 = e_b_hueco(&e);
	h4 = e_bc_hueco(&e, BO_CIERTO, 4 * 1 + CR_LT);
	e_parchea_b(h3, e.p);
	e_parchea_bc(h4, e.p);
	e_w(&e, ppc_bc(BO_CIERTO, 4 * 7 + CR_GT, (s32)((u8 *)buf - (u8 *)e.p)));
	{
		static const u32 saltos[8] = { 0x4086000Cu, 0x41820008u, 0x4BFFFFF8u, 0x4BFFFFF5u, 0x4080FFF0u, 0x48000008u, 0x41840004u, 0x419DFFE4u };
		CHECK_EQ(e_pos(&e), 8u);
		for(i = 0; i < 8; i++) CHECK_EQ(buf[i], saltos[i]);
	}
	CHECK(e_alcanza_b(buf, buf + 0x7FFFFF));
	CHECK(!e_alcanza_b(buf, buf + 0x800000));
	e_ini(&e, buf, 2);
	e_carga32(&e, 3, 0x12345678u);
	e_carga32(&e, 3, 1);
	CHECK(e.lleno);
	e_ini(&e, buf, 8);
	e_carga32(&e, 3, 0xFFFF8000u);
	e_carga32(&e, 3, 0x00008000u);
	e_carga32(&e, 3, 0x12340000u);
	CHECK_EQ(e_pos(&e), 4u);
	CHECK_EQ(buf[0], 0x38608000u);
	CHECK_EQ(buf[1], 0x3C600000u);
	CHECK_EQ(buf[2], 0x60638000u);
	CHECK_EQ(buf[3], 0x3C601234u);
	e_ini(&e, buf, 1);
	e_li(&e, 3, 0);
	h1 = e_bc_hueco(&e, BO_FALSO, CR_EQ);
	h3 = e_b_hueco(&e);
	CHECK(h1 == NULL && h3 == NULL && e.lleno);
	e_parchea_bc(h1, buf);
	e_parchea_b(h3, buf);
	CHECK_EQ(buf[0], 0x38600000u);
}

static u32 r_(u32 rs, u32 rt, u32 rd, u32 sa, u32 fn){ return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn; }
static u32 i_(u32 op, u32 rs, u32 rt, u32 imm){ return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }
static u32 dr(void){ return jrn(10) ? 1 + jrn(27) : jrn(2) ? 0 : 31; }
static u32 sr(void){ return jrn(12) ? jrn(28) : 28 + jrn(4); }
static u32 im(void){
	static const u32 sp[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFF, 0xFFFE, 0x8001 };
	return jrn(3) ? jrn(0x10000) : sp[jrn(7)];
}
static u32 mo(u32 al){ u32 o = (u32)((s32)jrn(0x1000) - 0x800); return (jrn(10) ? o & ~(al - 1) : o) & 0xFFFF; }

static u32 gen_normal(int nosoportadas){
	static const u32 alu[] = { 0x04, 0x06, 0x07, 0x0A, 0x0B, 0x16, 0x17, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2C, 0x2D };
	static const u32 hilo[] = { 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1C, 0x1D, 0x2E, 0x2F };
	static const u32 ld[] = { 0x20, 0x21, 0x23, 0x24, 0x25 };
	static const u32 st[] = { 0x28, 0x29, 0x2B };
	u32 f, op;
	if(nosoportadas && !jrn(30)){
		switch(jrn(4)){
		case 0: return r_(sr(), sr(), 0, 0, 0x1A);
		case 1: return i_(0x22, 28, 1 + jrn(27), mo(1));
		case 2: return (0x11u << 26) | (0x10u << 21) | (jrn(32) << 16) | (jrn(32) << 11) | (jrn(32) << 6);
		default: return r_(0, sr(), dr(), 0x14, 0x20) | (0x1Fu << 26);
		}
	}
	switch(jrn(12)){
	case 0: case 1: f = alu[jrn(19)]; return r_(sr(), sr(), dr(), f == 0x06 ? jrn(2) : jrn(4) ? 0 : jrn(32), f);
	case 2: f = jrn(3); f = f ? f + 1 : 0; return r_(f == 2 ? jrn(2) : 0, sr(), dr(), jrn(4) ? jrn(32) : 0, f);
	case 3: return r_(sr(), sr(), dr(), 0, hilo[jrn(10)]);
	case 4: case 5: return i_(0x08 + jrn(8), sr(), dr(), im());
	case 6: return (0x1Fu << 26) | r_(sr(), dr(), jrn(32), jrn(32), jrn(2) ? 0x00 : 0x04);
	case 7: return (0x1Fu << 26) | r_(0, sr(), dr(), jrn(2) ? 0x10 : 0x18, 0x20);
	case 8: case 9:
		op = ld[jrn(5)];
		return i_(op, jrn(8) ? 28 : 29, 1 + jrn(27), mo(op == 0x23 ? 4 : op == 0x21 || op == 0x25 ? 2 : 1));
	case 10:
		op = st[jrn(3)];
		return i_(op, jrn(8) ? 28 : 29, sr(), mo(op == 0x2B ? 4 : op == 0x29 ? 2 : 1));
	default: return jrn(2) ? 0 : r_(0, 0, 0, 0, 0x0F);
	}
}

static u32 gen_salto(void){
	switch(jrn(10)){
	case 0: return i_(0x04, sr(), sr(), im());
	case 1: return i_(0x05, sr(), sr(), im());
	case 2: return i_(0x06, sr(), 0, im());
	case 3: return i_(0x07, sr(), 0, im());
	case 4: return i_(0x01, sr(), 0, im());
	case 5: return i_(0x01, sr(), 1, im());
	case 6: return (2u << 26) | (jr_() & 0x03FFFFFFu);
	case 7: return (3u << 26) | (jr_() & 0x03FFFFFFu);
	case 8: return r_(sr(), 0, 0, 0, 0x08);
	default: return r_(sr(), 0, jrn(2) ? 31 : dr(), 0, 0x09);
	}
}

typedef struct {
	u32 r[32], hi, lo, pc, npc;
	u8 d[JD_TAM];
} JSnap;

static void j_toma(JSnap *s){
	memcpy(s->r, cpu.r, sizeof(s->r));
	s->hi = cpu.hi; s->lo = cpu.lo; s->pc = cpu.pc; s->npc = cpu.npc;
	memcpy(s->d, mem_ptr(JD, JD_TAM), JD_TAM);
}

static void j_pon(const JSnap *s){
	memcpy(cpu.r, s->r, sizeof(s->r));
	cpu.hi = s->hi; cpu.lo = s->lo; cpu.pc = s->pc; cpu.npc = s->npc;
	memcpy(mem_ptr(JD, JD_TAM), s->d, JD_TAM);
}

//Arreglo
static int j_acceso_nativo_malo(const u32 *raw, u32 hasta, int zona_codigo, u32 zona){
	u32 i;
	for(i = 0; i < hasta; i++){
		const u32 w = raw[i], op = w >> 26;
		if(op >= 0x20 && op <= 0x2B && op != 0x22 && op != 0x26 && op != 0x27 && op != 0x2A){
			const u32 t = (op & 3) == 3 ? 4 : (op & 3) == 1 ? 2 : 1;
			const u32 a = cpu.r[(w >> 21) & 31] + (u32)(s32)(s16)(w & 0xFFFF), o = (a & PSP_ADDR_MASK) - PSP_RAM_BASE;
			if(a & (t - 1)) return 1;
			if(o > psp_mem.ram_size - t) return 2;
			if(op >= 0x28 && zona_codigo && (o >> PREDEC_ZONA_BITS) == zona) return 3;
		}
		cpu_run_interp(1);
	}
	return 0;
}

static void test_bloques(void){
	static JSnap ini, nat, ref;
	static const u32 lentas[] = { 0x00010800u, 0x04000800u, 0x04200800u, 0x0A000000u, 0x00000000u, 0x88A50800u, 0x48A50800u };
	u32 prog, raw[64], i, total = 0, nativas = 0, salidas = 0, parciales = 0, hechos = 0, malos = 0, verif0, accesos_malos = 0;
	const u32 zona = ((JD & PSP_ADDR_MASK) - PSP_RAM_BASE) >> 10;
	const u8 saved_se = opt_on[OPT_CPU_SIN_EXTRA];
	JitStats a, b;
	opt_on[OPT_CPU_SIN_EXTRA] = 1;
	jit_tam_pruebas(65536);
	jit_stats(&a);
	verif0 = (u32)a.verif_mal;
	for(prog = 0; prog < 100000; prog++){
		const int branch = jrn(3) != 0, nosop = prog % 4 == 0;
		const u32 n = branch ? 2 + jrn(40) : 1 + jrn(40);
		const int verificar = prog % 5 == 4, zona_codigo = prog % 7 == 3;
		u32 cubre = 0, kn;
		JitFn f;
		for(i = 0; i < n; i++) raw[i] = gen_normal(nosop);
		if(branch){ raw[n - 2] = gen_salto(); if(!jrn(10)) raw[n - 1] = 0; }
		for(i = 0; i < n; i++) wr_le32(mem_ptr(JC + 4 * i, 4), raw[i]);
		for(i = 1; i < 32; i++) cpu.r[i] = jrn(4) ? jr_() : jrn(2) ? jrn(64) : jrn(2) ? 0xFFFFFFFFu : 0x80000000u;
		cpu.r[0] = 0;
		cpu.r[28] = JD + 0x800;
		cpu.r[29] = lentas[jrn(7)];
		cpu.hi = jr_(); cpu.lo = jr_();
		cpu.pc = JC; cpu.npc = JC + 4;
		for(i = 0; i < JD_TAM; i++) mem_ptr(JD, JD_TAM)[i] = (u8)jr_();
		j_toma(&ini);
		if(jit_lleno()) jit_vaciar();
		f = jit_compilar(JC, raw, n, branch, verificar, &cubre);
		if(!f && jit_lleno()){ jit_vaciar(); f = jit_compilar(JC, raw, n, branch, verificar, &cubre); }
		if(!f){ CHECK_EQ(cubre, 0u); CHECK_EQ(jit_prefijo(raw, n, branch), 0u); continue; }
		CHECK_EQ(cubre, jit_prefijo(raw, n, branch));
		cpu_code_map[zona] = (u8)zona_codigo;
		if(getenv("JIT_RASTRO")){
			FILE *fo = fopen("jit_bloque.bin", "wb");
			printf("bloque %u n %u br %d ver %d cubre %u:", (unsigned)prog, (unsigned)n, branch, verificar, (unsigned)cubre); for(i = 0; i < n; i++) printf(" %08X", (unsigned)raw[i]); printf("\n"); fflush(stdout);
			if(fo){ fwrite((const void *)f, 1, (size_t)((const u8 *)jit_arena_dir() + jit_arena_usada() - (const u8 *)f), fo); fclose(fo); }
		}
		if(verificar){
			kn = jit_verificar_bloque(f, JC, n);
			if(kn != JIT_HECHO){
				CHECK(kn < n);
				j_toma(&nat);
				j_pon(&ini);
				cpu_run_interp(kn);
				j_toma(&ref);
			} else {
				j_toma(&nat);
				j_pon(&ini);
				cpu_run_interp(n);
				j_toma(&ref);
			}
		} else {
			kn = f((u8 *)cpu.r, psp_mem.ram, cpu_code_map);
			j_toma(&nat);
			j_pon(&ini);
			cpu_run_interp(kn == JIT_HECHO ? n : kn);
			j_toma(&ref);
		}
		cpu_code_map[zona] = 0;
		total++;
		if(kn == JIT_HECHO){
			hechos++;
			nativas += n;
			if(nat.pc != ref.pc) malos++;
		} else {
			CHECK(kn <= cubre);
			if(kn < cubre){
				const u32 op = raw[kn] >> 26;
				salidas++;
				CHECK(op >= 0x20 && op <= 0x2B);
			} else parciales++;
			nativas += kn;
			if(branch && kn == n - 1 && nat.pc != ref.npc) malos++;
		}
		j_pon(&ini);
		{
			const int am = j_acceso_nativo_malo(raw, kn == JIT_HECHO ? n : kn, zona_codigo, zona);
			if(am && accesos_malos++ < 4) printf("  bloque %u: un acceso hecho en nativo que tenia que salir a la tarjeta (%s)\n", (unsigned)prog,
			                                    am == 1 ? "desalineado" : am == 2 ? "fuera de la RAM" : "escritura en una zona con codigo");
		}
		if(memcmp(nat.r, ref.r, sizeof(nat.r)) || nat.hi != ref.hi || nat.lo != ref.lo || memcmp(nat.d, ref.d, JD_TAM)){
			if(malos < 6){
				printf("  bloque %u (n %u, hasta %u, %s):", (unsigned)prog, (unsigned)n, (unsigned)(kn == JIT_HECHO ? n : kn),
				       verificar ? "verificado" : "nativo");
				for(i = 0; i < 32; i++) if(nat.r[i] != ref.r[i]) printf(" r%u %08X/%08X", (unsigned)i, (unsigned)nat.r[i], (unsigned)ref.r[i]);
				if(nat.hi != ref.hi || nat.lo != ref.lo) printf(" hi/lo %08X:%08X/%08X:%08X", (unsigned)nat.hi, (unsigned)nat.lo, (unsigned)ref.hi, (unsigned)ref.lo);
				if(memcmp(nat.d, ref.d, JD_TAM)) printf(" memoria");
				printf("\n");
				for(i = 0; i < n; i++) printf("    %08X\n", (unsigned)raw[i]);
			}
			malos++;
		}
	}
	jit_stats(&b);
	CHECK_EQ(malos, 0u);
	CHECK_EQ(accesos_malos, 0u);
	CHECK_EQ((u32)b.verif_mal, verif0);
	CHECK(total > 90000);
	CHECK(hechos > 40000);
	CHECK(salidas > 30000);
	CHECK(parciales > 5000);
	CHECK(b.llenos > a.llenos);
	printf("  jit_proto, bloques al azar: %u compilados (%u enteros, %u con salida en un acceso, %u hasta una no soportada), "
	       "%u instrucciones nativas comparadas con el interprete\n", (unsigned)total, (unsigned)hechos, (unsigned)salidas,
	       (unsigned)parciales, (unsigned)nativas);
	jit_tam_pruebas(0);
	jit_vaciar();
	opt_on[OPT_CPU_SIN_EXTRA] = saved_se;
}

void test_jit(void){
	printf("jit_proto: emisor PowerPC y bloques nativos contra el interprete\n");
	test_emisor();
	if(JIT_PPC) test_bloques();
	else printf("  (sin PowerPC: solo el emisor)\n");
}
