/**
 * WIISP - tests/test_guion.c
 * Pruebas del guion de botones (frontend/guion.h) y de los interruptores
 * de optimización (core/opt.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif
#include "test.h"
#include "frontend/guion.h"
#include "frontend/app.h"
#include "core/opt.h"

static void test_parse(void){
	Guion g;
	char err[128];
	const char *txt =
		"# comentario\n"
		"\n"
		"600 START   # pulsa START\r\n"
		"606 -\n"
		"900 cruz Derecha\n"
		"1500 stick 255 0\n"
		"3000 captura\n"
		"5400 fin\n";
	CHECK_EQ(guion_parse(&g, txt, err, sizeof(err)), 0);
	CHECK_EQ(g.n, 6);
	if(g.n == 6){
		CHECK_EQ(g.l[0].cuadro, 600);
		CHECK_EQ(g.l[0].botones, APP_BTN_START);
		CHECK_EQ(g.l[0].flags, GUION_MANDO);
		CHECK_EQ(g.l[1].botones, 0);
		CHECK_EQ(g.l[1].flags, GUION_MANDO);
		CHECK_EQ(g.l[2].botones, APP_BTN_CROSS | APP_BTN_RIGHT);
		CHECK_EQ(g.l[2].lx, 128);
		CHECK_EQ(g.l[3].botones, 0);
		CHECK_EQ(g.l[3].lx, 255);
		CHECK_EQ(g.l[3].ly, 0);
		CHECK_EQ(g.l[4].flags, GUION_CAPTURA);
		CHECK_EQ(g.l[5].flags, GUION_FIN);
	}
	CHECK_EQ(guion_fin(&g), 5400);
	guion_free(&g);
}

static void test_errores(void){
	Guion g;
	char err[128];
	CHECK(guion_parse(&g, "600 START\n500 CRUZ\n", err, sizeof(err)) != 0);
	CHECK(strstr(err, "linea 2") != NULL);
	CHECK_EQ(g.n, 0);
	CHECK(guion_parse(&g, "100 SALTAR\n", err, sizeof(err)) != 0);
	CHECK(strstr(err, "SALTAR") != NULL);
	CHECK(guion_parse(&g, "100 stick 300 0\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "100 stick 5\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "START\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "100\n", err, sizeof(err)) != 0);
	/* Un guion vacío es válido (no hace nada) */
	CHECK_EQ(guion_parse(&g, "# nada\n", err, sizeof(err)), 0);
	CHECK_EQ(guion_fin(&g), 0);
	guion_free(&g);
}

/* Lo que escribe la grabación se vuelve a leer igual */
static void test_ida_y_vuelta(void){
	static const struct { u32 b; u8 lx, ly; } estados[] = {
		{ 0, 128, 128 }, { APP_BTN_START, 128, 128 }, { APP_BTN_CROSS | APP_BTN_LTRIGGER | APP_BTN_UP, 128, 128 },
		{ APP_BTN_SQUARE, 0, 255 }, { APP_BTN_SELECT | APP_BTN_TRIANGLE | APP_BTN_CIRCLE, 200, 128 },
	};
	char texto[1024], linea[160];
	size_t len = 0;
	u32 i;
	Guion g;
	char err[128];
	for(i = 0; i < sizeof(estados) / sizeof(estados[0]); i++){
		guion_format(linea, sizeof(linea), i * 10, estados[i].b, estados[i].lx, estados[i].ly);
		len += (size_t)snprintf(texto + len, sizeof(texto) - len, "%s\n", linea);
	}
	CHECK_EQ(guion_parse(&g, texto, err, sizeof(err)), 0);
	CHECK_EQ(g.n, sizeof(estados) / sizeof(estados[0]));
	for(i = 0; i < g.n && i < sizeof(estados) / sizeof(estados[0]); i++){
		CHECK_EQ(g.l[i].cuadro, i * 10);
		CHECK_EQ(g.l[i].botones, estados[i].b);
		CHECK_EQ(g.l[i].lx, estados[i].lx);
		CHECK_EQ(g.l[i].ly, estados[i].ly);
		CHECK_EQ(g.l[i].flags, GUION_MANDO);
	}
	guion_free(&g);
}

static void test_teclado(void){
	static const char *const textos[] = { "VEGETA", "", "con \"comillas\"", "barra \\ y # almohadilla", NULL };
	char texto[1024], linea[400], err[128];
	size_t len = 0;
	u32 i;
	Guion g;
	/* Lo que escribe la grabación se vuelve a leer igual */
	for(i = 0; i < sizeof(textos) / sizeof(textos[0]); i++){
		guion_format_teclado(linea, sizeof(linea), 100 + i, textos[i]);
		len += (size_t)snprintf(texto + len, sizeof(texto) - len, "%s\n", linea);
	}
	CHECK_EQ(guion_parse(&g, texto, err, sizeof(err)), 0);
	CHECK_EQ(g.n, sizeof(textos) / sizeof(textos[0]));
	for(i = 0; i < g.n && i < sizeof(textos) / sizeof(textos[0]); i++){
		CHECK_EQ(g.l[i].flags, GUION_TECLADO);
		CHECK_EQ(g.l[i].cuadro, 100 + i);
		if(textos[i]) CHECK(g.l[i].texto && !strcmp(g.l[i].texto, textos[i]));
		else CHECK(g.l[i].texto == NULL);
	}
	guion_free(&g);
	CHECK_EQ(guion_parse(&g, "5 teclado \"A\"  # comentario\n6 teclado cancelar\n", err, sizeof(err)), 0);
	CHECK_EQ(g.n, 2);
	guion_free(&g);
	/* Errores */
	CHECK(guion_parse(&g, "5 teclado\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "5 teclado \"sin cerrar\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "5 teclado \"A\" CRUZ\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "5 CRUZ teclado \"A\"\n", err, sizeof(err)) != 0);
	CHECK(guion_parse(&g, "5 teclado otra\n", err, sizeof(err)) != 0);
}

