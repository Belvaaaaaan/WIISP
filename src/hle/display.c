/**
 * WIISP - display.c
 * HLE de sceDisplay y sceCtrl (sceGe_user está en gpu/ge.c).
 *
 * El framebuffer de la PSP vive en su VRAM o RAM; aquí solo se guarda dónde
 * está y en qué formato, y el frontend lo convierte al dibujar.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include "hle/hle.h"
#include "core/memory.h"

static HleFramebuffer fb;
static u32 display_mode, display_width, display_height;
static u32 input_buttons;
static u32 input_latched;   /* pulsados desde la última lectura del juego */
static u8 input_lx = 128, input_ly = 128;
static u32 ctrl_cycle, ctrl_mode;
static u32 read_buttons;    /* lo último que leyó el juego ([MANDO]) */
static int read_stick, read_logs;
static u32 ctrl_unread;     /* muestras (vblanks) sin leer, como PPSSPP */
static int ctrl_consumed;   /* la del próximo vblank ya se entregó */

void display_init(void){
	memset(&fb, 0, sizeof(fb));
	display_mode = 0;
	display_width = 480;
	display_height = 272;
	ctrl_cycle = ctrl_mode = 0;
	input_latched = 0;
	ctrl_unread = 0;
	ctrl_consumed = 0;
	read_buttons = 0;
	read_stick = 0;
	read_logs = 0;
}

void hle_get_framebuffer(HleFramebuffer *out){ *out = fb; }

/* Lo que se pulsa entre dos lecturas del juego se queda hasta que lo lea
   (con la emulación lenta, el juego lee el mando pocas veces por segundo
   real y un toque corto se perdía) */
void hle_set_input(u32 buttons, u8 lx, u8 ly){
	input_latched |= buttons & ~input_buttons;
	input_buttons = buttons;
	input_lx = lx;
	input_ly = ly;
}

static u32 ctrl_buttons(void){ return input_buttons | input_latched; }

/* Tras cada lectura del juego: se olvida lo retenido y se apunta en
   wiisp.log lo que cambió (los primeros cambios de cada programa) */
