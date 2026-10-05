/**
 * WIISP - tests/test_main.c
 * Pruebas unitarias del núcleo (memoria y cargador).
 *
 * Se ejecutan en PC (little-endian, con sanitizers) y en PowerPC big-endian
 * bajo qemu, para garantizar que el código funciona igual que en el Wii.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include "core/memory.h"
#include "loader/loader.h"
#include "loader/pbp.h"
#include "frontend/filelist.h"
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond) do { \
	checks++; \
	if(!(cond)){ failures++; printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

#define CHECK_EQ(a, b) do { \
	unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b); \
	checks++; \
	if(va_ != vb_){ failures++; \
		printf("  FALLO %s:%d: %s == %s (0x%llX != 0x%llX)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } \
} while(0)

/* ------------------------------------------------------------------ */
/* Constructor de ejecutables sintéticos                              */
/* ------------------------------------------------------------------ */

#define IMG_SEG_OFF   0x80   /* offset del segmento en el archivo */
#define IMG_SEG_SIZE  0x200
#define IMG_SEG_MEM   0x300  /* incluye 0x100 de BSS */
#define IMG_MAX_RELOCS 32

/* Offsets dentro del segmento */
#define OFF_CODE      0x000
#define OFF_FUNC      0x040
#define OFF_MODINFO   0x060
#define OFF_STUBENT   0x0A0
#define OFF_LIBNAME1  0x100
#define OFF_LIBNAME2  0x110
#define OFF_NIDS1     0x120
#define OFF_NIDS2     0x128
#define OFF_DATA      0x150
#define OFF_STUBS1    0x180
#define OFF_STUBS2    0x190

#define NID_A 0x0E20F177u
#define NID_B 0x289D82FEu
#define NID_C 0x1F4011E6u

typedef struct {
	u8  file[0x400];
	u32 len;
	u32 vbase;          /* 0 para PRX; dirección absoluta para ELF */
	u32 rel[IMG_MAX_RELOCS][2];
	int nrel;
} Img;

static u8 *seg(Img *img, u32 off){ return img->file + IMG_SEG_OFF + off; }

/* Escribe una palabra que contiene una dirección dentro del segmento */
static void put_addr32(Img *img, u32 off, u32 target){
	wr_le32(seg(img, off), img->vbase + target);
	if(!img->vbase){
		img->rel[img->nrel][0] = off;
		img->rel[img->nrel][1] = 2; /* R_MIPS_32, segmentos 0 y 0 */
		img->nrel++;
	}
}

static void put_reloc(Img *img, u32 off, u32 type){
	img->rel[img->nrel][0] = off;
	img->rel[img->nrel][1] = type;
	img->nrel++;
}

