/* Para correr código compilado para el Broadway en qemu-ppc (Linux): emula
   con SIGILL lo que no tiene un PowerPC normal o es privilegiado:
   mfmsr/mtmsr, mfspr/mtspr (los GQR se guardan), psq_l y psq_st (W = 0 y 1)
   y las instrucciones de paired singles (opcode 4) que usa WIISP. ps0 es el
   registro de qemu; ps1 va en una sombra (las cargas y operaciones normales
   no la tocan, como lfd en el Broadway). Cada carril se calcula con la
   operación escalar de precisión simple (fmadds, fmuls...), que es lo que
   la autoprueba prueba_ps comprobó en el Wii. psq_st de float convierte un
   denormal en cero (como Dolphin): se cuenta, igual que los merges que
   mueven un denormal (PSQ_INFORME=1 los muestra). */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

static unsigned int gqr[8];
static double ps1[32];
unsigned long sigill_psq_count, sigill_ps_count, sigill_ps_ftz, sigill_ps_merge_den;

static int denormal(float f){
	unsigned int b;
	memcpy(&b, &f, 4);
	return (b & 0x7F800000u) == 0 && (b & 0x007FFFFFu) != 0;
}

static double dequant(const unsigned char *p, unsigned int g){
	int type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
	double v;
	if(scale & 0x20) scale -= 64;
	switch(type){
	case 0: { unsigned int b = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; float f; memcpy(&f, &b, 4); return f; }
	case 4: v = p[0]; break;
	case 5: v = (p[0] << 8) | p[1]; break;
	case 6: v = (signed char)p[0]; break;
	case 7: v = (short)((p[0] << 8) | p[1]); break;
	default: fprintf(stderr, "GQR con tipo %d\n", type); abort();
	}
	return (double)(float)(v * __builtin_ldexp(1.0, -scale));
}

static int quant_size(unsigned int type){ return type == 0 ? 4 : type >= 6 ? (type == 6 ? 1 : 2) : (type == 4 ? 1 : 2); }

static void store_float(unsigned char *p, double v){
	float f = (float)v;
	unsigned int b;
	if(denormal(f)){ f = signbit(f) ? -0.0f : 0.0f; sigill_ps_ftz++; }
	memcpy(&b, &f, 4);
	p[0] = (unsigned char)(b >> 24); p[1] = (unsigned char)(b >> 16); p[2] = (unsigned char)(b >> 8); p[3] = (unsigned char)b;
}

/* Un carril de una instrucción A-form (xo de 5 bits) */
static float lane_a(int xo, float a, float b, float c, int *ok){
	switch(xo){
	case 18: return a / b;                      /* ps_div */
	case 20: return a - b;                      /* ps_sub */
	case 21: return a + b;                      /* ps_add */
	case 23: return a >= 0.0f ? c : b;          /* ps_sel */
	case 25: return a * c;                      /* ps_mul */
	case 28: return __builtin_fmaf(a, c, -b);   /* ps_msub */
	case 29: return __builtin_fmaf(a, c, b);    /* ps_madd */
	case 30: return -__builtin_fmaf(a, c, -b);  /* ps_nmsub */
	case 31: return -__builtin_fmaf(a, c, b);   /* ps_nmadd */
	}
	*ok = 0;
	return 0.0f;
}

