/**
 * WIISP - tests/test_io.c
 * Pruebas del HLE de archivos sobre un UMD montado: disc0:/, rutas
 * relativas, umd0: por sectores, sce_lbn, ioctl, directorios, E/S
 * asíncrona y sceUmd. Llama a las funciones del HLE como lo haría un
 * syscall: argumentos en a0..a3/t0..t3 y resultado en v0/v1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "hle/hle.h"
#include "core/memory.h"
#include "loader/disc.h"
#include "loader/inflate.h"
#include "vectors.h"

#define STR 0x08800000u   /* cadenas */
#define BUF 0x08900000u   /* datos */
#define RES 0x08A00000u   /* resultados */

static u32 call(const char *name, u32 a0, u32 a1, u32 a2, u32 a3, u32 t0, u32 t1){
	u32 l, f;
	for(l = 0; l < hle_io_libs_count; l++)
		for(f = 0; f < hle_io_libs[l].count; f++)
			if(!strcmp(hle_io_libs[l].funcs[f].name, name)){
				cpu.r[R_A0] = a0; cpu.r[R_A1] = a1; cpu.r[R_A2] = a2; cpu.r[R_A3] = a3;
				cpu.r[R_T0] = t0; cpu.r[R_T1] = t1;
				hle_io_libs[l].funcs[f].func();
				return cpu.r[R_V0];
			}
	printf("  falta %s\n", name);
	return 0xDEADBEEF;
}

static u32 str(const char *s){
	static u32 next;
	u32 a = STR + (next & 0xFFF0);
	next += 0x100;
	memcpy(mem_ptr(a, (u32)strlen(s) + 1), s, strlen(s) + 1);
	return a;
}

