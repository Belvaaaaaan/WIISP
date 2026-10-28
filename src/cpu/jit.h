/**
 * WIISP - cpu/jit.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#ifndef WIISP_JIT_H
#define WIISP_JIT_H

#include "core/types.h"

//Arreglo
#if (defined(__powerpc__) || defined(__PPC__) || defined(GEKKO)) && !defined(__powerpc64__) && !defined(__ppc64__)
#define JIT_PPC 1
#else
#define JIT_PPC 0
#endif

//Arreglo
#define JIT_HECHO 0x7FFFu
#define JIT_UMBRAL 2

//Arreglo
typedef u32 (*JitFn)(u8 *r8, u8 *ram, const u8 *mapa);

//Arreglo
typedef struct {
	u64 compilados, sin_nada, instr_compiladas, palabras, vaciados, llenos;
	u64 verif_bloques, verif_parciales, verif_mal;
} JitStats;

//Arreglo
JitFn jit_compilar(u32 pc, const u32 *raw, u32 n, int branch, int verificar, u32 *cubre);
u32 jit_prefijo(const u32 *raw, u32 n, int branch);
u32 jit_tam_estimado(u32 pc, const u32 *raw, u32 n, int branch, u32 *cubre);
void jit_uso_regs(u32 w, u64 *lee, u64 *esc);
int jit_lleno(void);
void jit_vaciar(void);
void jit_reserva(void *p, u32 bytes);
void jit_tam_pruebas(u32 bytes);
u32 jit_verificar_bloque(JitFn f, u32 pc, u32 n);
void jit_stats(JitStats *out);
const void *jit_arena_dir(void);
u32 jit_arena_usada(void);
u32 jit_arena_total(void);
void jit_publicar(void *p, u32 bytes);
extern int jit_prueba_error;
extern void *(*jit_pedir_arena)(u32 bytes);
void *jit_arena_lista(u32 *bytes);

#endif
