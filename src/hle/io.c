/**
 * WIISP - io.c
 * HLE de IoFileMgrForUser y sceUmdUser: archivos, el UMD, stdout y el
 * dispositivo "emulator:".
 *
 * Dispositivos:
 *   - ms0:/, fatms0:/ y host0:/ apuntan a una carpeta del anfitrión (la del
 *     ejecutable o la imagen). "." y ".." se resuelven, pero nunca se puede
 *     salir de esa carpeta. ms0:/PSP/GAME/WIISP, donde argv[0] dice que
 *     está el ejecutable, es esa misma carpeta.
 *   - disc0:/, umd0:/, umd1:/ y umd:/ son el UMD montado (loader/disc.c).
 *     "umd0:" sin ruta es el disco entero en modo sector: las lecturas y
 *     los desplazamientos cuentan sectores de 2048 bytes. "disc0:/sce_lbn
 *     0x<sector>_size0x<bytes>" abre un trozo del disco por su posición.
 *     Sin disco montado, apuntan a la carpeta del anfitrión (homebrew).
 *
 * Las rutas se leen como en la PSP: '\\' vale como '/', el dispositivo no
 * distingue mayúsculas, "umd00:" es umd0: y, arrancando desde un disco,
 * host0: es el disco.
 *
 * La Memory Stick (mscmhc0:, fatms0:) siempre está insertada: sus
 * sceIoDevctl responden como tal y avisan a sus callbacks al registrarlos.
 *
 * La E/S asíncrona (sceIo*Async) se completa en el acto y guarda el
 * resultado para sceIoWaitAsync/sceIoPollAsync.
 *
 * "emulator:" / "kemulator:" es la convención de pspautotests y PPSSPP para
 * que un test detecte que corre en un emulador y le mande su salida.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)   /* el CLI para Windows */
#endif
#include "hle/hle.h"
#include "core/memory.h"
#include "loader/disc.h"
#include "core/prof.h"

#define MAX_FILES 64
#define SCE_KERNEL_ERROR_BADF 0x80020323u
#define SCE_KERNEL_ERROR_MFILE 0x80020320u
#define SCE_KERNEL_ERROR_UNSUP 0x80020325u
#define SCE_KERNEL_ERROR_ASYNC_BUSY 0x80020329u
#define SCE_KERNEL_ERROR_NOASYNC 0x8002032Au
#define SCE_ERRNO_INVALID_ARGUMENT 0x80010016u
#define SCE_ERRNO_READ_ONLY 0x8001001Eu
#define SCE_ERRNO_INVALID_FLAG 0x8001B004u
#define SCE_ERRNO_IO_ERROR 0x80010005u
#define SCE_ERRNO_DEVICE_NOT_FOUND 0x80010013u
#define SCE_MEMSTICK_BAD_PARAMS 0x80220081u

#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

#define EMULATOR_DEVCTL_GET_HAS_DISPLAY 0x01
#define EMULATOR_DEVCTL_SEND_OUTPUT     0x02
#define EMULATOR_DEVCTL_IS_EMULATOR     0x03
#define EMULATOR_DEVCTL_EMIT_SCREENSHOT 0x20

#define MAX_DIRS 16
#define DIR_FD_BASE 0x100   /* los directorios usan otros identificadores */

enum { F_NONE, F_HOST, F_DISC, F_DISC_SECTORS, F_FAILED };

typedef struct {
	int kind;
	FILE *f;
	u32 lba;          /* disco: primer sector */
	u64 size, pos;    /* bytes, o sectores en F_DISC_SECTORS */
	int async_pending;
	s64 async_result;
	int by_sector;    /* abierto por sectores (umd0:, sce_lbn): sin espera */
	int free_on_collect;   /* sceIoOpenAsync fallido: se libera al recoger el error */
} IoFile;

typedef struct {
	int used;
	DIR *dir;            /* carpeta del anfitrión */
	char path[640];
	DiscEntry disc;      /* o directorio del disco */
	int disc_index;
	int is_disc;
	int entries_read;
} IoDir;

static IoFile files[MAX_FILES];
static IoDir dirs[MAX_DIRS];
static char host_dir[256];
#define EXEC_DIR "PSP/GAME/WIISP"   /* como en app.c */
#define SAVEDATA_DIR "PSP/SAVEDATA"
static char cwd[256];
static char savedata_dir[256];   /* "" = la de ms0:/ */        /* con dispositivo: "ms0:/PSP" o "disc0:/PSP_GAME/USRDIR" */
static int umd_activated;
static u32 umd_callback;   /* sceUmdRegisterUMDCallBack: solo hay uno */

/* Callbacks de inserción de la Memory Stick (sceIoDevctl): la tarjeta
   siempre está puesta (es la SD o el USB), así que se avisan al registrarlos */
static u32 umd_recheck(u32 stat, int *done);

#define MAX_MS_CALLBACKS 32
static u32 ms_callbacks[2][MAX_MS_CALLBACKS];   /* [0] mscmhc0:, [1] fatms0: */

void io_init(const char *dir, int boot_from_disc){
	io_shutdown();
	snprintf(host_dir, sizeof(host_dir), "%s", dir && dir[0] ? dir : ".");
	snprintf(cwd, sizeof(cwd), "%s", boot_from_disc ? "disc0:/PSP_GAME/USRDIR" : "ms0:/");
	umd_activated = 1;
	umd_callback = 0;
	kernel_set_wait_recheck(KWAIT_UMD, umd_recheck);
	memset(ms_callbacks, 0, sizeof(ms_callbacks));
}

void io_shutdown(void){
	int i;
	for(i = 3; i < MAX_FILES; i++){
		if(files[i].f) fclose(files[i].f);
		memset(&files[i], 0, sizeof(files[i]));
	}
	for(i = 0; i < MAX_DIRS; i++){
		if(dirs[i].dir) closedir(dirs[i].dir);
		memset(&dirs[i], 0, sizeof(dirs[i]));
	}
}

/* --- Rutas --------------------------------------------------------------------- */

typedef struct {
	int disc;            /* 1 = en el UMD montado */
	int whole_device;    /* "umd0:" sin ruta: el disco en modo sector */
	char path[512];      /* disco: ruta desde la raíz; anfitrión: ruta completa */
} Resolved;

