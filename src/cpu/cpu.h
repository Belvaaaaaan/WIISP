/**
 * WIISP - cpu.h
 * Estado de la CPU Allegrex (MIPS32 de la PSP) e intérprete.
 *
 * El intérprete es la referencia de exactitud: el futuro dynarec se valida
 * comparándose contra él.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_CPU_H
#define WIISP_CPU_H

#include "core/types.h"

/* Registros MIPS por nombre */
enum {
	R_ZERO = 0, R_AT, R_V0, R_V1, R_A0, R_A1, R_A2, R_A3,
	R_T0, R_T1, R_T2, R_T3, R_T4, R_T5, R_T6, R_T7,
	R_S0, R_S1, R_S2, R_S3, R_S4, R_S5, R_S6, R_S7,
	R_T8, R_T9, R_K0, R_K1, R_GP, R_SP, R_FP, R_RA
};

/* FPU: bit de condición en FCR31, bits escribibles (RM, flags, enables,
   cause, C y FS) y valor de FCR0 (identificación), según pspautotests */
#define FCR31_COND      (1u << 23)
#define FCR31_WRITABLE  0x0183FFFFu
#define FCR31_DEFAULT   0x00000E00u
#define FCR0_VALUE      0x00003351u

typedef struct {
	u32 r[32];
	u32 hi, lo;
	u32 pc;    /* instrucción que se ejecuta ahora */
	u32 npc;   /* la siguiente (distinta de pc+4 tras un salto: delay slot) */
	union { u32 u; float f; } fpr[32];
	u32 fcr31;
	int llbit;
	/* bc1x justo tras c.cond ve la condición anterior */
	u32 fcc_hazard_pc;
	int fcc_old;
} CpuState;

/* Estado de la CPU del hilo que corre ahora. Lo guardan y restauran los
   cambios de contexto del HLE. */
extern CpuState cpu;

/* Lo pone a 1 cualquier código que necesite que el intérprete pare antes de
   lo previsto (cambio de hilo, fin del programa...). */
extern volatile int cpu_stop_requested;

/* Ciclos ejecutados en total (base del tiempo emulado) */
extern u64 cpu_cycles;

/* Ejecuta hasta max_cycles instrucciones o hasta que se pida parar.
   Devuelve las instrucciones ejecutadas. */
u32 cpu_run(u32 max_cycles);

/* Lo implementa el HLE: se llama al ejecutar `syscall code`, con cpu.pc ya
   apuntando a la instrucción siguiente. */
void hle_syscall(u32 code);

/* Llamado ante una instrucción inválida o un fallo grave */
void cpu_fault(const char *what, u32 addr, u32 instr);

#endif
