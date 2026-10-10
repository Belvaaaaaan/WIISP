/**
 * WIISP - memory.c
 * Mapa de memoria de la PSP (ver memory.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "core/memory.h"

PspMemory psp_mem;
MemVramHook mem_vram_hook;
u32 mem_vram_watch_lo[2], mem_vram_watch_hi[2];

/* --- Avisos de escritura (ver memory.h) ------------------------------- */

#define PAGE_SHIFT 12
#define RAM_PAGES  (PSP_RAM_SIZE_64MB >> PAGE_SHIFT)
#define VRAM_PAGES (PSP_VRAM_SIZE >> PAGE_SHIFT)

static u32 page_stamp[RAM_PAGES + VRAM_PAGES];
static u32 write_stamp;
static int stamp_taken;   /* alguien guardó el sello actual: el próximo aviso lo sube */
u32 mem_write_all_hints;

/* Páginas [*first, *last] que toca un rango; 0 si no es RAM ni VRAM */
static int page_span(u32 addr, u32 len, u32 *first, u32 *last){
	u32 a = addr & PSP_ADDR_MASK, off, end;
	if(!len) return 0;
	if(a >= PSP_RAM_BASE){
		off = a - PSP_RAM_BASE;
		if(off >= psp_mem.ram_size) return 0;
		end = len > psp_mem.ram_size - off ? psp_mem.ram_size : off + len;
		*first = off >> PAGE_SHIFT;
		*last = (end - 1) >> PAGE_SHIFT;
		return 1;
	}
	if(a >= PSP_VRAM_BASE && a < PSP_VRAM_MIRROR_END){
		off = (a - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1);
		if(len >= PSP_VRAM_SIZE){ off = 0; end = PSP_VRAM_SIZE; }
		else {
			end = off + len;
			/* Los espejos con swizzle mueven los bytes dentro de su bloque
			   de 64 KB */
			if(((a - PSP_VRAM_BASE) >> 21) & 1){
				off &= ~0xFFFFu;
				end = (end + 0xFFFFu) & ~0xFFFFu;
			}
			if(end > PSP_VRAM_SIZE){ off = 0; end = PSP_VRAM_SIZE; }   /* da la vuelta */
		}
		*first = RAM_PAGES + (off >> PAGE_SHIFT);
		*last = RAM_PAGES + ((end - 1) >> PAGE_SHIFT);
		return 1;
	}
	return 0;
}

void mem_note_write(u32 addr, u32 len){
	u32 first, last, p;
	if(!page_span(addr, len, &first, &last)) return;
	if(stamp_taken){ write_stamp++; stamp_taken = 0; }
	for(p = first; p <= last; p++) page_stamp[p] = write_stamp;
}

void mem_note_write_all(void){ mem_write_all_hints++; }

u32 mem_write_stamp_take(void){
	stamp_taken = 1;
	return write_stamp;
}

/* Con diferencia con signo: los sellos pueden dar la vuelta */
int mem_written_since(u32 addr, u32 len, u32 stamp){
	u32 first, last, p;
	/* Sin avisos desde entonces (cada aviso tras guardar el sello lo sube) */
	if(stamp == write_stamp) return 0;
	if(!page_span(addr, len, &first, &last)) return 0;
	for(p = first; p <= last; p++)
		if((s32)(page_stamp[p] - stamp) > 0) return 1;
	return 0;
}

int mem_init(u8 *ram, u32 ram_size, u8 *vram, u8 *scratch){
	if(!ram || !vram || !scratch) return -1;
	if(ram_size != PSP_RAM_SIZE_32MB && ram_size != PSP_RAM_SIZE_64MB) return -1;
	psp_mem.ram = ram;
	psp_mem.ram_size = ram_size;
	psp_mem.vram = vram;
	psp_mem.scratch = scratch;
	mem_reset();
	return 0;
}

void mem_reset(void){
	memset(psp_mem.ram, 0, psp_mem.ram_size);
	memset(psp_mem.vram, 0, PSP_VRAM_SIZE);
	memset(psp_mem.scratch, 0, PSP_SCRATCH_SIZE);
	psp_mem.vram_translation = 0x400;
	/* Todo cambió */
	{
		u32 p;
		write_stamp++;
		stamp_taken = 0;
		for(p = 0; p < RAM_PAGES + VRAM_PAGES; p++) page_stamp[p] = write_stamp;
	}
}

/* off < size && len <= size - off evita desbordes con direcciones altas */
static inline u8 *region(u8 *base, u32 size, u32 off, u32 len){
	if(off < size && len <= size - off) return base + off;
	return NULL;
}

