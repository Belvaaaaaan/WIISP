/**
 * WIISP - tests/test_osk.c
 * Pruebas del teclado en pantalla (sceUtilityOsk): el diálogo llama al
 * frontend (hle_set_osk), copia el texto al juego y devuelve los estados y
 * resultados de la PSP. Sin frontend devuelve el texto inicial.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <string.h>
#include "test.h"
#include "hle/hle.h"
#include "core/memory.h"

#define PARAMS 0x08A10000u   /* SceUtilityOskParams (0x40 bytes) */
#define FIELD  0x08A10100u   /* SceUtilityOskData (52 bytes) */
#define DESC   0x08A10200u
#define INTEXT 0x08A10300u
#define OUTTXT 0x08A10400u

static u32 call(const char *name, u32 a0){
	u32 l, f;
	for(l = 0; l < hle_utility_libs_count; l++)
		for(f = 0; f < hle_utility_libs[l].count; f++)
			if(!strcmp(hle_utility_libs[l].funcs[f].name, name)){
				cpu.r[R_A0] = a0;
				hle_utility_libs[l].funcs[f].func();
				return cpu.r[R_V0];
			}
	printf("  falta %s\n", name);
	return 0xDEADBEEF;
}

static void put_str(u32 addr, const char *s){
	for(;; s++, addr += 2){
		mem_write16(addr, (u16)(unsigned char)*s);
		if(!*s) break;
	}
}

static void get_str(u32 addr, char *out, int n){
	int i;
	for(i = 0; i + 1 < n; i++){
		u16 c = mem_read16(addr + (u32)i * 2);
		if(!c) break;
		out[i] = (char)c;
	}
	out[i] = 0;
}

/* Lo que ve el frontend de prueba y lo que contesta */
static struct {
	int calls, accept;
	unsigned limit, lines, inputtype;
	char desc[32], initial[32];
	const char *answer;
} fake;

static int fake_osk(const unsigned short *desc, const unsigned short *initial, unsigned short *out,
                    unsigned limit, unsigned lines, unsigned inputtype){
	unsigned i;
	fake.calls++;
	fake.limit = limit;
	fake.lines = lines;
	fake.inputtype = inputtype;
	for(i = 0; desc[i] && i < 31; i++) fake.desc[i] = (char)desc[i];
	fake.desc[i] = 0;
	for(i = 0; initial[i] && i < 31; i++) fake.initial[i] = (char)initial[i];
	fake.initial[i] = 0;
	if(!fake.accept) return 0;
	for(i = 0; fake.answer[i] && i < limit; i++) out[i] = (unsigned char)fake.answer[i];
	out[i] = 0;
	return 1;
}

/* Abre el teclado, avanza el tiempo y lo recorre hasta FINISHED */
static void run_dialog(void){
	CHECK_EQ(call("sceUtilityOskInitStart", PARAMS), 0);
	cpu_cycles += 60000ull * CYCLES_PER_US;   /* pasa el retardo de INITIALIZE -> RUNNING */
	CHECK_EQ(call("sceUtilityOskGetStatus", 0), 2);
	CHECK_EQ(call("sceUtilityOskUpdate", 0), 0);
	CHECK_EQ(call("sceUtilityOskGetStatus", 0), 2);   /* aún no */
	CHECK_EQ(call("sceUtilityOskUpdate", 0), 0);
	CHECK_EQ(call("sceUtilityOskGetStatus", 0), 3);
	CHECK_EQ(call("sceUtilityOskShutdownStart", 0), 0);
	cpu_cycles += 60000ull * CYCLES_PER_US;
	CHECK_EQ(call("sceUtilityOskGetStatus", 0), 0);   /* de vuelta a NONE */
}

static void setup(const char *initial, u32 limit){
	memset(mem_ptr(PARAMS, 0x400), 0, 0x400);
	mem_write32(PARAMS, 0x40);          /* size */
	mem_write32(PARAMS + 0x30, 1);      /* un campo */
	mem_write32(PARAMS + 0x34, FIELD);
	mem_write32(FIELD + 16, 0);         /* inputtype */
	mem_write32(FIELD + 20, 1);         /* lines */
	mem_write32(FIELD + 28, DESC);
	mem_write32(FIELD + 32, INTEXT);
	mem_write32(FIELD + 40, OUTTXT);
	mem_write32(FIELD + 48, limit);
	put_str(DESC, "Nombre");
	put_str(INTEXT, initial);
	mem_write16(OUTTXT, 0xFFFF);
}

void test_osk(void){
	char s[64];

	printf("Teclado en pantalla (sceUtilityOsk)\n");
	utility_init();

	/* Sin frontend: el texto inicial pasa sin cambios */
	hle_set_osk(NULL);
	setup("Goku", 9);
	run_dialog();
	get_str(OUTTXT, s, sizeof(s));
	CHECK(!strcmp(s, "Goku"));
	CHECK_EQ(mem_read32(FIELD + 44), 0);   /* UNCHANGED */
	CHECK_EQ(mem_read32(FIELD + 36), 4);

	/* Con frontend que acepta */
	hle_set_osk(fake_osk);
	memset(&fake, 0, sizeof(fake));
	fake.accept = 1;
	fake.answer = "Vegeta";
	setup("Goku", 9);
	run_dialog();
	CHECK_EQ(fake.calls, 1);
	CHECK(!strcmp(fake.desc, "Nombre"));
	CHECK(!strcmp(fake.initial, "Goku"));
	CHECK_EQ(fake.limit, 8);               /* outtextlimit incluye el 0 final */
	CHECK_EQ(fake.lines, 1);
	get_str(OUTTXT, s, sizeof(s));
	CHECK(!strcmp(s, "Vegeta"));
	CHECK_EQ(mem_read32(FIELD + 44), 2);   /* CHANGED */
	CHECK_EQ(mem_read32(FIELD + 36), 6);
	CHECK_EQ(mem_read32(PARAMS + 0x1C), 0);

	/* No escribe más de lo que cabe: el límite son 4 caracteres + el 0 */
	fake.calls = 0;
	fake.answer = "Gohan Son";
	setup("", 5);
	mem_write16(OUTTXT + 10, 0x7777);      /* justo después del búfer */
	run_dialog();
	get_str(OUTTXT, s, sizeof(s));
	CHECK(!strcmp(s, "Goha"));
	CHECK_EQ(mem_read16(OUTTXT + 10), 0x7777);

	/* El usuario cancela: se conserva lo de antes */
	fake.accept = 0;
	setup("Goku", 9);
	run_dialog();
	get_str(OUTTXT, s, sizeof(s));
	CHECK(!strcmp(s, "Goku"));
	CHECK_EQ(mem_read32(FIELD + 44), 1);   /* CANCELLED */

	/* El texto inicial más largo que el límite se recorta */
	fake.accept = 0;
	setup("Piccolo", 4);
	run_dialog();
	get_str(OUTTXT, s, sizeof(s));
	CHECK(!strcmp(s, "Pic"));
	CHECK(!strcmp(fake.initial, "Pic"));

	/* Un tamaño de parámetros que no es de la PSP se rechaza */
	setup("x", 9);
	mem_write32(PARAMS, 0x41);
	CHECK_EQ(call("sceUtilityOskInitStart", PARAMS), 0x80110004u);

	hle_set_osk(NULL);
	utility_init();
}
