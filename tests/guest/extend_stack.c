/**
 * WIISP - tests/guest/extend_stack.c
 * sceKernelExtendThreadStack (la libc de DBZ Tenkaichi Tag Team arranca
 * así): la función corre con otra pila, puede esperar y lo que devuelve
 * llega a quien llamó. sceKernelCheckThreadStack (Kernel_Library) mide la
 * pila en uso: la extendida dentro. También sceKernelMemset y
 * sceKernelMemcpy, que devuelven el destino (Memcpy, aunque se solapen).
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
	sceKernelMemset(buf, 0, sizeof(buf));
	buf[0] = 'a'; buf[1] = 'b'; buf[2] = 'c';
	r = sceKernelMemcpy(buf + 8, buf, 3) == buf + 8 && buf[8] == 'a' && buf[10] == 'c' && !buf[11];
	/* Solapado hacia delante: como memmove de 8 en 8 */
	sceKernelMemcpy(buf + 1, buf, 3);
	printf("Memcpy devuelve el destino y copia: %s (solapado: %c%c%c%c)\n", r ? "si" : "NO", buf[0], buf[1], buf[2], buf[3]);
	return 0;
}
