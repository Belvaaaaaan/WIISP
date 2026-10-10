/**
 * WIISP - utility.c
 * HLE de sceUtility: módulos de utilidad, parámetros del sistema y los
 * diálogos del sistema (mensajes y partidas guardadas).
 *
 * Semántica según PPSSPP (Core/HLE/sceUtility.cpp y Core/Dialog/,
 * (c) 2012- PPSSPP Project, GPLv2+): tabla de módulos con sus tamaños y
 * dependencias, la máquina de estados común de los diálogos (NONE ->
 * INITIALIZE -> RUNNING -> FINISHED -> SHUTDOWN -> NONE, con sus retardos)
 * y los modos de sceUtilitySavedata con sus códigos de error.
 *
 * No hay interfaz: los diálogos se contestan solos. Un mensaje con botones
 * se acepta con la opción por defecto (lo que haría pulsar X enseguida) y
 * su texto queda en el registro. Las listas de partidas eligen según el
 * "focus" que pide el juego; borrar desde una lista se cancela, para no
 * borrar nada sin que el usuario lo pida. Las partidas se guardan sin
 * cifrar en ms0:/PSP/SAVEDATA/<juego><partida>/ (en el Wii, en la SD), con
 * su PARAM.SFO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)   /* el CLI para Windows */
#endif
#include "hle/hle.h"
#include "core/memory.h"
#include "loader/sfo.h"

#define ERR_INVALID_STATUS      0x80110001u
#define ERR_INVALID_ADDRESS     0x80110002u
#define ERR_INVALID_PARAM_SIZE  0x80110004u
#define ERR_WRONG_TYPE          0x80110005u
#define ERR_STRING_TOO_LONG     0x80110102u
#define ERR_INVALID_SYSPARAM_ID 0x80110103u
#define ERR_INVALID_ADHOC_CHANNEL 0x80110104u
#define ERR_MSG_BADOPTION       0x80110501u
#define ERR_MSG_ERRORCODEINVALID 0x80110502u
#define ERR_MODULE_BAD_ID       0x80111101u
#define ERR_MODULE_ALREADY_LOADED 0x80111102u
#define ERR_MODULE_NOT_LOADED   0x80111103u
#define ERR_AV_MODULE_BAD_ID    0x80110F01u
#define ERR_AV_MODULE_ALREADY_LOADED 0x80110F02u
#define ERR_AV_MODULE_NOT_LOADED 0x80110F03u
#define SCE_KERNEL_ERROR_LIBRARY_NOTFOUND 0x8002013Cu
#define SCE_KERNEL_ERROR_BAD_ARGUMENT 0x80000004u

#define SD_ERR_LOAD_DATA_BROKEN    0x80110306u
#define SD_ERR_LOAD_NO_DATA        0x80110307u
#define SD_ERR_LOAD_PARAM          0x80110308u
#define SD_ERR_LOAD_FILE_NOT_FOUND 0x80110309u
#define SD_ERR_RW_MEMSTICK_FULL    0x80110323u
#define SD_ERR_RW_DATA_BROKEN      0x80110326u
#define SD_ERR_RW_NO_DATA          0x80110327u
#define SD_ERR_RW_BAD_PARAMS       0x80110328u
#define SD_ERR_RW_FILE_NOT_FOUND   0x80110329u
#define SD_ERR_SAVE_MS_NOSPACE     0x80110383u
#define SD_ERR_SAVE_ACCESS_ERROR   0x80110385u
#define SD_ERR_SAVE_PARAM          0x80110388u
#define SD_ERR_DELETE_NO_DATA      0x80110347u
#define SD_ERR_SIZES_NO_DATA       0x801103C7u

#define RESULT_CANCEL 1

/* --- Módulos de utilidad ------------------------------------------------------------ */

typedef struct { u16 id; u32 size; u16 deps[5]; } ModInfo;

static const ModInfo mod_info[] = {
	{ 0x100, 0x00014000, { 0 } }, { 0x101, 0x00020000, { 0 } }, { 0x102, 0x00058000, { 0 } },
	{ 0x103, 0x00006000, { 0 } }, { 0x104, 0x00002000, { 0 } },
	{ 0x105, 0x00028000, { 0x102, 0x103, 0x104, 0 } }, { 0x106, 0x00044000, { 0x102, 0 } },
	{ 0x107, 0x00010000, { 0 } }, { 0x108, 0x00008000, { 0x100, 0x102, 0x103, 0x104, 0x105 } },
	{ 0x200, 0, { 0 } }, { 0x201, 0, { 0 } }, { 0x202, 0, { 0 } }, { 0x203, 0, { 0 } }, { 0x2FF, 0, { 0 } },
	{ 0x300, 0, { 0 } }, { 0x301, 0, { 0 } },
	{ 0x302, 0x00008000, { 0x300, 0 } }, { 0x303, 0x0000C000, { 0x300, 0 } },
	{ 0x304, 0x00004000, { 0 } }, { 0x305, 0x0000A300, { 0 } }, { 0x306, 0x00004000, { 0 } },
	{ 0x307, 0, { 0 } }, { 0x308, 0x0003C000, { 0x300, 0 } },
	{ 0x3FE, 0, { 0 } }, { 0x3FF, 0, { 0 } },
	{ 0x400, 0x0000C000, { 0 } }, { 0x401, 0x00018000, { 0 } }, { 0x402, 0x00048000, { 0 } },
	{ 0x403, 0x0000E000, { 0 } }, { 0x500, 0, { 0 } }, { 0x600, 0, { 0 } }, { 0x601, 0, { 0 } },
};
#define NUM_MODS (sizeof(mod_info) / sizeof(mod_info[0]))

static u8 mod_loaded[NUM_MODS];
static u32 mod_addr[NUM_MODS];

static int mod_index(u32 id){
	u32 i;
	for(i = 0; i < NUM_MODS; i++) if(mod_info[i].id == id) return (int)i;
	return -1;
}

static u32 load_module(u32 id, int av){
	int i = mod_index(id), k;
	if(i < 0) return av ? ERR_AV_MODULE_BAD_ID : ERR_MODULE_BAD_ID;
	if(mod_loaded[i]) return av ? ERR_AV_MODULE_ALREADY_LOADED : ERR_MODULE_ALREADY_LOADED;
	for(k = 0; k < 5 && mod_info[i].deps[k]; k++){
		int d = mod_index(mod_info[i].deps[k]);
		if(d < 0 || !mod_loaded[d]) return SCE_KERNEL_ERROR_LIBRARY_NOTFOUND;
	}
	/* Ocupa su memoria como el módulo de verdad (si no cabe, sin ella) */
	mod_addr[i] = mod_info[i].size ? kernel_alloc(mod_info[i].size, 0, "UtilityModule") : 0;
	mod_loaded[i] = 1;
	if(id == 0x302) atrac_notify_load(mod_addr[i]);
	return 0;
}

static u32 unload_module(u32 id, int av){
	int i = mod_index(id);
	if(i < 0) return av ? ERR_AV_MODULE_BAD_ID : ERR_MODULE_BAD_ID;
	if(!mod_loaded[i]) return av ? ERR_AV_MODULE_NOT_LOADED : ERR_MODULE_NOT_LOADED;
	if(mod_addr[i]) kernel_free(mod_addr[i]);
	mod_addr[i] = 0;
	mod_loaded[i] = 0;
	if(id == 0x302) atrac_notify_load(0);
	return 0;
}

static void sceUtilityLoadModule(void){
	u32 id = ARG(0);
	RETURN(load_module(id, 0));
	hle_delay_us(id == 0x3FF ? 130 : 25000);
}

static void sceUtilityUnloadModule(void){
	u32 id = ARG(0);
	RETURN(unload_module(id, 0));
	hle_delay_us(id == 0x3FF ? 110 : 400);
}

static void sceUtilityLoadAvModule(void){
	if(ARG(0) > 7){ RETURN(ERR_AV_MODULE_BAD_ID); return; }
	RETURN(load_module(0x300 | ARG(0), 1));
	hle_delay_us(25000);
}

static void sceUtilityUnloadAvModule(void){
	if(ARG(0) > 7){ RETURN(ERR_AV_MODULE_BAD_ID); return; }
	RETURN(unload_module(0x300 | ARG(0), 1));
	hle_delay_us(800);
}

