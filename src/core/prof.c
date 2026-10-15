/**
 * WIISP - core/prof.c
 * Desglose del tiempo real (ver prof.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "core/prof.h"

u64 (*prof_clock)(void);
u64 prof_hz;
u64 prof_acc[PROF_N];
u64 prof_last;
volatile int prof_cur;
volatile int prof_ge_phase;
volatile u32 prof_samples[PROF_N], prof_ge_samples[GEF_N];

void prof_set_clock(u64 (*clock)(void), u64 hz){
	int i;
	prof_clock = clock;
	prof_hz = hz;
	for(i = 0; i < PROF_N; i++) prof_acc[i] = 0;
	prof_cur = PROF_OTRO;
	prof_last = clock ? clock() : 0;
}

void prof_flush(void){
	prof_switch(prof_cur);
}

int prof_pmc_on;
void (*prof_pmc_read)(u32 *out);
u64 prof_pmc_acc[PROF_PMC_SLOTS][PMC_N];
static u32 pmc_last[PMC_N];

void prof_pmc_account(void){
	u32 now[PMC_N];
	int b = prof_cur, slot = b, i;
	if(b == PROF_GE && (unsigned)prof_ge_phase < GEF_N) slot = PROF_N + prof_ge_phase;
	if((unsigned)slot >= PROF_PMC_SLOTS) return;
	prof_pmc_read(now);
	for(i = 0; i < PMC_N; i++){
		prof_pmc_acc[slot][i] += now[i] - pmc_last[i];   /* u32: vale aunque den la vuelta */
		pmc_last[i] = now[i];
	}
}

void prof_pmc_enable(int on){
	on = on && prof_pmc_read;
	if(on && !prof_pmc_on) prof_pmc_read(pmc_last);
	prof_pmc_on = on;
}

void prof_sample(void){
	int b = prof_cur, f;
	if((unsigned)b >= PROF_N) return;
	prof_samples[b]++;
	if(b != PROF_GE) return;
	f = prof_ge_phase;
	if((unsigned)f < GEF_N) prof_ge_samples[f]++;
}
