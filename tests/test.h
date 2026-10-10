/**
 * WIISP - tests/test.h
 * Macros comunes de las pruebas unitarias.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_TEST_H
#define WIISP_TEST_H

#include <stdio.h>

extern int failures, checks;

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

/* tests/test_formats.c: criptografía, descompresión e imágenes de disco */
void test_formats(void);
/* tests/test_io.c: HLE de archivos sobre el UMD */
void test_io(void);
/* tests/test_vfpu.c: caminos rápidos de la VFPU contra el general */
void test_vfpu(void);
/* tests/test_texcache.c: avisos de escritura y caché de texturas */
void test_texcache(void);

#endif
