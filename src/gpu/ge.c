/**
 * WIISP - ge.c
 * GE: cola de listas de comandos, ejecución de comandos y HLE de sceGe_user.
 *
 * La gestión de la cola sigue el modelo de PPSSPP (GPU/GPUCommon.cpp y
 * Core/HLE/sceGe.cpp, GPLv2+; ver docs/sceGe.md de PPSSPP), comprobado
 * contra una PSP real con pspautotests (gpu/ge, gpu/signals):
 *
 *  - El GE se para en cada SIGNAL y cada FINISH y es la interrupción del GE
 *    la que lo pone en marcha otra vez. Tras un FINISH, primero corre la
 *    función de aviso del juego y solo después la lista sale de la cola.
 *  - Las listas se ejecutan de una vez hasta lo que las pare (stall, SIGNAL
 *    o FINISH), sumando lo que habría tardado el hardware; la interrupción
 *    se programa para ese momento.
 *  - Mientras la cabeza de la cola tenga una interrupción pendiente, nada
 *    más se ejecuta.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include "gpu/ge_internal.h"
#include "hle/hle.h"
#include "core/memory.h"
#include "core/prof.h"

GeState ge;
GeStats ge_stats;
unsigned long long (*ge_host_clock)(void);
#ifdef WIISP_PROF
unsigned long long ge_prof[8];
#endif

/* --- Listas ------------------------------------------------------------ */

#define GE_MAX_LISTS     64
#define GE_STACK_DEPTH   32
#define GE_MAX_CALLBACKS 16
#define GE_MAX_INTRS     64
#define LIST_ID_MAGIC    0x35000000u

/* Estado interno de cada lista */
enum { DL_NONE = 0, DL_QUEUED = 1, DL_RUNNING = 2, DL_COMPLETED = 3, DL_PAUSED = 4 };
/* Lo que ven sceGeListSync / sceGeDrawSync */
enum { LIST_COMPLETED = 0, LIST_QUEUED = 1, LIST_DRAWING = 2, LIST_STALLING = 3, LIST_PAUSED = 4 };
/* Estado del GE mientras ejecuta */
enum { GPU_RUNNING, GPU_DONE, GPU_STALL, GPU_INTERRUPT, GPU_ERROR };
enum { SYNC_DRAW = 0, SYNC_LIST = 1 };

/* Comportamientos de SIGNAL (bits 16-23) */
enum {
	SIG_NONE = 0x00, SIG_HANDLER_SUSPEND = 0x01, SIG_HANDLER_CONTINUE = 0x02, SIG_HANDLER_PAUSE = 0x03,
	SIG_SYNC = 0x08, SIG_JUMP = 0x10, SIG_CALL = 0x11, SIG_RET = 0x12,
	SIG_RJUMP = 0x13, SIG_RCALL = 0x14, SIG_OJUMP = 0x15, SIG_OCALL = 0x16
};

/* Subinterrupciones del GE: base = id de callback * 2 */
enum { SUBINTR_SIGNAL = 0, SUBINTR_FINISH = 1 };

/* Errores */
#define ERR_ALREADY          0x80000020u
#define ERR_BUSY             0x80000021u
#define ERR_OUT_OF_MEMORY    0x80000022u
#define ERR_PRIV_REQUIRED    0x80000023u
#define ERR_INVALID_ID       0x80000100u
#define ERR_INVALID_INDEX    0x80000102u
#define ERR_INVALID_POINTER  0x80000103u
#define ERR_INVALID_SIZE     0x80000104u
#define ERR_INVALID_MODE     0x80000107u
#define ERR_INVALID_VALUE    0x800001FEu
#define ERR_BAD_ARGUMENT     0x80000004u
#define ERR_CAN_NOT_WAIT     0x800201A7u
#define ERR_ILLEGAL_CONTEXT  0x80020064u

typedef struct {
	int state;
	int signal;              /* SIG_*: señal pendiente o entregada */
	u32 startpc;             /* dirección con que se encoló (o donde paró un break) */
	u32 pc, stall;
	int subintr_base;        /* -1 = sin funciones de aviso */
	u32 subintr_token;       /* 16 bits bajos del último SIGNAL/FINISH */
	struct { u32 pc, offset, base; } stack[GE_STACK_DEPTH];
	int stackptr;
	int interrupted;         /* la paró un sceGeBreak */
	int started;             /* ya ha empezado a ejecutarse alguna vez */
	int pending_interrupt;   /* llegó a SIGNAL/FINISH y la interrupción no se ha atendido */
	u64 wait_until;          /* sceGeListSync(id, 0) espera hasta este ciclo */
	u32 offset;              /* ORIGIN / OFFSETADDR guardado de la lista */
	u32 context;             /* PspGeContext donde guardar/restaurar el estado */
	u32 stack_addr;
} DisplayList;

typedef struct { int listid; u32 pc, cmd; } GeIntrData;
typedef struct { int used; u32 signal_func, signal_arg, finish_func, finish_arg; } GeCallback;

static DisplayList dls[GE_MAX_LISTS];
static int dlq[GE_MAX_LISTS];            /* cola: índices en dls */
static int dlq_len;
static DisplayList *current_list;
static int next_list_id;
static int gpu_state;
static u64 starting_ticks, busy_ticks, draw_complete_ticks;
static u64 cycles_executed;
static u32 cycle_last_pc;
static int interrupt_running, isbreak;
static GeIntrData ge_pending[GE_MAX_INTRS];
static int ge_pending_len;
static GeCallback callbacks[GE_MAX_CALLBACKS];

static void process_dl_queue(void);
static const KernelIntrHandler ge_intr_handler;

static inline u32 sdk(void){ return kernel_sdk_version(); }

/* Dirección relativa a BASE y al desplazamiento (getRelativeAddress) */
static inline u32 rel_addr(u32 data){
	u32 base_ext = ((ge.cmd[GE_BASE] & 0x000F0000u) << 8) | data;
	return (ge.offset + base_ext) & 0x0FFFFFFFu;
}

static void dlq_remove(int id){
	int i;
	for(i = 0; i < dlq_len && dlq[i] != id; i++);
	if(i == dlq_len) return;
	memmove(dlq + i, dlq + i + 1, (size_t)(dlq_len - i - 1) * sizeof(dlq[0]));
	dlq_len--;
}

static void dlq_push_front(int id){
	memmove(dlq + 1, dlq, (size_t)dlq_len * sizeof(dlq[0]));
	dlq[0] = id;
	dlq_len++;
}

static void pop_dl_queue(void){
	if(!dlq_len) return;
	dlq_remove(dlq[0]);
	if(dlq_len){
		int running = current_list && current_list->state == DL_RUNNING;
		current_list = &dls[dlq[0]];
		if(running) current_list->state = DL_RUNNING;
	} else current_list = NULL;
}

