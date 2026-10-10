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
 * evento (fin de un retardo, vblank o evento programado) en vez de gastar
 * ciclos esperando.
 *
 * Eventos e interrupciones siguen el modelo de PPSSPP (CoreTiming y
 * sceKernelInterrupt.cpp, GPLv2+): un evento se dispara en un ciclo dado,
 * y puede levantar una interrupción; las interrupciones pendientes se
 * atienden en cuanto están habilitadas, ejecutando los manejadores del
 * juego en contexto de interrupción.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include "hle/hle.h"
#include "core/memory.h"
#include "cpu/vfpu.h"
#include "core/prof.h"

/* ------------------------------------------------------------------ */
/* UIDs                                                               */
/* ------------------------------------------------------------------ */

enum { UID_THREAD = 1, UID_SEMA, UID_EVF, UID_MEMBLOCK, UID_CALLBACK, UID_LWMUTEX, UID_FPL, UID_MUTEX };

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
	W_FPL = 10,
	W_VBLANK = 100, W_LWMUTEX = 101, W_MUTEX = 106, W_GEDRAW = 102, W_GELIST = 103,
	W_MODULE = 104,     /* sceKernelStartModule: espera el fin de module_start */
	W_HLE = 200         /* + tipo: esperas genéricas del HLE (kernel_wait_object) */
};

/* Un callback en curso en un hilo: el hilo tal como estaba al
   interrumpirlo (justo después del syscall) y la espera que retomará */
#define MAX_CB_DEPTH 2   /* un callback puede hacer una espera CB (PPSSPP) */
typedef struct {
	CpuState cpu;
	int wait, wait_index, has_timeout, cb_wait;
	u64 wait_seq, wait_until;
	u32 wait_a, wait_b, wait_c, delay_ret, timeout_addr;
	int k;              /* callback que se está ejecutando */
} CbFrame;

/* Una llamada al juego como código del hilo (kernel_enqueue_call): el hilo
   tal como estaba al volver del syscall y qué hacer cuando vuelva */
#define MAX_GUEST_CALLS 3
typedef struct {
	CpuState cpu;
	KernelCallDone done;
	u32 data[KERNEL_CALL_DATA];
} GuestCall;

typedef struct {
	int used;
	char name[32];
	int status;
	u32 attr, entry, init_prio, prio;
	u32 stack, stack_size, gp;
	CpuState ctx;
	VfpuState vctx;     /* registros VFPU (al día solo si no es vfpu_owner) */
	u64 ready_seq;      /* orden de llegada a la cola de listos */

	int wait;           /* W_* */
	int wait_index;     /* objeto por el que espera */
	u64 wait_seq;       /* orden de llegada a la espera */
	int has_timeout;
	u64 wait_until;     /* ciclo en el que vence */
	u32 delay_ret;      /* v0 al despertar de W_DELAY y W_VBLANK */
	u32 timeout_addr;   /* puntero a u32 con microsegundos, o 0 */
	u32 wait_a, wait_b, wait_c; /* datos de la espera (cuenta, patrón...) */

	int wakeup_count;
	u32 exit_status;
	const char *last_hle;   /* última función del HLE que llamó (diagnóstico) */
	int cb_wait;    /* está en una espera CB: sus callbacks pueden ejecutarse */
	int cb_paused;  /* sacado de esa espera para ejecutar callbacks; la retoma */
	int cb_depth;   /* callbacks en curso (cbf[0..cb_depth-1]) */
	CbFrame cbf[MAX_CB_DEPTH];
	int gc_depth;   /* llamadas al juego en curso (gcf[0..gc_depth-1]) */
	GuestCall gcf[MAX_GUEST_CALLS];
	int suspend_pending;  /* sceKernelSuspendThread mientras esperaba */
} Thread;

static Thread threads[MAX_THREADS];
static int cur = -1;           /* hilo que corre ahora, -1 = ocioso */
static int need_resched;
static u64 seq;
static u32 module_gp;

static inline Thread *current(void){ return cur >= 0 ? &threads[cur] : NULL; }

/* Callbacks (más abajo) */
static void resume_after_callbacks(void);
static void forget_thread_callbacks(int i);

/* --- VFPU perezosa ---------------------------------------------------- */
/* `vfpu` tiene los registros VFPU del hilo vfpu_owner (-1: de ninguno). Al
   cambiar de hilo no se copian: si el nuevo hilo usa la VFPU, la primera
   instrucción llama a hle_vfpu_load(), que guarda los del dueño y trae los
   suyos. Los hilos que no usan la VFPU no pagan nada. */
static int vfpu_owner = -1;
u64 kernel_stat_switches, kernel_stat_vfpu_loads, kernel_stat_guest_calls;

/* Una llamada al juego desde el HLE (interrupción, callback del GE...)
   devuelve la VFPU como estaba, igual que PPSSPP; la copia se hace solo si
   la función llega a usar la VFPU. */
typedef struct GuestVfpu { VfpuState state; int owner, taken; struct GuestVfpu *prev; } GuestVfpu;
static GuestVfpu *guest_vfpu;

void hle_vfpu_load(void){
	if(guest_vfpu && !guest_vfpu->taken){
		guest_vfpu->state = vfpu;
		guest_vfpu->owner = vfpu_owner;
		guest_vfpu->taken = 1;
	}
	if(vfpu_owner != cur){
		if(vfpu_owner >= 0) threads[vfpu_owner].vctx = vfpu;
		if(cur >= 0) vfpu = threads[cur].vctx;
		vfpu_owner = cur;
		kernel_stat_vfpu_loads++;
	}
	vfpu_live = 1;
}

static void vfpu_update_live(void){
	vfpu_live = (guest_vfpu && !guest_vfpu->taken) ? 0 : vfpu_owner == cur;
}

/* Los registros VFPU del hilo i dejan de valer (hilo borrado o reiniciado),
   también en las copias de las llamadas al juego en curso */
static void vfpu_forget(int i){
	GuestVfpu *g;
	if(vfpu_owner == i) vfpu_owner = -1;
	for(g = guest_vfpu; g; g = g->prev) if(g->taken && g->owner == i) g->owner = -1;
	vfpu_update_live();
}
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
#define WAIT_TIMEOUT_LATENCY_US  18
#define WAIT_TIMEOUT_DEADLINE_US 12
#define SCE_KERNEL_ERROR_WAIT_CANCEL 0x800201A9u

/* Una espera con plazo que acaba antes escribe lo que le quedaba, menos la
   latencia del temporizador (PPSSPP WriteRemainingTimeout) */
static void write_remaining_timeout(int i){
	Thread *t = &threads[i];
	u64 left;
	if(t->status != TH_WAITING || !t->has_timeout || !t->timeout_addr || !mem_valid(t->timeout_addr, 4)) return;
	left = t->wait_until > cpu_cycles ? t->wait_until - cpu_cycles : 0;
	left = left > WAIT_TIMEOUT_LATENCY_US * CYCLES_PER_US ? left - WAIT_TIMEOUT_LATENCY_US * CYCLES_PER_US : 0;
	mem_write32(t->timeout_addr, (u32)(left / CYCLES_PER_US));
}

static void wake(int i, u32 ret){
	write_remaining_timeout(i);
	thread_ctx(i)->r[R_V0] = ret;
	threads[i].cb_wait = threads[i].cb_paused = 0;
	make_ready(i);
	if(threads[i].suspend_pending){   /* suspendido mientras esperaba */
		threads[i].suspend_pending = 0;
		threads[i].status = TH_SUSPEND;
	}
}

/* El hilo actual pasa a esperar. timeout_addr: puntero a microsegundos
   (0 = sin límite). El valor de retorno por defecto es 0. */
static int in_interrupt;     /* profundidad de kernel_call_guest */
static int intr_running;     /* atendiendo una interrupción */
static int intr_enabled = 1; /* sceKernelCpuSuspendIntr/ResumeIntr */
static int dispatch_enabled = 1;
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT 0x800201A7u

int kernel_in_interrupt(void){ return in_interrupt || intr_running; }
int kernel_interrupts_enabled(void){ return intr_enabled; }
int kernel_dispatch_enabled(void){ return dispatch_enabled && intr_enabled; }