static void test_cuantizar(void){
	int v;
	CHECK_EQ(guion_cuantizar(128), 128);
	CHECK_EQ(guion_cuantizar(128 + 23), 128);
	CHECK_EQ(guion_cuantizar(128 - 23), 128);
	CHECK_EQ(guion_cuantizar(0), 0);
	CHECK_EQ(guion_cuantizar(255), 255);
	CHECK_EQ(guion_cuantizar(128 + 24), 128 + 24);
	/* Redondear dos veces no cambia nada y los pasos son de 8 */
	for(v = 0; v < 256; v++){
		u8 q = guion_cuantizar((u8)v);
		CHECK_EQ(guion_cuantizar(q), q);
		CHECK(q == 255 || (q - 128) % 8 == 0);
	}
}

static int write_file(const char *path, const char *text){
	FILE *f = fopen(path, "wb");
	if(!f) return -1;
	fputs(text, f);
	return fclose(f);
}

static int file_is(const char *path, const char *text){
	char buf[64] = "";
	FILE *f = fopen(path, "rb");
	size_t n;
	if(!f) return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return !strcmp(buf, text);
}

static void test_partidas(void){
	char dir[256], base[256], p[400];
	snprintf(dir, sizeof(dir), "build-test-guion.partidas");
	snprintf(base, sizeof(base), "build-test-guion.partidas_base");
	/* Por seguridad, solo carpetas .partidas */
	CHECK(guion_preparar_partidas("build-test-guion", NULL) != 0);
	CHECK(guion_preparar_partidas("SAVEDATA", NULL) != 0);

	mkdir(base, 0777);
	snprintf(p, sizeof(p), "%s/ULUS10537DATA00", base);
	mkdir(p, 0777);
	snprintf(p, sizeof(p), "%s/ULUS10537DATA00/DATA.BIN", base);
	CHECK_EQ(write_file(p, "partida"), 0);

	CHECK_EQ(guion_preparar_partidas(dir, base), 0);
	snprintf(p, sizeof(p), "%s/ULUS10537DATA00/DATA.BIN", dir);
	CHECK(file_is(p, "partida"));
	/* Lo que el juego escriba durante la prueba desaparece en la siguiente */
	CHECK_EQ(write_file(p, "otra cosa"), 0);
	snprintf(p, sizeof(p), "%s/NUEVA.BIN", dir);
	CHECK_EQ(write_file(p, "x"), 0);
	CHECK_EQ(guion_preparar_partidas(dir, base), 0);
	CHECK(access(p, F_OK) != 0);
	snprintf(p, sizeof(p), "%s/ULUS10537DATA00/DATA.BIN", dir);
	CHECK(file_is(p, "partida"));
	/* Sin base: carpeta vacía */
	CHECK_EQ(guion_preparar_partidas(dir, "no-existe"), 0);
	CHECK(access(p, F_OK) != 0);

	/* Limpieza */
	snprintf(p, sizeof(p), "%s/ULUS10537DATA00/DATA.BIN", base);
	unlink(p);
	snprintf(p, sizeof(p), "%s/ULUS10537DATA00", base);
	rmdir(p);
	rmdir(base);
	rmdir(dir);
}

static void test_opt(void){
	char buf[256];
	opt_reset();
	CHECK_EQ(opt_find("ge_vdec_esp"), OPT_GE_VDEC_ESP);
	CHECK_EQ(opt_find("no_existe"), -1);
	CHECK_EQ(opt_set("no_existe", 1), -1);
	/* Todos los nombres de la hoja de ruta existen y son distintos */
	{
		int i, j;
		for(i = 0; i < OPT_N; i++){
			CHECK(opt_info[i].nombre != NULL);
			for(j = 0; j < i; j++) CHECK(strcmp(opt_info[i].nombre, opt_info[j].nombre) != 0);
			CHECK_EQ(opt_find(opt_info[i].nombre), i);
		}
	}
	CHECK_EQ(opt_parse("ge_vdec_esp=2"), -1);
	CHECK_EQ(opt_parse("ge_vdec_esp"), -1);
	CHECK_EQ(opt_parse("=1"), -1);
	/* Una optimización que aún no existe no se activa */
	if(!opt_info[OPT_JIT].lista){
		CHECK_EQ(opt_parse("jit 1"), 1);
		CHECK_EQ(opt_on[OPT_JIT], 0);
	}
	/* Las implementadas se cambian con los dos formatos */
	{
		int i;
		for(i = 0; i < OPT_N; i++){
			char t[64];
			if(!opt_info[i].lista) continue;
			snprintf(t, sizeof(t), "%s=0", opt_info[i].nombre);
			CHECK_EQ(opt_parse(t), 0);
			CHECK_EQ(opt_on[i], 0);
			snprintf(t, sizeof(t), "%s 1", opt_info[i].nombre);
			CHECK_EQ(opt_parse(t), 0);
			CHECK_EQ(opt_on[i], 1);
		}
	}
	opt_describe(buf, sizeof(buf));
	CHECK(buf[0] != 0);
	opt_reset();
}

void test_guion(void){
	printf("Guion de botones e interruptores\n");
	test_parse();
	test_errores();
	test_ida_y_vuelta();
	test_teclado();
	test_cuantizar();
	test_partidas();
	test_opt();
}
