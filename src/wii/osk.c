/**
 * WIISP - osk.c
 * Teclado en pantalla para sceUtilityOsk (nombres de perfil, etc.).
 *
 * Es un diálogo modal en la consola de texto, como el selector de archivos:
 * la emulación se detiene mientras está abierto y, al cerrarlo, el siguiente
 * fotograma vuelve a mostrar la imagen del juego.
 *
 *   Cruceta: moverse por las teclas     A: escribir la tecla elegida
 *   B: borrar la última letra           1 / X: mayúsculas <-> minúsculas
 *   2 / Y: aceptar                      HOME / START: cancelar
 *
 * La última fila tiene teclas de acción (MAYUS, ESPACIO, BORRAR, ACEPTAR,
 * CANCELAR) para poder hacerlo todo solo con A y la cruceta.
 *
 * Solo escribe caracteres ASCII; el texto inicial del juego se conserva tal
 * cual aunque tenga otros caracteres (se ven como '?').
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <string.h>
#include "wii/wii.h"
#include "frontend/app.h"

#define KEY_COLS    10
#define KEY_ROWS    6                 /* filas de caracteres */
#define ACTION_ROW  KEY_ROWS          /* la fila de acciones */
#define ACTION_KEYS 5
#define TEXT_MAX    256               /* igual que OSK_TEXT_MAX de utility.c */

static const char *const key_rows[KEY_ROWS] = {
	"abcdefghij",
	"klmnopqrst",
	"uvwxyz.,-_",
	"0123456789",
	"!?'\":;()/&",
	"@#$%+=*<>~",
};

enum { ACT_SHIFT, ACT_SPACE, ACT_DELETE, ACT_OK, ACT_CANCEL };
static const char *const action_names[ACTION_KEYS] = { "MAYUS", "ESPACIO", "BORRAR", "ACEPTAR", "CANCELAR" };

typedef struct {
	unsigned short text[TEXT_MAX + 1];
	unsigned len, limit;
	int shift;
	int row, col;          /* col: 0..9 en las filas de caracteres, 0..4 en la de acciones */
} Osk;

static char show_char(unsigned short c){
	return c >= 32 && c < 127 ? (char)c : '?';
}

static char key_char(const Osk *k, int row, int col){
	char c = key_rows[row][col];
	if(k->shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	return c;
}

static void put_char(Osk *k, char c){
	if(k->len >= k->limit) return;
	k->text[k->len++] = (unsigned short)(unsigned char)c;
	k->text[k->len] = 0;
}

static void delete_char(Osk *k){
	if(!k->len) return;
	k->text[--k->len] = 0;
}

static void draw(const Osk *k, const char *title){
	unsigned i;
	int r, c;
	video_clear_console();
	printf("\n TECLADO");
	if(title[0]) printf(": %.60s", title);
	printf("\n\n \x1b[33m");
	for(i = 0; i < k->len && i < 64; i++) putchar(show_char(k->text[i]));
	if(k->len > 64) printf("...");
	printf("_\x1b[37m   (%u/%u)\n\n", k->len, k->limit);
	for(r = 0; r < KEY_ROWS; r++){
		printf("   ");
		for(c = 0; c < KEY_COLS; c++){
			char ch = key_char(k, r, c);
			if(r == k->row && c == k->col) printf("\x1b[30;43m %c \x1b[37;40m", ch);
			else printf(" %c ", ch);
		}
		printf("\n");
	}
	printf("\n   ");
	for(c = 0; c < ACTION_KEYS; c++){
		const char *name = c == ACT_SHIFT && k->shift ? "minusc." : action_names[c];
		if(k->row == ACTION_ROW && k->col == c) printf("\x1b[30;43m[%s]\x1b[37;40m ", name);
		else printf("[%s] ", name);
	}
	printf("\n\n A: escribir   B: borrar   1/X: mayus   2/Y: aceptar   HOME/START: cancelar\n");
}

/* Espera a que se suelten los botones, para que el último A o 2 no llegue
   también al juego */
static void wait_release(void){
	Input in;
	int i;
	for(i = 0; i < 120; i++){
		VIDEO_WaitVSync();
		input_read(&in);
		if(!in.held) break;
	}
}

/* Devuelve 1 si aceptó, 0 si canceló */
int wii_osk(const unsigned short *desc, const unsigned short *initial, unsigned short *out,
            unsigned limit, unsigned lines, unsigned inputtype){
	static Osk k;
	char title[64];
	unsigned i;
	int redraw = 1, result = -1;

	(void)lines;
	(void)inputtype;   /* todos los caracteres de la tabla valen */

	memset(&k, 0, sizeof(k));
	k.limit = limit > TEXT_MAX ? TEXT_MAX : limit;
	for(i = 0; initial && initial[i] && i < k.limit; i++) k.text[i] = initial[i];
	k.len = i;
	k.text[k.len] = 0;
	for(i = 0; desc && desc[i] && i + 1 < sizeof(title); i++) title[i] = show_char(desc[i]);
	title[i] = 0;

	video_show_console();
	wait_release();

	while(result < 0){
		Input in;
		int act = -1;

		VIDEO_WaitVSync();
		input_read(&in);

		if(in.menu & IN_EXIT) result = 0;
		if(in.menu & IN_UP){
			if(k.row == ACTION_ROW){ k.row = KEY_ROWS - 1; k.col *= 2; }
			else k.row = k.row ? k.row - 1 : ACTION_ROW;
			if(k.row == ACTION_ROW) k.col /= 2;
			redraw = 1;
		}
		if(in.menu & IN_DOWN){
			if(k.row == KEY_ROWS - 1){ k.row = ACTION_ROW; k.col /= 2; }
			else if(k.row == ACTION_ROW){ k.row = 0; k.col *= 2; }
			else k.row++;
			redraw = 1;
		}
		if(in.menu & IN_LEFT){
			int cols = k.row == ACTION_ROW ? ACTION_KEYS : KEY_COLS;
			k.col = k.col ? k.col - 1 : cols - 1;
			redraw = 1;
		}
		if(in.menu & IN_RIGHT){
			int cols = k.row == ACTION_ROW ? ACTION_KEYS : KEY_COLS;
			k.col = k.col + 1 < cols ? k.col + 1 : 0;
			redraw = 1;
		}
		if(in.menu & IN_BACK){ delete_char(&k); redraw = 1; }
		if(in.menu & IN_SWITCH){ k.shift = !k.shift; redraw = 1; }
		if(in.menu & IN_OPTION) act = ACT_OK;
		if(in.menu & IN_ACCEPT){
			if(k.row == ACTION_ROW) act = k.col;
			else { put_char(&k, key_char(&k, k.row, k.col)); redraw = 1; }
		}

		switch(act){
		case ACT_SHIFT: k.shift = !k.shift; redraw = 1; break;
		case ACT_SPACE: put_char(&k, ' '); redraw = 1; break;
		case ACT_DELETE: delete_char(&k); redraw = 1; break;
		case ACT_OK: result = 1; break;
		case ACT_CANCEL: result = 0; break;
		default: break;
		}
		if(redraw && result < 0){ draw(&k, title); redraw = 0; }
	}

	if(result){
		memcpy(out, k.text, (k.len + 1) * sizeof(out[0]));
	}
	video_clear_console();
	wait_release();
	return result;
}
