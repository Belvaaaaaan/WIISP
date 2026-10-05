/**
 * WIISP - kernel.c
 * HLE del kernel de la PSP: hilos, planificador, sincronización, memoria,
 * tiempo y salida del programa.
 *
 * Los hilos de la PSP se ejecutan sobre un único hilo del anfitrión. El
 * planificador es como el de la PSP: siempre corre el hilo listo de mayor
 * prioridad (número menor); entre iguales, el que lleva más tiempo esperando.
 * Los cambios de hilo solo ocurren entre instrucciones: el HLE marca
 * need_resched y pide al intérprete que pare.
 *
 * Si no hay ningún hilo listo, el tiempo salta directamente al siguiente
 * evento (fin de un retardo o vblank) en vez de gastar ciclos esperando.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include "hle/hle.h"
#include "core/memory.h"

/* ------------------------------------------------------------------ */
/* UIDs                                                               */
/* ------------------------------------------------------------------ */

enum { UID_THREAD = 1, UID_SEMA, UID_EVF, UID_MEMBLOCK, UID_CALLBACK, UID_LWMUTEX };

/* UID = 0x04 tipo índice+1: positivo y fácil de reconocer en los logs */
static inline u32 make_uid(int type, int index){ return 0x04000000u | ((u32)type << 16) | (u32)(index + 1); }
static inline int uid_index(u32 uid, int type, int max){
	int index = (int)(uid & 0xFFFF) - 1;
	if((uid & 0xFFFF0000u) != (0x04000000u | ((u32)type << 16)) || index < 0 || index >= max) return -1;
	return index;
}

/* ------------------------------------------------------------------ */
/* Hilos                                                              */
/* ------------------------------------------------------------------ */

#define MAX_THREADS 64

enum {
	TH_RUNNING = 1, TH_READY = 2, TH_WAITING = 4, TH_SUSPEND = 8, TH_DORMANT = 16, TH_DEAD = 32
};

enum {
	W_NONE = 0, W_SLEEP = 1, W_DELAY = 2, W_SEMA = 3, W_EVF = 4, W_THREADEND = 9,
	W_VBLANK = 100, W_LWMUTEX = 101
};

typedef struct {
	int used;
	char name[32];
	int status;
	u32 attr, entry, init_prio, prio;
	u32 stack, stack_size, gp;
	CpuState ctx;
	u64 ready_seq;      /* orden de llegada a la cola de listos */

	int wait;           /* W_* */
	int wait_index;     /* objeto por el que espera */
	u64 wait_seq;       /* orden de llegada a la espera */
	int has_timeout;
	u64 wait_until;     /* ciclo en el que vence */
	u32 timeout_addr;   /* puntero a u32 con microsegundos, o 0 */
	u32 wait_a, wait_b, wait_c; /* datos de la espera (cuenta, patrón...) */

	int wakeup_count;
	u32 exit_status;
} Thread;

static Thread threads[MAX_THREADS];
static int cur = -1;           /* hilo que corre ahora, -1 = ocioso */
static int need_resched;
static u64 seq;
static u32 module_gp;

static inline Thread *current(void){ return cur >= 0 ? &threads[cur] : NULL; }
static inline u32 thread_uid(int i){ return make_uid(UID_THREAD, i); }

static int find_thread(u32 uid){
	int i;
	if(uid == 0) return cur;
	i = uid_index(uid, UID_THREAD, MAX_THREADS);
	return (i >= 0 && threads[i].used) ? i : -1;
}

static void request_resched(void){
	need_resched = 1;
	cpu_stop_requested = 1;
}

/* Registros del hilo: si es el actual, viven en `cpu` */
static inline CpuState *thread_ctx(int i){ return i == cur ? &cpu : &threads[i].ctx; }

static void make_ready(int i){
	threads[i].status = TH_READY;
	threads[i].wait = W_NONE;
	threads[i].ready_seq = ++seq;
	request_resched();
}

/* Despierta un hilo en espera con el valor de retorno indicado */
static void wake(int i, u32 ret){
	thread_ctx(i)->r[R_V0] = ret;
	make_ready(i);
}

/* El hilo actual pasa a esperar. timeout_addr: puntero a microsegundos
   (0 = sin límite). El valor de retorno por defecto es 0. */
static void wait_current(int type, int index, u32 timeout_addr){
	Thread *t = current();
	if(!t) return;
	t->status = TH_WAITING;
	t->wait = type;
	t->wait_index = index;
	t->wait_seq = ++seq;
	t->timeout_addr = timeout_addr;
	t->has_timeout = 0;
	if(timeout_addr && mem_valid(timeout_addr, 4)){
		t->has_timeout = 1;
		t->wait_until = cpu_cycles + (u64)mem_read32(timeout_addr) * CYCLES_PER_US;
	}
	RETURN(0);
	request_resched();
}

static void schedule(void){
	int i, best = -1;
	need_resched = 0;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || (t->status != TH_READY && t->status != TH_RUNNING)) continue;
		if(best < 0 || t->prio < threads[best].prio ||
		   (t->prio == threads[best].prio && t->ready_seq < threads[best].ready_seq))
			best = i;
	}
	if(best == cur){
		if(cur >= 0) threads[cur].status = TH_RUNNING;
		return;
	}
	if(cur >= 0){
		threads[cur].ctx = cpu;
		if(threads[cur].status == TH_RUNNING){
			threads[cur].status = TH_READY;
			/* expulsado por uno de más prioridad: conserva su turno */
		}
	}
	cur = best;
	if(cur >= 0){
		threads[cur].status = TH_RUNNING;
		cpu = threads[cur].ctx;
		cpu.llbit = 0; /* un cambio de hilo implica una interrupción */
	}
}

static void exit_thread(int i, u32 status, int del);
static void evf_timeout(Thread *t);

/* Despierta las esperas vencidas */
static void process_timeouts(void){
	int i;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING) continue;
		if(t->wait == W_DELAY && cpu_cycles >= t->wait_until) wake(i, 0);
		else if(t->has_timeout && cpu_cycles >= t->wait_until){
			if(t->timeout_addr) mem_write32(t->timeout_addr, 0);
			if(t->wait == W_EVF) evf_timeout(t);
			wake(i, SCE_KERNEL_ERROR_WAIT_TIMEOUT);
		}
	}
}

