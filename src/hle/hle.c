/**
 * WIISP - hle.c
 * Despachador de syscalls, arranque del HLE e informe de imports.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit   /* el CLI para Windows */
#endif
#include "hle/hle.h"
#include "hle/nid_names.h"
#include "core/memory.h"
#include "gpu/ge.h"
#include "cpu/vfpu.h"
#include "core/prof.h"
#include "core/opt.h"
#include "core/tirones.h"
#include "core/registro.h"
#include "cpu/predec.h"

/* Todos los imports de todos los módulos cargados: el código del syscall
   de cada stub es su índice aquí */
typedef struct {
	HleFunc func;
	const char *name;  /* nombre del NID o NULL si es desconocido */
	int warned;
	char lib[LOADER_LIB_NAME_LEN];
	u32 nid, stub;
	int linked;        /* el stub salta a la función de otro módulo */
} ResolvedImport;

static const PspModule *module;
static ResolvedImport *resolved;
static u32 num_resolved, cap_resolved;
static PspExport *exports;     /* funciones exportadas por los módulos cargados */
static u32 num_exports, cap_exports;
static HleOutputFunc output_func;
static int exited;
static const char *exit_reason = "";

/* ------------------------------------------------------------------ */
/* NIDs                                                               */
/* ------------------------------------------------------------------ */

const char *nid_lookup(u32 nid){
	u32 lo = 0, hi = nid_names_count;
	while(lo < hi){
		u32 mid = (lo + hi) / 2;
		if(nid_names[mid].nid == nid) return nid_names[mid].name;
		if(nid_names[mid].nid < nid) lo = mid + 1;
		else hi = mid;
	}
	return NULL;
}