static int is_disc_device(const char *dev, u32 n){
	static const char *const names[] = { "disc0", "umd0", "umd1", "umd", "isofs" };
	u32 i;
	for(i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if(strlen(names[i]) == n && !memcmp(dev, names[i], n)) return 1;
	return 0;
}

static int is_host_device(const char *dev, u32 n){
	static const char *const names[] = { "ms0", "host0", "fatms0" };
	u32 i;
	for(i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if(strlen(names[i]) == n && !memcmp(dev, names[i], n)) return 1;
	return 0;
}

/* Quita "." y ".." de una ruta sin dispositivo ni '/' inicial (en su
   sitio). Como en la PSP, ".." en la raíz se queda en la raíz. */
static int normalize_path(char *p){
	char *out = p, *in = p;
	while(*in){
		char *seg = in, *end = strchr(in, '/');
		size_t len = end ? (size_t)(end - in) : strlen(in);
		in = end ? end + 1 : in + len;
		if(len == 0 || (len == 1 && seg[0] == '.')) continue;
		if(len == 2 && seg[0] == '.' && seg[1] == '.'){
			if(out == p) continue;
			out--;
			while(out > p && out[-1] != '/') out--;
			continue;
		}
		memmove(out, seg, len);
		out += len;
		*out++ = '/';
	}
	if(out > p) out--;   /* sin la '/' final */
	*out = 0;
	return 0;
}

/* El dispositivo como lo entiende la PSP (PPSSPP MetaFileSystem.cpp):
   sin espacios delante, '\\' igual que '/', el nombre en minúsculas,
   "memstick:" es ms0:, "umd00:" es umd0:, "hostN:" es host0: y, si se
   arrancó desde un disco, host0: es el propio disco */
static void canonical_path(const char *in, char *out, u32 size){
	char *colon, *c;
	u32 n;
	if(strchr(in, ':')) while(*in == ' ') in++;
	snprintf(out, size, "%s", in);
	for(c = out; *c; c++) if(*c == '\\') *c = '/';
	colon = strchr(out, ':');
	if(!colon) return;
	for(c = out; c < colon; c++) *c = (char)tolower((unsigned char)*c);
	n = (u32)(colon - out);
	if(n == 8 && !memcmp(out, "memstick", 8)) memmove(out, "ms0", 3), memmove(out + 3, colon, strlen(colon) + 1);
	else if(n > 3 && !memcmp(out, "umd", 3) && strncmp(out, "umd1:", 5))
		memmove(out + 4, colon, strlen(colon) + 1), out[3] = '0';
	else if(n >= 4 && !memcmp(out, "host", 4))
		memmove(out + 5, colon, strlen(colon) + 1), out[4] = '0';
	if(!strncmp(out, "host0:", 6) && disc_is_open()) memcpy(out, "umd0:", 5), memmove(out + 4, out + 5, strlen(out + 5) + 1);
}

static int resolve_str(const char *raw, Resolved *r){
	char full[800], rel[800], in[512];
	const char *colon, *p;
	u32 n;
	memset(r, 0, sizeof(*r));
	canonical_path(raw, in, sizeof(in));
	colon = strchr(in, ':');
	if(colon) snprintf(full, sizeof(full), "%s", in);
	else {
		const char *slash = strchr(cwd, ':');
		if(in[0] == '/') snprintf(full, sizeof(full), "%.*s%s", (int)(slash - cwd + 1), cwd, in);
		else snprintf(full, sizeof(full), "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", in);
		colon = strchr(full, ':');
	}
	colon = strchr(full, ':');
	n = (u32)(colon - full);
	snprintf(rel, sizeof(rel), "%s", colon + 1);
	if(normalize_path(rel)) return -1;
	p = rel;
	if(is_disc_device(full, n)){
		if(disc_is_open()){
			r->disc = 1;
			r->whole_device = !*p && full[0] == 'u';
			n = (u32)snprintf(r->path, sizeof(r->path), "%s", p);
			return n < sizeof(r->path) ? 0 : -1;
		}
	} else if(!is_host_device(full, n)) return -1;
	/* Las partidas pueden vivir en otra carpeta (en el Wii, junto a WIISP) */
	if(savedata_dir[0] && !strncmp(p, SAVEDATA_DIR, sizeof(SAVEDATA_DIR) - 1) &&
	   (p[sizeof(SAVEDATA_DIR) - 1] == '/' || !p[sizeof(SAVEDATA_DIR) - 1])){
		p += sizeof(SAVEDATA_DIR) - 1;
		n = (u32)snprintf(r->path, sizeof(r->path), "%s%s", savedata_dir, p);
		return n < sizeof(r->path) ? 0 : -1;
	}
	/* La carpeta donde "vive" el ejecutable (argv[0]) es la del anfitrión */
	if(!strncmp(p, EXEC_DIR, sizeof(EXEC_DIR) - 1) && (p[sizeof(EXEC_DIR) - 1] == '/' || !p[sizeof(EXEC_DIR) - 1])){
		p += sizeof(EXEC_DIR) - 1;
		while(*p == '/') p++;
	}
	n = (u32)snprintf(r->path, sizeof(r->path), "%s/%s", host_dir, p);
	return n < sizeof(r->path) ? 0 : -1;   /* nunca una ruta recortada */
}

void io_set_savedata_dir(const char *dir){
	snprintf(savedata_dir, sizeof(savedata_dir), "%s", dir ? dir : "");
}

int io_host_path(const char *psp_path, char *out, u32 size){
	Resolved r;
	if(resolve_str(psp_path, &r) || r.disc) return -1;
	return (u32)snprintf(out, size, "%s", r.path) < size ? 0 : -1;
}

static int resolve(u32 addr, Resolved *r){
	char path[256];
	if(!addr || mem_read_cstr(addr, path, sizeof(path))) return -1;
	return resolve_str(path, r);
}

/* "sce_lbn0x5fa0_size0x1bb": un trozo del disco por posición */
static int parse_lbn(const char *p, u32 *lba, u32 *size){
	char *end;
	const char *q;
	if(strncmp(p, "sce_lbn", 7)) return 0;
	*lba = (u32)strtoul(p + 7, &end, 16);
	q = strstr(end, "_size");
	if(!q) return 0;
	*size = (u32)strtoul(q + 5, NULL, 16);
	return 1;
}

static IoFile *get_file(u32 fd){
	return fd < MAX_FILES && files[fd].kind != F_NONE ? &files[fd] : NULL;
}

static int alloc_fd(void){
	int fd;
	for(fd = 3; fd < MAX_FILES && files[fd].kind != F_NONE; fd++);
	return fd < MAX_FILES ? fd : -1;
}

/* --- Archivos ------------------------------------------------------------------ */

/* Lee hasta el final desde la posición actual (para cargar módulos) */
static u8 *load_rest(IoFile *o, u32 *len){
	u8 *buf;
	u64 n;
	if(o->kind == F_HOST){
		long pos = ftell(o->f), end;
		if(pos < 0 || fseek(o->f, 0, SEEK_END) || (end = ftell(o->f)) < pos) return NULL;
		fseek(o->f, pos, SEEK_SET);
		n = (u64)(end - pos);
		if(n > 64u * 1024 * 1024 || !(buf = malloc(n ? (size_t)n : 1))) return NULL;
		if(fread(buf, 1, (size_t)n, o->f) != n){ free(buf); return NULL; }
	} else if(o->kind == F_DISC){
		n = o->pos < o->size ? o->size - o->pos : 0;
		if(n > 64u * 1024 * 1024 || !(buf = malloc(n ? (size_t)n : 1))) return NULL;
		if(disc_read((u64)o->lba * DISC_SECTOR + o->pos, (u32)n, buf) != n){ free(buf); return NULL; }
		o->pos += n;
	} else return NULL;
	*len = (u32)n;
	return buf;
}

u8 *io_load_fd(u32 fd, u32 *len){
	IoFile *o = fd < MAX_FILES && files[fd].kind != F_NONE ? &files[fd] : NULL;
	return o ? load_rest(o, len) : NULL;
}

u8 *io_load_path(const char *path, u32 *len){
	Resolved r;
	IoFile o;
	u8 *buf;
	DiscEntry e;
	u32 lba, size;
	if(resolve_str(path, &r)) return NULL;
	memset(&o, 0, sizeof(o));
	if(r.disc){
		if(parse_lbn(r.path, &lba, &size)){ o.lba = lba; o.size = size; }
		else if(disc_lookup(r.path, &e) || e.is_dir) return NULL;
		else { o.lba = e.lba; o.size = e.size; }
		o.kind = F_DISC;
		return load_rest(&o, len);
	}
	o.f = fopen(r.path, "rb");
	if(!o.f) return NULL;
	o.kind = F_HOST;
	buf = load_rest(&o, len);
	fclose(o.f);
	return buf;
}

/* Abre en fd. Devuelve 0 o un código de error */
static u32 open_at(int fd, u32 path_addr, u32 flags){
	Resolved r;
	IoFile *o = &files[fd];
	memset(o, 0, sizeof(*o));
	if(resolve(path_addr, &r)) return SCE_ERROR_FILE_NOT_FOUND;
	if(r.disc){
		u32 lba, size;
		DiscEntry e;
		/* El UMD solo rechaza la escritura; CREAT, TRUNC o APPEND se ignoran */
		if(flags & PSP_O_WRONLY) return SCE_ERRNO_INVALID_FLAG;
		if(r.whole_device){
			o->kind = F_DISC_SECTORS;
			o->lba = 0;
			o->size = disc_sectors();
			o->by_sector = 1;
		} else if(parse_lbn(r.path, &lba, &size)){
			o->kind = F_DISC;
			o->lba = lba;
			o->size = size;
			o->by_sector = 1;
		} else {
			if(disc_lookup(r.path, &e) || e.is_dir) return SCE_ERROR_FILE_NOT_FOUND;
			o->kind = F_DISC;
			o->lba = e.lba;
			o->size = e.size;
		}
		return 0;
	}
	{
		const char *mode;
		if(flags & PSP_O_APPEND) mode = "ab";
		else if((flags & 3) == PSP_O_RDONLY) mode = "rb";
		else if(flags & PSP_O_TRUNC) mode = (flags & 1) ? "w+b" : "wb";
		else mode = "r+b";
		o->f = fopen(r.path, mode);
		if(!o->f && (flags & PSP_O_CREAT) && !strcmp(mode, "r+b")) o->f = fopen(r.path, "w+b");
		if(!o->f) return SCE_ERROR_FILE_NOT_FOUND;
		o->kind = F_HOST;
	}
	return 0;
}

/* Tiempos de la PSP (los de PPSSPP, Core/HLE/sceIo.cpp): el hilo espera
   lo que tarda el UMD o la Memory Stick, y otros hilos corren mientras. */
static int path_on_disc(u32 path_addr){
	Resolved r;
	return !resolve(path_addr, &r) && r.disc;
}

static u32 rw_delay_us(u32 size){
	u32 us = size / 100;
	return us < 100 ? 100 : us;
}

/* Un comodín ('?' o '*') en una ruta de la Memory Stick no es válido */
static int has_wildcard(u32 path_addr){
	char path[256];
	return !mem_read_cstr(path_addr, path, sizeof(path)) && (strchr(path, '?') || strchr(path, '*'));
}

static void sceIoOpen(void){
	int fd = alloc_fd(), disc = path_on_disc(ARG(0));
	u32 err;
	if(fd < 0){ RETURN(SCE_KERNEL_ERROR_MFILE); hle_delay_us(1000); return; }
	if(!disc && has_wildcard(ARG(0))){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); hle_delay_us(10000); return; }
	err = open_at(fd, ARG(0), ARG(1));
	if(err){
		memset(&files[fd], 0, sizeof(files[fd]));
		RETURN(err);
		hle_delay_us(err == SCE_ERROR_FILE_NOT_FOUND && disc ? 6000 : 10000);
		return;
	}
	RETURN(fd);
	/* Abrir por sectores (umd0:, sce_lbn) es inmediato */
	if(files[fd].by_sector) return;
	hle_delay_us(disc ? 4000 : 10000);
}

static u32 close_fd(u32 fd){
	IoFile *o = get_file(fd);
	if(!o) return SCE_KERNEL_ERROR_BADF;
	if(o->f) fclose(o->f);
	memset(o, 0, sizeof(*o));
	return 0;
}

static void sceIoClose(void){
	RETURN(close_fd(ARG(0)));
	hle_delay_us(100);
}

static s64 read_fd(u32 fd, u32 buf, u32 size){
	IoFile *o = get_file(fd);
	u8 *p;
	if(fd == 0) return 0;
	if(!o || o->kind == F_FAILED) return (s32)SCE_KERNEL_ERROR_BADF;
	if(o->kind == F_DISC_SECTORS){
		u32 n = size;
		if(o->pos >= o->size) return 0;
		if(n > o->size - o->pos) n = (u32)(o->size - o->pos);
		p = mem_ptr(buf, n * DISC_SECTOR);
		if(!p) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
		n = disc_read((o->lba + o->pos) * (u64)DISC_SECTOR, n * DISC_SECTOR, p) / DISC_SECTOR;
		o->pos += n;
		return n;
	}
	if(o->kind == F_DISC){
		u32 n = size;
		if(o->pos >= o->size) return 0;
		if(n > o->size - o->pos) n = (u32)(o->size - o->pos);
		p = mem_ptr(buf, n);
		if(!p && n) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
		n = disc_read((u64)o->lba * DISC_SECTOR + o->pos, n, p);
		o->pos += n;
		return n;
	}
	p = mem_ptr(buf, size);
	if(!p && size) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	{
		int old = prof_switch(PROF_ES);
		s64 n = (s64)fread(p, 1, size, o->f);
		prof_switch(old);
		return n;
	}
}

static void sceIoRead(void){
	s64 r = read_fd(ARG(0), ARG(1), ARG(2));
	RETURN((u32)r);
	if(r >= 0 && ARG(0) != 0) hle_delay_us(rw_delay_us(ARG(2)));
}

static s64 write_fd(u32 fd, u32 buf, u32 size){
	u8 *p = mem_ptr(buf, size);
	IoFile *o;
	if(!p && size) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if(fd == 1 || fd == 2){
		hle_output((const char *)p, size);
		return size;
	}
	o = get_file(fd);
	if(!o || o->kind == F_FAILED) return (s32)SCE_KERNEL_ERROR_BADF;
	if(o->kind != F_HOST) return (s32)SCE_ERRNO_READ_ONLY;
	{
		int old = prof_switch(PROF_ES);
		s64 n = (s64)fwrite(p, 1, size, o->f);
		prof_switch(old);
		return n;
	}
}

static void sceIoWrite(void){
	s64 r = write_fd(ARG(0), ARG(1), ARG(2));
	RETURN((u32)r);
	/* stdout/stderr no ceden la CPU (threads/scheduling/dispatch) */
	if(r >= 0 && ARG(0) > 2) hle_delay_us(rw_delay_us(ARG(2)));
}

static s64 do_seek(u32 fd, s64 offset, u32 whence){
	IoFile *o = get_file(fd);
	static const int whences[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	if(!o || o->kind == F_FAILED) return (s32)SCE_KERNEL_ERROR_BADF;
	if(whence > 2) return (s32)SCE_ERRNO_INVALID_ARGUMENT;
	if(o->kind != F_HOST){
		s64 base = whence == 0 ? 0 : whence == 1 ? (s64)o->pos : (s64)o->size, np = base + offset;
		if(np < 0) return (s32)SCE_ERRNO_INVALID_ARGUMENT;
		o->pos = (u64)np;
		return np;
	}
	if(fseek(o->f, (long)offset, whences[whence])) return (s32)SCE_ERRNO_INVALID_ARGUMENT;
	return ftell(o->f);
}

/* El offset de 64 bits va en el par de registros a2:a3 */
static void seek_timing(s64 r){
	if(r >= 0 || r == -1){
		kernel_eat_cycles(1400);
		kernel_reschedule();
	}
}

static void sceIoLseek(void){
	s64 r = do_seek(ARG(0), (s64)(((u64)ARG(3) << 32) | ARG(2)), ARG(4));
	RETURN64(r);
	seek_timing(r);
}

static void sceIoLseek32(void){
	s64 r = do_seek(ARG(0), (s32)ARG(1), ARG(2));
	RETURN((u32)r);
	seek_timing(r);
}

static void sceIoChdir(void){
	char path[200], tmp[512];
	Resolved r;
	if(mem_read_cstr(ARG(0), path, sizeof(path)) || resolve_str(path, &r)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	if(strchr(path, ':')) snprintf(tmp, sizeof(tmp), "%s", path);
	else if(path[0] == '/') snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(strchr(cwd, ':') - cwd + 1), cwd, path);
	else snprintf(tmp, sizeof(tmp), "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", path);
	if(strlen(tmp) >= sizeof(cwd)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	memcpy(cwd, tmp, strlen(tmp) + 1);
	RETURN(0);
}

/* SceIoStat (88 bytes): modo, atributos, tamaño (64 bits), fechas de
   creación, acceso y modificación, y 6 palabras privadas (en el UMD, la
   primera es el sector inicial) */
#define FIO_S_IFDIR  0x1000
#define FIO_S_IFREG  0x2000
#define FIO_SO_IFDIR 0x0010
#define FIO_SO_IFREG 0x0020

static void write_stat_common(u32 addr, int is_dir, u64 size, u64 mtime_us, u32 mode_bits){
	memset(mem_ptr(addr, 88), 0, 88);
	mem_write32(addr, (is_dir ? FIO_S_IFDIR : FIO_S_IFREG) | mode_bits);
	mem_write32(addr + 4, is_dir ? FIO_SO_IFDIR : FIO_SO_IFREG);
	mem_write32(addr + 8, (u32)size);
	mem_write32(addr + 12, (u32)(size >> 32));
	hle_write_datetime(addr + 16, mtime_us);
	hle_write_datetime(addr + 32, mtime_us);
	hle_write_datetime(addr + 48, mtime_us);
}

static void write_stat(u32 addr, const struct stat *st){
	write_stat_common(addr, S_ISDIR(st->st_mode), (u64)st->st_size, (u64)st->st_mtime * 1000000ull, 0x1FF);
}

static void write_disc_stat(u32 addr, const DiscEntry *e){
	write_stat_common(addr, e->is_dir, e->size, 0, 0x16D);   /* r-xr-xr-x */
	mem_write32(addr + 64, e->lba);
}

static void getstat(void);
static void sceIoGetstat(void){
	getstat();
	hle_delay_us(1000);
}

static void getstat(void){
	Resolved r;
	if(resolve(ARG(0), &r)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	if(!mem_valid(ARG(1), 88)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	if(r.disc){
		DiscEntry e;
		u32 lba, size;
		if(parse_lbn(r.path, &lba, &size)){ e.lba = lba; e.size = size; e.is_dir = 0; }
		else if(disc_lookup(r.path, &e)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
		write_disc_stat(ARG(1), &e);
	} else {
		struct stat st;
		if(stat(r.path, &st)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
		write_stat(ARG(1), &st);
	}
	RETURN(0);
}

/* --- Directorios --------------------------------------------------------------- */

static void sceIoDopen(void){
	Resolved r;
	int i;
	if(resolve(ARG(0), &r)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	for(i = 0; i < MAX_DIRS && dirs[i].used; i++);
	if(i == MAX_DIRS){ RETURN(SCE_KERNEL_ERROR_MFILE); return; }
	memset(&dirs[i], 0, sizeof(dirs[i]));
	if(r.disc){
		if(disc_lookup(r.path, &dirs[i].disc) || !dirs[i].disc.is_dir){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
		dirs[i].is_disc = 1;
	} else {
		DIR *d = opendir(r.path);
		if(!d){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
		dirs[i].dir = d;
		snprintf(dirs[i].path, sizeof(dirs[i].path), "%s", r.path);
	}
	dirs[i].used = 1;
	RETURN(DIR_FD_BASE + i);
}

static int dir_index(u32 fd){
	u32 i = fd - DIR_FD_BASE;
	return (fd >= DIR_FD_BASE && i < MAX_DIRS && dirs[i].used) ? (int)i : -1;
}

static void put_dirent_name(u32 out, const char *name){
	size_t n = strlen(name);
	if(n > 255) n = 255;
	memcpy(mem_ptr(out + 88, 256), name, n);
}

/* SceIoDirent (352 bytes): SceIoStat, char d_name[256], d_private, dummy.
   Devuelve 1 si leyó una entrada y 0 al llegar al final. En el UMD no hay
   "." ni ".." (como en PPSSPP): los juegos que recorren el disco entero,
   como GTA LCS, entran en cada directorio que ven y no acabarían nunca. */
static void sceIoDread(void){
	int i = dir_index(ARG(0));
	u32 out = ARG(1), d_private;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	if(!mem_valid(out, 352)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	d_private = mem_read32(out + 344);
	if(dirs[i].is_disc){
		DiscEntry e;
		if(!disc_dir_entry(&dirs[i].disc, dirs[i].disc_index, &e)){ RETURN(0); return; }
		dirs[i].disc_index++;
		memset(mem_ptr(out, 344), 0, 344);
		write_disc_stat(out, &e);
		put_dirent_name(out, e.name);
	} else {
		struct dirent *e = readdir(dirs[i].dir);
		char full[1024];
		struct stat st;
		if(!e){ RETURN(0); return; }
		memset(mem_ptr(out, 344), 0, 344);
		snprintf(full, sizeof(full), "%s/%s", dirs[i].path, e->d_name);
		if(!stat(full, &st)) write_stat(out, &st);
		put_dirent_name(out, e->d_name);
	}
	mem_write32(out + 344, d_private);
	RETURN(1);
	/* Solo la primera entrada tarda */
	if(dirs[i].entries_read++ == 0) hle_delay_us(1000);
}

static void sceIoDclose(void){
	int i = dir_index(ARG(0));
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	if(dirs[i].dir) closedir(dirs[i].dir);
	memset(&dirs[i], 0, sizeof(dirs[i]));
	RETURN(0);
}

static void sceIoRemove(void){
	Resolved r;
	if(resolve(ARG(0), &r) || r.disc || remove(r.path)) RETURN(SCE_ERROR_FILE_NOT_FOUND);
	else RETURN(0);
	hle_delay_us(100);
}

static void sceIoRename(void){
	Resolved from, to;
	if(resolve(ARG(0), &from) || resolve(ARG(1), &to) || from.disc || to.disc ||
	   rename(from.path, to.path)){ RETURN(SCE_ERROR_FILE_NOT_FOUND); return; }
	RETURN(0);
}

static void sceIoMkdir(void){
	Resolved r;
	if(resolve(ARG(0), &r) || r.disc || mkdir(r.path, 0777)) RETURN(0x80010011u /* EEXIST */);
	else RETURN(0);
	hle_delay_us(1000);
}

static void sceIoRmdir(void){
	Resolved r;
	if(resolve(ARG(0), &r) || r.disc || rmdir(r.path)) RETURN(SCE_ERROR_FILE_NOT_FOUND);
	else RETURN(0);
	hle_delay_us(1000);
}

/* --- ioctl y devctl ------------------------------------------------------------ */

/* Comandos de UMD (los de PPSSPP, Core/HLE/sceIo.cpp) */
static void sceIoIoctl(void){
	u32 fd = ARG(0), cmd = ARG(1), in = ARG(2), in_len = ARG(3), out = ARG(4), out_len = ARG(5);
	IoFile *o = get_file(fd);
	if(!o || o->kind == F_FAILED){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	switch(cmd){
	case 0x01020003:   /* tamaño de sector */
		if(!mem_valid(out, 4) || out_len < 4){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		mem_write32(out, DISC_SECTOR);
		break;
	case 0x01020004:   /* posición */
	case 0x01d20001:   /* posición en sectores (umd0:) */
		if(!mem_valid(out, 4) || out_len < 4){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		mem_write32(out, (u32)o->pos);
		break;
	case 0x01020006:   /* sector inicial */
		if(!mem_valid(out, 4) || out_len < 4){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		mem_write32(out, o->lba);
		break;
	case 0x01020007:   /* tamaño en bytes (64 bits) */
		if(!mem_valid(out, 8) || (out & 3)){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		mem_write32(out, (u32)o->size);
		mem_write32(out + 4, (u32)(o->size >> 32));
		break;
	case 0x01010005:   /* seek: { u64 offset, u32, u32 whence } */
	case 0x01f100a6: { /* seek por sectores (umd0:) */
		s64 off, np;
		u32 whence;
		if(!mem_valid(in, 16) || in_len < 4){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		off = (s64)(((u64)mem_read32(in + 4) << 32) | mem_read32(in));
		whence = mem_read32(in + 12);
		np = (whence == 0 ? 0 : whence == 1 ? (s64)o->pos : (s64)o->size) + off;
		if(whence > 2 || np < 0 || (u64)np > o->size){ RETURN(cmd == 0x01010005 ? SCE_ERRNO_IO_ERROR : 0x80010084u); return; }
		o->pos = (u64)np;
		break;
	}
	case 0x01030008:   /* leer bytes */
	case 0x01f30003: { /* leer sectores (umd0:) */
		u32 size;
		if(!mem_valid(in, 4) || in_len < 4){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		size = mem_read32(in);
		if(size > out_len){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
		RETURN((u32)read_fd(fd, out, size));
		return;
	}
	default: {
		static u32 logged[16];
		int i;
		for(i = 0; i < 16 && logged[i] && logged[i] != cmd; i++);
		if(i < 16 && !logged[i]){ logged[i] = cmd; hle_log("[IO] sceIoIoctl: comando %08X sin implementar\n", cmd); }
		break;
	}
	}
	RETURN(0);
}

/* Registra un callback de inserción y lo avisa: la tarjeta está puesta */
static u32 ms_register(int which, u32 arg, u32 arg_len){
	u32 cb, i;
	if(!mem_valid(arg, 4) || (arg & 3) || arg_len < 4) return SCE_MEMSTICK_BAD_PARAMS;
	cb = mem_read32(arg);
	if(!kernel_is_callback(cb)) return SCE_ERRNO_INVALID_ARGUMENT;
	for(i = 0; i < MAX_MS_CALLBACKS && ms_callbacks[which][i]; i++);
	if(i == MAX_MS_CALLBACKS) return SCE_ERRNO_INVALID_ARGUMENT;
	ms_callbacks[which][i] = cb;
	kernel_notify_callback(cb, 1);   /* 1 = insertada (y asignada, en fatms0:) */
	return 0;
}

static u32 ms_unregister(int which, u32 arg, u32 arg_len){
	u32 cb, i;
	if(!mem_valid(arg, 4) || arg_len < 4) return SCE_MEMSTICK_BAD_PARAMS;
	cb = mem_read32(arg);
	for(i = 0; i < MAX_MS_CALLBACKS; i++)
		if(cb && ms_callbacks[which][i] == cb){
			for(; i + 1 < MAX_MS_CALLBACKS; i++) ms_callbacks[which][i] = ms_callbacks[which][i + 1];
			ms_callbacks[which][i] = 0;
			return 0;
		}
	return SCE_ERRNO_INVALID_ARGUMENT;
}

/* Capacidad: el struct DeviceSize al que apunta *arg, con ~1 GB libre */
static u32 ms_capacity(u32 arg, u32 arg_len){
	u32 p, clusters = (u32)((1024ull * 1024 * 1024 * 95 / 100) / (0x200 * 64));
	if(!mem_valid(arg, 4) || arg_len < 4) return SCE_MEMSTICK_BAD_PARAMS;
	p = mem_read32(arg);
	if(mem_valid(p, 20)){
		mem_write32(p, clusters);        /* maxClusters */
		mem_write32(p + 4, clusters);    /* freeClusters */
		mem_write32(p + 8, clusters);    /* maxSectors */
		mem_write32(p + 12, 0x200);      /* sectorSize */
		mem_write32(p + 16, 64);         /* sectorCount */
	}
	return 0;
}

/* Memory Stick (como PPSSPP sceIo.cpp): siempre insertada y montada.
   Devuelve 1 si el comando era de la tarjeta */
static int ms_devctl(const char *dev, u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len, u32 *ret){
	int fat = !strcmp(dev, "fatms0:");
	if(!fat && strcmp(dev, "mscmhc0:") && strcmp(dev, "ms0:") && strcmp(dev, "memstick:")) return 0;
	*ret = 0;
	switch(cmd){
	case 0x02025801:   /* estado del controlador: 4 = tarjeta insertada */
		if(fat) return 0;
		if(!mem_valid(out, 4) || out_len < 4){ *ret = SCE_MEMSTICK_BAD_PARAMS; break; }
		mem_write32(out, 4);
		break;
	case 0x02025806:   /* ¿insertada? 1 = sí */
		if(fat) return 0;
		if(!mem_valid(out, 4) || (out & 3) || out_len < 4){ *ret = SCE_MEMSTICK_BAD_PARAMS; break; }
		mem_write32(out, 1);
		break;
	case 0x02015804: if(fat) return 0; *ret = out ? SCE_MEMSTICK_BAD_PARAMS : ms_register(0, in, in_len); break;
	case 0x02015805: if(fat) return 0; *ret = ms_unregister(0, in, in_len); break;
	case 0x02415821: if(!fat) return 0; *ret = ms_register(1, in, in_len); break;
	case 0x02415822: if(!fat) return 0; *ret = ms_unregister(1, in, in_len); break;
	case 0x0240D81E: if(!fat) return 0; break;   /* invalidar la caché de la FAT */
	case 0x02415823: if(!fat) return 0; break;   /* asignar la FAT */
	case 0x02425823:   /* ¿FAT asignada? 1 = sí */
		if(!fat) return 0;
		if(!mem_valid(out, 4)){ *ret = SCE_ERRNO_INVALID_ARGUMENT; break; }
		mem_write32(out, 1);
		hle_delay_us(23500 / 222);   /* 23500 ciclos de la PSP */
		break;
	case 0x02425824:   /* ¿protegida contra escritura? 0 = no */
		if(!mem_valid(out, 4) || out_len != 4){ *ret = 0xFFFFFFFFu; break; }
		mem_write32(out, 0);
		break;
	case 0x02425818: *ret = ms_capacity(in, in_len); break;
	case 0x02425856: if(fat) return 0; break;
	default: return 0;
	}
	return 1;
}

static void sceIoDevctl(void){
	char dev[32];
	u32 cmd = ARG(1), in = ARG(2), in_len = ARG(3), out = ARG(4), out_len = ARG(5), ret;
	mem_read_cstr(ARG(0), dev, sizeof(dev));
	/* Comandos del UMD: valen con cualquier nombre de dispositivo (PPSSPP) */
	switch(cmd){
	case 0x01E18030:   /* ¿coincide la región del disco? 1 = sí */
		RETURN(in_len >= 16 ? 1 : 0xFFFFFFFFu);
		return;
	case 0x01F20001:   /* tipo de disco y estado */
		if(!mem_valid(out, 8) || (out & 3) || out_len < 8){ RETURN(SCE_MEMSTICK_BAD_PARAMS); return; }
		mem_write32(out, 0xFFFFFFFFu);
		mem_write32(out + 4, 0x10);   /* disco de juego */
		RETURN(0);
		return;
	case 0x01F20002:   /* último sector leído */
	case 0x01F20003:   /* último sector del disco */
		if(!mem_valid(out, 4) || (out & 3) || out_len < 4){ RETURN(SCE_MEMSTICK_BAD_PARAMS); return; }
		mem_write32(out, cmd == 0x01F20003 ? disc_sectors() - 1 : 0x10);
		RETURN(0);
		return;
	case 0x01F300A5:   /* precarga a la caché y estado: lectura n.º 1 */
		if(!mem_valid(in, 4) || in_len < 4 || !mem_valid(out, 4) || (out & 3)){ RETURN(SCE_MEMSTICK_BAD_PARAMS); return; }
		mem_write32(out, 1);
		RETURN(0);
		return;
	case 0x01F100A3:   /* búsqueda en el disco */
		if(!mem_valid(in, 4) || in_len < 4){ RETURN(SCE_MEMSTICK_BAD_PARAMS); return; }
		RETURN(0);
		hle_delay_us(100);
		return;
	case 0x01F100A4: case 0x01F300A7: case 0x01F300A8: case 0x01F300A9:
		/* caché del UMD: el hilo de la caché siempre ha terminado */
		RETURN(mem_valid(in, 4) && in_len >= 4 ? 0 : SCE_MEMSTICK_BAD_PARAMS);
		return;
	case 0x01F100A6: case 0x01F100A8: case 0x01F100A9:
		RETURN(0);
		return;
	}
	if(ms_devctl(dev, cmd, in, in_len, out, out_len, &ret)){
		RETURN(ret);
		return;
	}
	if(strcmp(dev, "emulator:") && strcmp(dev, "kemulator:")){
		hle_log("[E/S] sceIoDevctl(\"%s\", %08X) sin implementar\n", dev, cmd);
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
	case EMULATOR_DEVCTL_EMIT_SCREENSHOT:
		hle_screenshot();
		break;
	default:
		break; /* mandos simulados y capturas: aún no */
	}
	RETURN(0);
}

/* --- E/S asíncrona ------------------------------------------------------------- */

/* Resultado de sceIoCloseAsync: el descriptor se libera al recogerlo */
#define CLOSE_PENDING 0x7FFFFFFF00000000ll

static void set_async(u32 fd, s64 result){
	files[fd].async_pending = 1;
	files[fd].async_result = result;
}

static void sceIoOpenAsync(void){
	int fd = alloc_fd();
	u32 err;
	if(fd < 0){ RETURN(SCE_KERNEL_ERROR_MFILE); return; }
	err = open_at(fd, ARG(0), ARG(1));
	if(err){
		memset(&files[fd], 0, sizeof(files[fd]));
		files[fd].kind = F_FAILED;
		files[fd].free_on_collect = 1;
	}
	set_async((u32)fd, err ? (s32)err : fd);
	RETURN(fd);
}

/* Las operaciones necesitan un archivo sin otra operación pendiente */
static IoFile *async_target(u32 fd){
	IoFile *o = get_file(fd);
	if(!o){ RETURN(SCE_KERNEL_ERROR_BADF); return NULL; }
	if(o->async_pending){ RETURN(SCE_KERNEL_ERROR_ASYNC_BUSY); return NULL; }
	return o;
}

static void sceIoCloseAsync(void){
	u32 fd = ARG(0);
	IoFile *o = get_file(fd);
	if(!o){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	if(o->async_pending){ RETURN(SCE_KERNEL_ERROR_ASYNC_BUSY); return; }
	/* Se cierra al recoger el resultado */
	if(o->f){ fclose(o->f); o->f = NULL; }
	o->kind = F_FAILED;
	set_async(fd, CLOSE_PENDING);
	RETURN(0);
}

static void sceIoReadAsync(void){
	u32 fd = ARG(0);
	if(!async_target(fd)) return;
	set_async(fd, read_fd(fd, ARG(1), ARG(2)));
	RETURN(0);
}

static void sceIoWriteAsync(void){
	u32 fd = ARG(0);
	if(!async_target(fd)) return;
	set_async(fd, write_fd(fd, ARG(1), ARG(2)));
	RETURN(0);
}

static void sceIoLseekAsync(void){
	u32 fd = ARG(0);
	if(!async_target(fd)) return;
	set_async(fd, do_seek(fd, (s64)(((u64)ARG(3) << 32) | ARG(2)), ARG(4)));
	RETURN(0);
}

static void sceIoLseek32Async(void){
	u32 fd = ARG(0);
	if(!async_target(fd)) return;
	set_async(fd, do_seek(fd, (s32)ARG(1), ARG(2)));
	RETURN(0);
}

/* Recoge el resultado de la operación pendiente en *res (s64) */
static void collect_async(u32 fd, u32 res){
	IoFile *o = fd < MAX_FILES ? &files[fd] : NULL;
	s64 r;
	if(!o || o->kind == F_NONE){ RETURN(SCE_KERNEL_ERROR_BADF); return; }
	if(!o->async_pending){ RETURN(SCE_KERNEL_ERROR_NOASYNC); return; }
	r = o->async_result;
	o->async_pending = 0;
	if(r == CLOSE_PENDING || o->free_on_collect){
		memset(o, 0, sizeof(*o));
		if(r == CLOSE_PENDING) r = 0;
	}
	if(mem_valid(res, 8)){
		mem_write32(res, (u32)r);
		mem_write32(res + 4, (u32)((u64)r >> 32));
	}
	RETURN(0);
}

static void sceIoWaitAsync(void){ collect_async(ARG(0), ARG(1)); }
static void sceIoWaitAsyncPlain(void){ collect_async(ARG(0), ARG(1)); }
static void sceIoWaitAsyncCB(void){ kernel_cb_wait(sceIoWaitAsyncPlain); }
static void sceIoPollAsync(void){ collect_async(ARG(0), ARG(1)); }
static void sceIoGetAsyncStat(void){ collect_async(ARG(0), ARG(2)); }   /* (fd, poll, res) */

static void sceIoChangeAsyncPriority(void){ RETURN(get_file(ARG(0)) || (s32)ARG(0) == -1 ? 0 : SCE_KERNEL_ERROR_BADF); }
static void sceIoSetAsyncCallback(void){ RETURN(get_file(ARG(0)) ? 0 : SCE_KERNEL_ERROR_BADF); }
/* La PSP no sabe cancelar en el UMD ni en la Memory Stick (PPSSPP) */
static void sceIoCancel(void){ RETURN(get_file(ARG(0)) ? SCE_KERNEL_ERROR_UNSUP : SCE_KERNEL_ERROR_BADF); }

/* --- UMD ----------------------------------------------------------------------- */

#define PSP_UMD_NOT_PRESENT 0x01
#define PSP_UMD_PRESENT     0x02
#define PSP_UMD_READY       0x10
#define PSP_UMD_READABLE    0x20

/* Como en PPSSPP (sceUmd.cpp): el disco está siempre puesto y listo, y el
   UMD empieza activado. sceUmdActivate tarda 4 ms en dejarlo legible. */
#define UMD_STAT_ALLOW_WAIT 0x3B   /* NOT_PRESENT|PRESENT|NOT_READY|READY|READABLE */
#define UMD_ACTIVATE_US     4000
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT 0x80020064u
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT    0x800201A7u
#define SCE_KERNEL_ERROR_WAIT_CANCEL     0x800201A9u

static u32 umd_state(void){
	u32 s = PSP_UMD_PRESENT | PSP_UMD_READY;
	if(umd_activated) s |= PSP_UMD_READABLE;
	return s;
}

/* Tras un cambio de estado despiertan los que esperaban alguno de sus bits */
static void umd_stat_change(u64 activated){
	umd_activated = activated != 0;
	kernel_wake_object_mask(KWAIT_UMD, umd_state(), 0);
}

/* Tras los callbacks de una espera CB: sigue esperando si nada cambió */
static u32 umd_recheck(u32 stat, int *done){
	*done = (stat & umd_state()) != 0;
	return 0;
}

static void sceUmdCheckMedium(void){ RETURN(1); }
static void sceUmdActivate(void){
	char name[8] = "";
	u32 p = ARG(1);
	if(ARG(0) < 1 || ARG(0) > 2){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
	if(p && mem_valid(p, 1)) mem_read_cstr(p, name, sizeof(name));
	if(strcmp(name, "disc0:") || (p & 0x80000000u)){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
	/* Avisa al callback del UMD, como la PSP: listo solo si el juego
	   declara versión de SDK */
	if(umd_callback)
		kernel_notify_callback(umd_callback, (s32)(PSP_UMD_PRESENT | PSP_UMD_READABLE |
		                                           (kernel_sdk_version() ? PSP_UMD_READY : 0)));
	/* El disco "arranca": no es legible en el acto */
	kernel_unschedule_event(umd_stat_change, 1);
	kernel_unschedule_event(umd_stat_change, 0);
	kernel_schedule_event(cpu_cycles + (u64)UMD_ACTIVATE_US * CYCLES_PER_US, umd_stat_change, 1);
	RETURN(0);
}
static void sceUmdDeactivate(void){
	u32 mode = ARG(0), p = ARG(1);
	if(mode > 18 || (mode == 2 && !p) || (p & 0x80000000u)){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
	if(umd_callback) kernel_notify_callback(umd_callback, (s32)(PSP_UMD_PRESENT | PSP_UMD_READY));
	kernel_unschedule_event(umd_stat_change, 1);
	umd_stat_change(0);
	RETURN(0);
}

static void sceUmdRegisterUMDCallBack(void){
	if(!kernel_is_callback(ARG(0))){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
	umd_callback = ARG(0);
	RETURN(0);
}

static void sceUmdUnRegisterUMDCallBack(void){
	u32 cb = ARG(0);
	if(cb != umd_callback){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
	umd_callback = 0;
	RETURN(kernel_sdk_version() > 0x3000000 ? 0 : cb);
}
static void sceUmdGetDriveStat(void){ RETURN(umd_state()); }

/* Las tres esperas del estado del disco. timeout = 0: sin límite (el
   firmware espera un event flag de mediaman.prx). Devuelve 1 si el hilo
   se quedó esperando. */
static int umd_wait(u32 stat, u32 timeout, int with_timer, int cb){
	if(!(stat & UMD_STAT_ALLOW_WAIT)){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return 0; }
	if(!kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return 0; }
	if(kernel_in_interrupt()){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); return 0; }
	kernel_eat_cycles(520);
	RETURN(0);
	if(stat & umd_state()){
		if(with_timer) kernel_reschedule();
		return 0;
	}
	/* Medido en una PSP: el mínimo es 240 us (25 us para 1 sin CB) */
	if(with_timer && timeout){
		if(timeout <= 1 && !cb) timeout = 25;
		else if(timeout <= 209) timeout = 240;
		kernel_wait_object_timeout(KWAIT_UMD, stat, timeout);
	} else
		kernel_wait_object(KWAIT_UMD, stat);
	return 1;
}
static void sceUmdWaitDriveStat(void){ umd_wait(ARG(0), 0, 0, 0); }
static void sceUmdWaitDriveStatWithTimer(void){ umd_wait(ARG(0), ARG(1), 1, 0); }
static void umd_wait_cb(void){ umd_wait(ARG(0), ARG(1), 1, 1); }
static void sceUmdWaitDriveStatCB(void){ kernel_cb_wait(umd_wait_cb); }
static void sceUmdCancelWaitDriveStat(void){
	kernel_wake_object_mask(KWAIT_UMD, ~0u, SCE_KERNEL_ERROR_WAIT_CANCEL);
	RETURN(0);
}
static void sceUmdGetDiscInfo(void){
	u32 p = ARG(0);
	if(!mem_valid(p, 8) || mem_read32(p) != 8){ RETURN(SCE_ERRNO_INVALID_ARGUMENT); return; }
	mem_write32(p + 4, 0x10);   /* PSP_UMD_TYPE_GAME */
	RETURN(0);
}
static void umd_zero(void){ RETURN(0); }

static const HleFunction io_user[] = {
	{ "sceIoOpen", sceIoOpen },
	{ "sceIoClose", sceIoClose },
	{ "sceIoRead", sceIoRead },
	{ "sceIoWrite", sceIoWrite },
	{ "sceIoLseek", sceIoLseek },
	{ "sceIoLseek32", sceIoLseek32 },
	{ "sceIoChdir", sceIoChdir },
	{ "sceIoIoctl", sceIoIoctl },
	{ "sceIoDevctl", sceIoDevctl },
	{ "sceIoGetstat", sceIoGetstat },
	{ "sceIoDopen", sceIoDopen },
	{ "sceIoDread", sceIoDread },
	{ "sceIoDclose", sceIoDclose },
	{ "sceIoRemove", sceIoRemove },
	{ "sceIoRename", sceIoRename },
	{ "sceIoMkdir", sceIoMkdir },
	{ "sceIoRmdir", sceIoRmdir },
	{ "sceIoOpenAsync", sceIoOpenAsync },
	{ "sceIoCloseAsync", sceIoCloseAsync },
	{ "sceIoReadAsync", sceIoReadAsync },
	{ "sceIoWriteAsync", sceIoWriteAsync },
	{ "sceIoLseekAsync", sceIoLseekAsync },
	{ "sceIoLseek32Async", sceIoLseek32Async },
	{ "sceIoWaitAsync", sceIoWaitAsync },
	{ "sceIoWaitAsyncCB", sceIoWaitAsyncCB },
	{ "sceIoPollAsync", sceIoPollAsync },
	{ "sceIoGetAsyncStat", sceIoGetAsyncStat },
	{ "sceIoChangeAsyncPriority", sceIoChangeAsyncPriority },
	{ "sceIoSetAsyncCallback", sceIoSetAsyncCallback },
	{ "sceIoCancel", sceIoCancel },
};

static const HleFunction umd_user[] = {
	{ "sceUmdCheckMedium", sceUmdCheckMedium },
	{ "sceUmdActivate", sceUmdActivate },
	{ "sceUmdDeactivate", sceUmdDeactivate },
	{ "sceUmdGetDriveStat", sceUmdGetDriveStat },
	{ "sceUmdWaitDriveStat", sceUmdWaitDriveStat },
	{ "sceUmdWaitDriveStatWithTimer", sceUmdWaitDriveStatWithTimer },
	{ "sceUmdWaitDriveStatCB", sceUmdWaitDriveStatCB },
	{ "sceUmdCancelWaitDriveStat", sceUmdCancelWaitDriveStat },
	{ "sceUmdGetDiscInfo", sceUmdGetDiscInfo },
	{ "sceUmdRegisterUMDCallBack", sceUmdRegisterUMDCallBack },
	{ "sceUmdUnRegisterUMDCallBack", sceUmdUnRegisterUMDCallBack },
	{ "sceUmdGetErrorStat", umd_zero },
	{ "sceUmdReplaceProhibit", umd_zero },
	{ "sceUmdReplacePermit", umd_zero },
};

const HleLibrary hle_io_libs[] = {
	HLE_LIBRARY("IoFileMgrForUser", io_user),
	HLE_LIBRARY("sceUmdUser", umd_user),
};
const u32 hle_io_libs_count = sizeof(hle_io_libs) / sizeof(hle_io_libs[0]);
