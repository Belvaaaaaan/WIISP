/**
 * WIISP - tests/guest/extend_stack.c
 * sceKernelExtendThreadStack (la libc de DBZ Tenkaichi Tag Team arranca
 * así): la función corre con otra pila, puede esperar y lo que devuelve
 * llega a quien llamó. sceKernelCheckThreadStack (Kernel_Library) mide la
 * pila en uso: la extendida dentro. También sceKernelMemset, que devuelve
 * el destino.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

static char buf[16];
static u32 outer_sp;

static int with_new_stack(void *arg){
	u32 sp;
	int free_now = sceKernelCheckThreadStack();
	__asm__ volatile("move %0, $sp" : "=r"(sp));
	printf("  dentro: arg %08X, pila nueva %s\n", (u32)arg, (outer_sp - sp > 0x100000 || sp - outer_sp > 0x100000) ? "si" : "NO");
	printf("  CheckThreadStack dentro: %s\n", free_now > 0x3000 && free_now < 0x4000 ? "en la pila nueva" : "NO");
	printf("  DelayThread dentro: %08X\n", sceKernelDelayThread(1000));
	return 0x1234 + (int)arg;
}

int main(int args, void *argp){
	int r;
	__asm__ volatile("move %0, $sp" : "=r"(outer_sp));
	r = sceKernelCheckThreadStack();
	printf("CheckThreadStack fuera: %s\n", r > 0x4000 && r <= 0x100000 ? "en la pila del hilo" : "NO");
	r = sceKernelExtendThreadStack(0x4000, with_new_stack, (void *)5);
	printf("ExtendThreadStack: %08X\n", r);
	r = sceKernelExtendThreadStack(100, with_new_stack, (void *)5);
	printf("pila de 100 bytes: %08X\n", r);
	r = sceKernelCheckThreadStack();
	printf("CheckThreadStack al volver: %s\n", r > 0x4000 && r <= 0x100000 ? "en la pila del hilo" : "NO");
	printf("Memset devuelve el destino: %s\n", sceKernelMemset(buf, 'x', 4) == buf && buf[3] == 'x' && !buf[4] ? "si" : "NO");
	return 0;
}