static const HleLibrary *find_library(const char *lib){
	static const struct { const HleLibrary *libs; const u32 *count; } groups[] = {
		{ hle_kernel_libs, &hle_kernel_libs_count },
		{ hle_io_libs, &hle_io_libs_count },
		{ hle_display_libs, &hle_display_libs_count },
		{ hle_misc_libs, &hle_misc_libs_count },
		{ hle_ge_libs, &hle_ge_libs_count },
		{ hle_module_libs, &hle_module_libs_count },
		{ hle_net_libs, &hle_net_libs_count },
		{ hle_audio_libs, &hle_audio_libs_count },
		{ hle_sas_libs, &hle_sas_libs_count },
		{ hle_atrac_libs, &hle_atrac_libs_count },
		{ hle_mpeg_libs, &hle_mpeg_libs_count },
		{ hle_utility_libs, &hle_utility_libs_count },
	};
	u32 g, i;
	for(g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
		for(i = 0; i < *groups[g].count; i++)
			if(!strcmp(groups[g].libs[i].lib, lib)) return &groups[g].libs[i];
	return NULL;
}

HleFunc hle_find(const char *lib, u32 nid, const char **name_out){
	const char *name = nid_lookup(nid);
	const HleLibrary *l;
	u32 i;
	if(name_out) *name_out = name;
	if(!name || !(l = find_library(lib))) return NULL;
	for(i = 0; i < l->count; i++)
		if(!strcmp(l->funcs[i].name, name)) return l->funcs[i].func;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Syscalls                                                           */
/* ------------------------------------------------------------------ */

static FILE *log_file;
static void log_file_only(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- Diagnóstico: traza del arranque y últimas llamadas ------------------- */
/* Las primeras llamadas al HLE van a wiisp.log con sus argumentos (y el
   texto de las rutas y nombres); las repeticiones de un bucle se resumen.
   Las últimas siempre quedan en un anillo que se vuelca si el juego se
   atasca, falla o lo detiene el usuario. */
#define TRACE_MAX   8000
#define RING_SIZE   96
typedef struct { const char *name; u32 a[4], ret; s8 th; u8 guest; u32 ms; } CallRec;
static CallRec ring[RING_SIZE];
static u32 ring_pos, trace_count, trace_skipped;
static CallRec trace_recent[6];

static int call_has_string(const char *n){
	return strstr(n, "Open") || strstr(n, "Dopen") || strstr(n, "stat") || strstr(n, "Mkdir") || strstr(n, "Rename") ||
	       strstr(n, "Remove") || strstr(n, "Chdir") || strstr(n, "Devctl") || strstr(n, "Create") ||
	       strstr(n, "LoadModule") || !strcmp(n, "sceKernelPrintf");
}

static void format_call(const CallRec *r, char *out, size_t cap){
	char str[96];
	int n;
	str[0] = 0;
	if(call_has_string(r->name) && r->a[0] && mem_read_cstr(r->a[0], str + 1, sizeof(str) - 3) == 0){
		size_t l;
		str[0] = '"';
		l = strlen(str);
		str[l] = '"'; str[l + 1] = 0;
	} else str[0] = 0;
	n = snprintf(out, cap, "h%d %s(", r->th, r->name);
	if(str[0]) n += snprintf(out + n, cap - (size_t)n, "%s, ", str);
	else n += snprintf(out + n, cap - (size_t)n, "%08X, ", r->a[0]);
	/* guest: el valor lo dará la función del juego que llama (kernel_enqueue_call) */
	if(r->guest) snprintf(out + n, cap - (size_t)n, "%08X, %08X, %08X) = (llama al juego) @%u ms", r->a[1], r->a[2], r->a[3], r->ms);
	else snprintf(out + n, cap - (size_t)n, "%08X, %08X, %08X) = %08X @%u ms", r->a[1], r->a[2], r->a[3], r->ret, r->ms);
}

static void trace_call(const CallRec *r){
	char line[256];
	int i;
	if(trace_count >= TRACE_MAX) return;
	/* Un bucle que repite las mismas llamadas no llena el registro */
	for(i = 0; i < 6; i++){
		const CallRec *p = &trace_recent[i];
		if(p->name == r->name && p->a[0] == r->a[0] && p->a[1] == r->a[1] && p->a[2] == r->a[2] && p->ret == r->ret &&
		   p->guest == r->guest){
			trace_skipped++;
			return;
		}
	}
	memmove(&trace_recent[1], &trace_recent[0], sizeof(trace_recent) - sizeof(trace_recent[0]));
	trace_recent[0] = *r;
	if(trace_skipped){
		log_file_only("[LLAMADA] (%u repetidas omitidas)\n", trace_skipped);
		trace_skipped = 0;
	}
	format_call(r, line, sizeof(line));
	log_file_only("[LLAMADA] %s\n", line);
	if(++trace_count == TRACE_MAX) log_file_only("[LLAMADA] fin de la traza del arranque (%u llamadas)\n", TRACE_MAX);
}

void hle_dump_state(const char *why){
	char line[256];
	u32 i;
	if(!module) return;
	hle_profile_report("hasta aqui");
	hle_log("[DIAGNOSTICO] %s\n", why);
	kernel_dump_state();
	hle_log("[DIAGNOSTICO] ultimas llamadas al HLE:\n");
	for(i = 0; i < RING_SIZE; i++){
		const CallRec *r = &ring[(ring_pos + i) % RING_SIZE];
		if(!r->name) continue;
		format_call(r, line, sizeof(line));
		hle_log("[ULTIMA] %s\n", line);
	}
	hle_log_sync();
}

void hle_syscall(u32 code){
	if(code == HLE_SYSCALL_THREAD_RETURN){
		kernel_thread_return();
		return;
	}
	if(code == HLE_SYSCALL_CALLBACK_RETURN){
		kernel_callback_return();
		return;
	}
	if(code == HLE_SYSCALL_THREAD_CB_RETURN){
		kernel_thread_cb_return();
		return;
	}
	if(code == HLE_SYSCALL_GUEST_CALL_RETURN){
		kernel_guest_call_return();
		return;
	}
	if(!module || code >= num_resolved){
		cpu_fault("syscall desconocido", cpu.pc - 4, code);
		return;
	}
	if(resolved[code].func){
		static int trace = -1;
		CallRec *r = &ring[ring_pos];
		if(trace < 0) trace = getenv("WIISP_TRACE_HLE") != NULL;
		if(trace) hle_log("[HLE] %s(%08X, %08X, %08X) @%llu\n", resolved[code].name, cpu.r[R_A0], cpu.r[R_A1], cpu.r[R_A2], (unsigned long long)cpu_cycles);
		r->name = resolved[code].name;
		r->a[0] = cpu.r[R_A0]; r->a[1] = cpu.r[R_A1]; r->a[2] = cpu.r[R_A2]; r->a[3] = cpu.r[R_A3];
		r->th = (s8)kernel_current_thread();
		r->ms = (u32)(cpu_cycles / (CYCLES_PER_US * 1000ull));
		kernel_note_hle_call(r->name);
		/* Avanza antes: la función puede llamar al juego, que llama al HLE */
		ring_pos = (ring_pos + 1) % RING_SIZE;
		{
			u32 enq = kernel_enqueue_count();
			int old = prof_switch(PROF_HLE);
			hle_stat_syscalls++;
			resolved[code].func();
			prof_switch(old);
			r->guest = kernel_enqueue_count() != enq;
		}
		r->ret = cpu.r[R_V0];
		if(log_file) trace_call(r);
		if(trace) hle_log("      -> %08X\n", cpu.r[R_V0]);
		return;
	}
	/* Una función conocida sin implementar devuelve 0; un NID que no
	   existe, "biblioteca sin enlazar" como la PSP (modules/unresolved) */
	if(!resolved[code].warned){
		resolved[code].warned = 1;
		hle_log("[HLE] sin implementar: %s::%s (NID 0x%08X), devuelve %s\n",
		        resolved[code].lib, resolved[code].name ? resolved[code].name : "?", resolved[code].nid,
		        resolved[code].name ? "0" : "8002013A");
	}
	RETURN(resolved[code].name ? 0 : 0x8002013Au);
}

u32 cpu_fault_pc, cpu_fault_npc, cpu_fault_count;

void cpu_fault(const char *what, u32 addr, u32 instr){
	char reason[160];
	cpu_fault_pc = cpu.pc;
	cpu_fault_npc = cpu.npc;
	cpu_fault_count++;
	snprintf(reason, sizeof(reason), "%s en 0x%08X (instr/valor 0x%08X, pc 0x%08X)",
	         what, addr, instr, cpu.pc);
	hle_log("[CPU] %s\n", reason);
	hle_dump_state("fallo de CPU");
	hle_exit("fallo de CPU");
}

/* ------------------------------------------------------------------ */
/* Salida, log y estado                                               */
/* ------------------------------------------------------------------ */

void hle_set_output(HleOutputFunc func){ output_func = func; }

static HleScreenshotFunc screenshot_func;
void hle_set_screenshot(HleScreenshotFunc func){ screenshot_func = func; }
void hle_screenshot(void){ if(screenshot_func) screenshot_func(); }

void hle_output(const char *text, u32 len){
	if(output_func) output_func(text, len);
	else fwrite(text, 1, len, stdout);
}

/* 2 MB por programa (así el segundo juego de una sesión también se ve) y
   8 MB en total */
#define LOG_FILE_MAX  (2u * 1024 * 1024)
#define LOG_TOTAL_MAX (8u * 1024 * 1024)
static u32 log_written, log_total;

/* registro_hilo: wiisp.log lo escribe otro hilo (core/registro.h) */
static int log_hilo(void){ return opt_on[OPT_REGISTRO_HILO] && reg_disponible(); }

void hle_set_log_file(const char *path){
	reg_archivo(NULL);   /* lo que quede en la cola, al archivo de antes */
	if(log_file) fclose(log_file);
	log_file = path ? fopen(path, "w") : NULL;
	reg_archivo(log_file);
	log_written = log_total = 0;
	if(log_file) fprintf(log_file, "[WIISP] version " WIISP_VERSION "\n");
}

void hle_reopen_log_file(const char *path){
	reg_archivo(NULL);
	if(log_file) fclose(log_file);
	log_file = path ? fopen(path, "a") : NULL;
	reg_archivo(log_file);
	log_written = 0;
}

void hle_log_new_program(void){ log_written = 0; }

/* En la SD (libfat) el tamaño del archivo solo se apunta al cerrarlo o con
   fsync: sin esto, si la sesión acaba apagando el Wii el registro queda
   vacío. Se sincroniza como mucho una vez por segundo, y siempre en los
   fallos, al terminar y con las estadísticas. */
void hle_log_sync(void){
	if(!log_file) return;
	if(reg_pendiente() || log_hilo()){
		reg_esperar(1);   /* espera: los fallos y el final tienen que quedar */
		return;
	}
	fflush(log_file);
	fsync(fileno(log_file));
}

static time_t last_sync;
static int unsynced;

/* registro_diferido: cada fsync en la SD para la emulación 10-40 ms. Las
   líneas de diagnóstico (lazy: [MANDO], el Broadway...) no lo piden: llegan
   a la tarjeta con la siguiente sincronización (una línea normal en otro
   segundo, el informe de [TIEMPOS] o log_tick como mucho a los 10 s). */
#define LOG_LAZY_SECONDS 10

/* Con el hilo: el texto, a la cola. Lo que decide el fsync es lo mismo que
   sin él; las líneas [CPU] (un fallo de la emulación) esperan a estar en la
   tarjeta, como antes. */
static void log_v_hilo(int lazy, const char *fmt, va_list ap){
	char buf[1024], *p = buf;
	int n, sync = 0;
	va_list ap2;
	va_copy(ap2, ap);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if(n >= (int)sizeof(buf)){
		p = malloc((size_t)n + 1);
		if(p) vsnprintf(p, (size_t)n + 1, fmt, ap2);
		else { p = buf; n = (int)sizeof(buf) - 1; }
	}
	va_end(ap2);
	if(n < 0) return;
	log_written += (u32)n;
	log_total += (u32)n;
	if(!lazy || !opt_on[OPT_REGISTRO_DIFERIDO]){
		time_t now = time(NULL);
		unsynced = 2;
		if(now != last_sync || !strncmp(fmt, "[CPU]", 5)){
			last_sync = now;
			sync = 1;
			unsynced = 0;
		}
	} else if(!unsynced) unsynced = 1;
	reg_poner(log_file, p, (u32)n, 0);
	if(log_written >= LOG_FILE_MAX || log_total >= LOG_TOTAL_MAX){
		static const char recortado[] = "[registro recortado: el siguiente programa vuelve a escribir]\n";
		reg_poner(log_file, recortado, sizeof(recortado) - 1, 0);
	}
	if(p != buf) free(p);
	if(sync){
		if(!strncmp(fmt, "[CPU]", 5)) reg_esperar(1);
		else reg_pedir_fsync();
	}
}

static int log_screen = 1;
void hle_set_log_screen(int on){ log_screen = on; }

static void log_v(int to_screen, int lazy, const char *fmt, va_list ap){
	va_list ap2;
	int old = prof_switch(PROF_REGISTRO);
	va_copy(ap2, ap);
	if(to_screen && (log_screen || !strncmp(fmt, "[CPU]", 5))) vfprintf(stderr, fmt, ap);
	if(log_file && log_written < LOG_FILE_MAX && log_total < LOG_TOTAL_MAX && log_hilo()){
		log_v_hilo(lazy, fmt, ap2);
	} else if(log_file && log_written < LOG_FILE_MAX && log_total < LOG_TOTAL_MAX){
		int n;
		reg_esperar(0);   /* si se apagó el hilo, lo de la cola va antes */
		n = vfprintf(log_file, fmt, ap2);
		if(n > 0){ log_written += (u32)n; log_total += (u32)n; }
		if(log_written >= LOG_FILE_MAX || log_total >= LOG_TOTAL_MAX)
			fputs("[registro recortado: el siguiente programa vuelve a escribir]\n", log_file);
		fflush(log_file);
		if(!lazy || !opt_on[OPT_REGISTRO_DIFERIDO]){
			time_t now = time(NULL);
			unsynced = 2;
			if(now != last_sync || !strncmp(fmt, "[CPU]", 5)){
				last_sync = now;
				fsync(fileno(log_file));
				unsynced = 0;
			}
		} else if(!unsynced) unsynced = 1;
	}
	va_end(ap2);
	prof_switch(old);
}

void hle_log(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	log_v(1, 0, fmt, ap);
	va_end(ap);
}

/* Solo a wiisp.log: la traza de llamadas. En la pantalla del Wii cada línea
   desplaza toda la consola y el emulador pasaba más tiempo escribiendo que
   ejecutando el juego (GTA saltando sus videos). */
static void log_file_only(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	log_v(0, 0, fmt, ap);
	va_end(ap);
}

void hle_log_quiet(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	log_v(0, 1, fmt, ap);
	va_end(ap);
}

/* Una vez por frame: lo escrito en el último segundo llega a la tarjeta
   aunque el juego deje de llamar al HLE (si no, se perdía al apagar) */
static void log_tick(void){
	time_t now;
	if(!log_file || !unsynced) return;
	now = time(NULL);
	/* 2: hay una línea normal pendiente (a la tarjeta en un segundo); 1:
	   solo de diagnóstico (a los 10 s) */
	if(now != last_sync && (unsynced == 2 || !opt_on[OPT_REGISTRO_DIFERIDO] || now - last_sync >= LOG_LAZY_SECONDS)){
		int old = prof_switch(PROF_REGISTRO);
		last_sync = now;
		if(log_hilo()) reg_pedir_fsync();
		else {
			reg_esperar(0);
			fsync(fileno(log_file));
		}
		unsynced = 0;
		prof_switch(old);
	}
}

int hle_has_exited(void){ return exited; }
const char *hle_exit_reason(void){ return exit_reason; }

static u64 stats_base_instr, stats_logged_instr, stall_last_instr;

/* --- Desglose del tiempo real ([TIEMPOS]) ------------------------------ */

u64 hle_stat_syscalls, hle_stat_flips;
static void (*profile_hook)(char *buf, size_t size);
static struct {
	int valid;
	u64 clock, acc[PROF_N], instr, cycles, syscalls, flips, switches, guest_calls;
	u32 vblanks;
	RegStats reg;
	IoCenso es;
	PredecStats pd;
	u32 samples[PROF_N], ge_samples[GEF_N];
	u32 cpu_samples[CPF_N];   //arreglo
	u32 vtx_samples[VXF_N];   //Arreglo
	u64 pmc[PROF_PMC_SLOTS][PMC_N];
	GeStats ge;
} prof_base;
static u32 prof_vblanks;   /* hle_run_frame lleva la cuenta */
static u32 ventana_n = 600;   //Arreglo
static u64 ventana_fin;   //Arreglo

void hle_set_ventana_juego(u32 vblanks){ if(vblanks) ventana_n = vblanks; }   //Arreglo

void hle_set_profile_hook(void (*fn)(char *buf, size_t size)){ profile_hook = fn; }

/* prof_tirones (core/tirones.c): cada imagen del juego (de un cambio de
   framebuffer al siguiente, o TIR_VENTANA vblanks si no cambia) con su
   tiempo real, su tiempo de juego y lo que se llevó cada cubeta. Lo que
   tarda escribir el propio informe no cuenta. */
static struct {
	int valid;
	u64 last, last_acc[PROF_N], cycles, flips;
	u32 vblanks;
	Tirones t;
} tir;

static void tirones_tick(void){
	if(!prof_clock || !prof_hz) return;
	prof_flush();
	if(!tir.valid){
		tir.valid = 1;
		tir.last = prof_last;
		tir.cycles = cpu_cycles;
		tir.flips = hle_stat_flips;
		tir.vblanks = 0;
		memcpy(tir.last_acc, prof_acc, sizeof(tir.last_acc));
		return;
	}
	tir.vblanks++;
	if(hle_stat_flips != tir.flips || tir.vblanks >= TIR_VENTANA){
		u64 acc[PROF_N];
		int i;
		for(i = 0; i < PROF_N; i++) acc[i] = prof_acc[i] - tir.last_acc[i];
		tir_imagen(&tir.t, (double)(prof_last - tir.last) / (double)prof_hz, (double)(cpu_cycles - tir.cycles) / (double)PSP_CPU_HZ,
		           acc, (double)cpu_cycles / (double)PSP_CPU_HZ);
		tir.last = prof_last;
		tir.cycles = cpu_cycles;
		tir.flips = hle_stat_flips;
		tir.vblanks = 0;
		memcpy(tir.last_acc, prof_acc, sizeof(tir.last_acc));
	}
}

static void profile_snapshot(void){
	prof_flush();
	prof_base.valid = 1;
	prof_base.clock = prof_last;
	memcpy(prof_base.acc, prof_acc, sizeof(prof_acc));
	prof_base.instr = cpu_executed;
	prof_base.cycles = cpu_cycles;
	prof_base.syscalls = hle_stat_syscalls;
	prof_base.flips = hle_stat_flips;
	prof_base.switches = kernel_stat_switches;
	prof_base.guest_calls = kernel_stat_guest_calls;
	prof_base.vblanks = prof_vblanks;
	reg_stats(&prof_base.reg);
	io_censo(&prof_base.es);
	cpu_predec_stats(&prof_base.pd);
	memcpy(prof_base.samples, (const void *)prof_samples, sizeof(prof_base.samples));
	memcpy(prof_base.ge_samples, (const void *)prof_ge_samples, sizeof(prof_base.ge_samples));
	memcpy(prof_base.cpu_samples, (const void *)prof_cpu_samples, sizeof(prof_base.cpu_samples));   //arreglo
	memcpy(prof_base.vtx_samples, (const void *)prof_vtx_samples, sizeof(prof_base.vtx_samples));   //Arreglo
	if(prof_pmc_on) prof_pmc_account();
	memcpy(prof_base.pmc, prof_pmc_acc, sizeof(prof_base.pmc));
	ge_get_stats(&prof_base.ge);
	ventana_fin = (cpu_cycles / CYCLES_PER_FRAME / ventana_n + 1) * ventana_n;   //Arreglo
}

static double pct(u32 part, u32 whole){ return whole ? 100.0 * (double)part / (double)whole : 0.0; }

/* Lo que hizo el GE: llamadas de dibujo, vértices y triángulos */
static void profile_ge_lines(const GeStats *g, const GeStats *b, u32 vblanks){
	static const char *const fnames[GEF_N] = {
		"comandos", "leer vertices", "transformar y luces", "ensamblar triangulos", "enviar a GX",
		"preparar estado", "curvas"
	};
	char line[1024];
	size_t len;
	u32 st = 0, draws = g->draws - b->draws, verts = g->vertices - b->vertices, tris = g->tris - b->tris;
	u32 lit = g->vertices_lit - b->vertices_lit;
	double per = vblanks ? (double)vblanks : 1.0;
	int i;

	/* Muestras: cuánto del tiempo real se fue en cada fase del GE */
	for(i = 0; i < PROF_N; i++) st += prof_samples[i] - prof_base.samples[i];
	if(st){
		len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] GE por dentro:");
		for(i = 0; i < GEF_N && len < sizeof(line) - 48; i++)
			len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%%", fnames[i],
			                        pct(prof_ge_samples[i] - prof_base.ge_samples[i], st));
		if(len < sizeof(line) - 32) snprintf(line + len, sizeof(line) - len, " (%u muestras)", (unsigned)st);
		hle_log("%s\n", line);
	}
	if(opt_on[OPT_PROF_VERTICES] && st){   //Arreglo
		static const char *const vn[VXF_N] = { "preparar", "decodificar", "huesos", "transformar", "pantalla",
		                                       "luces uv y niebla", "camino generico" };
		u32 sv = 0;
		for(i = 0; i < VXF_N; i++) sv += prof_vtx_samples[i] - prof_base.vtx_samples[i];
		len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] Leer vertices por dentro (prof_vertices, %u muestras, %% del tiempo total):",
		                       (unsigned)sv);
		for(i = 0; i < VXF_N && len < sizeof(line) - 48; i++)
			len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%%", vn[i],
			                        pct(prof_vtx_samples[i] - prof_base.vtx_samples[i], st));
		hle_log("%s\n", line);
	}   //Arreglo
	hle_log("[TIEMPOS] Dibujo: %u llamadas (%.0f por vblank; %.0f%% con indices, %.0f%% reutilizan vertices); "
	        "%.0f%% listas de triangulos, %.0f%% tiras, %.0f%% abanicos, %.0f%% sprites, %.0f%% lineas y puntos; "
	        "%u curvas\n",
	        (unsigned)draws, draws / per, pct(g->draws_indexed - b->draws_indexed, draws),
	        pct(g->draws_reuse - b->draws_reuse, draws),
	        pct(g->draws_prim[3] - b->draws_prim[3], draws), pct(g->draws_prim[4] - b->draws_prim[4], draws),
	        pct(g->draws_prim[5] - b->draws_prim[5], draws), pct(g->draws_prim[6] - b->draws_prim[6], draws),
	        pct((g->draws_prim[0] - b->draws_prim[0]) + (g->draws_prim[1] - b->draws_prim[1]) +
	            (g->draws_prim[2] - b->draws_prim[2]), draws),
	        (unsigned)(g->curves - b->curves));
	hle_log("[TIEMPOS] Vertices: %u pedidos, %u calculados (%.0f por vblank): %.0f%% con huesos, %.0f%% con morph, "
	        "%.0f%% con luces (%.1f de media), %.0f%% en 2D\n",
	        (unsigned)(g->vertex_reads - b->vertex_reads), (unsigned)verts, verts / per,
	        pct(g->vertices_skinned - b->vertices_skinned, verts), pct(g->vertices_morph - b->vertices_morph, verts),
	        pct(lit, verts), lit ? (double)(g->lights - b->lights) / (double)lit : 0.0,
	        pct(g->vertices_through - b->vertices_through, verts));
	hle_log("[TIEMPOS] Triangulos: %u recibidos (%.0f por vblank): %.0f%% dibujados, %.0f%% de espaldas o sin area, "
	        "%.0f%% fuera de pantalla; %u recortados; %u sprites; %u primitivas dibujadas en total; "
	        "%.0f%% sin descarte de caras\n",
	        (unsigned)tris, tris / per, pct(g->tris_drawn - b->tris_drawn, tris), pct(g->tris_back - b->tris_back, tris),
	        pct(g->tris_outside - b->tris_outside, tris), (unsigned)(g->tris_clipped - b->tris_clipped),
	        (unsigned)(g->sprites - b->sprites), (unsigned)(g->primitives - b->primitives),
	        pct(g->tris_cull_off - b->tris_cull_off, tris));
	{
		u32 mc = g->mtx_combined - b->mtx_combined, mr = g->mtx_reused - b->mtx_reused;
		hle_log("[TIEMPOS] Matrices mundo*vista*proyeccion: %u calculadas, %u reutilizadas (ge_mtx_cache, %.0f%%)\n",
		        (unsigned)mc, (unsigned)mr, pct(mr, mc + mr));
	}
	ge_vtype_census(line, sizeof(line));
	hle_log("[TIEMPOS] %s\n", line);
	if(opt_on[OPT_GE_VERIF]){
		ge_vdec_verif_report(line, sizeof(line));
		hle_log("[TIEMPOS] %s\n", line);
	}
	if(opt_on[OPT_GE_ESTADO_SUCIO] && g->es_gx - b->es_gx + g->es_tri - b->es_tri){
		/* Con ge_verificar el estado de GX se emite siempre: lo que se habría saltado son las emisiones comprobadas */
		u32 salt = opt_on[OPT_GE_VERIF] ? (g->es_gx_verif - b->es_gx_verif) + (g->es_gx_verif_sin - b->es_gx_verif_sin)
		                                : g->es_gx_saltado - b->es_gx_saltado;
		hle_log("[TIEMPOS] Estado sucio (ge_estado_sucio): estado de GX que poner %u veces, %.0f%% %s (el mismo); "
		        "triangulos a GX %u, %.0f%% sin preparar nada\n", (unsigned)(g->es_gx - b->es_gx),
		        pct(salt, g->es_gx - b->es_gx), opt_on[OPT_GE_VERIF] ? "se habria saltado (con ge_verificar se emite igual)" : "sin emitir",
		        (unsigned)(g->es_tri - b->es_tri), pct(g->es_tri_listo - b->es_tri_listo, g->es_tri - b->es_tri));
	}
	if(opt_on[OPT_GE_CHAROLAS] && g->ch_llamadas != b->ch_llamadas)   //Arreglo
		hle_log("[TIEMPOS] Charolas (ge_charolas): %u llamadas, %.0f%% %s la anterior (destinos, textura y estado de GX "
		        "sin rehacer); sin charola abierta %u, con otra clave %u, textura que volver a buscar %u\n",
		        (unsigned)(g->ch_llamadas - b->ch_llamadas),
		        pct(opt_on[OPT_GE_VERIF] ? g->chv_llamadas - b->chv_llamadas : g->ch_siguen - b->ch_siguen, g->ch_llamadas - b->ch_llamadas),
		        opt_on[OPT_GE_VERIF] ? "habrian seguido (con ge_verificar se rehace igual y se compara)" : "siguieron",
		        (unsigned)(g->ch_sin - b->ch_sin), (unsigned)(g->ch_clave - b->ch_clave), (unsigned)(g->ch_textura - b->ch_textura));   //Arreglo
	if(opt_on[OPT_GE_CHAROLAS_ENVIO] && g->ce_tandas != b->ce_tandas)   //Arreglo
		hle_log("[TIEMPOS] Envio por charolas (ge_charolas_envio): %u tandas, %u triangulos con un GX_Begin por tanda (%.1f de media) "
		        "y %u por el camino de cada triangulo\n", (unsigned)(g->ce_tandas - b->ce_tandas), (unsigned)(g->ce_juntos - b->ce_juntos),
		        g->ce_tandas != b->ce_tandas ? (double)(g->ce_juntos - b->ce_juntos) / (double)(g->ce_tandas - b->ce_tandas) : 0.0,
		        (unsigned)(g->ce_sueltos - b->ce_sueltos));   //Arreglo
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_CHAROLAS])   //Arreglo
		hle_log("[TIEMPOS] Verificacion de las charolas (ge_verificar): %u llamadas que habrian seguido la anterior comparadas, "
		        "%u distintas\n", (unsigned)(g->chv_llamadas - b->chv_llamadas), (unsigned)(g->chv_malas - b->chv_malas));   //Arreglo
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_ESTADO_SUCIO])
		hle_log("[TIEMPOS] Verificacion del estado de GX (ge_verificar): %u emisiones que se habrian saltado comparadas con "
		        "libogc, %u distintas (%u sin poder leer libogc); triangulos sin preparar %u comparados, %u distintos\n",
		        (unsigned)(g->es_gx_verif - b->es_gx_verif), (unsigned)(g->es_gx_verif_bad - b->es_gx_verif_bad),
		        (unsigned)(g->es_gx_verif_sin - b->es_gx_verif_sin), (unsigned)(g->es_tri_verif - b->es_tri_verif),
		        (unsigned)(g->es_tri_verif_bad - b->es_tri_verif_bad));
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_CMD_PUNTERO])   //Arreglo
		hle_log("[TIEMPOS] Verificacion de los comandos (ge_verificar): %u comandos sin caso propio leidos con el puntero y "
		        "comparados con execute, %u distintos\n", (unsigned)(g->cp_verif - b->cp_verif), (unsigned)(g->cp_verif_bad - b->cp_verif_bad));
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_CMD_PUNTERO] && opt_on[OPT_GE_CMD_MATRICES])   //Arreglo
		hle_log("[TIEMPOS] Verificacion de las matrices (ge_verificar): %u datos y numeros de matriz por el atajo comparados con "
		        "execute, %u distintos\n", (unsigned)(g->cm_verif - b->cm_verif), (unsigned)(g->cm_verif_bad - b->cm_verif_bad));   //Arreglo
	if(opt_on[OPT_GE_CMD_PUNTERO] && opt_on[OPT_GE_CMD_HUESOS])   //Arreglo
		hle_log("[TIEMPOS] Rachas de huesos (ge_cmd_huesos): %u rachas con %u comandos por el atajo\n", (unsigned)(g->ch_rachas - b->ch_rachas),
		        (unsigned)(g->ch_cmds - b->ch_cmds));   //Arreglo
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_CMD_PUNTERO] && opt_on[OPT_GE_CMD_HUESOS])   //Arreglo
		hle_log("[TIEMPOS] Verificacion de las rachas de huesos (ge_verificar): %u rachas con %u comandos por el atajo comparadas con "
		        "execute, %u distintas\n", (unsigned)(g->ch_verif - b->ch_verif), (unsigned)(g->ch_verif_cmds - b->ch_verif_cmds),
		        (unsigned)(g->ch_verif_bad - b->ch_verif_bad));   //Arreglo
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_BBOX_RAPIDO])   //Arreglo
		hle_log("[TIEMPOS] Verificacion de las cajas (ge_verificar): %u BBOX rapidas comparadas con la prueba de siempre, %u distintas\n",
		        (unsigned)(g->bbv_cajas - b->bbv_cajas), (unsigned)(g->bbv_malas - b->bbv_malas));   //Arreglo
	if(opt_on[OPT_GE_BBOX])   //Arreglo
		hle_log("[TIEMPOS] Cajas exactas (ge_bbox): %u cajas probadas, %u enteras fuera de un plano (%u detras de la camara); "
		        "llamadas tras una caja fuera o dudosa: %u, saltadas %u (%u sin calcular lo que deja para continuar; %u vertices); no saltadas: "
		        "formato o primitiva %u, matrices o pantalla cambiadas %u, "
		        "algun vertice fuera de la caja %u; cajas fuera de la pantalla %u, vistas por su primer vertice %u, dentro %u, dudosas %u "
		        "(%u cruzan el plano de la camara); por su caja (ge_bbox_llamada): %u llamadas probadas (%u vistas por su primer vertice), "
		        "%u fuera (%u por la pantalla), saltadas %u con %u vertices; por la banda de guarda (ge_bbox_guarda): %u cajas y %u llamadas "   //Arreglo
		        "fuera (%u probadas); por sus triangulos (ge_bbox_triangulos): %u llamadas fuera (%u probadas)%s\n",   //Arreglo
		        (unsigned)(g->bx_cajas - b->bx_cajas), (unsigned)(g->bx_fuera - b->bx_fuera), (unsigned)(g->bx_detras - b->bx_detras),
		        (unsigned)(g->bx_llamadas - b->bx_llamadas), (unsigned)(g->bx_saltadas - b->bx_saltadas),
		        (unsigned)(g->bx_ligeras - b->bx_ligeras), (unsigned)(g->bx_vertices - b->bx_vertices), (unsigned)(g->bx_formato - b->bx_formato),
		        (unsigned)(g->bx_matrices - b->bx_matrices), (unsigned)(g->bx_fuera_caja - b->bx_fuera_caja),
		        (unsigned)(g->bx_pantalla - b->bx_pantalla), (unsigned)(g->bx_vistas - b->bx_vistas), (unsigned)(g->bx_dentro - b->bx_dentro),   //Arreglo
		        (unsigned)(g->bx_dudosas - b->bx_dudosas), (unsigned)(g->bx_cruzan - b->bx_cruzan),   //Arreglo
		        (unsigned)(g->bx_ll_probadas - b->bx_ll_probadas), (unsigned)(g->bx_ll_vista - b->bx_ll_vista),   //Arreglo
		        (unsigned)(g->bx_ll_fuera - b->bx_ll_fuera),   //Arreglo
		        (unsigned)(g->bx_ll_pantalla - b->bx_ll_pantalla), (unsigned)(g->bx_ll_saltadas - b->bx_ll_saltadas),   //Arreglo
		        (unsigned)(g->bx_ll_vertices - b->bx_ll_vertices),   //Arreglo
		        (unsigned)(g->bx_guarda - b->bx_guarda), (unsigned)(g->bx_ll_guarda - b->bx_ll_guarda),   //Arreglo
		        (unsigned)(g->bx_g_probadas - b->bx_g_probadas),   //Arreglo
		        (unsigned)(g->bx_ll_tri - b->bx_ll_tri), (unsigned)(g->bx_t_probadas - b->bx_t_probadas),   //Arreglo
		        opt_on[OPT_GE_VERIF] ? " (con ge_verificar se dibujan igual)" : "");   //Arreglo
	if(opt_on[OPT_GE_VERIF] && opt_on[OPT_GE_BBOX])   //Arreglo
		hle_log("[TIEMPOS] Verificacion de ge_bbox (ge_verificar): %u llamadas que se habrian saltado, dibujadas y comparadas, %u distintas; "
		        "matrices y pantalla guardadas para las cajas distintas de las de ahora: %u\n",   //Arreglo
		        (unsigned)(g->bx_verif - b->bx_verif), (unsigned)(g->bx_verif_bad - b->bx_verif_bad),
		        (unsigned)(g->bx_cache_mal - b->bx_cache_mal));   //Arreglo
	if(opt_on[OPT_PROF_HUESOS]){   //Arreglo
		u32 hd = g->hu_datos - b->hu_datos, hm = g->hu_mat - b->hu_mat, hr = g->hu_rachas - b->hu_rachas;   //Arreglo
		u32 hl = g->hu_llamadas - b->hu_llamadas;   //Arreglo
		hle_log("[TIEMPOS] Huesos (prof_huesos): %u datos de matrices de huesos (%u iguales al que ya habia, %.1f%%), %u matrices "
		        "completas (%u iguales, %.1f%%), %u rachas de numeros y datos de huesos seguidos (%.1f datos de media, %u de 12 o mas), "
		        "%u llamadas con huesos (%.1f datos por llamada)\n", (unsigned)hd, (unsigned)(g->hu_iguales - b->hu_iguales),
		        hd ? 100.0 * (double)(g->hu_iguales - b->hu_iguales) / (double)hd : 0.0, (unsigned)hm,
		        (unsigned)(g->hu_mat_iguales - b->hu_mat_iguales), hm ? 100.0 * (double)(g->hu_mat_iguales - b->hu_mat_iguales) / (double)hm : 0.0,
		        (unsigned)hr, hr ? (double)(g->hu_racha_datos - b->hu_racha_datos) / (double)hr : 0.0,
		        (unsigned)(g->hu_rachas_largas - b->hu_rachas_largas), (unsigned)hl, hl ? (double)hd / (double)hl : 0.0);   //Arreglo
	}   //Arreglo
	if(opt_on[OPT_PROF_REPETIDOS]){   //Arreglo
		u32 rl = g->rp_llamadas - b->rp_llamadas, rv = g->rp_vertices - b->rp_vertices, rc = g->rp_cuadros - b->rp_cuadros;   //Arreglo
#define RP(x) (unsigned)(g->x - b->x)   //Arreglo
#define RPP(x) (rv ? 100.0 * (double)(g->x - b->x) / (double)rv : 0.0)   //Arreglo
		hle_log("[TIEMPOS] Repetidas (prof_repetidos): %u cuadros, %u llamadas dibujadas (%.0f por cuadro), %u vertices (%u con huesos); "
		        "iguales a otra del mismo cuadro (datos, huesos, matrices y estado de la transformacion): %u llamadas, %u vertices "
		        "(%.1f%%; %u con huesos); mismos datos y huesos con otras matrices o estado: %u llamadas, %u vertices (%.1f%%; %u con huesos); "
		        "solo los mismos datos: %u llamadas, %u vertices (%.1f%%); iguales a una del cuadro anterior: %u llamadas, "
		        "%u vertices (%.1f%%); tabla llena %u\n", (unsigned)rc, (unsigned)rl, rc ? (double)rl / (double)rc : 0.0,
		        (unsigned)rv, RP(rp_vert_huesos), RP(rp_igual_ll), RP(rp_igual_v), RPP(rp_igual_v), RP(rp_igual_vh),
		        RP(rp_huesos_ll), RP(rp_huesos_v), RPP(rp_huesos_v), RP(rp_huesos_vh), RP(rp_datos_ll), RP(rp_datos_v), RPP(rp_datos_v),
		        RP(rp_anterior_ll), RP(rp_anterior_v), RPP(rp_anterior_v), RP(rp_lleno));   //Arreglo
#undef RP
#undef RPP
		{
			static char rr[1024];
			ge_repetidas_report(rr, sizeof(rr));
			hle_log("[TIEMPOS] %s\n", rr);
		}
	}   //Arreglo
	if(opt_on[OPT_PROF_COMANDOS]){   //Arreglo
		char cl[512];   //Arreglo
		ge_cmd_census_report(cl, sizeof(cl));   //Arreglo
		hle_log("[TIEMPOS] %s\n", cl);   //Arreglo
	}   //Arreglo
	if(opt_on[OPT_PROF_ESTADO]){
		static char cs[1536];
		char *p = cs;
		ge_state_census_report(g, b, cs, sizeof(cs));
		while(*p){
			char *nl = strchr(p, '\n');
			if(nl) *nl = 0;
			hle_log("[TIEMPOS] %s\n", p);
			if(!nl) break;
			p = nl + 1;
		}
	}
	if(opt_on[OPT_PROF_BBOX]){   //Arreglo
		static char cb[4096];   //Arreglo
		char *p = cb;
		ge_bbox_census_report(g, b, prof_hz, cb, sizeof(cb));
		while(*p){
			char *nl = strchr(p, '\n');
			if(nl) *nl = 0;
			hle_log("[TIEMPOS] %s\n", p);
			if(!nl) break;
			p = nl + 1;
		}
	}   //Arreglo
}