/* vbase = 0 crea un PRX relocalizable; otro valor, un ELF absoluto */
static void build_image(Img *img, u32 vbase){
	u8 *f = img->file, *ph;
	int nph = vbase ? 1 : 2, i;

	memset(img, 0, sizeof(*img));
	img->vbase = vbase;

	/* Código: lui a0, %hi(data); addiu a0, a0, %lo(data); jal func; nop */
	u32 data = vbase + OFF_DATA;
	wr_le32(seg(img, OFF_CODE + 0), 0x3C040000u | (((data + 0x8000) >> 16) & 0xFFFF));
	wr_le32(seg(img, OFF_CODE + 4), 0x24840000u | (data & 0xFFFF));
	wr_le32(seg(img, OFF_CODE + 8), 0x0C000000u | (((vbase + OFF_FUNC) >> 2) & 0x03FFFFFF));
	if(!vbase){
		put_reloc(img, OFF_CODE + 0, 5); /* HI16 */
		put_reloc(img, OFF_CODE + 4, 6); /* LO16 */
		put_reloc(img, OFF_CODE + 8, 4); /* 26 */
	}
	wr_le32(seg(img, OFF_FUNC), MIPS_JR_RA);

	/* sceModuleInfo */
	wr_le16(seg(img, OFF_MODINFO + 0), 0x0000);
	wr_le16(seg(img, OFF_MODINFO + 2), 0x0102);
	memcpy(seg(img, OFF_MODINFO + 4), "TestMod", 7);
	put_addr32(img, OFF_MODINFO + 32, 0x8100);              /* gp */
	put_addr32(img, OFF_MODINFO + 44, OFF_STUBENT);         /* lib_stub */
	put_addr32(img, OFF_MODINFO + 48, OFF_STUBENT + 2*20);  /* lib_stub_end */

	/* Dos bibliotecas importadas */
	static const struct { u32 name, nids, stubs, count; } libs[2] = {
		{ OFF_LIBNAME1, OFF_NIDS1, OFF_STUBS1, 2 },
		{ OFF_LIBNAME2, OFF_NIDS2, OFF_STUBS2, 1 },
	};
	for(i = 0; i < 2; i++){
		u32 e = OFF_STUBENT + i*20;
		put_addr32(img, e + 0, libs[i].name);
		wr_le16(seg(img, e + 4), 0x0011);
		wr_le16(seg(img, e + 6), 0x4001);
		seg(img, e + 8)[0] = 5;
		seg(img, e + 9)[0] = 0;
		wr_le16(seg(img, e + 10), (u16)libs[i].count);
		put_addr32(img, e + 12, libs[i].nids);
		put_addr32(img, e + 16, libs[i].stubs);
	}
	memcpy(seg(img, OFF_LIBNAME1), "sceDisplay", 11);
	memcpy(seg(img, OFF_LIBNAME2), "sceCtrl", 8);
	wr_le32(seg(img, OFF_NIDS1 + 0), NID_A);
	wr_le32(seg(img, OFF_NIDS1 + 4), NID_B);
	wr_le32(seg(img, OFF_NIDS2 + 0), NID_C);

	wr_le32(seg(img, OFF_DATA), 0xDEADBEEFu);
	put_addr32(img, OFF_DATA + 4, OFF_DATA);

	/* Relocalizaciones justo después del segmento */
	u32 rel_off = IMG_SEG_OFF + IMG_SEG_SIZE;
	for(i = 0; i < img->nrel; i++){
		wr_le32(f + rel_off + i*8, img->rel[i][0]);
		wr_le32(f + rel_off + i*8 + 4, img->rel[i][1]);
	}
	img->len = rel_off + img->nrel*8;

	/* Cabecera ELF */
	f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
	f[4] = 1; f[5] = 1; f[6] = 1;
	wr_le16(f + 16, vbase ? 2 : 0xFFA0);
	wr_le16(f + 18, 8);
	wr_le32(f + 20, 1);
	wr_le32(f + 24, vbase + OFF_CODE);
	wr_le32(f + 28, 52);
	wr_le16(f + 40, 52);
	wr_le16(f + 42, 32);
	wr_le16(f + 44, (u16)nph);
	wr_le16(f + 46, 40);

	/* PT_LOAD; p_paddr = offset del sceModuleInfo en el archivo */
	ph = f + 52;
	wr_le32(ph + 0, 1);
	wr_le32(ph + 4, IMG_SEG_OFF);
	wr_le32(ph + 8, vbase);
	wr_le32(ph + 12, IMG_SEG_OFF + OFF_MODINFO);
	wr_le32(ph + 16, IMG_SEG_SIZE);
	wr_le32(ph + 20, IMG_SEG_MEM);
	wr_le32(ph + 24, 7);
	wr_le32(ph + 28, 16);
	if(!vbase){
		ph += 32;
		wr_le32(ph + 0, 0x700000A0u);
		wr_le32(ph + 4, rel_off);
		wr_le32(ph + 16, img->nrel * 8);
	}
}

