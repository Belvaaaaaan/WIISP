/**
 * WIISP - module.c
 * HLE de ModuleMgrForUser: cargar, arrancar, parar y descargar módulos.
 *
 * Los juegos traen en el disco módulos del sistema (Atrac3+, MPEG, SAS,
 * red...) y los cargan al arrancar. Como PPSSPP, WIISP no los ejecuta: sus
 * funciones ya las atiende el HLE, así que se "cargan" sin más (lista de
 * nombres de HLE.cpp de PPSSPP). Los demás PRX se cargan de verdad: se
 * descifran, se relocalizan en memoria de usuario, sus imports van al HLE
 * y sus exportaciones se enlazan con los imports de los demás módulos.
 * module_start corre en un hilo nuevo mientras quien llamó espera.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include "hle/hle.h"
#include "core/memory.h"
#include "loader/loader.h"

#define MAX_MODULES 32
#define MAIN_MODULE_UID 0x04100001u
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE   0x8002012Eu
#define SCE_KERNEL_ERROR_ALREADY_STARTED  0x80020133u
#define SCE_KERNEL_ERROR_NOT_STARTED      0x80020134u
#define SCE_KERNEL_ERROR_ILLEGAL_OBJECT   0x8002012Fu
#define SCE_KERNEL_ERROR_FILEERR          0x8002013Cu

#define NID_MODULE_START 0xD632ACDBu
#define NID_MODULE_STOP  0xCEE8593Cu

typedef struct {
	int used, fake, started;
	char name[32];
	PspModule mod;
	u32 alloc;
} Module;

static Module modules[MAX_MODULES];

/* Módulos que el HLE sustituye (nombre del sceModuleInfo) */
static const char *const hle_modules[] = {
	"sceATRAC3plus_Library", "sceFont_Library", "SceFont_Library", "SceHttp_Library",
	"sceMpeg_library", "sceMp3_Library", "sceNetAdhocctl_Library", "sceNetAdhocDownload_Library",
	"sceNetAdhocMatching_Library", "sceNetApDialogDummy_Library", "sceNetAdhoc_Library",
	"sceNetApctl_Library", "sceNetInet_Library", "sceNetResolver_Library", "sceNet_Library",
	"sceNetAdhocAuth_Service", "sceNetIfhandle_Service", "sceSsl_Module", "sceMemab",
	"sceAvcodec_driver", "sceAudiocodec_Driver", "sceAudiocodec", "sceVideocodec_Driver",
	"sceVideocodec", "sceMpegbase_Driver", "sceMpegbase", "scePsmf_library", "scePsmfP_library",
	"scePsmfPlayer", "sceSAScore", "sceCcc_Library", "sceMp4_library", "mp4msv_module",
	"SceParseHTTPheader_Library", "SceParseURI_Library", "sceDEFLATE_Library",
	"sceADLER32_Library", "sceMD5_Library", "sceSHA256_Library", "sceMT19937_Library",
	"sceSfmt19937_Library", "sceHeap_Library", "sceJpeg", "sceAac_Library", "sceAtrac3plus_Library",
};

static int is_hle_module(const char *name){
	u32 i;
	for(i = 0; i < sizeof(hle_modules) / sizeof(hle_modules[0]); i++)
		if(!strcmp(name, hle_modules[i])) return 1;
	return 0;
}

static inline u32 module_uid(int i){ return 0x04200001u + (u32)i; }

static int find_module(u32 uid){
	u32 i = uid - 0x04200001u;
	return i < MAX_MODULES && modules[i].used ? (int)i : -1;
}

static int new_slot(void){
	int i;
	for(i = 0; i < MAX_MODULES && modules[i].used; i++);
	return i < MAX_MODULES ? i : -1;
}

static void free_module(int i){
	Module *m = &modules[i];
	if(!m->fake){
		hle_unlink_module(&m->mod);
		if(m->alloc) kernel_free(m->alloc);
		loader_free(&m->mod);
	}
	memset(m, 0, sizeof(*m));
}

void module_shutdown(void){
	int i;
	for(i = 0; i < MAX_MODULES; i++)
		if(modules[i].used){
			if(!modules[i].fake) loader_free(&modules[i].mod);
			memset(&modules[i], 0, sizeof(modules[i]));
		}
}