/* prof_pmc (0.2): ciclos del procesador por cubeta (con IPC, fallos de la
   L1 de datos y saltos mal predichos por cada 1000 instrucciones) y lo que
   cuesta cada fase del GE por unidad */
static void profile_pmc_lines(const GeStats *g, const GeStats *b, u64 instr){
	static const char *const bn[PROF_N] = {
		"otros", "CPU", "syscalls", "GE", "rasterizar", "texturas", "framebuffers", "presentar", "esperar GX", "E/S",
		"audio", "registro"
	};
	static const char *const fn[GEF_N] = { "comandos", "leer", "transformar", "ensamblar", "enviar", "estado", "curvas" };
	//arreglo
	/* Lo que cuentan el tercero y el cuarto según prof_pmc_modo; los de
	   ciclos salen como parte de los ciclos, los demás por 1000 instr */
	static const char *const e3[4] = { "L1D", "fallos L2D", "ciclos esperando CTR", "ciclos de fallos de carga" };
	static const char *const e4[4] = { "saltos", "expulsiones L2", "saltos", "saltos" };
	const int m = (unsigned)prof_pmc_modo < 4 ? prof_pmc_modo : 0, e3_ciclos = m >= 2;
	u64 d[PROF_PMC_SLOTS][PMC_N], total = 0, ge_c = 0;
	char line[1024];
	size_t len;
	int i, k;
	u32 draws = g->draws - b->draws, verts = g->vertices - b->vertices, tris = g->tris - b->tris;
	u32 prims = g->primitives - b->primitives, cmds = g->commands - b->commands;
	if(prof_pmc_on) prof_pmc_account();
	for(i = 0; i < PROF_PMC_SLOTS; i++)
		for(k = 0; k < PMC_N; k++){
			d[i][k] = prof_pmc_acc[i][k] - prof_base.pmc[i][k];
			if(k == PMC_CICLOS) total += d[i][k];
		}
	if(!total) return;
	for(i = 0; i < GEF_N; i++) ge_c += d[PROF_N + i][PMC_CICLOS];
	len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] PMC: %.0f M ciclos;", (double)total / 1e6);
	for(i = 0; i < PROF_PMC_SLOTS && len + 96 < sizeof(line); i++){
		u64 c = d[i][PMC_CICLOS], n = d[i][PMC_INSTR];
		if(i == PROF_GE){
			/* El GE va por fases: aquí su suma */
			u64 gi = 0;
			for(k = 0; k < GEF_N; k++) gi += d[PROF_N + k][PMC_INSTR];
			c = ge_c; n = gi;
		} else if(i >= PROF_N) break;
		if(c * 200 < total) continue;   /* menos del 0,5 % */
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%% (IPC %.2f", bn[i], 100.0 * (double)c / (double)total,
		                        c ? (double)n / (double)c : 0.0);
		if(i != PROF_GE && n){   //arreglo
			if(e3_ciclos)
				len += (size_t)snprintf(line + len, sizeof(line) - len, ", %s %.1f%%", e3[m],
				                        c ? 100.0 * (double)d[i][PMC_FALLOS_L1D] / (double)c : 0.0);
			else
				len += (size_t)snprintf(line + len, sizeof(line) - len, ", %s %.2f", e3[m],
				                        1000.0 * (double)d[i][PMC_FALLOS_L1D] / (double)n);
			len += (size_t)snprintf(line + len, sizeof(line) - len, ", %s %.2f", e4[m],
			                        1000.0 * (double)d[i][PMC_SALTOS_MAL] / (double)n);
		}
		len += (size_t)snprintf(line + len, sizeof(line) - len, ")");
	}
	hle_log("%s\n", line);
	if(instr)   //arreglo
		hle_log("[TIEMPOS] PMC de la CPU: %.1f ciclos y %.1f instrucciones del Broadway por instruccion de la PSP; "
		        "%s %.4f y %s %.4f por instruccion de la PSP\n",
		        (double)d[PROF_CPU][PMC_CICLOS] / (double)instr, (double)d[PROF_CPU][PMC_INSTR] / (double)instr,
		        e3[m], (double)d[PROF_CPU][PMC_FALLOS_L1D] / (double)instr, e4[m], (double)d[PROF_CPU][PMC_SALTOS_MAL] / (double)instr);
	if(ge_c){
		/* Cada fase por su unidad: llamada de dibujo, vértice, triángulo recibido, primitiva dibujada */
		static const int unit[GEF_N] = { 0, 1, 1, 2, 3, 0, 0 };
		static const char *const un[4] = { "llamada", "vertice", "triangulo", "primitiva" };
		u32 per[4];
		per[0] = draws; per[1] = verts; per[2] = tris; per[3] = prims;
		len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] PMC del GE (ciclos por unidad, IPC, %s y %s por 1000 instr):", e3[m], e4[m]);   //arreglo
		for(i = 0; i < GEF_N && len + 128 < sizeof(line); i++){
			u64 c = d[PROF_N + i][PMC_CICLOS], n = d[PROF_N + i][PMC_INSTR];
			u32 u = per[unit[i]];
			if(!c) continue;
			len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.0f por %s (IPC %.2f, %.1f, %.1f)",   //arreglo
			                        fn[i], u ? (double)c / (double)u : 0.0, un[unit[i]], n ? (double)n / (double)c : 0.0,
			                        n ? 1000.0 * (double)d[PROF_N + i][PMC_FALLOS_L1D] / (double)n : 0.0,
			                        n ? 1000.0 * (double)d[PROF_N + i][PMC_SALTOS_MAL] / (double)n : 0.0);
		}
		if(cmds && len + 64 < sizeof(line))
			snprintf(line + len, sizeof(line) - len, "; comandos %.0f ciclos por comando",
			         (double)d[PROF_N + GEF_COMANDOS][PMC_CICLOS] / (double)cmds);
		hle_log("%s\n", line);
	}
}

