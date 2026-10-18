/**
 * WIISP - tools/banco/vfpu_banco.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include "guest.h"

#define LVQ(vt, off, rs) (0xD8000000u | ((rs) << 21) | (((vt) & 31) << 16) | ((off) & 0xFFFC) | (((vt) >> 5) & 1))
#define SVQ(vt, off, rs) (0xF8000000u | ((rs) << 21) | (((vt) & 31) << 16) | ((off) & 0xFFFC) | (((vt) >> 5) & 1))
#define LVS(vt, off, rs) (0xC8000000u | ((rs) << 21) | (((vt) & 31) << 16) | ((off) & 0xFFFC) | (((vt) >> 5) & 3))
#define SVS(vt, off, rs) (0xE8000000u | ((rs) << 21) | (((vt) & 31) << 16) | ((off) & 0xFFFC) | (((vt) >> 5) & 3))
#define MTV(rt, vd) (0x48E00000u | ((rt) << 16) | (vd))
#define MFV(rt, vs) (0x48600000u | ((rt) << 16) | (vs))

static u32 buf[64] __attribute__((aligned(16)));

static void vueltas(u32 *p, u32 n){
	__asm__ volatile(
		".set push\n.set noreorder\n"
		"move $8, %0\n"
		"move $9, %1\n"
		"1:\n"
		".word %2\n.word %3\n.word %4\n.word %5\n"
		".word %6\n.word %7\n.word %8\n"
		".word %9\n.word %10\n.word %11\n.word %12\n"
		"addiu $9, $9, -1\n"
		"bnez $9, 1b\n"
		"addu $11, $11, $10\n"
		".set pop\n"
		:: "r"(p), "r"(n),
		   "i"(LVQ(0, 0, 8)), "i"(LVQ(1, 16, 8)), "i"(LVQ(36, 32, 8)), "i"(LVQ(8, 48, 8)),
		   "i"(SVQ(0, 64, 8)), "i"(SVQ(1, 80, 8)), "i"(SVQ(36, 96, 8)),
		   "i"(LVS(12, 112, 8)), "i"(SVS(12, 116, 8)), "i"(MTV(9, 13)), "i"(MFV(10, 12))
		: "$8", "$9", "$10", "$11", "memory");
}

int main(void){
	u32 i, s = 0;
	for(i = 0; i < 64; i++) buf[i] = i * 2654435761u;
	for(i = 0; i < VUELTAS; i++) vueltas(buf, 2000);
	for(i = 0; i < 64; i++) s = s * 31 + buf[i];
	printf_("banco vfpu: %08x\n", s);
	return 0;
}
