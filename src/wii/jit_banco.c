/**
 * WIISP - wii/jit_banco.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
#include "cpu/jit.h"
#include "cpu/ppc_emit.h"
#include "cpu/predec.h"
#include "core/memory.h"
#include "core/opt.h"
#include "hle/hle.h"
#if defined(GEKKO)
#include <malloc.h>
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <tuxedo/ppc/exception.h>
#include "wii/wii.h"
#else
#include <sys/mman.h>
#endif

//Arreglo
#if defined(GEKKO)
#define MFSPR(n) ({ u32 v_; __asm__ volatile("mfspr %0, %1" : "=r"(v_) : "i"(n)); v_; })
#define MTSPR(n, v) __asm__ volatile("mtspr %0, %1" : : "i"(n), "r"((u32)(v)))
#define SIN_IRQ(c) _CPU_ISR_Disable(c)
#define CON_IRQ(c) _CPU_ISR_Restore(c)
#else
#define SIN_IRQ(c) ((c) = 0)
#define CON_IRQ(c) ((void)(c))
#endif

typedef u32 (*Fn)(u32 a, u32 b, u32 c);

#define K_SALTOS 256
#define LI3(v) (0x38600000u | ((u32)(v) & 0xFFFFu))
#define BLR 0x4E800020u

static int con_pmc;
static u32 malos;

//Arreglo
static inline u32 reloj(void){
	u32 t;
#if defined(GEKKO)
	if(con_pmc) return MFSPR(953);
#endif
	__asm__ volatile("mftb %0" : "=r"(t));
	return t * 12;
}

//Arreglo
static u32 __attribute__((noinline)) uno(u32 x){ return x + 1; }

//Arreglo
static u32 medir(Fn f, u32 a, u32 b, u32 c, u32 *res){
	u32 mejor = 0xFFFFFFFFu, i, t0, t1, r = 0, nivel;
	for(i = 0; i < 9; i++){
		SIN_IRQ(nivel);
		t0 = reloj();
		r = f(a, b, c);
		t1 = reloj();
		CON_IRQ(nivel);
		if(i && t1 - t0 < mejor) mejor = t1 - t0;
	}
	if(res) *res = r;
	return mejor;
}

//Arreglo
static void prologo(Emit *e){ e_mflr(e, 0); e_w(e, PPC_D(37, 1, 1, -16)); e_stw(e, 0, 20, 1); }
static void epilogo(Emit *e){ e_lwz(e, 0, 20, 1); e_mtlr(e, 0); e_addi(e, 1, 1, 16); e_blr(e); }
static Fn publicar(Emit *e){
	if(e->lleno) return NULL;
	jit_publicar(e->ini, (u32)((u8 *)e->p - (u8 *)e->ini));
	return (Fn)(uintptr_t)e->ini;
}
static void bc_a(Emit *e, int bo, int bi, const u32 *destino){ e_w(e, ppc_bc(bo, bi, (s32)((const u8 *)destino - (const u8 *)e->p))); }

static u32 rs = 0x9E3779B9u;
static u32 azar(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

//Arreglo
static void comprobar(const char *que, u32 r, u32 bien){
	if(r == bien) return;
	malos++;
	hle_log_quiet("[PRUEBA_JIT] MAL: %s dio %08X y tenia que dar %08X\n", que, (unsigned)r, (unsigned)bien);
}

//Arreglo
static void prueba_publicar(u32 *w, const char *donde){
	const Fn f = (Fn)(uintptr_t)w;
	u32 r[6];
	w[0] = LI3(1); w[1] = BLR; jit_publicar(w, 8); r[0] = f(0, 0, 0);
	w[0] = LI3(2); jit_publicar(w, 4); r[1] = f(0, 0, 0);
	w[0] = LI3(3);
	__asm__ volatile("dcbst 0,%0\n\tsync" :: "r"(w) : "memory");
	r[2] = f(0, 0, 0);
	w[0] = LI3(4);
	__asm__ volatile("icbi 0,%0\n\tsync\n\tisync" :: "r"(w) : "memory");
	r[3] = f(0, 0, 0);
	w[0] = LI3(5);
#if defined(GEKKO)
	DCFlushRange(w, 32);
	ICInvalidateRange(w, 32);
#else
	jit_publicar(w, 4);
#endif
	r[4] = f(0, 0, 0);
	w[0] = LI3(6); jit_publicar(w, 4); r[5] = f(0, 0, 0);
	hle_log_quiet("[PRUEBA_JIT] publicar en %s (%08X): dcbst+sync+icbi+isync %u y %u (bien: 1 y 2); sin icbi %u (3 = se vio, 2 = la "
	              "vieja); sin dcbst %u (4 = se vio, 3 = la vieja); DCFlushRange+ICInvalidateRange %u (bien: 5); otra vez %u (bien: 6)\n",
	              donde, (unsigned)(uintptr_t)w, (unsigned)r[0], (unsigned)r[1], (unsigned)r[2], (unsigned)r[3], (unsigned)r[4],
	              (unsigned)r[5]);
	comprobar("publicar 1", r[0], 1);
	comprobar("publicar 2", r[1], 2);
	comprobar("DCFlushRange+ICInvalidateRange", r[4], 5);
	comprobar("publicar 6", r[5], 6);
}

//Arreglo
static void prueba_coste_publicar(u32 *buf, u32 bytes, const char *donde){
	static const u32 tams[4] = { 32, 1024, 16384, 262144 };
	char l[400];
	size_t len = (size_t)snprintf(l, sizeof(l), "[PRUEBA_JIT] publicar en %s, ciclos por linea de 32 bytes (escrita antes):", donde);
	u32 k, i, v;
	for(k = 0; k < 4 && tams[k] <= bytes; k++){
		u32 mejor = 0xFFFFFFFFu;
		for(v = 0; v < 5; v++){
			u32 t0, t1, nivel;
			for(i = 0; i < tams[k] / 4; i++) buf[i] = 0x60000000u;
			SIN_IRQ(nivel);
			t0 = reloj();
			jit_publicar(buf, tams[k]);
			t1 = reloj();
			CON_IRQ(nivel);
			if(t1 - t0 < mejor) mejor = t1 - t0;
		}
		len += (size_t)snprintf(l + len, sizeof(l) - len, " %u B %.1f", (unsigned)tams[k], (double)mejor / (double)(tams[k] / 32));
	}
	hle_log_quiet("%s\n", l);
}

//Arreglo
enum { D_VACIO, D_RECTA, D_B, D_BCTR, D_BCTR_PRONTO, D_BCTR_TARDE, D_MTLR, D_BL, D_LI_BL, D_BCTRL_C, D_BL_C, D_B_PRESUPUESTO,
       D_TABLA, D_N };

//Arreglo
#define G_PC(s) (0x08800000u + 4u * (s))

//Arreglo
static u32 *buscar_en_tabla(Emit *e){
	u32 *h;
	e_rlwinm(e, 0, 5, 1, 19, 28);
	e_add(e, 12, 4, 0);
	e_lwz(e, 11, 0, 12);
	e_lwz(e, 12, 4, 12);
	e_cmpw(e, 0, 11, 5);
	h = e_bc_hueco(e, BO_FALSO, CR_EQ);
	e_mtctr(e, 12);
	e_bctr(e);
	return h;
}

//Arreglo
static Fn generar_salto(u32 *buf, u32 palabras, int estilo, const u32 *orden, u32 *tabla, u32 *t2){
	Emit e;
	const u32 paso = estilo == D_TABLA ? 16 : 8;
	u32 *const huecos = buf + 4 * K_SALTOS + 64;
	u32 i, *hueco = NULL, *salida, *fallos[K_SALTOS + 1];
	e_ini(&e, buf, palabras);
	if(palabras < 4 * K_SALTOS + 64 + paso * K_SALTOS) return NULL;
	memset(fallos, 0, sizeof(fallos));
	prologo(&e);
	switch(estilo){
	case D_B_PRESUPUESTO:
		e_lis(&e, 9, 0x7FFF);
		hueco = e_b_hueco(&e);
		break;
	case D_TABLA:
		e_lis(&e, 5, (s32)(G_PC(orden[0]) >> 16));
		e_ori(&e, 5, 5, G_PC(orden[0]) & 0xFFFF);
		fallos[K_SALTOS] = buscar_en_tabla(&e);
		break;
	case D_RECTA:
		for(i = 0; i < K_SALTOS; i++) e_addi(&e, 3, 3, 1);
		break;
	case D_B:
		hueco = e_b_hueco(&e);
		break;
	case D_BCTR: case D_BCTR_PRONTO: case D_BCTR_TARDE: case D_MTLR:
		e_w(&e, PPC_D(33, 0, 4, 4));
		if(estilo == D_MTLR){ e_mtlr(&e, 0); e_blr(&e); }
		else { e_mtctr(&e, 0); e_bctr(&e); }
		break;
	case D_BL:
		for(i = 0; i < K_SALTOS; i++) e_bl(&e, huecos + paso * orden[i]);
		break;
	case D_LI_BL:
		for(i = 0; i < K_SALTOS; i++){ e_li(&e, 5, 1); e_bl(&e, huecos); }
		break;
	case D_BCTRL_C:
		for(i = 0; i < K_SALTOS; i++){ e_carga32(&e, 11, (u32)(uintptr_t)uno); e_mtctr(&e, 11); e_bctrl(&e); }
		break;
	case D_BL_C:
		if(!e_alcanza_b(e.p, (const void *)uno) || !e_alcanza_b(e.p + K_SALTOS, (const void *)uno)) return NULL;
		for(i = 0; i < K_SALTOS; i++) e_bl(&e, (const void *)uno);
		break;
	}
	salida = e.p;
	epilogo(&e);
	if(e.lleno) return NULL;
	if(estilo == D_LI_BL){
		huecos[0] = PPC_X(3, 3, 5, 266, 0);
		huecos[1] = BLR;
	}
	for(i = 0; i < K_SALTOS && estilo != D_LI_BL; i++){
		Emit b;
		u32 *const s = huecos + paso * orden[i];
		e_ini(&b, s, paso);
		switch(estilo){
		case D_B_PRESUPUESTO:
			e_addi(&b, 3, 3, 1);
			e_w(&b, PPC_D(13, 9, 9, -4));
			e_w(&b, ppc_bc(BO_CIERTO, CR_LT, (s32)((const u8 *)salida - (const u8 *)b.p)));
			e_b(&b, i + 1 < K_SALTOS ? huecos + paso * orden[i + 1] : salida);
			break;
		case D_TABLA: {
			const u32 g = i + 1 < K_SALTOS ? G_PC(orden[i + 1]) : G_PC(K_SALTOS);
			e_addi(&b, 3, 3, 1);
			e_lis(&b, 5, (s32)(g >> 16));
			e_ori(&b, 5, 5, g & 0xFFFF);
			fallos[i] = buscar_en_tabla(&b);
			break;
		}
		case D_B:
			e_addi(&b, 3, 3, 1);
			e_b(&b, i + 1 < K_SALTOS ? huecos + paso * orden[i + 1] : salida);
			break;
		case D_BCTR:
			e_addi(&b, 3, 3, 1);
			e_w(&b, PPC_D(33, 0, 4, 4));
			e_mtctr(&b, 0);
			e_bctr(&b);
			break;
		case D_BCTR_PRONTO:
			e_w(&b, PPC_D(33, 0, 4, 4));
			e_mtctr(&b, 0);
			e_addi(&b, 3, 3, 1); e_addi(&b, 6, 6, 1); e_addi(&b, 7, 7, 1); e_addi(&b, 8, 8, 1);
			e_bctr(&b);
			break;
		case D_BCTR_TARDE:
			e_addi(&b, 3, 3, 1); e_addi(&b, 6, 6, 1); e_addi(&b, 7, 7, 1); e_addi(&b, 8, 8, 1);
			e_w(&b, PPC_D(33, 0, 4, 4));
			e_mtctr(&b, 0);
			e_bctr(&b);
			break;
		case D_MTLR:
			e_addi(&b, 3, 3, 1);
			e_w(&b, PPC_D(33, 0, 4, 4));
			e_mtlr(&b, 0);
			e_blr(&b);
			break;
		case D_BL:
			e_addi(&b, 3, 3, 1);
			e_blr(&b);
			break;
		}
		if(tabla) tabla[i] = (u32)(uintptr_t)s;
		if(t2){ t2[2 * (orden[i] & 0x3FF)] = G_PC(orden[i]); t2[2 * (orden[i] & 0x3FF) + 1] = (u32)(uintptr_t)s; }
		if(b.lleno) return NULL;
	}
	if(tabla) tabla[K_SALTOS] = (u32)(uintptr_t)salida;
	if(t2){ t2[2 * K_SALTOS] = G_PC(K_SALTOS); t2[2 * K_SALTOS + 1] = (u32)(uintptr_t)salida; }
	for(i = 0; i <= K_SALTOS; i++) e_parchea_bc(fallos[i], salida);
	e_parchea_b(hueco, huecos + paso * orden[0]);
	jit_publicar(buf, (4 * K_SALTOS + 64 + paso * K_SALTOS) * 4);
	return (Fn)(uintptr_t)buf;
}

//Arreglo
static void prueba_saltos(u32 *buf, u32 bytes, const char *donde){
	static const char *const nom[D_N] = { "vacio", "recta (por addi)", "b", "lwzu+mtctr+bctr (tarjetas)", "mtctr pronto", "mtctr tarde",
	                                      "mtlr+blr", "bl+blr (llamadas)", "li+bl a la misma", "bctrl a C", "bl a C",
	                                      "b con presupuesto (addic.+blt)", "jr por tabla con etiqueta (lis/ori, rlwinm, lwz x2, cmpw, bne, mtctr, bctr)" };
	static u32 orden[K_SALTOS], tabla[K_SALTOS + 1];
	u32 *const t2 = (u32 *)(psp_mem.ram + 0x20000);
	u32 c[D_N], i, r;
	char l[1400];
	size_t len;
	for(i = 0; i < K_SALTOS; i++) orden[i] = i;
	for(i = K_SALTOS - 1; i > 0; i--){ const u32 j = azar() % (i + 1), t = orden[i]; orden[i] = orden[j]; orden[j] = t; }
	len = (size_t)snprintf(l, sizeof(l), "[PRUEBA_JIT] saltos en %s (%u bloques de 32 bytes, 64 en el de tabla, en orden al azar), ciclos por salto:", donde,
	                       (unsigned)K_SALTOS);
	for(i = 0; i < D_N; i++){
		const Fn f = generar_salto(buf, bytes / 4, (int)i, orden, tabla, i == D_TABLA ? t2 : NULL);
		const u32 bien = i == D_VACIO ? 0 : K_SALTOS;
		if(!f){ c[i] = 0; len += (size_t)snprintf(l + len, sizeof(l) - len, " %s -", nom[i]); continue; }
		c[i] = medir(f, 0, i == D_TABLA ? (u32)(uintptr_t)t2 : (u32)(uintptr_t)tabla - 4, 0, &r);
		comprobar(nom[i], r, bien);
		if(i) len += (size_t)snprintf(l + len, sizeof(l) - len, " %s %.2f", nom[i], (double)((s32)(c[i] - c[0])) / K_SALTOS);
		else len += (size_t)snprintf(l + len, sizeof(l) - len, " (vacio %u ciclos)", (unsigned)c[0]);
	}
	hle_log_quiet("%s\n", l);
	memset(t2, 0, 8 * 1024);
}

//Arreglo
enum { C_LWZ, C_LWBRX, C_LHA, C_LHBRX, C_LWZ4, C_LWBRX4, C_STW4, C_STWBRX4, C_N };

//Arreglo
static Fn generar_carga(u32 *buf, u32 palabras, int k, u32 n){
	Emit e;
	u32 i;
	e_ini(&e, buf, palabras);
	if(k == C_LWBRX4 || k == C_STWBRX4){ e_li(&e, 9, 0); e_li(&e, 10, 4); e_li(&e, 11, 8); e_li(&e, 12, 12); }
	for(i = 0; i < n; i++)
		switch(k){
		case C_LWZ: e_lwz(&e, 3, 0, 3); break;
		case C_LWBRX: e_lwbrx(&e, 3, 0, 3); break;
		case C_LHA: e_w(&e, PPC_D(42, 5, 3, 0)); e_add(&e, 3, 3, 5); break;
		case C_LHBRX: e_lhbrx(&e, 5, 0, 3); e_extsh(&e, 5, 5); e_add(&e, 3, 3, 5); break;
		case C_LWZ4: e_lwz(&e, 5, 0, 3); e_lwz(&e, 6, 4, 3); e_lwz(&e, 7, 8, 3); e_lwz(&e, 8, 12, 3); break;
		case C_LWBRX4: e_lwbrx(&e, 5, 3, 9); e_lwbrx(&e, 6, 3, 10); e_lwbrx(&e, 7, 3, 11); e_lwbrx(&e, 8, 3, 12); break;
		case C_STW4: e_stw(&e, 5, 0, 3); e_stw(&e, 6, 4, 3); e_stw(&e, 7, 8, 3); e_stw(&e, 8, 12, 3); break;
		case C_STWBRX4: e_stwbrx(&e, 5, 3, 9); e_stwbrx(&e, 6, 3, 10); e_stwbrx(&e, 7, 3, 11); e_stwbrx(&e, 8, 3, 12); break;
		}
	e_blr(&e);
	return publicar(&e);
}

//Arreglo
static void prueba_cargas(u32 *buf, u32 bytes, u8 *datos){
	static const char *const nom[C_N] = { "lwz (latencia)", "lwbrx (latencia)", "lha+add (latencia)", "lhbrx+extsh+add (latencia)",
	                                      "4 lwz", "4 lwbrx", "4 stw", "4 stwbrx" };
	const u32 n = 256;
	u32 *const d = (u32 *)datos;
	char l[600];
	size_t len = (size_t)snprintf(l, sizeof(l), "[PRUEBA_JIT] cargas (datos en %08X, en la L1), ciclos por grupo:", (unsigned)(uintptr_t)datos);
	u32 k, base, r;
	d[0] = (u32)(uintptr_t)d;
	d[8] = __builtin_bswap32((u32)(uintptr_t)(d + 8));
	d[16] = 0;
	d[17] = 0;
	for(k = 0; k < C_N; k++){
		const Fn f = generar_carga(buf, bytes / 4, (int)k, n);
		const u32 a = (u32)(uintptr_t)(k == C_LWZ ? d : k == C_LWBRX ? d + 8 : k == C_LHA || k == C_LHBRX ? d + 16 : d + 24);
		if(!f) continue;
		base = medir(f, a, 0, 0, &r);
		if(k <= C_LHBRX) comprobar(nom[k], r, a);
		len += (size_t)snprintf(l + len, sizeof(l) - len, " %s %.2f", nom[k], (double)base / n);
	}
	hle_log_quiet("%s\n", l);
}

//Arreglo
static void prueba_prediccion(u32 *buf, u32 bytes, u8 *datos){
	const u32 n = 4096;
	Emit e;
	u32 *lazo, i, r, c[2], m[2], unos = 0, nivel;
	Fn f;
	e_ini(&e, buf, bytes / 4);
	lazo = e.p;
	e_lbz(&e, 6, 0, 4);
	e_addi(&e, 4, 4, 1);
	e_cmpwi(&e, 0, 6, 0);
	e_w(&e, ppc_bc(BO_CIERTO, CR_EQ, 8));
	e_addi(&e, 3, 3, 1);
	e_w(&e, PPC_D(13, 5, 5, -1));
	bc_a(&e, BO_FALSO, CR_EQ, lazo);
	e_blr(&e);
	f = publicar(&e);
	if(!f) return;
	for(i = 0; i < n; i++){ datos[i] = (u8)(azar() & 1); unos += datos[i]; datos[n + i] = 1; }
	for(i = 0; i < 2; i++){
		u32 m0 = 0, m1 = 0;
		SIN_IRQ(nivel);
#if defined(GEKKO)
		m0 = MFSPR(958);
#endif
		f(0, (u32)(uintptr_t)(datos + i * n), n);
#if defined(GEKKO)
		m1 = MFSPR(958);
#endif
		CON_IRQ(nivel);
		m[i] = m1 - m0;
		c[i] = medir(f, 0, (u32)(uintptr_t)(datos + i * n), n, &r);
		comprobar(i ? "prediccion (unos)" : "prediccion (al azar)", r, i ? n : unos);
	}
	hle_log_quiet("[PRUEBA_JIT] prediccion: %u saltos al azar %u ciclos (%u mal predichos segun PMC4), siempre igual %u ciclos (%u mal); "
	              "%.1f ciclos por salto mal predicho\n", (unsigned)n, (unsigned)c[0], (unsigned)m[0], (unsigned)c[1], (unsigned)m[1],
	              m[0] > m[1] ? (double)((s32)(c[0] - c[1])) / (double)(m[0] - m[1]) : (double)((s32)(c[0] - c[1])) / (double)(n / 2));
}

//Arreglo
static volatile u32 sumidero;

//Arreglo
static u32 __attribute__((noinline)) perseguir(u32 *p, u32 n){
	u32 i;
	for(i = 0; i < n; i += 8){
		p = (u32 *)(uintptr_t)*p; p = (u32 *)(uintptr_t)*p; p = (u32 *)(uintptr_t)*p; p = (u32 *)(uintptr_t)*p;
		p = (u32 *)(uintptr_t)*p; p = (u32 *)(uintptr_t)*p; p = (u32 *)(uintptr_t)*p; p = (u32 *)(uintptr_t)*p;
	}
	return (u32)(uintptr_t)p;
}

//Arreglo
static void prueba_latencia(u8 *zona, u32 bytes, const char *donde){
	static const u32 tams[7] = { 16384, 65536, 262144, 1u << 20, 2u << 20, 4u << 20, 8u << 20 };
	char l[400];
	size_t len = (size_t)snprintf(l, sizeof(l), "[PRUEBA_JIT] latencia de la memoria en %s (%08X), ciclos por carga persiguiendo punteros:",
	                              donde, (unsigned)(uintptr_t)zona);
	u32 k;
	for(k = 0; k < 7 && tams[k] <= bytes; k++){
		const u32 lineas = tams[k] / 32, n = 16384;
		u32 i, x = 0, t0, t1, mejor = 0xFFFFFFFFu, v, nivel;
		for(i = 0; i < lineas; i++){
			const u32 y = (x * 1664525u + 1013904223u) & (lineas - 1);
			*(u32 *)(zona + 32 * x) = (u32)(uintptr_t)(zona + 32 * y);
			x = y;
		}
		sumidero += perseguir((u32 *)zona, n);
		for(v = 0; v < 3; v++){
			SIN_IRQ(nivel);
			t0 = reloj();
			sumidero += perseguir((u32 *)zona, n);
			t1 = reloj();
			CON_IRQ(nivel);
			if(t1 - t0 < mejor) mejor = t1 - t0;
		}
		len += (size_t)snprintf(l + len, sizeof(l) - len, " %u KB %.1f", (unsigned)(tams[k] / 1024), (double)mejor / n);
	}
	hle_log_quiet("%s\n", l);
}

//Arreglo
static void prueba_codigo(u32 *buf, u32 bytes, const char *donde){
	static const u32 tams[5] = { 16384, 131072, 1u << 20, 2u << 20, 4u << 20 };
	char l[400];
	size_t len = (size_t)snprintf(l, sizeof(l), "[PRUEBA_JIT] codigo en linea recta en %s, ciclos por instruccion (caliente):", donde);
	u32 k, r;
	for(k = 0; k < 5 && tams[k] <= bytes; k++){
		const u32 n = tams[k] / 4 - 1;
		u32 i, c;
		for(i = 0; i < n; i++) buf[i] = PPC_D(14, 3, 3, 1);
		buf[n] = BLR;
		jit_publicar(buf, tams[k]);
		c = medir((Fn)(uintptr_t)buf, 0, 0, 0, &r);
		comprobar("linea recta", r, n);
		len += (size_t)snprintf(l + len, sizeof(l) - len, " %u KB %.2f", (unsigned)(tams[k] / 1024), (double)c / n);
	}
	hle_log_quiet("%s\n", l);
}

//Arreglo
#define RT(op, rs, rt, rd, sa, fn) (((u32)(op) << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) | ((u32)(rd) << 11) | ((u32)(sa) << 6) | (u32)(fn))
#define IT(op, rs, rt, imm) (((u32)(op) << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) | ((u32)(imm) & 0xFFFFu))
#define JT(op, dest) (((u32)(op) << 26) | (((dest) >> 2) & 0x03FFFFFFu))
#define PB 0x08A00000u

//Arreglo
static void banco_cpu(void){
	static const char *const nom[4] = { "alu", "memoria", "llamadas", "ramas" };
	static const u32 alu[] = {
		RT(0, 2, 4, 2, 0, 0x21), RT(0, 3, 2, 3, 0, 0x26), RT(0, 0, 3, 8, 3, 0x00), IT(9, 5, 5, 0xFFFF),
		IT(5, 5, 0, 0xFFFB), RT(0, 4, 8, 4, 0, 0x21)
	};
	static const u32 memo[] = {
		IT(0x0F, 0, 4, 0x08B0), IT(0x0F, 0, 6, 0x08B1), IT(9, 0, 7, 256),
		IT(0x23, 4, 8, 0), IT(0x23, 4, 9, 4), IT(0x2B, 6, 8, 0), IT(0x2B, 6, 9, 4), IT(9, 7, 7, 0xFFFF), IT(9, 4, 4, 8),
		IT(5, 7, 0, 0xFFF9), IT(9, 6, 6, 8), JT(2, PB), 0
	};
	static const u32 llam[] = {
		JT(3, PB + 16), IT(9, 5, 5, 1), JT(2, PB), 0,
		RT(0, 2, 5, 2, 0, 0x21), RT(0, 3, 2, 3, 0, 0x26), RT(0, 31, 0, 0, 0, 0x08), 0
	};
	static const u32 ramas[] = {
		IT(0x0C, 4, 8, 1), IT(4, 8, 0, 2), IT(9, 4, 4, 7), IT(9, 2, 2, 1), RT(0, 0, 4, 9, 3, 0x02), RT(0, 3, 9, 3, 0, 0x21),
		JT(2, PB), RT(0, 4, 3, 4, 0, 0x26)
	};
	static const u32 *const progs[4] = { alu, memo, llam, ramas };
	static const u32 largos[4] = { sizeof(alu) / 4, sizeof(memo) / 4, sizeof(llam) / 4, sizeof(ramas) / 4 };
	static CpuState guardado, fin[2];
	const u8 jp = opt_on[OPT_JIT_PROTO], jv = opt_on[OPT_JIT_VERIFICAR];
	const u32 calentar = 50000, n = 2000000;
	char l[700];
	size_t len = (size_t)snprintf(l, sizeof(l), "[PRUEBA_JIT] CPU de la PSP (%u instrucciones), ciclos del Wii por instruccion, tarjetas / "
	                              "jit_proto:", (unsigned)n);
	u32 p, i, k;
	guardado = cpu;
	opt_on[OPT_JIT_VERIFICAR] = 0;
	for(p = 0; p < 4; p++){
		u32 c[2];
		for(i = 0; i < largos[p]; i++) mem_write32(PB + 4 * i, progs[p][i]);
		for(k = 0; k < 2; k++){
			u32 t0, t1;
			opt_on[OPT_JIT_PROTO] = (u8)k;
			cpu_predec_flush();
			memset(&cpu, 0, sizeof(cpu));
			cpu.r[4] = 3;
			cpu.r[5] = 0x7FFFFFFFu;
			cpu.r[29] = 0x09F00000u;
			cpu.pc = PB;
			cpu.npc = PB + 4;
			cpu_stop_requested = 0;
			cpu_run(calentar);
			t0 = reloj();
			cpu_run(n);
			t1 = reloj();
			c[k] = t1 - t0;
			fin[k] = cpu;
		}
		len += (size_t)snprintf(l + len, sizeof(l) - len, " %s %.1f / %.1f%s", nom[p], (double)c[0] / n, (double)c[1] / n,
		                        memcmp(fin[0].r, fin[1].r, sizeof(fin[0].r)) || fin[0].pc != fin[1].pc ? " (REGISTROS DISTINTOS)" : "");
		if(memcmp(fin[0].r, fin[1].r, sizeof(fin[0].r)) || fin[0].pc != fin[1].pc) malos++;
	}
	{
		JitStats j;
		jit_stats(&j);
		snprintf(l + len, sizeof(l) - len, "; %llu bloques compilados", (unsigned long long)j.compilados);
	}
	hle_log_quiet("%s\n", l);
	memset(psp_mem.ram + (PB - PSP_RAM_BASE), 0, 4096);
	memset(psp_mem.ram + (0x08B00000u - PSP_RAM_BASE), 0, 0x20000);
	opt_on[OPT_JIT_PROTO] = jp;
	opt_on[OPT_JIT_VERIFICAR] = jv;
	cpu = guardado;
	cpu_predec_flush();
}

#if defined(GEKKO)
//Arreglo
static PPCExcptPanicFn panic_viejo;
static volatile u32 g_dsi, g_isi, g_dar, g_dsisr, g_activo;
static u32 g_ini, g_fin;

//Arreglo
static void gancho(unsigned exid, PPCContext *ctx){
	if(g_activo && exid == PPC_EXCPT_DSI && ctx->pc >= g_ini && ctx->pc < g_fin){
		g_dsi++;
		g_dar = MFSPR(19);
		g_dsisr = MFSPR(18);
		ctx->gpr[3] = 0xD5100000u | (g_dsi & 0xFFFF);
		ctx->pc += 4;
		return;
	}
	if(g_activo && exid == PPC_EXCPT_ISI){
		g_isi++;
		ctx->gpr[3] = 0xBAD;
		ctx->pc = ctx->lr;
		return;
	}
	panic_viejo(exid, ctx);
}

//Arreglo
static void prueba_dsi(u32 *buf){
	Emit e;
	Fn f;
	u32 c_bien, c_mal, r, r2, d0;
	static u32 dato = 0x1234;
	e_ini(&e, buf, 16);
	e_lwz(&e, 3, 0, 4);
	e_blr(&e);
	f = publicar(&e);
	if(!f) return;
	g_ini = (u32)(uintptr_t)buf;
	g_fin = g_ini + 8;
	d0 = g_dsi;
	g_activo = 1;
	c_bien = medir(f, 0, (u32)(uintptr_t)&dato, 0, &r);
	c_mal = medir(f, 0, 0x0A000000u, 0, &r2);
	g_activo = 0;
	hle_log_quiet("[PRUEBA_JIT] DSI con PPCExcptCurPanicFn: lwz de 0x0A000000 da %08X (%u DSI, DAR %08X, DSISR %08X); la llamada "
	              "cuesta %u ciclos con la DSI y %u sin ella: %u por DSI de ida y vuelta\n", (unsigned)r2, (unsigned)(g_dsi - d0),
	              (unsigned)g_dar, (unsigned)g_dsisr, (unsigned)c_mal, (unsigned)c_bien, (unsigned)(c_mal - c_bien));
	comprobar("lwz normal", r, 0x1234);
	if(g_dsi - d0 != 9) malos++;
	hle_log_sync();
}

//Arreglo
static u32 cadena(u8 *zona, u32 bytes, u32 base_punteros){
	const u32 lineas = bytes / 32, n = 16384;
	u32 i, x = 0, t0, t1, mejor = 0xFFFFFFFFu, v, nivel;
	for(i = 0; i < lineas; i++){
		const u32 y = (x * 1664525u + 1013904223u) & (lineas - 1);
		*(u32 *)(zona + 32 * x) = base_punteros + 32 * y;
		x = y;
	}
	sumidero += perseguir((u32 *)(uintptr_t)base_punteros, n);
	for(v = 0; v < 3; v++){
		SIN_IRQ(nivel);
		t0 = reloj();
		sumidero += perseguir((u32 *)(uintptr_t)base_punteros, n);
		t1 = reloj();
		CON_IRQ(nivel);
		if(t1 - t0 < mejor) mejor = t1 - t0;
	}
	return mejor;
}

//Arreglo
static void prueba_dbat(void){
	const u32 hid4 = MFSPR(1011), sbe = (hid4 & 0x02000000u) != 0;
	const u32 u6 = sbe ? MFSPR(572) : 0, l6 = sbe ? MFSPR(573) : 0, u7 = sbe ? MFSPR(574) : 0, l7 = sbe ? MFSPR(575) : 0;
	const u32 u4 = sbe ? MFSPR(568) : 0, l4 = sbe ? MFSPR(569) : 0;
	const u32 ram = (u32)(uintptr_t)psp_mem.ram, fis = (l4 & 0xFFFE0000u) + (ram - (u4 & 0xFFFE0000u));
	const u32 brpn = fis & ~0x01FFFFFFu, desp = fis - brpn, tam = psp_mem.ram_size;
	const int con7 = !(u7 & 3);
	u32 nivel, i, mal = 0, escrito, c_bat, c_ram, mitad, ultima = 0, s1 = 0, s2 = 0;
	volatile u32 *const alias = (volatile u32 *)(uintptr_t)(0x08000000u + desp);
	const u32 *const ram32 = (const u32 *)psp_mem.ram;
	if(!sbe || (u6 & 3) || !(u4 & 2) || ram < (u4 & 0xFFFE0000u) || (l4 & 0x28) || desp >= 0x20000u){
		hle_log_quiet("[PRUEBA_JIT] DBAT6: no se prueba (HID4 %08X, DBAT6 %08X %08X, DBAT7 %08X %08X, DBAT4 %08X %08X)\n", (unsigned)hid4,
		              (unsigned)u6, (unsigned)l6, (unsigned)u7, (unsigned)l7, (unsigned)u4, (unsigned)l4);
		return;
	}
	for(i = 0; i < 1024; i++) ((u32 *)psp_mem.ram)[i] = i * 2654435761u;
	((u32 *)psp_mem.ram)[(16u << 20) / 4] = 0x16161616u;
	((u32 *)psp_mem.ram)[tam / 4 - 1] = 0x1A57u;
	SIN_IRQ(nivel);
	__asm__ volatile("sync" ::: "memory");
	MTSPR(572, 0);
	MTSPR(573, brpn | (l4 & 0x7Fu));
	MTSPR(572, 0x08000000u | (0xFFu << 2) | 2u);
	if(con7){
		MTSPR(574, 0);
		MTSPR(575, (brpn + (32u << 20)) | (l4 & 0x7Fu));
		MTSPR(574, 0x0A000000u | 2u);
	}
	__asm__ volatile("isync" ::: "memory");
	for(i = 0; i < 1024; i++) if(alias[i] != ram32[i]) mal++;
	mitad = alias[(16u << 20) / 4];
	if(con7) ultima = alias[tam / 4 - 1];
	alias[5] = 0xCAFEF00Du;
	escrito = ram32[5];
	for(i = 0; i < 1024; i++){ s1 += alias[i]; s2 += ((volatile const u32 *)ram32)[i]; }
	CON_IRQ(nivel);
	c_bat = cadena(psp_mem.ram + (4u << 20), 1u << 20, 0x08000000u + desp + (4u << 20));
	c_ram = cadena(psp_mem.ram + (4u << 20), 1u << 20, ram + (4u << 20));
	SIN_IRQ(nivel);
	__asm__ volatile("sync" ::: "memory");
	MTSPR(572, 0);
	MTSPR(573, l6);
	MTSPR(572, u6);
	if(con7){
		MTSPR(574, 0);
		MTSPR(575, l7);
		MTSPR(574, u7);
	}
	__asm__ volatile("isync" ::: "memory");
	CON_IRQ(nivel);
	hle_log_quiet("[PRUEBA_JIT] DBAT6 (32 MB en 0x08000000 -> fisica %08X) y %s: la RAM de la PSP empieza %u bytes despues (en el JIT, "
	              "direccion + %u); %u de 1024 palabras distintas, a 16 MB %s, la ultima palabra %s; escribir por el alias se ve en la "
	              "RAM: %s (sumas %s); 1 MB persiguiendo punteros: %.1f ciclos por carga por el BAT y %.1f por la RAM; DBAT6 y 7 como "
	              "estaban: %s\n", (unsigned)brpn, con7 ? "DBAT7 (128 KB en 0x0A000000, el final)" : "sin DBAT7 (ocupado)",
	              (unsigned)desp, (unsigned)desp, (unsigned)mal, mitad == 0x16161616u ? "bien" : "MAL",
	              !con7 ? "sin probar" : ultima == 0x1A57u ? "bien" : "MAL", escrito == 0xCAFEF00Du ? "si" : "NO",
	              s1 == s2 ? "iguales" : "DISTINTAS", (double)c_bat / 16384.0, (double)c_ram / 16384.0,
	              MFSPR(572) == u6 && MFSPR(573) == l6 && MFSPR(574) == u7 && MFSPR(575) == l7 ? "si" : "NO");
	if(mal || mitad != 0x16161616u || (con7 && ultima != 0x1A57u) || escrito != 0xCAFEF00Du || s1 != s2) malos++;
	memset(psp_mem.ram, 0, 4096);
	memset(psp_mem.ram + (4u << 20), 0, 1u << 20);
	((u32 *)psp_mem.ram)[(16u << 20) / 4] = 0;
	((u32 *)psp_mem.ram)[tam / 4 - 1] = 0;
	hle_log_sync();
}

//Arreglo
static void prueba_ibat(u8 *arena, u32 bytes){
	const u32 hid4 = MFSPR(1011);
	const u32 c = ((u32)(uintptr_t)arena + 0x3FFFFu) & ~0x3FFFFu, alias = 0x7FFC0000u;
	u32 u[8], lo[8], k, libre = 8, wimg = 0, fis = 0, nivel, r1 = 0, r2 = 0, leido_u, leido_l, i0;
	u32 *const w = (u32 *)(uintptr_t)c;
	Emit e;
	u[0] = MFSPR(528); lo[0] = MFSPR(529); u[1] = MFSPR(530); lo[1] = MFSPR(531);
	u[2] = MFSPR(532); lo[2] = MFSPR(533); u[3] = MFSPR(534); lo[3] = MFSPR(535);
	k = 4;
	if(hid4 & 0x02000000u){
		u[4] = MFSPR(560); lo[4] = MFSPR(561); u[5] = MFSPR(562); lo[5] = MFSPR(563);
		u[6] = MFSPR(564); lo[6] = MFSPR(565); u[7] = MFSPR(566); lo[7] = MFSPR(567);
		k = 8;
	}
	for(i0 = 0; i0 < k; i0++){
		const u32 ini = u[i0] & 0xFFFE0000u, tam = (((u[i0] >> 2) & 0x7FFu) + 1) << 17;
		if(!(u[i0] & 3) && !lo[i0] && libre == 8 && i0 != 0 && i0 != 1) libre = i0;
		if((u[i0] & 2) && c >= ini && c - ini < tam){ wimg = lo[i0] & 0x7Fu; fis = (lo[i0] & 0xFFFE0000u) + (c - ini); }
	}
	if(!arena || bytes < 0x80000u || libre == 8 || !fis || (fis & 0x3FFFFu) || (wimg & 0x28)){
		hle_log_quiet("[PRUEBA_JIT] IBAT: no se prueba (arena %08X, IBAT libre %u, WIMG %02X)\n", (unsigned)(uintptr_t)arena, (unsigned)libre,
		              (unsigned)wimg);
		return;
	}
	e_ini(&e, w, 64);
	e_li(&e, 3, 0x77);
	e_blr(&e);
	prologo(&e);
	e_li(&e, 3, 41);
	{
		const s32 d = (s32)((u32)(uintptr_t)uno - (alias + 4 * (u32)(e.p - w)));
		if(d < -0x02000000 || d >= 0x02000000){ hle_log_quiet("[PRUEBA_JIT] IBAT: no se prueba (C en %08X, lejos del alias)\n", (unsigned)(uintptr_t)uno); return; }
		e_w(&e, ppc_b(d, 1));
	}
	epilogo(&e);
	if(e.lleno) return;
	jit_publicar(w, 64 * 4);
	g_activo = 1;
	SIN_IRQ(nivel);
	__asm__ volatile("sync" ::: "memory");
	switch(libre){
	case 2: MTSPR(532, 0); MTSPR(533, fis | wimg); MTSPR(532, alias | (1u << 2) | 2u); break;
	case 3: MTSPR(534, 0); MTSPR(535, fis | wimg); MTSPR(534, alias | (1u << 2) | 2u); break;
	case 4: MTSPR(560, 0); MTSPR(561, fis | wimg); MTSPR(560, alias | (1u << 2) | 2u); break;
	case 5: MTSPR(562, 0); MTSPR(563, fis | wimg); MTSPR(562, alias | (1u << 2) | 2u); break;
	case 6: MTSPR(564, 0); MTSPR(565, fis | wimg); MTSPR(564, alias | (1u << 2) | 2u); break;
	default: MTSPR(566, 0); MTSPR(567, fis | wimg); MTSPR(566, alias | (1u << 2) | 2u); break;
	}
	__asm__ volatile("isync" ::: "memory");
	switch(libre){
	case 2: leido_u = MFSPR(532); leido_l = MFSPR(533); break;
	case 3: leido_u = MFSPR(534); leido_l = MFSPR(535); break;
	case 4: leido_u = MFSPR(560); leido_l = MFSPR(561); break;
	case 5: leido_u = MFSPR(562); leido_l = MFSPR(563); break;
	case 6: leido_u = MFSPR(564); leido_l = MFSPR(565); break;
	default: leido_u = MFSPR(566); leido_l = MFSPR(567); break;
	}
	if(leido_u == (alias | (1u << 2) | 2u) && leido_l == (fis | wimg)){
		r1 = ((Fn)(uintptr_t)alias)(0, 0, 0);
		r2 = ((Fn)(uintptr_t)(alias + 8))(0, 0, 0);
	}
	__asm__ volatile("sync" ::: "memory");
	switch(libre){
	case 2: MTSPR(532, 0); MTSPR(533, 0); break;
	case 3: MTSPR(534, 0); MTSPR(535, 0); break;
	case 4: MTSPR(560, 0); MTSPR(561, 0); break;
	case 5: MTSPR(562, 0); MTSPR(563, 0); break;
	case 6: MTSPR(564, 0); MTSPR(565, 0); break;
	default: MTSPR(566, 0); MTSPR(567, 0); break;
	}
	__asm__ volatile("isync" ::: "memory");
	CON_IRQ(nivel);
	g_activo = 0;
	hle_log_quiet("[PRUEBA_JIT] IBAT%u: alias %08X (256 KB) del codigo en %08X (fisica %08X, WIMG %02X), leido %08X %08X: li+blr da %X "
	              "(bien: 77), bl a C (en MEM1) desde el alias da %u (bien: 42); ISI %u\n", (unsigned)libre, (unsigned)alias, (unsigned)c,
	              (unsigned)fis, (unsigned)wimg, (unsigned)leido_u, (unsigned)leido_l, (unsigned)r1, (unsigned)r2,
	              (unsigned)g_isi);
	if(r1 != 0x77 || r2 != 42) malos++;
	hle_log_sync();
}
#endif

//Arreglo
void jit_banco(void){
	const u64 t_ini = reloj();
	u32 bytes2 = 0, bytes1 = 256u << 10, nivel, a, b;
	u8 *arena = (u8 *)jit_arena_lista(&bytes2), *m1 = NULL;
	malos = 0;
	SIN_IRQ(nivel);
#if defined(GEKKO)
	a = MFSPR(953);
	{ volatile u32 i; for(i = 0; i < 1000; i++) ; }
	b = MFSPR(953);
	con_pmc = b - a > 1000 && b - a < 100000;
#else
	a = b = 0;
#endif
	CON_IRQ(nivel);
#if defined(GEKKO)
	m1 = (u8 *)memalign(32, 2u << 20);
	if(m1 && ((u32)(uintptr_t)m1 < 0x80000000u || (u32)(uintptr_t)m1 + (2u << 20) > 0x81800000u)){ free(m1); m1 = NULL; }
#else
	m1 = mmap(NULL, 2u << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(m1 == MAP_FAILED) m1 = NULL;
#endif
	hle_log_quiet("[PRUEBA_JIT] microprobas del Broadway para el JIT: reloj %s (%u en 1000 vueltas); arena del JIT %08X (%u KB), "
	              "zona de MEM1 %08X (2 MB), RAM de la PSP %08X\n", con_pmc ? "PMC1 (ciclos)" : "base de tiempo x 12", (unsigned)(b - a),
	              (unsigned)(uintptr_t)arena, (unsigned)(bytes2 / 1024), (unsigned)(uintptr_t)m1, (unsigned)(uintptr_t)psp_mem.ram);
	hle_log_sync();
	if(m1) prueba_publicar((u32 *)m1, "MEM1");
	if(arena) prueba_publicar((u32 *)arena, "MEM2 (arena del JIT)");
	hle_log_sync();
	if(m1) prueba_coste_publicar((u32 *)m1, bytes1, "MEM1");
	if(arena) prueba_coste_publicar((u32 *)arena, bytes2, "MEM2");
	if(m1) prueba_saltos((u32 *)m1, bytes1, "MEM1");
	if(arena) prueba_saltos((u32 *)arena, bytes2, "MEM2");
	hle_log_sync();
	if(arena) prueba_cargas((u32 *)arena, bytes2, psp_mem.ram);
	if(arena) prueba_prediccion((u32 *)arena, bytes2, psp_mem.ram);
	hle_log_sync();
	if(m1) prueba_latencia(m1, 2u << 20, "MEM1");
	prueba_latencia(psp_mem.ram, 8u << 20, "MEM2 (RAM de la PSP)");
	memset(psp_mem.ram, 0, 8u << 20);
	hle_log_sync();
	if(m1) prueba_codigo((u32 *)m1, 2u << 20, "MEM1");
	if(arena) prueba_codigo((u32 *)arena, bytes2, "MEM2");
	hle_log_sync();
	banco_cpu();
	hle_log_sync();
#if defined(GEKKO)
	panic_viejo = PPCExcptCurPanicFn;
	PPCExcptCurPanicFn = gancho;
	if(arena) prueba_dsi((u32 *)arena);
	prueba_dbat();
	prueba_ibat(arena, bytes2);
	PPCExcptCurPanicFn = panic_viejo;
#endif
	jit_vaciar();
	if(arena) memset(arena, 0, bytes2 < (1u << 20) ? bytes2 : (1u << 20));
#if defined(GEKKO)
	free(m1);
#else
	if(m1) munmap(m1, 2u << 20);
#endif
	hle_log_quiet("[PRUEBA_JIT] fin: %s (%.0f ms)\n", malos ? "HAY RESULTADOS MAL" : "todo bien",
	              (double)(u32)(reloj() - (u32)t_ini) / 729000.0);
	hle_log_sync();
}