/* Envuelve un ejecutable en un EBOOT.PBP con PARAM.SFO */
static u32 build_pbp(u8 *out, const u8 *exec, u32 exec_len){
	u8 sfo[128];
	u32 i, sfo_len, off;

	/* PARAM.SFO con TITLE y DISC_ID */
	memset(sfo, 0, sizeof(sfo));
	sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
	wr_le32(sfo + 4, 0x0101);
	wr_le32(sfo + 8, 20 + 2*16);       /* tabla de claves */
	wr_le32(sfo + 12, 20 + 2*16 + 16); /* tabla de datos */
	wr_le32(sfo + 16, 2);
	/* entrada 0: DISC_ID */
	wr_le16(sfo + 20, 0);  wr_le16(sfo + 22, 0x0204);
	wr_le32(sfo + 24, 10); wr_le32(sfo + 28, 16); wr_le32(sfo + 32, 0);
	/* entrada 1: TITLE */
	wr_le16(sfo + 36, 8);  wr_le16(sfo + 38, 0x0204);
	wr_le32(sfo + 40, 7);  wr_le32(sfo + 44, 16); wr_le32(sfo + 48, 16);
	memcpy(sfo + 52, "DISC_ID\0TITLE\0\0", 16);
	memcpy(sfo + 68, "UCES00000\0", 10);
	memcpy(sfo + 84, "Prueba\0", 7);
	sfo_len = 100;

	memset(out, 0, 40);
	out[1] = 'P'; out[2] = 'B'; out[3] = 'P';
	wr_le32(out + 4, 0x00010000);
	off = 40;
	for(i = 0; i < PBP_NUM_SECTIONS; i++){
		wr_le32(out + 8 + i*4, off);
		if(i == PBP_PARAM_SFO){ memcpy(out + off, sfo, sfo_len); off += sfo_len; }
		if(i == PBP_DATA_PSP){ memcpy(out + off, exec, exec_len); off += exec_len; }
	}
	return off;
}

/* ------------------------------------------------------------------ */
/* Pruebas                                                            */
/* ------------------------------------------------------------------ */

static void test_memory(void){
	u8 *p;
	printf("memoria\n");

	mem_write32(0x08800000, 0x11223344u);
	p = mem_ptr(0x08800000, 4);
	CHECK(p != NULL);
	/* little-endian en la memoria emulada, sea cual sea el host */
	CHECK_EQ(p[0], 0x44); CHECK_EQ(p[3], 0x11);
	CHECK_EQ(mem_read16(0x08800002), 0x1122);
	CHECK_EQ(mem_read8(0x08800001), 0x33);

	/* Espejos: sin caché (0x40000000) y kernel (0x80000000) */
	CHECK_EQ(mem_read32(0x48800000), 0x11223344u);
	CHECK_EQ(mem_read32(0x88800000), 0x11223344u);
	CHECK_EQ(mem_read32(0xC8800000), 0x11223344u);

	/* Límites de la RAM */
	CHECK(mem_valid(0x09FFFFFC, 4));
	CHECK(!mem_valid(0x09FFFFFD, 4));
	CHECK(!mem_valid(0x0A000000, 1));
	CHECK(!mem_valid(0x08000000, 0xFFFFFFFFu));
	CHECK(!mem_valid(0x07FFFFFF, 1));

	/* Scratchpad y VRAM (con espejos) */
	CHECK(mem_valid(0x00010000, PSP_SCRATCH_SIZE));
	CHECK(!mem_valid(0x00014000, 1));
	CHECK(!mem_valid(0x0000FFFF, 1));
	mem_write32(0x04000010, 0xCAFEBABEu);
	CHECK_EQ(mem_read32(0x04200010), 0xCAFEBABEu);
	CHECK_EQ(mem_read32(0x44600010), 0xCAFEBABEu);
	CHECK(!mem_valid(0x04800000, 1));

	/* Direcciones inválidas leen 0 y no escriben */
	CHECK_EQ(mem_read32(0x00000000), 0);
	mem_write32(0x12345678, 1);

	char s[8];
	mem_write32(0x09FFFFFC, 0x00434241u); /* "ABC\0" */
	CHECK_EQ(mem_read_cstr(0x09FFFFFC, s, sizeof(s)), 0);
	CHECK(!strcmp(s, "ABC"));
	mem_write32(0x09FFFFFC, 0x44434241u); /* sin terminador antes del final */
	CHECK_EQ(mem_read_cstr(0x09FFFFFC, s, sizeof(s)), -1);
	mem_reset();
}

