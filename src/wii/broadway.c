/**
 * WIISP - broadway.c
 * Qué tiene configurado el CPU del Wii al arrancar, para wiisp.log.
 *
 * Sirve para decidir antes de diseñar dos optimizaciones de la hoja de ruta:
 *   - 2.5 (mem_bat): mapear la RAM de la PSP en 0x08000000 con dos BAT de
 *     datos. Hace falta saber qué pares DBAT deja libres libogc (Vs = Vp = 0
 *     en la mitad alta) y si los BAT 4-7 están activos (HID4[SBE]).
 *   - 1.2b (ge_vdec_psq): convertir vértices con psq_l. Hace falta saber
 *     qué GQR usa ya alguien (GQR0 debe seguir en 0 para el compilador).
 * También se apuntan SDR1 (tabla de páginas: 0 = ninguna) y las arenas de
 * MEM1 y MEM2.
 *
 * Solo lee registros: no cambia nada.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <string.h>   //Arreglo
#include <gccore.h>
#include <ogc/lwp_watchdog.h>   //Arreglo
#include "wii/wii.h"
#include "hle/hle.h"
#include "core/prof.h"

#define MFSPR(n) ({ u32 v_; __asm__ volatile("mfspr %0, %1" : "=r"(v_) : "i"(n)); v_; })
#define MTSPR(n, v) __asm__ volatile("mtspr %0, %1" : : "i"(n), "r"((u32)(v)))

#ifndef HID4_SBE
#define HID4_SBE 0x02000000u   /* bit 6: BAT 4-7 activos */
#endif

/* Una pareja BAT: dirección efectiva, tamaño, física, WIMG y protección */
static void log_bat(const char *name, int i, u32 up, u32 lo, int *libre){
	u32 bl = (up >> 2) & 0x7FF, size_kb = (bl + 1) * 128;
	if(!(up & 3)){
		hle_log_quiet("[BROADWAY] %s%d %08X %08X  libre\n", name, i, up, lo);
		*libre = 1;
		return;
	}
	*libre = 0;
	hle_log_quiet("[BROADWAY] %s%d %08X %08X  %08X-%08X (%u %s) -> fisica %08X, WIMG %c%c%c%c, PP %u, %s%s\n",
	              name, i, up, lo, up & 0xFFFE0000u, (up & 0xFFFE0000u) + size_kb * 1024u - 1,
	              size_kb >= 1024 ? size_kb / 1024 : size_kb, size_kb >= 1024 ? "MB" : "KB", lo & 0xFFFE0000u,
	              lo & 0x40 ? 'W' : '-', lo & 0x20 ? 'I' : '-', lo & 0x10 ? 'M' : '-', lo & 0x08 ? 'G' : '-',
	              lo & 3, up & 2 ? "Vs" : "", up & 1 ? " Vp" : "");
}

