/**
 * WIISP - tools/banco/vfpu_arit_banco.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include "guest.h"

static u32 buf[32] __attribute__((aligned(16)));

#define LVQ(vt, off) (0xD9000000u | (((vt) & 31) << 16) | (off) | (((vt) >> 5) & 1))

static void vueltas(u32 *p, u32 n){
	__asm__ volatile(
		".set push\n.set noreorder\n"
		"move $8, %0\n"
		"move $9, %1\n"
		".word %2\n.word %3\n.word %4\n.word %5\n"
		"1:\n"
		".word 0x6404888C\n"
		".word 0x6085890D\n"
		".word 0x6D048890\n"
		".word 0x6D858991\n"
		".word 0xD0008894\n"
		".word 0xD2008C98\n"
		".word 0xD280989C\n"
		".word 0x6540889E\n"
		".word 0x6004888E\n"
		".word 0x6404890D\n"
		".word 0xDE000000\n"
		".word 0xD0018995\n"
		"addiu $9, $9, -1\n"
		"bnez $9, 1b\n"
		"nop\n"
		".set pop\n"
		:: "r"(p), "r"(n), "i"(LVQ(4, 0)), "i"(LVQ(5, 16)), "i"(LVQ(8, 32)), "i"(LVQ(9, 48))
		: "$8", "$9", "memory");
}

int main(void){
	u32 i;
	for(i = 0; i < 32; i++) buf[i] = 0x3F800000u + i * 0x00011111u;
	for(i = 0; i < VUELTAS; i++) vueltas(buf, 2000);
	printf_("banco vfpu arit: hecho\n");
	return 0;
}
