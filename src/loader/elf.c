/**
 * WIISP - elf.c
 * Carga de ELF/PRX de PSP (ver loader.h).
 *
 * Todo lo que viene del archivo se considera no confiable: cada offset y
 * tamaño se comprueba antes de usarse.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include "core/memory.h"
#include "loader/loader.h"

#define EHDR_SIZE 52
#define PHDR_SIZE 32
#define SHDR_SIZE 40

#define ET_EXEC       2
#define ET_PSP_PRX    0xFFA0
#define EM_MIPS       8

#define PT_LOAD       1
#define PT_PSP_REL    0x700000A0u  /* relocalizaciones formato 1 */
#define PT_PSP_REL2   0x700000A1u  /* formato 2 (comprimido) */
#define SHT_PSP_REL   0x700000A0u

#define R_MIPS_NONE    0
#define R_MIPS_32      2
#define R_MIPS_26      4
#define R_MIPS_HI16    5
#define R_MIPS_LO16    6

#define MODINFO_SIZE  52
#define MAX_SEGMENTS  32

typedef struct {
	const u8 *buf;
	u32 len;
	u16 type;
	u32 entry, phoff, shoff;
	u16 phnum, shnum, shstrndx;
	u32 seg_addr[MAX_SEGMENTS]; /* base + p_vaddr de cada program header */
} Elf;

static inline const u8 *phdr(const Elf *e, int i){ return e->buf + e->phoff + i*PHDR_SIZE; }
static inline const u8 *shdr(const Elf *e, int i){ return e->buf + e->shoff + i*SHDR_SIZE; }

/* ¿[off, off+size) cabe en el archivo? */
static int in_file(const Elf *e, u32 off, u32 size){
	return off <= e->len && size <= e->len - off;
}

