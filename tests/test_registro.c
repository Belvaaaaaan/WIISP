/**
 * WIISP - tests/test_registro.c
 * El escritor de wiisp.log en otro hilo (core/registro.c): lo que llega al
 * archivo es exactamente lo que se puso y en el mismo orden, aunque la cola
 * sea diminuta (da la vuelta y hay que esperar a que haya sitio), los
 * trozos sean más grandes que la cola, se pidan fsync por medio o se cambie
 * de archivo.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <stdlib.h>
#include <string.h>
#include "core/registro.h"
#include "test.h"

static u32 semilla = 12345;
static u32 azar(u32 n){ semilla = semilla * 1103515245u + 12345u; return (semilla >> 8) % n; }

/* Lo escrito en f, desde el principio */
static int igual(FILE *f, const char *esperado, size_t n){
	char *leido = malloc(n + 1);
	size_t r;
	int ok;
	fflush(f);
	rewind(f);
	r = fread(leido, 1, n + 1, f);
	ok = r == n && !memcmp(leido, esperado, n);
	free(leido);
	fseek(f, 0, SEEK_END);
	return ok;
}

static void una_capacidad(u32 cap, int *fallos){
	enum { TOTAL = 300000 };
	static char esperado[2][TOTAL + 70000];
	char trozo[70000];
	size_t n[2] = { 0, 0 };
	FILE *f[2];
	int cual = 0, i;
	RegStats a, b;
	f[0] = tmpfile();
	f[1] = tmpfile();
	if(!f[0] || !f[1]){ (*fallos)++; return; }
	reg_test_capacidad(cap);
	reg_stats(&a);
	reg_archivo(f[0]);
	while(n[0] + n[1] < TOTAL){
		u32 k = azar(10) == 0 ? 1 + azar(sizeof(trozo) - 1) : 1 + azar(200), j;
		int que = (int)azar(40);
		for(j = 0; j < k; j++) trozo[j] = (char)('a' + azar(26));
		reg_poner(f[cual], trozo, k, que == 0);
		memcpy(esperado[cual] + n[cual], trozo, k);
		n[cual] += k;
		if(que == 1) reg_pedir_fsync();
		if(que == 2) reg_esperar(azar(2));
		if(que == 3){ reg_archivo(NULL); cual ^= 1; reg_archivo(f[cual]); }
		if(que == 4) cual ^= 1;   /* reg_poner cambia solo de archivo */
	}
	reg_archivo(NULL);
	reg_stats(&b);
	for(i = 0; i < 2; i++){
		if(!igual(f[i], esperado[i], n[i])){
			printf("  FALLO: cola de %u bytes, archivo %d distinto (%zu bytes)\n", cap, i, n[i]);
			(*fallos)++;
		}
		fclose(f[i]);
	}
	if(b.bytes - a.bytes != n[0] + n[1]){
		printf("  FALLO: cola de %u bytes, el escritor cuenta %llu bytes y se pusieron %zu\n", cap,
		       (unsigned long long)(b.bytes - a.bytes), n[0] + n[1]);
		(*fallos)++;
	}
	if(b.max_cola > (cap > a.max_cola ? cap : a.max_cola)){
		printf("  FALLO: la cola llegó a %u bytes con capacidad %u\n", b.max_cola, cap);
		(*fallos)++;
	}
}

void test_registro(void){
	static const u32 caps[] = { 1, 7, 100, 4096, 20000, REG_COLA };
	unsigned i;
	int fallos = 0;
	printf("Registro en otro hilo: lo escrito es lo que se puso, en orden\n");
	if(!reg_disponible()){
		printf("  (sin hilos en este anfitrión: se escribe en el acto)\n");
		return;
	}
	for(i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) una_capacidad(caps[i], &fallos);
	reg_test_capacidad(REG_COLA);
	CHECK_EQ(fallos, 0);
	CHECK(!reg_pendiente());
}
