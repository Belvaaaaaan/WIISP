/**
 * WIISP - tests/test_wiimote.c
 * El Wiimote en horizontal como mando de PSP (frontend/wiimote.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include "frontend/app.h"
#include "frontend/wiimote.h"
#include "test.h"

static WiimoteMap wm;
static WiimoteOut wo;

static unsigned step(unsigned held, unsigned t){
	wm_step(&wm, held, t, &wo);
	return wo.buttons;
}

/* Gravedad con el Wiimote inclinado fw grados hacia delante y sd de giro */
static void tilt(float fw, float sd, int reports){
	float a = fw / 57.29577951f, b = sd / 57.29577951f;
	float gx = sinf(a), gy = sinf(b), gz = sqrtf(fmaxf(0.0f, 1.0f - gx * gx - gy * gy));
	while(reports-- > 0) wm_accel(&wm, gx, gy, gz);
}

static void test_wiimote_buttons(void){
	printf("Wiimote: botones y stick\n");
	wm_init(&wm);
	CHECK_EQ(step(0, 0), 0);
	CHECK_EQ(wo.lx, 128); CHECK_EQ(wo.ly, 128); CHECK(!wo.stick);
	/* En horizontal: la flecha de "derecha" es arriba en pantalla */
	step(WM_RIGHT, 10);
	CHECK_EQ(wo.lx, 128); CHECK_EQ(wo.ly, 0); CHECK(wo.stick);
	step(WM_LEFT, 20);   CHECK_EQ(wo.ly, 255);
	step(WM_UP, 30);     CHECK_EQ(wo.lx, 0);
	step(WM_DOWN, 40);   CHECK_EQ(wo.lx, 255);
	step(WM_DOWN | WM_RIGHT, 50);   /* derecha y arriba en pantalla */
	CHECK_EQ(wo.lx, 218); CHECK_EQ(wo.ly, 38);
	CHECK_EQ(wm_dpad(WM_RIGHT | WM_UP), APP_BTN_UP | APP_BTN_LEFT);

	CHECK_EQ(step(WM_A, 60), APP_BTN_SQUARE);
	CHECK_EQ(step(WM_MINUS, 70), APP_BTN_LTRIGGER);
	CHECK_EQ(step(WM_PLUS, 80), APP_BTN_RTRIGGER);
	CHECK_EQ(step(WM_1, 90), APP_BTN_CROSS);
	CHECK_EQ(step(WM_2, 100), APP_BTN_CIRCLE);
	CHECK_EQ(step(WM_1 | WM_2 | WM_A, 110), APP_BTN_CROSS | APP_BTN_CIRCLE | APP_BTN_SQUARE);
	CHECK_EQ(step(0, 120), 0);
}

static void test_wiimote_b(void){
	printf("Wiimote: B como triangulo y B+1 / B+2\n");

	/* Toque corto: el triángulo al soltar, un paso */
	wm_init(&wm);
	CHECK_EQ(step(WM_B, 1000), 0);
	CHECK_EQ(step(0, 1050), APP_BTN_TRIANGLE);
	CHECK_EQ(step(0, 1100), 0);

	/* Mantenido: triángulo tras WM_COMBO_MS */
	CHECK_EQ(step(WM_B, 2000), 0);
	CHECK_EQ(step(WM_B, 2000 + WM_COMBO_MS - 1), 0);
	CHECK_EQ(step(WM_B, 2000 + WM_COMBO_MS), APP_BTN_TRIANGLE);
	CHECK_EQ(step(WM_B, 2500), APP_BTN_TRIANGLE);
	CHECK_EQ(step(0, 2600), 0);

	/* B y luego 2: START, sin triángulo ni O, tampoco al soltar */
	CHECK_EQ(step(WM_B, 3000), 0);
	CHECK_EQ(step(WM_B | WM_2, 3050), APP_BTN_START);
	CHECK_EQ(step(WM_B | WM_2, 3400), APP_BTN_START);
	CHECK_EQ(step(WM_B, 3500), 0);
	CHECK_EQ(step(WM_B, 3900), 0);
	CHECK_EQ(step(0, 4000), 0);

	/* B y 1 a la vez: SELECT */
	CHECK_EQ(step(WM_B | WM_1, 5000), APP_BTN_SELECT);
	CHECK_EQ(step(0, 5100), 0);

	/* 1 ya pulsado y luego B: X y triángulo en el acto */
	CHECK_EQ(step(WM_1, 6000), APP_BTN_CROSS);
	CHECK_EQ(step(WM_1 | WM_B, 6010), APP_BTN_CROSS | APP_BTN_TRIANGLE);
	CHECK_EQ(step(WM_1 | WM_B | WM_2, 6020), APP_BTN_CROSS | APP_BTN_CIRCLE | APP_BTN_TRIANGLE);
	CHECK_EQ(step(WM_B, 6030), APP_BTN_TRIANGLE);
	CHECK_EQ(step(0, 6040), 0);

	/* Triángulo ya mandado y luego 2: pasa a START */
	CHECK_EQ(step(WM_B, 7000), 0);
	CHECK_EQ(step(WM_B, 7200), APP_BTN_TRIANGLE);
	CHECK_EQ(step(WM_B | WM_2, 7300), APP_BTN_START);
	CHECK_EQ(step(0, 7400), 0);

	/* Los demás botones siguen igual con B */
	CHECK_EQ(step(WM_B | WM_A | WM_PLUS, 8000), APP_BTN_SQUARE | APP_BTN_RTRIGGER);
	CHECK_EQ(step(WM_B | WM_A | WM_PLUS, 8000 + WM_COMBO_MS), APP_BTN_SQUARE | APP_BTN_RTRIGGER | APP_BTN_TRIANGLE);
	CHECK_EQ(step(0, 8500), 0);

	/* El reloj puede dar la vuelta */
	CHECK_EQ(step(WM_B, 0xFFFFFFF0u), 0);
	CHECK_EQ(step(WM_B, 0xFFFFFFF0u + WM_COMBO_MS), APP_BTN_TRIANGLE);
	CHECK_EQ(step(0, 0xFFFFFFF0u + WM_COMBO_MS + 10), 0);
}

