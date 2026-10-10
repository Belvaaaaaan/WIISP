/**
 * WIISP - frontend/wiimote.h
 * El Wiimote solo, en horizontal (como un mando de NES: la cruceta a la
 * izquierda), como mando de PSP:
 *
 *   PSP                Wiimote
 *   stick analógico    cruceta
 *   X / O              1 / 2
 *   triángulo          B
 *   cuadrado           A
 *   L / R              - / +
 *   SELECT / START     B + 1 / B + 2
 *   cruceta            movimiento: inclinar hacia delante (arriba) o hacia
 *                      ti (abajo) y girar como un volante (izquierda y
 *                      derecha), hasta cerca del tope
 *   (salir al menú)    mantener HOME 1 segundo
 *
 * Con B pulsado, 1 y 2 son SELECT y START. Por eso B espera WM_COMBO_MS
 * antes de mandar el triángulo (si en ese tiempo llega 1 o 2, era una
 * combinación y el triángulo no llega a pulsarse); un toque corto de B
 * manda el triángulo al soltarlo. Si 1 o 2 ya estaban pulsados antes que
 * B, B es el triángulo en el acto y 1 y 2 siguen siendo X y O.
 *
 * El movimiento usa la gravedad que mide el Wiimote en cada informe (unos
 * 100 por segundo, se emule rápido o lento): hace falta pasar del umbral
 * en WM_TILT_REPORTS informes seguidos, se suelta WM_TILT_HYST grados
 * antes (para que no parpadee) y los informes de una sacudida (más o menos
 * de 1 g) no cuentan.
 *
 * No depende de libogc: se prueba en el PC (tests/test_wiimote.c).
 * Solo tipos de C estándar (como app.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_WIIMOTE_H
#define WIISP_WIIMOTE_H

/* Botones físicos, con los nombres del Wiimote en vertical */
#define WM_UP     0x0001u
#define WM_DOWN   0x0002u
#define WM_LEFT   0x0004u
#define WM_RIGHT  0x0008u
#define WM_A      0x0010u
#define WM_B      0x0020u
#define WM_1      0x0040u
#define WM_2      0x0080u
#define WM_PLUS   0x0100u
#define WM_MINUS  0x0200u
#define WM_HOME   0x0400u

#define WM_COMBO_MS     120   /* B espera esto antes de ser triángulo */
#define WM_HOME_MS      1000  /* HOME mantenido para salir */
#define WM_TILT_FORWARD 50    /* grados hacia delante: cruceta arriba */
#define WM_TILT_BACK    75    /* grados hacia ti: cruceta abajo */
#define WM_TILT_SIDE    55    /* grados de giro: izquierda y derecha */
#define WM_TILT_HYST    15    /* se suelta tantos grados antes */
#define WM_TILT_REPORTS 3     /* informes seguidos para cambiar */

typedef struct {
	unsigned prev_held;
	/* B: modificador o triángulo */
	int b_mod;            /* esta pulsación de B puede ser combinación */
	int b_combo;          /* ya hubo B+1 o B+2 */
	int b_tri;            /* el triángulo está pulsado */
	unsigned b_since;
	unsigned home_since;
	/* Movimiento: direcciones activas (APP_BTN_*) y su rebote */
	unsigned tilt;
	int pending[4];
	float forward, side;  /* últimos ángulos (grados) */
	int tilt_changed;     /* cambió desde la última vez que se miró */
} WiimoteMap;

typedef struct {
	unsigned buttons;     /* APP_BTN_* */
	unsigned char lx, ly; /* stick: 0-255, 128 en el centro, arriba 0 */
	int stick;            /* la cruceta mueve el stick */
	int exit;             /* HOME mantenido el tiempo suficiente */
	int home_ms;          /* cuánto lleva HOME pulsado (0 si no) */
} WiimoteOut;

void wm_init(WiimoteMap *m);
/* Cada informe del acelerómetro (gravedad en g, ejes del Wiimote) */
void wm_accel(WiimoteMap *m, float gx, float gy, float gz);
/* Botones sujetos ahora (WM_*) y tiempo real en ms: lo que va a la PSP */
void wm_step(WiimoteMap *m, unsigned held, unsigned now_ms, WiimoteOut *out);
/* La cruceta girada para el Wiimote en horizontal, como APP_BTN_UP... */
unsigned wm_dpad(unsigned held);

#endif