/* Velocidad, a dónde va el tiempo real, el GE por dentro y qué dibujó, y lo
   que añada el renderizador. Sin acentos: también sale en la consola del
   Wii. */
static void profile_report(const char *why, int forzar);   //Arreglo

void hle_profile_report(const char *why){ profile_report(why, 0); }   //Arreglo

//Arreglo
static void profile_bloques_lines(const PredecStats *p, const PredecStats *b, u64 instr){
	static const char *const tn[7] = { "1", "2", "3-4", "5-8", "9-16", "17-32", "33-65" };   //Arreglo
	u64 n = p->bq_bloques - b->bq_bloques, e[2][2], caidas = 0, ops[PREDEC_CAIDA_OPS];
	char line[1024], nm[24];
	size_t len;
	u32 i, k, j, orden[10], no = 0;
	for(i = 0; i < 2; i++) for(k = 0; k < 2; k++) e[i][k] = p->bq_enlace[i][k] - b->bq_enlace[i][k];
	len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] Bloques (prof_bloques): %llu ejecutados, %.2f instrucciones por bloque; por tamano:",
	                       (unsigned long long)n, n ? (double)(p->bq_instr - b->bq_instr) / (double)n : 0.0);
	for(i = 0; i < 7; i++)
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%%", tn[i],
		                        n ? 100.0 * (double)(p->bq_tam[i] - b->bq_tam[i]) / (double)n : 0.0);
	snprintf(line + len, sizeof(line) - len, "; al siguiente por el enlace %.1f%% (%llu sin el), al destino del salto %.1f%% (%llu sin el, "
	         "%llu acaban en jr o jalr)",
	         e[0][0] + e[0][1] ? 100.0 * (double)e[0][0] / (double)(e[0][0] + e[0][1]) : 0.0, (unsigned long long)e[0][1],
	         e[1][0] + e[1][1] ? 100.0 * (double)e[1][0] / (double)(e[1][0] + e[1][1]) : 0.0, (unsigned long long)e[1][1],
	         (unsigned long long)(p->bq_jr_sin_enlace - b->bq_jr_sin_enlace));
	hle_log("%s\n", line);
	{   //Arreglo
		const u64 jr = p->ras_jr - b->ras_jr, sb = p->ras_stub - b->ras_stub;
		hle_log("[TIEMPOS] Pila de retornos simulada (prof_bloques, 32 entradas): %llu jr ra; el enlace acierta %.1f%%, la pila "
		        "%.1f%%; la pila acertaria %llu de los %llu que falla el enlace; stubs jr ra + syscall: %llu, la pila acierta %.1f%%\n",
		        (unsigned long long)jr, jr ? 100.0 * (double)(p->ras_enlace - b->ras_enlace) / (double)jr : 0.0,
		        jr ? 100.0 * (double)(p->ras_ok - b->ras_ok) / (double)jr : 0.0, (unsigned long long)(p->ras_gana - b->ras_gana),
		        (unsigned long long)(jr - (p->ras_enlace - b->ras_enlace)), (unsigned long long)sb,
		        sb ? 100.0 * (double)(p->ras_stub_ok - b->ras_stub_ok) / (double)sb : 0.0);
	}   //Arreglo
	for(i = 0; i < 4; i++) caidas += p->bq_caidas[i] - b->bq_caidas[i];
	for(i = 0; i < PREDEC_CAIDA_OPS; i++){
		ops[i] = p->bq_caida_op[i] - b->bq_caida_op[i];
		if(!ops[i]) continue;
		if(no < 10) no++;
		else if(ops[orden[9]] >= ops[i]) continue;
		for(j = no - 1; j > 0 && ops[orden[j - 1]] < ops[i]; j--) orden[j] = orden[j - 1];
		orden[j] = i;
	}
	len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] Interprete de siempre (prof_bloques): %llu instrucciones (%.3f%%): "
	                       "delay slot suelto %llu, sin tarjeta %llu, no cabe en el reloj %llu, fuera de la RAM %llu; las que mas:",
	                       (unsigned long long)caidas, instr ? 100.0 * (double)caidas / (double)instr : 0.0,
	                       (unsigned long long)(p->bq_caidas[CAIDA_DELAY] - b->bq_caidas[CAIDA_DELAY]),
	                       (unsigned long long)(p->bq_caidas[CAIDA_SIN_TARJETA] - b->bq_caidas[CAIDA_SIN_TARJETA]),
	                       (unsigned long long)(p->bq_caidas[CAIDA_NO_CABE] - b->bq_caidas[CAIDA_NO_CABE]),
	                       (unsigned long long)(p->bq_caidas[CAIDA_FUERA] - b->bq_caidas[CAIDA_FUERA]));
	for(i = 0; i < no && len + 48 < sizeof(line); i++){
		cpu_predec_caida_nombre(orden[i], nm, sizeof(nm));
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %llu", nm, (unsigned long long)ops[orden[i]]);
	}
	hle_log("%s\n", line);
}

