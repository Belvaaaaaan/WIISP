/**
 * WIISP - input.c
 * Wiimote y mando de GameCube: acciones del menú y botones de la PSP.
 *
 * El Wiimote va en horizontal (frontend/wiimote.h tiene el mapeo y la
 * cruceta por movimiento); el mando de GameCube, como siempre. Los
 * informes del Wiimote se leen todos (WPAD_ReadPending): el acelerómetro
 * manda unos 100 por segundo y la cola guarda más de un segundo, así que
 * aunque la emulación vaya lenta no se pierde ni la inclinación ni un
 * toque corto (libogc conserva la primera pulsación de cada botón).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include "hle/hle.h"
#include "wii/wii.h"
#include "frontend/app.h"
#include "frontend/wiimote.h"

#define GC(b) ((u32)(b) << 16)

/* Autorrepetición al mantener una dirección en el menú (en frames) */
#define REPEAT_DELAY 18
#define REPEAT_RATE  4

static volatile int power_pressed;

static WiimoteMap wmap;
static WiimoteOut wout;
static WPADData wpad_queue[160];   /* ~1,6 s de informes */
static int tilt_logs;

static void power_button(void){ power_pressed = 1; }
static void wiimote_power_button(s32 chan){ (void)chan; power_pressed = 1; }

void input_init(void){
	WPAD_Init();
	PAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC);
	WPAD_SetEventBufs(WPAD_CHAN_0, wpad_queue, sizeof(wpad_queue) / sizeof(wpad_queue[0]));
	wm_init(&wmap);
	SYS_SetPowerCallback(power_button);
	WPAD_SetPowerButtonCallback(wiimote_power_button);
}

void input_check_power(void){
	if(!power_pressed) return;
	hle_log_stats();
	hle_set_log_file(NULL);   /* cierra wiisp.log antes de desmontar */
	fatUnmount("sd:");
	fatUnmount("usb:");
	SYS_ResetSystem(SYS_POWEROFF, 0, 0);
}

/* Cada informe del Wiimote: la gravedad para la cruceta por movimiento */
static void wpad_event(s32 chan, const WPADData *d){
	if(chan == WPAD_CHAN_0 && (d->data_present & WPAD_DATA_ACCEL))
		wm_accel(&wmap, d->gforce.x, d->gforce.y, d->gforce.z);
}

