/**
 * WIISP - frontend/guion.c
 * Lectura y escritura de guiones de botones (ver guion.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)   /* el CLI para Windows */
#endif
#include "frontend/guion.h"
#include "frontend/app.h"

#define GUION_MAX_BYTES (4u * 1024 * 1024)

static const struct { const char *nombre; u32 bit; } botones[] = {
	{ "SELECT", APP_BTN_SELECT }, { "START", APP_BTN_START },
	{ "ARRIBA", APP_BTN_UP }, { "DERECHA", APP_BTN_RIGHT },
	{ "ABAJO", APP_BTN_DOWN }, { "IZQUIERDA", APP_BTN_LEFT },
	{ "L", APP_BTN_LTRIGGER }, { "R", APP_BTN_RTRIGGER },
	{ "TRIANGULO", APP_BTN_TRIANGLE }, { "CIRCULO", APP_BTN_CIRCLE },
	{ "CRUZ", APP_BTN_CROSS }, { "CUADRADO", APP_BTN_SQUARE },
};
#define N_BOTONES (sizeof(botones) / sizeof(botones[0]))

void guion_free(Guion *g){
	u32 i;
	for(i = 0; i < g->n; i++) free(g->l[i].texto);
	free(g->l);
	memset(g, 0, sizeof(*g));
}

static int add(Guion *g, const GuionLinea *l){
	if(g->n == g->cap){
		u32 cap = g->cap ? g->cap * 2 : 64;
		GuionLinea *nl = realloc(g->l, cap * sizeof(*nl));
		if(!nl) return -1;
		g->l = nl;
		g->cap = cap;
	}
	g->l[g->n++] = *l;
	return 0;
}

/* Siguiente palabra de la línea (separada por espacios); NULL al final */
static const char *word(const char **p, size_t *len){
	const char *s = *p;
	while(*s == ' ' || *s == '\t') s++;
	if(!*s || *s == '#') return NULL;
	*len = 0;
	while(s[*len] && s[*len] != ' ' && s[*len] != '\t' && s[*len] != '#') (*len)++;
	*p = s + *len;
	return s;
}

static int is_word(const char *w, size_t len, const char *nombre){
	return strlen(nombre) == len && !strncasecmp(w, nombre, len);
}

static int number(const char *w, size_t len, u32 max, u32 *out){
	u32 v = 0;
	size_t i;
	if(!len || len > 10) return -1;
	for(i = 0; i < len; i++){
		if(w[i] < '0' || w[i] > '9') return -1;
		v = v * 10 + (u32)(w[i] - '0');
	}
	if(v > max) return -1;
	*out = v;
	return 0;
}

/* Resto de una línea "teclado": "TEXTO" (con \" y \\) o cancelar, y nada
   más salvo un comentario. Deja el texto en l->texto (NULL = cancelar). */