static void check_loaded_prx(const PspModule *m, u32 base){
	u32 data = base + OFF_DATA;
	CHECK(m->relocatable);
	CHECK_EQ(m->base, base);
	CHECK_EQ(m->entry, base);
	CHECK(!strcmp(m->name, "TestMod"));
	CHECK_EQ(m->version, 0x0102);
	CHECK_EQ(m->gp, base + 0x8100);
	CHECK_EQ(m->modinfo_addr, base + OFF_MODINFO);
	CHECK_EQ(m->load_start, base);
	CHECK_EQ(m->load_end, base + IMG_SEG_MEM);
	CHECK_EQ(m->num_relocs_skipped, 0);

	/* lui/addiu reconstruyen la dirección exacta (con acarreo de %lo) */
	u32 hi = mem_read32(base + 0) & 0xFFFF, lo = mem_read32(base + 4) & 0xFFFF;
	CHECK_EQ((hi << 16) + (u32)(s32)(s16)lo, data);
	CHECK_EQ(mem_read32(base + 0) >> 16, 0x3C04);
	/* jal apunta a la función relocalizada */
	CHECK_EQ(mem_read32(base + 8), 0x0C000000u | (((base + OFF_FUNC) >> 2) & 0x03FFFFFF));
	/* puntero de datos relocalizado */
	CHECK_EQ(mem_read32(base + OFF_DATA + 4), data);
	CHECK_EQ(mem_read32(data), 0xDEADBEEFu);
	/* BSS a cero */
	CHECK_EQ(mem_read32(base + IMG_SEG_SIZE), 0);
	CHECK_EQ(mem_read32(base + IMG_SEG_MEM - 4), 0);

	/* imports y stubs */
	CHECK_EQ(m->num_libs, 2);
	CHECK_EQ(m->num_imports, 3);
	if(m->num_imports == 3){
		CHECK(!strcmp(m->imports[0].lib, "sceDisplay"));
		CHECK(!strcmp(m->imports[2].lib, "sceCtrl"));
		CHECK_EQ(m->imports[0].nid, NID_A);
		CHECK_EQ(m->imports[1].nid, NID_B);
		CHECK_EQ(m->imports[2].nid, NID_C);
		CHECK_EQ(m->imports[1].stub_addr, base + OFF_STUBS1 + 8);
		CHECK_EQ(m->imports[2].stub_addr, base + OFF_STUBS2);
	}
	CHECK_EQ(mem_read32(base + OFF_STUBS1), MIPS_JR_RA);
	CHECK_EQ(mem_read32(base + OFF_STUBS1 + 4), MIPS_SYSCALL(0));
	CHECK_EQ(mem_read32(base + OFF_STUBS1 + 12), MIPS_SYSCALL(1));
	CHECK_EQ(mem_read32(base + OFF_STUBS2 + 4), 0x0000008Cu); /* syscall 2 */
}

static void test_prx(void){
	static Img img;
	PspModule m;
	printf("PRX relocalizable\n");

	build_image(&img, 0);
	/* Basura en la RAM para comprobar que la BSS se limpia */
	memset(psp_mem.ram, 0xAA, psp_mem.ram_size);
	CHECK_EQ(loader_load(img.file, img.len, 0, &m), LOADER_OK);
	check_loaded_prx(&m, LOADER_DEFAULT_PRX_BASE);
	loader_free(&m);

	/* Base donde %lo(data) >= 0x8000: el %hi debe llevar acarreo */
	mem_reset();
	CHECK_EQ(loader_load(img.file, img.len, 0x08809000u, &m), LOADER_OK);
	check_loaded_prx(&m, 0x08809000u);
	CHECK_EQ(mem_read32(0x08809000u) & 0xFFFF, 0x0881);
	loader_free(&m);
	mem_reset();
}

