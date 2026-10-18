/**
 * WIISP - cpu/predec.h
 * Intérprete predecodificado (2.2, cpu_predec).
 *
 * Cada trozo de código de la PSP se traduce una vez a un bloque de
 * "tarjetas": una por instrucción, con la dirección del código que la
 * ejecuta y los operandos ya preparados (registros como desplazamientos en
 * bytes, inmediato con su signo). Las tarjetas se ejecutan con un salto
 * indirecto de una a la siguiente (computed goto), sin leer la instrucción,
 * sin switch y sin contar instrucción a instrucción.
 *
 * El resultado es el mismo ciclo a ciclo que cpu_run sin el interruptor
 * (tests/test_interp.c): un bloque solo se ejecuta entero si caben todas
 * sus instrucciones en lo que queda de cpu_run; las salidas a mitad de
 * bloque (memoria lenta, VFPU, fallos, mtic, parar) reconstruyen pc, npc y
 * el reloj con la posición de la tarjeta; lo raro (instrucciones que
 * fallan, saltos en el delay slot, código fuera de la RAM) lo ejecuta el
 * intérprete de siempre, de una en una.
 *
 * Código que cambia: la RAM se divide en zonas de 1 KB y cpu_code_map dice
 * cuáles tienen bloques. Las escrituras de la CPU (también la VFPU y
 * mem_write*), los avisos a granel (mem_note_write: archivos, DMA, módulos,
 * memset del kernel) y sceKernelIcacheInvalidate* tiran los bloques de las
 * zonas tocadas. Con cpu_verificar, cada bloque comprueba al entrar que sus
 * instrucciones siguen en la RAM (si no, lo apunta en el registro).
 *
 * Memoria (0.5.17.1): con predec_compacto cada bloque guarda aparte lo que
 * se lee al ejecutar (cabecera de 24 bytes y tarjetas, alineado a 32) y lo
 * que casi nunca (listas, zonas e instrucciones); con predec_mem1 los
 * bloques van en MEM1; con predec_sin_unir el bucle se compila sin unir los
 * finales de las tarjetas. //arreglo
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_PREDEC_H
#define WIISP_PREDEC_H

#include <stdint.h>
#include "core/types.h"
#include "core/memory.h"

#ifndef PREDEC_ZONA_BITS   //Arreglo
#define PREDEC_ZONA_BITS 10   /* zonas de 1 KB */
#endif   //Arreglo

/* Una entrada por zona de 1 KB de la RAM (hasta 64 MB): distinta de 0 si
   hay bloques en ella */
extern u8 cpu_code_map[(64u << 20) >> PREDEC_ZONA_BITS];

/* Ejecuta como cpu_run (ver cpu.h) */
u32 cpu_run_predec(u32 max_cycles);

/* Se escribieron len bytes en el desplazamiento off de la RAM, en una zona
   con código (fuera de línea: el que escribe ya miró cpu_code_map) */
void cpu_predec_store(u32 off, u32 len);

/* Lo mismo para cualquier rango de la RAM (lo mira entero) */
static inline void cpu_predec_written(u32 off, u32 len){
	u32 z = off >> PREDEC_ZONA_BITS, last = (off + (len ? len - 1 : 0)) >> PREDEC_ZONA_BITS;
	for(; z <= last; z++)
		if(cpu_code_map[z]){ cpu_predec_store(off, len); return; }
}

/* Una escritura por puntero del anfitrión (si cae en la RAM de la PSP) */
static inline void cpu_predec_ptr_written(const void *p, u32 len){
	uintptr_t off = (uintptr_t)p - (uintptr_t)psp_mem.ram;
	if(off < psp_mem.ram_size && cpu_code_map[off >> PREDEC_ZONA_BITS]) cpu_predec_store((u32)off, len);
}

/* Todo fuera (otro programa, memoria reiniciada, sceKernelIcacheInvalidateAll) */
void cpu_predec_flush(void);

enum { CAIDA_DELAY, CAIDA_SIN_TARJETA, CAIDA_NO_CABE, CAIDA_FUERA };   //Arreglo
#define PREDEC_CAIDA_OPS 256   //Arreglo

typedef struct {
	u64 bloques_hechos, bloques_tirados, vaciados, busquedas, pasos;
	u64 escrituras_en_codigo, verif_viejos;
	u64 fpu, vfpu, lentas;   /* tarjetas de FPU y VFPU y accesos a la memoria lenta */   //arreglo
	u32 arena_usada, arena_total;
	u32 frio_usada, frio_total;   /* la arena de lo frío (predec_compacto) */   //arreglo
	uintptr_t dir_arena, dir_frio, dir_tabla, dir_mapa;   /* dónde están (MEM1 o MEM2) */   //arreglo
	u8 compacto;   //arreglo
	u64 bq_bloques, bq_instr, bq_tam[7], bq_enlace[2][2], bq_jr_sin_enlace;   //Arreglo
	u64 bq_caidas[4], bq_caida_op[PREDEC_CAIDA_OPS];   //Arreglo
	u64 vfpu_tarjetas, vfpu_verif, vfpu_verif_mal;   //Arreglo
} PredecStats;
void cpu_predec_stats(PredecStats *out);

//arreglo
/* Memoria del anfitrión para los bloques (el Wii la reserva en MEM1 al
   arrancar); con predec_mem1 se usa en vez de malloc */
void cpu_predec_reserva(void *p, u32 bytes);
/* Para las pruebas: arenas de otro tamaño (0 = el normal) */
void cpu_predec_tam_pruebas(u32 bloques, u32 frio);
/* Para las pruebas: ¿son todas las tarjetas de los bloques de la tabla de
   la copia del bucle que corre ahora (predec_sin_unir)? */
int cpu_predec_tarjetas_ok(void);
void cpu_predec_caida_nombre(u32 k, char *buf, u32 size);   //Arreglo
int cpu_predec_enlaces_ok(void);   //Arreglo

#endif