static void profile_report(const char *why, int forzar){   //Arreglo
	static const char *const names[PROF_N] = {
		"otros", "CPU", "syscalls", "GE", "rasterizar", "texturas", "framebuffers", "presentar", "esperar GX", "E/S",
		"audio", "registro"
	};
	char line[512];
	static char extra[12288];   /* el informe del renderizador puede ser largo */
	u64 total, d[PROF_N];
	double secs, game;
	size_t len;
	int i;
	GeStats ge;
	if(!prof_clock || !prof_hz || !prof_base.valid) return;
	prof_flush();
	total = prof_last - prof_base.clock;
	if(!total || (why && !forzar && total < prof_hz / 2)) return;   /* justo tras otro informe */   //Arreglo
	for(i = 0; i < PROF_N; i++) d[i] = prof_acc[i] - prof_base.acc[i];
	secs = (double)total / (double)prof_hz;
	game = (double)(cpu_cycles - prof_base.cycles) / (double)PSP_CPU_HZ;
	ge_get_stats(&ge);
	opt_describe(extra, sizeof(extra));
	hle_log("[OPT] %s\n", extra);
	hle_log("[TIEMPOS] %s%s%.1f s reales: %.2f s de juego (%.1f%%), %u vblanks, %llu imagenes (%.2f/s), "
	        "%.1f M instr (%.2f MIPS)\n", why ? why : "", why ? ", " : "", secs, game, 100.0 * game / secs,
	        prof_vblanks - prof_base.vblanks, (unsigned long long)(hle_stat_flips - prof_base.flips),
	        (double)(hle_stat_flips - prof_base.flips) / secs, (double)(cpu_executed - prof_base.instr) / 1e6,
	        (double)(cpu_executed - prof_base.instr) / secs / 1e6);
	len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS]");
	for(i = 0; i < PROF_N && len < sizeof(line) - 32; i++){
		static const int order[PROF_N] = { PROF_CPU, PROF_HLE, PROF_GE, PROF_RASTER, PROF_TEXTURAS, PROF_FB,
		                                   PROF_PRESENTAR, PROF_ESPERA_GX, PROF_ES, PROF_AUDIO, PROF_REGISTRO, PROF_OTRO };
		int b = order[i];
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%%", names[b], 100.0 * (double)d[b] / (double)total);
	}
	hle_log("%s\n", line);
	{
		/* El escritor de wiisp.log (registro_hilo): lo suyo no para la
		   emulación; lo que sí, la espera por cola llena o por un fallo */
		RegStats r;
		reg_stats(&r);
		if(r.bytes != prof_base.reg.bytes || r.esperas != prof_base.reg.esperas || r.fsyncs != prof_base.reg.fsyncs)
			hle_log("[TIEMPOS] registro en otro hilo: %llu KB en %llu lotes (%.0f ms), %llu fsync (%.0f ms, el peor %.0f ms "
			        "desde el arranque), fuera de la emulacion; la emulacion espero %llu veces (%.0f ms); cola hasta %u KB\n",
			        (unsigned long long)((r.bytes - prof_base.reg.bytes) / 1024),
			        (unsigned long long)(r.lotes - prof_base.reg.lotes),
			        1000.0 * (double)(r.t_escribir - prof_base.reg.t_escribir) / (double)prof_hz,
			        (unsigned long long)(r.fsyncs - prof_base.reg.fsyncs),
			        1000.0 * (double)(r.t_fsync - prof_base.reg.t_fsync) / (double)prof_hz,
			        1000.0 * (double)r.t_fsync_max / (double)prof_hz,
			        (unsigned long long)(r.esperas - prof_base.reg.esperas),
			        1000.0 * (double)(r.t_esperas - prof_base.reg.t_esperas) / (double)prof_hz, r.max_cola / 1024);
	}
	if(opt_on[OPT_CPU_PREDEC]){
		PredecStats p, *b = &prof_base.pd;
		u64 instr = cpu_executed - prof_base.instr;
		cpu_predec_stats(&p);
		hle_log("[TIEMPOS] CPU predecodificada (cpu_predec): %.1f%% de las instrucciones en bloques; %llu bloques hechos, "
		        "%llu tirados (%llu escrituras en codigo), %llu busquedas, arena %u de %u KB (%llu vaciados)%s\n",
		        instr ? 100.0 - 100.0 * (double)(p.pasos - b->pasos) / (double)instr : 0.0,
		        (unsigned long long)(p.bloques_hechos - b->bloques_hechos), (unsigned long long)(p.bloques_tirados - b->bloques_tirados),
		        (unsigned long long)(p.escrituras_en_codigo - b->escrituras_en_codigo), (unsigned long long)(p.busquedas - b->busquedas),
		        p.arena_usada / 1024, p.arena_total / 1024, (unsigned long long)(p.vaciados - b->vaciados),
		        opt_on[OPT_CPU_VERIFICAR] ? (p.verif_viejos ? "; cpu_verificar: HAY BLOQUES VIEJOS ([PREDEC] en el registro)"
		                                                    : "; cpu_verificar: ningun bloque viejo") : "");
		if(opt_on[OPT_CPU_VFPU_VERIFICAR])   //Arreglo
			hle_log("[TIEMPOS] cpu_vfpu_verificar: %llu tarjetas de la VFPU comparadas con vfpu_exec, %s\n",
			        (unsigned long long)(p.vfpu_verif - b->vfpu_verif),
			        p.vfpu_verif_mal ? "HAY DIFERENCIAS ([PREDEC] en el registro)" : "todas iguales");   //Arreglo
		//arreglo
		/* La CPU por dentro (muestreo) y dónde está cada cosa (0x80... es
		   MEM1 y 0x90... MEM2 en el Wii) */
		{
			static const char *const nom[CPF_N] = { "tarjetas", "VFPU", "FPU", "memoria lenta", "buscar y traducir", "interprete de siempre" };
			const u64 cuenta[CPF_N] = { 0, p.vfpu - b->vfpu, p.fpu - b->fpu, p.lentas - b->lentas,
			                            p.busquedas - b->busquedas, p.pasos - b->pasos };
			double cpu_s = (double)(prof_acc[PROF_CPU] - prof_base.acc[PROF_CPU]) / (double)prof_hz;
			u32 ms = 0, k;
			char l2[640];
			size_t n2;
			for(k = 0; k < CPF_N; k++) ms += prof_cpu_samples[k] - prof_base.cpu_samples[k];
			if(!opt_on[OPT_PROF_CPU]) ms = 0;   //Arreglo
			n2 = opt_on[OPT_PROF_CPU] ? (size_t)snprintf(l2, sizeof(l2), "[TIEMPOS] CPU por dentro (muestreo, %u muestras):", (unsigned)ms)
			                          : (size_t)snprintf(l2, sizeof(l2), "[TIEMPOS] CPU por dentro (sin prof_cpu)");   //Arreglo
			for(k = 0; k < CPF_N && ms; k++){
				u32 sm = prof_cpu_samples[k] - prof_base.cpu_samples[k];
				double f = (double)sm / (double)ms;
				n2 += (size_t)snprintf(l2 + n2, sizeof(l2) - n2, " %s %.1f%%", nom[k], 100.0 * f);
				if(k && cuenta[k])
					n2 += (size_t)snprintf(l2 + n2, sizeof(l2) - n2, " (%llu, %.0f ns cada una)", (unsigned long long)cuenta[k],
					                       1e9 * cpu_s * f / (double)cuenta[k]);
			}
			if(opt_on[OPT_PROF_CPU])   //Arreglo
				n2 += (size_t)snprintf(l2 + n2, sizeof(l2) - n2, "; VFPU en tarjetas propias %llu",
				                       (unsigned long long)(p.vfpu_tarjetas - b->vfpu_tarjetas));   //Arreglo
			snprintf(l2 + n2, sizeof(l2) - n2, "; memoria: bloques en %08X%s (%u de %u KB), frio en %08X (%u de %u KB), "
			         "tabla en %08X, mapa en %08X, RAM de la PSP en %08X", (unsigned)p.dir_arena,
			         p.compacto ? " compactos" : "", p.arena_usada / 1024, p.arena_total / 1024, (unsigned)p.dir_frio,
			         p.frio_usada / 1024, p.frio_total / 1024, (unsigned)p.dir_tabla, (unsigned)p.dir_mapa,
			         (unsigned)(uintptr_t)psp_mem.ram);
			hle_log("%s\n", l2);
		}
		if(opt_on[OPT_PROF_BLOQUES]) profile_bloques_lines(&p, b, instr);   //Arreglo
	}
	if(opt_on[OPT_PROF_ES]){
		IoCenso c;
		const IoCenso *b = &prof_base.es;
		io_censo(&c);
		if(c.lecturas != b->lecturas){
			double ms = 1000.0 * (double)(c.t_host - b->t_host) / (double)prof_hz;
			hle_log("[TIEMPOS] E/S (prof_es): %llu lecturas del disco (%llu asincronas), %llu KB en %.0f ms (%.1f MB/s), "
			        "seguidas %.0f%%; de hasta 64 KB %llu, hasta 1 MB %llu, mas %llu; la peor desde el arranque %u KB en %.0f ms; "
			        "leyendo en otro hilo se esconderian como mucho %.0f ms de esos (%llu medidas)\n",
			        (unsigned long long)(c.lecturas - b->lecturas), (unsigned long long)(c.asincronas - b->asincronas),
			        (unsigned long long)((c.bytes - b->bytes) / 1024), ms,
			        ms > 0.0 ? (double)(c.bytes - b->bytes) / 1048576.0 / (ms / 1000.0) : 0.0,
			        100.0 * (double)(c.seguidas - b->seguidas) / (double)(c.lecturas - b->lecturas),
			        (unsigned long long)(c.por_tam[0] - b->por_tam[0]), (unsigned long long)(c.por_tam[1] - b->por_tam[1]),
			        (unsigned long long)(c.por_tam[2] - b->por_tam[2]), c.peor_bytes / 1024,
			        1000.0 * (double)c.peor_t / (double)prof_hz,
			        1000.0 * (double)(c.solapable - b->solapable) / (double)prof_hz,
			        (unsigned long long)(c.medidas - b->medidas));
		}
	}
	if(opt_on[OPT_PROF_TIRONES] && tir.t.imagenes){
		const Tirones *t = &tir.t;
		double normal = tir_normal(t);
		u64 sum = 0, peor = 0;
		hle_log("[TIEMPOS] Tirones (prof_tirones): %u imagenes, lo normal %.1f s reales por s de juego (%.0f%%); "
		        "respecto a lo normal: hasta 1,25x %u, 1,25-2x %u, 2-4x %u, mas de 4x %u; tirones (mas del doble y %d ms "
		        "perdidos): %u, %.2f s perdidos de %.1f s\n", (unsigned)t->imagenes, normal, normal > 0.0 ? 100.0 / normal : 0.0,
		        (unsigned)t->rel[0], (unsigned)t->rel[1], (unsigned)t->rel[2], (unsigned)t->rel[3], TIR_MIN_MS,
		        (unsigned)t->n, t->perdido_s, t->real_s);
		for(i = 0; i < PROF_N; i++){ sum += t->acc[i]; peor += t->peor_acc[i]; }
		if(t->n && sum){
			len = (size_t)snprintf(line, sizeof(line), "[TIEMPOS] Tirones: su tiempo en");
			for(i = 0; i < PROF_N && len < sizeof(line) - 32; i++)
				if(t->acc[i] * 50 >= sum)   /* desde el 2 % */
					len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%%", names[i], 100.0 * (double)t->acc[i] / (double)sum);
			len += (size_t)snprintf(line + len, sizeof(line) - len, "; el peor %.0f ms (%.0f de mas) a los %.1f s de juego:",
			                        1000.0 * t->peor_s, 1000.0 * t->peor_perdido_s, t->peor_juego_s);
			for(i = 0; i < PROF_N && len < sizeof(line) - 32; i++)
				if(t->peor_acc[i] * 50 >= peor)
					len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.0f ms", names[i],
					                        1000.0 * (double)t->peor_acc[i] / (double)prof_hz);
			hle_log("%s\n", line);
		}
		tir_nuevo_bloque(&tir.t);
	}
	hle_log("[TIEMPOS] GE: %u listas, %u comandos, %u vertices, %u primitivas | %llu syscalls, %llu cambios de hilo, "
	        "%llu llamadas al juego\n",
	        ge.lists - prof_base.ge.lists, ge.commands - prof_base.ge.commands, ge.vertices - prof_base.ge.vertices,
	        ge.primitives - prof_base.ge.primitives, (unsigned long long)(hle_stat_syscalls - prof_base.syscalls),
	        (unsigned long long)(kernel_stat_switches - prof_base.switches),
	        (unsigned long long)(kernel_stat_guest_calls - prof_base.guest_calls));
	profile_ge_lines(&ge, &prof_base.ge, prof_vblanks - prof_base.vblanks);
	profile_pmc_lines(&ge, &prof_base.ge, cpu_executed - prof_base.instr);
	/* Lo del renderizador (una línea por cada \n) */
	extra[0] = 0;
	if(profile_hook) profile_hook(extra, sizeof(extra));
	{
		char *p = extra;
		while(*p){
			char *nl = strchr(p, '\n');
			if(nl) *nl = 0;
			hle_log("[TIEMPOS] %s\n", p);
			if(!nl) break;
			p = nl + 1;
		}
	}
	/* A la tarjeta. Con el hilo, sin esperar: el fsync lo hace el escritor
	   y la emulación sigue (los fallos y el final sí esperan) */
	if(log_hilo()) reg_pedir_fsync();
	else hle_log_sync();
	profile_snapshot();
	tir.valid = 0;   /* lo que tardó el informe no es un tirón */
}

