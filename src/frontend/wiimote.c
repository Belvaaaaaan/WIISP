/**
 * WIISP - frontend/wiimote.c
 * El Wiimote solo en horizontal como mando de PSP (ver wiimote.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <math.h>
#include <string.h>
#include "frontend/app.h"
#include "frontend/wiimote.h"

#define RAD_TO_DEG 57.29577951f

void wm_init(WiimoteMap *m){
	memset(m, 0, sizeof(*m));
}

/* En horizontal con el sensor a la izquierda, la flecha de "arriba" apunta
   a la izquierda de la pantalla, "derecha" hacia arriba, etc. */
unsigned wm_dpad(unsigned held){
	unsigned d = 0;
	if(held & WM_RIGHT) d |= APP_BTN_UP;
	if(held & WM_LEFT) d |= APP_BTN_DOWN;
	if(held & WM_UP) d |= APP_BTN_LEFT;
	if(held & WM_DOWN) d |= APP_BTN_RIGHT;
	return d;
}

/* --- Movimiento ------------------------------------------------------------ */

enum { T_UP, T_DOWN, T_LEFT, T_RIGHT };

/* Cambia una dirección tras WM_TILT_REPORTS informes seguidos pidiéndolo */
static void tilt_dir(WiimoteMap *m, int i, unsigned bit, int beyond, int inside){
	int on = (m->tilt & bit) != 0;
	if(!(on ? inside : beyond)){ m->pending[i] = 0; return; }
	if(++m->pending[i] < WM_TILT_REPORTS) return;
	m->pending[i] = 0;
	m->tilt ^= bit;
	m->tilt_changed = 1;
}

/* Ejes del Wiimote (WiiBrew): en vertical, +x a la izquierda, +y hacia
   delante (el sensor) y +z hacia arriba; boca arriba en reposo mide
   (0, 0, +1 g). En horizontal con el sensor a la izquierda, +y apunta a la
   izquierda y +x hacia el jugador. Inclinar hacia delante sube el borde del
   jugador (+x hacia arriba); girar a la derecha como un volante sube el
   extremo del sensor (+y hacia arriba). */
void wm_accel(WiimoteMap *m, float gx, float gy, float gz){
	float mag2 = gx * gx + gy * gy + gz * gz, fw, sd;
	/* Una sacudida no dice hacia dónde está inclinado */
	if(!(mag2 >= 0.75f * 0.75f && mag2 <= 1.25f * 1.25f)) return;
	fw = atan2f(gx, sqrtf(gy * gy + gz * gz)) * RAD_TO_DEG;
	sd = atan2f(gy, sqrtf(gx * gx + gz * gz)) * RAD_TO_DEG;
	m->forward = fw;
	m->side = sd;
	tilt_dir(m, T_UP, APP_BTN_UP, fw >= WM_TILT_FORWARD, fw < WM_TILT_FORWARD - WM_TILT_HYST);
	tilt_dir(m, T_DOWN, APP_BTN_DOWN, fw <= -WM_TILT_BACK, fw > -WM_TILT_BACK + WM_TILT_HYST);
	tilt_dir(m, T_LEFT, APP_BTN_LEFT, sd <= -WM_TILT_SIDE, sd > -WM_TILT_SIDE + WM_TILT_HYST);
	tilt_dir(m, T_RIGHT, APP_BTN_RIGHT, sd >= WM_TILT_SIDE, sd < WM_TILT_SIDE - WM_TILT_HYST);
}

/* --- Botones ------------------------------------------------------------- */

void wm_step(WiimoteMap *m, unsigned held, unsigned now_ms, WiimoteOut *out){
	unsigned b = m->tilt, d = wm_dpad(held), prev = m->prev_held;
	int b_held = (held & WM_B) != 0, dx = 0, dy = 0;

	memset(out, 0, sizeof(*out));

	/* Cruceta -> stick; en diagonal, sobre el círculo */
	if(d & APP_BTN_LEFT) dx--;
	if(d & APP_BTN_RIGHT) dx++;
	if(d & APP_BTN_UP) dy--;
	if(d & APP_BTN_DOWN) dy++;
	{
		int r = dx && dy ? 90 : 128;
		int x = 128 + dx * r, y = 128 + dy * r;
		out->lx = (unsigned char)(x > 255 ? 255 : x);
		out->ly = (unsigned char)(y > 255 ? 255 : y);
		out->stick = dx || dy;
	}

	/* B: con 1, 2, A, - o + ya pulsados es el triángulo en el acto; si no,
	   puede ser el principio de una combinación */
	if(b_held && !(prev & WM_B)){
		m->b_since = now_ms;
		m->b_combo = 0;
		m->b_mod = !(prev & (WM_1 | WM_2 | WM_A | WM_MINUS | WM_PLUS));
		m->b_tri = !m->b_mod;
	}
	if(b_held){
		if(m->b_mod && (held & WM_A) && !(prev & WM_A)){
			out->turbo_toggle = 1;
			m->b_combo = 1;
			m->b_tri = 0;
		}
		if(m->b_mod && (held & WM_MINUS) && !(prev & WM_MINUS)) out->diag_toggle |= WM_DIAG_MATH;
		if(m->b_mod && (held & WM_PLUS) && !(prev & WM_PLUS)) out->diag_toggle |= WM_DIAG_TEX;
		if(m->b_mod && (held & (WM_MINUS | WM_PLUS))){
			m->b_combo = 1;
			m->b_tri = 0;
		}
		if(m->b_mod && (held & (WM_1 | WM_2))){
			m->b_combo = 1;
			m->b_tri = 0;
			if(held & WM_1) b |= APP_BTN_SELECT;
			if(held & WM_2) b |= APP_BTN_START;
		}
		if(m->b_mod && !m->b_combo && !m->b_tri && now_ms - m->b_since >= WM_COMBO_MS) m->b_tri = 1;
		if(m->b_tri) b |= APP_BTN_TRIANGLE;
	} else if(prev & WM_B){
		/* Un toque corto, sin combinación: el triángulo al soltar */
		if(m->b_mod && !m->b_combo && !m->b_tri) b |= APP_BTN_TRIANGLE;
		m->b_mod = m->b_combo = m->b_tri = 0;
	}
	if(!(b_held && m->b_mod)){
		if(held & WM_MINUS) b |= APP_BTN_LTRIGGER;
		if(held & WM_PLUS) b |= APP_BTN_RTRIGGER;
		if(held & WM_A) b |= APP_BTN_SQUARE;
		if(held & WM_1) b |= APP_BTN_CROSS;
		if(held & WM_2) b |= APP_BTN_CIRCLE;
	}

	/* HOME mantenido: salir al menú */
	if(held & WM_HOME){
		if(!(prev & WM_HOME)) m->home_since = now_ms;
		out->home_ms = (int)(now_ms - m->home_since) + 1;
		out->exit = now_ms - m->home_since >= WM_HOME_MS;
	}

	out->buttons = b;
	m->prev_held = held;
}