static void wait_current(int type, int index, u32 timeout_addr){
	Thread *t = current();
	if(kernel_in_interrupt() || !kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
	if(!t) return;
	t->status = TH_WAITING;
	t->wait = type;
	t->wait_index = index;
	t->wait_seq = ++seq;
	t->timeout_addr = timeout_addr;
	t->has_timeout = 0;
	if(timeout_addr && mem_valid(timeout_addr, 4)){
		/* Como la PSP (PPSSPP __KernelWaitTimeoutUs): al menos 205 us,
		   más el margen y la latencia del temporizador */
		u32 us = mem_read32(timeout_addr);
		if(us < 205) us = 205;
		t->has_timeout = 1;
		t->wait_until = cpu_cycles + (u64)(us + WAIT_TIMEOUT_DEADLINE_US + WAIT_TIMEOUT_LATENCY_US) * CYCLES_PER_US;
	}
	RETURN(0);
	request_resched();
}

static void schedule(void){
	int i, best = -1;
	need_resched = 0;
	/* Sin dispatch el hilo actual sigue (no puede esperar en ese estado) */
	if(!kernel_dispatch_enabled() && cur >= 0 && threads[cur].status == TH_RUNNING) return;
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
	/* Cambiar de hilo cuesta unos 5 us en la PSP (1200 ciclos desde o hacia
	   ocioso; medido por PPSSPP con threads/scheduling/handoff) */
	if(cur >= 0 || best >= 0) cpu_cycles += (cur < 0 || best < 0) ? 1200 : 1150;
	cur = best;
	if(cur >= 0){
		threads[cur].status = TH_RUNNING;
		cpu = threads[cur].ctx;
		cpu.llbit = 0; /* un cambio de hilo implica una interrupción */
	}
	vfpu_update_live();
	kernel_stat_switches++;
}

static void exit_thread(int i, u32 status, int del);
static void mutex_thread_end(int i);
static void evf_timeout(Thread *t);

/* Despierta las esperas vencidas */
static void process_timeouts(void){
	int i;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING) continue;
		if(t->wait == W_DELAY && cpu_cycles >= t->wait_until) wake(i, t->delay_ret);
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

/* ------------------------------------------------------------------ */
/* Eventos programados                                                */
/* ------------------------------------------------------------------ */

#define MAX_EVENTS 64

typedef struct {
	int used;
	u64 when, order;
	KernelEventFunc fn;
	u64 userdata;
} KEvent;

static KEvent events[MAX_EVENTS];
static u64 event_order;

void kernel_schedule_event(u64 when, KernelEventFunc fn, u64 userdata){
	int i;
	for(i = 0; i < MAX_EVENTS && events[i].used; i++);
	if(i == MAX_EVENTS){
		hle_log("[KERNEL] demasiados eventos programados\n");
		return;
	}
	events[i].used = 1;
	events[i].when = when;
	events[i].order = ++event_order;
	events[i].fn = fn;
	events[i].userdata = userdata;
	/* Que el bucle principal recalcule cuánto puede correr la CPU */
	cpu_stop_requested = 1;
}

s64 kernel_unschedule_event(KernelEventFunc fn, u64 userdata){
	s64 left = 0;
	int i, found = 0;
	for(i = 0; i < MAX_EVENTS; i++){
		if(!events[i].used || events[i].fn != fn || events[i].userdata != userdata) continue;
		if(!found){ left = (s64)(events[i].when - cpu_cycles); found = 1; }
		events[i].used = 0;
	}
	return left;
}

static u64 next_event(void){
	u64 next = ~0ull;
	int i;
	for(i = 0; i < MAX_EVENTS; i++)
		if(events[i].used && events[i].when < next) next = events[i].when;
	return next;
}

/* Dispara, en orden, los eventos vencidos */
static void process_events(void){
	for(;;){
		int i, best = -1;
		for(i = 0; i < MAX_EVENTS; i++){
			if(!events[i].used || events[i].when > cpu_cycles) continue;
			if(best < 0 || events[i].when < events[best].when ||
			   (events[i].when == events[best].when && events[i].order < events[best].order))
				best = i;
		}
		if(best < 0) break;
		events[best].used = 0;
		events[best].fn(events[best].userdata);
	}
}

/* ------------------------------------------------------------------ */
/* Interrupciones                                                     */
/* ------------------------------------------------------------------ */

#define MAX_SUBINTR  32
#define MAX_PENDING  128
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT   0x80020064u
#define SCE_KERNEL_ERROR_ILLEGAL_INTRCODE  0x80020065u
#define SCE_KERNEL_ERROR_CPUDI             0x80020066u
#define SCE_KERNEL_ERROR_FOUND_HANDLER     0x80020067u
#define SCE_KERNEL_ERROR_NOTFOUND_HANDLER  0x80020068u

typedef struct { int used, enabled; u32 handler, arg; } SubIntr;
typedef struct { int intr, sub; } PendingIntr;

static SubIntr subintrs[PSP_NUM_INTR][MAX_SUBINTR];
static const KernelIntrHandler *intr_handlers[PSP_NUM_INTR];
static PendingIntr pending[MAX_PENDING];
static int pending_len;

void kernel_register_intr(int intno, const KernelIntrHandler *h){
	if(intno >= 0 && intno < PSP_NUM_INTR) intr_handlers[intno] = h;
}

int kernel_get_subintr(int intno, int sub, u32 *handler, u32 *arg){
	const SubIntr *si;
	if(intno < 0 || intno >= PSP_NUM_INTR || sub < 0 || sub >= MAX_SUBINTR) return 0;
	si = &subintrs[intno][sub];
	if(!si->used) return 0;
	if(handler) *handler = si->handler;
	if(arg) *arg = si->arg;
	return 1;
}

static void pending_remove(int i){
	memmove(pending + i, pending + i + 1, (size_t)(pending_len - i - 1) * sizeof(pending[0]));
	pending_len--;
}

static void pending_push(int intno, int sub){
	if(pending_len == MAX_PENDING) return;
	pending[pending_len].intr = intno;
	pending[pending_len].sub = sub;
	pending_len++;
}

/* Atiende las interrupciones pendientes, si se puede. Cada una puede
   llamar a una función del juego (con su propia pila, sin poder esperar). */
static void run_interrupts(void){
	int ran = 0;
	while(pending_len > 0 && intr_enabled && !intr_running && !in_interrupt && !hle_has_exited()){
		PendingIntr p = pending[0];
		const KernelIntrHandler *h = intr_handlers[p.intr];
		u32 func = 0, a[3] = { 0, 0, 0 };
		int call;
		intr_running = 1;
		ran = 1;
		if(h && h->run) call = h->run(p.sub, &func, a);
		else {
			call = kernel_get_subintr(p.intr, p.sub, &func, &a[1]);
			a[0] = (u32)p.sub;
		}
		if(!call || !func){
			if(pending_len > 0) pending_remove(0);
			intr_running = 0;
			continue;
		}
		cpu.llbit = 0;
		kernel_call_guest(func, a[0], a[1], a[2]);
		if(pending_len > 0) pending_remove(0);
		if(h && h->result) h->result(p.sub);
		intr_running = 0;
	}
	if(ran) request_resched();
}

int kernel_trigger_interrupt(int intno, int sub){
	int count = 0, i;
	if(intno < 0 || intno >= PSP_NUM_INTR) return 0;
	cpu.llbit = 0; /* volver de una interrupción rompe ll/sc */
	if(sub == INTR_SUB_NONE){
		pending_push(intno, sub);
		count = 1;
	} else {
		for(i = 0; i < MAX_SUBINTR; i++){
			const SubIntr *si = &subintrs[intno][i];
			if((sub == INTR_SUB_ALL || sub == i) && si->used && si->enabled && si->handler){
				pending_push(intno, i);
				count++;
			}
		}
	}
	run_interrupts();
	return count;
}

int kernel_cancel_raised_interrupts(int intno){
	int i = intr_running ? 1 : 0, count = 0;
	while(i < pending_len){
		if(pending[i].intr == intno){ pending_remove(i); count++; }
		else i++;
	}
	return count;
}

int kernel_register_subintr(int intno, int sub, u32 handler, u32 arg){
	SubIntr *si;
	if(intno < 0 || intno >= PSP_NUM_INTR || sub < 0 || sub >= MAX_SUBINTR) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	si = &subintrs[intno][sub];
	if(si->used && si->handler) return (int)SCE_KERNEL_ERROR_FOUND_HANDLER;
	if(!si->used) si->enabled = 0;
	si->used = 1;
	si->handler = handler;
	si->arg = arg;
	return 0;
}

int kernel_release_subintr(int intno, int sub){
	int i = 0;
	SubIntr *si;
	if(intno < 0 || intno >= PSP_NUM_INTR || sub < 0 || sub >= MAX_SUBINTR) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	si = &subintrs[intno][sub];
	if(!si->used || !si->handler) return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	while(i < pending_len){
		if(pending[i].intr == intno && pending[i].sub == sub) pending_remove(i);
		else i++;
	}
	memset(si, 0, sizeof(*si));
	return 0;
}

int kernel_enable_subintr(int intno, int sub, int enable){
	SubIntr *si;
	if(intno < 0 || intno >= PSP_NUM_INTR || sub < 0 || sub >= MAX_SUBINTR) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	si = &subintrs[intno][sub];
	if(!si->used){
		if(!enable) return 0;
		si->used = 1;
		si->handler = si->arg = 0;
	}
	si->enabled = enable;
	return 0;
}

/* Quién puede poner manejadores en cada interrupción, como lo ve un
   programa de usuario en una PSP 6.61 (medido por PPSSPP). */
enum { ACCESS_NO_HANDLER, ACCESS_NO_SUBS, ACCESS_KERNEL_SUBS, ACCESS_USER };
static int intr_user_access(u32 intno){
	switch(intno){
	case PSP_GE_INTR: case PSP_VBLANK_INTR: return ACCESS_USER;
	case 4: case 6: case 21: return ACCESS_KERNEL_SUBS;
	case 7: case 10: case 12: case 15: case 16: case 17: case 18: case 19: case 20: case 22:
	case 23: case 24: case 26: case 31: case 36: case 50: case 56: case 57: case 58: case 59:
	case 60: case 61: case 65:
		return ACCESS_NO_SUBS;
	default: return ACCESS_NO_HANDLER;
	}
}

static void sceKernelRegisterSubIntrHandler(void){
	u32 intno = ARG(0), sub = ARG(1);
	if(intno >= PSP_NUM_INTR){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	switch(intr_user_access(intno)){
	case ACCESS_NO_HANDLER: RETURN(SCE_KERNEL_ERROR_NOTFOUND_HANDLER); return;
	case ACCESS_NO_SUBS: case ACCESS_KERNEL_SUBS: RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return;
	default: break;
	}
	if(sub >= MAX_SUBINTR){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	if(intno == PSP_VBLANK_INTR){
		if((sub >= 18 && sub <= 20) || (sub >= 24 && sub <= 26)){ RETURN(SCE_KERNEL_ERROR_FOUND_HANDLER); return; }
		if(sub >= 16){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	}
	RETURN(kernel_register_subintr((int)intno, (int)sub, ARG(2), ARG(3)));
}

static void sceKernelReleaseSubIntrHandler(void){
	u32 intno = ARG(0), sub = ARG(1);
	int access;
	if(intno >= PSP_NUM_INTR){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	access = intr_user_access(intno);
	if(access == ACCESS_NO_HANDLER){ RETURN(SCE_KERNEL_ERROR_NOTFOUND_HANDLER); return; }
	if(access == ACCESS_NO_SUBS || sub >= MAX_SUBINTR){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	if(access == ACCESS_KERNEL_SUBS || (intno == PSP_VBLANK_INTR && sub >= 16)){
		RETURN(SCE_KERNEL_ERROR_NOTFOUND_HANDLER);
		return;
	}
	RETURN(kernel_release_subintr((int)intno, (int)sub));
}

static void sceKernelEnableSubIntr(void){
	u32 intno = ARG(0), sub = ARG(1);
	if(intno >= PSP_NUM_INTR || sub >= MAX_SUBINTR){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	RETURN(kernel_enable_subintr((int)intno, (int)sub, 1));
}

static void sceKernelDisableSubIntr(void){
	u32 intno = ARG(0), sub = ARG(1);
	if(intno >= PSP_NUM_INTR || sub >= MAX_SUBINTR){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE); return; }
	RETURN(kernel_enable_subintr((int)intno, (int)sub, 0));
}

static void sceKernelCpuSuspendIntr(void){
	RETURN(intr_enabled ? 1 : 0);
	intr_enabled = 0;
	kernel_eat_cycles(15);
}

static void sceKernelCpuResumeIntr(void){
	/* mtic a0: solo cuenta el bit 0 */
	if(ARG(0) & 1){
		intr_enabled = 1;
		run_interrupts();
		request_resched();
	} else intr_enabled = 0;
	kernel_eat_cycles(15);
}

static void sceKernelIsCpuIntrEnable(void){ RETURN(intr_enabled); }

int hle_get_intr_enabled(void){ return intr_enabled; }
void hle_set_intr_enabled(int enabled){
	intr_enabled = enabled;
	/* Las interrupciones pendientes se atienden al volver al bucle */
	if(enabled && pending_len) cpu_stop_requested = 1;
}
static void sceKernelIsCpuIntrSuspended(void){ RETURN(ARG(0) == 0 ? 1 : 0); }

static void sceKernelSuspendDispatchThread(void){
	if(!intr_enabled){ RETURN(SCE_KERNEL_ERROR_CPUDI); return; }
	RETURN(dispatch_enabled);
	dispatch_enabled = 0;
	kernel_eat_cycles(940);
}

static void sceKernelResumeDispatchThread(void){
	if(!intr_enabled){ RETURN(SCE_KERNEL_ERROR_CPUDI); return; }
	dispatch_enabled = ARG(0) != 0;
	RETURN(0);
	request_resched();
	kernel_eat_cycles(940);
}

/* Algunas funciones del sistema tardan: el tiempo pasa sin ejecutar nada */
void kernel_eat_cycles(u32 n){
	cpu_cycles += n;
	cpu_stop_requested = 1; /* pueden haber vencido eventos */
}

void kernel_reschedule(void){ request_resched(); }

/* ------------------------------------------------------------------ */
/* Bucle principal                                                    */
/* ------------------------------------------------------------------ */

void kernel_run_until(u64 target){
	while(!hle_has_exited() && cpu_cycles < target){
		u64 limit, ev;
		process_events();
		process_timeouts();
		run_interrupts();
		if(need_resched) schedule();
		/* Un hilo sacado de su espera CB: sus callbacks y de vuelta a esperar */
		if(cur >= 0 && threads[cur].cb_paused){
			resume_after_callbacks();
			continue;
		}
		if(!any_thread_alive()){
			hle_exit("todos los hilos terminaron");
			break;
		}
		limit = next_timeout();
		ev = next_event();
		/* El cambio de hilo gasta tiempo: puede haber vencido algo */
		if(ev <= cpu_cycles || limit <= cpu_cycles) continue;
		if(ev < limit) limit = ev;
		if(limit > target) limit = target;
		if(cur < 0){
			/* Ocioso: saltar al siguiente evento */
			if(limit > cpu_cycles) cpu_cycles = limit;
			continue;
		}
		{
			u64 slice = limit > cpu_cycles ? limit - cpu_cycles : 1;
			if(slice > 1000000) slice = 1000000;
			int old;
			cpu_stop_requested = 0;
			old = prof_switch(PROF_CPU);
			cpu_run((u32)slice);
			prof_switch(old);
		}
	}
}

static int kwait_to_w(int type){
	return type == KWAIT_GE_DRAW ? W_GEDRAW : type == KWAIT_GE_LIST ? W_GELIST : W_HLE + type;
}

/* Esperas por objetos del HLE: el hilo actual espera a (tipo, id) */
void kernel_wait_object(int type, u32 id){
	wait_current(kwait_to_w(type), (int)id, 0);
}

void kernel_wait_object_timeout(int type, u32 id, u32 us){
	Thread *t = current();
	wait_current(kwait_to_w(type), (int)id, 0);
	if(t && t->status == TH_WAITING){
		t->has_timeout = 1;
		t->wait_until = cpu_cycles + (u64)us * CYCLES_PER_US;
	}
}

int kernel_wake_object_mask(int type, u32 mask, u32 ret){
	int w = kwait_to_w(type), woke = 0, i;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(t->used && t->status == TH_WAITING && t->wait == w && ((u32)t->wait_index & mask)){
			wake(i, ret);
			woke++;
		}
	}
	return woke;
}

/* Despierta, por orden de llegada, a los que esperan (tipo, id) */
int kernel_wake_object(int type, u32 id, u32 ret){
	int w = kwait_to_w(type), woke = 0;
	for(;;){
		int i, best = -1;
		for(i = 0; i < MAX_THREADS; i++){
			Thread *t = &threads[i];
			if(!t->used || t->status != TH_WAITING || t->wait != w || t->wait_index != (int)id) continue;
			if(best < 0 || t->wait_seq < threads[best].wait_seq) best = i;
		}
		if(best < 0) break;
		wake(best, ret);
		woke++;
	}
	return woke;
}

void kernel_vblank(void){
	int i;
	cpu.llbit = 0; /* la interrupción de vblank rompe ll/sc */
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_VBLANK)
			wake(i, threads[i].delay_ret);
	kernel_trigger_interrupt(PSP_VBLANK_INTR, INTR_SUB_ALL);
}

void kernel_wait_until(u64 cycle){
	Thread *t = current();
	u32 v0 = cpu.r[R_V0];
	if(!t || kernel_in_interrupt() || !kernel_dispatch_enabled() || cycle <= cpu_cycles) return;
	wait_current(W_DELAY, 0, 0);
	t->wait_until = cycle;
	t->delay_ret = v0;   /* el resultado del syscall se conserva */
	cpu.r[R_V0] = v0;
}

int kernel_wait_vblank(void){ return kernel_wait_vblank_ret(0); }

/* Como kernel_wait_vblank, pero el syscall devuelve ret al despertar (si no,
   sceCtrlReadBufferPositive devolvía 0 muestras y el juego ignoraba el mando) */
int kernel_wait_vblank_ret(u32 ret){
	Thread *t = current();
	wait_current(W_VBLANK, 0, 0);
	if(t && t->status == TH_WAITING && t->wait == W_VBLANK) t->delay_ret = ret;
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
				/* Reiniciar: pos cambió, y el "siguiente" visto hasta ahora
				   puede haberse quedado atrás (bloques pegados): sin esto el
				   hueco salía negativo (GTA LCS: 0xFFFE0000 libres) */
				next = user_mem_end;
				i = -1;
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
		if(!align || (align & (align - 1))){ RETURN(0x800200E4u /* ILLEGAL_ALIGNMENT_SIZE */); return; }
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

/* Versión del SDK con que se compiló el juego: cambia detalles del
   firmware (p. ej. en sceGe). Sale de module_sdk_version o de
   sceKernelSetCompiledSdkVersion*. */
static u32 sdk_version;
u32 kernel_sdk_version(void){ return sdk_version; }
static void sceKernelSetCompiledSdkVersion(void){ sdk_version = ARG(0); RETURN(0); }
static void sceKernelGetCompiledSdkVersion(void){ RETURN(sdk_version); }
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
	vfpu_reset(&t->vctx);
	vfpu_forget(i);   /* sus registros vivos ya no valen */
	c->pc = t->entry;
	c->npc = t->entry + 4;
	t->exit_status = SCE_KERNEL_ERROR_NOT_DORMANT;
	t->wakeup_count = 0;
	t->last_hle = NULL;
	t->cb_wait = t->cb_paused = t->cb_depth = 0;
	t->gc_depth = 0;
	t->prio = t->init_prio;
	make_ready(i);
}

static void sceKernelStartThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(threads[i].status != TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	RETURN(0);
	kernel_eat_cycles(3400);
	start_thread(i, ARG(1), ARG(2));
}

static void free_thread(int i){
	kernel_free(threads[i].stack);
	threads[i].used = 0;
	vfpu_forget(i);
	forget_thread_callbacks(i);
}

/* Termina el hilo i (pasa a DORMANT) y despierta a quien esperaba su fin */
static void exit_thread(int i, u32 status, int del){
	int j;
	threads[i].status = TH_DORMANT;
	threads[i].wait = W_NONE;
	threads[i].exit_status = status;
	mutex_thread_end(i);   /* los mutex que tenía se sueltan */
	for(j = 0; j < MAX_THREADS; j++){
		if(!threads[j].used || threads[j].status != TH_WAITING || threads[j].wait_index != i) continue;
		if(threads[j].wait == W_THREADEND) wake(j, status);
		else if(threads[j].wait == W_MODULE){
			/* sceKernelStartModule devuelve el UID del módulo y el estado de
			   module_start va a *status; el hilo de module_start se borra */
			if(threads[j].wait_b && mem_valid(threads[j].wait_b, 4)) mem_write32(threads[j].wait_b, status);
			wake(j, threads[j].wait_a);
			del = 1;
		}
	}
	if(i == cur){
		threads[cur].ctx = cpu;
		cur = -1;
	}
	if(del) free_thread(i);
	request_resched();
}

/* --- Llamadas al juego desde el HLE (como interrupciones) ------------- */

static int callback_done;

void kernel_callback_return(void){
	callback_done = 1;
	cpu_stop_requested = 1;
}

u32 kernel_module_gp(void){ return module_gp; }

u32 kernel_call_guest(u32 func, u32 a0, u32 a1, u32 a2){
	return kernel_call_guest_sp(func, 0, a0, a1, a2);
}

u32 kernel_call_guest_sp(u32 func, u32 sp, u32 a0, u32 a1, u32 a2){
	CpuState saved = cpu;
	int saved_done = callback_done, saved_stop = cpu_stop_requested;
	GuestVfpu gv;
	u32 ret;

	if(!func || !mem_valid(func, 4)) return 0;
	kernel_stat_guest_calls++;
	gv.taken = 0;
	gv.prev = guest_vfpu;
	guest_vfpu = &gv;
	vfpu_live = 0;
	in_interrupt++;
	cpu.r[R_A0] = a0;
	cpu.r[R_A1] = a1;
	cpu.r[R_A2] = a2;
	cpu.r[R_SP] = sp ? sp : HLE_INTERRUPT_STACK_TOP - 64 - (u32)(in_interrupt - 1) * 0x2000;
	cpu.r[R_GP] = module_gp;
	cpu.r[R_RA] = HLE_CALLBACK_TRAMPOLINE;
	cpu.pc = func;
	cpu.npc = func + 4;
	callback_done = 0;
	/* Los cambios de hilo que pida la función esperan a que termine */
	while(!callback_done && !hle_has_exited()){
		int old = prof_switch(PROF_CPU);
		cpu_stop_requested = 0;
		cpu_run(100000);
		prof_switch(old);
	}
	ret = cpu.r[R_V0];
	cpu = saved;
	guest_vfpu = gv.prev;
	if(gv.taken){
		vfpu = gv.state;
		vfpu_owner = gv.owner;
	}
	vfpu_update_live();
	callback_done = saved_done;
	(void)saved_stop;
	cpu_stop_requested = 1; /* que el bucle principal revise eventos y cambios de hilo */
	in_interrupt--;
	return ret;
}

/* --- Llamadas al juego como código del hilo (PPSSPP hleEnqueueCall) --- */

static u32 enqueue_count;

u32 kernel_enqueue_count(void){ return enqueue_count; }

static int enqueue_call(u32 func, u32 sp, u32 a0, u32 a1, u32 a2, KernelCallDone done, const u32 *data){
	Thread *t = current();
	GuestCall *f;
	if(!t || kernel_in_interrupt() || t->gc_depth >= MAX_GUEST_CALLS || !func || !mem_valid(func, 4)) return -1;
	f = &t->gcf[t->gc_depth++];
	/* cpu está justo después del syscall: ahí se vuelve */
	f->cpu = cpu;
	f->done = done;
	if(data) memcpy(f->data, data, sizeof(f->data));
	else memset(f->data, 0, sizeof(f->data));
	cpu.r[R_A0] = a0;
	cpu.r[R_A1] = a1;
	cpu.r[R_A2] = a2;
	/* En la pila del hilo, por debajo de lo que esté usando */
	cpu.r[R_SP] = sp ? sp : (f->cpu.r[R_SP] - 64) & ~15u;
	cpu.r[R_RA] = HLE_GUEST_CALL_TRAMPOLINE;
	cpu.pc = func;
	cpu.npc = func + 4;
	kernel_stat_guest_calls++;
	enqueue_count++;
	cpu_stop_requested = 1;
	return 0;
}

int kernel_enqueue_call(u32 func, u32 a0, u32 a1, u32 a2, KernelCallDone done, const u32 *data){
	return enqueue_call(func, 0, a0, a1, a2, done, data);
}

/* sceKernelExtendThreadStack(size, entry, arg): ejecuta entry(arg) con una
   pila nueva de size bytes y devuelve lo que devuelva (PPSSPP). La libc
   de muchos juegos (DBZ Tenkaichi Tag Team) arranca así. */
static u32 extend_stack_done(u32 ret, u32 *d){
	kernel_free(d[0]);
	return ret;
}

static void sceKernelExtendThreadStack(void){
	u32 size = ARG(0), entry = ARG(1), stack;
	Thread *t = current();
	u32 d[KERNEL_CALL_DATA] = { 0 };
	if(size < 512){ RETURN(0x80020194u /* ILLEGAL_STACK_SIZE */); return; }
	if(!t){ RETURN(0xFFFFFFFFu); return; }
	stack = kernel_alloc(size, 1, "extended");
	if(!stack){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	/* Como una pila de hilo: llena de 0xFF y el UID del hilo al principio */
	memset(mem_ptr(stack, size), 0xFF, size);
	mem_write32(stack, thread_uid(cur));
	d[0] = stack;
	if(enqueue_call(entry, (stack + size - 0x10) & ~15u, ARG(2), 0, 0, extend_stack_done, d)){
		kernel_free(stack);
		RETURN(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
	}
}

/* El trampolín: la función del juego volvió. El hilo sigue donde estaba
   al volver del syscall, con lo que diga done como valor de retorno. */
void kernel_guest_call_return(void){
	Thread *t = current();
	GuestCall f;
	u32 ret, v1, result;
	int depth;
	if(!t || !t->gc_depth){
		cpu_fault("vuelta de una llamada al juego sin llamada", cpu.pc - 4, 0);
		return;
	}
	ret = cpu.r[R_V0];
	v1 = cpu.r[R_V1];
	f = t->gcf[--t->gc_depth];
	depth = t->gc_depth;
	cpu = f.cpu;
	cpu.r[R_V1] = v1;   /* lo que dejó en v1 también llega (PPSSPP) */
	result = f.done ? f.done(ret, f.data) : ret;
	/* Si done encadenó otra llamada, el valor lo dará la última */
	if(t->gc_depth == depth) cpu.r[R_V0] = result;
	cpu_stop_requested = 1;
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
	kernel_eat_cycles(330);
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

/* --- Hilos creados por el HLE (module_start) ------------------------------ */

u32 kernel_create_thread(const char *name, u32 entry, u32 prio, u32 stack_size, u32 attr, u32 gp){
	u32 saved[5], uid;
	int i;
	/* El nombre va en la zona de kernel, como el del hilo principal */
	u32 name_addr = HLE_KERNEL_TRAMPOLINE + 0x200;
	size_t n = strlen(name);
	if(n > 31) n = 31;
	memcpy(mem_ptr(name_addr, 32), name, n);
	mem_write8(name_addr + (u32)n, 0);
	saved[0] = cpu.r[R_A0]; saved[1] = cpu.r[R_A1]; saved[2] = cpu.r[R_A2]; saved[3] = cpu.r[R_A3]; saved[4] = cpu.r[R_T0];
	cpu.r[R_A0] = name_addr; cpu.r[R_A1] = entry; cpu.r[R_A2] = prio; cpu.r[R_A3] = stack_size; cpu.r[R_T0] = attr;
	sceKernelCreateThread();
	uid = cpu.r[R_V0];
	cpu.r[R_A0] = saved[0]; cpu.r[R_A1] = saved[1]; cpu.r[R_A2] = saved[2]; cpu.r[R_A3] = saved[3]; cpu.r[R_T0] = saved[4];
	i = find_thread(uid);
	if(i >= 0 && gp) threads[i].gp = gp;
	return uid;
}

int kernel_start_thread(u32 uid, u32 arglen, u32 argp){
	int i = find_thread(uid);
	if(i < 0 || uid == 0 || threads[i].status != TH_DORMANT) return -1;
	start_thread(i, arglen, argp);
	return 0;
}

void kernel_wait_module_start(u32 thread, u32 ret, u32 status_addr){
	int i = find_thread(thread);
	Thread *t = current();
	if(i < 0 || !t || i == cur){ RETURN(ret); return; }
	wait_current(W_MODULE, i, 0);
	t->wait_a = ret;
	t->wait_b = status_addr;
}

/* sceKernelSuspendThread / ResumeThread: un hilo suspendido no se
   planifica; si estaba esperando, lo queda también al despertar */
static void sceKernelSuspendThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || ARG(0) == 0 || i == cur){ RETURN(i < 0 ? SCE_KERNEL_ERROR_UNKNOWN_THID : SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
	if(threads[i].status == TH_DORMANT){ RETURN(SCE_KERNEL_ERROR_DORMANT); return; }
	if(threads[i].status == TH_SUSPEND){ RETURN(0x800201A3u /* SUSPEND */); return; }
	if(threads[i].status == TH_READY) threads[i].status = TH_SUSPEND;
	else threads[i].suspend_pending = 1;   /* al despertar quedará suspendido */
	RETURN(0);
}

static void sceKernelResumeThread(void){
	int i = find_thread(ARG(0));
	if(i < 0 || ARG(0) == 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if(threads[i].status == TH_SUSPEND) make_ready(i);
	else if(threads[i].suspend_pending) threads[i].suspend_pending = 0;
	else { RETURN(0x800201A4u); return; }
	RETURN(0);
}

static void sceKernelChangeCurrentThreadAttr(void){
	Thread *t = current();
	u32 clear = ARG(0), set = ARG(1);
	/* Solo VFPU y similares; no se pueden cambiar a modo kernel */
	if((clear | set) & 0xF0000000u & ~0x80000000u){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
	if(t) t->attr = (t->attr & ~clear) | set;
	RETURN(0);
}

static void sceKernelSleepThread(void){
	Thread *t = current();
	if(t && t->wakeup_count > 0){ t->wakeup_count--; RETURN(0); return; }
	wait_current(W_SLEEP, 0, 0);
}

static void sceKernelWakeupThread(void){
	int i = ARG(0) ? find_thread(ARG(0)) : -1;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	/* No se puede despertar a sí mismo (PPSSPP) */
	if(i == cur){ RETURN(0x80020197u /* ILLEGAL_THID */); return; }
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

/* Un retardo nunca despierta justo a tiempo: como mínimo 210 us y, si no,
   10 us de más (medido por PPSSPP en hardware) */
static void delay_us(u64 us){
	Thread *t = current();
	if(!t) return;
	us = us < 200 ? 210 : us + 10;
	wait_current(W_DELAY, 0, 0);
	t->wait_until = cpu_cycles + us * CYCLES_PER_US;
	t->delay_ret = 0;
}

static void sceKernelDelayThread(void){
	kernel_eat_cycles(2000);
	delay_us(ARG(0));
}

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


/* ------------------------------------------------------------------ */
/* Tiempo                                                             */
/* ------------------------------------------------------------------ */

/* Los costes en ciclos de estas llamadas son los medidos por PPSSPP en
   hardware (sceKernelTime.cpp): deciden en qué momento cae una interrupción */
static void sceKernelGetSystemTimeLow(void){ RETURN((u32)hle_now_us()); kernel_eat_cycles(165); }
static void sceKernelGetSystemTimeWide(void){ RETURN64(hle_now_us()); kernel_eat_cycles(250); }

static void sceKernelGetSystemTime(void){
	u32 p = ARG(0);
	u64 now = hle_now_us();
	if(!mem_valid(p, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(p, (u32)now);
	mem_write32(p + 4, (u32)(now >> 32));
	RETURN(0);
	kernel_eat_cycles(265);
}

static void sceKernelUSec2SysClock(void){
	u32 p = ARG(1);
	if(mem_valid(p, 8)){ mem_write32(p, ARG(0)); mem_write32(p + 4, 0); }
	RETURN(0);
	kernel_eat_cycles(165);
}

static void sceKernelUSec2SysClockWide(void){ RETURN64(ARG(0)); kernel_eat_cycles(150); }

static void sceKernelSysClock2USec(void){
	u32 p = ARG(0), sec = ARG(1), usec = ARG(2);
	u64 v = mem_valid(p, 8) ? ((u64)mem_read32(p + 4) << 32) | mem_read32(p) : 0;
	if(sec) mem_write32(sec, (u32)(v / 1000000));
	if(usec) mem_write32(usec, (u32)(v % 1000000));
	RETURN(0);
	kernel_eat_cycles(415);
}

static void sceKernelSysClock2USecWide(void){
	u64 v = ((u64)ARG(1) << 32) | ARG(0);
	u32 sec = ARG(2), usec = ARG(3);
	if(sec) mem_write32(sec, (u32)(v / 1000000));
	if(usec) mem_write32(usec, (u32)(v % 1000000));
	RETURN(0);
	kernel_eat_cycles(385);
}


static void sceKernelLibcTime(void){
	u32 t = HLE_EPOCH_BASE + (u32)(hle_now_us() / 1000000);
	if(ARG(0)) mem_write32(ARG(0), t);
	RETURN(t);
	kernel_eat_cycles(3385);
}

static void sceKernelLibcClock(void){ RETURN((u32)hle_now_us()); kernel_eat_cycles(330); }

static void sceKernelLibcGettimeofday(void){
	u64 now = hle_now_us();
	u32 tv = ARG(0);
	if(tv && mem_valid(tv, 8)){
		mem_write32(tv, HLE_EPOCH_BASE + (u32)(now / 1000000));
		mem_write32(tv + 4, (u32)(now % 1000000));
	}
	RETURN(0);
	kernel_eat_cycles(1885);
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

static void sceKernelCancelSema(void){
	int s = find_sema(ARG(0)), i, woke = 0;
	s32 n = (s32)ARG(1);
	if(s < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if(n > semas[s].max){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if(mem_valid(ARG(2), 4)) mem_write32(ARG(2), (u32)sema_waiters(s));
	semas[s].count = n < 0 ? semas[s].init : n;
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING &&
		   threads[i].wait == W_SEMA && threads[i].wait_index == s){
			wake(i, SCE_KERNEL_ERROR_WAIT_CANCEL);
			woke = 1;
		}
	RETURN(0);
	if(woke) request_resched();
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

#define MAX_EVFS 1024
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
	if(t->wait_c && mem_valid(t->wait_c, 4)) mem_write32(t->wait_c, evfs[t->wait_index].bits);
}

static void sceKernelReferEventFlagStatus(void){
	int e = find_evf(ARG(0));
	u32 info = ARG(1);
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	if(!mem_valid(info, 52)){ RETURN(0xFFFFFFFFu); return; }
	RETURN(0);
	if(!mem_read32(info)) return;   /* size 0: no escribe nada */
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
	if((attr & 0x100) || attr >= 0x300){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
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
		   threads[i].wait == W_EVF && threads[i].wait_index == e){
			/* Los que esperaban reciben los bits actuales (PPSSPP) */
			evf_timeout(&threads[i]);
			wake(i, SCE_KERNEL_ERROR_WAIT_DELETE);
		}
	evfs[e].used = 0;
	RETURN(0);
}

static void sceKernelSetEventFlag(void){
	int e = find_evf(ARG(0)), i, last_set = 0;
	u64 last = 0;
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	evfs[e].bits |= ARG(1);
	RETURN(0);
	kernel_eat_cycles(430);
	/* Por orden de llegada, como PPSSPP: con WAITCLEAR el primero puede
	   llevarse los bits y el siguiente ya no los ve */
	for(;;){
		int best = -1;
		for(i = 0; i < MAX_THREADS; i++){
			Thread *t = &threads[i];
			if(!t->used || t->status != TH_WAITING || t->wait != W_EVF || t->wait_index != e) continue;
			if(t->wait_seq <= last && last_set) continue;
			if(best < 0 || t->wait_seq < threads[best].wait_seq) best = i;
		}
		if(best < 0) break;
		last = threads[best].wait_seq;
		last_set = 1;
		if(evf_try(e, threads[best].wait_a, threads[best].wait_b, threads[best].wait_c)) wake(best, 0);
	}
}

static void sceKernelCancelEventFlag(void){
	int e = find_evf(ARG(0)), i, n = 0;
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_EVF && threads[i].wait_index == e) n++;
	if(mem_valid(ARG(2), 4) && !(ARG(2) & 3)) mem_write32(ARG(2), (u32)n);
	evfs[e].bits = ARG(1);
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING || t->wait != W_EVF || t->wait_index != e) continue;
		if(t->wait_c && mem_valid(t->wait_c, 4)) mem_write32(t->wait_c, evfs[e].bits);
		wake(i, SCE_KERNEL_ERROR_WAIT_CANCEL);
	}
	kernel_eat_cycles(580);
	RETURN(0);
	if(n) request_resched();
}

static void sceKernelClearEventFlag(void){
	int e = find_evf(ARG(0));
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	evfs[e].bits &= ARG(1);
	kernel_eat_cycles(430);
	RETURN(0);
}


static void wait_evf(void){
	int e = find_evf(ARG(0));
	u32 pattern = ARG(1), mode = ARG(2), out = ARG(3), timeout = ARG(4);
	/* El orden de las comprobaciones es el de la PSP (PPSSPP) */
	if(mode & ~0x31u){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MODE); return; }
	if(!pattern){ RETURN(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
	if(!kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	kernel_eat_cycles(500);
	if(evf_try(e, pattern, mode, out)){ RETURN(0); return; }
	if(!(evfs[e].attr & EVF_ATTR_MULTI) && evf_has_waiters(e)){ RETURN(SCE_KERNEL_ERROR_EVF_MULTI); return; }
	wait_current(W_EVF, e, timeout);
	if(cur >= 0 && threads[cur].status == TH_WAITING){
		threads[cur].wait_a = pattern;
		threads[cur].wait_b = mode;
		/* Con plazo 0 la PSP no escribe los bits */
		threads[cur].wait_c = (timeout && mem_valid(timeout, 4) && mem_read32(timeout) == 0) ? 0 : out;
	}
}

static void sceKernelPollEventFlag(void){
	int e = find_evf(ARG(0));
	u32 pattern = ARG(1), mode = ARG(2), out = ARG(3);
	/* El orden de las comprobaciones es el de la PSP (PPSSPP) */
	if((mode & ~0x31u) || (mode & 0x30) == 0x30){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MODE); return; }
	if(!pattern){ RETURN(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
	kernel_eat_cycles(360);
	if(e < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	if(evf_try(e, pattern, mode, out)){ RETURN(0); return; }
	if(out && mem_valid(out, 4) && !(out & 3)) mem_write32(out, evfs[e].bits);
	if(!(evfs[e].attr & EVF_ATTR_MULTI) && evf_has_waiters(e)){ RETURN(SCE_KERNEL_ERROR_EVF_MULTI); return; }
	RETURN(SCE_KERNEL_ERROR_EVF_COND);
}

/* ------------------------------------------------------------------ */
/* LwMutex (mutex ligero: su estado vive en memoria del juego)        */
/* ------------------------------------------------------------------ */
/* Como en PPSSPP (sceKernelMutex.cpp): cada uno es además un objeto del
   kernel con UID. El workarea (32 bytes) guarda s32 lockLevel, SceUID
   lockThread, u32 attr, s32 numWaitThreads (no se actualiza), SceUID uid
   y 3 de relleno. Si el workarea se borra o se reutiliza, el UID ya no
   existe y la llamada falla en vez de esperar para siempre. */

#define MAX_LWMUTEXES 2048
#define LWMUTEX_PRIORITY  0x100
#define LWMUTEX_RECURSIVE 0x200
#define LWMUTEX_ERROR_NO_SUCH        0x800201CAu
#define LWMUTEX_ERROR_TRYLOCK_FAILED 0x800201CBu
#define LWMUTEX_ERROR_NOT_LOCKED     0x800201CCu
#define LWMUTEX_ERROR_LOCK_OVERFLOW  0x800201CDu
#define LWMUTEX_ERROR_UNDERFLOW      0x800201CEu
#define LWMUTEX_ERROR_ALREADY_LOCKED 0x800201CFu
#define MUTEX_ERROR_TRYLOCK_FAILED   0x800201C4u
#define SCE_KERNEL_ERROR_ACCESS_ERROR 0x8000020Du

typedef struct {
	int used;
	char name[32];
	u32 attr, uid, workarea;
	s32 init;
} LwMutex;
static LwMutex lwmutexes[MAX_LWMUTEXES];
static u32 lwmutex_gen;

/* El UID lleva una generación (0-30, en los bits 11-15): el de uno
   borrado no vale para el que ocupe su sitio */
static int find_lwmutex(u32 uid){
	int i = uid_index(uid, UID_LWMUTEX, 0xFFFF);
	if(i < 0) return -1;
	i &= 0x7FF;
	return (i < MAX_LWMUTEXES && lwmutexes[i].used && lwmutexes[i].uid == uid) ? i : -1;
}

static void sceKernelCreateLwMutex(void){
	u32 wa = ARG(0), name = ARG(1), attr = ARG(2);
	s32 init = (s32)ARG(3);
	int i;
	if(!name){ RETURN(SCE_KERNEL_ERROR_ERROR); return; }
	if(attr >= 0x400){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
	if(init < 0 || (init > 1 && !(attr & LWMUTEX_RECURSIVE))){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	for(i = 0; i < MAX_LWMUTEXES && lwmutexes[i].used; i++);
	if(i == MAX_LWMUTEXES){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	memset(&lwmutexes[i], 0, sizeof(lwmutexes[i]));
	lwmutexes[i].used = 1;
	mem_read_cstr(name, lwmutexes[i].name, sizeof(lwmutexes[i].name));
	lwmutexes[i].attr = attr;
	lwmutexes[i].workarea = wa;
	lwmutexes[i].init = init;
	lwmutexes[i].uid = make_uid(UID_LWMUTEX, (int)((lwmutex_gen++ % 31) << 11) | i);
	memset(mem_ptr(wa, 32), 0, 32);
	mem_write32(wa, (u32)init);
	mem_write32(wa + 4, init && cur >= 0 ? thread_uid(cur) : 0);
	mem_write32(wa + 8, attr);
	mem_write32(wa + 16, lwmutexes[i].uid);
	RETURN(0);
}

/* El hilo i deja de esperar al lwmutex: con ret = 0 se lo queda */
static void lwmutex_hand_over(u32 wa, int i, u32 ret){
	if(ret == 0){
		mem_write32(wa, threads[i].wait_b);
		mem_write32(wa + 4, thread_uid(i));
	}
	wake(i, ret);
}

/* El siguiente que espera al lwmutex m: por prioridad o por llegada */
static int lwmutex_first_waiter(int m){
	int i, best = -1;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING || t->wait != W_LWMUTEX || t->wait_index != m) continue;
		if(best < 0 ||
		   ((lwmutexes[m].attr & LWMUTEX_PRIORITY) ? t->prio < threads[best].prio : t->wait_seq < threads[best].wait_seq))
			best = i;
	}
	return best;
}

static void sceKernelDeleteLwMutex(void){
	u32 wa = ARG(0);
	int m, i, woke = 0;
	if(!wa || !mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	if((m = find_lwmutex(mem_read32(wa + 16))) < 0){ RETURN(LWMUTEX_ERROR_NO_SUCH); return; }
	while((i = lwmutex_first_waiter(m)) >= 0){
		lwmutex_hand_over(wa, i, SCE_KERNEL_ERROR_WAIT_DELETE);
		woke = 1;
	}
	mem_write32(wa, 0);
	mem_write32(wa + 4, 0xFFFFFFFFu);
	mem_write32(wa + 16, 0xFFFFFFFFu);
	lwmutexes[m].used = 0;
	RETURN(0);
	if(woke) request_resched();
}

/* Toma el lwmutex para el hilo actual: 1 si lo consigue, 0 si hay que
   esperar; *err != 0 si la llamada es un error (volver a tomar uno no
   recursivo propio es un error, no una espera eterna) */
static int lwmutex_try(u32 wa, s32 n, u32 *err){
	s32 level = (s32)mem_read32(wa);
	u32 owner = mem_read32(wa + 4), attr = mem_read32(wa + 8), me = cur >= 0 ? thread_uid(cur) : 0;
	*err = 0;
	if(n <= 0 || (n > 1 && !(attr & LWMUTEX_RECURSIVE))) *err = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	else if(level + n < 0) *err = LWMUTEX_ERROR_LOCK_OVERFLOW;
	else if(mem_read32(wa + 16) == 0xFFFFFFFFu) *err = LWMUTEX_ERROR_NO_SUCH;
	if(*err) return 0;
	if(level == 0){
		/* Libre, pero si alguien lo tuvo: ¿sigue existiendo? */
		if(owner != 0 && find_lwmutex(mem_read32(wa + 16)) < 0){ *err = LWMUTEX_ERROR_NO_SUCH; return 0; }
		mem_write32(wa, (u32)n);
		mem_write32(wa + 4, me);
		return 1;
	}
	if(owner == me){
		if(attr & LWMUTEX_RECURSIVE){
			mem_write32(wa, (u32)(level + n));
			return 1;
		}
		*err = LWMUTEX_ERROR_ALREADY_LOCKED;
	}
	return 0;
}

static void sceKernelLockLwMutex(void){
	u32 wa = ARG(0), err;
	s32 n = (s32)ARG(1);
	int m;
	if(kernel_in_interrupt()){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); return; }
	if(!kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ACCESS_ERROR); return; }
	kernel_eat_cycles(48);
	if(lwmutex_try(wa, n, &err)){ RETURN(0); return; }
	if(err){ RETURN(err); return; }
	if((m = find_lwmutex(mem_read32(wa + 16))) < 0){ RETURN(LWMUTEX_ERROR_NO_SUCH); return; }
	wait_current(W_LWMUTEX, m, ARG(2));
	if(cur >= 0) threads[cur].wait_b = (u32)n;
}

static void sceKernelTryLockLwMutex(void){
	u32 wa = ARG(0), err;
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ACCESS_ERROR); return; }
	kernel_eat_cycles(24);
	/* Sea cual sea el motivo, el mismo error (PPSSPP) */
	RETURN(lwmutex_try(wa, (s32)ARG(1), &err) ? 0 : MUTEX_ERROR_TRYLOCK_FAILED);
}

static void sceKernelTryLockLwMutex_600(void){
	u32 wa = ARG(0), err;
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ACCESS_ERROR); return; }
	kernel_eat_cycles(24);
	if(lwmutex_try(wa, (s32)ARG(1), &err)) RETURN(0);
	else RETURN(err ? err : LWMUTEX_ERROR_TRYLOCK_FAILED);
}

static void sceKernelUnlockLwMutex(void){
	u32 wa = ARG(0);
	s32 n = (s32)ARG(1), level;
	int m, i;
	if(!mem_valid(wa, 32)){ RETURN(SCE_KERNEL_ERROR_ACCESS_ERROR); return; }
	kernel_eat_cycles(28);
	level = (s32)mem_read32(wa);
	if(mem_read32(wa + 16) == 0xFFFFFFFFu){ RETURN(LWMUTEX_ERROR_NO_SUCH); return; }
	if(n <= 0 || (n > 1 && !(mem_read32(wa + 8) & LWMUTEX_RECURSIVE))){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if(level == 0 || mem_read32(wa + 4) != (cur >= 0 ? thread_uid(cur) : 0)){ RETURN(LWMUTEX_ERROR_NOT_LOCKED); return; }
	if(level < n){ RETURN(LWMUTEX_ERROR_UNDERFLOW); return; }
	level -= n;
	mem_write32(wa, (u32)level);
	RETURN(0);
	if(level) return;
	/* Pasa al primero que espera; si no hay nadie (o ya no existe), libre */
	m = find_lwmutex(mem_read32(wa + 16));
	if(m >= 0 && (i = lwmutex_first_waiter(m)) >= 0){
		lwmutex_hand_over(wa, i, 0);
		request_resched();
	} else
		mem_write32(wa + 4, 0);
}

/* SceKernelLwMutexInfo: size, name[32], attr, uid, workarea, initCount,
   currentCount, lockThread, numWaitThreads (64 bytes) */
static u32 lwmutex_refer(u32 uid, u32 info){
	int m = find_lwmutex(uid), i, waiting = 0;
	u32 wa, owner;
	if(m < 0) return LWMUTEX_ERROR_NO_SUCH;
	if(!mem_valid(info, 64)) return 0xFFFFFFFFu;
	if(!mem_read32(info)) return 0;
	wa = lwmutexes[m].workarea;
	owner = mem_read32(wa + 4);
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_LWMUTEX && threads[i].wait_index == m)
			waiting++;
	memset(mem_ptr(info, 64), 0, 64);
	mem_write32(info, 64);
	memcpy(mem_ptr(info + 4, 32), lwmutexes[m].name, strnlen(lwmutexes[m].name, 31));
	mem_write32(info + 36, lwmutexes[m].attr);
	mem_write32(info + 40, lwmutexes[m].uid);
	mem_write32(info + 44, wa);
	mem_write32(info + 48, (u32)lwmutexes[m].init);
	mem_write32(info + 52, mem_read32(wa));
	mem_write32(info + 56, owner ? owner : 0xFFFFFFFFu);
	mem_write32(info + 60, (u32)waiting);
	return 0;
}

static void sceKernelReferLwMutexStatusByID(void){ RETURN(lwmutex_refer(ARG(0), ARG(1))); }
static void sceKernelReferLwMutexStatus(void){
	if(!mem_valid(ARG(0), 32)){ RETURN(SCE_KERNEL_ERROR_ACCESS_ERROR); return; }
	RETURN(lwmutex_refer(mem_read32(ARG(0) + 16), ARG(1)));
}

/* ------------------------------------------------------------------ */
/* Mutex (objetos del kernel)                                         */
/* ------------------------------------------------------------------ */
/* Como PPSSPP (sceKernelMutex.cpp). Al terminar un hilo, los mutex que
   tenía se sueltan y pasan al siguiente que espera. */

#define MAX_MUTEXES 1024
#define MUTEX_ERROR_NO_SUCH          0x800201C3u
#define MUTEX_ERROR_NOT_LOCKED       0x800201C5u
#define MUTEX_ERROR_LOCK_OVERFLOW    0x800201C6u
#define MUTEX_ERROR_UNLOCK_UNDERFLOW 0x800201C7u
#define MUTEX_ERROR_ALREADY_LOCKED   0x800201C8u

typedef struct {
	int used;
	char name[32];
	u32 attr;
	s32 init, level;
	int owner;          /* hilo que lo tiene, o -1 */
} Mutex;
static Mutex mutexes[MAX_MUTEXES];

static int find_mutex(u32 uid){
	int i = uid_index(uid, UID_MUTEX, MAX_MUTEXES);
	return (i >= 0 && mutexes[i].used) ? i : -1;
}

static int mutex_first_waiter(int m){
	int i, best = -1;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING || t->wait != W_MUTEX || t->wait_index != m) continue;
		if(best < 0 ||
		   ((mutexes[m].attr & LWMUTEX_PRIORITY) ? t->prio < threads[best].prio : t->wait_seq < threads[best].wait_seq))
			best = i;
	}
	return best;
}

static int mutex_waiters(int m){
	int i, n = 0;
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_MUTEX && threads[i].wait_index == m) n++;
	return n;
}

/* Libre: pasa al primero que espera (con lo que pidió), o queda sin dueño */
static int mutex_release(int m){
	int i = mutex_first_waiter(m);
	if(i < 0){ mutexes[m].owner = -1; return 0; }
	mutexes[m].owner = i;
	mutexes[m].level = (s32)threads[i].wait_b;
	wake(i, 0);
	return 1;
}

static void mutex_thread_end(int i){
	int m;
	for(m = 0; m < MAX_MUTEXES; m++)
		if(mutexes[m].used && mutexes[m].owner == i && mutexes[m].level){
			mutexes[m].level = 0;
			mutex_release(m);
		}
}

/* ¿Se puede tomar? 1 sí, 0 hay que esperar; *err si es un error */
static int mutex_check(int m, s32 n, u32 *err){
	int recursive = (mutexes[m].attr & LWMUTEX_RECURSIVE) != 0;
	*err = 0;
	if(n <= 0 || (n > 1 && !recursive)) *err = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	else if(n + mutexes[m].level < 0) *err = MUTEX_ERROR_LOCK_OVERFLOW;
	else if(mutexes[m].owner == cur && mutexes[m].level){
		if(recursive) return 1;
		*err = MUTEX_ERROR_ALREADY_LOCKED;
	} else if(mutexes[m].level == 0) return 1;
	return 0;
}

static int mutex_lock(int m, s32 n, u32 *err){
	if(!mutex_check(m, n, err)) return 0;
	if(mutexes[m].level == 0) mutexes[m].level = n;
	else mutexes[m].level += n;
	mutexes[m].owner = cur;
	return 1;
}

static void sceKernelCreateMutex(void){
	u32 name = ARG(0), attr = ARG(1);
	s32 init = (s32)ARG(2);
	int i;
	if(!name){ RETURN(SCE_KERNEL_ERROR_ERROR); return; }
	if(attr & ~0xBFFu){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ATTR); return; }
	if(init < 0 || (init > 1 && !(attr & LWMUTEX_RECURSIVE))){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	for(i = 0; i < MAX_MUTEXES && mutexes[i].used; i++);
	if(i == MAX_MUTEXES){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	memset(&mutexes[i], 0, sizeof(mutexes[i]));
	mutexes[i].used = 1;
	mem_read_cstr(name, mutexes[i].name, sizeof(mutexes[i].name));
	mutexes[i].attr = attr;
	mutexes[i].init = mutexes[i].level = init;
	mutexes[i].owner = init ? cur : -1;
	RETURN(make_uid(UID_MUTEX, i));
}

static void sceKernelDeleteMutex(void){
	int m = find_mutex(ARG(0)), i, woke = 0;
	if(m < 0){ RETURN(MUTEX_ERROR_NO_SUCH); return; }
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_MUTEX && threads[i].wait_index == m){
			wake(i, SCE_KERNEL_ERROR_WAIT_DELETE);
			woke = 1;
		}
	mutexes[m].used = 0;
	RETURN(0);
	if(woke) request_resched();
}

static void lock_mutex(void){
	u32 err;
	s32 n = (s32)ARG(1);
	int m;
	if(ARG(0) == 0x80020001u && !ARG(2)){ RETURN(0); return; }   /* PPSSPP */
	if((m = find_mutex(ARG(0))) < 0){ RETURN(MUTEX_ERROR_NO_SUCH); return; }
	if(mutex_lock(m, n, &err)){ RETURN(0); return; }
	if(err){ RETURN(err); return; }
	wait_current(W_MUTEX, m, ARG(2));
	if(cur >= 0) threads[cur].wait_b = (u32)n;
}
static void sceKernelLockMutex(void){ lock_mutex(); }
/* Con error no se atienden callbacks; si no, sí, aunque lo tome sin
   esperar (PPSSPP) */
static void sceKernelLockMutexCB(void){
	u32 err = 0;
	int m = find_mutex(ARG(0));
	if(m >= 0 && !mutex_check(m, (s32)ARG(1), &err) && err){ RETURN(err); return; }
	kernel_cb_wait(lock_mutex);
}

static void sceKernelTryLockMutex(void){
	u32 err;
	int m = find_mutex(ARG(0));
	if(m < 0){ RETURN(MUTEX_ERROR_NO_SUCH); return; }
	if(mutex_lock(m, (s32)ARG(1), &err)) RETURN(0);
	else RETURN(err ? err : MUTEX_ERROR_TRYLOCK_FAILED);
}

static void sceKernelUnlockMutex(void){
	s32 n = (s32)ARG(1);
	int m;
	if(ARG(0) == 0x80020001u){ RETURN(0); return; }
	if((m = find_mutex(ARG(0))) < 0){ RETURN(MUTEX_ERROR_NO_SUCH); return; }
	if(n <= 0 || (n > 1 && !(mutexes[m].attr & LWMUTEX_RECURSIVE))){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if(mutexes[m].level == 0 || mutexes[m].owner != cur){ RETURN(MUTEX_ERROR_NOT_LOCKED); return; }
	if(mutexes[m].level < n){ RETURN(MUTEX_ERROR_UNLOCK_UNDERFLOW); return; }
	mutexes[m].level -= n;
	RETURN(0);
	if(mutexes[m].level == 0 && mutex_release(m)) request_resched();
}

static void sceKernelCancelMutex(void){
	int m = find_mutex(ARG(0)), i, woke = 0;
	s32 n = (s32)ARG(1);
	u32 err = 0;
	if(m < 0){ RETURN(MUTEX_ERROR_NO_SUCH); return; }
	if(n > 0 && !mutex_check(m, n, &err) && err &&
	   err != MUTEX_ERROR_LOCK_OVERFLOW && err != MUTEX_ERROR_ALREADY_LOCKED){ RETURN(err); return; }
	if(mem_valid(ARG(2), 4) && !(ARG(2) & 3)) mem_write32(ARG(2), (u32)mutex_waiters(m));
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_MUTEX && threads[i].wait_index == m){
			wake(i, SCE_KERNEL_ERROR_WAIT_CANCEL);
			woke = 1;
		}
	if(n <= 0){ mutexes[m].level = 0; mutexes[m].owner = -1; }
	else { mutexes[m].level = n; mutexes[m].owner = cur; }
	RETURN(0);
	if(woke) request_resched();
}

/* SceKernelMutexInfo: size, name[32], attr, initCount, currentCount,
   lockThread, numWaitThreads (56 bytes) */
static void sceKernelReferMutexStatus(void){
	int m = find_mutex(ARG(0));
	u32 info = ARG(1);
	if(m < 0){ RETURN(MUTEX_ERROR_NO_SUCH); return; }
	if(!mem_valid(info, 56)){ RETURN(0xFFFFFFFFu); return; }
	RETURN(0);
	if(!mem_read32(info)) return;
	memset(mem_ptr(info, 56), 0, 56);
	mem_write32(info, 56);
	memcpy(mem_ptr(info + 4, 32), mutexes[m].name, strnlen(mutexes[m].name, 31));
	mem_write32(info + 36, mutexes[m].attr);
	mem_write32(info + 40, (u32)mutexes[m].init);
	mem_write32(info + 44, (u32)mutexes[m].level);
	mem_write32(info + 48, mutexes[m].owner >= 0 && mutexes[m].level ? thread_uid(mutexes[m].owner) : 0xFFFFFFFFu);
	mem_write32(info + 52, (u32)mutex_waiters(m));
}

/* ------------------------------------------------------------------ */
/* Pools de bloques fijos (FPL)                                       */
/* ------------------------------------------------------------------ */

#define MAX_FPLS 64
#define SCE_KERNEL_ERROR_UNKNOWN_FPLID 0x8002019Du
#define SCE_KERNEL_ERROR_ILLEGAL_FPL_BLOCK 0x800201B6u
#define FPL_ATTR_PRIORITY 0x100
#define FPL_ATTR_HIGHMEM  0x4000

typedef struct {
	int used;
	char name[32];
	u32 attr, block_size, aligned, num_blocks, base;
	u8 *taken;
} Fpl;

static Fpl fpls[MAX_FPLS];

static int find_fpl(u32 uid){
	int i = uid_index(uid, UID_FPL, MAX_FPLS);
	return (i >= 0 && fpls[i].used) ? i : -1;
}

static int fpl_take(Fpl *f){
	u32 b;
	for(b = 0; b < f->num_blocks; b++)
		if(!f->taken[b]){ f->taken[b] = 1; return (int)b; }
	return -1;
}

/* El primer hilo que espera (por llegada o prioridad) */
static int fpl_first_waiter(int fi){
	int i, best = -1;
	for(i = 0; i < MAX_THREADS; i++){
		Thread *t = &threads[i];
		if(!t->used || t->status != TH_WAITING || t->wait != W_FPL || t->wait_index != fi) continue;
		if(best < 0 || ((fpls[fi].attr & FPL_ATTR_PRIORITY) ? t->prio < threads[best].prio : t->wait_seq < threads[best].wait_seq))
			best = i;
	}
	return best;
}

static void sceKernelCreateFpl(void){
	u32 name = ARG(0), part = ARG(1), attr = ARG(2), bs = ARG(3), n = ARG(4), opt = ARG(5), align = 4, total;
	int i;
	Fpl *f;
	if(!name){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	if(part < 1 || part > 6){ RETURN(0x800200D6u); return; }
	if(bs == 0 || n == 0 || (u64)((bs + 3) & ~3u) * n > 0x01800000u){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE); return; }
	if(opt && mem_valid(opt, 8) && mem_read32(opt) >= 4) align = mem_read32(opt + 4);
	if(align & (align - 1)){ RETURN(0x800200D2u); return; }
	if(align < 4) align = 4;
	for(i = 0; i < MAX_FPLS && fpls[i].used; i++);
	if(i == MAX_FPLS){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	f = &fpls[i];
	memset(f, 0, sizeof(*f));
	f->aligned = (bs + align - 1) & ~(align - 1);
	total = f->aligned * n;
	f->base = kernel_alloc(total, (attr & FPL_ATTR_HIGHMEM) != 0, "fpl");
	f->taken = calloc(n, 1);
	if(!f->base || !f->taken){
		if(f->base) kernel_free(f->base);
		free(f->taken);
		f->taken = NULL;
		RETURN(SCE_KERNEL_ERROR_NO_MEMORY);
		return;
	}
	f->used = 1;
	mem_read_cstr(name, f->name, sizeof(f->name));
	f->attr = attr;
	f->block_size = bs;
	f->num_blocks = n;
	RETURN(make_uid(UID_FPL, i));
}

static void fpl_wake_all(int fi, u32 ret){
	int i;
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_FPL && threads[i].wait_index == fi)
			wake(i, ret);
}

static void sceKernelDeleteFpl(void){
	int fi = find_fpl(ARG(0));
	if(fi < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	fpl_wake_all(fi, SCE_KERNEL_ERROR_WAIT_DELETE);
	kernel_free(fpls[fi].base);
	free(fpls[fi].taken);
	memset(&fpls[fi], 0, sizeof(fpls[fi]));
	RETURN(0);
}

static void allocate_fpl(int can_wait){
	int fi = find_fpl(ARG(0)), b;
	u32 out = ARG(1);
	if(fi < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	if(!mem_valid(out, 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	b = fpl_first_waiter(fi) < 0 ? fpl_take(&fpls[fi]) : -1;
	if(b >= 0){
		mem_write32(out, fpls[fi].base + (u32)b * fpls[fi].aligned);
		RETURN(0);
		return;
	}
	if(!can_wait){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	wait_current(W_FPL, fi, ARG(2));
	if(cur >= 0) threads[cur].wait_a = out;
}

static void sceKernelAllocateFpl(void){ allocate_fpl(1); }
static void sceKernelTryAllocateFpl(void){ allocate_fpl(0); }

static void sceKernelFreeFpl(void){
	int fi = find_fpl(ARG(0)), w;
	u32 addr = ARG(1), b;
	Fpl *f;
	if(fi < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	f = &fpls[fi];
	b = (addr - f->base) / f->aligned;
	if(addr < f->base || b >= f->num_blocks || (addr - f->base) % f->aligned || !f->taken[b]){
		RETURN(SCE_KERNEL_ERROR_ILLEGAL_FPL_BLOCK);
		return;
	}
	f->taken[b] = 0;
	/* El bloque pasa directamente al primero que esperaba */
	w = fpl_first_waiter(fi);
	if(w >= 0){
		int nb = fpl_take(f);
		mem_write32(threads[w].wait_a, f->base + (u32)nb * f->aligned);
		wake(w, 0);
	}
	RETURN(0);
}

static void sceKernelCancelFpl(void){
	int fi = find_fpl(ARG(0)), i, n = 0;
	if(fi < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	for(i = 0; i < MAX_THREADS; i++)
		if(threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_FPL && threads[i].wait_index == fi){
			wake(i, SCE_KERNEL_ERROR_WAIT_CANCEL);
			n++;
		}
	if(ARG(1) && mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)n);
	RETURN(0);
}

static void sceKernelReferFplStatus(void){
	int fi = find_fpl(ARG(0)), i, waiting = 0;
	u32 info = ARG(1), b, free_blocks = 0;
	if(fi < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	if(!mem_valid(info, 56)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	for(b = 0; b < fpls[fi].num_blocks; b++) free_blocks += !fpls[fi].taken[b];
	for(i = 0; i < MAX_THREADS; i++)
		waiting += threads[i].used && threads[i].status == TH_WAITING && threads[i].wait == W_FPL && threads[i].wait_index == fi;
	if(mem_read32(info) != 0){
		memset(mem_ptr(info + 4, 52), 0, 52);
		memcpy(mem_ptr(info + 4, 32), fpls[fi].name, strlen(fpls[fi].name));
		mem_write32(info + 36, fpls[fi].attr);
		mem_write32(info + 40, fpls[fi].block_size);
		mem_write32(info + 44, fpls[fi].num_blocks);
		mem_write32(info + 48, free_blocks);
		mem_write32(info + 52, (u32)waiting);
	}
	RETURN(0);
}

/* ------------------------------------------------------------------ */
/* Callbacks, interrupciones y varios                                 */
/* ------------------------------------------------------------------ */

/* Un callback pertenece al hilo que lo crea. Se notifica (el sistema o el
   juego) y se ejecuta en ese hilo cuando está en una espera CB
   (sceKernelSleepThreadCB, DelayThreadCB, WaitSemaCB...) o llama a
   sceKernelCheckCallback; luego el hilo vuelve a su espera. Si el callback
   devuelve algo distinto de 0, se borra. Como en PPSSPP
   (sceKernelThread.cpp). */
#define MAX_CALLBACKS 1024
#define SCE_KERNEL_ERROR_UNKNOWN_CBID 0x800201A1u
#ifndef SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT 0x800200D2u
#endif
typedef struct {
	int used;
	char name[32];
	int thread;
	u32 entry, common;
	s32 notify_count, notify_arg;
	u64 seq;            /* orden de creación: en ese orden se ejecutan */
} Callback;
static Callback callbacks[MAX_CALLBACKS];
static u32 exit_callback;

static int find_callback(u32 uid){
	int i = uid_index(uid, UID_CALLBACK, MAX_CALLBACKS);
	return (i >= 0 && callbacks[i].used) ? i : -1;
}

int kernel_is_callback(u32 uid){ return find_callback(uid) >= 0; }

/* Los callbacks de un hilo borrado desaparecen con él */
static void forget_thread_callbacks(int i){
	int k;
	for(k = 0; k < MAX_CALLBACKS; k++) if(callbacks[k].used && callbacks[k].thread == i) callbacks[k].used = 0;
}

static u32 (*hle_recheck[16])(u32 id, int *done);

void kernel_set_wait_recheck(int type, u32 (*fn)(u32 id, int *done)){
	if(type > 0 && type < 16) hle_recheck[type] = fn;
}

/* Vuelve a mirar si la espera del hilo i (el actual) ya se cumple: tras
   sus callbacks, la espera sigue, pero lo que pasó mientras tanto (un
   semáforo libre, un objeto borrado...) cuenta, como en PPSSPP */
static void recheck_wait(int i){
	Thread *t = &threads[i];
	switch(t->wait){
	case W_SLEEP:
		if(t->wakeup_count > 0){ t->wakeup_count--; wake(i, 0); }
		break;
	case W_DELAY:
		if(cpu_cycles >= t->wait_until) wake(i, t->delay_ret);
		break;
	case W_SEMA:
		if(!semas[t->wait_index].used) wake(i, SCE_KERNEL_ERROR_WAIT_DELETE);
		else sema_wake_waiters(t->wait_index);
		break;
	case W_EVF:
		if(!evfs[t->wait_index].used) wake(i, SCE_KERNEL_ERROR_WAIT_DELETE);
		else if(evf_try(t->wait_index, t->wait_a, t->wait_b, t->wait_c)) wake(i, 0);
		break;
	case W_THREADEND:
		if(!threads[t->wait_index].used || threads[t->wait_index].status == TH_DORMANT)
			wake(i, threads[t->wait_index].exit_status);
		break;
	case W_MUTEX: {
		int m = t->wait_index;
		if(!mutexes[m].used){ wake(i, SCE_KERNEL_ERROR_WAIT_DELETE); break; }
		if(mutexes[m].level == 0){
			mutexes[m].owner = i;
			mutexes[m].level = (s32)t->wait_b;
			wake(i, 0);
		}
		break;
	}
	case W_LWMUTEX: {
		int m = t->wait_index;
		u32 wa;
		if(!lwmutexes[m].used){ wake(i, SCE_KERNEL_ERROR_WAIT_DELETE); break; }
		wa = lwmutexes[m].workarea;
		if(mem_read32(wa) == 0) lwmutex_hand_over(wa, i, 0);
		break;
	}
	case W_FPL: {
		int fi = t->wait_index, b;
		if(!fpls[fi].used){ wake(i, SCE_KERNEL_ERROR_WAIT_DELETE); break; }
		if(fpl_first_waiter(fi) != i || (b = fpl_take(&fpls[fi])) < 0) break;
		mem_write32(t->wait_a, fpls[fi].base + (u32)b * fpls[fi].aligned);
		wake(i, 0);
		break;
	}
	default:
		if(t->wait > W_HLE && t->wait < W_HLE + 16 && hle_recheck[t->wait - W_HLE]){
			int done = 0;
			u32 ret = hle_recheck[t->wait - W_HLE]((u32)t->wait_index, &done);
			if(done) wake(i, ret);
		}
		break;
	}
}

/* El siguiente callback notificado del hilo i, o -1 */
static int next_pending_callback(int i){
	int k, best = -1;
	for(k = 0; k < MAX_CALLBACKS; k++)
		if(callbacks[k].used && callbacks[k].thread == i && callbacks[k].notify_count > 0 &&
		   (best < 0 || callbacks[k].seq < callbacks[best].seq))
			best = k;
	return best;
}

/* Pone el hilo actual a ejecutar el callback k: a0 = veces notificado,
   a1 = último argumento, a2 = el del creador; vuelve al trampolín */
static void enter_callback(int k){
	Thread *t = &threads[cur];
	Callback *c = &callbacks[k];
	CbFrame *f = &t->cbf[t->cb_depth - 1];
	f->k = k;
	cpu.r[R_A0] = (u32)c->notify_count;
	cpu.r[R_A1] = (u32)c->notify_arg;
	cpu.r[R_A2] = c->common;
	c->notify_count = 0;
	c->notify_arg = 0;
	cpu.r[R_SP] = (f->cpu.r[R_SP] - 64) & ~15u;
	cpu.r[R_RA] = HLE_THREAD_CB_TRAMPOLINE;
	cpu.pc = c->entry;
	cpu.npc = c->entry + 4;
	kernel_stat_guest_calls++;
	/* Preparar la llamada cuesta ~8 us en la PSP (PPSSPP) */
	cpu_cycles += 1800;
	cpu_stop_requested = 1;
}

/* Interrumpe el hilo actual (dentro de un syscall) para ejecutar el
   callback k. El callback es código normal del hilo: puede esperar, y
   mientras tanto corren los demás hilos. Al volver (trampolín), el hilo
   sigue donde estaba y, si esperaba, retoma la espera. */
static void start_callback(int k){
	Thread *t = &threads[cur];
	CbFrame *f = &t->cbf[t->cb_depth++];
	f->cpu = cpu;
	f->wait = t->wait; f->wait_index = t->wait_index; f->has_timeout = t->has_timeout; f->cb_wait = t->cb_wait;
	f->wait_seq = t->wait_seq; f->wait_until = t->wait_until;
	f->wait_a = t->wait_a; f->wait_b = t->wait_b; f->wait_c = t->wait_c;
	f->delay_ret = t->delay_ret; f->timeout_addr = t->timeout_addr;
	t->status = TH_RUNNING;
	t->wait = W_NONE;
	t->has_timeout = 0;
	t->cb_wait = t->cb_paused = 0;
	enter_callback(k);
}

static int can_run_callbacks(void){
	return cur >= 0 && threads[cur].cb_depth < MAX_CB_DEPTH && !kernel_in_interrupt() && !hle_has_exited();
}

/* El trampolín: un callback del hilo actual ha vuelto */
void kernel_thread_cb_return(void){
	Thread *t;
	CbFrame *f;
	int k, i = cur;
	if(cur < 0 || !threads[cur].cb_depth) return;
	t = &threads[cur];
	f = &t->cbf[t->cb_depth - 1];
	k = f->k;
	/* Si devuelve algo distinto de 0, el callback se borra */
	if(cpu.r[R_V0] != 0 && callbacks[k].used && callbacks[k].thread == i) callbacks[k].used = 0;
	k = next_pending_callback(i);
	if(k >= 0 && !hle_has_exited()){ enter_callback(k); return; }
	cpu = f->cpu;
	t->cb_depth--;
	if(f->wait != W_NONE){
		t->status = TH_WAITING;
		t->wait = f->wait; t->wait_index = f->wait_index; t->has_timeout = f->has_timeout; t->cb_wait = f->cb_wait;
		t->wait_seq = f->wait_seq; t->wait_until = f->wait_until;
		t->wait_a = f->wait_a; t->wait_b = f->wait_b; t->wait_c = f->wait_c;
		t->delay_ret = f->delay_ret; t->timeout_addr = f->timeout_addr;
		recheck_wait(i);
	}
	request_resched();
}

static void notify_callback(int k, s32 arg){
	Callback *c = &callbacks[k];
	Thread *t = &threads[c->thread];
	c->notify_count++;
	c->notify_arg = arg;
	/* Un hilo en espera CB sale de ella para ejecutar sus callbacks cuando
	   le toque correr; después la retoma */
	if(t->used && t->status == TH_WAITING && t->cb_wait && !t->cb_paused && t->cb_depth < MAX_CB_DEPTH){
		t->cb_paused = 1;
		t->status = TH_READY;
		t->ready_seq = ++seq;
		request_resched();
	}
}

int kernel_notify_callback(u32 uid, s32 arg){
	int k = find_callback(uid);
	if(k < 0) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	notify_callback(k, arg);
	return 0;
}

/* El hilo actual, sacado de su espera CB, ejecuta sus callbacks y luego
   vuelve a esperar (o termina la espera si ya se cumple) */
static void resume_after_callbacks(void){
	Thread *t = &threads[cur];
	int k = next_pending_callback(cur);
	t->cb_paused = 0;
	if(k >= 0 && can_run_callbacks()){
		start_callback(k);
		return;
	}
	t->status = TH_WAITING;
	recheck_wait(cur);
	request_resched();
}

/* Las variantes CB de las esperas: la espera empieza y, si hay callbacks
   pendientes, se ejecutan ya (con la espera en pausa), como PPSSPP
   (hleCheckCurrentCallbacks + __KernelForceCallbacks) */
static void cb_wait(void (*fn)(void), int only_if_waiting){
	Thread *t;
	int k;
	fn();
	t = current();
	if(!t) return;
	if(t->status == TH_WAITING) t->cb_wait = 1;
	else if(only_if_waiting) return;
	if(!can_run_callbacks() || (k = next_pending_callback(cur)) < 0) return;
	start_callback(k);
}

void kernel_cb_wait(void (*fn)(void)){ cb_wait(fn, 0); }

static void sceKernelCreateCallback(void){
	u32 name = ARG(0), entry = ARG(1);
	int i;
	if(!name){ RETURN(SCE_KERNEL_ERROR_ERROR); return; }
	if(entry & 0xF0000000u){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	for(i = 0; i < MAX_CALLBACKS && callbacks[i].used; i++);
	if(i == MAX_CALLBACKS){ RETURN(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	memset(&callbacks[i], 0, sizeof(callbacks[i]));
	callbacks[i].used = 1;
	mem_read_cstr(name, callbacks[i].name, sizeof(callbacks[i].name));
	callbacks[i].thread = cur;
	callbacks[i].entry = entry;
	callbacks[i].common = ARG(2);
	callbacks[i].seq = ++seq;
	RETURN(make_uid(UID_CALLBACK, i));
}

static void sceKernelDeleteCallback(void){
	int k = find_callback(ARG(0));
	if(k < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
	callbacks[k].used = 0;
	RETURN(0);
}

static void sceKernelNotifyCallback(void){
	int k = find_callback(ARG(0));
	if(k < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
	notify_callback(k, (s32)ARG(1));
	RETURN(0);
}

static void sceKernelCancelCallback(void){
	int k = find_callback(ARG(0));
	if(k < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
	callbacks[k].notify_count = callbacks[k].notify_arg = 0;
	RETURN(0);
}

static void sceKernelGetCallbackCount(void){
	int k = find_callback(ARG(0));
	if(k < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
	RETURN((u32)callbacks[k].notify_count);
}

static void sceKernelReferCallbackStatus(void){
	int k = find_callback(ARG(0));
	u32 p = ARG(1);
	const Callback *c;
	if(k < 0){ RETURN(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
	c = &callbacks[k];
	if(mem_valid(p, 56) && mem_read32(p) != 0){
		/* size, name[32], threadId, entry, common, notifyCount, notifyArg */
		mem_write32(p, 56);
		memset(mem_ptr(p + 4, 32), 0, 32);
		memcpy(mem_ptr(p + 4, 32), c->name, strlen(c->name));
		mem_write32(p + 36, c->thread >= 0 ? thread_uid(c->thread) : 0);
		mem_write32(p + 40, c->entry);
		mem_write32(p + 44, c->common);
		mem_write32(p + 48, (u32)c->notify_count);
		mem_write32(p + 52, (u32)c->notify_arg);
	}
	RETURN(0);
}

/* Ejecuta los callbacks pendientes del hilo; devuelve 1 si había alguno
   (el valor se ve cuando terminan) */
static void sceKernelCheckCallback(void){
	Thread *t = current();
	int k;
	if(t && t->cb_depth){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); kernel_eat_cycles(230); return; }
	k = t && can_run_callbacks() ? next_pending_callback(cur) : -1;
	RETURN(k >= 0 ? 1 : 0);
	kernel_eat_cycles(230);
	if(k >= 0) start_callback(k);
}

static void sceKernelRegisterExitCallback(void){
	/* Un UID inválido (también 0) solo es error desde el SDK 3.95 (PPSSPP) */
	if(find_callback(ARG(0)) < 0){
		RETURN(sdk_version >= 0x3090500 ? SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT : 0);
		return;
	}
	exit_callback = ARG(0);
	RETURN(0);
}

static void cb_sleep(void){ kernel_cb_wait(sceKernelSleepThread); }
static void cb_delay(void){ kernel_cb_wait(sceKernelDelayThread); }
static void cb_delay_sysclock(void){ kernel_cb_wait(sceKernelDelaySysClockThread); }
static void cb_wait_thread_end(void){ kernel_cb_wait(sceKernelWaitThreadEnd); }
static void cb_wait_sema(void){ kernel_cb_wait(wait_sema); }
static void cb_wait_evf(void){ kernel_cb_wait(wait_evf); }
static void cb_allocate_fpl(void){ kernel_cb_wait(sceKernelAllocateFpl); }
/* Si lo toma sin esperar, los callbacks no se atienden (PPSSPP) */
static void cb_lock_lwmutex(void){ cb_wait(sceKernelLockLwMutex, 1); }


static void sceKernelStdin(void){ RETURN(0); }
static void sceKernelStdout(void){ RETURN(1); }
static void sceKernelStderr(void){ RETURN(2); }

static void sceKernelExitGame(void){ hle_exit("sceKernelExitGame"); }

static void sceKernelGetGPI(void){ RETURN(0); }

/* Mersenne Twister estándar con el estado en memoria del juego:
   u32 índice, u32 estado[624] */
#define MT_N 624
#define MT_M 397

static void sceKernelUtilsMt19937Init(void){
	u32 ctx = ARG(0), seed = ARG(1), i;
	if(!mem_valid(ctx, 4 + MT_N * 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(ctx + 4, seed);
	for(i = 1; i < MT_N; i++){
		u32 prev = mem_read32(ctx + 4 + (i - 1) * 4);
		mem_write32(ctx + 4 + i * 4, 1812433253u * (prev ^ (prev >> 30)) + i);
	}
	mem_write32(ctx, MT_N);
	RETURN(0);
}

static void sceKernelUtilsMt19937UInt(void){
	u32 ctx = ARG(0), index, y;
	if(!mem_valid(ctx, 4 + MT_N * 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	index = mem_read32(ctx);
	if(index >= MT_N){
		u32 i;
		for(i = 0; i < MT_N; i++){
			u32 a = mem_read32(ctx + 4 + i * 4);
			u32 b = mem_read32(ctx + 4 + ((i + 1) % MT_N) * 4);
			u32 c = mem_read32(ctx + 4 + ((i + MT_M) % MT_N) * 4);
			y = (a & 0x80000000u) | (b & 0x7FFFFFFFu);
			mem_write32(ctx + 4 + i * 4, c ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0));
		}
		index = 0;
	}
	y = mem_read32(ctx + 4 + index * 4);
	mem_write32(ctx, index + 1);
	y ^= y >> 11;
	y ^= (y << 7) & 0x9D2C5680u;
	y ^= (y << 15) & 0xEFC60000u;
	y ^= y >> 18;
	RETURN(y);
}

/* ------------------------------------------------------------------ */
/* Arranque                                                           */
/* ------------------------------------------------------------------ */

void kernel_init(const PspModule *mod, const char *exec_path){
	int i;
	u32 len, argp;

	memset(threads, 0, sizeof(threads));
	for(i = 0; i < MAX_FPLS; i++) free(fpls[i].taken);
	memset(fpls, 0, sizeof(fpls));
	memset(lwmutexes, 0, sizeof(lwmutexes));
	memset(mutexes, 0, sizeof(mutexes));
	memset(semas, 0, sizeof(semas));
	memset(evfs, 0, sizeof(evfs));
	memset(blocks, 0, sizeof(blocks));
	memset(events, 0, sizeof(events));
	memset(subintrs, 0, sizeof(subintrs));
	memset(callbacks, 0, sizeof(callbacks));
	exit_callback = 0;
	pending_len = 0;
	intr_running = in_interrupt = 0;
	intr_enabled = dispatch_enabled = 1;
	sdk_version = mod->sdk_version;
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
	vfpu_reset(&vfpu);
	vfpu_owner = -1;
	vfpu_live = 0;
	guest_vfpu = NULL;
	kernel_stat_switches = kernel_stat_vfpu_loads = kernel_stat_guest_calls = 0;
	memcpy(mem_ptr(HLE_KERNEL_TRAMPOLINE + 0x10, 5), "root", 5);
	cpu.r[R_A0] = HLE_KERNEL_TRAMPOLINE + 0x10;  /* nombre */
	cpu.r[R_A1] = mod->entry;
	/* Prioridad 0x20, pila 0x40000 y modo usuario, salvo que el módulo
	   pida otra cosa en module_start_thread_parameter (PPSSPP) */
	cpu.r[R_A2] = mod->start_prio ? mod->start_prio : 0x20;
	cpu.r[R_A3] = mod->start_stack ? mod->start_stack : 0x40000;
	cpu.r[R_T0] = mod->start_attr & 0x0FFFFFFFu;
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

/* ------------------------------------------------------------------ */
/* Diagnóstico: qué hace cada hilo (para wiisp.log)                   */
/* ------------------------------------------------------------------ */

void kernel_note_hle_call(const char *name){
	if(cur >= 0) threads[cur].last_hle = name;
}

int kernel_current_thread(void){ return cur; }

static const char *status_name(int st){
	switch(st){
	case TH_RUNNING: return "corriendo";
	case TH_READY: return "listo";
	case TH_WAITING: return "esperando";
	case TH_SUSPEND: return "suspendido";
	case TH_WAITING | TH_SUSPEND: return "esperando+suspendido";
	case TH_DORMANT: return "dormant";
	default: return "?";
	}
}

void kernel_dump_state(void){
	int i, j;
	hle_log("[ESTADO] %.1f s emulados, hilo actual %d, interrupciones %s, dispatch %s\n",
	        (double)cpu_cycles / (CYCLES_PER_US * 1e6), cur, intr_enabled ? "si" : "NO",
	        dispatch_enabled ? "si" : "NO");
	for(i = 0; i < MAX_THREADS; i++){
		const Thread *t = &threads[i];
		char det[160];
		const CpuState *c = (i == cur) ? &cpu : &t->ctx;
		if(!t->used) continue;
		det[0] = 0;
		if(t->status & TH_WAITING){
			switch(t->wait){
			case W_SLEEP: snprintf(det, sizeof(det), "sleep (despertares %d)", t->wakeup_count); break;
			case W_DELAY: snprintf(det, sizeof(det), "delay"); break;
			case W_SEMA:
				snprintf(det, sizeof(det), "sema \"%s\" cuenta %d/%d, pide %u", semas[t->wait_index].name,
				         (int)semas[t->wait_index].count, (int)semas[t->wait_index].max, t->wait_a);
				break;
			case W_EVF:
				snprintf(det, sizeof(det), "evf \"%s\" bits %08X, patron %08X modo %X", evfs[t->wait_index].name,
				         evfs[t->wait_index].bits, t->wait_a, t->wait_b);
				break;
			case W_THREADEND:
				snprintf(det, sizeof(det), "fin del hilo \"%s\"", threads[t->wait_index].name);
				break;
			case W_FPL: snprintf(det, sizeof(det), "fpl \"%s\"", fpls[t->wait_index].name); break;
			case W_VBLANK: snprintf(det, sizeof(det), "vblank"); break;
			case W_MUTEX: snprintf(det, sizeof(det), "mutex \"%s\"", mutexes[t->wait_index].name); break;
			case W_LWMUTEX:
				snprintf(det, sizeof(det), "lwmutex \"%s\" (%08X)", lwmutexes[t->wait_index].name,
				         lwmutexes[t->wait_index].workarea);
				break;
			case W_GEDRAW: snprintf(det, sizeof(det), "GE drawsync"); break;
			case W_GELIST: snprintf(det, sizeof(det), "GE lista %d", t->wait_index); break;
			case W_MODULE: snprintf(det, sizeof(det), "module_start del hilo \"%s\"", threads[t->wait_index].name); break;
			default: snprintf(det, sizeof(det), "HLE tipo %d obj %d", t->wait - W_HLE, t->wait_index); break;
			}
			if(t->has_timeout){
				size_t l = strlen(det);
				snprintf(det + l, sizeof(det) - l, ", vence en %lld us",
				         (long long)((s64)(t->wait_until - cpu_cycles) / CYCLES_PER_US));
			} else if(t->wait == W_DELAY){
				size_t l = strlen(det);
				snprintf(det + l, sizeof(det) - l, ", quedan %lld us",
				         (long long)((s64)(t->wait_until - cpu_cycles) / CYCLES_PER_US));
			}
		}
		hle_log("[HILO] %d \"%s\" prio %u attr %08X %s%s%s pc %08X ra %08X ultima %s%s%s\n", i, t->name, t->prio,
		        t->attr, status_name(t->status), det[0] ? ": " : "", det, c->pc, c->r[R_RA],
		        t->last_hle ? t->last_hle : "-",
		        t->gc_depth ? " (dentro de una funcion del juego llamada por el HLE)" : "",
		        t->cb_depth ? " (en un callback)" : "");
	}
	for(i = 0; i < PSP_NUM_INTR; i++)
		for(j = 0; j < MAX_SUBINTR; j++)
			if(subintrs[i][j].used)
				hle_log("[INTR] %d.%d manejador %08X arg %08X %s\n", i, j, subintrs[i][j].handler,
				        subintrs[i][j].arg, subintrs[i][j].enabled ? "activo" : "inactivo");
	for(i = 0; i < MAX_CALLBACKS; i++)
		if(callbacks[i].used)
			hle_log("[CALLBACK] \"%s\" hilo %d funcion %08X notificado %d (arg %08X)\n", callbacks[i].name,
			        callbacks[i].thread, callbacks[i].entry, (int)callbacks[i].notify_count, (u32)callbacks[i].notify_arg);
	for(i = 0; i < MAX_SEMAS; i++)
		if(semas[i].used) hle_log("[SEMA] \"%s\" %d/%d\n", semas[i].name, (int)semas[i].count, (int)semas[i].max);
	for(i = 0; i < MAX_EVFS; i++)
		if(evfs[i].used) hle_log("[EVF] \"%s\" bits %08X attr %X\n", evfs[i].name, evfs[i].bits, evfs[i].attr);
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
	{ "sceKernelExtendThreadStack", sceKernelExtendThreadStack },
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
	{ "sceKernelSleepThreadCB", cb_sleep },
	{ "sceKernelWakeupThread", sceKernelWakeupThread },
	{ "sceKernelCancelWakeupThread", sceKernelCancelWakeupThread },
	{ "sceKernelDelayThread", sceKernelDelayThread },
	{ "sceKernelDelayThreadCB", cb_delay },
	{ "sceKernelDelaySysClockThread", sceKernelDelaySysClockThread },
	{ "sceKernelDelaySysClockThreadCB", cb_delay_sysclock },
	{ "sceKernelWaitThreadEnd", sceKernelWaitThreadEnd },
	{ "sceKernelWaitThreadEndCB", cb_wait_thread_end },
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
	{ "sceKernelWaitSemaCB", cb_wait_sema },
	{ "sceKernelPollSema", sceKernelPollSema },
	{ "sceKernelReferSemaStatus", sceKernelReferSemaStatus },
	{ "sceKernelCreateEventFlag", sceKernelCreateEventFlag },
	{ "sceKernelDeleteEventFlag", sceKernelDeleteEventFlag },
	{ "sceKernelSetEventFlag", sceKernelSetEventFlag },
	{ "sceKernelClearEventFlag", sceKernelClearEventFlag },
	{ "sceKernelWaitEventFlag", wait_evf },
	{ "sceKernelWaitEventFlagCB", cb_wait_evf },
	{ "sceKernelPollEventFlag", sceKernelPollEventFlag },
	{ "sceKernelReferEventFlagStatus", sceKernelReferEventFlagStatus },
	{ "sceKernelCreateLwMutex", sceKernelCreateLwMutex },
	{ "sceKernelCreateMutex", sceKernelCreateMutex },
	{ "sceKernelDeleteMutex", sceKernelDeleteMutex },
	{ "sceKernelLockMutex", sceKernelLockMutex },
	{ "sceKernelLockMutexCB", sceKernelLockMutexCB },
	{ "sceKernelTryLockMutex", sceKernelTryLockMutex },
	{ "sceKernelUnlockMutex", sceKernelUnlockMutex },
	{ "sceKernelCancelMutex", sceKernelCancelMutex },
	{ "sceKernelReferMutexStatus", sceKernelReferMutexStatus },
	{ "sceKernelDeleteLwMutex", sceKernelDeleteLwMutex },
	{ "sceKernelReferLwMutexStatusByID", sceKernelReferLwMutexStatusByID },
	{ "sceKernelCreateCallback", sceKernelCreateCallback },
	{ "sceKernelDeleteCallback", sceKernelDeleteCallback },
	{ "sceKernelCheckCallback", sceKernelCheckCallback },
	{ "sceKernelNotifyCallback", sceKernelNotifyCallback },
	{ "sceKernelCancelCallback", sceKernelCancelCallback },
	{ "sceKernelGetCallbackCount", sceKernelGetCallbackCount },
	{ "sceKernelReferCallbackStatus", sceKernelReferCallbackStatus },
	{ "sceKernelCreateFpl", sceKernelCreateFpl },
	{ "sceKernelDeleteFpl", sceKernelDeleteFpl },
	{ "sceKernelAllocateFpl", sceKernelAllocateFpl },
	{ "sceKernelAllocateFplCB", cb_allocate_fpl },
	{ "sceKernelTryAllocateFpl", sceKernelTryAllocateFpl },
	{ "sceKernelFreeFpl", sceKernelFreeFpl },
	{ "sceKernelCancelFpl", sceKernelCancelFpl },
	{ "sceKernelCancelSema", sceKernelCancelSema },
	{ "sceKernelCancelEventFlag", sceKernelCancelEventFlag },
	{ "sceKernelReferFplStatus", sceKernelReferFplStatus },
	{ "sceKernelSuspendThread", sceKernelSuspendThread },
	{ "sceKernelResumeThread", sceKernelResumeThread },
	{ "sceKernelChangeCurrentThreadAttr", sceKernelChangeCurrentThreadAttr },
	{ "sceKernelReferThreadProfiler", return_zero },
	{ "sceKernelReferGlobalProfiler", return_zero },
};

/* Devuelve el destino (PPSSPP) */
static void sceKernelMemset(void){
	u32 addr = ARG(0), n = ARG(2);
	if(n && mem_valid(addr, n)){
		memset(mem_ptr(addr, n), (int)(ARG(1) & 0xFF), n);
		mem_note_write(addr, n);
	}
	RETURN(addr);
}

static const HleFunction kernel_library[] = {
	{ "sceKernelMemset", sceKernelMemset },
	{ "sceKernelCpuSuspendIntr", sceKernelCpuSuspendIntr },
	{ "sceKernelCpuResumeIntr", sceKernelCpuResumeIntr },
	{ "sceKernelCpuResumeIntrWithSync", sceKernelCpuResumeIntr },
	{ "sceKernelIsCpuIntrEnable", sceKernelIsCpuIntrEnable },
	{ "sceKernelIsCpuIntrSuspended", sceKernelIsCpuIntrSuspended },
	{ "sceKernelLockLwMutex", sceKernelLockLwMutex },
	{ "sceKernelLockLwMutexCB", cb_lock_lwmutex },
	{ "sceKernelTryLockLwMutex", sceKernelTryLockLwMutex },
	{ "sceKernelTryLockLwMutex_600", sceKernelTryLockLwMutex_600 },
	{ "sceKernelReferLwMutexStatus", sceKernelReferLwMutexStatus },
	{ "sceKernelUnlockLwMutex", sceKernelUnlockLwMutex },
};

static const HleFunction sysmem_user[] = {
	{ "sceKernelAllocPartitionMemory", sceKernelAllocPartitionMemory },
	{ "sceKernelFreePartitionMemory", sceKernelFreePartitionMemory },
	{ "sceKernelGetBlockHeadAddr", sceKernelGetBlockHeadAddr },
	{ "sceKernelMaxFreeMemSize", sceKernelMaxFreeMemSize },
	{ "sceKernelTotalFreeMemSize", sceKernelTotalFreeMemSize },
	{ "sceKernelDevkitVersion", sceKernelDevkitVersion },
	{ "sceKernelSetCompiledSdkVersion", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion370", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion380_390", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion395", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion401_402", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion500_505", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion507", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion600_602", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion603_605", sceKernelSetCompiledSdkVersion },
	{ "sceKernelSetCompiledSdkVersion606", sceKernelSetCompiledSdkVersion },
	{ "sceKernelGetCompiledSdkVersion", sceKernelGetCompiledSdkVersion },
	{ "sceKernelSetCompilerVersion", return_zero },
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
	{ "sceKernelRegisterExitCallback", sceKernelRegisterExitCallback },
};


/* La caché de datos no se emula, pero vaciarla avisa de que el juego
   escribió esa memoria con la CPU (texturas que el GE leerá) */
static void dcache_range(void){
	u32 addr = ARG(0), size = ARG(1);
	if(addr && (s32)size > 0) mem_note_write(addr, size);
	RETURN(0);
}

static void dcache_all(void){
	mem_note_write_all();
	RETURN(0);
}

static const HleFunction utils_user[] = {
	{ "sceKernelLibcTime", sceKernelLibcTime },
	{ "sceKernelLibcClock", sceKernelLibcClock },
	{ "sceKernelLibcGettimeofday", sceKernelLibcGettimeofday },
	{ "sceKernelDcacheWritebackAll", dcache_all },
	{ "sceKernelDcacheWritebackInvalidateAll", dcache_all },
	{ "sceKernelDcacheWritebackRange", dcache_range },
	{ "sceKernelDcacheWritebackInvalidateRange", dcache_range },
	{ "sceKernelDcacheInvalidateRange", dcache_range },
	{ "sceKernelIcacheInvalidateAll", return_zero },
	{ "sceKernelIcacheInvalidateRange", return_zero },
	{ "sceKernelGetGPI", sceKernelGetGPI },
	{ "sceKernelUtilsMt19937Init", sceKernelUtilsMt19937Init },
	{ "sceKernelUtilsMt19937UInt", sceKernelUtilsMt19937UInt },
	{ "sceKernelSetGPO", return_zero },
};

static const HleFunction interrupt_manager[] = {
	{ "sceKernelRegisterSubIntrHandler", sceKernelRegisterSubIntrHandler },
	{ "sceKernelReleaseSubIntrHandler", sceKernelReleaseSubIntrHandler },
	{ "sceKernelEnableSubIntr", sceKernelEnableSubIntr },
	{ "sceKernelDisableSubIntr", sceKernelDisableSubIntr },
};

const HleLibrary hle_kernel_libs[] = {
	HLE_LIBRARY("InterruptManager", interrupt_manager),
	HLE_LIBRARY("ThreadManForUser", thread_man),
	HLE_LIBRARY("Kernel_Library", kernel_library),
	HLE_LIBRARY("SysMemUserForUser", sysmem_user),
	HLE_LIBRARY("StdioForUser", stdio_user),
	HLE_LIBRARY("LoadExecForUser", loadexec_user),
	HLE_LIBRARY("UtilsForUser", utils_user),
};
const u32 hle_kernel_libs_count = sizeof(hle_kernel_libs) / sizeof(hle_kernel_libs[0]);