static void profile_tick(void){
	if(!prof_clock || !prof_hz) return;
	if(opt_on[OPT_PROF_PMC] != prof_pmc_on) prof_pmc_enable(opt_on[OPT_PROF_PMC]);
	prof_vtx_on = opt_on[OPT_PROF_VERTICES];   //Arreglo
	//arreglo
	prof_pmc_set_modo(opt_on[OPT_PROF_PMC_L2] ? 1 : opt_on[OPT_PROF_PMC_CTR] ? 2 : opt_on[OPT_PROF_PMC_CARGA] ? 3 : 0);
	if(!prof_base.valid){ profile_snapshot(); return; }
	if(opt_on[OPT_PROF_VENTANA_JUEGO]){   //Arreglo
		if(cpu_cycles / CYCLES_PER_FRAME >= ventana_fin){   //Arreglo
			char why[64];   //Arreglo
			snprintf(why, sizeof(why), "ventana de juego: vblanks %u a %u",
			         (unsigned)(prof_base.cycles / CYCLES_PER_FRAME), (unsigned)(cpu_cycles / CYCLES_PER_FRAME));   //Arreglo
			profile_report(why, 1);   //Arreglo
		}   //Arreglo
		return;   //Arreglo
	}   //Arreglo
	if(prof_clock() - prof_base.clock >= (u64)HLE_PROFILE_SECONDS * prof_hz) hle_profile_report(NULL);
}
static int stats_final, stalled, stall_dumps;

