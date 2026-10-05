/**
 * WIISP - wii.h
 * Piezas del frontend del Wii (entrada, vídeo y menú).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_WII_H
#define WIISP_WII_H

#include <gccore.h>

/* --- Entrada (input.c) ---------------------------------------------------- */

/* Acciones del menú, comunes a Wiimote y mando de GameCube */
enum {
	IN_UP = 1 << 0, IN_DOWN = 1 << 1, IN_LEFT = 1 << 2, IN_RIGHT = 1 << 3,
	IN_ACCEPT = 1 << 4,   /* A */
	IN_BACK = 1 << 5,     /* B */
	IN_SWITCH = 1 << 6,   /* 1 / X: cambiar SD <-> USB */
	IN_EXIT = 1 << 7      /* HOME / START */
};

typedef struct {
	u32 down, held;       /* botones crudos: Wiimote bajos, GameCube << 16 */
	u32 menu;             /* acciones IN_* con autorrepetición */
} Input;

void input_init(void);
void input_read(Input *in);
/* Envía el estado de los mandos a la PSP emulada */
void input_send_to_psp(const Input *in);
/* HOME (Wiimote) o Z+START (GameCube) durante la emulación */
int  input_wants_exit(const Input *in);

/* --- Vídeo (video.c) ------------------------------------------------------ */

void video_init(void);
void video_show_console(void);
void video_clear_console(void);
/* Dibuja el framebuffer de la PSP (si hay) y el texto superpuesto */
void video_draw_psp_frame(const char *overlay);

/* --- Menú (menu.c) -------------------------------------------------------- */

/* Muestra el selector de archivos. Devuelve 1 y la ruta elegida, o 0 si el
   usuario quiere salir. */
int  menu_choose_file(char *out, int out_size);
/* Recuerda la carpeta del último archivo ejecutado */
void menu_remember(const char *file_path);
void menu_set_config_path(const char *path);

#endif
