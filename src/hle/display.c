/**
 * WIISP - display.c
 * HLE de sceDisplay, sceCtrl y lo mínimo de sceGe_user.
 *
 * El framebuffer de la PSP vive en su VRAM o RAM; aquí solo se guarda dónde
 * está y en qué formato, y el frontend lo convierte al dibujar.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "hle/hle.h"
#include "core/memory.h"

static HleFramebuffer fb;
static u32 display_mode, display_width, display_height;
static u32 input_buttons;
static u8 input_lx = 128, input_ly = 128;
static u32 ctrl_cycle, ctrl_mode;

void display_init(void){
	memset(&fb, 0, sizeof(fb));
	display_mode = 0;
	display_width = 480;
	display_height = 272;
	ctrl_cycle = ctrl_mode = 0;
}

void hle_get_framebuffer(HleFramebuffer *out){ *out = fb; }

void hle_set_input(u32 buttons, u8 lx, u8 ly){
	input_buttons = buttons;
	input_lx = lx;
	input_ly = ly;
}

/* SceCtrlData: u32 TimeStamp, u32 Buttons, u8 Lx, u8 Ly, u8 Rsrv[6] */
void display_fill_ctrl(u32 addr){
	u8 *p = mem_ptr(addr, 16);
	if(!p) return;
	memset(p, 0, 16);
	wr_le32(p, (u32)hle_now_us());
	wr_le32(p + 4, input_buttons);
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

static void sceDisplayGetVcount(void){ RETURN((u32)(cpu_cycles / CYCLES_PER_FRAME)); }
static void sceDisplayIsVblank(void){ RETURN(0); }
static void sceDisplayGetFramePerSec(void){ union { float f; u32 u; } v = { 59.9400599f }; RETURN(v.u); }

/* --- sceCtrl --------------------------------------------------------- */

static void sceCtrlSetSamplingCycle(void){ RETURN(ctrl_cycle); ctrl_cycle = ARG(0); }
static void sceCtrlGetSamplingCycle(void){ if(ARG(0)) mem_write32(ARG(0), ctrl_cycle); RETURN(0); }
static void sceCtrlSetSamplingMode(void){ RETURN(ctrl_mode); ctrl_mode = ARG(0); }
static void sceCtrlGetSamplingMode(void){ if(ARG(0)) mem_write32(ARG(0), ctrl_mode); RETURN(0); }

static void sceCtrlPeekBufferPositive(void){
	u32 i, n = ARG(1);
	for(i = 0; i < n; i++) display_fill_ctrl(ARG(0) + i * 16);
	RETURN(n);
}

static void sceCtrlReadBufferPositive(void){
	/* La lectura bloquea hasta la siguiente muestra (cada vblank) */
	u32 n = ARG(1), i;
	for(i = 0; i < n; i++) display_fill_ctrl(ARG(0) + i * 16);
	kernel_wait_vblank();
	RETURN(n);
}

/* --- sceGe_user (mínimo hasta la fase 4) ------------------------------ */

static void sceGeEdramGetAddr(void){ RETURN(PSP_VRAM_BASE); }
static void sceGeEdramGetSize(void){ RETURN(PSP_VRAM_SIZE); }
static void return_zero(void){ RETURN(0); }

static const HleFunction display[] = {
	{ "sceDisplaySetMode", sceDisplaySetMode },
	{ "sceDisplayGetMode", sceDisplayGetMode },
	{ "sceDisplaySetFrameBuf", sceDisplaySetFrameBuf },
	{ "sceDisplayGetFrameBuf", sceDisplayGetFrameBuf },
	{ "sceDisplayWaitVblank", sceDisplayWaitVblank },
	{ "sceDisplayWaitVblankCB", sceDisplayWaitVblank },
	{ "sceDisplayWaitVblankStart", sceDisplayWaitVblank },
	{ "sceDisplayWaitVblankStartCB", sceDisplayWaitVblank },
	{ "sceDisplayGetVcount", sceDisplayGetVcount },
	{ "sceDisplayIsVblank", sceDisplayIsVblank },
	{ "sceDisplayGetFramePerSec", sceDisplayGetFramePerSec },
};

static const HleFunction ctrl[] = {
	{ "sceCtrlSetSamplingCycle", sceCtrlSetSamplingCycle },
	{ "sceCtrlGetSamplingCycle", sceCtrlGetSamplingCycle },
	{ "sceCtrlSetSamplingMode", sceCtrlSetSamplingMode },
	{ "sceCtrlGetSamplingMode", sceCtrlGetSamplingMode },
	{ "sceCtrlPeekBufferPositive", sceCtrlPeekBufferPositive },
	{ "sceCtrlReadBufferPositive", sceCtrlReadBufferPositive },
};

static const HleFunction ge[] = {
	{ "sceGeEdramGetAddr", sceGeEdramGetAddr },
	{ "sceGeEdramGetSize", sceGeEdramGetSize },
	{ "sceGeDrawSync", return_zero },
	{ "sceGeListSync", return_zero },
};

const HleLibrary hle_display_libs[] = {
	HLE_LIBRARY("sceDisplay", display),
	HLE_LIBRARY("sceCtrl", ctrl),
	HLE_LIBRARY("sceGe_user", ge),
};
const u32 hle_display_libs_count = sizeof(hle_display_libs) / sizeof(hle_display_libs[0]);