static void utility_zero(void){ RETURN(0); }

/* --- Parámetros del sistema ------------------------------------------------------- */

enum {
	SP_NICKNAME = 1, SP_ADHOC_CHANNEL = 2, SP_WLAN_POWERSAVE = 3, SP_DATE_FORMAT = 4,
	SP_TIME_FORMAT = 5, SP_TIMEZONE = 6, SP_DAYLIGHT = 7, SP_LANGUAGE = 8, SP_BUTTON = 9,
	SP_PARENTAL = 10,
};

static void sceUtilityGetSystemParamInt(void){
	u32 v;
	switch(ARG(0)){
	case SP_ADHOC_CHANNEL: v = 0; break;
	case SP_WLAN_POWERSAVE: v = 0; break;
	case SP_DATE_FORMAT: v = 0; break;      /* YYYYMMDD */
	case SP_TIME_FORMAT: v = 0; break;      /* 24 h */
	case SP_TIMEZONE: v = 0; break;
	case SP_DAYLIGHT: v = 0; break;
	case SP_LANGUAGE: v = 1; break;         /* inglés */
	case SP_BUTTON: v = 1; break;           /* X confirma */
	case SP_PARENTAL: v = 0; break;
	default: RETURN(ERR_INVALID_SYSPARAM_ID); return;
	}
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), v);
	RETURN(0);
}

static void sceUtilitySetSystemParamInt(void){
	u32 id = ARG(0), v = ARG(1);
	if(id == SP_ADHOC_CHANNEL){
		if(v != 0 && v != 1 && v != 6 && v != 11){ RETURN(ERR_INVALID_ADHOC_CHANNEL); return; }
	} else if(id != SP_WLAN_POWERSAVE){ RETURN(ERR_INVALID_SYSPARAM_ID); return; }
	RETURN(0);
}

static void sceUtilityGetSystemParamString(void){
	static const char nick[] = "WIISP";
	u32 dst = ARG(1);
	s32 size = (s32)ARG(2);
	if(size > 0 && !mem_valid(dst, (u32)size)){ RETURN(0xFFFFFFFFu); return; }
	if(ARG(0) != SP_NICKNAME){ RETURN(ERR_INVALID_SYSPARAM_ID); return; }
	if(size <= (s32)(sizeof(nick) - 1)){ RETURN(ERR_STRING_TOO_LONG); return; }
	memset(mem_ptr(dst, (u32)size), 0, (size_t)size);
	memcpy(mem_ptr(dst, sizeof(nick)), nick, sizeof(nick));
	RETURN(0);
}

/* --- Diálogos: estado común ---------------------------------------------------------- */

enum { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };
enum {
	DLG_NONE, DLG_SAVEDATA, DLG_MSG, DLG_OSK, DLG_NET, DLG_NPSIGNIN, DLG_SCREENSHOT,
	DLG_GAMEDATA, DLG_GAMESHARING, DLG_HTML, DLG_COUNT
};

typedef struct {
	int status, pending;
	u64 pending_at;   /* 0 = nada pendiente */
	u32 addr, size;   /* la petición (diálogos genéricos) */
	int updates;
} Dialog;

static Dialog dlgs[DLG_COUNT];
#define dlg_save dlgs[DLG_SAVEDATA]
#define dlg_msg dlgs[DLG_MSG]
static int current_type;

static void dlg_update(Dialog *d){
	if(d->pending_at && cpu_cycles >= d->pending_at){
		d->status = d->pending;
		d->pending_at = 0;
	}
}

static void dlg_change(Dialog *d, int st, u32 delay_us){
	if(!delay_us){ d->status = d->pending = st; d->pending_at = 0; }
	else { d->pending = st; d->pending_at = cpu_cycles + (u64)delay_us * CYCLES_PER_US; }
}

static Dialog *dlg_of(int type){
	return type > DLG_NONE && type < DLG_COUNT ? &dlgs[type] : NULL;
}

/* Un diálogo a la vez, del tipo que sea */
static int dlg_busy(void){
	Dialog *d = dlg_of(current_type);
	if(!d) return 0;
	dlg_update(d);
	return d->status != ST_NONE;
}

static u32 check_request(u32 addr, const u32 *sizes, int n){
	u32 size;
	int i;
	if(!mem_valid(addr, 48)) return ERR_INVALID_ADDRESS;
	size = mem_read32(addr);
	for(i = 0; i < n && sizes[i] != size; i++);
	if(i == n) return ERR_INVALID_PARAM_SIZE;
	if(!mem_valid(addr, size)) return ERR_INVALID_ADDRESS;
	if(addr & 3) return SCE_KERNEL_ERROR_BAD_ARGUMENT;
	return 0;
}

/* Copia s a un campo de n bytes, recortado y terminado en 0 */
static void put_field(void *dst, const char *s, size_t n){
	size_t len = strlen(s);
	if(len > n - 1) len = n - 1;
	memcpy(dst, s, len);
	((char *)dst)[len] = 0;
}

/* Copia un campo de texto de tamaño fijo (puede no terminar en 0) */
static void read_field(u32 addr, u32 n, char *out){
	u32 i;
	for(i = 0; i < n; i++){
		out[i] = (char)mem_read8(addr + i);
		if(!out[i]) break;
	}
	out[i < n ? i : n] = 0;
}

/* --- MsgDialog ---------------------------------------------------------------------- */

#define MSG_SIZE_V1 572
#define MSG_SIZE_V2 580
#define MSG_SIZE_V3 708
#define MSG_OPT_YESNO      0x10
#define MSG_OPT_OK         0x20
#define MSG_OPT_NOCANCEL   0x80
#define MSG_OPT_DEFAULT_NO 0x100
#define MSG_OPT_TEXTSOUND  0x01
#define MSG_OPT_SUPPORTED  0x1B3

static struct {
	u32 addr, size, options;
	int error, abort, frames, valid_button, yesno, default_no;
	u32 result;
} msg;

static void sceUtilityMsgDialogInitStart(void){
	static const u32 sizes[] = { MSG_SIZE_V1, MSG_SIZE_V2, MSG_SIZE_V3 };
	u32 addr = ARG(0), r, type, err_num;
	char text[513];
	if(dlg_busy()){ RETURN(ERR_INVALID_STATUS); return; }
	dlg_update(&dlg_msg);
	if(dlg_msg.status != ST_NONE){ RETURN(ERR_INVALID_STATUS); return; }
	if((r = check_request(addr, sizes, 3)) != 0){ RETURN(r); return; }
	memset(&msg, 0, sizeof(msg));
	msg.addr = addr;
	msg.size = mem_read32(addr);
	type = mem_read32(addr + 0x34);
	err_num = mem_read32(addr + 0x38);
	msg.options = msg.size >= MSG_SIZE_V2 ? mem_read32(addr + 0x23C) : 0;
	if(type == 0 && !(err_num & 0x80000000u)){ msg.error = 1; msg.result = ERR_MSG_ERRORCODEINVALID; }
	else if(msg.size == MSG_SIZE_V2 && type == 1){
		u32 valid = MSG_OPT_TEXTSOUND | MSG_OPT_YESNO | MSG_OPT_DEFAULT_NO;
		if((msg.options | valid) ^ valid){ msg.error = 1; msg.result = ERR_MSG_BADOPTION; }
	} else if(msg.size == MSG_SIZE_V3){
		if(((msg.options & MSG_OPT_DEFAULT_NO) && !(msg.options & MSG_OPT_YESNO)) || (msg.options & ~MSG_OPT_SUPPORTED)){
			msg.error = 1;
			msg.result = ERR_MSG_BADOPTION;
		}
	}
	if(!msg.error){
		msg.yesno = (msg.options & MSG_OPT_YESNO) && (msg.size == MSG_SIZE_V3 || (msg.size == MSG_SIZE_V2 && type == 1));
		msg.default_no = (msg.options & MSG_OPT_DEFAULT_NO) != 0;
		msg.valid_button = msg.yesno || ((msg.options & MSG_OPT_OK) && msg.size == MSG_SIZE_V3);
		if(type == 1){
			read_field(addr + 0x3C, 512, text);
			hle_log("[Utility] mensaje: \"%s\"%s\n", text, msg.yesno ? (msg.default_no ? " (Si/No: No)" : " (Si/No: Si)") : "");
		} else hle_log("[Utility] mensaje de error 0x%08X\n", err_num);
	}
	dlg_change(&dlg_msg, ST_INIT, 0);
	dlg_change(&dlg_msg, ST_RUNNING, 300000);
	current_type = DLG_MSG;
	RETURN(0);
}