static int parse_header(const u8 *buf, u32 len, Elf *e){
	memset(e, 0, sizeof(*e));
	e->buf = buf;
	e->len = len;

	if(len >= 4 && buf[0] == '~' && buf[1] == 'P' && buf[2] == 'S' && buf[3] == 'P')
		return LOADER_ERR_ENCRYPTED;
	if(len < EHDR_SIZE || buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F')
		return LOADER_ERR_FORMAT;
	/* ELFCLASS32, little-endian, MIPS */
	if(buf[4] != 1 || buf[5] != 1 || rd_le16(buf + 18) != EM_MIPS)
		return LOADER_ERR_FORMAT;

	e->type = rd_le16(buf + 16);
	if(e->type != ET_EXEC && e->type != ET_PSP_PRX) return LOADER_ERR_UNSUPPORTED;

	e->entry = rd_le32(buf + 24);
	e->phoff = rd_le32(buf + 28);
	e->shoff = rd_le32(buf + 32);
	e->phnum = rd_le16(buf + 44);
	e->shnum = rd_le16(buf + 48);
	e->shstrndx = rd_le16(buf + 50);

	if(e->phnum == 0 || e->phnum > MAX_SEGMENTS) return LOADER_ERR_CORRUPT;
	if(rd_le16(buf + 42) != PHDR_SIZE || !in_file(e, e->phoff, e->phnum * PHDR_SIZE))
		return LOADER_ERR_CORRUPT;
	/* Las secciones son opcionales; si están mal, se ignoran */
	if(e->shnum && (rd_le16(buf + 46) != SHDR_SIZE || !in_file(e, e->shoff, e->shnum * SHDR_SIZE)))
		e->shnum = 0;
	return LOADER_OK;
}

static int load_segments(Elf *e, u32 base, PspModule *mod){
	int i, loaded = 0;
	mod->load_start = 0xFFFFFFFFu;
	mod->load_end = 0;

	for(i = 0; i < e->phnum; i++){
		const u8 *ph = phdr(e, i);
		u32 offset = rd_le32(ph + 4), vaddr = rd_le32(ph + 8);
		u32 filesz = rd_le32(ph + 16), memsz = rd_le32(ph + 20);
		u32 dest = base + vaddr;
		u8 *p;

		e->seg_addr[i] = dest;
		if(rd_le32(ph) != PT_LOAD || memsz == 0) continue;

		if(filesz > memsz || !in_file(e, offset, filesz)) return LOADER_ERR_CORRUPT;
		if(dest < base) return LOADER_ERR_MEMORY; /* desborde de 32 bits */
		p = mem_ptr(dest, memsz);
		if(!p || (dest & PSP_ADDR_MASK) < PSP_RAM_BASE) return LOADER_ERR_MEMORY;

		memcpy(p, e->buf + offset, filesz);
		memset(p + filesz, 0, memsz - filesz);

		if(dest < mod->load_start) mod->load_start = dest;
		if(dest + memsz > mod->load_end) mod->load_end = dest + memsz;
		loaded++;
	}
	return loaded ? LOADER_OK : LOADER_ERR_CORRUPT;
}

/* Lee el campo info de la relocalización r y valida sus índices */
static int reloc_target(const Elf *e, const u8 *rel, u32 r, u32 *addr, u32 *relocate_to, u32 *type){
	u32 offset = rd_le32(rel + r*8), info = rd_le32(rel + r*8 + 4);
	u32 ofs_base = (info >> 8) & 0xFF, addr_base = (info >> 16) & 0xFF;
	if(ofs_base >= e->phnum || addr_base >= e->phnum) return LOADER_ERR_CORRUPT;
	*type = info & 0xFF;
	*addr = e->seg_addr[ofs_base] + offset;
	*relocate_to = e->seg_addr[addr_base];
	return mem_valid(*addr, 4) ? LOADER_OK : LOADER_ERR_CORRUPT;
}

/* Formato 1 de PSP: entradas Elf32_Rel donde info = tipo | segmento del
   offset << 8 | segmento destino << 16. */
static int apply_relocs(const Elf *e, const u8 *rel, u32 count, PspModule *mod){
	u32 r;
	for(r = 0; r < count; r++){
		u32 addr, relocate_to, type, word;
		int err = reloc_target(e, rel, r, &addr, &relocate_to, &type);
		if(err) return err;
		word = mem_read32(addr);

		switch(type){
		case R_MIPS_NONE:
			continue;
		case R_MIPS_32:
			word += relocate_to;
			break;
		case R_MIPS_26:
			word = (word & 0xFC000000u) | ((word + (relocate_to >> 2)) & 0x03FFFFFFu);
			break;
		case R_MIPS_HI16: {
			/* La mitad alta depende del acarreo de la baja: se busca el
			   siguiente LO16 (aún sin aplicar) para calcular la suma real. */
			s32 lo = 0;
			u32 t, full;
			for(t = r + 1; t < count; t++){
				u32 t_addr, t_reloc, t_type;
				err = reloc_target(e, rel, t, &t_addr, &t_reloc, &t_type);
				if(err) return err;
				if(t_type == R_MIPS_HI16) continue;
				if(t_type == R_MIPS_LO16) lo = (s16)(mem_read32(t_addr) & 0xFFFF);
				break;
			}
			full = (word << 16) + (u32)lo + relocate_to;
			word = (word & 0xFFFF0000u) | (((full + 0x8000u) >> 16) & 0xFFFF);
			break;
		}
		case R_MIPS_LO16:
			word = (word & 0xFFFF0000u) | ((word + relocate_to) & 0xFFFF);
			break;
		default:
			mod->num_relocs_skipped++;
			continue;
		}
		mem_write32(addr, word);
		mod->num_relocs++;
	}
	return LOADER_OK;
}

static int relocate(Elf *e, PspModule *mod){
	int i, found = 0, err;

	/* Preferimos las relocalizaciones en program headers (lo que usa el
	   firmware); si no hay, las de las secciones. */
	for(i = 0; i < e->phnum; i++){
		const u8 *ph = phdr(e, i);
		u32 type = rd_le32(ph), offset = rd_le32(ph + 4), size = rd_le32(ph + 16);
		if(type == PT_PSP_REL2) return LOADER_ERR_UNSUPPORTED;
		if(type != PT_PSP_REL) continue;
		if(!in_file(e, offset, size)) return LOADER_ERR_CORRUPT;
		err = apply_relocs(e, e->buf + offset, size / 8, mod);
		if(err) return err;
		found = 1;
	}
	if(found) return LOADER_OK;

	for(i = 0; i < e->shnum; i++){
		const u8 *sh = shdr(e, i);
		u32 offset = rd_le32(sh + 16), size = rd_le32(sh + 20);
		if(rd_le32(sh + 4) != SHT_PSP_REL) continue;
		if(!in_file(e, offset, size)) return LOADER_ERR_CORRUPT;
		err = apply_relocs(e, e->buf + offset, size / 8, mod);
		if(err) return err;
	}
	return LOADER_OK;
}

/* Devuelve la dirección de la sección name o 0 */
static u32 find_section_addr(const Elf *e, u32 base, const char *name){
	u32 name_len = (u32)strlen(name), str_off;
	int i;
	if(!e->shnum || e->shstrndx >= e->shnum) return 0;
	str_off = rd_le32(shdr(e, e->shstrndx) + 16);
	for(i = 0; i < e->shnum; i++){
		u32 off = str_off + rd_le32(shdr(e, i));
		if(off < str_off || !in_file(e, off, name_len + 1)) continue;
		if(!memcmp(e->buf + off, name, name_len + 1))
			return base + rd_le32(shdr(e, i) + 12);
	}
	return 0;
}

static int add_import(PspModule *mod, const char *lib, u32 nid, u32 stub){
	PspImport *imp;
	if(mod->num_imports >= LOADER_MAX_IMPORTS) return LOADER_ERR_UNSUPPORTED;
	/* Crece en potencias de dos */
	if((mod->num_imports & (mod->num_imports - 1)) == 0){
		u32 cap = mod->num_imports ? mod->num_imports * 2 : 64;
		PspImport *n = realloc(mod->imports, cap * sizeof(PspImport));
		if(!n) return LOADER_ERR_NOMEM;
		mod->imports = n;
	}
	imp = &mod->imports[mod->num_imports];
	strncpy(imp->lib, lib, LOADER_LIB_NAME_LEN - 1);
	imp->lib[LOADER_LIB_NAME_LEN - 1] = 0;
	imp->nid = nid;
	imp->stub_addr = stub;

	mem_write32(stub, MIPS_JR_RA);
	mem_write32(stub + 4, MIPS_SYSCALL(mod->num_imports));
	mod->num_imports++;
	return LOADER_OK;
}

/* Tabla de stubs: entradas de (tamaño * 4) bytes:
     u32 nombre, u16 versión, u16 atributos, u8 tamaño en palabras,
     u8 nº de variables, u16 nº de funciones, u32 tabla de NIDs,
     u32 tabla de stubs (8 bytes por función) */
static int patch_imports(u32 start, u32 end, PspModule *mod){
	u32 addr = start;
	if(end < start || !mem_valid(start, end - start)) return LOADER_ERR_CORRUPT;

	while(end - addr >= 20){
		char lib[LOADER_LIB_NAME_LEN];
		u32 name = mem_read32(addr);
		u32 size = mem_read8(addr + 8);
		u32 var_count = mem_read8(addr + 9);
		u32 func_count = mem_read16(addr + 10);
		u32 nids = mem_read32(addr + 12);
		u32 stubs = mem_read32(addr + 16);
		u32 i;
		int err;

		if(size < 5) return LOADER_ERR_CORRUPT;
		if(!name) strcpy(lib, "(sin nombre)");
		else mem_read_cstr(name, lib, sizeof(lib));

		if(func_count && (!mem_valid(nids, func_count * 4) || !mem_valid(stubs, func_count * 8)))
			return LOADER_ERR_CORRUPT;
		for(i = 0; i < func_count; i++){
			err = add_import(mod, lib, mem_read32(nids + i*4), stubs + i*8);
			if(err) return err;
		}
		mod->num_var_imports += var_count;
		mod->num_libs++;
		addr += size * 4;
	}
	return LOADER_OK;
}

static int read_modinfo(Elf *e, u32 base, PspModule *mod){
	u32 addr = find_section_addr(e, base, ".rodata.sceModuleInfo");
	if(!addr){
		/* Sin secciones: p_paddr del primer segmento es el offset en el
		   archivo del sceModuleInfo */
		const u8 *ph = phdr(e, 0);
		addr = e->seg_addr[0] + (rd_le32(ph + 12) & 0x7FFFFFFFu) - rd_le32(ph + 4);
	}
	if(!mem_valid(addr, MODINFO_SIZE)) return LOADER_ERR_CORRUPT;

	mod->modinfo_addr = addr;
	mod->attr = mem_read16(addr);
	mod->version = mem_read16(addr + 2);
	memcpy(mod->name, mem_ptr(addr + 4, 28), 28);
	mod->name[28] = 0;
	mod->gp = mem_read32(addr + 32);
	return patch_imports(mem_read32(addr + 44), mem_read32(addr + 48), mod);
}

int loader_load_elf(const u8 *buf, u32 len, u32 prx_base, PspModule *mod){
	Elf e;
	int err;

	memset(mod, 0, sizeof(*mod));
	err = parse_header(buf, len, &e);
	if(err) return err;

	mod->relocatable = e.type == ET_PSP_PRX;
	mod->base = mod->relocatable ? (prx_base ? prx_base : LOADER_DEFAULT_PRX_BASE) : 0;
	mod->entry = mod->base + e.entry;

	err = load_segments(&e, mod->base, mod);
	if(!err && mod->relocatable) err = relocate(&e, mod);
	if(!err) err = read_modinfo(&e, mod->base, mod);
	if(err) loader_free(mod);
	return err;
}
