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
   válido o cruza el final de una región. mem_ptr_r es para accesos que
   solo leen. */
u8  *mem_ptr_slow(u32 addr, u32 len);
const u8 *mem_ptr_r_slow(u32 addr, u32 len);
int  mem_valid(u32 addr, u32 len);

/* La RAM principal en línea; lo demás (VRAM con sus ganchos, scratchpad)
   por la función */
static inline u8 *mem_ptr(u32 addr, u32 len){
	u32 off = (addr & PSP_ADDR_MASK) - PSP_RAM_BASE;
	if(off < psp_mem.ram_size && len <= psp_mem.ram_size - off) return psp_mem.ram + off;
	return mem_ptr_slow(addr, len);
}

static inline const u8 *mem_ptr_r(u32 addr, u32 len){
	u32 off = (addr & PSP_ADDR_MASK) - PSP_RAM_BASE;
	if(off < psp_mem.ram_size && len <= psp_mem.ram_size - off) return psp_mem.ram + off;
	return mem_ptr_r_slow(addr, len);
}

/* Coherencia con un renderizador por hardware que guarda en su GPU la
   versión más nueva de parte de la VRAM (framebuffers dibujados por GX).
   Antes de acceder a bytes de la VRAM física dentro de
   [mem_vram_watch_lo[w], mem_vram_watch_hi[w]) se llama al gancho con el
   rango físico tocado; w = 0 para lecturas y 1 para accesos que pueden
   escribir. Sin renderizador por hardware los rangos están vacíos. */
typedef void (*MemVramHook)(u32 off, u32 len, int write);
extern MemVramHook mem_vram_hook;
extern u32 mem_vram_watch_lo[2], mem_vram_watch_hi[2];

/* Avisos de escritura para las cachés de texturas de un renderizador por
   hardware: así sabe qué texturas pudieron cambiar sin volver a leerlas.
   Avisan las escrituras "a granel" (lecturas de archivos, DMA,
   transferencias del GE, memset del kernel, módulos y partidas cargados),
   la CPU escribiendo en la VRAM y la caché de datos (sceKernelDcache* y la
   instrucción cache): en la PSP, lo que la CPU escribe en la RAM no lo ve
   el GE hasta que el juego vacía la caché de datos. Las escrituras normales
   de la CPU en la RAM no avisan (serían demasiadas).

   Cada página de 4 KB de la RAM y de la VRAM guarda el sello de su último
   aviso. Quien comprueba una textura guarda mem_write_stamp_take(); la
   textura cambió si alguna de sus páginas tiene un sello posterior. */
void mem_note_write(u32 addr, u32 len);
/* Sin rango (sceKernelDcacheWritebackAll): solo cuenta */
void mem_note_write_all(void);
extern u32 mem_write_all_hints;
u32  mem_write_stamp_take(void);
int  mem_written_since(u32 addr, u32 len, u32 stamp);

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