static u64 next_timeout(void){
	u64 next = ~0ull;
	int i;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING) continue;
		if((t->wait == W_DELAY || t->has_timeout) && t->wait_until < next) next = t->wait_until;
	}
	return next;
}

static int any_thread_alive(void){
	int i;
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status != TH_DORMANT && threads[i].status != TH_DEAD)
			return 1;
	return 0;
}

void kernel_run_until(u64 target){
	while(!hle_has_exited() && cpu_cycles < target){
		process_timeouts();
		if(need_resched) schedule();
		if(!any_thread_alive()){
			hle_exit("todos los hilos terminaron");
			break;
		}
		if(cur < 0){
			/* Ocioso: saltar al siguiente evento */
			u64 next = next_timeout();
			cpu_cycles = next < target ? next : target;
			continue;
		}
		u64 limit = next_timeout();
		if(limit > target) limit = target;
		u64 slice = limit > cpu_cycles ? limit - cpu_cycles : 1;
		if(slice > 1000000) slice = 1000000;
		cpu_stop_requested = 0;
		cpu_run((u32)slice);
	}
}

void kernel_vblank(void){
	int i;
	cpu.llbit = 0; /* la interrupción de vblank rompe ll/sc */
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_VBLANK)
			wake(i, 0);
}

int kernel_wait_vblank(void){
	wait_current(W_VBLANK, 0, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Memoria de usuario                                                 */
/* ------------------------------------------------------------------ */

#define MAX_BLOCKS 256
#define USER_MEM_START 0x08800000u

typedef struct {
	int used;
	u32 addr, size;
	char name[32];
} MemBlock;

static MemBlock blocks[MAX_BLOCKS];
static u32 user_mem_end;

static int block_overlaps(u32 addr, u32 size){
	int i;
	for(i = 0; i < MAX_BLOCKS; i++)
		if(blocks[i].used && addr < blocks[i].addr + blocks[i].size && blocks[i].addr < addr + size)
			return 1;
	return 0;
}

static int add_block(u32 addr, u32 size, const char *name){
	int i;
	for(i = 0; i < MAX_BLOCKS; i++){
		if(blocks[i].used) continue;
		blocks[i].used = 1;
		blocks[i].addr = addr;
		blocks[i].size = size;
		snprintf(blocks[i].name, sizeof(blocks[i].name), "%s", name ? name : "");
		return i;
	}
	return -1;
}

/* Recorre los huecos libres en orden de dirección; devuelve el elegido.
   Un asignador simple basta: hay pocos bloques. */
static u32 find_gap(u32 size, u32 align, int from_high, u32 *largest, u32 *total){
	u32 pos = USER_MEM_START, found = 0;
	if(largest) *largest = 0;
	if(total) *total = 0;
	for(;;){
		/* siguiente bloque ocupado a partir de pos */
		u32 next = user_mem_end;
		int i;
		for(i = 0; i < MAX_BLOCKS; i++){
			if(!blocks[i].used) continue;
			if(blocks[i].addr <= pos && pos < blocks[i].addr + blocks[i].size){
				pos = blocks[i].addr + blocks[i].size;
				i = -1; /* reiniciar: pos cambió */
				continue;
			}
			if(blocks[i].addr > pos && blocks[i].addr < next) next = blocks[i].addr;
		}
		if(pos >= user_mem_end) break;
		{
			u32 gap = next - pos;
			if(largest && gap > *largest) *largest = gap;
			if(total) *total += gap;
			if(size){
				if(!from_high){
					u32 a = (pos + align - 1) & ~(align - 1);
					if(!found && a + size <= next && a >= pos) found = a;
				} else {
					u32 a = (next - size) & ~(align - 1);
					if(next >= size && a >= pos) found = a; /* el último que cabe */
				}
			}
		}
		pos = next;
	}
	return found;
}

u32 kernel_alloc(u32 size, int from_high, const char *name){
	u32 addr;
	size = (size + 255) & ~255u;
	if(!size) return 0;
	addr = find_gap(size, 256, from_high, NULL, NULL);
	if(!addr || add_block(addr, size, name) < 0) return 0;
	return addr;
}

void kernel_free(u32 addr){
	int i;
	for(i = 0; i < MAX_BLOCKS; i++)
		if(blocks[i].used && blocks[i].addr == addr) blocks[i].used = 0;
}

static void sceKernelAllocPartitionMemory(void){
	u32 partition = ARG(0), name = ARG(1), type = ARG(2), size = ARG(3), addr = ARG(4);
	char n[32] = "";
	u32 align = 256, where = 0;
	int i;

	if(partition != 2 && partition != 6){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_PARTITION); return; }
	if(type > 4){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK); return; }
	if(size == 0 || size > 0x02000000u){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
	if(name) mem_read_cstr(name, n, sizeof(n));
	if(type >= 3){
		align = addr;
		if(!align || (align & (align - 1))){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
		if(align < 256) align = 256;
	}
	size = (size + 255) & ~255u;
	if(type == 2){
		where = addr & ~255u;
		if(where < USER_MEM_START || where + size > user_mem_end || block_overlaps(where, size))
			where = 0;
	} else {
		where = find_gap(size, align, type == 1 || type == 4, NULL, NULL);
	}
	if(!where || (i = add_block(where, size, n)) < 0){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	RETURN(make_uid(UID_MEMBLOCK, i));
}

static void sceKernelFreePartitionMemory(void){
	int i = uid_index(ARG(0), UID_MEMBLOCK, MAX_BLOCKS);
	if(i < 0 || !blocks[i].used){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
	blocks[i].used = 0;
	RETURN(0);
}

static void sceKernelGetBlockHeadAddr(void){
	int i = uid_index(ARG(0), UID_MEMBLOCK, MAX_BLOCKS);
	if(i < 0 || !blocks[i].used){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
	RETURN(blocks[i].addr);
}

static void sceKernelMaxFreeMemSize(void){
	u32 largest;
	find_gap(0, 256, 0, &largest, NULL);
	RETURN(largest);
}

static void sceKernelTotalFreeMemSize(void){
	u32 total;
	find_gap(0, 256, 0, NULL, &total);
	RETURN(total);
}

static void sceKernelDevkitVersion(void){ RETURN(0x06060010); } /* firmware 6.60 */
static void return_zero(void){ RETURN(0); }

/* Printf mínimo para sceKernelPrintf: lee hasta 7 argumentos de registros */
static void sceKernelPrintf(void){
	char fmt[256], out[512], spec[16];
	u32 i, o = 0, argn = 1;
	mem_read_cstr(ARG(0), fmt, sizeof(fmt));
	for(i = 0; fmt[i] && o < sizeof(out) - 64; i++){
		u32 s = 0;
		if(fmt[i] != '%'){ out[o++] = fmt[i]; continue; }
		spec[s++] = '%';
		while(fmt[++i] && strchr("-+ #0123456789.l", fmt[i]) && s < sizeof(spec) - 2)
			if(fmt[i] != 'l') spec[s++] = fmt[i];
		if(!fmt[i]) break;
		spec[s++] = fmt[i];
		spec[s] = 0;
		if(fmt[i] == '%'){ out[o++] = '%'; continue; }
		u32 v = argn < 8 ? ARG(argn++) : 0;
		if(fmt[i] == 's'){
			char str[48];
			mem_read_cstr(v, str, sizeof(str));
			o += (u32)snprintf(out + o, sizeof(out) - o, spec, str);
		} else {
			o += (u32)snprintf(out + o, sizeof(out) - o, spec, v);
		}
		if(o >= sizeof(out)) o = sizeof(out) - 1;
	}
	hle_output(out, o);
	RETURN(0);
}

/* ------------------------------------------------------------------ */
/* ThreadManForUser: hilos                                            */
/* ------------------------------------------------------------------ */

static void sceKernelCreateThread(void){
	u32 name = ARG(0), entry = ARG(1), prio = ARG(2), stack_size = ARG(3), attr = ARG(4);
	int i;
	Thread *t;

	if(!name){ RETURN(SCE_KERNEL_ERROR_ERROR); return; }
	if(prio < 0x08 || prio > 0x77){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY); return; }
	if(!mem_valid(entry, 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	for(i = 0; i < MAX_THREADS && threads[i].used; i++);
	if(i == MAX_THREADS){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }

	stack_size = (stack_size + 255) & ~255u;
	if(stack_size < 0x200) stack_size = 0x200;
	u32 stack = kernel_alloc(stack_size, 1, "stack");
	if(!stack){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }

	t = &threads[i];
	memset(t, 0, sizeof(*t));
	t->used = 1;
	if(name) mem_read_cstr(name, t->name, sizeof(t->name));
	t->status = TH_DORMANT;
	/* Los hilos de usuario llevan 0x80000000 y la PSP fuerza el byte bajo
	   (pspautotests threads/threads/create) */
	t->attr = attr | 0x800000FFu;
	t->exit_status = SCE_KERNEL_ERROR_DORMANT;
	t->entry = entry;
	t->prio = t->init_prio = prio;
	t->stack = stack;
	t->stack_size = stack_size;
	t->gp = cur >= 0 ? cpu.r[R_GP] : module_gp;
	/* La PSP rellena la pila con 0xFF */
	memset(mem_ptr(stack, stack_size), 0xFF, stack_size);
	RETURN(thread_uid(i));
}

static void start_thread(int i, u32 arglen, u32 argp){
	Thread *t = &threads[i];
	CpuState *c = &t->ctx;
	u32 top = t->stack + t->stack_size;
	u32 k0 = top - 0x100, sp = k0;

	memset(c, 0, sizeof(*c));

	/* Los últimos 0x100 bytes de la pila guardan un bloque del kernel al que
	   apunta k0: UID del hilo en +0xC0, base de la pila en +0xC8 y dos
	   palabras a -1 en +0xF8 (pspautotests threads/k0). La base de la pila
	   guarda también el UID. */
	memset(mem_ptr(k0, 0x100), 0, 0x100);
	mem_write32(k0 + 0xC0, thread_uid(i));
	mem_write32(k0 + 0xC8, t->stack);
	mem_write32(k0 + 0xF8, 0xFFFFFFFFu);
	mem_write32(k0 + 0xFC, 0xFFFFFFFFu);
	mem_write32(t->stack, thread_uid(i));

	/* Sin puntero de argumentos, el hilo recibe argSize = 0 */
	if(argp && arglen && mem_valid(argp, arglen)){
		sp -= (arglen + 15) & ~15u;
		memmove(mem_ptr(sp, arglen), mem_ptr(argp, arglen), arglen);
		c->r[R_A0] = arglen;
		c->r[R_A1] = sp;
	}
	sp -= 64;
	c->r[R_SP] = sp;
	c->r[R_K0] = k0;
	c->r[R_GP] = t->gp;
	c->r[R_RA] = HLE_KERNEL_TRAMPOLINE;
	c->fcr31 = FCR31_DEFAULT;
	c->pc = t->entry;
	c->npc = t->entry + 4;
	t->exit_status = SCE_KERNEL_ERROR_NOT_DORMANT;
	t->wakeup_count = 0;
	t->prio = t->init_prio;
	make_ready(i);
}

static void sceKernelStartThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(threads[i].status != TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	RETURN(0);
	start_thread(i, ARG(1), ARG(2));
}

static void free_thread(int i){
	kernel_free(threads[i].stack);
	threads[i].used = 0;
}

/* Termina el hilo i (pasa a DORMANT) y despierta a quien esperaba su fin */
static void exit_thread(int i, u32 status, int del){
	int j;
	threads[i].status = TH_DORMANT;
	threads[i].wait = W_NONE;
	threads[i].exit_status = status;
	for(j = 0; j < MAX_THREADS; j++)
		if(threads[j].used && threads[j].status == TH_WAITING &&
		   threads[j].wait == W_THREADEND && threads[j].wait_index == i)
			wake(j, status);
	if(i == cur){
		threads[cur].ctx = cpu;
		cur = -1;
	}
	if(del) free_thread(i);
	request_resched();
}

void kernel_thread_return(void){
	if(cur >= 0) exit_thread(cur, cpu.r[R_V0], 0);
}

static void sceKernelExitThread(void){ if(cur >= 0) exit_thread(cur, ARG(0), 0); }
static void sceKernelExitDeleteThread(void){ if(cur >= 0) exit_thread(cur, ARG(0), 1); }

static void sceKernelDeleteThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(threads[i].status != TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	free_thread(i);
	RETURN(0);
}

static void sceKernelTerminateThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || i == cur || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
	if(threads[i].status == TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_DORMANT); return; }
	RETURN(0);
	exit_thread(i, SCE_KERNEL_ERROR_ERROR, 0);
}

static void sceKernelTerminateDeleteThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || i == cur || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
	RETURN(0);
	exit_thread(i, SCE_KERNEL_ERROR_ERROR, 1);
}

static void sceKernelGetThreadId(void){ RETURN(cur >= 0 ? thread_uid(cur) : 0); }

static void sceKernelGetThreadCurrentPriority(void){ RETURN(cur >= 0 ? threads[cur].prio : 0); }

static void sceKernelChangeThreadPriority(void){
	int i = find_thread(ARG(0));
	u32 prio = ARG(1);
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(prio == 0) prio = threads[cur >= 0 ? cur : i].prio;
	if(prio < 0x08 || prio > 0x77){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY); return; }
	if(threads[i].status == TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_DORMANT); return; }
	threads[i].prio = prio;
	if(threads[i].status == TH_READY) threads[i].ready_seq = ++seq;
	RETURN(0);
	request_resched();
}

static void sceKernelRotateThreadReadyQueue(void){
	/* Cede el turno a otros hilos de la misma prioridad */
	if(cur >= 0){
		threads[cur].ready_seq = ++seq;
		request_resched();
	}
	RETURN(0);
}

static void sceKernelGetThreadExitStatus(void){
	int i = find_thread(ARG(0));
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(threads[i].status != TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	RETURN(threads[i].exit_status);
}

static void sceKernelReferThreadStatus(void){
	int i = find_thread(ARG(0));
	u32 info = ARG(1);
	Thread *t;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(!mem_valid(info, 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	t = &threads[i];
	u32 size = mem_read32(info);
	if(size < 104 || !mem_valid(info, 104)){ RETURN(0x800200C9u /* ILLEGAL_SIZE */); return; }
	memset(mem_ptr(info + 4, 100), 0, 100);
	memcpy(mem_ptr(info + 4, 32), t->name, sizeof(t->name));
	mem_write32(info + 36, t->attr);
	mem_write32(info + 40, (u32)t->status);
	mem_write32(info + 44, t->entry);
	mem_write32(info + 48, t->stack);
	mem_write32(info + 52, t->stack_size);
	mem_write32(info + 56, t->gp);
	mem_write32(info + 60, t->init_prio);
	mem_write32(info + 64, t->prio);
	mem_write32(info + 68, t->status == TH_WAITING ? (u32)(t->wait < 100 ? t->wait : 0) : 0);
	mem_write32(info + 76, (u32)t->wakeup_count);
	mem_write32(info + 80, t->exit_status);
	RETURN(0);
}

static void sceKernelCheckThreadStack(void){
	RETURN(cur >= 0 ? cpu.r[R_SP] - threads[cur].stack : 0);
}

static void sceKernelGetThreadStackFreeSize(void){
	int i = find_thread(ARG(0));
	u32 n = 0;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	/* Cuenta los 0xFF que quedan al fondo de la pila */
	while(n < threads[i].stack_size && mem_read8(threads[i].stack + n) == 0xFF) n++;
	RETURN(n);
}

static void sceKernelSleepThread(void){
	Thread *t = current();
	if(t && t->wakeup_count > 0){ t->wakeup_count--; RETURN(0); return; }
	wait_current(W_SLEEP, 0, 0);
}

static void sceKernelWakeupThread(void){
	int i = find_thread(ARG(0));
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(threads[i].status == TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_DORMANT); return; }
	if(threads[i].status == TH_WAITING && threads[i].wait == W_SLEEP) wake(i, 0);
	else threads[i].wakeup_count++;
	RETURN(0);
}

static void sceKernelCancelWakeupThread(void){
	int i = find_thread(ARG(0));
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	RETURN(threads[i].wakeup_count);
	threads[i].wakeup_count = 0;
}

static void delay_us(u64 us){
	Thread *t = current();
	if(!t) return;
	wait_current(W_DELAY, 0, 0);
	t->wait_until = cpu_cycles + us * CYCLES_PER_US;
}

static void sceKernelDelayThread(void){ delay_us(ARG(0)); }

static void sceKernelDelaySysClockThread(void){
	u32 p = ARG(0);
	if(!mem_valid(p, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	delay_us(((u64)mem_read32(p + 4) << 32) | mem_read32(p));
}

static void sceKernelWaitThreadEnd(void){
	int i = find_thread(ARG(0));
	if(i < 0 || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(i == cur){ RETURN(0x800201A1u /* ILLEGAL_THID */); return; }
	if(threads[i].status == TH_DORMANT){ RETURN(threads[i].exit_status); return; }
	wait_current(W_THREADEND, i, ARG(1));
}

static void sceKernelSuspendDispatchThread(void){ RETURN(1); }
static void sceKernelResumeDispatchThread(void){ RETURN(0); }

/* ------------------------------------------------------------------ */
/* Tiempo                                                             */
/* ------------------------------------------------------------------ */

static void sceKernelGetSystemTimeLow(void){ RETURN((u32)hle_now_us()); }
static void sceKernelGetSystemTimeWide(void){ RETURN64(hle_now_us()); }

static void sceKernelGetSystemTime(void){
	u32 p = ARG(0);
	u64 now = hle_now_us();
	if(!mem_valid(p, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(p, (u32)now);
	mem_write32(p + 4, (u32)(now >> 32));
	RETURN(0);
}

static void sceKernelUSec2SysClock(void){
	u32 p = ARG(1);
	if(mem_valid(p, 8)){ mem_write32(p, ARG(0)); mem_write32(p + 4, 0); }
	RETURN(0);
}

static void sceKernelUSec2SysClockWide(void){ RETURN64(ARG(0)); }

static void sceKernelSysClock2USec(void){
	u32 p = ARG(0), sec = ARG(1), usec = ARG(2);
	u64 v = mem_valid(p, 8) ? ((u64)mem_read32(p + 4) << 32) | mem_read32(p) : 0;
	if(sec) mem_write32(sec, (u32)(v / 1000000));
	if(usec) mem_write32(usec, (u32)(v % 1000000));
	RETURN(0);
}

static void sceKernelSysClock2USecWide(void){
	u64 v = ((u64)ARG(1) << 32) | ARG(0);
	u32 sec = ARG(2), usec = ARG(3);
	if(sec) mem_write32(sec, (u32)(v / 1000000));
	if(usec) mem_write32(usec, (u32)(v % 1000000));
	RETURN(0);
}

/* Fecha base fija (2010-01-01) para que las ejecuciones sean reproducibles */
#define EPOCH_BASE 1262304000u

static void sceKernelLibcTime(void){
	u32 t = EPOCH_BASE + (u32)(hle_now_us() / 1000000);
	if(ARG(0)) mem_write32(ARG(0), t);
	RETURN(t);
}

static void sceKernelLibcClock(void){ RETURN((u32)hle_now_us()); }

static void sceKernelLibcGettimeofday(void){
	u64 now = hle_now_us();
	u32 tv = ARG(0);
	if(tv && mem_valid(tv, 8)){
		mem_write32(tv, EPOCH_BASE + (u32)(now / 1000000));
		mem_write32(tv + 4, (u32)(now % 1000000));
	}
	RETURN(0);
}

/* ------------------------------------------------------------------ */
/* Semáforos                                                          */
/* ------------------------------------------------------------------ */

#define MAX_SEMAS 128

typedef struct {
	int used;
	char name[32];
	u32 attr;
	s32 count, max, init;
} Sema;

static Sema semas[MAX_SEMAS];

static int find_sema(u32 uid){
	int i = uid_index(uid, UID_SEMA, MAX_SEMAS);
	return (i >= 0 && semas[i].used) ? i : -1;
}

static int sema_waiters(int s){
	int i, n = 0;
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING &&
		   threads[i].wait == W_SEMA && threads[i].wait_index == s) n++;
	return n;
}

/* Despierta a quien pueda tomar el semáforo, en orden FIFO o de prioridad */
static void sema_wake_waiters(int s){
	for(;;){
		int i, best = -1;
		for(i = 0; i < MAX_THREADS; i++){
			Thread *t = &threads[i];
			if(!t->used || t->status != TH_WAITING || t->wait != W_SEMA || t->wait_index != s) continue;
			if(best < 0 ||
			   ((semas[s].attr & 0x100) ? t->prio < threads[best].prio : t->wait_seq < threads[best].wait_seq))
				best = i;
		}
		if(best < 0 || (s32)threads[best].wait_a > semas[s].count) return;
		semas[s].count -= (s32)threads[best].wait_a;
		wake(best, 0);
	}
}

static void sceKernelCreateSema(void){
	u32 name = ARG(0), attr = ARG(1);
	s32 init = (s32)ARG(2), max = (s32)ARG(3);
	int i;
	if(!name){ RETURN(SCE_KERNEL_ERROR_ERROR); return; }
	/* El contador inicial puede ser negativo (pspautotests threads/semaphores/signal) */
	if(max <= 0 || init > max){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	for(i = 0; i < MAX_SEMAS && semas[i].used; i++);
	if(i == MAX_SEMAS){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	memset(&semas[i], 0, sizeof(semas[i]));
	semas[i].used = 1;
	mem_read_cstr(name, semas[i].name, sizeof(semas[i].name));
	semas[i].attr = attr;
	semas[i].count = semas[i].init = init;
	semas[i].max = max;
	RETURN(make_uid(UID_SEMA, i));
}

static void sceKernelDeleteSema(void){
	int s = find_sema(ARG(0)), i;
	if(s < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING &&
		   threads[i].wait == W_SEMA && threads[i].wait_index == s)
			wake(i, SCE_KERNEL_ERROR_WAIT_DELETE);
	semas[s].used = 0;
	RETURN(0);
}

static void sceKernelSignalSema(void){
	int s = find_sema(ARG(0));
	s32 n = (s32)ARG(1);
	if(s < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if(semas[s].count + n - sema_waiters(s) > semas[s].max){ RETURN(SCE_KERNEL_ERROR_SEMA_OVF); return; }
	semas[s].count += n;
	RETURN(0);
	sema_wake_waiters(s);
}

static void wait_sema(void){
	int s = find_sema(ARG(0));
	s32 n = (s32)ARG(1);
	if(s < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if(n <= 0 || n > semas[s].max){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if(semas[s].count >= n && !sema_waiters(s)){
		semas[s].count -= n;
		RETURN(0);
		return;
	}
	wait_current(W_SEMA, s, ARG(2));
	if(cur >= 0) threads[cur].wait_a = (u32)n;
}

static void sceKernelPollSema(void){
	int s = find_sema(ARG(0));
	s32 n = (s32)ARG(1);
	if(s < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if(n <= 0){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if(semas[s].count >= n && !sema_waiters(s)){ semas[s].count -= n; RETURN(0); }
	else RETURN(SCE_KERNEL_ERROR_SEMA_ZERO);
}

static void sceKernelReferSemaStatus(void){
	int s = find_sema(ARG(0));
	u32 info = ARG(1);
	if(s < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if(!mem_valid(info, 56)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(info, 56);
	memset(mem_ptr(info + 4, 32), 0, 32);
	memcpy(mem_ptr(info + 4, 32), semas[s].name, 32);
	mem_write32(info + 36, semas[s].attr);
	mem_write32(info + 40, (u32)semas[s].init);
	mem_write32(info + 44, (u32)semas[s].count);
	mem_write32(info + 48, (u32)semas[s].max);
	mem_write32(info + 52, (u32)sema_waiters(s));
	RETURN(0);
}

/* ------------------------------------------------------------------ */
/* Event flags                                                        */
/* ------------------------------------------------------------------ */

#define MAX_EVFS 128
#define EVF_WAITAND      0x00
#define EVF_WAITOR       0x01
#define EVF_WAITCLEARALL 0x10
#define EVF_WAITCLEAR    0x20
#define EVF_ATTR_MULTI   0x200

typedef struct {
	int used;
	char name[32];
	u32 attr, bits, init;
} EventFlag;

static EventFlag evfs[MAX_EVFS];

static int find_evf(u32 uid){
	int i = uid_index(uid, UID_EVF, MAX_EVFS);
	return (i >= 0 && evfs[i].used) ? i : -1;
}

/* Si se cumple la condición, escribe outBits, limpia y devuelve 1 */
static int evf_try(int e, u32 pattern, u32 mode, u32 out_addr){
	u32 bits = evfs[e].bits;
	int ok = (mode & EVF_WAITOR) ? (bits & pattern) != 0 : (bits & pattern) == pattern;
	if(!ok) return 0;
	if(out_addr) mem_write32(out_addr, bits);
	if(mode & EVF_WAITCLEARALL) evfs[e].bits = 0;
	else if(mode & EVF_WAITCLEAR) evfs[e].bits &= ~pattern;
	return 1;
}

static int evf_has_waiters(int e){
	int i, n = 0;
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING &&
		   threads[i].wait == W_EVF && threads[i].wait_index == e) n++;
	return n;
}

/* Al vencer la espera, outBits recibe los bits actuales */
static void evf_timeout(Thread *t){
	if(t->wait_c) mem_write32(t->wait_c, evfs[t->wait_index].bits);
}

static void sceKernelReferEventFlagStatus(void){
	int e = find_evf(ARG(0));
	u32 info = ARG(1);
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	if(!mem_valid(info, 52)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(info, 52);
	memset(mem_ptr(info + 4, 32), 0, 32);
	memcpy(mem_ptr(info + 4, 32), evfs[e].name, 32);
	mem_write32(info + 36, evfs[e].attr);
	mem_write32(info + 40, evfs[e].init);
	mem_write32(info + 44, evfs[e].bits);
	mem_write32(info + 48, (u32)evf_has_waiters(e));
	RETURN(0);
}

static void sceKernelCreateEventFlag(void){
	u32 name = ARG(0), attr = ARG(1), bits = ARG(2);
	int i;
	if(!name){ RETURN(SCE_KERNEL_ERROR_ERROR); return; }
	for(i = 0; i < MAX_EVFS && evfs[i].used; i++);
	if(i == MAX_EVFS){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	memset(&evfs[i], 0, sizeof(evfs[i]));
	evfs[i].used = 1;
	mem_read_cstr(name, evfs[i].name, sizeof(evfs[i].name));
	evfs[i].attr = attr;
	evfs[i].bits = evfs[i].init = bits;
	RETURN(make_uid(UID_EVF, i));
}

static void sceKernelDeleteEventFlag(void){
	int e = find_evf(ARG(0)), i;
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING &&
		   threads[i].wait == W_EVF && threads[i].wait_index == e)
			wake(i, SCE_KERNEL_ERROR_WAIT_DELETE);
	evfs[e].used = 0;
	RETURN(0);
}

static void sceKernelSetEventFlag(void){
	int e = find_evf(ARG(0)), i;
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	evfs[e].bits |= ARG(1);
	RETURN(0);
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(t->used && t->status == TH_WAITING && t->wait == W_EVF && t->wait_index == e &&
		   evf_try(e, t->wait_a, t->wait_b, t->wait_c))
			wake(i, 0);
	}
}

static void sceKernelClearEventFlag(void){
	int e = find_evf(ARG(0));
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	evfs[e].bits &= ARG(1);
	RETURN(0);
}


static void wait_evf(void){
	int e = find_evf(ARG(0));
	u32 pattern = ARG(1), mode = ARG(2), out = ARG(3), timeout = ARG(4);
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	if((mode & ~0x31u) || (mode & 0x30) == 0x30){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MODE); return; }
	if(!pattern){ RETURN(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
	if(!(evfs[e].attr & EVF_ATTR_MULTI) && evf_has_waiters(e)){ RETURN(SCE_KERNEL_ERROR_EVF_MULTI); return; }
	if(evf_try(e, pattern, mode, out)){ RETURN(0); return; }
	wait_current(W_EVF, e, timeout);
	if(cur >= 0){
		threads[cur].wait_a = pattern;
		threads[cur].wait_b = mode;
		threads[cur].wait_c = out;
	}
}

static void sceKernelPollEventFlag(void){
	int e = find_evf(ARG(0));
	u32 pattern = ARG(1), mode = ARG(2), out = ARG(3);
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	if((mode & ~0x31u) || (mode & 0x30) == 0x30){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MODE); return; }
	if(!pattern){ RETURN(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
	if(evf_try(e, pattern, mode, out)){ RETURN(0); return; }
	if(out) mem_write32(out, evfs[e].bits);
	RETURN(SCE_KERNEL_ERROR_EVF_COND);
}

/* ------------------------------------------------------------------ */
/* LwMutex (mutex ligero: su estado vive en memoria del juego)        */
/* ------------------------------------------------------------------ */
/* workarea: s32 count, SceUID owner, u32 attr, s32 waiters, SceUID uid */

#define LWMUTEX_RECURSIVE 0x200

static void sceKernelCreateLwMutex(void){
	u32 wa = ARG(0), attr = ARG(2);
	s32 init = (s32)ARG(3);
	static int counter;
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(wa, (u32)init);
	mem_write32(wa + 4, init > 0 && cur >= 0 ? thread_uid(cur) : 0);
	mem_write32(wa + 8, attr);
	mem_write32(wa + 12, 0);
	mem_write32(wa + 16, make_uid(UID_LWMUTEX, counter++ & 0x7FFF));
	RETURN(0);
}

static void sceKernelDeleteLwMutex(void){
	if(mem_valid(ARG(0), 32)) mem_write32(ARG(0) + 16, 0);
	RETURN(0);
}

static int lwmutex_try(u32 wa, s32 n){
	s32 count = (s32)mem_read32(wa);
	u32 owner = mem_read32(wa + 4), me = cur >= 0 ? thread_uid(cur) : 0;
	if(count == 0){
		mem_write32(wa, (u32)n);
		mem_write32(wa + 4, me);
		return 1;
	}
	if(owner == me && (mem_read32(wa + 8) & LWMUTEX_RECURSIVE)){
		mem_write32(wa, (u32)(count + n));
		return 1;
	}
	return 0;
}

static void sceKernelLockLwMutex(void){
	u32 wa = ARG(0);
	s32 n = (s32)ARG(1);
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	if(lwmutex_try(wa, n)){ RETURN(0); return; }
	wait_current(W_LWMUTEX, 0, ARG(2));
	if(cur >= 0){ threads[cur].wait_a = wa; threads[cur].wait_b = (u32)n; }
	mem_write32(wa + 12, mem_read32(wa + 12) + 1);
}

static void sceKernelTryLockLwMutex(void){
	u32 wa = ARG(0);
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	RETURN(lwmutex_try(wa, (s32)ARG(1)) ? 0 : 0x800201C4u /* LWMUTEX_LOCKED */);
}

static void sceKernelUnlockLwMutex(void){
	u32 wa = ARG(0);
	s32 n = (s32)ARG(1), count;
	int i;
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	count = (s32)mem_read32(wa) - n;
	if(count < 0) count = 0;
	mem_write32(wa, (u32)count);
	RETURN(0);
	if(count) return;
	mem_write32(wa + 4, 0);
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(t->used && t->status == TH_WAITING && t->wait == W_LWMUTEX && t->wait_a == wa){
			int prev = cur;
			cur = i; /* el dueño será el hilo despertado */
			lwmutex_try(wa, (s32)t->wait_b);
			cur = prev;
			mem_write32(wa + 12, mem_read32(wa + 12) - 1);
			wake(i, 0);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Callbacks, interrupciones y varios                                 */
/* ------------------------------------------------------------------ */

static void sceKernelCreateCallback(void){
	static int counter;
	RETURN(make_uid(UID_CALLBACK, counter++ & 0x7FFF));
}

static void sceKernelCpuSuspendIntr(void){ RETURN(1); }
static void sceKernelIsCpuIntrEnable(void){ RETURN(1); }

static void sceKernelStdin(void){ RETURN(0); }
static void sceKernelStdout(void){ RETURN(1); }
static void sceKernelStderr(void){ RETURN(2); }

static void sceKernelExitGame(void){ hle_exit("sceKernelExitGame"); }
static void sceKernelSelfStopUnloadModule(void){ hle_exit("el modulo se descargo"); }
static void sceKernelGetModuleIdByAddress(void){ RETURN(0x04100001); }
static void sceKernelLoadModule(void){ RETURN(SCE_ERROR_FILE_NOT_FOUND); }

static void sceKernelGetGPI(void){ RETURN(0); }

/* ------------------------------------------------------------------ */
/* Arranque                                                           */
/* ------------------------------------------------------------------ */

void kernel_init(const PspModule *mod, const char *exec_path){
	int i;
	u32 len, argp;

	memset(threads, 0, sizeof(threads));
	memset(semas, 0, sizeof(semas));
	memset(evfs, 0, sizeof(evfs));
	memset(blocks, 0, sizeof(blocks));
	cur = -1;
	need_resched = 0;
	seq = 0;
	module_gp = mod->gp;
	user_mem_end = PSP_RAM_BASE + psp_mem.ram_size;

	/* El módulo ocupa desde el inicio de la memoria de usuario */
	add_block(USER_MEM_START, ((mod->load_end + 255) & ~255u) - USER_MEM_START, "module");

	/* Hilo inicial que ejecuta module_start(arglen, argp) con la ruta.
	   Se crea igual que lo haría el juego, con los mismos chequeos. */
	cpu_cycles = 0;
	memset(&cpu, 0, sizeof(cpu));
	memcpy(mem_ptr(HLE_KERNEL_TRAMPOLINE + 0x10, 5), "root", 5);
	cpu.r[R_A0] = HLE_KERNEL_TRAMPOLINE + 0x10;  /* nombre */
	cpu.r[R_A1] = mod->entry;
	cpu.r[R_A2] = 0x20;                          /* prioridad */
	cpu.r[R_A3] = 0x40000;                       /* pila */
	cpu.r[R_T0] = 0;                             /* atributos */
	sceKernelCreateThread();
	i = find_thread(cpu.r[R_V0]);
	if(i < 0){
		hle_exit("no se pudo crear el hilo principal");
		return;
	}

	/* argv[0]: la ruta del ejecutable, en la zona de kernel */
	argp = HLE_KERNEL_TRAMPOLINE + 0x100;
	len = (u32)strlen(exec_path) + 1;
	if(len > 0x100) len = 0x100;
	memcpy(mem_ptr(argp, len), exec_path, len);
	mem_write8(argp + len - 1, 0);
	start_thread(i, len, argp);
	memset(&cpu, 0, sizeof(cpu));
}

void kernel_shutdown(void){
	cur = -1;
}

/* ------------------------------------------------------------------ */
/* Tablas                                                             */
/* ------------------------------------------------------------------ */

static const HleFunction thread_man[] = {
	{ "sceKernelCreateThread", sceKernelCreateThread },
	{ "sceKernelStartThread", sceKernelStartThread },
	{ "sceKernelExitThread", sceKernelExitThread },
	{ "_sceKernelExitThread", sceKernelExitThread },
	{ "sceKernelExitDeleteThread", sceKernelExitDeleteThread },
	{ "sceKernelDeleteThread", sceKernelDeleteThread },
	{ "sceKernelTerminateThread", sceKernelTerminateThread },
	{ "sceKernelTerminateDeleteThread", sceKernelTerminateDeleteThread },
	{ "sceKernelGetThreadId", sceKernelGetThreadId },
	{ "sceKernelGetThreadCurrentPriority", sceKernelGetThreadCurrentPriority },
	{ "sceKernelChangeThreadPriority", sceKernelChangeThreadPriority },
	{ "sceKernelRotateThreadReadyQueue", sceKernelRotateThreadReadyQueue },
	{ "sceKernelGetThreadExitStatus", sceKernelGetThreadExitStatus },
	{ "sceKernelReferThreadStatus", sceKernelReferThreadStatus },
	{ "sceKernelCheckThreadStack", sceKernelCheckThreadStack },
	{ "sceKernelGetThreadStackFreeSize", sceKernelGetThreadStackFreeSize },
	{ "sceKernelSleepThread", sceKernelSleepThread },
	{ "sceKernelSleepThreadCB", sceKernelSleepThread },
	{ "sceKernelWakeupThread", sceKernelWakeupThread },
	{ "sceKernelCancelWakeupThread", sceKernelCancelWakeupThread },
	{ "sceKernelDelayThread", sceKernelDelayThread },
	{ "sceKernelDelayThreadCB", sceKernelDelayThread },
	{ "sceKernelDelaySysClockThread", sceKernelDelaySysClockThread },
	{ "sceKernelDelaySysClockThreadCB", sceKernelDelaySysClockThread },
	{ "sceKernelWaitThreadEnd", sceKernelWaitThreadEnd },
	{ "sceKernelWaitThreadEndCB", sceKernelWaitThreadEnd },
	{ "sceKernelSuspendDispatchThread", sceKernelSuspendDispatchThread },
	{ "sceKernelResumeDispatchThread", sceKernelResumeDispatchThread },
	{ "sceKernelGetSystemTimeLow", sceKernelGetSystemTimeLow },
	{ "sceKernelGetSystemTimeWide", sceKernelGetSystemTimeWide },
	{ "sceKernelGetSystemTime", sceKernelGetSystemTime },
	{ "sceKernelUSec2SysClock", sceKernelUSec2SysClock },
	{ "sceKernelUSec2SysClockWide", sceKernelUSec2SysClockWide },
	{ "sceKernelSysClock2USec", sceKernelSysClock2USec },
	{ "sceKernelSysClock2USecWide", sceKernelSysClock2USecWide },
	{ "sceKernelCreateSema", sceKernelCreateSema },
	{ "sceKernelDeleteSema", sceKernelDeleteSema },
	{ "sceKernelSignalSema", sceKernelSignalSema },
	{ "sceKernelWaitSema", wait_sema },
	{ "sceKernelWaitSemaCB", wait_sema },
	{ "sceKernelPollSema", sceKernelPollSema },
	{ "sceKernelReferSemaStatus", sceKernelReferSemaStatus },
	{ "sceKernelCreateEventFlag", sceKernelCreateEventFlag },
	{ "sceKernelDeleteEventFlag", sceKernelDeleteEventFlag },
	{ "sceKernelSetEventFlag", sceKernelSetEventFlag },
	{ "sceKernelClearEventFlag", sceKernelClearEventFlag },
	{ "sceKernelWaitEventFlag", wait_evf },
	{ "sceKernelWaitEventFlagCB", wait_evf },
	{ "sceKernelPollEventFlag", sceKernelPollEventFlag },
	{ "sceKernelReferEventFlagStatus", sceKernelReferEventFlagStatus },
	{ "sceKernelCreateLwMutex", sceKernelCreateLwMutex },
	{ "sceKernelDeleteLwMutex", sceKernelDeleteLwMutex },
	{ "sceKernelCreateCallback", sceKernelCreateCallback },
	{ "sceKernelCheckCallback", return_zero },
};

static const HleFunction kernel_library[] = {
	{ "sceKernelCpuSuspendIntr", sceKernelCpuSuspendIntr },
	{ "sceKernelCpuResumeIntr", return_zero },
	{ "sceKernelCpuResumeIntrWithSync", return_zero },
	{ "sceKernelIsCpuIntrEnable", sceKernelIsCpuIntrEnable },
	{ "sceKernelIsCpuIntrSuspended", return_zero },
	{ "sceKernelLockLwMutex", sceKernelLockLwMutex },
	{ "sceKernelLockLwMutexCB", sceKernelLockLwMutex },
	{ "sceKernelTryLockLwMutex", sceKernelTryLockLwMutex },
	{ "sceKernelUnlockLwMutex", sceKernelUnlockLwMutex },
};

static const HleFunction sysmem_user[] = {
	{ "sceKernelAllocPartitionMemory", sceKernelAllocPartitionMemory },
	{ "sceKernelFreePartitionMemory", sceKernelFreePartitionMemory },
	{ "sceKernelGetBlockHeadAddr", sceKernelGetBlockHeadAddr },
	{ "sceKernelMaxFreeMemSize", sceKernelMaxFreeMemSize },
	{ "sceKernelTotalFreeMemSize", sceKernelTotalFreeMemSize },
	{ "sceKernelDevkitVersion", sceKernelDevkitVersion },
	{ "sceKernelSetCompiledSdkVersion", return_zero },
	{ "sceKernelGetCompiledSdkVersion", return_zero },
	{ "sceKernelPrintf", sceKernelPrintf },
};

static const HleFunction stdio_user[] = {
	{ "sceKernelStdin", sceKernelStdin },
	{ "sceKernelStdout", sceKernelStdout },
	{ "sceKernelStderr", sceKernelStderr },
};

static const HleFunction loadexec_user[] = {
	{ "sceKernelExitGame", sceKernelExitGame },
	{ "sceKernelExitGameWithStatus", sceKernelExitGame },
	{ "sceKernelRegisterExitCallback", return_zero },
};

static const HleFunction modulemgr_user[] = {
	{ "sceKernelSelfStopUnloadModule", sceKernelSelfStopUnloadModule },
	{ "sceKernelStopUnloadSelfModule", sceKernelSelfStopUnloadModule },
	{ "sceKernelGetModuleIdByAddress", sceKernelGetModuleIdByAddress },
	{ "sceKernelLoadModule", sceKernelLoadModule },
};

static const HleFunction utils_user[] = {
	{ "sceKernelLibcTime", sceKernelLibcTime },
	{ "sceKernelLibcClock", sceKernelLibcClock },
	{ "sceKernelLibcGettimeofday", sceKernelLibcGettimeofday },
	{ "sceKernelDcacheWritebackAll", return_zero },
	{ "sceKernelDcacheWritebackInvalidateAll", return_zero },
	{ "sceKernelDcacheWritebackRange", return_zero },
	{ "sceKernelDcacheWritebackInvalidateRange", return_zero },
	{ "sceKernelDcacheInvalidateRange", return_zero },
	{ "sceKernelIcacheInvalidateAll", return_zero },
	{ "sceKernelIcacheInvalidateRange", return_zero },
	{ "sceKernelGetGPI", sceKernelGetGPI },
	{ "sceKernelSetGPO", return_zero },
};

const HleLibrary hle_kernel_libs[] = {
	HLE_LIBRARY("ThreadManForUser", thread_man),
	HLE_LIBRARY("Kernel_Library", kernel_library),
	HLE_LIBRARY("SysMemUserForUser", sysmem_user),
	HLE_LIBRARY("StdioForUser", stdio_user),
	HLE_LIBRARY("LoadExecForUser", loadexec_user),
	HLE_LIBRARY("ModuleMgrForUser", modulemgr_user),
	HLE_LIBRARY("UtilsForUser", utils_user),
};
const u32 hle_kernel_libs_count = sizeof(hle_kernel_libs) / sizeof(hle_kernel_libs[0]);