static void sceUtilityMsgDialogGetStatus(void){
	if(current_type != DLG_MSG){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_msg);
	RETURN((u32)dlg_msg.status);
}

static void sceUtilityMsgDialogUpdate(void){
	u32 a = msg.addr;
	if(current_type != DLG_MSG){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_msg);
	if(dlg_msg.status != ST_RUNNING){ RETURN(ERR_INVALID_STATUS); return; }
	msg.frames++;
	if(msg.error || (msg.abort && msg.frames >= 8)){
		dlg_change(&dlg_msg, ST_FINISHED, 0);
	} else {
		/* Con botones: se acepta la opción por defecto enseguida. Sin
		   botones (avisos tipo "guardando..."), sigue hasta que el juego lo
		   cierra o pasa un segundo. */
		if(msg.valid_button && msg.frames >= 2){
			if(mem_valid(a + 0x240, 4) && msg.size >= MSG_SIZE_V2) mem_write32(a + 0x240, msg.yesno && msg.default_no ? 2 : 1);
			dlg_change(&dlg_msg, ST_FINISHED, 0);
		} else if(!msg.valid_button && msg.frames >= 60){
			if(mem_valid(a + 0x240, 4) && msg.size >= MSG_SIZE_V2) mem_write32(a + 0x240, msg.size == MSG_SIZE_V3 ? 3 : 0);
			dlg_change(&dlg_msg, ST_FINISHED, 0);
		}
		msg.result = 0;
	}
	if(mem_valid(a + 0x30, 4)) mem_write32(a + 0x30, msg.result);
	RETURN(0);
	hle_delay_us(800);
}

static void sceUtilityMsgDialogAbort(void){
	if(current_type != DLG_MSG){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_msg);
	if(dlg_msg.status != ST_RUNNING){ RETURN(ERR_INVALID_STATUS); return; }
	msg.abort = 1;
	RETURN(0);
}

static void sceUtilityMsgDialogShutdownStart(void){
	if(current_type != DLG_MSG){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_msg);
	if(dlg_msg.status != ST_FINISHED){ RETURN(ERR_INVALID_STATUS); return; }
	dlg_change(&dlg_msg, ST_SHUTDOWN, 0);
	dlg_change(&dlg_msg, ST_NONE, 26000);
	RETURN(0);
}

/* --- Savedata ---------------------------------------------------------------------------- */

enum {
	SD_AUTOLOAD = 0, SD_AUTOSAVE, SD_LOAD, SD_SAVE, SD_LISTLOAD, SD_LISTSAVE, SD_LISTDELETE,
	SD_LISTALLDELETE, SD_SIZES, SD_AUTODELETE, SD_DELETE, SD_LIST, SD_FILES, SD_MAKEDATASECURE,
	SD_MAKEDATA, SD_READDATASECURE, SD_READDATA, SD_WRITEDATASECURE, SD_WRITEDATA, SD_ERASESECURE,
	SD_ERASE, SD_DELETEDATA, SD_GETSIZE,
};

enum {
	FOCUS_NAME = 0, FOCUS_FIRSTLIST, FOCUS_LASTLIST, FOCUS_LATEST, FOCUS_OLDEST, FOCUS_FIRSTDATA,
	FOCUS_LASTDATA, FOCUS_FIRSTEMPTY, FOCUS_LASTEMPTY,
};

/* Desplazamientos en SceUtilitySavedataParam */
#define P_RESULT     0x1C
#define P_MODE       0x30
#define P_BIND       0x34
#define P_GAMENAME   0x3C
#define P_SAVENAME   0x4C
#define P_NAMELIST   0x60
#define P_FILENAME   0x64
#define P_DATABUF    0x74
#define P_DATABUFSZ  0x78
#define P_DATASIZE   0x7C
#define P_TITLE      0x80
#define P_SAVETITLE  0x100
#define P_DETAIL     0x180
#define P_PARENTAL   0x580
#define P_ICON0      0x584
#define P_ICON1      0x594
#define P_PIC1       0x5A4
#define P_SND0       0x5B4
#define P_FOCUS      0x5C8
#define P_MSFREE     0x5D0
#define P_MSDATA     0x5D4
#define P_UTILDATA   0x5D8
#define P_SECUREVER  0x5EC
#define P_IDLIST     0x5F4
#define P_FILELIST   0x5F8
#define P_SIZEINFO   0x5FC

#define SAVE_SIZE_V1 1480
#define SAVE_SIZE_V2 1500
#define SAVE_SIZE_V3 1536

#define MAX_LIST 100
#define CLUSTER 32768u
/* Lo que tenía libre la Memory Stick con que se grabaron las pruebas */
#define FREE_BYTES 0x3FFFFEC00ull

static const char *const extra_files[4] = { "ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3" };
static const u32 extra_offs[4] = { P_ICON0, P_ICON1, P_PIC1, P_SND0 };

static struct {
	u32 addr, size;
	int done;
	/* Lo que la PSP escribe en un Update posterior (el diálogo trabaja
	   durante varios fotogramas): se aplica en el siguiente */
	u32 late_addr, late_len;
	u8 late[64];
} save;

static void defer_write(u32 addr, const void *data, u32 len){
	if(len > sizeof(save.late) || !mem_valid(addr, len)) return;
	save.late_addr = addr;
	save.late_len = len;
	memcpy(save.late, data, len);
}

static u32 prm32(u32 off){ return off + 4 <= save.size ? mem_read32(save.addr + off) : 0; }
static void set_prm32(u32 off, u32 v){ if(off + 4 <= save.size) mem_write32(save.addr + off, v); }

/* Nombres de archivo sin rutas: nada de '/', '\\' ni "." / ".." */
static int safe_name(const char *s){
	if(!strcmp(s, ".") || !strcmp(s, "..")) return 0;
	for(; *s; s++) if(*s == '/' || *s == '\\' || *s == ':') return 0;
	return 1;
}

static void game_name(char out[14]){ read_field(save.addr + P_GAMENAME, 13, out); }
static void file_name(char out[14]){ read_field(save.addr + P_FILENAME, 13, out); }

static void save_name(char out[21]){
	read_field(save.addr + P_SAVENAME, 20, out);
	if(!strcmp(out, "<>")) out[0] = 0;
}

/* Carpeta del anfitrión de una partida ("" = la raíz de SAVEDATA) */
static int save_dir(const char *dirname, char *out, u32 n){
	char p[96];
	snprintf(p, sizeof(p), "ms0:/PSP/SAVEDATA%s%s", dirname[0] ? "/" : "", dirname);
	return io_host_path(p, out, n);
}