static int ps_op(unsigned int ins, double *fpr){
	int d = (ins >> 21) & 31, ra = (ins >> 16) & 31, rb = (ins >> 11) & 31, rc = (ins >> 6) & 31;
	int xo5 = (ins >> 1) & 31, xo10 = (ins >> 1) & 0x3FF, ok = 1;
	float a0 = (float)fpr[ra], a1 = (float)ps1[ra], b0 = (float)fpr[rb], b1 = (float)ps1[rb];
	float c0 = (float)fpr[rc], c1 = (float)ps1[rc], r0, r1;
	switch(xo5){
	case 10: r0 = a0 + b1; r1 = c1; break;                                   /* ps_sum0 */
	case 11: r0 = c0; r1 = a0 + b1; break;                                   /* ps_sum1 */
	case 12: r0 = a0 * c0; r1 = a1 * c0; break;                              /* ps_muls0 */
	case 13: r0 = a0 * c1; r1 = a1 * c1; break;                              /* ps_muls1 */
	case 14: r0 = __builtin_fmaf(a0, c0, b0); r1 = __builtin_fmaf(a1, c0, b1); break;  /* ps_madds0 */
	case 15: r0 = __builtin_fmaf(a0, c1, b0); r1 = __builtin_fmaf(a1, c1, b1); break;  /* ps_madds1 */
	case 18: case 20: case 21: case 23: case 25: case 28: case 29: case 30: case 31:
		r0 = lane_a(xo5, a0, b0, c0, &ok);
		r1 = lane_a(xo5, a1, b1, c1, &ok);
		break;
	default:
		switch(xo10){
		case 40: r0 = -b0; r1 = -b1; break;                                  /* ps_neg */
		case 72: r0 = b0; r1 = b1; break;                                    /* ps_mr */
		case 136: r0 = -__builtin_fabsf(b0); r1 = -__builtin_fabsf(b1); break;  /* ps_nabs */
		case 264: r0 = __builtin_fabsf(b0); r1 = __builtin_fabsf(b1); break; /* ps_abs */
		case 528: r0 = a0; r1 = b0; break;                                   /* ps_merge00 */
		case 560: r0 = a0; r1 = b1; break;                                   /* ps_merge01 */
		case 592: r0 = a1; r1 = b0; break;                                   /* ps_merge10 */
		case 624: r0 = a1; r1 = b1; break;                                   /* ps_merge11 */
		default: return 0;
		}
		if(xo10 >= 528 && (denormal(r0) || denormal(r1))) sigill_ps_merge_den++;
	}
	if(!ok) return 0;
	fpr[d] = r0;
	ps1[d] = r1;
	sigill_ps_count++;
	return 1;
}

static void on_sigill(int sig, siginfo_t *si, void *ctx){
	ucontext_t *uc = ctx;
	unsigned long *gr = (unsigned long *)uc->uc_mcontext.uc_regs->gregs;
	double *fpr = uc->uc_mcontext.uc_regs->fpregs.fpregs;
	unsigned int ins = *(unsigned int *)gr[32];   /* PT_NIP */
	unsigned int spr = ((ins >> 16) & 0x1F) | ((ins >> 6) & 0x3E0), op = ins >> 26;
	(void)sig; (void)si;
	if((ins & 0xFC0007FE) == 0x7C0000A6) gr[(ins >> 21) & 31] = 0x9032;           /* mfmsr */
	else if((ins & 0xFC0007FE) == 0x7C000124) {}                                 /* mtmsr */
	else if((ins & 0xFC0007FE) == 0x7C0002A6) gr[(ins >> 21) & 31] = spr >= 912 && spr <= 919 ? gqr[spr - 912] : 0;  /* mfspr */
	else if((ins & 0xFC0007FE) == 0x7C0003A6){ if(spr >= 912 && spr <= 919) gqr[spr - 912] = (unsigned)gr[(ins >> 21) & 31]; }  /* mtspr */
	else if(op == 56 || op == 57 || op == 60 || op == 61){                       /* psq_l(u), psq_st(u) */
		int fr = (ins >> 21) & 31, ra = (ins >> 16) & 31, w = (ins >> 15) & 1, i = (ins >> 12) & 7;
		int d = ins & 0xFFF;
		unsigned long ea;
		if(d & 0x800) d -= 0x1000;
		ea = (ra ? gr[ra] : 0) + (unsigned long)(long)d;
		if(op & 1) gr[ra] = ea;
		if(op <= 57){
			fpr[fr] = dequant((const unsigned char *)ea, gqr[i]);
			ps1[fr] = w ? 1.0 : dequant((const unsigned char *)ea + quant_size((gqr[i] >> 16) & 7), gqr[i]);
			sigill_psq_count++;
		} else {
			if(gqr[i] & 7){ fprintf(stderr, "psq_st con tipo %u en %08lx\n", gqr[i] & 7, gr[32]); abort(); }
			store_float((unsigned char *)ea, fpr[fr]);
			if(!w) store_float((unsigned char *)ea + 4, ps1[fr]);
			sigill_ps_count++;
		}
	}
	else if(op == 4 && ps_op(ins, fpr)) {}
	else { fprintf(stderr, "SIGILL %08x en %08lx\n", ins, gr[32]); abort(); }
	gr[32] += 4;
}

static void report(void){
	if(getenv("PSQ_INFORME"))
		fprintf(stderr, "psq_l emulados: %lu; paired singles: %lu (denormales aplanados por psq_st %lu, movidos por merges %lu)\n",
		        sigill_psq_count, sigill_ps_count, sigill_ps_ftz, sigill_ps_merge_den);
}

__attribute__((constructor)) static void install(void){
	struct sigaction sa;
	atexit(report);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_sigill;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGILL, &sa, 0);
}
