/**
 * WIISP - ge.c
 * GE: cola de listas de comandos, ejecución de comandos y HLE de sceGe_user.
 *
 * Las listas se ejecutan en orden (como en el hardware: solo avanza la
 * primera de la cola). Cada vez que una lista se encola o su "stall"
 * avanza, se ejecuta hasta llegar al stall o al final. Al terminar, el GE
 * llama a las funciones de aviso del juego (sceGeSetCallback) como lo haría
 * la interrupción del GE.
 *
 * Para que la sincronización se parezca a la real, cada lista suma un coste
 * estimado (vértices y píxeles) y sceGeDrawSync espera ese tiempo.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include "gpu/ge_internal.h"
#include "hle/hle.h"
#include "core/memory.h"

GeState ge;
GeStats ge_stats;

/* --- Listas ------------------------------------------------------------ */

#define GE_MAX_LISTS     64
#define GE_STACK_DEPTH   32
#define GE_MAX_CALLBACKS 16

enum {
	LIST_COMPLETED = 0, LIST_QUEUED = 1, LIST_DRAWING = 2, LIST_STALLING = 3,
	LIST_CANCEL_DONE = 4, LIST_PAUSED = 5
};

/* Comportamientos de SIGNAL (bits 16-23) */
enum {
	SIG_HANDLER_SUSPEND = 0x01, SIG_HANDLER_CONTINUE = 0x02, SIG_HANDLER_PAUSE = 0x03,
	SIG_SYNC = 0x08, SIG_JUMP = 0x10, SIG_CALL = 0x11, SIG_RET = 0x12,
	SIG_RJUMP = 0x13, SIG_RCALL = 0x14, SIG_OJUMP = 0x15, SIG_OCALL = 0x16,
	SIG_RTBASE = 0x17, SIG_RTPATH = 0x18, SIG_BREAK1 = 0xF0, SIG_BREAK2 = 0xFF
};

typedef struct {
	int used;
	int state;
	u32 pc, stall;
	u32 stack_pc[GE_STACK_DEPTH], stack_offset[GE_STACK_DEPTH];
	int sp;
	int cbid;
	u32 arg;
	u32 prev_op;        /* comando anterior (para END tras SIGNAL/FINISH) */
	u32 finish_arg;
} GeList;

typedef struct {
	int used;
	u32 signal_func, signal_arg, finish_func, finish_arg;
} GeCallback;

static GeList lists[GE_MAX_LISTS];
static int queue[GE_MAX_LISTS];
static int queue_len;
static GeCallback callbacks[GE_MAX_CALLBACKS];
static u64 busy_until;      /* ciclo en que el GE terminaría lo pendiente */
static u32 list_cost;       /* coste estimado de lo ejecutado ahora */
static int queue_running;   /* run_queue en curso (evita la reentrada) */

void ge_init(void){
	queue_running = 0;
	memset(&ge, 0, sizeof(ge));
	memset(lists, 0, sizeof(lists));
	memset(callbacks, 0, sizeof(callbacks));
	memset(&ge_stats, 0, sizeof(ge_stats));
	queue_len = 0;
	busy_until = 0;
	ge.last_prim = GE_PRIM_TRIANGLES;
}

void ge_shutdown(void){
	queue_len = 0;
}

void ge_get_stats(GeStats *out){ *out = ge_stats; }

static inline u32 list_addr(u32 a){ return a & 0x0FFFFFFFu; }

/* --- Ejecución de comandos ---------------------------------------------- */

