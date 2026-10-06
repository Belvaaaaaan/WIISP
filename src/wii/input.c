/**
 * WIISP - input.c
 * Wiimote y mando de GameCube: acciones del menú y botones de la PSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <wiiuse/wpad.h>
#include <fat.h>
#include "wii/wii.h"
#include "frontend/app.h"

#define GC(b) ((u32)(b) << 16)

/* Autorrepetición al mantener una dirección en el menú (en frames) */
#define REPEAT_DELAY 18
#define REPEAT_RATE  4

static volatile int power_pressed;

static void power_button(void){ power_pressed = 1; }
static void wiimote_power_button(s32 chan){ (void)chan; power_pressed = 1; }

void input_init(void){
	WPAD_Init();
	PAD_Init();
	SYS_SetPowerCallback(power_button);
	WPAD_SetPowerButtonCallback(wiimote_power_button);
}

void input_check_power(void){
	if(!power_pressed) return;
	fatUnmount("sd:");
	fatUnmount("usb:");
	SYS_ResetSystem(SYS_POWEROFF, 0, 0);
}

void input_read(Input *in){
	static u32 hold_frames;
	u32 dirs;

	input_check_power();
	WPAD_ScanPads();
	PAD_ScanPads();
	in->down = WPAD_ButtonsDown(0) | GC(PAD_ButtonsDown(0));
	in->held = WPAD_ButtonsHeld(0) | GC(PAD_ButtonsHeld(0));

	in->menu = 0;
	dirs = 0;
	if(in->held & (WPAD_BUTTON_UP | GC(PAD_BUTTON_UP))) dirs |= IN_UP;
	if(in->held & (WPAD_BUTTON_DOWN | GC(PAD_BUTTON_DOWN))) dirs |= IN_DOWN;
	if(in->held & (WPAD_BUTTON_LEFT | GC(PAD_BUTTON_LEFT))) dirs |= IN_LEFT;
	if(in->held & (WPAD_BUTTON_RIGHT | GC(PAD_BUTTON_RIGHT))) dirs |= IN_RIGHT;
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

int input_wants_exit(const Input *in){
	return (in->down & WPAD_BUTTON_HOME) ||
	       ((in->held & GC(PAD_TRIGGER_Z)) && (in->down & GC(PAD_BUTTON_START)));
}

/* Wiimote en vertical o mando de GameCube -> botones de la PSP */
void input_send_to_psp(const Input *in){
	static const struct { u32 wii; unsigned psp; } map[] = {
		{ WPAD_BUTTON_UP, APP_BTN_UP },       { WPAD_BUTTON_DOWN, APP_BTN_DOWN },
		{ WPAD_BUTTON_LEFT, APP_BTN_LEFT },   { WPAD_BUTTON_RIGHT, APP_BTN_RIGHT },
		{ WPAD_BUTTON_A, APP_BTN_CROSS },     { WPAD_BUTTON_B, APP_BTN_CIRCLE },
		{ WPAD_BUTTON_1, APP_BTN_SQUARE },    { WPAD_BUTTON_2, APP_BTN_TRIANGLE },
		{ WPAD_BUTTON_PLUS, APP_BTN_START },  { WPAD_BUTTON_MINUS, APP_BTN_SELECT },
		{ GC(PAD_BUTTON_UP), APP_BTN_UP },    { GC(PAD_BUTTON_DOWN), APP_BTN_DOWN },
		{ GC(PAD_BUTTON_LEFT), APP_BTN_LEFT },{ GC(PAD_BUTTON_RIGHT), APP_BTN_RIGHT },
		{ GC(PAD_BUTTON_A), APP_BTN_CROSS },  { GC(PAD_BUTTON_B), APP_BTN_CIRCLE },
		{ GC(PAD_BUTTON_Y), APP_BTN_SQUARE }, { GC(PAD_BUTTON_X), APP_BTN_TRIANGLE },
		{ GC(PAD_BUTTON_START), APP_BTN_START }, { GC(PAD_TRIGGER_Z), APP_BTN_SELECT },
		{ GC(PAD_TRIGGER_L), APP_BTN_LTRIGGER }, { GC(PAD_TRIGGER_R), APP_BTN_RTRIGGER },
	};
	unsigned buttons = 0, i;
	int sx = PAD_StickX(0), sy = PAD_StickY(0);
	for(i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if(in->held & map[i].wii) buttons |= map[i].psp;
	/* Stick de GameCube (-128..127, arriba positivo) -> PSP (0..255, arriba 0) */
	app_set_input(buttons, (unsigned char)(sx + 128), (unsigned char)(127 - sy));
}