void broadway_report(void){
	u32 hid4 = MFSPR(1011), msr;
	u32 dbat[8][2], ibat[8][2], gqr[8];
	int n = (hid4 & HID4_SBE) ? 8 : 4, i, libre;
	char dfree[64] = "", ifree[64] = "";
	size_t dl = 0, il = 0;

	__asm__ volatile("mfmsr %0" : "=r"(msr));
	dbat[0][0] = MFSPR(536); dbat[0][1] = MFSPR(537); dbat[1][0] = MFSPR(538); dbat[1][1] = MFSPR(539);
	dbat[2][0] = MFSPR(540); dbat[2][1] = MFSPR(541); dbat[3][0] = MFSPR(542); dbat[3][1] = MFSPR(543);
	ibat[0][0] = MFSPR(528); ibat[0][1] = MFSPR(529); ibat[1][0] = MFSPR(530); ibat[1][1] = MFSPR(531);
	ibat[2][0] = MFSPR(532); ibat[2][1] = MFSPR(533); ibat[3][0] = MFSPR(534); ibat[3][1] = MFSPR(535);
	if(n == 8){
		/* Solo se pueden leer con HID4[SBE] activo */
		dbat[4][0] = MFSPR(568); dbat[4][1] = MFSPR(569); dbat[5][0] = MFSPR(570); dbat[5][1] = MFSPR(571);
		dbat[6][0] = MFSPR(572); dbat[6][1] = MFSPR(573); dbat[7][0] = MFSPR(574); dbat[7][1] = MFSPR(575);
		ibat[4][0] = MFSPR(560); ibat[4][1] = MFSPR(561); ibat[5][0] = MFSPR(562); ibat[5][1] = MFSPR(563);
		ibat[6][0] = MFSPR(564); ibat[6][1] = MFSPR(565); ibat[7][0] = MFSPR(566); ibat[7][1] = MFSPR(567);
	}
	gqr[0] = MFSPR(912); gqr[1] = MFSPR(913); gqr[2] = MFSPR(914); gqr[3] = MFSPR(915);
	gqr[4] = MFSPR(916); gqr[5] = MFSPR(917); gqr[6] = MFSPR(918); gqr[7] = MFSPR(919);

	hle_log_quiet("[BROADWAY] PVR %08X MSR %08X HID0 %08X HID1 %08X HID2 %08X HID4 %08X (BAT 4-7 %s) L2CR %08X SDR1 %08X%s\n",
	              MFSPR(287), msr, MFSPR(1008), MFSPR(1009), MFSPR(920), hid4, n == 8 ? "activos" : "apagados",
	              MFSPR(1017), MFSPR(25), MFSPR(25) ? "" : " (sin tabla de paginas)");
	for(i = 0; i < n; i++){
		log_bat("DBAT", i, dbat[i][0], dbat[i][1], &libre);
		if(libre && dl + 4 < sizeof(dfree)) dl += (size_t)snprintf(dfree + dl, sizeof(dfree) - dl, " %d", i);
	}
	for(i = 0; i < n; i++){
		log_bat("IBAT", i, ibat[i][0], ibat[i][1], &libre);
		if(libre && il + 4 < sizeof(ifree)) il += (size_t)snprintf(ifree + il, sizeof(ifree) - il, " %d", i);
	}
	hle_log_quiet("[BROADWAY] BAT libres: DBAT%s; IBAT%s\n", dl ? dfree : " ninguno", il ? ifree : " ninguno");
	hle_log_quiet("[BROADWAY] GQR0-7: %08X %08X %08X %08X %08X %08X %08X %08X%s\n",
	              gqr[0], gqr[1], gqr[2], gqr[3], gqr[4], gqr[5], gqr[6], gqr[7],
	              gqr[0] ? " (GQR0 no es 0!)" : "");
	hle_log_quiet("[BROADWAY] MEM1 libre %08X-%08X, MEM2 libre %08X-%08X\n",
	              (u32)SYS_GetArena1Lo(), (u32)SYS_GetArena1Hi(), (u32)SYS_GetArena2Lo(), (u32)SYS_GetArena2Hi());
	hle_log_sync();
}

/* --- Contadores de rendimiento (prof_pmc, 0.2) ------------------------------
   MMCR0 (SPR 952) elige los eventos de PMC1 (bits 19-25) y PMC2 (26-31);
   MMCR1 (956) los de PMC3 (0-4) y PMC4 (5-9). Eventos del manual del
   750CL: PMC1 1 = ciclos, PMC2 2 = instrucciones terminadas, PMC3 5 =
   fallos de la caché de datos L1, PMC4 8 = saltos mal predichos. Todo
   corre en modo supervisor, así que se leen los registros directamente. */
static void pmc_read(u32 *out){
	out[PMC_CICLOS] = MFSPR(953);
	out[PMC_INSTR] = MFSPR(954);
	out[PMC_FALLOS_L1D] = MFSPR(957);
	out[PMC_SALTOS_MAL] = MFSPR(958);
}

//arreglo
/* Lo que cuentan PMC3 y PMC4 en cada modo (prof_pmc_l2, prof_pmc_ctr,
   prof_pmc_carga; tabla de eventos del 750): 0 = fallos de L1D (5) y saltos
   mal predichos (8); 1 = fallos de datos en L2 (7) y expulsiones de L2 (5);
   2 = ciclos con la unidad de saltos parada esperando LR o CTR (17) y
   saltos mal; 3 = ciclos de fallos de carga en L1D (15) y saltos mal.
   PMC1 y PMC2 siguen con ciclos e instrucciones. */
static void pmc_modo(int modo){
	static const u8 ev3[4] = { 5, 7, 17, 15 }, ev4[4] = { 8, 5, 8, 8 };
	if((unsigned)modo > 3) modo = 0;
	MTSPR(952, 0x80000000u);   /* FC: parados mientras se configuran */
	MTSPR(953, 0); MTSPR(954, 0); MTSPR(957, 0); MTSPR(958, 0);
	MTSPR(956, ((u32)ev3[modo] << 27) | ((u32)ev4[modo] << 22));
	MTSPR(952, (1u << 6) | 2u);
}

