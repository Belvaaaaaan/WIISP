/**
 * WIISP - tools/banco/jit_banco_pc.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdlib.h>
#include "core/memory.h"
#include "core/opt.h"
#include "hle/hle.h"

void jit_banco(void);

int main(int argc, char **argv){
	u8 *ram = aligned_alloc(32, PSP_RAM_SIZE_32MB), *vram = aligned_alloc(32, PSP_VRAM_SIZE), *sc = aligned_alloc(32, PSP_SCRATCH_SIZE);
	if(!ram || !vram || !sc || mem_init(ram, PSP_RAM_SIZE_32MB, vram, sc)) return 1;
	opt_reset();
	hle_set_log_file(argc > 1 ? argv[1] : "jit_banco.log");
	jit_banco();
	hle_set_log_file(NULL);
	return 0;
}