static void test_wiimote_home(void){
	printf("Wiimote: HOME mantenido\n");
	wm_init(&wm);
	step(WM_HOME, 100);
	CHECK(!wo.exit); CHECK(wo.home_ms > 0);
	step(WM_HOME, 100 + WM_HOME_MS - 1);
	CHECK(!wo.exit);
	step(WM_HOME, 100 + WM_HOME_MS);
	CHECK(wo.exit);
	step(0, 2000);
	CHECK(!wo.exit); CHECK_EQ(wo.home_ms, 0);
	/* Soltar a medias vuelve a empezar */
	step(WM_HOME, 3000);
	step(0, 3500);
	step(WM_HOME, 3600);
	step(WM_HOME, 3600 + WM_HOME_MS - 1);
	CHECK(!wo.exit);
	/* HOME no llega a la PSP */
	CHECK_EQ(wo.buttons, 0);
}

static void test_wiimote_tilt(void){
	printf("Wiimote: cruceta por movimiento\n");
	wm_init(&wm);
	tilt(-30.0f, 0.0f, 10);                     /* en las manos, un poco hacia ti */
	CHECK_EQ(step(0, 0), 0);

	/* Adelante: hace falta llegar al umbral en WM_TILT_REPORTS informes */
	tilt(WM_TILT_FORWARD + 5.0f, 0.0f, WM_TILT_REPORTS - 1);
	CHECK_EQ(step(0, 10), 0);
	tilt(WM_TILT_FORWARD + 5.0f, 0.0f, 1);
	CHECK_EQ(step(0, 20), APP_BTN_UP);
	CHECK(wm.tilt_changed);
	/* Casi al umbral no se suelta (histéresis) */
	tilt(WM_TILT_FORWARD - 5.0f, 0.0f, 10);
	CHECK_EQ(step(0, 30), APP_BTN_UP);
	tilt(WM_TILT_FORWARD - WM_TILT_HYST - 5.0f, 0.0f, WM_TILT_REPORTS);
	CHECK_EQ(step(0, 40), 0);
	/* Lejos del tope no hace nada */
	tilt(WM_TILT_FORWARD - 10.0f, 0.0f, 20);
	tilt(-WM_TILT_BACK + 10.0f, 0.0f, 20);
	tilt(0.0f, WM_TILT_SIDE - 10.0f, 20);
	CHECK_EQ(step(0, 50), 0);
	/* Hacia ti y los giros */
	tilt(-WM_TILT_BACK - 5.0f, 0.0f, WM_TILT_REPORTS);
	CHECK_EQ(step(0, 60), APP_BTN_DOWN);
	tilt(0.0f, 0.0f, WM_TILT_REPORTS);
	tilt(0.0f, WM_TILT_SIDE + 5.0f, WM_TILT_REPORTS);
	CHECK_EQ(step(0, 70), APP_BTN_RIGHT);
	tilt(0.0f, -(WM_TILT_SIDE + 5.0f), WM_TILT_REPORTS);
	CHECK_EQ(step(0, 80), APP_BTN_LEFT);
	tilt(0.0f, 0.0f, WM_TILT_REPORTS);
	CHECK_EQ(step(0, 90), 0);

	/* Un informe suelto en medio reinicia la cuenta */
	tilt(WM_TILT_FORWARD + 5.0f, 0.0f, WM_TILT_REPORTS - 1);
	tilt(0.0f, 0.0f, 1);
	tilt(WM_TILT_FORWARD + 5.0f, 0.0f, WM_TILT_REPORTS - 1);
	CHECK_EQ(step(0, 100), 0);

	/* Una sacudida (más de 1 g) no cuenta */
	{
		int i;
		for(i = 0; i < 10; i++) wm_accel(&wm, 2.0f, 0.0f, 0.5f);
		CHECK_EQ(step(0, 110), 0);
		for(i = 0; i < 10; i++) wm_accel(&wm, 0.1f, 0.0f, 0.2f);
		CHECK_EQ(step(0, 120), 0);
	}

	/* Junto con los botones */
	tilt(WM_TILT_FORWARD + 5.0f, 0.0f, WM_TILT_REPORTS);
	CHECK_EQ(step(WM_1, 130), APP_BTN_UP | APP_BTN_CROSS);
	CHECK_EQ(wo.lx, 128); CHECK_EQ(wo.ly, 128);   /* la cruceta del Wiimote es el stick */
}

void test_wiimote(void){
	test_wiimote_buttons();
	test_wiimote_b();
	test_wiimote_home();
	test_wiimote_tilt();
}