void test_io(void){
	static u8 iso[1 << 17];
	int n = gunzip(iso_gz, sizeof(iso_gz), iso, sizeof(iso)), ok;
	u32 fd, i, lba, r;
	FILE *f;
	char lbn[64];

	printf("HLE de archivos sobre el UMD\n");
	f = fopen("build-pc/io.iso", "wb");
	if(!f || n <= 0){ CHECK(0); return; }
	fwrite(iso, 1, (size_t)n, f);
	fclose(f);
	CHECK_EQ(disc_open("build-pc/io.iso"), 0);
	io_init("build-pc", 1);

	/* Ruta absoluta y lectura completa */
	fd = call("sceIoOpen", str("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	CHECK_EQ(call("sceIoRead", fd, BUF, 6000, 0, 0, 0), 5000);
	for(ok = 1, i = 0; i < 5000; i++) if(mem_read8(BUF + i) != (u8)(i * 13 + 5)) ok = 0;
	CHECK(ok);
	CHECK_EQ(call("sceIoRead", fd, BUF, 10, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoLseek32", fd, 100, 0, 0, 0, 0), 100);
	CHECK_EQ(call("sceIoRead", fd, BUF, 1, 0, 0, 0), 1);
	CHECK_EQ(mem_read8(BUF), (u8)(100 * 13 + 5));
	CHECK_EQ(call("sceIoLseek32", fd, 0, 2, 0, 0, 0), 5000);
	CHECK_EQ(call("sceIoWrite", fd, BUF, 4, 0, 0, 0), 0x8001001Eu);
	CHECK_EQ(call("sceIoClose", fd, 0, 0, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoClose", fd, 0, 0, 0, 0, 0), 0x80020323u);
	/* En el UMD solo se rechaza la escritura; CREAT o TRUNC se ignoran */
	CHECK_EQ(call("sceIoOpen", str("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"), 0x602, 0, 0, 0, 0), 0x8001B004u);
	fd = call("sceIoOpen", str("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"), 0x601, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);

	/* Rutas como en la PSP: '\\', dispositivo en mayúsculas, umd00:, espacios */
	fd = call("sceIoOpen", str("DISC0:\\PSP_GAME\\USRDIR\\DATA\\A.TXT"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	fd = call("sceIoOpen", str("  umd00:/PSP_GAME/USRDIR/DATA/A.TXT"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	fd = call("sceIoOpen", str("DATA\\A.TXT"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	/* Arrancando desde un disco, host0: es el disco */
	fd = call("sceIoOpen", str("host0:/UMD_DATA.BIN"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);

	/* Relativa al directorio actual (disc0:/PSP_GAME/USRDIR), sin mayúsculas */
	fd = call("sceIoOpen", str("DATA/A.TXT"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	CHECK_EQ(call("sceIoRead", fd, BUF, 100, 0, 0, 0), 11);
	CHECK(!memcmp(mem_ptr(BUF, 11), "hola disco\n", 11));
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	CHECK_EQ(call("sceIoChdir", str("data"), 0, 0, 0, 0, 0), 0);
	fd = call("sceIoOpen", str("a.txt"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	CHECK_EQ(call("sceIoOpen", str("NO.TXT"), 1, 0, 0, 0, 0), SCE_ERROR_FILE_NOT_FOUND);
	/* "." y ".." se resuelven; ".." en la raíz se queda en la raíz, así que
	   nunca se sale de ella */
	fd = call("sceIoOpen", str("../data/./a.txt"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	fd = call("sceIoOpen", str("disc0:/PSP_GAME/../PSP_GAME/USRDIR/DATA/A.TXT"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	fd = call("sceIoOpen", str("disc0:/../PSP_GAME/USRDIR/DATA/A.TXT"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	fd = call("sceIoOpen", str("ms0:/../io.iso"), 1, 0, 0, 0, 0);   /* build-pc/io.iso */
	CHECK((s32)fd >= 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	/* Saliendo de build-pc sí existiría: build-pc/build-pc/io.iso no */
	CHECK_EQ(call("sceIoOpen", str("ms0:/../../build-pc/io.iso"), 1, 0, 0, 0, 0), SCE_ERROR_FILE_NOT_FOUND);

	/* El disco entero por sectores: el descriptor de volumen en el 16 */
	fd = call("sceIoOpen", str("umd0:"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	CHECK_EQ(call("sceIoLseek32", fd, 16, 0, 0, 0, 0), 16);
	CHECK_EQ(call("sceIoRead", fd, BUF, 1, 0, 0, 0), 1);
	CHECK(!memcmp(mem_ptr(BUF + 1, 5), "CD001", 5));
	call("sceIoClose", fd, 0, 0, 0, 0, 0);

	/* stat: el sector inicial en st_private[0]; luego sce_lbn */
	CHECK_EQ(call("sceIoGetstat", str("disc0:/PSP_GAME/USRDIR/DATA/GRANDE.BIN"), RES, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_read32(RES + 8), 9000);
	lba = mem_read32(RES + 64);
	CHECK(lba > 16);
	snprintf(lbn, sizeof(lbn), "disc0:/sce_lbn0x%x_size0x%x", lba, 9000);
	fd = call("sceIoOpen", str(lbn), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	CHECK_EQ(call("sceIoRead", fd, BUF, 20000, 0, 0, 0), 9000);
	for(ok = 1, i = 0; i < 9000; i++) if(mem_read8(BUF + i) != (u8)(i * 7)) ok = 0;
	CHECK(ok);
	/* ioctl de UMD: sector inicial, tamaño y seek */
	CHECK_EQ(call("sceIoIoctl", fd, 0x01020006, 0, 0, RES, 4), 0);
	CHECK_EQ(mem_read32(RES), lba);
	CHECK_EQ(call("sceIoIoctl", fd, 0x01020007, 0, 0, RES, 8), 0);
	CHECK_EQ(mem_read32(RES), 9000);
	mem_write32(RES + 16, 10); mem_write32(RES + 20, 0); mem_write32(RES + 24, 0); mem_write32(RES + 28, 0);
	CHECK_EQ(call("sceIoIoctl", fd, 0x01010005, RES + 16, 16, 0, 0), 0);
	CHECK_EQ(call("sceIoRead", fd, BUF, 1, 0, 0, 0), 1);
	CHECK_EQ(mem_read8(BUF), (u8)70);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);

	/* Directorio del disco: DATA, VACIO (sin "." ni "..", como en la PSP) */
	fd = call("sceIoDopen", str("disc0:/PSP_GAME/USRDIR"), 0, 0, 0, 0, 0);
	CHECK(fd >= 0x100);
	{
		static const char *const want[] = { "DATA", "VACIO" };
		for(i = 0; i < 2; i++){
			CHECK_EQ(call("sceIoDread", fd, RES, 0, 0, 0, 0), 1);
			CHECK(!strcmp((const char *)mem_ptr(RES + 88, 8), want[i]));
		}
		CHECK_EQ(mem_read32(RES) & 0x1000, 0x1000);   /* directorio */
		CHECK_EQ(call("sceIoDread", fd, RES, 0, 0, 0, 0), 0);
	}
	CHECK_EQ(call("sceIoDclose", fd, 0, 0, 0, 0, 0), 0);

	/* Asíncrona */
	fd = call("sceIoOpenAsync", str("disc0:/UMD_DATA.BIN"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	CHECK_EQ(call("sceIoWaitAsync", fd, RES, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_read32(RES), fd);
	CHECK_EQ(call("sceIoPollAsync", fd, RES, 0, 0, 0, 0), 0x8002032Au);   /* nada pendiente */
	CHECK_EQ(call("sceIoReadAsync", fd, BUF, 100, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoReadAsync", fd, BUF, 100, 0, 0, 0), 0x80020329u);   /* ocupado */
	CHECK_EQ(call("sceIoPollAsync", fd, RES, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_read32(RES), 21);
	CHECK(!memcmp(mem_ptr(BUF, 9), "ULUS99999", 9));
	CHECK_EQ(call("sceIoCloseAsync", fd, 0, 0, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoWaitAsync", fd, RES, 0, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoClose", fd, 0, 0, 0, 0, 0), 0x80020323u);
	fd = call("sceIoOpenAsync", str("disc0:/NO"), 1, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	CHECK_EQ(call("sceIoWaitAsync", fd, RES, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_read32(RES), SCE_ERROR_FILE_NOT_FOUND);
	/* Un sceIoOpenAsync fallido se libera al recoger el error */
	CHECK_EQ(call("sceIoClose", fd, 0, 0, 0, 0, 0), 0x80020323u);
	/* sceIoCloseAsync espera a que se recoja lo pendiente; sceIoCancel no
	   está disponible en el UMD */
	fd = call("sceIoOpen", str("disc0:/UMD_DATA.BIN"), 1, 0, 0, 0, 0);
	CHECK_EQ(call("sceIoReadAsync", fd, BUF, 4, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoCloseAsync", fd, 0, 0, 0, 0, 0), 0x80020329u);
	CHECK_EQ(call("sceIoCancel", fd, 0, 0, 0, 0, 0), 0x80020325u);
	CHECK_EQ(call("sceIoWaitAsync", fd, RES, 0, 0, 0, 0), 0);
	CHECK_EQ(call("sceIoClose", fd, 0, 0, 0, 0, 0), 0);

	/* Memory Stick: insertada, con la FAT montada y sin protección */
	mem_write32(RES, 0xFFFFFFFFu);
	CHECK_EQ(call("sceIoDevctl", str("mscmhc0:"), 0x02025801, 0, 0, RES, 4), 0);
	CHECK_EQ(mem_read32(RES), 4);
	CHECK_EQ(call("sceIoDevctl", str("mscmhc0:"), 0x02025806, 0, 0, RES, 4), 0);
	CHECK_EQ(mem_read32(RES), 1);
	mem_write32(RES, 0);
	CHECK_EQ(call("sceIoDevctl", str("fatms0:"), 0x02425823, 0, 0, RES, 4), 0);
	CHECK_EQ(mem_read32(RES), 1);
	mem_write32(RES, 0xFFFFFFFFu);
	CHECK_EQ(call("sceIoDevctl", str("fatms0:"), 0x02425824, 0, 0, RES, 4), 0);
	CHECK_EQ(mem_read32(RES), 0);
	mem_write32(RES + 32, RES + 64);
	CHECK_EQ(call("sceIoDevctl", str("ms0:"), 0x02425818, RES + 32, 4, 0, 0), 0);
	CHECK_EQ(mem_read32(RES + 64 + 12), 0x200);
	CHECK(mem_read32(RES + 64 + 4) > 0);
	/* Comandos del UMD con cualquier dispositivo */
	CHECK_EQ(call("sceIoDevctl", str("umd0:"), 0x01F300A5, RES + 32, 4, RES, 4), 0);
	CHECK_EQ(mem_read32(RES), 1);

	/* ms0: sigue en la carpeta del anfitrión */
	fd = call("sceIoOpen", str("ms0:/io_test.txt"), 0x602, 0, 0, 0, 0);
	CHECK((s32)fd >= 3);
	memcpy(mem_ptr(BUF, 3), "abc", 3);
	CHECK_EQ(call("sceIoWrite", fd, BUF, 3, 0, 0, 0), 3);
	call("sceIoClose", fd, 0, 0, 0, 0, 0);
	CHECK_EQ(call("sceIoRemove", str("ms0:/io_test.txt"), 0, 0, 0, 0, 0), 0);

	/* sceUmd */
	CHECK_EQ(call("sceUmdCheckMedium", 0, 0, 0, 0, 0, 0), 1);
	CHECK_EQ(call("sceUmdGetDriveStat", 0, 0, 0, 0, 0, 0), 0x12);
	CHECK_EQ(call("sceUmdActivate", 1, str("disc0:"), 0, 0, 0, 0), 0);
	CHECK_EQ(call("sceUmdGetDriveStat", 0, 0, 0, 0, 0, 0), 0x32);
	mem_write32(RES, 8);
	CHECK_EQ(call("sceUmdGetDiscInfo", RES, 0, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_read32(RES + 4), 0x10);

	io_shutdown();
	disc_close();
	r = call("sceUmdCheckMedium", 0, 0, 0, 0, 0, 0);
	CHECK_EQ(r, 0);
	remove("build-pc/io.iso");
}