void broadway_pmc_init(void){
	pmc_modo(0);   //arreglo
	prof_pmc_read = pmc_read;
	prof_pmc_modo_fn = pmc_modo;   //arreglo
}

#define PSQ_L(r, p) __asm__ volatile("psq_l %0,0(%1),0,0" : "=f"(r) : "b"(p))   //Arreglo
#define PSQ_ST(r, p) __asm__ volatile("psq_st %0,0(%1),0,0" : : "f"(r), "b"(p) : "memory")   //Arreglo
#define PS3(ins, d, a, c, b) __asm__ volatile(ins " %0,%1,%2,%3" : "=f"(d) : "f"(a), "f"(c), "f"(b))   //Arreglo
#define PS2(ins, d, a, b) __asm__ volatile(ins " %0,%1,%2" : "=f"(d) : "f"(a), "f"(b))   //Arreglo
#define FS3(ins, a, c, b) ({ float d_; __asm__ volatile(ins " %0,%1,%2,%3" : "=f"(d_) : "f"(a), "f"(c), "f"(b)); d_; })   //Arreglo
#define FS2(ins, a, b) ({ float d_; __asm__ volatile(ins " %0,%1,%2" : "=f"(d_) : "f"(a), "f"(b)); d_; })   //Arreglo
#define PS1(ins, d, b) __asm__ volatile(ins " %0,%1" : "=f"(d) : "f"(b))   //Arreglo
#define FS1(ins, b) ({ float d_; __asm__ volatile(ins " %0,%1" : "=f"(d_) : "f"(b)); d_; })   //Arreglo

enum { PSP_ADD, PSP_SUB, PSP_MUL, PSP_MADD, PSP_MSUB, PSP_NMADD, PSP_NMSUB, PSP_SUM0, PSP_SUM1, PSP_MULS0, PSP_MULS1,
       PSP_MADDS0, PSP_MADDS1, PSP_LS, PSP_MERGE00, PSP_MERGE01, PSP_MERGE10, PSP_MERGE11, PSP_MR, PSP_NEG, PSP_ABS, PSP_NABS,
       PSP_DIV, PSP_SEL, PSP_N };   //Arreglo
static const char *const psp_nombre[PSP_N] = { "ps_add", "ps_sub", "ps_mul", "ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub",
                                               "ps_sum0", "ps_sum1", "ps_muls0", "ps_muls1", "ps_madds0", "ps_madds1",
                                               "psq_l+psq_st", "ps_merge00", "ps_merge01", "ps_merge10", "ps_merge11", "ps_mr",
                                               "ps_neg", "ps_abs", "ps_nabs", "ps_div", "ps_sel" };   //Arreglo

static u32 psp_bits(float f){ u32 u; memcpy(&u, &f, 4); return u; }   //Arreglo
static float psp_f(u32 u){ float f; memcpy(&f, &u, 4); return f; }   //Arreglo

static void __attribute__((noinline)) psp_pares(const float *a, const float *b, const float *c, float out[PSP_N][2]){   //Arreglo
	double fa, fb, fc, d;
	PSQ_L(fa, a); PSQ_L(fb, b); PSQ_L(fc, c);
	PS2("ps_add", d, fa, fb); PSQ_ST(d, out[PSP_ADD]);
	PS2("ps_sub", d, fa, fb); PSQ_ST(d, out[PSP_SUB]);
	PS2("ps_mul", d, fa, fc); PSQ_ST(d, out[PSP_MUL]);
	PS3("ps_madd", d, fa, fc, fb); PSQ_ST(d, out[PSP_MADD]);
	PS3("ps_msub", d, fa, fc, fb); PSQ_ST(d, out[PSP_MSUB]);
	PS3("ps_nmadd", d, fa, fc, fb); PSQ_ST(d, out[PSP_NMADD]);
	PS3("ps_nmsub", d, fa, fc, fb); PSQ_ST(d, out[PSP_NMSUB]);
	PS3("ps_sum0", d, fa, fc, fb); PSQ_ST(d, out[PSP_SUM0]);
	PS3("ps_sum1", d, fa, fc, fb); PSQ_ST(d, out[PSP_SUM1]);
	PS2("ps_muls0", d, fa, fc); PSQ_ST(d, out[PSP_MULS0]);
	PS2("ps_muls1", d, fa, fc); PSQ_ST(d, out[PSP_MULS1]);
	PS3("ps_madds0", d, fa, fc, fb); PSQ_ST(d, out[PSP_MADDS0]);
	PS3("ps_madds1", d, fa, fc, fb); PSQ_ST(d, out[PSP_MADDS1]);
	PSQ_ST(fa, out[PSP_LS]);   //Arreglo
	PS2("ps_merge00", d, fa, fb); PSQ_ST(d, out[PSP_MERGE00]);   //Arreglo
	PS2("ps_merge01", d, fa, fb); PSQ_ST(d, out[PSP_MERGE01]);   //Arreglo
	PS2("ps_merge10", d, fa, fb); PSQ_ST(d, out[PSP_MERGE10]);   //Arreglo
	PS2("ps_merge11", d, fa, fb); PSQ_ST(d, out[PSP_MERGE11]);   //Arreglo
	PS1("ps_mr", d, fb); PSQ_ST(d, out[PSP_MR]);   //Arreglo
	PS1("ps_neg", d, fb); PSQ_ST(d, out[PSP_NEG]);   //Arreglo
	PS1("ps_abs", d, fb); PSQ_ST(d, out[PSP_ABS]);   //Arreglo
	PS1("ps_nabs", d, fb); PSQ_ST(d, out[PSP_NABS]);   //Arreglo
	PS2("ps_div", d, fa, fb); PSQ_ST(d, out[PSP_DIV]);   //Arreglo
	PS3("ps_sel", d, fa, fc, fb); PSQ_ST(d, out[PSP_SEL]);   //Arreglo
}