/* Botones de libogc -> WM_* */
static unsigned wiimote_bits(u32 b){
	static const struct { u32 wpad; unsigned wm; } map[] = {
		{ WPAD_BUTTON_UP, WM_UP }, { WPAD_BUTTON_DOWN, WM_DOWN }, { WPAD_BUTTON_LEFT, WM_LEFT },
		{ WPAD_BUTTON_RIGHT, WM_RIGHT }, { WPAD_BUTTON_A, WM_A }, { WPAD_BUTTON_B, WM_B },
		{ WPAD_BUTTON_1, WM_1 }, { WPAD_BUTTON_2, WM_2 }, { WPAD_BUTTON_PLUS, WM_PLUS },
		{ WPAD_BUTTON_MINUS, WM_MINUS }, { WPAD_BUTTON_HOME, WM_HOME },
	};
	unsigned r = 0, i;
	for(i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if(b & map[i].wpad) r |= map[i].wm;
	return r;
}

void input_read(Input *in){
	static u32 hold_frames;
	u32 dirs;
	unsigned d;

	input_check_power();
	WPAD_ReadPending(WPAD_CHAN_ALL, wpad_event);
	PAD_ScanPads();
	in->down = WPAD_ButtonsDown(0) | GC(PAD_ButtonsDown(0));
	in->held = WPAD_ButtonsHeld(0) | GC(PAD_ButtonsHeld(0));
	wm_step(&wmap, wiimote_bits(in->held & 0xFFFFu), (unsigned)ticks_to_millisecs(gettime()), &wout);
	if(wmap.tilt_changed){
		wmap.tilt_changed = 0;
		if(tilt_logs < 40){
			tilt_logs++;
			hle_log_quiet("[MANDO] cruceta por movimiento:%s%s%s%s%s (adelante %d, giro %d grados)\n",
			        wmap.tilt & APP_BTN_UP ? " arriba" : "", wmap.tilt & APP_BTN_DOWN ? " abajo" : "",
			        wmap.tilt & APP_BTN_LEFT ? " izquierda" : "", wmap.tilt & APP_BTN_RIGHT ? " derecha" : "",
			        wmap.tilt ? "" : " suelta", (int)wmap.forward, (int)wmap.side);
		}
	}

	in->menu = 0;
	dirs = 0;
	/* El Wiimote en horizontal también en el menú */
	d = wm_dpad(wiimote_bits(in->held & 0xFFFFu));
	if((d & APP_BTN_UP) || (in->held & GC(PAD_BUTTON_UP))) dirs |= IN_UP;
	if((d & APP_BTN_DOWN) || (in->held & GC(PAD_BUTTON_DOWN))) dirs |= IN_DOWN;
	if((d & APP_BTN_LEFT) || (in->held & GC(PAD_BUTTON_LEFT))) dirs |= IN_LEFT;
	if((d & APP_BTN_RIGHT) || (in->held & GC(PAD_BUTTON_RIGHT))) dirs |= IN_RIGHT;
	if(dirs){
		hold_frames++;
		if(hold_frames == 1 || (hold_frames > REPEAT_DELAY && (hold_frames % REPEAT_RATE) == 0))
			in->menu |= dirs;
	} else hold_frames = 0;

	if(in->down & (WPAD_BUTTON_A | GC(PAD_BUTTON_A))) in->menu |= IN_ACCEPT;
	if(in->down & (WPAD_BUTTON_B | GC(PAD_BUTTON_B))) in->menu |= IN_BACK;
	if(in->down & (WPAD_BUTTON_1 | GC(PAD_BUTTON_X))) in->menu |= IN_SWITCH;
	if(in->down & (WPAD_BUTTON_HOME | GC(PAD_BUTTON_START))) in->menu |= IN_EXIT;
	if(in->down & (WPAD_BUTTON_2 | GC(PAD_BUTTON_Y))) in->menu |= IN_OPTION;
}

/* HOME mantenido (Wiimote) o Z+START (GameCube) */
int input_wants_exit(const Input *in){
	return wout.exit || ((in->held & GC(PAD_TRIGGER_Z)) && (in->down & GC(PAD_BUTTON_START)));
}

int input_home_ms(void){ return wout.home_ms; }

/* B + A (Wiimote) o Z + Y (GameCube) recién pulsados */
int input_turbo_toggle(const Input *in){
	return wout.turbo_toggle || ((in->held & GC(PAD_TRIGGER_Z)) && (in->down & GC(PAD_BUTTON_Y)));
}

/* B + - / B + + (Wiimote) o Z + L / Z + R (GameCube) recién pulsados:
   WM_DIAG_MATH y WM_DIAG_TEX */
int input_diag_toggle(const Input *in){
	int t = wout.diag_toggle;
	if(in->held & GC(PAD_TRIGGER_Z)){
		if(in->down & GC(PAD_TRIGGER_L)) t |= WM_DIAG_MATH;
		if(in->down & GC(PAD_TRIGGER_R)) t |= WM_DIAG_TEX;
	}
	return t;
}

/* Wiimote en horizontal (frontend/wiimote.c) y mando de GameCube -> PSP */
void input_send_to_psp(const Input *in){
	static const struct { u32 wii; unsigned psp; } map[] = {
		{ GC(PAD_BUTTON_UP), APP_BTN_UP },    { GC(PAD_BUTTON_DOWN), APP_BTN_DOWN },
		{ GC(PAD_BUTTON_LEFT), APP_BTN_LEFT },{ GC(PAD_BUTTON_RIGHT), APP_BTN_RIGHT },
		{ GC(PAD_BUTTON_A), APP_BTN_CROSS },  { GC(PAD_BUTTON_B), APP_BTN_CIRCLE },
		{ GC(PAD_BUTTON_Y), APP_BTN_SQUARE }, { GC(PAD_BUTTON_X), APP_BTN_TRIANGLE },
		{ GC(PAD_BUTTON_START), APP_BTN_START }, { GC(PAD_TRIGGER_Z), APP_BTN_SELECT },
		{ GC(PAD_TRIGGER_L), APP_BTN_LTRIGGER }, { GC(PAD_TRIGGER_R), APP_BTN_RTRIGGER },
	};
	unsigned buttons = wout.buttons, i;
	int sx = PAD_StickX(0), sy = PAD_StickY(0);
	for(i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if(in->held & map[i].wii) buttons |= map[i].psp;
	/* La cruceta del Wiimote manda sobre el stick de GameCube (-128..127,
	   arriba positivo -> PSP 0..255, arriba 0) */
	if(wout.stick) app_set_input(buttons, wout.lx, wout.ly);
	else app_set_input(buttons, (unsigned char)(sx + 128), (unsigned char)(127 - sy));
}