void hle_exit(const char *reason){
	if(!exited){
		exited = 1;
		exit_reason = reason;
		if(log_file) hle_log("[WIISP] fin: %s\n", reason);
		hle_profile_report("hasta el final");
		hle_log_stats();
		stats_final = 1;   /* las del programa terminado ya están */
		hle_log_sync();
	}
	cpu_stop_requested = 1;
}


void hle_log_stats(void){
	u64 instr = cpu_executed - stats_base_instr, vf = 0;
	int order[VS_COUNT], i, j, n = 0;
	char line[512];
	size_t len;
	if(!log_file && !getenv("WIISP_STATS")) return;
	if(!instr || cpu_executed == stats_logged_instr || stats_final) return;
	stats_logged_instr = cpu_executed;
	vfpu_stats_fold();
	for(i = 0; i < VS_COUNT; i++) vf += vfpu_stat[i];
	/* Cada vpfx afecta a una instrucción: da la proporción con prefijos */
	hle_log("[STATS] %llu instrucciones; VFPU %.1f%% (%llu, %.1f%% con prefijos)\n",
	        (unsigned long long)instr, 100.0 * (double)vf / (double)instr, (unsigned long long)vf,
	        vf ? 100.0 * (double)vfpu_stat[VS_VPFX] / (double)vf : 0.0);
	hle_log("[STATS] cambios de hilo %llu, cambios de VFPU %llu, llamadas del HLE al juego %llu\n",
	        (unsigned long long)kernel_stat_switches, (unsigned long long)kernel_stat_vfpu_loads,
	        (unsigned long long)kernel_stat_guest_calls);
	if(!vf) return;
	/* Las 16 instrucciones VFPU más usadas */
	for(i = 0; i < VS_COUNT; i++){
		if(!vfpu_stat[i]) continue;
		for(j = n; j > 0 && vfpu_stat[order[j - 1]] < vfpu_stat[i]; j--) order[j] = order[j - 1];
		order[j] = i;
		n++;
	}
	len = (size_t)snprintf(line, sizeof(line), "[STATS] VFPU:");
	for(i = 0; i < n && i < 16 && len < sizeof(line) - 40; i++)
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %.1f%%", vfpu_stat_name[order[i]],
		                        100.0 * (double)vfpu_stat[order[i]] / (double)vf);
	hle_log("%s\n", line);
	/* Cada minuto de juego: con el hilo, sin esperar a la tarjeta (al
	   terminar, hle_exit espera y cerrar el archivo lo escribe todo) */
	if(log_hilo()) reg_pedir_fsync();
	else hle_log_sync();
}

