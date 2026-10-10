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

void prof_sample(void){
	int b = prof_cur, f;
	if((unsigned)b >= PROF_N) return;
	prof_samples[b]++;
	if(b != PROF_GE) return;
	f = prof_ge_phase;
	if((unsigned)f < GEF_N) prof_ge_samples[f]++;
}