static int dir_exists(const char *path){
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static long file_size(const char *path){
	struct stat st;
	return stat(path, &st) ? -1 : (long)st.st_size;
}

static void make_dirs(const char *path){
	char tmp[400];
	char *p;
	snprintf(tmp, sizeof(tmp), "%s", path);
	for(p = tmp + 1; *p; p++){
		if(*p != '/' || p[-1] == ':') continue;
		*p = 0;
		mkdir(tmp, 0777);
		*p = '/';
	}
	mkdir(tmp, 0777);
}

/* Escribe [addr, addr+len) de la PSP a un archivo */
static int write_file(const char *path, u32 addr, u32 len){
	FILE *f = fopen(path, "wb");
	const u8 *p = len ? mem_ptr_r(addr, len) : NULL;
	int ok;
	if(!f) return -1;
	ok = !len || (p && fwrite(p, 1, len, f) == len);
	if(fclose(f)) ok = 0;
	return ok ? 0 : -1;
}

/* Lee un archivo a la PSP (hasta cap bytes). Devuelve los leídos o -1. */
static long read_file(const char *path, u32 addr, u32 cap){
	FILE *f = fopen(path, "rb");
	long n = 0;
	if(!f) return -1;
	if(cap){
		u8 *p = mem_ptr(addr, cap);
		n = p ? (long)fread(p, 1, cap, f) : -1;
		if(n > 0) mem_note_write(addr, (u32)n);
	}
	fclose(f);
	return n;
}

#define FILE_LIST_MAX 99
#define FILE_LIST_SIZE (FILE_LIST_MAX * 32)   /* nombre[13], hash[16], relleno[3] */

/* SAVEDATA_FILE_LIST del PARAM.SFO existente (los archivos "seguros").
   -1 si no hay un PARAM.SFO válido. */
static int read_file_list(const char *dir, u8 list[FILE_LIST_SIZE]){
	char path[700];
	u8 buf[0x1400];
	FILE *f;
	size_t n;
	SfoFile sfo;
	memset(list, 0, FILE_LIST_SIZE);
	snprintf(path, sizeof(path), "%s/PARAM.SFO", dir);
	if(!(f = fopen(path, "rb"))) return -1;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if(sfo_parse(buf, (u32)n, &sfo)) return -1;
	sfo_get_data(&sfo, "SAVEDATA_FILE_LIST", list, FILE_LIST_SIZE);
	return 0;
}

static int in_file_list(const u8 list[FILE_LIST_SIZE], const char *name){
	int i;
	for(i = 0; i < FILE_LIST_MAX; i++)
		if(list[i * 32] && !strncmp((const char *)list + i * 32, name, 13)) return 1;
	return 0;
}

/* secure_file: el archivo de datos guardado en modo seguro (o NULL) */
static void write_sfo(const char *dir, const char *dirname, const char *secure_file){
	char title[0x81], stitle[0x81], detail[0x401], path[700];
	u8 buf[0x1400], list[FILE_LIST_SIZE];
	u32 n;
	int i;
	SfoEntry e[7];
	read_field(save.addr + P_TITLE, 0x80, title);
	read_field(save.addr + P_SAVETITLE, 0x80, stitle);
	read_field(save.addr + P_DETAIL, 0x400, detail);
	read_file_list(dir, list);
	if(secure_file && secure_file[0] && !in_file_list(list, secure_file))
		for(i = 0; i < FILE_LIST_MAX; i++)
			if(!list[i * 32]){ put_field(list + i * 32, secure_file, 13); break; }
	memset(e, 0, sizeof(e));
	e[0].key = "CATEGORY"; e[0].fmt = SFO_FMT_UTF8; e[0].max_len = 4; e[0].str = "MS";
	e[1].key = "PARENTAL_LEVEL"; e[1].fmt = SFO_FMT_INT32; e[1].max_len = 4; e[1].value = mem_read8(save.addr + P_PARENTAL);
	e[2].key = "SAVEDATA_DETAIL"; e[2].fmt = SFO_FMT_UTF8; e[2].max_len = 0x400; e[2].str = detail;
	e[3].key = "SAVEDATA_DIRECTORY"; e[3].fmt = SFO_FMT_UTF8; e[3].max_len = 0x40; e[3].str = dirname;
	e[4].key = "SAVEDATA_FILE_LIST"; e[4].fmt = SFO_FMT_UTF8_RAW; e[4].max_len = FILE_LIST_SIZE;
	e[4].str = (const char *)list; e[4].len = FILE_LIST_SIZE;
	e[5].key = "SAVEDATA_TITLE"; e[5].fmt = SFO_FMT_UTF8; e[5].max_len = 0x80; e[5].str = stitle;
	e[6].key = "TITLE"; e[6].fmt = SFO_FMT_UTF8; e[6].max_len = 0x80; e[6].str = title;
	n = sfo_build(e, 7, buf, sizeof(buf));
	snprintf(path, sizeof(path), "%s/PARAM.SFO", dir);
	if(n){
		FILE *f = fopen(path, "wb");
		if(f){ fwrite(buf, 1, n, f); fclose(f); }
	}
}

/* Lee el PARAM.SFO de la partida al parámetro (título, detalle...) */
static int load_sfo(const char *dir){
	char path[700], s[0x401];
	u8 buf[0x1400];
	FILE *f;
	size_t n;
	SfoFile sfo;
	u32 v;
	snprintf(path, sizeof(path), "%s/PARAM.SFO", dir);
	f = fopen(path, "rb");
	if(!f) return -1;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if(sfo_parse(buf, (u32)n, &sfo)) return -1;
	if(!sfo_get_string(&sfo, "TITLE", s, 0x80) && save.size > P_TITLE + 0x80)
		memcpy(mem_ptr(save.addr + P_TITLE, 0x80), s, strlen(s) + 1);
	if(!sfo_get_string(&sfo, "SAVEDATA_TITLE", s, 0x80) && save.size > P_SAVETITLE + 0x80)
		memcpy(mem_ptr(save.addr + P_SAVETITLE, 0x80), s, strlen(s) + 1);
	if(!sfo_get_string(&sfo, "SAVEDATA_DETAIL", s, 0x400) && save.size > P_DETAIL + 0x400)
		memcpy(mem_ptr(save.addr + P_DETAIL, 0x400), s, strlen(s) + 1);
	if(!sfo_get_int(&sfo, "PARENTAL_LEVEL", &v) && save.size > P_PARENTAL) mem_write8(save.addr + P_PARENTAL, (u8)v);
	return 0;
}

static u32 do_save(const char *dirname, int rw, int secure){
	char dir[400], path[700], fname[14];
	u32 buf = prm32(P_DATABUF), bufsz = prm32(P_DATABUFSZ), size = prm32(P_DATASIZE);
	int i;
	if(prm32(P_SECUREVER) > 3) return SD_ERR_SAVE_PARAM;
	if(size > bufsz) return SD_ERR_RW_BAD_PARAMS;
	if(save_dir(dirname, dir, sizeof(dir))) return SD_ERR_SAVE_ACCESS_ERROR;
	make_dirs(dir);
	if(!dir_exists(dir)) return rw ? SD_ERR_RW_MEMSTICK_FULL : SD_ERR_SAVE_MS_NOSPACE;
	file_name(fname);
	if(fname[0] && buf){
		if(!safe_name(fname)) return SD_ERR_SAVE_PARAM;
		snprintf(path, sizeof(path), "%s/%s", dir, fname);
		if(write_file(path, buf, size)) return rw ? SD_ERR_RW_MEMSTICK_FULL : SD_ERR_SAVE_MS_NOSPACE;
	}
	for(i = 0; i < 4; i++){
		u32 o = extra_offs[i], b = prm32(o), bs = prm32(o + 4), s = prm32(o + 8);
		if(!b || !s || s > bs) continue;
		snprintf(path, sizeof(path), "%s/%s", dir, extra_files[i]);
		write_file(path, b, s);
	}
	write_sfo(dir, dirname, secure && buf ? fname : NULL);
	hle_log("[Utility] partida guardada: %s\n", dirname);
	return 0;
}

static u32 do_load(const char *dirname, int rw){
	char dir[400], path[700], fname[14];
	u32 buf = prm32(P_DATABUF), bufsz = prm32(P_DATABUFSZ);
	long n;
	int i;
	u8 list[FILE_LIST_SIZE];
	if(save_dir(dirname, dir, sizeof(dir)) || !dir_exists(dir)) return rw ? SD_ERR_RW_NO_DATA : SD_ERR_LOAD_NO_DATA;
	/* Sin un PARAM.SFO válido la partida está rota (utility/savedata/loadbroken) */
	if(read_file_list(dir, list)) return rw ? SD_ERR_RW_DATA_BROKEN : SD_ERR_LOAD_DATA_BROKEN;
	file_name(fname);
	if(fname[0]){
		if(!safe_name(fname)) return SD_ERR_LOAD_PARAM;
		snprintf(path, sizeof(path), "%s/%s", dir, fname);
		if(file_size(path) < 0) return rw ? SD_ERR_RW_FILE_NOT_FOUND : SD_ERR_LOAD_FILE_NOT_FOUND;
	}
	if(prm32(P_SECUREVER) > 3) return SD_ERR_LOAD_PARAM;
	/* Un archivo guardado en modo seguro no se puede leer con READDATA */
	if(fname[0] && prm32(P_MODE) == SD_READDATA && in_file_list(list, fname)) return SD_ERR_RW_DATA_BROKEN;
	set_prm32(P_DATASIZE, 0);
	if(fname[0]){
		n = read_file(path, buf, bufsz);
		if(n < 0) return rw ? SD_ERR_RW_DATA_BROKEN : SD_ERR_LOAD_DATA_BROKEN;
		set_prm32(P_DATASIZE, (u32)n);
	}
	if(load_sfo(dir)) return rw ? SD_ERR_RW_DATA_BROKEN : SD_ERR_LOAD_DATA_BROKEN;
	set_prm32(P_BIND, 1021);   /* lo que contesta la PSP (PPSSPP) */
	for(i = 0; i < 4; i++){
		u32 o = extra_offs[i], b = prm32(o), bs = prm32(o + 4);
		snprintf(path, sizeof(path), "%s/%s", dir, extra_files[i]);
		if(b && bs){
			n = read_file(path, b, bs);
			set_prm32(o + 8, n > 0 ? (u32)n : 0);
		}
	}
	hle_log("[Utility] partida cargada: %s\n", dirname);
	return 0;
}

static int remove_dir(const char *dir){
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[700];
	if(!d) return -1;
	while((e = readdir(d))){
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
	return rmdir(dir);
}

/* Lista de nombres de partida que ofrece el juego (saveNameList) */
static int name_list(char names[MAX_LIST][21]){
	u32 p = prm32(P_NAMELIST);
	int n = 0;
	if(!p) return 0;
	while(n < MAX_LIST && mem_valid(p + (u32)n * 20, 20) && mem_read8(p + (u32)n * 20)){
		read_field(p + (u32)n * 20, 20, names[n]);
		if(!strcmp(names[n], "<>")) names[n][0] = 0;
		n++;
	}
	return n;
}

/* Fecha de una partida (0 si no existe) */
static long long save_time(const char *game, const char *name){
	char dirname[40], dir[400];
	struct stat st;
	snprintf(dirname, sizeof(dirname), "%.13s%.20s", game, name);
	if(save_dir(dirname, dir, sizeof(dir)) || stat(dir, &st) || !S_ISDIR(st.st_mode)) return 0;
	return (long long)st.st_mtime + 1;
}

/* Elige la partida de una lista como si el usuario aceptara lo que
   propone el diálogo. -1 si no hay ninguna adecuada. */
static int choose(char names[MAX_LIST][21], int n, const char *game, int want_data){
	int focus = (int)prm32(P_FOCUS), i, pick = -1;
	long long t[MAX_LIST], best = 0;
	char cur[21];
	if(n <= 0) return -1;
	memset(t, 0, sizeof(t));
	save_name(cur);
	for(i = 0; i < n; i++) t[i] = save_time(game, names[i]);
	switch(focus){
	case FOCUS_NAME: for(i = 0; i < n; i++) if(!strcmp(names[i], cur)){ pick = i; break; } break;
	case FOCUS_FIRSTLIST: pick = 0; break;
	case FOCUS_LASTLIST: pick = n - 1; break;
	case FOCUS_FIRSTDATA: for(i = 0; i < n; i++) if(t[i]){ pick = i; break; } break;
	case FOCUS_LASTDATA: for(i = n - 1; i >= 0; i--) if(t[i]){ pick = i; break; } break;
	case FOCUS_FIRSTEMPTY: for(i = 0; i < n; i++) if(!t[i]){ pick = i; break; } break;
	case FOCUS_LASTEMPTY: for(i = n - 1; i >= 0; i--) if(!t[i]){ pick = i; break; } break;
	case FOCUS_OLDEST:
		for(i = 0; i < n; i++) if(t[i] && (pick < 0 || t[i] < best)){ pick = i; best = t[i]; }
		break;
	default:   /* LATEST */
		for(i = 0; i < n; i++) if(t[i] > best){ pick = i; best = t[i]; }
		break;
	}
	/* Para cargar hace falta una con datos: la más reciente */
	if(want_data && (pick < 0 || !t[pick])){
		pick = -1;
		best = 0;
		for(i = 0; i < n; i++) if(t[i] > best){ pick = i; best = t[i]; }
	}
	if(!want_data && pick < 0 && n > 0) pick = 0;
	return pick;
}

static int wildcard_match(const char *pat, const char *s){
	if(!*pat) return !*s;
	if(*pat == '*') return wildcard_match(pat + 1, s) || (*s && wildcard_match(pat, s + 1));
	return *s && (*pat == '?' || *pat == *s) && wildcard_match(pat + 1, s + 1);
}

/* "15 GB": como GetSpaceText de PPSSPP (redondeando hacia arriba o abajo) */
static void write_space_str(u32 addr, u64 bytes, int round_up){
	static const char *const suffix[] = { "B", "KB", "MB", "GB", "TB" };
	char s[24];
	int i;
	for(i = 0; i < 4 && bytes >= 1024; i++) bytes = round_up ? (bytes + 1023) / 1024 : bytes / 1024;
	snprintf(s, sizeof(s), "%llu %s", (unsigned long long)bytes, suffix[i]);
	if(mem_valid(addr, 8)){ memset(mem_ptr(addr, 8), 0, 8); memcpy(mem_ptr(addr, 8), s, strlen(s) > 7 ? 7 : strlen(s)); }
}

/* Ocupación de una carpeta en clústeres */
static u32 dir_clusters(const char *dir){
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[700];
	u32 c = 0;
	if(!d) return 0;
	while((e = readdir(d))){
		long sz;
		if(e->d_name[0] == '.') continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		sz = file_size(path);
		if(sz > 0) c += ((u32)sz + CLUSTER - 1) / CLUSTER;
	}
	closedir(d);
	return c;
}

static u32 do_sizes(void){
	u32 r = 0, p;
	if((p = prm32(P_MSFREE)) && mem_valid(p, 20)){
		mem_write32(p, CLUSTER);
		mem_write32(p + 4, (u32)(FREE_BYTES / CLUSTER));
		mem_write32(p + 8, (u32)(FREE_BYTES / 1024));
		write_space_str(p + 12, FREE_BYTES, 0);
	}
	if((p = prm32(P_MSDATA)) && mem_valid(p, 64)){
		char g[14], s[21], dirname[40], dir[400];
		read_field(p, 13, g);
		read_field(p + 16, 20, s);
		snprintf(dirname, sizeof(dirname), "%s%s", g, strcmp(s, "<>") ? s : "");
		u8 info[28];
		memcpy(info, mem_ptr_r(p + 36, 28), 28);
		if(!save_dir(dirname, dir, sizeof(dir)) && dir_exists(dir)){
			u32 c = dir_clusters(dir) + 1;   /* y la propia carpeta */
			mem_write32(p + 36, c);
			mem_write32(p + 40, c * CLUSTER / 1024);
			write_space_str(p + 44, (u64)c * CLUSTER, 1);
			mem_write32(p + 52, c * CLUSTER / 1024);
			write_space_str(p + 56, (u64)c * CLUSTER, 1);
		} else {
			memset(mem_ptr(p + 36, 28), 0, 28);
			r = SD_ERR_SIZES_NO_DATA;
		}
		/* msData llega un Update más tarde que msFree y utilityData */
		{
			u8 now[28];
			memcpy(now, mem_ptr_r(p + 36, 28), 28);
			memcpy(mem_ptr(p + 36, 28), info, 28);
			defer_write(p + 36, now, 28);
		}
	}
	if((p = prm32(P_UTILDATA)) && mem_valid(p, 28)){
		u32 total = 2 * CLUSTER, i;
		char fname[14];
		file_name(fname);
		if(fname[0]) total += (prm32(P_DATASIZE) + CLUSTER - 1) / CLUSTER * CLUSTER;
		for(i = 0; i < 4; i++) total += (prm32(extra_offs[i] + 8) + CLUSTER - 1) / CLUSTER * CLUSTER;
		mem_write32(p, total / CLUSTER);
		mem_write32(p + 4, total / 1024);
		write_space_str(p + 8, total, 1);
		mem_write32(p + 16, total / 1024);
		write_space_str(p + 20, total, 1);
	}
	return r;
}

/* LIST: las partidas de este juego que encajan con saveName ('*' = comodín) */
static void do_list(void){
	u32 p = prm32(P_IDLIST), max, entries, count = 0;
	char game[14], name[21], pat[40], root[400];
	DIR *d;
	struct dirent *e;
	if(!p || !mem_valid(p, 12)) return;
	max = mem_read32(p);
	entries = mem_read32(p + 8);
	game_name(game);
	save_name(name);
	snprintf(pat, sizeof(pat), "%s%s", game, name[0] ? name : "*");
	if(!save_dir("", root, sizeof(root)) && (d = opendir(root))){
		while((e = readdir(d)) && count < max){
			char path[700];
			struct stat st;
			u32 ent = entries + count * 0x48;
			if(e->d_name[0] == '.' || !wildcard_match(pat, e->d_name)) continue;
			snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
			if(stat(path, &st) || !S_ISDIR(st.st_mode) || !mem_valid(ent, 0x48)) continue;
			mem_write32(ent, 0x11FF);
			hle_write_datetime(ent + 4, (u64)st.st_ctime * 1000000ull);
			hle_write_datetime(ent + 20, (u64)st.st_atime * 1000000ull);
			hle_write_datetime(ent + 36, (u64)st.st_mtime * 1000000ull);
			memset(mem_ptr(ent + 52, 20), 0, 20);
			put_field(mem_ptr(ent + 52, 20), e->d_name + strlen(game), 20);
			count++;
		}
		closedir(d);
	}
	{
		u8 v[4];
		wr_le32(v, count);
		defer_write(p + 4, v, 4);
	}
}

/* FILES: los archivos de la partida (seguros, normales y del sistema) */
static int has_lower(const char *n){
	for(; *n; n++) if(*n >= 'a' && *n <= 'z') return 1;
	return 0;
}

static u32 do_files(const char *dirname){
	u32 p = prm32(P_FILELIST), max[3], base[3], cnt[3] = { 0, 0, 0 };
	char dir[400];
	u8 list[FILE_LIST_SIZE];
	DIR *d;
	struct dirent *e;
	int k;
	if(!p || !mem_valid(p, 36)) return 0xFFFFFFFFu;
	for(k = 0; k < 3; k++){ max[k] = mem_read32(p + (u32)k * 4); base[k] = mem_read32(p + 24 + (u32)k * 4); }
	if((base[0] && max[0] > 99) || (base[1] && max[1] > 8192) || (base[2] && max[2] > 5 && kernel_sdk_version() >= 0x02060000))
		return SD_ERR_RW_BAD_PARAMS;
	if(save_dir(dirname, dir, sizeof(dir)) || !dir_exists(dir)) return SD_ERR_RW_NO_DATA;
	if(read_file_list(dir, list)) return SD_ERR_RW_DATA_BROKEN;
	set_prm32(P_BIND, 1021);
	if((d = opendir(dir))){
		while((e = readdir(d))){
			char path[700];
			struct stat st;
			const char *nm = e->d_name;
			int sys = !strcmp(nm, "PARAM.SFO") || !strcmp(nm, "ICON0.PNG") || !strcmp(nm, "ICON1.PMF") ||
			          !strcmp(nm, "PIC1.PNG") || !strcmp(nm, "SND0.AT3");
			u32 ent;
			k = sys ? 2 : in_file_list(list, nm) ? 0 : 1;
			if(nm[0] == '.' || has_lower(nm)) continue;
			snprintf(path, sizeof(path), "%s/%s", dir, nm);
			if(stat(path, &st) || S_ISDIR(st.st_mode) || !base[k] || cnt[k] >= max[k]) continue;
			ent = base[k] + cnt[k]++ * 80;
			if(!mem_valid(ent, 80)) continue;
			mem_write32(ent, 0x21FF);
			mem_write32(ent + 8, (u32)st.st_size);
			mem_write32(ent + 12, 0);
			hle_write_datetime(ent + 16, (u64)st.st_ctime * 1000000ull);
			hle_write_datetime(ent + 32, (u64)st.st_atime * 1000000ull);
			hle_write_datetime(ent + 48, (u64)st.st_mtime * 1000000ull);
			memset(mem_ptr(ent + 64, 16), 0, 16);
			put_field(mem_ptr(ent + 64, 16), nm, 16);
		}
		closedir(d);
	}
	for(k = 0; k < 3; k++) mem_write32(p + 12 + (u32)k * 4, cnt[k]);
	return 0;
}

/* GETSIZE: espacio que harían falta para escribir los archivos de sizeInfo */
static u32 do_getsize(const char *dirname){
	u32 p = prm32(P_SIZEINFO);
	char dir[400];
	int exists = !save_dir(dirname, dir, sizeof(dir)) && dir_exists(dir);
	if(p && mem_valid(p, 60)){
		s64 write = 0, over = 0;
		int k;
		for(k = 0; k < 2; k++){
			u32 n = mem_read32(p + (k ? 4 : 0)), list = mem_read32(p + (k ? 12 : 8)), i;
			for(i = 0; i < n && i < 256; i++){
				u32 ent = list + i * 24;
				char name[17], path[700];
				long sz;
				if(!mem_valid(ent, 24)) break;
				write += (s64)mem_read32(ent) + (k ? 0 : 0x10);   /* los seguros ocupan 16 más */
				read_field(ent + 8, 16, name);
				snprintf(path, sizeof(path), "%s/%s", dir, name);
				if(exists && name[0] && safe_name(name) && (sz = file_size(path)) > 0) over += sz;
			}
		}
		mem_write32(p + 16, CLUSTER);
		mem_write32(p + 20, (u32)(FREE_BYTES / CLUSTER));
		mem_write32(p + 24, (u32)(FREE_BYTES / 1024));
		write_space_str(p + 28, FREE_BYTES, 0);
		if(write - over < (s64)FREE_BYTES){
			/* Cabe: las cadenas no se tocan (utility/savedata/getsize) */
			mem_write32(p + 36, 0);
			mem_write32(p + 48, 0);
		} else {
			s64 need = write - (s64)FREE_BYTES;
			mem_write32(p + 36, (u32)((need + 1023) / 1024));
			write_space_str(p + 40, (u64)need, 1);
			mem_write32(p + 48, (u32)((write - over - (s64)FREE_BYTES + 1023) / 1024));
			write_space_str(p + 52, (u64)(write - over - (s64)FREE_BYTES), 1);
		}
	}
	return exists ? 0 : SD_ERR_RW_NO_DATA;
}

static u32 do_erase(const char *dirname){
	char dir[400], path[700], fname[14];
	u8 list[FILE_LIST_SIZE];
	if(save_dir(dirname, dir, sizeof(dir)) || !dir_exists(dir)) return SD_ERR_RW_NO_DATA;
	if(read_file_list(dir, list)) return SD_ERR_RW_DATA_BROKEN;
	file_name(fname);
	if(!fname[0]) return 0;   /* nada que borrar */
	if(!safe_name(fname)) return SD_ERR_RW_BAD_PARAMS;
	snprintf(path, sizeof(path), "%s/%s", dir, fname);
	if(unlink(path)) return SD_ERR_RW_FILE_NOT_FOUND;
	return 0;
}

/* Hace lo que pide el modo. Devuelve el "result" del parámetro. */
static u32 savedata_run(void){
	u32 mode = prm32(P_MODE), r = 0;
	char game[14], name[21], dirname[40], names[MAX_LIST][21], dir[400];
	int n, pick;
	game_name(game);
	save_name(name);
	if(!safe_name(game) || !safe_name(name)) return mode == SD_AUTOSAVE || mode == SD_SAVE ? SD_ERR_SAVE_PARAM : SD_ERR_LOAD_PARAM;
	snprintf(dirname, sizeof(dirname), "%s%s", game, name);
	hle_log("[Utility] savedata modo %u: %s\n", mode, dirname);
	switch(mode){
	case SD_AUTOLOAD: case SD_LOAD:
		return do_load(dirname, 0);
	case SD_AUTOSAVE: case SD_SAVE:
		return do_save(dirname, 0, 1);
	case SD_LISTLOAD: case SD_LISTSAVE:
		n = name_list(names);
		if(n == 0){ names[0][0] = 0; strcpy(names[0], name); n = 1; }
		pick = choose(names, n, game, mode == SD_LISTLOAD);
		if(pick < 0) return SD_ERR_LOAD_NO_DATA;
		/* El juego lee qué partida se eligió en saveName */
		memset(mem_ptr(save.addr + P_SAVENAME, 20), 0, 20);
		memcpy(mem_ptr(save.addr + P_SAVENAME, 20), names[pick], strlen(names[pick]));
		snprintf(dirname, sizeof(dirname), "%.13s%.20s", game, names[pick]);
		return mode == SD_LISTLOAD ? do_load(dirname, 0) : do_save(dirname, 0, 1);
	case SD_LISTDELETE: case SD_LISTALLDELETE:
		/* Como en la PSP sin tocar nada (utility/savedata/deletedata): se
		   borra la partida que propone la lista y el diálogo contesta "sin
		   datos" (LISTDELETE) o "cancelado" (LISTALLDELETE) */
		n = name_list(names);
		if(n == 0){ strcpy(names[0], name); n = 1; }
		pick = choose(names, n, game, 1);
		if(pick >= 0){
			snprintf(dirname, sizeof(dirname), "%.13s%.20s", game, names[pick]);
			if(!save_dir(dirname, dir, sizeof(dir)) && !remove_dir(dir)) hle_log("[Utility] partida borrada: %s\n", dirname);
		}
		return mode == SD_LISTDELETE ? SD_ERR_DELETE_NO_DATA : RESULT_CANCEL;
	case SD_SIZES:
		return do_sizes();
	case SD_AUTODELETE: case SD_DELETE:
		if(save_dir(dirname, dir, sizeof(dir)) || remove_dir(dir)) return SD_ERR_DELETE_NO_DATA;
		return 0;
	case SD_DELETEDATA:
		if(save_dir(dirname, dir, sizeof(dir)) || remove_dir(dir)) return SD_ERR_RW_NO_DATA;
		return 0;
	case SD_LIST:
		do_list();
		return 0;
	case SD_FILES:
		return do_files(dirname);
	case SD_MAKEDATA: case SD_MAKEDATASECURE:
		r = do_save(dirname, 1, mode == SD_MAKEDATASECURE);
		return r == SD_ERR_SAVE_MS_NOSPACE ? SD_ERR_RW_MEMSTICK_FULL : r;
	case SD_WRITEDATA: case SD_WRITEDATASECURE:
		/* A diferencia de MAKEDATA, la partida ya tiene que existir */
		if(save_dir(dirname, dir, sizeof(dir)) || !dir_exists(dir)) return SD_ERR_RW_NO_DATA;
		return do_save(dirname, 1, mode == SD_WRITEDATASECURE);
	case SD_READDATA: case SD_READDATASECURE:
		return do_load(dirname, 1);
	case SD_ERASE: case SD_ERASESECURE:
		return do_erase(dirname);
	case SD_GETSIZE:
		return do_getsize(dirname);
	default:
		return 0;
	}
}

static void sceUtilitySavedataInitStart(void){
	static const u32 sizes[] = { SAVE_SIZE_V1, SAVE_SIZE_V2, SAVE_SIZE_V3 };
	u32 addr = ARG(0), r;
	if(dlg_busy()){ RETURN(ERR_INVALID_STATUS); return; }
	dlg_update(&dlg_save);
	if(dlg_save.status != ST_NONE){ RETURN(ERR_INVALID_STATUS); return; }
	if((r = check_request(addr, sizes, 3)) != 0){ RETURN(r); return; }
	save.addr = addr;
	save.size = mem_read32(addr);
	save.done = 0;
	dlg_change(&dlg_save, ST_INIT, 0);
	dlg_change(&dlg_save, ST_RUNNING, 200000);
	current_type = DLG_SAVEDATA;
	RETURN(0);
}

static void sceUtilitySavedataGetStatus(void){
	kernel_eat_cycles(200);
	if(current_type != DLG_SAVEDATA){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_save);
	RETURN((u32)dlg_save.status);
}

static void sceUtilitySavedataUpdate(void){
	if(current_type != DLG_SAVEDATA){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_save);
	if(dlg_save.status != ST_RUNNING){ RETURN(ERR_INVALID_STATUS); return; }
	if(!save.done){
		save.late_len = 0;
		set_prm32(P_RESULT, savedata_run());
		save.done = 1;
		if(!save.late_len) dlg_change(&dlg_save, ST_FINISHED, 0);
	} else if(save.late_len){
		memcpy(mem_ptr(save.late_addr, save.late_len), save.late, save.late_len);
		mem_note_write(save.late_addr, save.late_len);
		save.late_len = 0;
		dlg_change(&dlg_save, ST_FINISHED, 0);
	}
	RETURN(0);
	hle_delay_us(300);
}

static void sceUtilitySavedataShutdownStart(void){
	if(current_type != DLG_SAVEDATA){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlg_save);
	if(dlg_save.status != ST_FINISHED){ RETURN(ERR_INVALID_STATUS); return; }
	dlg_change(&dlg_save, ST_SHUTDOWN, 0);
	dlg_change(&dlg_save, ST_NONE, 2000);
	kernel_eat_cycles(30000);
	RETURN(0);
}

/* --- Los demás diálogos: sin interfaz, se cierran solos ----------------------------- */
/* El teclado (OSK) acepta el texto que ya traía; los de red, PSN, capturas,
   instalación, compartir y navegador se dan por cancelados. */

static const u32 sizes_osk[] = { 0x40, 0x44 };
static const u32 sizes_net[] = { 0x38, 0x40, 0x44 };
static const u32 sizes_np[] = { 0x40 };
static const u32 sizes_shot[] = { 436, 928, 932 };
static const u32 sizes_install[] = { 1424, 1432 };
static const u32 sizes_share[] = { 0x50, 0x54, 0x64 };
static const u32 sizes_html[] = { 0x70, 0x78, 0x80, 0x98, 0xA4, 0xA8 };

static void osk_finish(u32 addr){
	u32 n = mem_read32(addr + 0x30), data = mem_read32(addr + 0x34), i;
	for(i = 0; i < n && i < 8; i++){
		u32 d = data + i * 52, in, out, lim, k;
		if(!mem_valid(d, 52)) break;
		in = mem_read32(d + 32);
		out = mem_read32(d + 40);
		lim = mem_read32(d + 48);
		for(k = 0; out && k + 1 < lim && mem_valid(in + k * 2, 2); k++){
			u16 c = mem_read16(in + k * 2);
			if(!c) break;
			mem_write16(out + k * 2, c);
		}
		if(out && lim && mem_valid(out + k * 2, 2)) mem_write16(out + k * 2, 0);
		mem_write32(d + 44, 0);   /* PSP_UTILITY_OSK_RESULT_UNCHANGED */
	}
	mem_write32(addr + 0x38, 0);
	mem_write32(addr + 0x1C, 0);
}

static void generic_init(int type, const u32 *sizes, int n){
	Dialog *d = &dlgs[type];
	u32 r, addr = ARG(0);
	if(type != DLG_HTML && dlg_busy()){ RETURN(ERR_INVALID_STATUS); return; }
	dlg_update(d);
	if(d->status != ST_NONE){ RETURN(ERR_INVALID_STATUS); return; }
	if((r = check_request(addr, sizes, n)) != 0){ RETURN(r); return; }
	d->addr = addr;
	d->size = mem_read32(addr);
	d->updates = 0;
	dlg_change(d, ST_INIT, 0);
	dlg_change(d, ST_RUNNING, 50000);
	if(type != DLG_HTML) current_type = type;
	hle_log("[Utility] dialogo %d omitido\n", type);
	RETURN(0);
}

static int generic_is(int type){ return type == DLG_HTML || current_type == type; }

static void generic_status(int type){
	if(!generic_is(type)){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(&dlgs[type]);
	RETURN((u32)dlgs[type].status);
}

static void generic_update(int type){
	Dialog *d = &dlgs[type];
	if(!generic_is(type)){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(d);
	if(d->status != ST_RUNNING){ RETURN(ERR_INVALID_STATUS); return; }
	if(++d->updates >= 2){
		if(type == DLG_OSK) osk_finish(d->addr);
		else if(mem_valid(d->addr + 0x1C, 4)) mem_write32(d->addr + 0x1C, RESULT_CANCEL);
		dlg_change(d, ST_FINISHED, 0);
	}
	RETURN(0);
}

static void generic_shutdown(int type){
	Dialog *d = &dlgs[type];
	if(!generic_is(type)){ RETURN(ERR_WRONG_TYPE); return; }
	dlg_update(d);
	if(d->status != ST_FINISHED){ RETURN(ERR_INVALID_STATUS); return; }
	dlg_change(d, ST_SHUTDOWN, 0);
	dlg_change(d, ST_NONE, 2000);
	RETURN(0);
}

#define GENERIC_DIALOG(name, type, sizes) \
	static void name##InitStart(void){ generic_init(type, sizes, (int)(sizeof(sizes) / sizeof(sizes[0]))); } \
	static void name##GetStatus(void){ generic_status(type); } \
	static void name##Update(void){ generic_update(type); } \
	static void name##ShutdownStart(void){ generic_shutdown(type); }

GENERIC_DIALOG(sceUtilityOsk, DLG_OSK, sizes_osk)
GENERIC_DIALOG(sceUtilityNetconf, DLG_NET, sizes_net)
GENERIC_DIALOG(sceUtilityNpSignin, DLG_NPSIGNIN, sizes_np)
GENERIC_DIALOG(sceUtilityScreenshot, DLG_SCREENSHOT, sizes_shot)
GENERIC_DIALOG(sceUtilityGamedataInstall, DLG_GAMEDATA, sizes_install)
GENERIC_DIALOG(sceUtilityGameSharing, DLG_GAMESHARING, sizes_share)

/* El navegador nunca arranca: sin su módulo, la PSP no tiene memoria para
   él (0x800200D9) y su estado siempre es WRONG_TYPE (utility/dialog/htmlviewer) */
static void sceUtilityHtmlViewerInitStart(void){
	u32 r = check_request(ARG(0), sizes_html, (int)(sizeof(sizes_html) / sizeof(sizes_html[0])));
	RETURN(r ? r : 0x800200D9u);
}
static void sceUtilityHtmlViewerGetStatus(void){ RETURN(ERR_WRONG_TYPE); }
static void sceUtilityHtmlViewerUpdate(void){ RETURN(ERR_WRONG_TYPE); }
static void sceUtilityHtmlViewerShutdownStart(void){ RETURN(ERR_WRONG_TYPE); }

#define GENERIC_ENTRIES(name) \
	{ #name "InitStart", name##InitStart }, { #name "GetStatus", name##GetStatus }, \
	{ #name "Update", name##Update }, { #name "ShutdownStart", name##ShutdownStart }

/* --- sceImpose: idioma, botón de confirmar y avisos del sistema (PPSSPP) ---- */

static u32 impose_language, impose_button, impose_umd_popup, impose_backlight_off, impose_home_popup;

static void impose_reset(void){
	impose_language = 1;   /* inglés, como sceUtilityGetSystemParamInt */
	impose_button = 1;     /* X confirma */
	impose_umd_popup = 0;
	impose_backlight_off = 0;
	impose_home_popup = 1;
}

static void sceImposeSetLanguageMode(void){
	impose_language = ARG(0);
	impose_button = ARG(1);
	RETURN(0);
}

static void sceImposeGetLanguageMode(void){
	if(mem_valid(ARG(0), 4)) mem_write32(ARG(0), impose_language);
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), impose_button);
	RETURN(0);
}

/* Sin cargar y con la carga en el último de 4 niveles */
static void sceImposeGetBatteryIconStatus(void){
	if(mem_valid(ARG(0), 4)) mem_write32(ARG(0), 0);
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), 3);
	RETURN(0);
}

static void sceImposeSetUMDPopup(void){ impose_umd_popup = ARG(0); RETURN(0); }
static void sceImposeGetUMDPopup(void){ RETURN(impose_umd_popup); }
static void sceImposeSetBacklightOffTime(void){ impose_backlight_off = ARG(0); RETURN(0); }
static void sceImposeGetBacklightOffTime(void){ RETURN(impose_backlight_off); }
static void sceImposeSetHomePopup(void){ impose_home_popup = ARG(0); RETURN(0); }
static void sceImposeGetHomePopup(void){ RETURN(impose_home_popup); }

static const HleFunction impose[] = {
	{ "sceImposeSetLanguageMode", sceImposeSetLanguageMode },
	{ "sceImposeGetLanguageMode", sceImposeGetLanguageMode },
	{ "sceImposeGetBatteryIconStatus", sceImposeGetBatteryIconStatus },
	{ "sceImposeSetUMDPopup", sceImposeSetUMDPopup },
	{ "sceImposeGetUMDPopup", sceImposeGetUMDPopup },
	{ "sceImposeSetBacklightOffTime", sceImposeSetBacklightOffTime },
	{ "sceImposeGetBacklightOffTime", sceImposeGetBacklightOffTime },
	{ "sceImposeSetHomePopup", sceImposeSetHomePopup },
	{ "sceImposeGetHomePopup", sceImposeGetHomePopup },
};

void utility_init(void){
	impose_reset();
	memset(mod_loaded, 0, sizeof(mod_loaded));
	memset(mod_addr, 0, sizeof(mod_addr));
	memset(dlgs, 0, sizeof(dlgs));
	memset(&msg, 0, sizeof(msg));
	memset(&save, 0, sizeof(save));
	current_type = DLG_NONE;
}

static const HleFunction utility[] = {
	{ "sceUtilityLoadModule", sceUtilityLoadModule },
	{ "sceUtilityUnloadModule", sceUtilityUnloadModule },
	{ "sceUtilityLoadAvModule", sceUtilityLoadAvModule },
	{ "sceUtilityUnloadAvModule", sceUtilityUnloadAvModule },
	{ "sceUtilityLoadNetModule", utility_zero },
	{ "sceUtilityUnloadNetModule", utility_zero },
	{ "sceUtilityLoadUsbModule", utility_zero },
	{ "sceUtilityUnloadUsbModule", utility_zero },
	{ "sceUtilityGetSystemParamInt", sceUtilityGetSystemParamInt },
	{ "sceUtilitySetSystemParamInt", sceUtilitySetSystemParamInt },
	{ "sceUtilityGetSystemParamString", sceUtilityGetSystemParamString },
	{ "sceUtilitySetSystemParamString", utility_zero },
	{ "sceUtilityMsgDialogInitStart", sceUtilityMsgDialogInitStart },
	{ "sceUtilityMsgDialogGetStatus", sceUtilityMsgDialogGetStatus },
	{ "sceUtilityMsgDialogUpdate", sceUtilityMsgDialogUpdate },
	{ "sceUtilityMsgDialogAbort", sceUtilityMsgDialogAbort },
	{ "sceUtilityMsgDialogShutdownStart", sceUtilityMsgDialogShutdownStart },
	{ "sceUtilitySavedataInitStart", sceUtilitySavedataInitStart },
	{ "sceUtilitySavedataGetStatus", sceUtilitySavedataGetStatus },
	{ "sceUtilitySavedataUpdate", sceUtilitySavedataUpdate },
	{ "sceUtilitySavedataShutdownStart", sceUtilitySavedataShutdownStart },
	GENERIC_ENTRIES(sceUtilityOsk),
	GENERIC_ENTRIES(sceUtilityNetconf),
	GENERIC_ENTRIES(sceUtilityNpSignin),
	GENERIC_ENTRIES(sceUtilityScreenshot),
	GENERIC_ENTRIES(sceUtilityGamedataInstall),
	GENERIC_ENTRIES(sceUtilityGameSharing),
	GENERIC_ENTRIES(sceUtilityHtmlViewer),
};

const HleLibrary hle_utility_libs[] = {
	HLE_LIBRARY("sceUtility", utility),
	HLE_LIBRARY("sceImpose", impose),
};
const u32 hle_utility_libs_count = sizeof(hle_utility_libs) / sizeof(hle_utility_libs[0]);
