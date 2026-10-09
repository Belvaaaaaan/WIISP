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
int prof_cur;

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