static inline void vram_watch(u32 off, u32 len, int write){
	if(off < mem_vram_watch_hi[write] && off + len > mem_vram_watch_lo[write])
		mem_vram_hook(off, len, write);
}

static u8 *vram_ptr(u32 addr, u32 len, int write){
	u32 off = addr - PSP_VRAM_BASE, mirror = off >> 21;
	off &= PSP_VRAM_SIZE - 1;
	if(mirror & 1){
		/* El swizzle conserva los 5 bits bajos: solo hay puntero
		   contiguo dentro de un bloque de 32 bytes */
		if((off & 31) + len > 32) return NULL;
		off = mem_vram_deswizzle(off, mirror == 3);
	} else if(!(off < PSP_VRAM_SIZE && len <= PSP_VRAM_SIZE - off)) return NULL;
	vram_watch(off, len, write);
	if(write) mem_note_write(addr, len);
	return psp_mem.vram + off;
}

u8 *mem_ptr_slow(u32 addr, u32 len){
	addr &= PSP_ADDR_MASK;
	if(addr >= PSP_RAM_BASE)
		return region(psp_mem.ram, psp_mem.ram_size, addr - PSP_RAM_BASE, len);
	if(addr >= PSP_VRAM_BASE && addr < PSP_VRAM_MIRROR_END) return vram_ptr(addr, len, 1);
	if(addr >= PSP_SCRATCH_BASE)
		return region(psp_mem.scratch, PSP_SCRATCH_SIZE, addr - PSP_SCRATCH_BASE, len);
	return NULL;
}

const u8 *mem_ptr_r_slow(u32 addr, u32 len){
	addr &= PSP_ADDR_MASK;
	if(addr >= PSP_RAM_BASE)
		return region(psp_mem.ram, psp_mem.ram_size, addr - PSP_RAM_BASE, len);
	if(addr >= PSP_VRAM_BASE && addr < PSP_VRAM_MIRROR_END) return vram_ptr(addr, len, 0);
	if(addr >= PSP_SCRATCH_BASE)
		return region(psp_mem.scratch, PSP_SCRATCH_SIZE, addr - PSP_SCRATCH_BASE, len);
	return NULL;
}

/* Un rango por los espejos con swizzle es válido aunque no sea contiguo.
   Solo comprueba: no toca la memoria (ni los ganchos de la VRAM ni los
   avisos de escritura). */
int mem_valid(u32 addr, u32 len){
	u32 a = addr & PSP_ADDR_MASK;
	if(a >= PSP_RAM_BASE)
		return a - PSP_RAM_BASE < psp_mem.ram_size && len <= psp_mem.ram_size - (a - PSP_RAM_BASE);
	if(a >= PSP_VRAM_BASE && a < PSP_VRAM_MIRROR_END)
		return ((a - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1)) + len <= PSP_VRAM_SIZE &&
		       len <= PSP_VRAM_SIZE;
	if(a >= PSP_SCRATCH_BASE)
		return a - PSP_SCRATCH_BASE < PSP_SCRATCH_SIZE && len <= PSP_SCRATCH_SIZE - (a - PSP_SCRATCH_BASE);
	return 0;
}

u8 mem_read8(u32 addr){
	const u8 *p = mem_ptr_r(addr, 1);
	return p ? *p : 0;
}

u16 mem_read16(u32 addr){
	const u8 *p = mem_ptr_r(addr, 2);
	return p ? rd_le16(p) : 0;
}

u32 mem_read32(u32 addr){
	const u8 *p = mem_ptr_r(addr, 4);
	return p ? rd_le32(p) : 0;
}

void mem_write8(u32 addr, u8 v){
	u8 *p = mem_ptr(addr, 1);
	if(p) *p = v;
}

void mem_write16(u32 addr, u16 v){
	u8 *p = mem_ptr(addr, 2);
	if(p) wr_le16(p, v);
}

void mem_write32(u32 addr, u32 v){
	u8 *p = mem_ptr(addr, 4);
	if(p) wr_le32(p, v);
}

int mem_read_cstr(u32 addr, char *dst, u32 dst_size){
	u32 i;
	if(!dst_size) return -1;
	for(i = 0; i + 1 < dst_size; i++){
		const u8 *p = mem_ptr_r(addr + i, 1);
		if(!p){ dst[i] = 0; return -1; }
		dst[i] = (char)*p;
		if(!*p) return 0;
	}
	dst[i] = 0;
	return 0;
}