void ge_init(void){
	memset(&ge, 0, sizeof(ge));
	memset(dls, 0, sizeof(dls));
	memset(callbacks, 0, sizeof(callbacks));
	memset(&ge_stats, 0, sizeof(ge_stats));
	dlq_len = 0;
	current_list = NULL;
	next_list_id = 0;
	gpu_state = GPU_DONE;
	starting_ticks = busy_ticks = draw_complete_ticks = 0;
	cycles_executed = 0;
	interrupt_running = isbreak = 0;
	ge_pending_len = 0;
	psp_mem.vram_translation = 0x400;
	ge.last_prim = GE_PRIM_TRIANGLES;
	kernel_register_intr(PSP_GE_INTR, &ge_intr_handler);
}

void ge_shutdown(void){
	dlq_len = 0;
	current_list = NULL;
}

void ge_get_stats(GeStats *out){ *out = ge_stats; }

/* --- Tiempo ------------------------------------------------------------ */

/* Unos 2 ciclos de CPU por comando (el GE va a la mitad de reloj) */
static void update_pc(u32 cur_pc, u32 new_pc){
	u32 executed = (cur_pc - cycle_last_pc) / 4;
	cycles_executed += 2ull * executed;
	cycle_last_pc = new_pc;
}

/* Coste por vértice estimado por PPSSPP (transformación, luces...) */
static u32 vertex_cost(void){
	u32 cost = 20, morphs;
	int i;
	if(ge.cmd[GE_LIGHTINGENABLE] & 1){
		cost += 10;
		for(i = 0; i < 4; i++) if(ge.cmd[GE_LIGHTENABLE0 + i] & 1) cost += 7;
	}
	if((ge.cmd[GE_TEXMAPMODE] & 3) != 0) cost += 20;
	morphs = ((ge.cmd[GE_VERTEXTYPE] >> 18) & 7) + 1;
	if(morphs > 1) cost += 5 * morphs;
	return cost;
}

/* --- Esperas y eventos -------------------------------------------------- */

static int trigger_wait(int type, int id){
	return kernel_wake_object(type == SYNC_DRAW ? KWAIT_GE_DRAW : KWAIT_GE_LIST, (u32)id, 0) > 0;
}

/* Cuando se despierta a quien esperaba en sceGeDrawSync(0), las listas
   completadas dejan de existir */
static void sync_end(int type, int woke){
	int i;
	if(type == SYNC_DRAW && woke)
		for(i = 0; i < GE_MAX_LISTS; i++)
			if(dls[i].state == DL_COMPLETED) dls[i].state = DL_NONE;
}

static void sync_event(u64 userdata){
	int id = (int)(userdata >> 32), type = (int)(userdata & 0xFFFFFFFFu);
	sync_end(type, trigger_wait(type, id));
}

static void trigger_sync(int type, int id, u64 at){
	u64 userdata = ((u64)(u32)id << 32) | (u32)type;
	s64 future = (s64)(at - cpu_cycles);
	if(type == SYNC_DRAW){
		s64 left = kernel_unschedule_event(sync_event, userdata);
		if(left > future) future = left;
	}
	kernel_schedule_event(future > 0 ? cpu_cycles + (u64)future : cpu_cycles, sync_event, userdata);
}

static void interrupt_event(u64 userdata){
	(void)userdata;
	kernel_trigger_interrupt(PSP_GE_INTR, INTR_SUB_NONE);
}

static int trigger_interrupt(int listid, u32 pc, u64 at){
	GeIntrData *d;
	if(ge_pending_len == GE_MAX_INTRS) return 0;
	d = &ge_pending[ge_pending_len++];
	d->listid = listid;
	d->pc = pc;
	d->cmd = mem_read32(pc - 4) >> 24;
	kernel_schedule_event(at > cpu_cycles ? at : cpu_cycles, interrupt_event, ((u64)(u32)listid << 32) | pc);
	return 1;
}

static void ge_pending_pop(int i){
	memmove(ge_pending + i, ge_pending + i + 1, (size_t)(ge_pending_len - i - 1) * sizeof(ge_pending[0]));
	ge_pending_len--;
}

/* sceGeBreak(1): las interrupciones levantadas y no atendidas se pierden */
static void cancel_raised_interrupts(void){
	int count = kernel_cancel_raised_interrupts(PSP_GE_INTR);
	int i = (interrupt_running && ge_pending_len > 0) ? 1 : 0;
	while(count > 0 && i < ge_pending_len){
		dls[ge_pending[i].listid].pending_interrupt = 0;
		ge_pending_pop(i);
		count--;
	}
}

/* --- Contexto (formato de PPSSPP: 17 palabras de cabecera, registros,
       matrices y contadores; ocupa 385 de las 512 palabras) ------------- */

static const struct { u8 start, end; } ctx_ranges[] = {
	{ 0x00, 0x02 }, { 0x10, 0x10 }, { 0x12, 0x28 }, { 0x2C, 0x33 }, { 0x36, 0x38 },
	{ 0x42, 0x4D }, { 0x50, 0x51 }, { 0x53, 0x58 }, { 0x5B, 0xB5 }, { 0xB8, 0xC3 },
	{ 0xC5, 0xD0 }, { 0xD2, 0xE9 }, { 0xEB, 0xEC }, { 0xEE, 0xEE }, { 0xF0, 0xF6 },
	{ 0xF8, 0xF9 },
};

static inline u32 to_f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }
static inline u32 cmdword(int c){ return ((u32)c << 24) | ge.cmd[c]; }

static u32 *save_matrix(u32 *w, const float *m, int n, int numcmd, int datacmd){
	int i;
	*w++ = (u32)numcmd << 24;
	for(i = 0; i < n; i++) *w++ = ((u32)datacmd << 24) | to_f24(m[i]);
	return w;
}

static const u32 *load_matrix(const u32 *w, float *m, int n){
	int i;
	w++;
	for(i = 0; i < n; i++) m[i] = ge_f24(*w++ & 0xFFFFFF);
	return w;
}

static void save_context(u32 addr){
	u32 buf[512], *w;
	u8 *p = mem_ptr(addr, sizeof(buf));
	unsigned r;
	int n, i;
	if(!p) return;
	for(i = 0; i < 512; i++) buf[i] = rd_le32(p + i * 4);
	buf[5] = ge.vaddr;
	buf[6] = ge.iaddr;
	buf[7] = ge.offset;
	w = buf + 17;
	for(r = 0; r < sizeof(ctx_ranges) / sizeof(ctx_ranges[0]); r++)
		for(n = ctx_ranges[r].start; n <= ctx_ranges[r].end; n++) *w++ = cmdword(n);
	w = save_matrix(w, &ge.bone[0][0], 96, GE_BONEMATRIXNUMBER, GE_BONEMATRIXDATA);
	w = save_matrix(w, ge.world, 12, GE_WORLDMATRIXNUMBER, GE_WORLDMATRIXDATA);
	w = save_matrix(w, ge.view, 12, GE_VIEWMATRIXNUMBER, GE_VIEWMATRIXDATA);
	w = save_matrix(w, ge.proj, 16, GE_PROJMATRIXNUMBER, GE_PROJMATRIXDATA);
	w = save_matrix(w, ge.tgen, 12, GE_TGENMATRIXNUMBER, GE_TGENMATRIXDATA);
	*w++ = cmdword(GE_BONEMATRIXNUMBER) & 0xFF00007Fu;
	*w++ = cmdword(GE_WORLDMATRIXNUMBER) & 0xFF00000Fu;
	*w++ = cmdword(GE_VIEWMATRIXNUMBER) & 0xFF00000Fu;
	*w++ = cmdword(GE_PROJMATRIXNUMBER) & 0xFF00000Fu;
	*w++ = cmdword(GE_TGENMATRIXNUMBER) & 0xFF00000Fu;
	*w++ = (u32)GE_END << 24;
	for(i = 0; i < (int)(w - buf); i++) wr_le32(p + i * 4, buf[i]);
}

