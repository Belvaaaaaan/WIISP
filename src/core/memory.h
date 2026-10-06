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
 * La VRAM se ve cuatro veces. Los espejos 1 (0x04200000) y 3 (0x04600000)
 * reordenan los bytes ("swizzle"): el GE guarda la profundidad en ese orden
 * y el espejo 3 la muestra lineal. El orden depende de la traducción de
 * EDRAM (sceGeEdramSetAddrTranslation); fórmulas medidas en una PSP real
 * (pspautotests gpu/ge/edramswizzle).
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
	u32 vram_translation;  /* 0 o 0x200-0x1000 (potencia de 2); 0x400 al arrancar */
} PspMemory;

extern PspMemory psp_mem;

/* Offset lineal de la VRAM que se ve en el offset `off` (< 2 MB) del
   espejo 1 (mirror3 = 0) o del espejo 3 (mirror3 = 1) */
static inline u32 mem_vram_deswizzle(u32 off, int mirror3){
	u32 t = psp_mem.vram_translation, k, x, m, h;
	if(t == 0){
		u32 b = off & 0x600;
		return (!mirror3 || b == 0 || b == 0x600) ? off ^ 0x600 : off;
	}
	k = t >= 0x1000 ? 3 : t >= 0x800 ? 2 : t >= 0x400 ? 1 : 0;
	x = 0x1000u << k;
	if(!mirror3) return off ^ (x | 0x40);
	m = (t - 1) & ~0x1Fu;
	h = (t - 1) & ~0x7Fu;
	return ((off ^ x) & ~m) | ((off & (h >> 1)) << 1) | ((~off & 0x20) << 1) | ((off >> (3 + k)) & 0x20);
}

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
