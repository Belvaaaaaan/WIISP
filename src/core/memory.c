/**
 * WIISP - memory.c
 * Mapa de memoria de la PSP (ver memory.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "core/memory.h"

PspMemory psp_mem;

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
}

/* off < size && len <= size - off evita desbordes con direcciones altas */
static inline u8 *region(u8 *base, u32 size, u32 off, u32 len){
	if(off < size && len <= size - off) return base + off;
	return NULL;
}

u8 *mem_ptr(u32 addr, u32 len){
	addr &= PSP_ADDR_MASK;
	if(addr >= PSP_RAM_BASE)
		return region(psp_mem.ram, psp_mem.ram_size, addr - PSP_RAM_BASE, len);
	if(addr >= PSP_VRAM_BASE && addr < PSP_VRAM_MIRROR_END){
		u32 off = addr - PSP_VRAM_BASE, mirror = off >> 21;
		off &= PSP_VRAM_SIZE - 1;
		if(mirror & 1){
			/* El swizzle conserva los 5 bits bajos: solo hay puntero
			   contiguo dentro de un bloque de 32 bytes */
			if((off & 31) + len > 32) return NULL;
			return psp_mem.vram + mem_vram_deswizzle(off, mirror == 3);
		}
		return region(psp_mem.vram, PSP_VRAM_SIZE, off, len);
	}
	if(addr >= PSP_SCRATCH_BASE)
		return region(psp_mem.scratch, PSP_SCRATCH_SIZE, addr - PSP_SCRATCH_BASE, len);
	return NULL;
}

/* Un rango por los espejos con swizzle es válido aunque no sea contiguo */
int mem_valid(u32 addr, u32 len){
	u32 a = addr & PSP_ADDR_MASK;
	if(a >= PSP_VRAM_BASE + PSP_VRAM_SIZE && a < PSP_VRAM_MIRROR_END)
		return ((a - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1)) + len <= PSP_VRAM_SIZE &&
		       len <= PSP_VRAM_SIZE;
	return mem_ptr(addr, len) != NULL;
}

u8 mem_read8(u32 addr){
	u8 *p = mem_ptr(addr, 1);
	return p ? *p : 0;
}

u16 mem_read16(u32 addr){
	u8 *p = mem_ptr(addr, 2);
	return p ? rd_le16(p) : 0;
}

u32 mem_read32(u32 addr){
	u8 *p = mem_ptr(addr, 4);
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
		u8 *p = mem_ptr(addr + i, 1);
		if(!p){ dst[i] = 0; return -1; }
		dst[i] = (char)*p;
		if(!*p) return 0;
	}
	dst[i] = 0;
	return 0;
}