/* ------------------------------------------------------------------ */
/* Arranque                                                           */
/* ------------------------------------------------------------------ */

/* --- Varios módulos ---------------------------------------------------------- */

static void patch_jump(u32 stub, u32 target){
	mem_write32(stub, 0x08000000u | ((target >> 2) & 0x03FFFFFFu));   /* j target */
	mem_write32(stub + 4, 0);
}

int hle_link_module(const PspModule *m){
	u32 i, j;
	if(m->syscall_base != num_resolved) return -1;
	if(num_resolved + m->num_imports > cap_resolved){
		u32 cap = (num_resolved + m->num_imports) * 2 + 64;
		ResolvedImport *n = realloc(resolved, cap * sizeof(ResolvedImport));
		if(!n) return -1;
		resolved = n;
		cap_resolved = cap;
	}
	for(i = 0; i < m->num_imports; i++){
		ResolvedImport *r = &resolved[num_resolved + i];
		memset(r, 0, sizeof(*r));
		snprintf(r->lib, sizeof(r->lib), "%s", m->imports[i].lib);
		r->nid = m->imports[i].nid;
		r->stub = m->imports[i].stub_addr;
		r->func = hle_find(r->lib, r->nid, &r->name);
		/* Una función de un módulo ya cargado gana al HLE */
		for(j = 0; j < num_exports; j++)
			if(exports[j].nid == r->nid && !strcmp(exports[j].lib, r->lib)){
				patch_jump(r->stub, exports[j].addr);
				r->linked = 1;
				break;
			}
	}
	num_resolved += m->num_imports;

	/* Sus exportaciones, para los imports pendientes y los módulos futuros */
	for(i = 0; i < m->num_exports; i++){
		const PspExport *x = &m->exports[i];
		if(!x->lib[0]) continue;   /* module_start y compañía */
		if(num_exports == cap_exports){
			u32 cap = cap_exports * 2 + 64;
			PspExport *n = realloc(exports, cap * sizeof(PspExport));
			if(!n) return -1;
			exports = n;
			cap_exports = cap;
		}
		exports[num_exports++] = *x;
		for(j = 0; j < num_resolved; j++)
			if(!resolved[j].linked && resolved[j].nid == x->nid && !strcmp(resolved[j].lib, x->lib)){
				patch_jump(resolved[j].stub, x->addr);
				resolved[j].linked = 1;
			}
	}
	return 0;
}

void hle_unlink_module(const PspModule *m){
	u32 i = 0;
	/* Fuera sus exportaciones (quien las importó apuntará a memoria libre,
	   como en la PSP si no se descarga antes) */
	while(i < num_exports){
		if(exports[i].addr >= m->load_start && exports[i].addr < m->load_end) exports[i] = exports[--num_exports];
		else i++;
	}
}

u32 hle_next_syscall(void){ return num_resolved; }

int hle_init(PspModule *mod, const char *host_dir, const char *exec_name){
	hle_shutdown();
	module = mod;
	exited = 0;
	exit_reason = "";
	cpu_cycles = 0;
	tir_init(&tir.t);   /* lo normal es de cada programa */
	tir.valid = 0;
	stats_base_instr = stats_logged_instr = cpu_executed;
	stats_final = 0;
	stall_last_instr = cpu_executed;
	stalled = stall_dumps = 0;
	prof_base.valid = 0;
	memset(ring, 0, sizeof(ring));
	memset(trace_recent, 0, sizeof(trace_recent));
	ring_pos = trace_count = trace_skipped = 0;
	vfpu_stats_fold();
	memset(vfpu_stat, 0, sizeof(vfpu_stat));

	if(hle_link_module(mod)) return -1;

	/* Trampolín al que vuelven los hilos al terminar su función */
	mem_write32(HLE_KERNEL_TRAMPOLINE, MIPS_SYSCALL(HLE_SYSCALL_THREAD_RETURN));
	mem_write32(HLE_KERNEL_TRAMPOLINE + 4, 0);
	mem_write32(HLE_CALLBACK_TRAMPOLINE, MIPS_SYSCALL(HLE_SYSCALL_CALLBACK_RETURN));
	mem_write32(HLE_CALLBACK_TRAMPOLINE + 4, 0);
	mem_write32(HLE_THREAD_CB_TRAMPOLINE, MIPS_SYSCALL(HLE_SYSCALL_THREAD_CB_RETURN));
	mem_write32(HLE_THREAD_CB_TRAMPOLINE + 4, 0);
	mem_write32(HLE_GUEST_CALL_TRAMPOLINE, MIPS_SYSCALL(HLE_SYSCALL_GUEST_CALL_RETURN));
	mem_write32(HLE_GUEST_CALL_TRAMPOLINE + 4, 0);

	io_init(host_dir, exec_name && !strncmp(exec_name, "disc0:", 6));
	display_init();
	audio_init();
	sas_init();
	atrac_init();
	mpeg_init();
	utility_init();
	power_init();
	misc_init();
	ge_init();
	kernel_init(mod, exec_name);
	return 0;
}

void hle_shutdown(void){
	if(module){
		hle_log_stats();
		module_shutdown();
		atrac_shutdown();
		kernel_shutdown();
		io_shutdown();
	}
	free(resolved);
	resolved = NULL;
	num_resolved = cap_resolved = 0;
	free(exports);
	exports = NULL;
	num_exports = cap_exports = 0;
	module = NULL;
}

int hle_run_frame(void){
	u64 frame;
	if(exited) return 1;
	kernel_run_until((cpu_cycles / CYCLES_PER_FRAME + 1) * CYCLES_PER_FRAME);
	if(!exited){
		display_vblank();
		kernel_vblank();
	}
	vfpu_stats_fold();
	log_tick();
	prof_vblanks++;
	if(opt_on[OPT_PROF_TIRONES]) tirones_tick();
	else tir.valid = 0;
	if(opt_on[OPT_PROF_ES]) io_censo_tick();
	profile_tick();
	frame = cpu_cycles / CYCLES_PER_FRAME;
	if(exited || !frame) return exited;
	/* Cada minuto de juego emulado, por si la sesión acaba apagando */
	if(frame % 3600 == 0) hle_log_stats();
	/* Atasco: 10 s emulados casi sin ejecutar nada (todos los hilos
	   esperan algo). Se vuelca el estado para saber qué esperan. */
	if(frame % 600 == 0){
		u64 done = cpu_executed - stall_last_instr;
		stall_last_instr = cpu_executed;
		if(done < 60000){
			if(!stalled && stall_dumps < 3){
				char why[96];
				stall_dumps++;
				snprintf(why, sizeof(why), "atasco: %llu instrucciones en 10 s emulados", (unsigned long long)done);
				hle_dump_state(why);
			}
			stalled = 1;
		} else stalled = 0;
	}
	return exited;
}

/* ------------------------------------------------------------------ */
/* Informe de imports                                                 */
/* ------------------------------------------------------------------ */

int hle_write_imports_report(const PspModule *mod, const char *exec_path, const char *out_path){
	FILE *f = fopen(out_path, "w");
	u32 i, j, total_impl = 0, total_named = 0;
	if(!f) return -1;

	for(i = 0; i < mod->num_imports; i++){
		const char *name;
		if(hle_find(mod->imports[i].lib, mod->imports[i].nid, &name)) total_impl++;
		if(name) total_named++;
	}

	fprintf(f, "WIISP - informe de imports\n");
	fprintf(f, "Archivo:  %s\n", exec_path);
	if(mod->title[0])   fprintf(f, "Titulo:   %s\n", mod->title);
	if(mod->disc_id[0]) fprintf(f, "ID:       %s\n", mod->disc_id);
	fprintf(f, "Modulo:   %s\n", mod->name);
	fprintf(f, "Imports:  %u funciones de %u bibliotecas\n", mod->num_imports, mod->num_libs);
	fprintf(f, "          %u con nombre conocido, %u implementadas en WIISP\n\n",
	        total_named, total_impl);

	/* Resumen por biblioteca (en el orden en que aparecen) */
	fprintf(f, "Resumen por biblioteca:\n");
	fprintf(f, "  %-28s %9s %13s\n", "Biblioteca", "Funciones", "Implementadas");
	for(i = 0; i < mod->num_imports; i++){
		u32 count = 0, impl = 0;
		for(j = 0; j < i; j++)
			if(!strcmp(mod->imports[j].lib, mod->imports[i].lib)) break;
		if(j < i) continue; /* biblioteca ya contada */
		for(j = i; j < mod->num_imports; j++){
			if(strcmp(mod->imports[j].lib, mod->imports[i].lib)) continue;
			count++;
			if(hle_find(mod->imports[j].lib, mod->imports[j].nid, NULL)) impl++;
		}
		fprintf(f, "  %-28s %9u %13u\n", mod->imports[i].lib, count, impl);
	}

	fprintf(f, "\nLista completa ([x] = implementada):\n");
	for(i = 0; i < mod->num_imports; i++){
		const PspImport *imp = &mod->imports[i];
		const char *name;
		int impl = hle_find(imp->lib, imp->nid, &name) != NULL;
		fprintf(f, "  [%c] %-24s 0x%08X  %s\n", impl ? 'x' : ' ', imp->lib, imp->nid,
		        name ? name : "(NID desconocido)");
	}
	fclose(f);
	return 0;
}