/* Carga buf (se modifica). Devuelve el UID o un error */
static u32 load_module(u8 *buf, u32 len, const char *what){
	char name[32];
	const u8 *elf;
	u32 elf_len, span;
	u8 *owned;
	int i = new_slot(), relocatable = 0, err;
	Module *m;
	if(i < 0) return SCE_KERNEL_ERROR_NO_MEMORY;
	m = &modules[i];
	memset(m, 0, sizeof(*m));

	loader_psp_name(buf, len, name, sizeof(name));
	if(name[0] && is_hle_module(name)){
		m->used = m->fake = 1;
		snprintf(m->name, sizeof(m->name), "%s", name);
		hle_log("[MOD] %s: %s lo sustituye el HLE\n", what, name);
		return module_uid(i);
	}

	err = loader_unwrap(buf, len, &elf, &elf_len, &owned);
	if(err){
		/* Cifrado con una clave desconocida (módulos del kernel): como
		   PPSSPP, se hace como si se cargara */
		m->used = m->fake = 1;
		snprintf(m->name, sizeof(m->name), "%s", name[0] ? name : "?");
		hle_log("[MOD] %s: no se pudo descifrar (%s); se finge cargado\n", what, loader_strerror(err));
		return module_uid(i);
	}
	span = loader_elf_span(elf, elf_len, &relocatable);
	if(!span){ free(owned); return SCE_KERNEL_ERROR_ILLEGAL_OBJECT; }
	if(relocatable){
		m->alloc = kernel_alloc(span, 0, "module");
		if(!m->alloc){ free(owned); return SCE_KERNEL_ERROR_NO_MEMORY; }
	}
	loader_syscall_base = hle_next_syscall();
	err = loader_load_elf(elf, elf_len, m->alloc, &m->mod);
	free(owned);
	if(err){
		if(m->alloc) kernel_free(m->alloc);
		memset(m, 0, sizeof(*m));
		hle_log("[MOD] %s: %s\n", what, loader_strerror(err));
		return SCE_KERNEL_ERROR_ILLEGAL_OBJECT;
	}
	memcpy(m->name, m->mod.name, sizeof(m->name) - 1);   /* mod.name: 28 + 0 */
	if(is_hle_module(m->name)){
		/* Sin cifrar pero sustituido igualmente */
		kernel_free(m->alloc);
		loader_free(&m->mod);
		m->alloc = 0;
		m->fake = 1;
		m->used = 1;
		hle_log("[MOD] %s: %s lo sustituye el HLE\n", what, m->name);
		return module_uid(i);
	}
	if(hle_link_module(&m->mod)){
		if(m->alloc) kernel_free(m->alloc);
		loader_free(&m->mod);
		memset(m, 0, sizeof(*m));
		return SCE_KERNEL_ERROR_NO_MEMORY;
	}
	m->used = 1;
	hle_log("[MOD] %s: %s en 0x%08X (%u imports)\n", what, m->name, m->mod.load_start, m->mod.num_imports);
	return module_uid(i);
}

