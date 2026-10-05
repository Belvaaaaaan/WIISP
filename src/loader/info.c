/**
 * WIISP - info.c
 * Resumen legible de un módulo cargado (para el CLI y la consola del Wii).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include "loader/loader.h"

void loader_print_info(const PspModule *mod, int max_imports){
	u32 i, shown;

	if(mod->title[0])   printf("Titulo:      %s\n", mod->title);
	if(mod->disc_id[0]) printf("ID:          %s\n", mod->disc_id);
	printf("Modulo:      %s (v%u.%u, attr 0x%04X)\n", mod->name,
	       mod->version >> 8, mod->version & 0xFF, mod->attr);
	printf("Tipo:        %s\n", mod->relocatable ? "PRX relocalizable" : "ELF absoluto");
	printf("Cargado en:  0x%08X - 0x%08X (%u KB)\n", mod->load_start, mod->load_end,
	       (mod->load_end - mod->load_start) / 1024);
	printf("Entrada:     0x%08X   gp: 0x%08X\n", mod->entry, mod->gp);
	if(mod->relocatable)
		printf("Relocs:      %u aplicadas, %u omitidas\n", mod->num_relocs, mod->num_relocs_skipped);
	printf("Imports:     %u funciones de %u bibliotecas", mod->num_imports, mod->num_libs);
	if(mod->num_var_imports) printf(" (+%u variables sin soporte)", mod->num_var_imports);
	printf("\n");

	shown = max_imports < 0 ? mod->num_imports : (u32)max_imports;
	if(shown > mod->num_imports) shown = mod->num_imports;
	for(i = 0; i < shown; i++){
		const PspImport *imp = &mod->imports[i];
		printf("  [%4u] %-24s NID 0x%08X  stub 0x%08X\n", i, imp->lib, imp->nid, imp->stub_addr);
	}
	if(shown < mod->num_imports)
		printf("  ... y %u mas\n", mod->num_imports - shown);
}