static void test_exec(void){
	static Img img;
	PspModule m;
	const u32 base = 0x08900000u;
	printf("ELF absoluto\n");

	build_image(&img, base);
	CHECK_EQ(loader_load(img.file, img.len, 0, &m), LOADER_OK);
	CHECK(!m.relocatable);
	CHECK_EQ(m.base, 0);
	CHECK_EQ(m.entry, base);
	CHECK_EQ(m.gp, base + 0x8100);
	CHECK_EQ(m.num_relocs, 0);
	CHECK_EQ(m.num_imports, 3);
	CHECK_EQ(mem_read32(base + OFF_STUBS2 + 4), MIPS_SYSCALL(2));
	loader_free(&m);

	/* Un ELF absoluto fuera de la RAM se rechaza */
	build_image(&img, 0x04000000u);
	CHECK_EQ(loader_load(img.file, img.len, 0, &m), LOADER_ERR_MEMORY);
	mem_reset();
}

static void test_pbp(void){
	static Img img;
	static u8 pbp[0x800];
	PspModule m;
	u32 len;
	printf("EBOOT.PBP + PARAM.SFO\n");

	build_image(&img, 0);
	len = build_pbp(pbp, img.file, img.len);
	CHECK_EQ(loader_load(pbp, len, 0, &m), LOADER_OK);
	CHECK(!strcmp(m.title, "Prueba"));
	CHECK(!strcmp(m.disc_id, "UCES00000"));
	check_loaded_prx(&m, LOADER_DEFAULT_PRX_BASE);
	loader_free(&m);
	mem_reset();

	/* Ejecutable cifrado */
	memcpy(img.file, "~PSP", 4);
	len = build_pbp(pbp, img.file, img.len);
	CHECK_EQ(loader_load(pbp, len, 0, &m), LOADER_ERR_ENCRYPTED);

	/* Basura */
	CHECK_EQ(loader_load((const u8 *)"hola", 4, 0, &m), LOADER_ERR_FORMAT);
	CHECK_EQ(loader_load(pbp, 0, 0, &m), LOADER_ERR_FORMAT);
	mem_reset();
}

/* Un archivo truncado o corrupto debe dar error, nunca colgarse */
static void test_robustness(void){
	static Img img;
	static u8 pbp[0x800], bad[0x800];
	PspModule m;
	u32 len, cut, n, seed = 12345;
	int ok = 0;
	printf("archivos truncados y corruptos\n");

	build_image(&img, 0);
	len = build_pbp(pbp, img.file, img.len);
	for(cut = 0; cut < len; cut++){
		if(loader_load(pbp, cut, 0, &m) == LOADER_OK){ ok++; loader_free(&m); }
	}
	/* Truncar solo la cola de relocalizaciones puede seguir cargando,
	   pero el PBP truncado en su cabecera no */
	CHECK(loader_load(pbp, 39, 0, &m) != LOADER_OK);

	for(n = 0; n < 20000; n++){
		u32 flips, k;
		memcpy(bad, pbp, len);
		seed = seed * 1103515245u + 12345u;
		flips = 1 + ((seed >> 16) % 4);
		for(k = 0; k < flips; k++){
			seed = seed * 1103515245u + 12345u;
			u32 pos = (seed >> 8) % len;
			seed = seed * 1103515245u + 12345u;
			bad[pos] = (u8)(seed >> 16);
		}
		if(loader_load(bad, len, 0, &m) == LOADER_OK){ ok++; loader_free(&m); }
	}
	/* Si llegamos aquí sin que ASan/UBSan aborte, no hubo accesos fuera
	   de rango. */
	CHECK(1);
	printf("  (%d variantes cargaron sin error)\n", ok);
	mem_reset();
}

static void touch(const char *dir, const char *name){
	char p[512];
	FILE *f;
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	f = fopen(p, "w");
	if(f) fclose(f);
}

