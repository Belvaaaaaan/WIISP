/**
 * WIISP - io.c
 * HLE de IoFileMgrForUser: archivos, stdout y el dispositivo "emulator:".
 *
 * ms0:/, host0:/, umd0:/ y disc0:/ apuntan a una carpeta del anfitrión (la
 * del ejecutable). Las rutas con ".." se rechazan para que un programa no
 * pueda salir de esa carpeta.
 *
 * "emulator:" / "kemulator:" es la convención de pspautotests y PPSSPP para
 * que un test detecte que corre en un emulador y le mande su salida.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include "hle/hle.h"
#include "core/memory.h"

#define MAX_FILES 64
#define SCE_KERNEL_ERROR_BADF 0x80020323u
#define SCE_KERNEL_ERROR_MFILE 0x80020320u
#define SCE_KERNEL_ERROR_UNSUP 0x80020325u

#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

#define EMULATOR_DEVCTL_GET_HAS_DISPLAY 0x01
#define EMULATOR_DEVCTL_SEND_OUTPUT     0x02
#define EMULATOR_DEVCTL_IS_EMULATOR     0x03

static FILE *files[MAX_FILES];
static char host_dir[256];
static char cwd[128];

void io_init(const char *dir){
	io_shutdown();
	snprintf(host_dir, sizeof(host_dir), "%s", dir && dir[0] ? dir : ".");
	cwd[0] = 0;
}

void io_shutdown(void){
	int i;
	for(i = 3; i < MAX_FILES; i++)
		if(files[i]){ fclose(files[i]); files[i] = NULL; }
}

/* Traduce una ruta de la PSP a una del anfitrión. Devuelve 0 si es válida. */
static int host_path(u32 addr, char *out, u32 out_size){
	char path[256];
	const char *p, *colon;
	if(!addr || mem_read_cstr(addr, path, sizeof(path))) return -1;
	if(strstr(path, "..")) return -1;
	colon = strchr(path, ':');
	if(colon){
		static const char *const devices[] = { "ms0", "host0", "umd0", "disc0", "fatms0" };
		u32 i, n = (u32)(colon - path);
		for(i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
			if(strlen(devices[i]) == n && !memcmp(path, devices[i], n)) break;
		if(i == sizeof(devices) / sizeof(devices[0])) return -1;
		p = colon + 1;
		snprintf(out, out_size, "%s/%s", host_dir, p[0] == '/' ? p + 1 : p);
	} else {
		snprintf(out, out_size, "%s/%s%s%s", host_dir, cwd, cwd[0] ? "/" : "",
		         path[0] == '/' ? path + 1 : path);
	}
	return 0;
}

static FILE *get_file(u32 fd){
	return fd < MAX_FILES ? files[fd] : NULL;
}

static void sceIoOpen(void){
	char path[640];
	u32 flags = ARG(1);
	const char *mode;
	FILE *f;
	int fd;

	if(host_path(ARG(0), path, sizeof(path))){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	if(flags & PSP_O_APPEND) mode = "ab";
	else if((flags & 3) == PSP_O_RDONLY) mode = "rb";
	else if(flags & PSP_O_TRUNC) mode = (flags & 1) ? "w+b" : "wb";
	else mode = "r+b";

	f = fopen(path, mode);
	if(!f && (flags & PSP_O_CREAT) && !strcmp(mode, "r+b")) f = fopen(path, "w+b");
	if(!f){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	for(fd = 3; fd < MAX_FILES && files[fd]; fd++);
	if(fd == MAX_FILES){ fclose(f); RETURN(SCE_KERNEL_ERROR_MFILE); return; }
	files[fd] = f;
	RETURN(fd);
}

static void sceIoClose(void){
	FILE *f = get_file(ARG(0));
	if(!f){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	fclose(f);
	files[ARG(0)] = NULL;
	RETURN(0);
}

static void sceIoRead(void){
	u32 fd = ARG(0), buf = ARG(1), size = ARG(2);
	FILE *f = get_file(fd);
	u8 *p;
	if(fd == 0){ RETURN(0); return; }
	if(!f){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	p = mem_ptr(buf, size);
	if(!p){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	RETURN(fread(p, 1, size, f));
}

static void sceIoWrite(void){
	u32 fd = ARG(0), buf = ARG(1), size = ARG(2);
	u8 *p = mem_ptr(buf, size);
	if(!p && size){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	if(fd == 1 || fd == 2){
		hle_output((const char *)p, size);
		RETURN(size);
		return;
	}
	if(!get_file(fd)){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	RETURN(fwrite(p, 1, size, files[fd]));
}

static s64 do_seek(u32 fd, s64 offset, u32 whence){
	FILE *f = get_file(fd);
	static const int whences[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	if(!f) return (s32)SCE_KERNEL_ERROR_BADF;
	if(whence > 2 || fseek(f, (long)offset, whences[whence])) return (s32)0x80020324u; /* INVAL */
	return ftell(f);
}

/* El offset de 64 bits va en el par de registros a2:a3 */
static void sceIoLseek(void){
	s64 r = do_seek(ARG(0), (s64)(((u64)ARG(3) << 32) | ARG(2)), ARG(4));
	RETURN64(r);
}

static void sceIoLseek32(void){
	RETURN((u32)do_seek(ARG(0), (s32)ARG(1), ARG(2)));
}

static void sceIoChdir(void){
	char path[128];
	if(mem_read_cstr(ARG(0), path, sizeof(path)) || strstr(path, "..")){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	{
		const char *p = strchr(path, ':');
		p = p ? p + 1 : path;
		while(*p == '/') p++;
		snprintf(cwd, sizeof(cwd), "%s", p);
	}
	RETURN(0);
}

static void sceIoDevctl(void){
	char dev[32];
	u32 cmd = ARG(1), in = ARG(2), in_len = ARG(3), out = ARG(4), out_len = ARG(5);
	mem_read_cstr(ARG(0), dev, sizeof(dev));
	if(strcmp(dev, "emulator:") && strcmp(dev, "kemulator:")){
		RETURN(SCE_KERNEL_ERROR_UNSUP);
		return;
	}
	switch(cmd){
	case EMULATOR_DEVCTL_GET_HAS_DISPLAY:
		if(out && out_len >= 4) mem_write32(out, 0);
		break;
	case EMULATOR_DEVCTL_SEND_OUTPUT: {
		u8 *p = mem_ptr(in, in_len);
		if(p) hle_output((const char *)p, in_len);
		break;
	}
	case EMULATOR_DEVCTL_IS_EMULATOR:
		break;
	default:
		break; /* mandos simulados y capturas: aún no */
	}
	RETURN(0);
}

static const HleFunction io_user[] = {
	{ "sceIoOpen", sceIoOpen },
	{ "sceIoClose", sceIoClose },
	{ "sceIoRead", sceIoRead },
	{ "sceIoWrite", sceIoWrite },
	{ "sceIoLseek", sceIoLseek },
	{ "sceIoLseek32", sceIoLseek32 },
	{ "sceIoChdir", sceIoChdir },
	{ "sceIoDevctl", sceIoDevctl },
};

const HleLibrary hle_io_libs[] = {
	HLE_LIBRARY("IoFileMgrForUser", io_user),
};
const u32 hle_io_libs_count = sizeof(hle_io_libs) / sizeof(hle_io_libs[0]);