void display_ctrl_read_done(void){
	static const struct { u32 bit; const char *name; } names[] = {
		{ 0x0001, "SELECT" }, { 0x0008, "START" }, { 0x0010, "arriba" }, { 0x0020, "derecha" },
		{ 0x0040, "abajo" }, { 0x0080, "izquierda" }, { 0x0100, "L" }, { 0x0200, "R" },
		{ 0x1000, "triangulo" }, { 0x2000, "circulo" }, { 0x4000, "X" }, { 0x8000, "cuadrado" },
	};
	u32 b = ctrl_buttons();
	int stick = input_lx < 96 || input_lx > 160 || input_ly < 96 || input_ly > 160;
	input_latched = 0;
	if((b != read_buttons || stick != read_stick) && read_logs < 300){
		char line[160];
		size_t len = 0;
		unsigned i;
		read_logs++;
		for(i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if(b & names[i].bit) len += (size_t)snprintf(line + len, sizeof(line) - len, " %s", names[i].name);
		if(!len) snprintf(line, sizeof(line), " nada");
		hle_log_quiet("[MANDO] el juego lee:%s; stick %u, %u @%u ms\n", line, input_lx, input_ly,
		        (unsigned)(hle_now_us() / 1000));
	}
	read_buttons = b;
	read_stick = stick;
}

/* SceCtrlData: u32 TimeStamp, u32 Buttons, u8 Lx, u8 Ly, u8 Rsrv[6] */
void display_fill_ctrl(u32 addr){
	u8 *p = mem_ptr(addr, 16);
	if(!p) return;
	memset(p, 0, 16);
	wr_le32(p, (u32)hle_now_us());
	wr_le32(p + 4, ctrl_buttons());
	p[8] = input_lx;
	p[9] = input_ly;
}

/* --- sceDisplay ------------------------------------------------------ */

static void sceDisplaySetMode(void){
	display_mode = ARG(0);
	display_width = ARG(1);
	display_height = ARG(2);
	RETURN(0);
}

static void sceDisplayGetMode(void){
	if(ARG(0)) mem_write32(ARG(0), display_mode);
	if(ARG(1)) mem_write32(ARG(1), display_width);
	if(ARG(2)) mem_write32(ARG(2), display_height);
	RETURN(0);
}

static void sceDisplaySetFrameBuf(void){
	u32 addr = ARG(0), stride = ARG(1), format = ARG(2);
	if(format > 3 || (addr && (stride < 480 || (stride & 63)))){
		RETURN(0x80000107u); /* INVALID_ARGUMENT */
		return;
	}
	if(addr && addr != fb.addr) hle_stat_flips++;   /* una imagen nueva (doble búfer) */
	fb.addr = addr;
	fb.stride = stride;
	fb.format = format;
	RETURN(0);
}

static void sceDisplayGetFrameBuf(void){
	if(ARG(0)) mem_write32(ARG(0), fb.addr);
	if(ARG(1)) mem_write32(ARG(1), fb.stride);
	if(ARG(2)) mem_write32(ARG(2), fb.format);
	RETURN(0);
}

static void sceDisplayWaitVblank(void){ kernel_wait_vblank(); }
static void sceDisplayWaitVblankCB(void){ kernel_cb_wait(sceDisplayWaitVblank); }

static void sceDisplayGetVcount(void){ RETURN((u32)(cpu_cycles / CYCLES_PER_FRAME)); }
static void sceDisplayIsVblank(void){ RETURN(0); }
static void sceDisplayGetFramePerSec(void){ union { float f; u32 u; } v = { 59.9400599f }; RETURN(v.u); }

/* 286 líneas por cuadro; como en PPSSPP, el contador nunca da 0 */
#define HCOUNT_PER_FRAME 286u
static u32 current_hcount(void){
	return 1u + (u32)((cpu_cycles % CYCLES_PER_FRAME) / (PSP_CPU_HZ / 60u / HCOUNT_PER_FRAME));
}
static void sceDisplayGetCurrentHcount(void){ RETURN(current_hcount()); }
static void sceDisplayGetAccumulatedHcount(void){
	RETURN((u32)(cpu_cycles / CYCLES_PER_FRAME) * HCOUNT_PER_FRAME + current_hcount());
}
static void sceDisplayGetVblankRest(void){
	RETURN((u32)((CYCLES_PER_FRAME - cpu_cycles % CYCLES_PER_FRAME) / CYCLES_PER_US));
}
static void sceDisplayIsForeground(void){ RETURN(fb.addr != 0); }
static void display_zero(void){ RETURN(0); }

/* --- sceCtrl --------------------------------------------------------- */

static void sceCtrlSetSamplingCycle(void){ RETURN(ctrl_cycle); ctrl_cycle = ARG(0); }
static void sceCtrlGetSamplingCycle(void){ if(ARG(0)) mem_write32(ARG(0), ctrl_cycle); RETURN(0); }
static void sceCtrlSetSamplingMode(void){ RETURN(ctrl_mode); ctrl_mode = ARG(0); }
static void sceCtrlGetSamplingMode(void){ if(ARG(0)) mem_write32(ARG(0), ctrl_mode); RETURN(0); }

static void sceCtrlPeekBufferPositive(void){
	u32 i, n = ARG(1);
	for(i = 0; i < n; i++) display_fill_ctrl(ARG(0) + i * 16);
	display_ctrl_read_done();
	RETURN(n);
}

/* Cada vblank hay una muestra nueva del mando */
void display_vblank(void){
	if(ctrl_consumed){ ctrl_consumed = 0; return; }
	if(ctrl_unread < 64) ctrl_unread++;
}

/* Como PPSSPP (__CtrlReadBuffer): con muestras sin leer devuelve en el acto
   cuántas copió; si no, espera a la del siguiente vblank y devuelve 1. El
   valor tiene que sobrevivir a la espera: antes el hilo despertaba con 0 y
   juegos como GTA tiraban los botones ("0 muestras"). */
static void sceCtrlReadBufferPositive(void){
	u32 n = ARG(1), i, got;
	if(n > 64){ RETURN(0x80000104u); return; }
	got = ctrl_unread ? (ctrl_unread < n ? ctrl_unread : n) : (n ? 1 : 0);
	for(i = 0; i < got; i++) display_fill_ctrl(ARG(0) + i * 16);
	display_ctrl_read_done();
	RETURN(got);
	if(ctrl_unread || !got || kernel_in_interrupt() || !kernel_dispatch_enabled()){ ctrl_unread = 0; return; }
	kernel_wait_vblank_ret(got);
	ctrl_unread = 0;
	ctrl_consumed = 1;   /* la muestra de ese vblank es la que se acaba de dar */
}

static const HleFunction display[] = {
	{ "sceDisplaySetMode", sceDisplaySetMode },
	{ "sceDisplayGetMode", sceDisplayGetMode },
	{ "sceDisplaySetFrameBuf", sceDisplaySetFrameBuf },
	{ "sceDisplayGetFrameBuf", sceDisplayGetFrameBuf },
	{ "sceDisplayWaitVblank", sceDisplayWaitVblank },
	{ "sceDisplayWaitVblankCB", sceDisplayWaitVblankCB },
	{ "sceDisplayWaitVblankStart", sceDisplayWaitVblank },
	{ "sceDisplayWaitVblankStartCB", sceDisplayWaitVblankCB },
	{ "sceDisplayGetVcount", sceDisplayGetVcount },
	{ "sceDisplayIsVblank", sceDisplayIsVblank },
	{ "sceDisplayGetFramePerSec", sceDisplayGetFramePerSec },
	{ "sceDisplayGetCurrentHcount", sceDisplayGetCurrentHcount },
	{ "sceDisplayGetAccumulatedHcount", sceDisplayGetAccumulatedHcount },
	{ "sceDisplayGetVblankRest", sceDisplayGetVblankRest },
	{ "sceDisplayIsForeground", sceDisplayIsForeground },
	{ "sceDisplaySetHoldMode", display_zero },
	{ "sceDisplaySetResumeMode", display_zero },
};

static const HleFunction ctrl[] = {
	{ "sceCtrlSetSamplingCycle", sceCtrlSetSamplingCycle },
	{ "sceCtrlGetSamplingCycle", sceCtrlGetSamplingCycle },
	{ "sceCtrlSetSamplingMode", sceCtrlSetSamplingMode },
	{ "sceCtrlGetSamplingMode", sceCtrlGetSamplingMode },
	{ "sceCtrlPeekBufferPositive", sceCtrlPeekBufferPositive },
	{ "sceCtrlReadBufferPositive", sceCtrlReadBufferPositive },
};

const HleLibrary hle_display_libs[] = {
	HLE_LIBRARY("sceDisplay", display),
	HLE_LIBRARY("sceCtrl", ctrl),
};
const u32 hle_display_libs_count = sizeof(hle_display_libs) / sizeof(hle_display_libs[0]);