static void restore_context(u32 addr){
	u32 buf[512];
	const u32 *w;
	const u8 *p = mem_ptr_r(addr, sizeof(buf));
	unsigned r;
	int n, i;
	if(!p) return;
	for(i = 0; i < 512; i++) buf[i] = rd_le32(p + i * 4);
	ge.vaddr = buf[5];
	ge.iaddr = buf[6];
	ge.offset = buf[7];
	w = buf + 17;
	for(r = 0; r < sizeof(ctx_ranges) / sizeof(ctx_ranges[0]); r++)
		for(n = ctx_ranges[r].start; n <= ctx_ranges[r].end; n++) ge.cmd[n] = *w++ & 0xFFFFFF;
	w = load_matrix(w, &ge.bone[0][0], 96);
	w = load_matrix(w, ge.world, 12);
	w = load_matrix(w, ge.view, 12);
	w = load_matrix(w, ge.proj, 16);
	w = load_matrix(w, ge.tgen, 12);
	ge.cmd[GE_BONEMATRIXNUMBER] = *w++ & 0x7F;
	ge.cmd[GE_WORLDMATRIXNUMBER] = *w++ & 0xF;
	ge.cmd[GE_VIEWMATRIXNUMBER] = *w++ & 0xF;
	ge.cmd[GE_PROJMATRIXNUMBER] = *w++ & 0xF;
	ge.cmd[GE_TGENMATRIXNUMBER] = *w++ & 0xF;
}

/* --- Ejecución de comandos ---------------------------------------------- */

/* Transferencia de bloques. Como en el hardware (medido por PPSSPP):
   - una dirección de VRAM que se pasa del último espejo vuelve al
     principio de la VRAM;
   - con solapamiento o vuelta, cada línea se copia en trozos de 64 bytes
     de delante hacia atrás (lo que "arrastra" los datos si dst > src);
   - si el origen o el destino no son válidos (y no son VRAM), no se copia. */
static inline int is_vram(u32 a){ return (a & 0x3F800000u) == 0x04000000u; }
static inline u32 vram_wrap(u32 a){ return (a & 0x04800000u) == 0x04800000u ? a & ~0x00800000u : a; }

static void copy_chunk(u32 d, u32 s, u32 n){
	u8 tmp[64];
	u32 i;
	for(i = 0; i < n; i++) tmp[i] = mem_read8(vram_wrap(s + i));
	for(i = 0; i < n; i++) mem_write8(vram_wrap(d + i), tmp[i]);
}