static void __attribute__((noinline)) psp_escalares(const float *a, const float *b, const float *c, float out[PSP_N][2]){   //Arreglo
	int i;
	for(i = 0; i < 2; i++){
		float x = a[i], y = b[i], z = c[i];
		out[PSP_ADD][i] = FS2("fadds", x, y);
		out[PSP_SUB][i] = FS2("fsubs", x, y);
		out[PSP_MUL][i] = FS2("fmuls", x, z);
		out[PSP_MADD][i] = FS3("fmadds", x, z, y);
		out[PSP_MSUB][i] = FS3("fmsubs", x, z, y);
		out[PSP_NMADD][i] = FS3("fnmadds", x, z, y);
		out[PSP_NMSUB][i] = FS3("fnmsubs", x, z, y);
		out[PSP_MULS0][i] = FS2("fmuls", x, c[0]);
		out[PSP_MULS1][i] = FS2("fmuls", x, c[1]);
		out[PSP_MADDS0][i] = FS3("fmadds", x, c[0], y);
		out[PSP_MADDS1][i] = FS3("fmadds", x, c[1], y);
		memcpy(&out[PSP_LS][i], &a[i], 4);   //Arreglo
		memcpy(&out[PSP_MR][i], &b[i], 4);   //Arreglo
		out[PSP_NEG][i] = FS1("fneg", y);   //Arreglo
		out[PSP_ABS][i] = FS1("fabs", y);   //Arreglo
		out[PSP_NABS][i] = FS1("fnabs", y);   //Arreglo
		out[PSP_DIV][i] = FS2("fdivs", x, y);   //Arreglo
		out[PSP_SEL][i] = FS3("fsel", x, z, y);   //Arreglo
	}
	memcpy(&out[PSP_MERGE00][0], &a[0], 4); memcpy(&out[PSP_MERGE00][1], &b[0], 4);   //Arreglo
	memcpy(&out[PSP_MERGE01][0], &a[0], 4); memcpy(&out[PSP_MERGE01][1], &b[1], 4);   //Arreglo
	memcpy(&out[PSP_MERGE10][0], &a[1], 4); memcpy(&out[PSP_MERGE10][1], &b[0], 4);   //Arreglo
	memcpy(&out[PSP_MERGE11][0], &a[1], 4); memcpy(&out[PSP_MERGE11][1], &b[1], 4);   //Arreglo
	out[PSP_SUM0][0] = FS2("fadds", a[0], b[1]);
	out[PSP_SUM0][1] = c[1];
	out[PSP_SUM1][0] = c[0];
	out[PSP_SUM1][1] = FS2("fadds", a[0], b[1]);
}

static u32 psp_rs = 0x2545F491u;   //Arreglo
static u32 psp_rnd(void){ psp_rs ^= psp_rs << 13; psp_rs ^= psp_rs >> 17; psp_rs ^= psp_rs << 5; return psp_rs; }   //Arreglo

