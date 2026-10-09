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
#include "hle/hle.h"
#include "hle/nid_names.h"
#include "core/memory.h"
#include "gpu/ge.h"
#include "cpu/vfpu.h"

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
		hle_log("[LLAMADA] (%u repetidas omitidas)\n", trace_skipped);
		trace_skipped = 0;
	}
	format_call(r, line, sizeof(line));
	hle_log("[LLAMADA] %s\n", line);
	if(++trace_count == TRACE_MAX) hle_log("[LLAMADA] fin de la traza del arranque (%u llamadas)\n", TRACE_MAX);
}

void hle_dump_state(const char *why){
	char line[256];
	u32 i;
	if(!module) return;
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
			resolved[code].func();
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

void cpu_fault(const char *what, u32 addr, u32 instr){
	char reason[160];
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

#define LOG_FILE_MAX (2u * 1024 * 1024)
static u32 log_written;

void hle_set_log_file(const char *path){
	if(log_file) fclose(log_file);
	log_file = path ? fopen(path, "w") : NULL;
	log_written = 0;
	if(log_file) fprintf(log_file, "[WIISP] version " WIISP_VERSION "\n");
}

/* En la SD (libfat) el tamaño del archivo solo se apunta al cerrarlo o con
   fsync: sin esto, si la sesión acaba apagando el Wii el registro queda
   vacío. Se sincroniza como mucho una vez por segundo, y siempre en los
   fallos, al terminar y con las estadísticas. */
void hle_log_sync(void){
	if(log_file){
		fflush(log_file);
		fsync(fileno(log_file));
	}
}

void hle_log(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if(log_file && log_written < LOG_FILE_MAX){
		int n;
		va_start(ap, fmt);
		n = vfprintf(log_file, fmt, ap);
		va_end(ap);
		if(n > 0) log_written += (u32)n;
		if(log_written >= LOG_FILE_MAX) fputs("[registro recortado]\n", log_file);
		fflush(log_file);
		{
			static time_t last_sync;
			time_t now = time(NULL);
			if(now != last_sync || !strncmp(fmt, "[CPU]", 5)){
				last_sync = now;
				fsync(fileno(log_file));
			}
		}
	}
}

int hle_has_exited(void){ return exited; }
const char *hle_exit_reason(void){ return exit_reason; }

static u64 stats_base_instr, stats_logged_instr, stall_last_instr;
static int stats_final, stalled, stall_dumps;

void hle_exit(const char *reason){
	if(!exited){
		exited = 1;
		exit_reason = reason;
		if(log_file) hle_log("[WIISP] fin: %s\n", reason);
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
	hle_log_sync();
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
	stats_base_instr = stats_logged_instr = cpu_executed;
	stats_final = 0;
	stall_last_instr = cpu_executed;
	stalled = stall_dumps = 0;
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
	if(!exited) kernel_vblank();
	vfpu_stats_fold();
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
