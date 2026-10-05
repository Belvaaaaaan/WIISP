/**
 * WIISP - loader.h
 * Carga de ejecutables de PSP en la memoria emulada.
 *
 * Acepta un EBOOT.PBP o un ELF/PRX suelto (EBOOT.BIN, BOOT.BIN, *.prx).
 * Carga los segmentos, aplica las relocalizaciones de PRX, lee el
 * sceModuleInfo y parchea cada stub importado con:
 *
 *     jr    $ra
 *     syscall <índice en la tabla de imports>
 *
 * Así, cuando el juego llama a una función del firmware (sceDisplay...,
 * sceCtrl..., etc.), la CPU emulada cae en un syscall cuyo código nos dice
 * exactamente qué NID quería, y el HLE lo atiende en código nativo.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_LOADER_H
#define WIISP_LOADER_H

#include "core/types.h"

enum {
	LOADER_OK            =  0,
	LOADER_ERR_FORMAT    = -1, /* no es un formato reconocido */
	LOADER_ERR_CORRUPT   = -2, /* cabeceras incoherentes o truncadas */
	LOADER_ERR_ENCRYPTED = -3, /* "~PSP": ejecutable cifrado */
	LOADER_ERR_MEMORY    = -4, /* no cabe en la RAM emulada */
	LOADER_ERR_UNSUPPORTED = -5,
	LOADER_ERR_NOMEM     = -6  /* sin memoria en el host */
};

/* Dirección donde el firmware carga el primer PRX de usuario */
#define LOADER_DEFAULT_PRX_BASE 0x08804000u

#define LOADER_MAX_IMPORTS 8192
#define LOADER_LIB_NAME_LEN 32

typedef struct {
	char lib[LOADER_LIB_NAME_LEN]; /* p. ej. "sceDisplay" */
	u32  nid;                      /* identificador de la función */
	u32  stub_addr;                /* dirección del stub parcheado */
} PspImport;

typedef struct {
	/* sceModuleInfo */
	char name[29];
	u16  attr;
	u16  version;
	u32  gp;
	u32  modinfo_addr;

	int  relocatable;  /* 1 = PRX (e_type 0xFFA0), 0 = ELF absoluto */
	u32  base;         /* dirección de carga (0 para ELF absoluto) */
	u32  entry;
	u32  load_start;   /* rango ocupado en RAM */
	u32  load_end;
	u32  num_relocs;
	u32  num_relocs_skipped; /* tipos que aún no se aplican (p. ej. GPREL16) */

	u32  num_libs;     /* bibliotecas importadas */
	u32  num_imports;  /* funciones importadas */
	u32  num_var_imports; /* variables importadas (aún no soportadas) */
	PspImport *imports;   /* el índice es el código del syscall */

	/* Metadatos del PARAM.SFO si venía en un PBP */
	char title[128];
	char disc_id[16];
} PspModule;

/* Instrucciones MIPS que se escriben en cada stub */
#define MIPS_JR_RA          0x03E00008u
#define MIPS_SYSCALL(code)  (0x0000000Cu | ((u32)(code) << 6))

/* Carga buf (PBP o ELF). La memoria emulada debe estar inicializada.
   prx_base: dirección para PRX relocalizables (0 = LOADER_DEFAULT_PRX_BASE).
   Si devuelve error, mod queda liberado. */
int  loader_load(const u8 *buf, u32 len, u32 prx_base, PspModule *mod);
int  loader_load_elf(const u8 *buf, u32 len, u32 prx_base, PspModule *mod);
void loader_free(PspModule *mod);
const char *loader_strerror(int err);

/* Imprime un resumen del módulo con printf (sin acentos: la consola del
   Wii no muestra UTF-8). max_imports < 0 = todos. */
void loader_print_info(const PspModule *mod, int max_imports);

#endif