static u32 psp_valor(void){   //Arreglo
	static const u32 raros[] = { 0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u,
	                             0x7FA00000u, 0x7FC12345u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00800000u,
	                             0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u, 0xBF800000u, 0x33800000u, 0x34000000u,
	                             0x42480000u, 0xBC88CC38u, 0x1B1C72A0u, 0x00400000u, 0x0C000000u, 0x72000000u };
	u32 r = psp_rnd(), k = r % 100;
	if(k < 25) return psp_rnd();
	if(k < 85) return (psp_rnd() & 0x807FFFFFu) | ((107u + psp_rnd() % 41u) << 23);
	if(k < 92) return (psp_rnd() & 0x807FFFFFu) | ((psp_rnd() % 30u) << 23);
	return raros[psp_rnd() % (sizeof(raros) / sizeof(raros[0]))];
}

void broadway_prueba_ps(u32 pares){   //Arreglo
	static float a[2] __attribute__((aligned(8))), b[2] __attribute__((aligned(8))), c[2] __attribute__((aligned(8)));
	static float p[PSP_N][2] __attribute__((aligned(8))), e[PSP_N][2] __attribute__((aligned(8)));
	u32 malos[PSP_N], vistos = 0, n, i, k, fpscr_hi, fpscr;
	u64 t0 = gettime();
	double fpd;
	memset(malos, 0, sizeof(malos));
	__asm__ volatile("mffs %0" : "=f"(fpd));
	{ u64 w; memcpy(&w, &fpd, 8); fpscr = (u32)w; fpscr_hi = (u32)(w >> 32); }
	(void)fpscr_hi;
	hle_log_quiet("[PRUEBA] paired singles contra escalares: FPSCR %08X (NI %u, RN %u), HID2 %08X (PSE %u)\n", fpscr,
	              (fpscr >> 2) & 1, fpscr & 3, MFSPR(920), (MFSPR(920) >> 29) & 1);
	a[0] = psp_f(0x42480000u); c[0] = psp_f(0xBC88CC38u); b[0] = psp_f(0x1B1C72A0u);
	a[1] = a[0]; b[1] = b[0]; c[1] = c[0];
	psp_pares(a, b, c, p);
	psp_escalares(a, b, c, e);
	hle_log_quiet("[PRUEBA] caso de Mario Strikers (a 42480000, c BC88CC38, b 1B1C72A0; la consola da BF55BF17): fmadds %08X, "
	              "ps_madd %08X %08X\n", psp_bits(e[PSP_MADD][0]), psp_bits(p[PSP_MADD][0]), psp_bits(p[PSP_MADD][1]));
	for(n = 0; n < pares; n++){
		a[0] = psp_f(psp_valor()); a[1] = psp_f(psp_valor());
		c[0] = psp_f(psp_valor()); c[1] = psp_f(psp_valor());
		if(psp_rnd() % 4 == 0){
			b[0] = psp_f(psp_bits(-(a[0] * c[0])) ^ (psp_rnd() & 0xFu));
			b[1] = psp_f(psp_bits(-(a[1] * c[1])) ^ (psp_rnd() & 0xFu));
		} else { b[0] = psp_f(psp_valor()); b[1] = psp_f(psp_valor()); }
		psp_pares(a, b, c, p);
		psp_escalares(a, b, c, e);
		for(i = 0; i < PSP_N; i++)
			for(k = 0; k < 2; k++)
				if(psp_bits(p[i][k]) != psp_bits(e[i][k])){
					malos[i]++;
					if(vistos++ < 12)
						hle_log_quiet("[PRUEBA] distinto: %s carril %u: a %08X %08X b %08X %08X c %08X %08X: par %08X, escalar %08X\n",
						              psp_nombre[i], k, psp_bits(a[0]), psp_bits(a[1]), psp_bits(b[0]), psp_bits(b[1]),
						              psp_bits(c[0]), psp_bits(c[1]), psp_bits(p[i][k]), psp_bits(e[i][k]));
				}
	}
	{
		char l[900];   //Arreglo
		size_t len = 0;
		for(i = 0; i < PSP_N && len < sizeof(l); i++)
			len += (size_t)snprintf(l + len, sizeof(l) - len, "%s%s %u", i ? ", " : "", psp_nombre[i], malos[i]);
		hle_log_quiet("[PRUEBA] %u pares (%u operaciones por carril), %u ms: distintos por instruccion: %s\n", pares, pares * PSP_N,
		              (unsigned)(ticks_to_millisecs(gettime() - t0)), l);
	}
	hle_log_sync();
}