static void do_transfer(u32 arg){
	u32 bpp = (arg & 1) ? 4 : 2;
	u32 src = (ge.cmd[GE_TRANSFERSRC] & 0xFFFFF0u) | ((ge.cmd[GE_TRANSFERSRCW] & 0xFF0000u) << 8);
	u32 dst = (ge.cmd[GE_TRANSFERDST] & 0xFFFFF0u) | ((ge.cmd[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
	u32 src_stride = ge.cmd[GE_TRANSFERSRCW] & 0x7F8;
	u32 dst_stride = ge.cmd[GE_TRANSFERDSTW] & 0x7F8;
	u32 sx = ge.cmd[GE_TRANSFERSRCPOS] & 0x3FF, sy = (ge.cmd[GE_TRANSFERSRCPOS] >> 10) & 0x3FF;
	u32 dx = ge.cmd[GE_TRANSFERDSTPOS] & 0x3FF, dy = (ge.cmd[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
	u32 w = (ge.cmd[GE_TRANSFERSIZE] & 0x3FF) + 1, h = ((ge.cmd[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
	u32 y;

	for(y = 0; y < h; y++){
		u8 *s = mem_ptr(src + ((sy + y) * src_stride + sx) * bpp, w * bpp);
		u8 *d = mem_ptr(dst + ((dy + y) * dst_stride + dx) * bpp, w * bpp);
		if(s && d) memmove(d, s, w * bpp);
	}
	list_cost += w * h / 2;
}

void ge_load_clut(u32 blocks){
	u32 addr = (ge.cmd[GE_CLUTADDR] & 0xFFFFF0u) | ((ge.cmd[GE_CLUTADDRUPPER] << 8) & 0x0F000000u);
	u32 bytes = (blocks & 0x3F) * 32;
	u8 *p;
	if(bytes > sizeof(ge.clut)) bytes = sizeof(ge.clut);
	p = mem_ptr(addr, bytes);
	if(p) memcpy(ge.clut, p, bytes);
	else memset(ge.clut, 0, bytes);
	ge.clut_bytes = bytes;
}

static void matrix_data(float *m, u32 *n, u32 max, u32 arg){
	if(*n < max) m[*n] = ge_f24(arg);
	(*n)++;
}

static void call_signal(GeList *l, u32 id){
	if(l->cbid >= 0 && l->cbid < GE_MAX_CALLBACKS && callbacks[l->cbid].used)
		kernel_call_guest(callbacks[l->cbid].signal_func, id, callbacks[l->cbid].signal_arg, 0);
}

static void call_finish(GeList *l, u32 id){
	if(l->cbid >= 0 && l->cbid < GE_MAX_CALLBACKS && callbacks[l->cbid].used)
		kernel_call_guest(callbacks[l->cbid].finish_func, id, callbacks[l->cbid].finish_arg, 0);
}

/* SIGNAL seguido de END: la señal se procesa al llegar al END */
static void do_signal(GeList *l, u32 sig_arg, u32 end_arg){
	u32 behavior = (sig_arg >> 16) & 0xFF;
	u32 id = sig_arg & 0xFFFF;
	u32 target = (id << 16) | (end_arg & 0xFFFF);

	switch(behavior){
	case SIG_HANDLER_SUSPEND:
	case SIG_HANDLER_CONTINUE:
	case SIG_HANDLER_PAUSE:
		call_signal(l, id);
		if(behavior == SIG_HANDLER_PAUSE) l->state = LIST_PAUSED;
		break;
	case SIG_JUMP: case SIG_RJUMP: case SIG_OJUMP:
		if(behavior == SIG_RJUMP) target += l->pc - 4;
		else if(behavior == SIG_OJUMP) target += ge.offset;
		l->pc = target & ~3u;
		break;
	case SIG_CALL: case SIG_RCALL: case SIG_OCALL:
		if(behavior == SIG_RCALL) target += l->pc - 4;
		else if(behavior == SIG_OCALL) target += ge.offset;
		if(l->sp < GE_STACK_DEPTH){
			l->stack_pc[l->sp] = l->pc;
			l->stack_offset[l->sp] = ge.offset;
			l->sp++;
			l->pc = target & ~3u;
		}
		break;
	case SIG_RET:
		if(l->sp > 0){
			l->sp--;
			l->pc = l->stack_pc[l->sp];
			ge.offset = l->stack_offset[l->sp];
		}
		break;
	default:
		break; /* SYNC, BREAK y comportamientos raros: seguir */
	}
}

/* Ejecuta un comando. Devuelve 0 si la lista debe parar. */
static int execute(GeList *l, u32 op){
	u32 cmd = op >> 24, arg = op & 0x00FFFFFFu;
	u32 prev = l->prev_op;
	l->prev_op = op;
	ge_stats.commands++;

	switch(cmd){
	case GE_NOP:
		return 1;
	case GE_VADDR: case GE_IADDR:
		ge.cmd[cmd] = ge_address(arg) + ge.offset;
		return 1;
	case GE_PRIM:
		ge.cmd[cmd] = arg;
		ge_draw_prim((arg >> 16) & 7, arg & 0xFFFF);
		return 1;
	case GE_BEZIER:
		ge.cmd[cmd] = arg;
		ge_draw_bezier(arg);
		return 1;
	case GE_SPLINE:
		ge.cmd[cmd] = arg;
		ge_draw_spline(arg);
		return 1;
	case GE_BOUNDINGBOX:
		ge_bounding_box(arg & 0xFFFF);
		return 1;
	case GE_JUMP:
		l->pc = (ge_address(arg) + ge.offset) & ~3u;
		return 1;
	case GE_BJUMP:
		if(!ge.bbox_visible) l->pc = (ge_address(arg) + ge.offset) & ~3u;
		return 1;
	case GE_CALL:
		if(l->sp < GE_STACK_DEPTH){
			l->stack_pc[l->sp] = l->pc;
			l->stack_offset[l->sp] = ge.offset;
			l->sp++;
			l->pc = (ge_address(arg) + ge.offset) & ~3u;
		}
		return 1;
	case GE_RET:
		if(l->sp > 0){
			l->sp--;
			l->pc = l->stack_pc[l->sp];
			ge.offset = l->stack_offset[l->sp];
		}
		return 1;
	case GE_SIGNAL:
	case GE_FINISH:
		ge.cmd[cmd] = arg;
		return 1;
	case GE_END:
		if((prev >> 24) == GE_SIGNAL){
			do_signal(l, prev & 0xFFFFFF, arg);
			return l->state != LIST_PAUSED;
		}
		if((prev >> 24) == GE_FINISH){
			l->finish_arg = prev & 0xFFFF;
			l->state = LIST_COMPLETED;
			return 0;
		}
		/* END suelto: la lista termina sin aviso */
		l->state = LIST_COMPLETED;
		l->finish_arg = 0xFFFFFFFFu;
		return 0;
	case GE_BASE:
		ge.base = arg;
		ge.cmd[cmd] = arg;
		return 1;
	case GE_ORIGIN:
		ge.offset = l->pc - 4;
		return 1;
	case GE_OFFSETADDR:
		ge.offset = arg << 8;
		return 1;
	case GE_WORLDMATRIXNUMBER: ge.world_n = arg & 0xF; return 1;
	case GE_WORLDMATRIXDATA:   matrix_data(ge.world, &ge.world_n, 12, arg); return 1;
	case GE_VIEWMATRIXNUMBER:  ge.view_n = arg & 0xF; return 1;
	case GE_VIEWMATRIXDATA:    matrix_data(ge.view, &ge.view_n, 12, arg); return 1;
	case GE_PROJMATRIXNUMBER:  ge.proj_n = arg & 0xF; return 1;
	case GE_PROJMATRIXDATA:    matrix_data(ge.proj, &ge.proj_n, 16, arg); return 1;
	case GE_TGENMATRIXNUMBER:  ge.tgen_n = arg & 0xF; return 1;
	case GE_TGENMATRIXDATA:    matrix_data(ge.tgen, &ge.tgen_n, 12, arg); return 1;
	case GE_BONEMATRIXNUMBER:  ge.bone_n = arg & 0x7F; return 1;
	case GE_BONEMATRIXDATA:    matrix_data(&ge.bone[0][0], &ge.bone_n, 96, arg); return 1;
	case GE_LOADCLUT:
		ge.cmd[cmd] = arg;
		ge_load_clut(arg);
		return 1;
	case GE_TRANSFERSTART:
		ge.cmd[cmd] = arg;
		do_transfer(arg);
		return 1;
	case GE_VAP:
		ge.cmd[cmd] = arg;
		ge_immediate_vertex();
		return 1;
	default:
		ge.cmd[cmd] = arg;
		return 1;
	}
}

static void run_list(GeList *l){
	u32 vertices_before = ge_stats.vertices;
	u64 pixels_before = ge_stats.pixels;
	l->state = LIST_DRAWING;
	for(;;){
		u32 op;
		if(l->stall && list_addr(l->pc) == list_addr(l->stall)){
			l->state = LIST_STALLING;
			break;
		}
		if(!mem_valid(l->pc, 4)){
			hle_log("[GE] lista fuera de memoria en 0x%08X\n", l->pc);
			l->state = LIST_COMPLETED;
			l->finish_arg = 0xFFFFFFFFu;
			break;
		}
		op = mem_read32(l->pc);
		l->pc += 4;
		if(!execute(l, op)) break;
		if(l->state == LIST_PAUSED) break;
	}
	list_cost += 40 + (ge_stats.vertices - vertices_before) * 12 +
	             (u32)((ge_stats.pixels - pixels_before) / 2);
}

/* Avanza la cola: la primera lista corre hasta su stall o hasta acabar.
   No es reentrante: si una función de aviso del juego encola o avanza una
   lista mientras tanto, el bucle exterior la recoge. */

static void run_queue(void){
	if(queue_running) return;
	queue_running = 1;
	list_cost = 0;
	while(queue_len > 0){
		GeList *l = &lists[queue[0]];
		run_list(l);
		if(l->state != LIST_COMPLETED) break;
		memmove(queue, queue + 1, (size_t)(--queue_len) * sizeof(queue[0]));
		if(l->finish_arg != 0xFFFFFFFFu) call_finish(l, l->finish_arg);
		l->used = 0;
	}
	if(list_cost){
		u64 now = cpu_cycles;
		busy_until = (busy_until > now ? busy_until : now) + list_cost;
	}
	queue_running = 0;
}

/* --- HLE: sceGe_user ------------------------------------------------- */

#define SCE_GE_ERROR_INVALID_ARGUMENT 0x80000100u
#define SCE_KERNEL_ERROR_INVALID_ID   0x80000100u

static int find_list(u32 id){
	return (id < GE_MAX_LISTS && lists[id].used) ? (int)id : -1;
}

static void enqueue(int head){
	u32 addr = ARG(0), stall = ARG(1);
	int cbid = (int)ARG(2), i;
	if((addr & 3) || !mem_valid(addr, 4)){ RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	for(i = 0; i < GE_MAX_LISTS && lists[i].used; i++);
	if(i == GE_MAX_LISTS){ RETURN(0x80000022u /* SCE_KERNEL_ERROR_OUT_OF_MEMORY */); return; }
	memset(&lists[i], 0, sizeof(lists[i]));
	lists[i].used = 1;
	lists[i].state = LIST_QUEUED;
	lists[i].pc = addr;
	lists[i].stall = stall;
	lists[i].cbid = cbid;
	lists[i].arg = ARG(3);
	if(head){
		memmove(queue + 1, queue, (size_t)queue_len * sizeof(queue[0]));
		queue[0] = i;
	} else queue[queue_len] = i;
	queue_len++;
	RETURN(i);
	run_queue();
}

static void sceGeListEnQueue(void){ enqueue(0); }
static void sceGeListEnQueueHead(void){ enqueue(1); }

static void sceGeListDeQueue(void){
	int i = find_list(ARG(0)), q;
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_INVALID_ID); return; }
	for(q = 0; q < queue_len && queue[q] != i; q++);
	if(q < queue_len){
		memmove(queue + q, queue + q + 1, (size_t)(queue_len - q - 1) * sizeof(queue[0]));
		queue_len--;
	}
	lists[i].used = 0;
	RETURN(0);
}

static void sceGeListUpdateStallAddr(void){
	int i = find_list(ARG(0));
	if(i < 0){ RETURN(SCE_KERNEL_ERROR_INVALID_ID); return; }
	lists[i].stall = ARG(1);
	RETURN(0);
	run_queue();
}

/* Espera a que el GE termine lo pendiente (tiempo estimado) */
static void wait_busy(void){
	if(busy_until > cpu_cycles) kernel_wait_until(busy_until);
}

static void sceGeListSync(void){
	int i = find_list(ARG(0));
	u32 mode = ARG(1);
	if(mode == 1){
		RETURN(i < 0 ? LIST_COMPLETED : (u32)lists[i].state);
		return;
	}
	RETURN(0);
	wait_busy();
}

static void sceGeDrawSync(void){
	u32 mode = ARG(0);
	if(mode == 1){
		if(queue_len) RETURN(lists[queue[0]].state == LIST_STALLING ? LIST_STALLING : LIST_DRAWING);
		else RETURN(busy_until > cpu_cycles ? LIST_DRAWING : LIST_COMPLETED);
		return;
	}
	RETURN(0);
	wait_busy();
}

static void sceGeContinue(void){
	if(queue_len && lists[queue[0]].state == LIST_PAUSED){
		lists[queue[0]].state = LIST_QUEUED;
		run_queue();
	}
	RETURN(0);
}

static void sceGeBreak(void){
	if(queue_len) lists[queue[0]].state = LIST_PAUSED;
	RETURN(0);
}

static void sceGeSetCallback(void){
	u32 data = ARG(0);
	int i;
	if(!mem_valid(data, 16)){ RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	for(i = 0; i < GE_MAX_CALLBACKS && callbacks[i].used; i++);
	if(i == GE_MAX_CALLBACKS){ RETURN(0x80000022u); return; }
	callbacks[i].used = 1;
	callbacks[i].signal_func = mem_read32(data);
	callbacks[i].signal_arg = mem_read32(data + 4);
	callbacks[i].finish_func = mem_read32(data + 8);
	callbacks[i].finish_arg = mem_read32(data + 12);
	RETURN(i);
}

static void sceGeUnsetCallback(void){
	u32 i = ARG(0);
	if(i >= GE_MAX_CALLBACKS || !callbacks[i].used){ RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	callbacks[i].used = 0;
	RETURN(0);
}

static void sceGeEdramGetAddr(void){ RETURN(PSP_VRAM_BASE); }
static void sceGeEdramGetSize(void){ RETURN(PSP_VRAM_SIZE); }
static void sceGeEdramSetAddrTranslation(void){
	static u32 translation = 0x400;
	RETURN(translation);
	translation = ARG(0);
}

static void sceGeGetCmd(void){
	u32 c = ARG(0);
	RETURN(c < 256 ? (c << 24) | ge.cmd[c] : SCE_GE_ERROR_INVALID_ARGUMENT);
}

/* sceGeGetMtx(tipo, float *m): 0-7 huesos, 8 mundo, 9 vista, 10 proyección, 11 textura */
static void sceGeGetMtx(void){
	u32 type = ARG(0), out = ARG(1), n, i;
	const float *m;
	if(type < 8){ m = ge.bone[type]; n = 12; }
	else if(type == 8){ m = ge.world; n = 12; }
	else if(type == 9){ m = ge.view; n = 12; }
	else if(type == 10){ m = ge.proj; n = 16; }
	else if(type == 11){ m = ge.tgen; n = 12; }
	else { RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	if(!mem_valid(out, n * 4)){ RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	for(i = 0; i < n; i++){
		u32 bits;
		memcpy(&bits, &m[i], 4);
		mem_write32(out + i * 4, bits);
	}
	RETURN(0);
}

/* Contexto: registros y matrices en el búfer de 2048 bytes del juego */
static void sceGeSaveContext(void){
	u32 ctx = ARG(0);
	u8 *p = mem_ptr(ctx, 2048);
	if(!p){ RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	memset(p, 0, 2048);
	memcpy(p, ge.cmd, sizeof(ge.cmd));                           /* 1024 bytes */
	memcpy(p + 1024, ge.world, sizeof(ge.world));                /* 48 */
	memcpy(p + 1072, ge.view, sizeof(ge.view));                  /* 48 */
	memcpy(p + 1120, ge.proj, sizeof(ge.proj));                  /* 64 */
	memcpy(p + 1184, ge.tgen, sizeof(ge.tgen));                  /* 48 */
	memcpy(p + 1232, ge.bone, sizeof(ge.bone));                  /* 384 */
	memcpy(p + 1616, &ge.base, 4);
	memcpy(p + 1620, &ge.offset, 4);
	RETURN(0);
}

static void sceGeRestoreContext(void){
	u32 ctx = ARG(0);
	u8 *p = mem_ptr(ctx, 2048);
	if(!p){ RETURN(SCE_GE_ERROR_INVALID_ARGUMENT); return; }
	memcpy(ge.cmd, p, sizeof(ge.cmd));
	memcpy(ge.world, p + 1024, sizeof(ge.world));
	memcpy(ge.view, p + 1072, sizeof(ge.view));
	memcpy(ge.proj, p + 1120, sizeof(ge.proj));
	memcpy(ge.tgen, p + 1184, sizeof(ge.tgen));
	memcpy(ge.bone, p + 1232, sizeof(ge.bone));
	memcpy(&ge.base, p + 1616, 4);
	memcpy(&ge.offset, p + 1620, 4);
	RETURN(0);
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
};

const HleLibrary hle_ge_libs[] = {
	HLE_LIBRARY("sceGe_user", ge_user),
};
const u32 hle_ge_libs_count = sizeof(hle_ge_libs) / sizeof(hle_ge_libs[0]);