static void sceKernelLoadModule(void){
	char path[256];
	u8 *buf;
	u32 len;
	if(!ARG(0) || mem_read_cstr(ARG(0), path, sizeof(path))){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	buf = io_load_path(path, &len);
	if(!buf){
		hle_log("[MOD] %s: no existe\n", path);
		RETURN(SCE_ERROR_FILE_NOT_FOUND);
		return;
	}
	RETURN(load_module(buf, len, path));
	free(buf);
}

static void sceKernelLoadModuleByID(void){
	u8 *buf;
	u32 len;
	char what[32];
	buf = io_load_fd(ARG(0), &len);
	if(!buf){ RETURN(0x80020323u /* BADF */); return; }
	snprintf(what, sizeof(what), "descriptor %u", ARG(0));
	RETURN(load_module(buf, len, what));
	free(buf);
}

static u32 module_export(const Module *m, u32 nid){
	u32 i;
	for(i = 0; i < m->mod.num_exports; i++)
		if(!m->mod.exports[i].lib[0] && m->mod.exports[i].nid == nid) return m->mod.exports[i].addr;
	return 0;
}

/* Lanza module_start o module_stop en un hilo; quien llama espera.
   Devuelve 0, o un error ya puesto en v0 */
static int run_entry(int i, u32 entry, u32 arglen, u32 argp, u32 status, u32 option){
	/* Primero lo que pide el módulo (module_start_thread_parameter), y la
	   opción de quien lo arranca manda sobre eso (PPSSPP) */
	const PspModule *mod = &modules[i].mod;
	u32 prio = mod->start_prio ? mod->start_prio : 0x20;
	u32 stack = mod->start_stack ? mod->start_stack : 0x40000;
	u32 attr = mod->start_attr, oattr = 0, tid;
	if(option && mem_valid(option, 20) && mem_read32(option) >= 20){
		if(mem_read32(option + 8)) stack = mem_read32(option + 8);
		if(mem_read32(option + 12)) prio = mem_read32(option + 12);
		oattr = mem_read32(option + 16);
	}
	/* Solo VFPU, 0x2000 y 0x00F00000; ni usuario ni kernel (modules/startoptions) */
	if(oattr & ~0x00F06000u){ RETURN(SCE_KERNEL_ERROR_ERROR); return -1; }
	attr = (attr | oattr) & 0x0FFFFFFFu;
	if(stack < 0x200){ RETURN(0x80020194u /* ILLEGAL_STACK_SIZE */); return -1; }
	tid = kernel_create_thread(modules[i].name, entry, prio, stack, attr, modules[i].mod.gp);
	if((s32)tid < 0 || kernel_start_thread(tid, arglen, argp)){
		RETURN(tid);
		return -1;
	}
	kernel_wait_module_start(tid, module_uid(i), status);
	return 0;
}

static void sceKernelStartModule(void){
	int i = find_module(ARG(0));
	u32 status = ARG(3), entry;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	if(modules[i].started){ RETURN(SCE_KERNEL_ERROR_ALREADY_STARTED); return; }
	entry = modules[i].fake ? 0 : module_export(&modules[i], NID_MODULE_START);
	if(!entry && !modules[i].fake) entry = modules[i].mod.entry;
	if(!entry || entry == 0xFFFFFFFFu || !mem_valid(entry, 4)){
		if(status && mem_valid(status, 4)) mem_write32(status, 0);
		modules[i].started = 1;
		RETURN(ARG(0));
		return;
	}
	if(!run_entry(i, entry, ARG(1), ARG(2), status, ARG(4))) modules[i].started = 1;
}

static void sceKernelStopModule(void){
	int i = find_module(ARG(0));
	u32 status = ARG(3), entry;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	if(!modules[i].started){ RETURN(SCE_KERNEL_ERROR_NOT_STARTED); return; }
	entry = modules[i].fake ? 0 : module_export(&modules[i], NID_MODULE_STOP);
	if(!entry || !mem_valid(entry, 4)){
		if(status && mem_valid(status, 4)) mem_write32(status, 0);
		modules[i].started = 0;
		RETURN(ARG(0));
		return;
	}
	if(!run_entry(i, entry, ARG(1), ARG(2), status, ARG(4))) modules[i].started = 0;
}

static void sceKernelUnloadModule(void){
	int i = find_module(ARG(0));
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	free_module(i);
	RETURN(ARG(0));
}

static void sceKernelGetModuleId(void){ RETURN(MAIN_MODULE_UID); }

static void sceKernelGetModuleIdByAddress(void){
	u32 addr = ARG(0);
	int i;
	for(i = 0; i < MAX_MODULES; i++)
		if(modules[i].used && !modules[i].fake && addr >= modules[i].mod.load_start && addr < modules[i].mod.load_end){
			RETURN(module_uid(i));
			return;
		}
	RETURN(MAIN_MODULE_UID);
}

static void sceKernelSelfStopUnloadModule(void){ hle_exit("el modulo se descargo"); }

static const HleFunction modulemgr_user[] = {
	{ "sceKernelLoadModule", sceKernelLoadModule },
	{ "sceKernelLoadModuleByID", sceKernelLoadModuleByID },
	{ "sceKernelStartModule", sceKernelStartModule },
	{ "sceKernelStopModule", sceKernelStopModule },
	{ "sceKernelUnloadModule", sceKernelUnloadModule },
	{ "sceKernelGetModuleId", sceKernelGetModuleId },
	{ "sceKernelGetModuleIdByAddress", sceKernelGetModuleIdByAddress },
	{ "sceKernelSelfStopUnloadModule", sceKernelSelfStopUnloadModule },
	{ "sceKernelStopUnloadSelfModule", sceKernelSelfStopUnloadModule },
};

const HleLibrary hle_module_libs[] = {
	HLE_LIBRARY("ModuleMgrForUser", modulemgr_user),
};
const u32 hle_module_libs_count = sizeof(hle_module_libs) / sizeof(hle_module_libs[0]);
