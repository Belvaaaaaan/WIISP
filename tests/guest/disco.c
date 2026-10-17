/**
 * WIISP - tests/guest/disco.c
 * Lecturas del UMD (se ejecuta desde una imagen: make la crea con mkiso.py,
 * en ISO y en CSO, con este programa como BOOT.BIN y DATOS.BIN con el patrón
 * de patron()). Lecturas de varios tamaños, saltos, una asíncrona, por
 * sce_lbn y por sectores (umd0:). De cada una: lo devuelto, los bytes que
 * no son los del patrón y el tiempo emulado que tardó, que no puede cambiar
 * aunque cambie cómo lee el anfitrión.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

typedef unsigned char u8;

int sceIoOpen(const char *path, int flags, int mode);
int sceIoClose(int fd);
int sceIoRead(int fd, void *buf, u32 size);
long long sceIoLseek(int fd, long long offset, int whence);
int sceIoGetstat(const char *path, void *stat);
int sceIoReadAsync(int fd, void *buf, u32 size);
int sceIoWaitAsync(int fd, long long *res);
int sceIoPollAsync(int fd, long long *res);

#define TAM   (700u * 1024 + 123)   /* lo que mide DATOS.BIN (en el Makefile) */
#define RUTA  "disc0:/PSP_GAME/USRDIR/DATOS.BIN"

static u8 buf[TAM + 4096] __attribute__((aligned(64)));
static u32 stat_[22];

static u8 patron(u32 i){ return (u8)(((i ^ (i >> 8) ^ (i >> 16)) * 13 + 7) & 0xFF); }

/* Bytes de buf[0..n) que no son los del archivo desde off */
static u32 malos(u32 off, u32 n){
	u32 i, m = 0;
	for(i = 0; i < n; i++) m += buf[i] != patron(off + i);
	return m;
}

static void leer(int fd, u32 off, u32 n){
	u32 t0 = sceKernelGetSystemTimeLow(), t;
	int r = sceIoRead(fd, buf, n);
	t = sceKernelGetSystemTimeLow() - t0;
	printf_("disco: leer %u en %u: %d, malos %u, %u us\n", n, off, r, r > 0 ? malos(off, (u32)r) : 0, t);
}

static char *hex(char *p, u32 v){
	int k;
	*p++ = '0'; *p++ = 'x';
	for(k = 28; k >= 0; k -= 4) *p++ = "0123456789abcdef"[(v >> k) & 15];
	return p;
}

int main(void){
	int fd, r;
	u32 i, off, lba;
	long long res = -1;
	char lbn[64], *p;

	r = sceIoGetstat(RUTA, stat_);
	lba = stat_[16];
	printf_("disco: getstat %d, tamano %u\n", r, stat_[2]);

	fd = sceIoOpen(RUTA, 1, 0);
	printf_("disco: abrir %s\n", fd >= 0 ? "bien" : "mal");
	/* Seguidas: pequeña, una que cruza sectores, una grande */
	leer(fd, 0, 100);
	leer(fd, 100, 5000);
	leer(fd, 5100, 300 * 1024);
	/* Saltos */
	off = (u32)sceIoLseek(fd, 123457, 0);
	leer(fd, off, 64 * 1024);
	off = (u32)sceIoLseek(fd, -1000, 2);
	leer(fd, off, 5000);   /* pide más de lo que queda */
	leer(fd, TAM, 10);     /* ya en el final */
	/* Asíncrona */
	sceIoLseek(fd, 2048, 0);
	for(i = 0; i < 1024; i++) buf[i] = 0;
	r = sceIoReadAsync(fd, buf, 200 * 1024);
	printf_("disco: asincrona %d\n", r);
	r = sceIoPollAsync(fd, &res);
	printf_("disco: poll %d\n", r);
	if(r == 1){
		r = sceIoWaitAsync(fd, &res);
		printf_("disco: wait %d\n", r);
	}
	printf_("disco: resultado %d, malos %u\n", (int)res, res > 0 ? malos(2048, (u32)res) : 0);
	sceIoClose(fd);

	/* Todo de una vez por sce_lbn (lo que hacen muchos juegos) */
	p = lbn;
	{
		const char *a = "disc0:/sce_lbn";
		while(*a) *p++ = *a++;
	}
	p = hex(p, lba);
	*p++ = '_'; *p++ = 's'; *p++ = 'i'; *p++ = 'z'; *p++ = 'e';
	p = hex(p, TAM);
	*p = 0;
	fd = sceIoOpen(lbn, 1, 0);
	printf_("disco: sce_lbn %s\n", fd >= 0 ? "bien" : "mal");
	leer(fd, 0, TAM);
	sceIoClose(fd);

	/* Por sectores: tres desde el segundo del archivo */
	fd = sceIoOpen("umd0:", 1, 0);
	printf_("disco: umd0 %s\n", fd >= 0 ? "bien" : "mal");
	sceIoLseek(fd, lba + 1, 0);
	{
		u32 t0 = sceKernelGetSystemTimeLow(), t;
		r = sceIoRead(fd, buf, 3);
		t = sceKernelGetSystemTimeLow() - t0;
		printf_("disco: sectores %d, malos %u, %u us\n", r, r > 0 ? malos(2048, (u32)r * 2048) : 0, t);
	}
	sceIoClose(fd);
	return 0;
}
