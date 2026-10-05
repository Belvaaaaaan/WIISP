/**
 * WIISP - memory.h
 * Mapa de memoria de la PSP.
 *
 *   0x00010000 - 0x00013FFF  Scratchpad (16 KB)
 *   0x04000000 - 0x041FFFFF  VRAM (2 MB, con espejos hasta 0x047FFFFF)
 *   0x08000000 - 0x09FFFFFF  RAM principal (32 MB; 64 MB en PSP-2000/3000)
 *                            0x08000000-0x087FFFFF kernel, 0x08800000+ usuario
 *
 * Los bits altos solo cambian el modo de acceso, no la memoria física:
 *   0x40000000 = sin caché, 0x80000000 = segmento de kernel.
 * Por eso toda dirección se enmascara con PSP_ADDR_MASK antes de traducirla.
 *
 * Los búferes los aporta la plataforma (en el Wii, la RAM vive en MEM2).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_MEMORY_H
#define WIISP_MEMORY_H

#include "core/types.h"

#define PSP_ADDR_MASK        0x3FFFFFFFu

#define PSP_SCRATCH_BASE     0x00010000u
#define PSP_SCRATCH_SIZE     0x00004000u
#define PSP_VRAM_BASE        0x04000000u
#define PSP_VRAM_SIZE        0x00200000u
#define PSP_VRAM_MIRROR_END  0x04800000u
#define PSP_RAM_BASE         0x08000000u
#define PSP_RAM_SIZE_32MB    0x02000000u
#define PSP_RAM_SIZE_64MB    0x04000000u
#define PSP_USER_BASE        0x08800000u

typedef struct {
	u8 *ram;
	u32 ram_size;
	u8 *vram;
	u8 *scratch;
} PspMemory;

extern PspMemory psp_mem;

/* Registra los búferes de la plataforma y los pone a cero.
   ram_size debe ser PSP_RAM_SIZE_32MB o PSP_RAM_SIZE_64MB. */
int  mem_init(u8 *ram, u32 ram_size, u8 *vram, u8 *scratch);
void mem_reset(void);

/* Devuelve un puntero host a [addr, addr+len) o NULL si el rango no es
   válido o cruza el final de una región. */
u8  *mem_ptr(u32 addr, u32 len);
int  mem_valid(u32 addr, u32 len);

/* Accesos sueltos. Una dirección inválida lee 0 e ignora la escritura
   (el intérprete/dynarec decidirá luego si eso es una excepción). */
u8   mem_read8 (u32 addr);
u16  mem_read16(u32 addr);
u32  mem_read32(u32 addr);
void mem_write8 (u32 addr, u8  v);
void mem_write16(u32 addr, u16 v);
void mem_write32(u32 addr, u32 v);

/* Copia una cadena terminada en 0 de la PSP a dst (siempre termina en 0).
   Devuelve 0 si la cadena entera era accesible, -1 si no. */
int  mem_read_cstr(u32 addr, char *dst, u32 dst_size);

#endif
