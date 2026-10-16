/* Para correr código compilado para el Broadway en qemu-ppc (Linux): emula
   con SIGILL lo que no tiene un PowerPC normal o es privilegiado:
   mfmsr/mtmsr, mfspr/mtspr (los GQR se guardan) y psq_l con W = 1 (un valor
   cuantizado según su GQR en ps0; ps1 no existe aquí). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

static unsigned int gqr[8];
unsigned long sigill_psq_count;

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

static void on_sigill(int sig, siginfo_t *si, void *ctx){
	ucontext_t *uc = ctx;
	unsigned long *gr = (unsigned long *)uc->uc_mcontext.uc_regs->gregs;
	unsigned int ins = *(unsigned int *)gr[32];   /* PT_NIP */
	unsigned int spr = ((ins >> 16) & 0x1F) | ((ins >> 6) & 0x3E0);
	(void)sig; (void)si;
	if((ins & 0xFC0007FE) == 0x7C0000A6) gr[(ins >> 21) & 31] = 0x9032;           /* mfmsr */
	else if((ins & 0xFC0007FE) == 0x7C000124) {}                                 /* mtmsr */
	else if((ins & 0xFC0007FE) == 0x7C0002A6) gr[(ins >> 21) & 31] = spr >= 912 && spr <= 919 ? gqr[spr - 912] : 0;  /* mfspr */
	else if((ins & 0xFC0007FE) == 0x7C0003A6){ if(spr >= 912 && spr <= 919) gqr[spr - 912] = (unsigned)gr[(ins >> 21) & 31]; }  /* mtspr */
	else if((ins >> 26) == 56 && (ins & 0x8000)){                                /* psq_l, W = 1 */
		int frd = (ins >> 21) & 31, ra = (ins >> 16) & 31, i = (ins >> 12) & 7;
		int d = ins & 0xFFF;
		unsigned long ea;
		if(d & 0x800) d -= 0x1000;
		ea = (ra ? gr[ra] : 0) + (unsigned long)(long)d;
		uc->uc_mcontext.uc_regs->fpregs.fpregs[frd] = dequant((const unsigned char *)ea, gqr[i]);
		sigill_psq_count++;
	}
	else { fprintf(stderr, "SIGILL %08x en %08lx\n", ins, gr[32]); abort(); }
	gr[32] += 4;
}

static void report(void){ if(getenv("PSQ_INFORME")) fprintf(stderr, "psq_l emulados: %lu\n", sigill_psq_count); }

__attribute__((constructor)) static void install(void){
	struct sigaction sa;
	atexit(report);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_sigill;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGILL, &sa, 0);
}