static int teclado(const char **p, GuionLinea *l){
	const char *s = *p, *w;
	size_t wl, n = 0;
	char buf[300];
	while(*s == ' ' || *s == '\t') s++;
	if(*s == '"'){
		for(s++; *s && *s != '"'; s++){
			if(*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
			if(n + 1 >= sizeof(buf)) return -1;
			buf[n++] = *s;
		}
		if(*s != '"') return -1;
		buf[n] = 0;
		*p = s + 1;
		if(word(p, &wl)) return -1;   /* algo más detrás */
		l->texto = malloc(n + 1);
		if(!l->texto) return -1;
		memcpy(l->texto, buf, n + 1);
		return 0;
	}
	*p = s;
	if(!(w = word(p, &wl)) || !is_word(w, wl, "cancelar") || word(p, &wl)) return -1;
	l->texto = NULL;
	return 0;
}

static int fail(Guion *g, char *err, size_t errlen, u32 linea, const char *msg){
	if(err && errlen) snprintf(err, errlen, "linea %u: %s", linea, msg);
	guion_free(g);
	return -1;
}

int guion_parse(Guion *g, const char *texto, char *err, size_t errlen){
	const char *p = texto;
	u32 linea = 0, ultimo = 0;
	memset(g, 0, sizeof(*g));
	if(err && errlen) err[0] = 0;
	while(*p){
		const char *eol = strchr(p, '\n'), *w;
		char buf[512];
		const char *q = buf;
		size_t len = eol ? (size_t)(eol - p) : strlen(p), wl;
		GuionLinea l;
		linea++;
		if(len >= sizeof(buf)) return fail(g, err, errlen, linea, "demasiado larga");
		memcpy(buf, p, len);
		buf[len] = 0;
		if(len && buf[len - 1] == '\r') buf[len - 1] = 0;
		p = eol ? eol + 1 : p + len;

		if(!(w = word(&q, &wl))) continue;   /* vacía o comentario */
		memset(&l, 0, sizeof(l));
		l.lx = l.ly = 128;
		if(number(w, wl, 0xFFFFFFFFu, &l.cuadro)) return fail(g, err, errlen, linea, "falta el numero de cuadro");
		if(l.cuadro < ultimo) return fail(g, err, errlen, linea, "los cuadros deben ir en orden");
		ultimo = l.cuadro;
		while((w = word(&q, &wl))){
			size_t i;
			if(is_word(w, wl, "-")){ l.flags |= GUION_MANDO; continue; }
			if(is_word(w, wl, "captura")){ l.flags |= GUION_CAPTURA; continue; }
			if(is_word(w, wl, "fin")){ l.flags |= GUION_FIN; continue; }
			if(is_word(w, wl, "teclado")){
				if(l.flags) return fail(g, err, errlen, linea, "teclado va solo en su linea");
				if(teclado(&q, &l)) return fail(g, err, errlen, linea, "teclado necesita \"TEXTO\" o cancelar");
				l.flags = GUION_TECLADO;
				break;
			}
			if(is_word(w, wl, "stick")){
				const char *a, *b;
				size_t al, bl;
				u32 x, y;
				if(!(a = word(&q, &al)) || !(b = word(&q, &bl)) || number(a, al, 255, &x) || number(b, bl, 255, &y))
					return fail(g, err, errlen, linea, "stick necesita X e Y entre 0 y 255");
				l.lx = (u8)x;
				l.ly = (u8)y;
				l.flags |= GUION_MANDO;
				continue;
			}
			for(i = 0; i < N_BOTONES; i++)
				if(is_word(w, wl, botones[i].nombre)) break;
			if(i == N_BOTONES){
				char msg[64];
				snprintf(msg, sizeof(msg), "no se entiende \"%.*s\"", (int)(wl < 32 ? wl : 32), w);
				return fail(g, err, errlen, linea, msg);
			}
			l.botones |= botones[i].bit;
			l.flags |= GUION_MANDO;
		}
		if(!l.flags) return fail(g, err, errlen, linea, "linea sin nada que hacer");
		if(add(g, &l)){ free(l.texto); return fail(g, err, errlen, linea, "sin memoria"); }
	}
	return 0;
}

int guion_load(Guion *g, const char *path, char *err, size_t errlen){
	FILE *f = fopen(path, "rb");
	char *buf;
	long size;
	int r;
	memset(g, 0, sizeof(*g));
	if(!f){
		if(err && errlen) snprintf(err, errlen, "no se puede abrir %s", path);
		return -1;
	}
	if(fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || (unsigned long)size > GUION_MAX_BYTES ||
	   fseek(f, 0, SEEK_SET) || !(buf = malloc((size_t)size + 1))){
		fclose(f);
		if(err && errlen) snprintf(err, errlen, "%s: demasiado grande o ilegible", path);
		return -1;
	}
	if(fread(buf, 1, (size_t)size, f) != (size_t)size){
		free(buf);
		fclose(f);
		if(err && errlen) snprintf(err, errlen, "%s: error de lectura", path);
		return -1;
	}
	fclose(f);
	buf[size] = 0;
	r = guion_parse(g, buf, err, errlen);
	free(buf);
	return r;
}

u32 guion_fin(const Guion *g){
	u32 i;
	for(i = 0; i < g->n; i++)
		if(g->l[i].flags & GUION_FIN) return g->l[i].cuadro;
	return 0;
}

int guion_format(char *out, size_t size, u32 cuadro, u32 b, u8 lx, u8 ly){
	size_t len;
	size_t i;
	int any = 0;
	if(!size) return 0;
	len = (size_t)snprintf(out, size, "%u", cuadro);
	for(i = 0; i < N_BOTONES && len < size; i++)
		if(b & botones[i].bit){
			len += (size_t)snprintf(out + len, size - len, " %s", botones[i].nombre);
			any = 1;
		}
	if(len < size && (lx != 128 || ly != 128)){
		len += (size_t)snprintf(out + len, size - len, " stick %u %u", lx, ly);
		any = 1;
	}
	if(len < size && !any) len += (size_t)snprintf(out + len, size - len, " -");
	return (int)(len < size ? len : size - 1);
}

int guion_format_teclado(char *out, size_t size, u32 cuadro, const char *texto){
	size_t len;
	if(!size) return 0;
	if(!texto) len = (size_t)snprintf(out, size, "%u teclado cancelar", cuadro);
	else {
		len = (size_t)snprintf(out, size, "%u teclado \"", cuadro);
		for(; *texto && len + 3 < size; texto++){
			if(*texto == '"' || *texto == '\\') out[len++] = '\\';
			out[len++] = *texto;
		}
		if(len + 1 < size) out[len++] = '"';
		out[len] = 0;
	}
	return (int)(len < size ? len : size - 1);
}

u8 guion_cuantizar(u8 eje){
	int d = (int)eje - 128, v;
	if(d > -24 && d < 24) return 128;
	v = 128 + (d >= 0 ? (d + 4) / 8 : -((-d + 4) / 8)) * 8;
	return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* --- Partidas de prueba ------------------------------------------------- */

static int is_dir(const char *path){
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* Borra el contenido de dir (no dir mismo) */
static int vaciar(const char *dir){
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[700];
	int r = 0;
	if(!d) return -1;
	while((e = readdir(d))){
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)){ r = -1; continue; }
		if(is_dir(path)){
			if(vaciar(path) || rmdir(path)) r = -1;
		} else if(unlink(path)) r = -1;
	}
	closedir(d);
	return r;
}

static int copiar_archivo(const char *from, const char *to){
	FILE *a = fopen(from, "rb"), *b;
	char buf[16384];
	size_t n;
	int r = 0;
	if(!a) return -1;
	if(!(b = fopen(to, "wb"))){ fclose(a); return -1; }
	while((n = fread(buf, 1, sizeof(buf), a)) > 0)
		if(fwrite(buf, 1, n, b) != n){ r = -1; break; }
	if(ferror(a)) r = -1;
	fclose(a);
	if(fclose(b)) r = -1;
	return r;
}

/* Copia el contenido de from dentro de to (que ya existe) */
static int copiar(const char *from, const char *to){
	DIR *d = opendir(from);
	struct dirent *e;
	char a[700], b[700];
	int r = 0;
	if(!d) return -1;
	while((e = readdir(d))){
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if((size_t)snprintf(a, sizeof(a), "%s/%s", from, e->d_name) >= sizeof(a) ||
		   (size_t)snprintf(b, sizeof(b), "%s/%s", to, e->d_name) >= sizeof(b)){ r = -1; continue; }
		if(is_dir(a)){
			if((mkdir(b, 0777) && !is_dir(b)) || copiar(a, b)) r = -1;
		} else if(copiar_archivo(a, b)) r = -1;
	}
	closedir(d);
	return r;
}

int guion_preparar_partidas(const char *dir, const char *base){
	static const char sufijo[] = ".partidas";
	size_t n = strlen(dir);
	while(n > 1 && dir[n - 1] == '/') n--;
	if(n < sizeof(sufijo) || strncmp(dir + n - (sizeof(sufijo) - 1), sufijo, sizeof(sufijo) - 1)) return -1;
	if(is_dir(dir)){
		if(vaciar(dir)) return -1;
	} else if(mkdir(dir, 0777)) return -1;
	if(base && base[0] && is_dir(base)) return copiar(base, dir);
	return 0;
}