static void do_transfer(u32 arg){
	u32 bpp = (arg & 1) ? 4 : 2;
	u32 src_base = (ge.cmd[GE_TRANSFERSRC] & 0xFFFFF0u) | ((ge.cmd[GE_TRANSFERSRCW] & 0xFF0000u) << 8);
	u32 dst_base = (ge.cmd[GE_TRANSFERDST] & 0xFFFFF0u) | ((ge.cmd[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
	u32 src_stride = ge.cmd[GE_TRANSFERSRCW] & 0x7F8;
	u32 dst_stride = ge.cmd[GE_TRANSFERDSTW] & 0x7F8;
	u32 sx = ge.cmd[GE_TRANSFERSRCPOS] & 0x3FF, sy = (ge.cmd[GE_TRANSFERSRCPOS] >> 10) & 0x3FF;
	u32 dx = ge.cmd[GE_TRANSFERDSTPOS] & 0x3FF, dy = (ge.cmd[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
	u32 w = (ge.cmd[GE_TRANSFERSIZE] & 0x3FF) + 1, h = ((ge.cmd[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
	u32 src, dst, src_size, dst_size, line = w * bpp, y;
	int overlap, src_ok, dst_ok, src_wraps, dst_wraps;

	src_base = vram_wrap(src_base);
	dst_base = vram_wrap(dst_base);
	src = src_base + (sy * src_stride + sx) * bpp;
	dst = dst_base + (dy * dst_stride + dx) * bpp;
	src_size = (h - 1) * src_stride + line;
	dst_size = (h - 1) * dst_stride + line;
	overlap = src + src_size > dst && dst + dst_size > src;
	src_ok = mem_valid(src, src_size);
	dst_ok = mem_valid(dst, dst_size);
	src_wraps = is_vram(src_base) && !src_ok;
	dst_wraps = is_vram(dst_base) && !dst_ok;
	cycles_executed += (u64)w * h * bpp * 16 / 10;

	if((overlap || src_wraps || dst_wraps) && (src_ok || src_wraps) && (dst_ok || dst_wraps)){
		for(y = 0; y < h; y++){
			u32 s = vram_wrap(src_base + ((y + sy) * src_stride + sx) * bpp);
			u32 d = vram_wrap(dst_base + ((y + dy) * dst_stride + dx) * bpp);
			u32 i;
			for(i = 0; i < line; i += 64){
				u32 n = line - i < 64 ? line - i : 64;
				copy_chunk(d, s, n);
				s = vram_wrap(s + n);
				d = vram_wrap(d + n);
			}
		}
	} else if(src_ok && dst_ok){
		for(y = 0; y < h; y++){
			u32 sa = src_base + ((y + sy) * src_stride + sx) * bpp;
			u32 da = dst_base + ((y + dy) * dst_stride + dx) * bpp;
			const u8 *s = mem_ptr_r(sa, line);
			u8 *d = mem_ptr(da, line);
			if(s && d) memcpy(d, s, line);
			else { /* espejo con swizzle: byte a byte */
				u32 i;
				for(i = 0; i < line; i++) mem_write8(da + i, mem_read8(sa + i));
			}
		}
	}
}

/* La CLUT interna es de 1 KB por carga; lo que cae fuera de memoria válida
   se rellena con ceros (p. ej. una paleta pegada al final de la VRAM). */
void ge_load_clut(u32 blocks){
	u32 addr = (ge.cmd[GE_CLUTADDR] & 0xFFFFF0u) | ((ge.cmd[GE_CLUTADDRUPPER] << 8) & 0x0F000000u);
	u32 bytes = (blocks & 0x3F) * 32, off;
	if(bytes > 1024) bytes = 1024;
	if(!mem_valid(addr, 1) && addr == 0) return;
	for(off = 0; off < bytes; off += 16){
		const u8 *p = mem_ptr_r(addr + off, 16);
		if(p) memcpy(ge.clut + off, p, 16);
		else memset(ge.clut + off, 0, 16);
	}
	ge.clut_bytes = bytes;
	ge.clut_gen++;
}


/* Matrices: NUMBER pone el contador y cada DATA escribe y avanza. El índice
   usa solo 4 bits (7 en los huesos): la proyección da la vuelta al pasar
   de 16 y lo que sobra en las 4x3 se pierde. */
static void matrix_num(int numcmd, int datacmd, u32 arg, u32 mask){
	(void)datacmd;
	ge.cmd[numcmd] = arg & mask;
}

static void matrix_data(int numcmd, int datacmd, float *m, u32 size, u32 mask, u32 arg){
	u32 num = ge.cmd[numcmd] & 0xFFFFFF;
	if((num & mask) < size) m[num & mask] = ge_f24(arg);
	ge.cmd[numcmd] = (num + 1) & 0xFFFFFF;
	ge.cmd[datacmd] = 0;
}

static void complete_failed_list(DisplayList *l){
	if(l->started && l->context){
		restore_context(l->context);
		l->started = 0;
	}
	l->state = DL_COMPLETED;
	l->wait_until = starting_ticks + cycles_executed;
	if(l->wait_until > busy_ticks) busy_ticks = l->wait_until;
	trigger_sync(SYNC_LIST, (int)(l - dls), l->wait_until);
}

static void raise_interrupt(DisplayList *l){
	if(trigger_interrupt((int)(l - dls), l->pc, starting_ticks + cycles_executed)){
		l->pending_interrupt = 1;
		gpu_state = GPU_INTERRUPT;
	}
}

/* END: lo que hace depende del comando anterior (SIGNAL o FINISH) */
static void execute_end(DisplayList *l, u32 op){
	u32 prev = mem_read32(l->pc - 4);
	update_pc(l->pc, l->pc);
	cycles_executed += 60;

	switch(prev >> 24){
	case GE_SIGNAL: {
		int behaviour = (int)((prev >> 16) & 0xFF), trigger = 1;
		u32 signal = prev & 0xFFFF, enddata = op & 0xFFFF;
		u32 target = (((signal << 16) | enddata) & 0xFFFFFFFCu) - 4;
		l->subintr_token = signal;
		switch(behaviour){
		case SIG_HANDLER_SUSPEND:
			/* Con SDK antiguos la lista se ve en pausa mientras corre el aviso */
			if(sdk() <= 0x02000010) l->state = DL_PAUSED;
			l->signal = behaviour;
			break;
		case SIG_HANDLER_CONTINUE:
			l->signal = behaviour;
			break;
		case SIG_HANDLER_PAUSE:
			/* Se ve en pausa ya, pero sigue hasta el FINISH que la entrega */
			trigger = 0;
			l->signal = behaviour;
			l->state = DL_PAUSED;
			break;
		case SIG_SYNC:
			trigger = 0;
			l->signal = behaviour;
			break;
		case SIG_JUMP: case SIG_RJUMP: case SIG_OJUMP:
			trigger = 0;
			l->signal = behaviour;
			if(behaviour == SIG_RJUMP) target += l->pc - 4;
			else if(behaviour == SIG_OJUMP) target = rel_addr(target);
			if(!mem_valid(target, 4)) gpu_state = GPU_ERROR;
			else { update_pc(l->pc, target); l->pc = target; }
			break;
		case SIG_CALL: case SIG_RCALL: case SIG_OCALL:
			trigger = 0;
			l->signal = behaviour;
			if(behaviour == SIG_RCALL) target += l->pc - 4;
			else if(behaviour == SIG_OCALL) target = rel_addr(target);
			if(l->stackptr == GE_STACK_DEPTH) break;
			if(!mem_valid(target, 4)){ gpu_state = GPU_ERROR; break; }
			l->stack[l->stackptr].pc = l->pc;
			l->stack[l->stackptr].offset = ge.offset;
			l->stack[l->stackptr].base = ge.cmd[GE_BASE];
			l->stackptr++;
			update_pc(l->pc, target);
			l->pc = target;
			break;
		case SIG_RET:
			trigger = 0;
			l->signal = behaviour;
			if(l->stackptr == 0) break;
			l->stackptr--;
			ge.offset = l->stack[l->stackptr].offset;
			ge.cmd[GE_BASE] = l->stack[l->stackptr].base;
			update_pc(l->pc, l->stack[l->stackptr].pc);
			l->pc = l->stack[l->stackptr].pc;
			break;
		default:
			break;
		}
		if(trigger) raise_interrupt(l);
		break;
	}
	case GE_FINISH:
		switch(l->signal){
		case SIG_HANDLER_PAUSE:
			l->state = DL_PAUSED;
			raise_interrupt(l);
			break;
		case SIG_SYNC:
			/* El FINISH tras un SYNC no cuenta */
			l->signal = SIG_NONE;
			break;
		default:
			l->subintr_token = prev & 0xFFFF;
			gpu_state = GPU_DONE;
			if(trigger_interrupt((int)(l - dls), l->pc, starting_ticks + cycles_executed))
				l->pending_interrupt = 1;
			else {
				if(l->started && l->context){
					restore_context(l->context);
					l->started = 0;
				}
				l->state = DL_COMPLETED;
				l->wait_until = starting_ticks + cycles_executed;
				if(l->wait_until > busy_ticks) busy_ticks = l->wait_until;
				trigger_sync(SYNC_LIST, (int)(l - dls), l->wait_until);
			}
			break;
		}
		break;
	default:
		break; /* END suelto: no hace nada */
	}
}

static void jump_to(DisplayList *l, u32 target){
	if(!mem_valid(target, 4)){ gpu_state = GPU_ERROR; return; }
	update_pc(l->pc, target - 4);
	l->pc = target - 4;
}

/* Ejecuta el comando en l->pc (que luego avanza 4) */
static void execute(DisplayList *l, u32 op){
	u32 cmd = op >> 24, arg = op & 0x00FFFFFFu;
	ge.cmd[cmd] = arg;
	ge_stats.commands++;

	switch(cmd){
	case GE_VADDR:
		ge.vaddr = rel_addr(arg);
		break;
	case GE_IADDR:
		ge.iaddr = rel_addr(arg);
		break;
	case GE_PRIM:
		cycles_executed += (u64)vertex_cost() * (arg & 0xFFFF);
		ge_draw_prim((arg >> 16) & 7, arg & 0xFFFF);
		break;
	case GE_BEZIER:
		ge_draw_bezier(arg);
		break;
	case GE_SPLINE:
		ge_draw_spline(arg);
		break;
	case GE_BOUNDINGBOX:
		if((arg & 0xFFFF) == 0){ ge.bbox_visible = 0; break; }
		cycles_executed += 22ull * (arg & 0xFFFF);
		ge_bounding_box(arg & 0xFFFF);
		break;
	case GE_JUMP:
		jump_to(l, rel_addr(arg & 0xFFFFFC));
		break;
	case GE_BJUMP:
		if(!ge.bbox_visible) jump_to(l, rel_addr(arg & 0xFFFFFC));
		break;
	case GE_CALL: {
		u32 target = rel_addr(arg & 0xFFFFFC);
		if(!mem_valid(target, 4)){ gpu_state = GPU_ERROR; break; }
		if(l->stackptr == GE_STACK_DEPTH) break;
		l->stack[l->stackptr].pc = l->pc + 4;
		l->stack[l->stackptr].offset = ge.offset;
		l->stackptr++;
		update_pc(l->pc, target - 4);
		l->pc = target - 4;
		break;
	}
	case GE_RET:
		if(l->stackptr == 0) break;
		l->stackptr--;
		ge.offset = l->stack[l->stackptr].offset;
		{
			u32 target = l->stack[l->stackptr].pc & 0x0FFFFFFFu;
			update_pc(l->pc, target - 4);
			l->pc = target - 4;
		}
		break;
	case GE_END:
		execute_end(l, op);
		break;
	case GE_ORIGIN:
		ge.offset = l->pc;
		break;
	case GE_OFFSETADDR:
		ge.offset = arg << 8;
		break;
	case GE_WORLDMATRIXNUMBER: matrix_num(cmd, GE_WORLDMATRIXDATA, arg, 0xF); break;
	case GE_VIEWMATRIXNUMBER:  matrix_num(cmd, GE_VIEWMATRIXDATA, arg, 0xF); break;
	case GE_PROJMATRIXNUMBER:  matrix_num(cmd, GE_PROJMATRIXDATA, arg, 0xF); break;
	case GE_TGENMATRIXNUMBER:  matrix_num(cmd, GE_TGENMATRIXDATA, arg, 0xF); break;
	case GE_BONEMATRIXNUMBER:  matrix_num(cmd, GE_BONEMATRIXDATA, arg, 0x7F); break;
	case GE_WORLDMATRIXDATA: matrix_data(GE_WORLDMATRIXNUMBER, cmd, ge.world, 12, 0xF, arg); break;
	case GE_VIEWMATRIXDATA:  matrix_data(GE_VIEWMATRIXNUMBER, cmd, ge.view, 12, 0xF, arg); break;
	case GE_PROJMATRIXDATA:  matrix_data(GE_PROJMATRIXNUMBER, cmd, ge.proj, 16, 0xF, arg); break;
	case GE_TGENMATRIXDATA:  matrix_data(GE_TGENMATRIXNUMBER, cmd, ge.tgen, 12, 0xF, arg); break;
	case GE_BONEMATRIXDATA:  matrix_data(GE_BONEMATRIXNUMBER, cmd, &ge.bone[0][0], 96, 0x7F, arg); break;
	case GE_LOADCLUT:
		ge_load_clut(arg);
		break;
	case GE_TEXFLUSH:
		ge_raster_tex_flush();
		break;
	case GE_TRANSFERSTART:
		do_transfer(arg);
		break;
	case GE_VAP:
		ge_immediate_vertex();
		break;
	default:
		break;
	}
}

/* Ejecuta la cola hasta que algo la pare. Las listas no corren mientras la
   cabeza espere a que se atienda su interrupción. */
static void process_dl_queue_body(void);

static void process_dl_queue(void){
	u64 t0;
	int old = prof_switch(PROF_GE);
	ge_stats.lists++;
	if(!ge_host_clock) process_dl_queue_body();
	else {
		t0 = ge_host_clock();
		process_dl_queue_body();
		ge_stats.host_ticks += ge_host_clock() - t0;
	}
	prof_switch(old);
}

static void process_dl_queue_body(void){
	starting_ticks = cpu_cycles;
	cycles_executed = 0;
	/* Si el GE sigue ocupado con lo anterior, esto empieza cuando acabe */
	if(starting_ticks < busy_ticks) cycles_executed = busy_ticks - starting_ticks;

	while(dlq_len > 0){
		int id = dlq[0];
		DisplayList *l = &dls[id];
		if(l->state == DL_PAUSED || l->pending_interrupt) return;

		current_list = l;
		if(!l->started && l->context) save_context(l->context);
		l->started = 1;
		ge.offset = l->offset;

		if(!mem_valid(l->pc, 4)){
			hle_log("[GE] lista %d con pc invalido 0x%08X\n", id, l->pc);
			complete_failed_list(l);
			dlq_remove(id);
			continue;
		}

		cycle_last_pc = l->pc;
		cycles_executed += 60;
		l->state = DL_RUNNING;
		l->interrupted = 0;
		gpu_state = l->pc == l->stall ? GPU_STALL : GPU_RUNNING;

		while(gpu_state == GPU_RUNNING){
			if(l->pc == l->stall){ gpu_state = GPU_STALL; break; }
			if(!mem_valid(l->pc, 4)){ gpu_state = GPU_ERROR; break; }
			execute(l, mem_read32(l->pc));
			l->pc += 4;
		}

		if(cycle_last_pc != l->pc) update_pc(l->pc - 4, l->pc);
		l->offset = ge.offset;

		switch(gpu_state){
		case GPU_DONE:
			if(l->pending_interrupt){
				/* La lista sigue en la cola hasta que corra su aviso de FINISH.
				   Si era la última, el dibujo ya terminó: DrawSync no espera. */
				if(dlq_len == 1){
					draw_complete_ticks = starting_ticks + cycles_executed;
					if(draw_complete_ticks > busy_ticks) busy_ticks = draw_complete_ticks;
					trigger_sync(SYNC_DRAW, 1, draw_complete_ticks);
				}
				return;
			}
			break;
		case GPU_ERROR:
			complete_failed_list(l);
			break;
		case GPU_STALL:
			if(starting_ticks + cycles_executed > busy_ticks) busy_ticks = starting_ticks + cycles_executed;
			return;
		default:
			return;
		}
		if(l->state != DL_QUEUED) dlq_remove(id);
	}

	current_list = NULL;
	draw_complete_ticks = starting_ticks + cycles_executed;
	if(draw_complete_ticks > busy_ticks) busy_ticks = draw_complete_ticks;
	trigger_sync(SYNC_DRAW, 1, draw_complete_ticks);
}

/* --- Interrupción del GE ------------------------------------------------ */

static void interrupt_end(int listid){
	DisplayList *dl = &dls[listid];
	interrupt_running = 0;
	isbreak = 0;
	dl->pending_interrupt = 0;
	if(dl->state == DL_COMPLETED || dl->state == DL_NONE){
		if(dl->started && dl->context) restore_context(dl->context);
		if(dlq_len){
			if(listid == dlq[0]) pop_dl_queue();
			else dlq_remove(listid);
		}
		/* Si era la última, primero los de sceGeDrawSync y luego los de la
		   lista (salvo tras un sceGeBreak(1), que no despierta a nadie) */
		if(!dlq_len && dl->state == DL_COMPLETED)
			sync_end(SYNC_DRAW, trigger_wait(SYNC_DRAW, 1));
		dl->wait_until = 0;
		trigger_wait(SYNC_LIST, listid);
	}
}

static int ge_intr_run(int sub, u32 *func, u32 a[3]){
	GeIntrData d;
	DisplayList *dl;
	int subintr = -1;
	u32 handler = 0, harg = 0;
	(void)sub;
	if(!ge_pending_len) return 0; /* sceGeBreak(1) llegó antes */
	d = ge_pending[0];
	dl = &dls[d.listid];
	interrupt_running = 1;

	if(dl->subintr_base >= 0){
		switch(dl->signal){
		case SIG_SYNC: case SIG_JUMP: case SIG_CALL: case SIG_RET:
			break;
		case SIG_HANDLER_PAUSE:
			if(d.cmd == GE_FINISH) subintr = dl->subintr_base | SUBINTR_SIGNAL;
			break;
		default:
			subintr = dl->subintr_base | (d.cmd == GE_SIGNAL ? SUBINTR_SIGNAL : SUBINTR_FINISH);
			break;
		}
	}

	/* La lista se da por completada al empezar la interrupción */
	if(dl->signal != SIG_HANDLER_PAUSE && d.cmd == GE_FINISH && dl->state != DL_NONE)
		dl->state = DL_COMPLETED;
	/* La pausa ya está entregada: igual que tras sceGeBreak */
	if(dl->signal == SIG_HANDLER_PAUSE && d.cmd == GE_FINISH)
		dl->signal = SIG_HANDLER_SUSPEND;

	if(subintr >= 0 && kernel_get_subintr(PSP_GE_INTR, subintr, &handler, &harg) && handler){
		*func = handler;
		a[0] = dl->subintr_token & 0xFFFF;
		a[1] = harg;
		a[2] = sdk() <= 0x02000010 ? 0 : d.pc + 4;
		return 1;
	}

	if(dl->signal == SIG_HANDLER_SUSPEND && d.cmd == GE_SIGNAL && sdk() <= 0x02000010 &&
	   dl->state != DL_NONE && dl->state != DL_COMPLETED)
		dl->state = DL_QUEUED;

	ge_pending_pop(0);
	interrupt_end(d.listid);
	process_dl_queue();
	return 0;
}

static void ge_intr_result(int sub){
	GeIntrData d;
	DisplayList *dl;
	(void)sub;
	if(!ge_pending_len){ interrupt_running = 0; return; }
	d = ge_pending[0];
	ge_pending_pop(0);
	dl = &dls[d.listid];
	if(d.cmd == GE_SIGNAL && dl->signal == SIG_HANDLER_SUSPEND && sdk() <= 0x02000010 &&
	   dl->state != DL_NONE && dl->state != DL_COMPLETED)
		dl->state = DL_QUEUED;
	interrupt_end(d.listid);
	process_dl_queue();
}

static const KernelIntrHandler ge_intr_handler = { ge_intr_run, ge_intr_result };

/* --- Operaciones de la cola --------------------------------------------- */

static u32 enqueue_list(u32 listpc, u32 stall, int subintr_base, u32 args, int head, int *run){
	int id = -1, i, args_ok, args_full;
	u32 stack_addr;
	DisplayList *dl;
	*run = 0;

	if(((listpc | stall) & 3) || !mem_valid(listpc, 4)) return ERR_INVALID_POINTER;
	args_ok = args && mem_valid(args, 16);
	args_full = args_ok && mem_read32(args) >= 16;
	if(args_full && mem_read32(args + 8) >= 256) return ERR_INVALID_SIZE;
	stack_addr = args_full ? mem_read32(args + 12) : 0;

	/* Con SDK nuevos no se puede encolar dos veces la misma lista ni
	   compartir pila con una que ya empezó */
	if(sdk() > 0x01FFFFFF){
		for(i = 0; i < dlq_len; i++){
			const DisplayList *o = &dls[dlq[i]];
			if(o->pending_interrupt && o->state != DL_COMPLETED && gpu_state == GPU_DONE) continue;
			if(o->startpc == (listpc & 0x0FFFFFFFu)) return ERR_BUSY;
			if(stack_addr && o->stack_addr == stack_addr && o->started) return ERR_BUSY;
		}
	}

	for(i = 0; i < GE_MAX_LISTS; i++){
		int pid = (i + next_list_id) % GE_MAX_LISTS;
		const DisplayList *p = &dls[pid];
		if(p->pending_interrupt) continue;
		if(p->state == DL_NONE){ id = pid; break; }
		if(p->state == DL_COMPLETED && p->wait_until < cpu_cycles) id = pid;
	}
	if(id < 0) return ERR_OUT_OF_MEMORY;
	next_list_id = id + 1;

	dl = &dls[id];
	dl->startpc = listpc & 0x0FFFFFFFu;
	dl->pc = listpc & 0x0FFFFFFFu;
	dl->stall = stall & 0x0FFFFFFFu;
	dl->subintr_base = subintr_base < -1 ? -1 : subintr_base;
	dl->stackptr = 0;
	dl->signal = SIG_NONE;
	dl->interrupted = 0;
	dl->wait_until = ~0ull;
	dl->started = 0;
	dl->offset = 0;
	dl->stack_addr = stack_addr;
	dl->context = 0;
	if(args_ok){
		u32 ctx = mem_read32(args + 4);
		if(ctx && mem_valid(ctx, 4)) dl->context = ctx;
	}

	if(head){
		if(current_list){
			if(current_list->state != DL_PAUSED) return ERR_INVALID_VALUE;
			current_list->state = DL_QUEUED;
			current_list->signal = SIG_NONE;
		}
		dl->state = DL_PAUSED;
		current_list = dl;
		dlq_push_front(id);
	} else if(current_list){
		dl->state = DL_QUEUED;
		dlq[dlq_len++] = id;
		/* Puede venir del aviso de FINISH de la que era la última */
		draw_complete_ticks = ~0ull;
	} else {
		dl->state = DL_RUNNING;
		current_list = dl;
		dlq_push_front(id);
		draw_complete_ticks = ~0ull;
		*run = 1;
	}
	return (u32)id;
}

static u32 draw_sync(int mode){
	int i;
	const DisplayList *top = NULL;
	if(mode < 0 || mode > 1) return ERR_INVALID_MODE;
	if(mode == 0){
		if(!kernel_dispatch_enabled()) return ERR_CAN_NOT_WAIT;
		if(kernel_in_interrupt()) return ERR_ILLEGAL_CONTEXT;
		if(draw_complete_ticks > cpu_cycles) kernel_wait_object(KWAIT_GE_DRAW, 1);
		else for(i = 0; i < GE_MAX_LISTS; i++) if(dls[i].state == DL_COMPLETED) dls[i].state = DL_NONE;
		return 0;
	}
	for(i = 0; i < dlq_len; i++)
		if(dls[dlq[i]].state != DL_COMPLETED){ top = &dls[dlq[i]]; break; }
	if(!top) return LIST_COMPLETED;
	/* El firmware compara el stall con el pc del hardware */
	if(top->state != DL_QUEUED && top->pc == top->stall) return LIST_STALLING;
	return LIST_DRAWING;
}

/* ¿Está el GE ejecutando ahora mismo? (no si está parado en un aviso) */
static int busy_drawing(void){
	u32 st;
	if(interrupt_running){
		int cont = gpu_state == GPU_INTERRUPT && current_list && current_list->signal == SIG_HANDLER_CONTINUE;
		if(!cont) return 0;
	}
	st = draw_sync(1);
	return (st == LIST_DRAWING || st == LIST_STALLING) && current_list && current_list->state != DL_PAUSED;
}

/* --- HLE: sceGe_user ---------------------------------------------------- */

static void enqueue(int head){
	int cbid = (int)ARG(2), run;
	u32 id = enqueue_list(ARG(0), ARG(1), cbid * 2, ARG(3), head, &run);
	if((s32)id >= 0) id ^= LIST_ID_MAGIC;
	RETURN(id);
	if(run) process_dl_queue();
	kernel_eat_cycles(head ? 480 : 490);
}

static void sceGeListEnQueue(void){ enqueue(0); }
static void sceGeListEnQueueHead(void){ enqueue(1); }

static void sceGeListDeQueue(void){
	int id = (int)(ARG(0) ^ LIST_ID_MAGIC);
	DisplayList *dl;
	if(id < 0 || id >= GE_MAX_LISTS || dls[id].state == DL_NONE){ RETURN(ERR_INVALID_ID); return; }
	dl = &dls[id];
	/* Lo que ya empezó (incluidas las completadas) no se puede quitar */
	if(dl->started || dl->state == DL_COMPLETED){ RETURN(ERR_BUSY); return; }
	dl->state = DL_NONE;
	if(dlq_len && id == dlq[0]) pop_dl_queue();
	else dlq_remove(id);
	dl->wait_until = 0;
	trigger_wait(SYNC_LIST, id);
	RETURN(0);
	kernel_reschedule();
}

static void sceGeListUpdateStallAddr(void){
	int id = (int)(ARG(0) ^ LIST_ID_MAGIC);
	kernel_eat_cycles(190);
	if(id < 0 || id >= GE_MAX_LISTS || dls[id].state == DL_NONE){ RETURN(ERR_INVALID_ID); return; }
	if(dls[id].state == DL_COMPLETED){ RETURN(ERR_ALREADY); return; }
	dls[id].stall = ARG(1) & 0x0FFFFFFFu;
	RETURN(0);
	process_dl_queue();
}

static void sceGeListSync(void){
	int id = (int)(ARG(0) ^ LIST_ID_MAGIC);
	int mode = (int)ARG(1);
	DisplayList *dl;
	kernel_eat_cycles(220);
	if(id < 0 || id >= GE_MAX_LISTS){ RETURN(ERR_INVALID_ID); return; }
	if(mode < 0 || mode > 1){ RETURN(ERR_INVALID_MODE); return; }
	dl = &dls[id];
	if(mode == 1){
		switch(dl->state){
		case DL_QUEUED:    RETURN(dl->interrupted ? LIST_PAUSED : LIST_QUEUED); return;
		case DL_RUNNING:   RETURN(dl->pc == dl->stall ? LIST_STALLING : LIST_DRAWING); return;
		case DL_COMPLETED: RETURN(LIST_COMPLETED); return;
		case DL_PAUSED:    RETURN(LIST_PAUSED); return;
		default:           RETURN(ERR_INVALID_ID); return;
		}
	}
	if(!kernel_dispatch_enabled()){ RETURN(ERR_CAN_NOT_WAIT); return; }
	if(kernel_in_interrupt()){ RETURN(ERR_ILLEGAL_CONTEXT); return; }
	RETURN(LIST_COMPLETED);
	if(dl->wait_until > cpu_cycles) kernel_wait_object(KWAIT_GE_LIST, (u32)id);
}

static void sceGeDrawSync(void){
	u32 mode = ARG(0);
	kernel_eat_cycles(1240);
	RETURN(draw_sync((int)mode));
}

static void sceGeContinue(void){
	u32 ret = 0;
	int run = 0;
	if(current_list){
		if(current_list->state == DL_PAUSED){
			if(!isbreak){
				/* Entre la señal PAUSE y el FINISH que la entrega no se sale */
				if(current_list->signal == SIG_HANDLER_PAUSE) ret = ERR_BUSY;
				else {
					current_list->state = DL_RUNNING;
					current_list->signal = SIG_NONE;
					draw_complete_ticks = ~0ull;
					run = 1;
				}
			} else {
				current_list->state = DL_QUEUED;
				current_list->signal = SIG_NONE;
				run = 1;
			}
		} else if(current_list->state == DL_RUNNING)
			ret = sdk() >= 0x02000000 ? ERR_ALREADY : 0xFFFFFFFFu;
		else
			ret = sdk() >= 0x02000000 ? ERR_BAD_ARGUMENT : 0xFFFFFFFFu;
	}
	RETURN(ret);
	if(run) process_dl_queue();
	kernel_eat_cycles(220);
	kernel_reschedule();
}

static void sceGeBreak(void){
	u32 mode = ARG(0), ptr = ARG(1);
	int i;
	if(mode > 1){ RETURN(ERR_INVALID_MODE); return; }
	if((s32)ptr < 0 || (s32)(ptr + 16) < 0){ RETURN(ERR_PRIV_REQUIRED); return; }
	if(!current_list){ RETURN(ERR_ALREADY); return; }

	if(mode == 1){
		/* Se tira toda la cola y se reinicia el GE; no despierta a nadie */
		dlq_len = 0;
		for(i = 0; i < GE_MAX_LISTS; i++){
			dls[i].state = DL_NONE;
			dls[i].signal = SIG_NONE;
		}
		cancel_raised_interrupts();
		next_list_id = 0;
		current_list = NULL;
		RETURN(0);
		return;
	}

	if(current_list->state == DL_NONE || current_list->state == DL_COMPLETED){
		RETURN(sdk() >= 0x02000000 ? ERR_BAD_ARGUMENT : 0xFFFFFFFFu);
		return;
	}
	if(current_list->state == DL_PAUSED){
		if(sdk() > 0x02000010 && current_list->signal != SIG_HANDLER_PAUSE){ RETURN(ERR_ALREADY); return; }
		RETURN(ERR_BUSY);
		return;
	}
	if(current_list->state == DL_QUEUED){
		current_list->state = DL_PAUSED;
		RETURN((u32)(current_list - dls) ^ LIST_ID_MAGIC);
		return;
	}
	if(current_list->signal == SIG_SYNC) current_list->pc += 8;
	/* Al reanudar se seguirá donde paró: esa es ahora su dirección */
	current_list->startpc = current_list->pc;
	current_list->interrupted = 1;
	current_list->state = DL_PAUSED;
	current_list->signal = SIG_HANDLER_SUSPEND;
	isbreak = kernel_in_interrupt() || !kernel_interrupts_enabled();
	RETURN((u32)(current_list - dls) ^ LIST_ID_MAGIC);
}

static void sceGeSetCallback(void){
	u32 data = ARG(0);
	int i, base;
	GeCallback *cb;
	for(i = 0; i < GE_MAX_CALLBACKS && callbacks[i].used; i++);
	if(i == GE_MAX_CALLBACKS){ RETURN(ERR_OUT_OF_MEMORY); return; }
	cb = &callbacks[i];
	memset(cb, 0, sizeof(*cb));
	cb->used = 1;
	if(mem_valid(data, 16)){
		cb->signal_func = mem_read32(data);
		cb->signal_arg = mem_read32(data + 4);
		cb->finish_func = mem_read32(data + 8);
		cb->finish_arg = mem_read32(data + 12);
	}
	base = i * 2;
	if(cb->finish_func){
		kernel_register_subintr(PSP_GE_INTR, base | SUBINTR_FINISH, cb->finish_func, cb->finish_arg);
		kernel_enable_subintr(PSP_GE_INTR, base | SUBINTR_FINISH, 1);
	}
	if(cb->signal_func){
		kernel_register_subintr(PSP_GE_INTR, base | SUBINTR_SIGNAL, cb->signal_func, cb->signal_arg);
		kernel_enable_subintr(PSP_GE_INTR, base | SUBINTR_SIGNAL, 1);
	}
	RETURN(i);
}

static void sceGeUnsetCallback(void){
	u32 i = ARG(0);
	if(i >= GE_MAX_CALLBACKS){ RETURN(ERR_INVALID_ID); return; }
	if(callbacks[i].used){
		kernel_release_subintr(PSP_GE_INTR, (int)i * 2 | SUBINTR_FINISH);
		kernel_release_subintr(PSP_GE_INTR, (int)i * 2 | SUBINTR_SIGNAL);
	}
	callbacks[i].used = 0;
	RETURN(0);
}

static void sceGeEdramGetAddr(void){ RETURN(PSP_VRAM_BASE); kernel_eat_cycles(150); }
static void sceGeEdramGetSize(void){ RETURN(PSP_VRAM_SIZE); }

static void sceGeEdramSetAddrTranslation(void){
	u32 v = ARG(0);
	if((v != 0 && (v < 0x200 || v > 0x1000)) || (v & (v - 1))){ RETURN(ERR_INVALID_VALUE); return; }
	RETURN(psp_mem.vram_translation);
	psp_mem.vram_translation = v;
}

/* Devuelve la palabra entera; los registros de matrices no se leen */
static void sceGeGetCmd(void){
	u32 c = ARG(0), v;
	if(c >= 256){ RETURN(ERR_INVALID_INDEX); return; }
	v = cmdword((int)c);
	switch(c){
	case GE_BONEMATRIXDATA: case GE_WORLDMATRIXDATA: case GE_VIEWMATRIXDATA:
	case GE_PROJMATRIXDATA: case GE_TGENMATRIXDATA:
		v &= 0xFF000000u; break;
	case GE_BONEMATRIXNUMBER:
		v &= 0xFF00007Fu; break;
	case GE_WORLDMATRIXNUMBER: case GE_VIEWMATRIXNUMBER: case GE_PROJMATRIXNUMBER: case GE_TGENMATRIXNUMBER:
		v &= 0xFF00000Fu; break;
	default: break;
	}
	RETURN(v);
}

/* sceGeGetMtx(tipo, u32 *m): 0-7 huesos, 8 mundo, 9 vista, 10 proyección,
   11 textura; en float de 24 bits como los comandos */
static void sceGeGetMtx(void){
	u32 type = ARG(0), out = ARG(1), n, i;
	const float *m;
	n = type == 10 ? 16 : 12;
	if(!mem_valid(out, n * 4)){ RETURN(0xFFFFFFFFu); return; }
	if(type < 8) m = ge.bone[type];
	else if(type == 8) m = ge.world;
	else if(type == 9) m = ge.view;
	else if(type == 10) m = ge.proj;
	else if(type == 11) m = ge.tgen;
	else { RETURN(ERR_INVALID_INDEX); return; }
	for(i = 0; i < n; i++) mem_write32(out + i * 4, to_f24(m[i]));
	RETURN(0);
}

static void sceGeSaveContext(void){
	u32 ctx = ARG(0);
	if(busy_drawing()){ RETURN(0xFFFFFFFFu); return; }
	save_context(ctx);
	RETURN(0);
}

static void sceGeRestoreContext(void){
	u32 ctx = ARG(0);
	if(busy_drawing()){ RETURN(ERR_BUSY); return; }
	restore_context(ctx);
	RETURN(0);
}

/* sceGeGetStack(índice, u32 *pila): profundidad de CALL de la lista actual */
static void sceGeGetStack(void){
	int index = (int)ARG(0);
	u32 out = ARG(1);
	DisplayList *l = current_list;
	if(!l){ RETURN(0); return; }
	if(l->stackptr <= index){ RETURN(ERR_INVALID_INDEX); return; }
	if(index >= 0 && mem_valid(out, 32)){
		mem_write32(out, 0);
		mem_write32(out + 4, l->stack[index].pc + 4);
		mem_write32(out + 8, l->stack[index].offset);
		mem_write32(out + 28, l->stack[index].base);
	}
	RETURN((u32)l->stackptr);
}

static const HleFunction ge_user[] = {
	{ "sceGeEdramGetAddr", sceGeEdramGetAddr },
	{ "sceGeEdramGetSize", sceGeEdramGetSize },
	{ "sceGeEdramSetAddrTranslation", sceGeEdramSetAddrTranslation },
	{ "sceGeListEnQueue", sceGeListEnQueue },
	{ "sceGeListEnQueueHead", sceGeListEnQueueHead },
	{ "sceGeListDeQueue", sceGeListDeQueue },
	{ "sceGeListUpdateStallAddr", sceGeListUpdateStallAddr },
	{ "sceGeListSync", sceGeListSync },
	{ "sceGeDrawSync", sceGeDrawSync },
	{ "sceGeContinue", sceGeContinue },
	{ "sceGeBreak", sceGeBreak },
	{ "sceGeSetCallback", sceGeSetCallback },
	{ "sceGeUnsetCallback", sceGeUnsetCallback },
	{ "sceGeGetCmd", sceGeGetCmd },
	{ "sceGeGetMtx", sceGeGetMtx },
	{ "sceGeSaveContext", sceGeSaveContext },
	{ "sceGeRestoreContext", sceGeRestoreContext },
	{ "sceGeGetStack", sceGeGetStack },
};

const HleLibrary hle_ge_libs[] = {
	HLE_LIBRARY("sceGe_user", ge_user),
};
const u32 hle_ge_libs_count = sizeof(hle_ge_libs) / sizeof(hle_ge_libs[0]);