static void test_filelist(void){
	char dir[256], sub[300], out[512];
	FileList fl = { "", NULL, 0 };
	const char *tmp = getenv("TMPDIR");
	printf("menu: listado de carpetas\n");

	CHECK(filelist_is_supported("EBOOT.PBP"));
	CHECK(filelist_is_supported("juego.prx"));
	CHECK(filelist_is_supported("a.Elf"));
	CHECK(!filelist_is_supported("imports.txt"));
	CHECK(!filelist_is_supported("pbp"));

	CHECK(filelist_is_root("sd:/"));
	CHECK(filelist_is_root("usb:/"));
	CHECK(!filelist_is_root("sd:/wiisp"));

	filelist_parent("sd:/wiisp/psp", out, sizeof(out)); CHECK(!strcmp(out, "sd:/wiisp"));
	filelist_parent("sd:/wiisp", out, sizeof(out));     CHECK(!strcmp(out, "sd:/"));
	filelist_parent("sd:/wiisp/", out, sizeof(out));    CHECK(!strcmp(out, "sd:/"));
	filelist_parent("usb:/", out, sizeof(out));         CHECK(!strcmp(out, "usb:/"));
	filelist_join("sd:/", "wiisp", out, sizeof(out));   CHECK(!strcmp(out, "sd:/wiisp"));
	filelist_join("sd:/wiisp", "a.pbp", out, sizeof(out)); CHECK(!strcmp(out, "sd:/wiisp/a.pbp"));

	/* Carpeta real: carpetas primero, luego ejecutables, sin otros archivos */
	snprintf(dir, sizeof(dir), "%s/wiisp_filelist_%d", tmp ? tmp : "/tmp", (int)getpid());
	snprintf(sub, sizeof(sub), "%s/Juegos", dir);
	mkdir(dir, 0777);
	mkdir(sub, 0777);
	touch(dir, "b.PBP");
	touch(dir, "a.prx");
	touch(dir, "notas.txt");
	touch(dir, ".oculto.pbp");
	CHECK_EQ(filelist_load(&fl, dir), 0);
	CHECK_EQ(fl.count, 4);
	if(fl.count == 4){
		CHECK(!strcmp(fl.entries[0].name, "..") && fl.entries[0].is_dir);
		CHECK(!strcmp(fl.entries[1].name, "Juegos") && fl.entries[1].is_dir);
		CHECK(!strcmp(fl.entries[2].name, "a.prx") && !fl.entries[2].is_dir);
		CHECK(!strcmp(fl.entries[3].name, "b.PBP"));
	}
	filelist_free(&fl);
	CHECK(filelist_load(&fl, "/no/existe/wiisp") != 0);
	filelist_free(&fl);

	snprintf(out, sizeof(out), "%s/b.PBP", dir); remove(out);
	snprintf(out, sizeof(out), "%s/a.prx", dir); remove(out);
	snprintf(out, sizeof(out), "%s/notas.txt", dir); remove(out);
	snprintf(out, sizeof(out), "%s/.oculto.pbp", dir); remove(out);
	rmdir(sub);
	rmdir(dir);
}

/* Escribe el PBP de prueba a un archivo, para probar el frontend del Wii
   con un EBOOT que se sabe válido (make -f Makefile.pc eboot) */
static int write_test_pbp(const char *path){
	static Img img;
	static u8 pbp[0x800];
	FILE *f;
	u32 len;
	int ok;

	build_image(&img, 0);
	len = build_pbp(pbp, img.file, img.len);
	f = fopen(path, "wb");
	if(!f){ perror(path); return 1; }
	ok = fwrite(pbp, 1, len, f) == len;
	fclose(f);
	printf("%s: %u bytes\n", path, len);
	return ok ? 0 : 1;
}

int main(int argc, char **argv){
	if(argc == 3 && !strcmp(argv[1], "--write-pbp")) return write_test_pbp(argv[2]);

	u8 *ram = malloc(PSP_RAM_SIZE_32MB), *vram = malloc(PSP_VRAM_SIZE);
	u8 *scratch = malloc(PSP_SCRATCH_SIZE);

	printf("WIISP pruebas (%s-endian)\n", WIISP_HOST_BIG_ENDIAN ? "big" : "little");
	if(mem_init(ram, PSP_RAM_SIZE_32MB, vram, scratch)){
		printf("mem_init fallo\n");
		return 1;
	}

	test_memory();
	test_prx();
	test_exec();
	test_pbp();
	test_robustness();
	test_filelist();

	printf("\n%d comprobaciones, %d fallos\n", checks, failures);
	free(ram); free(vram); free(scratch);
	return failures ? 1 : 0;
}
