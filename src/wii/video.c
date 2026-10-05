/**
 * WIISP - video.c
 * Vídeo del Wii: consola de texto y framebuffer de la PSP.
 *
 * El framebuffer de la PSP se convierte a mano al formato del XFB (YUY2:
 * Y1 Cb Y2 Cr por cada dos píxeles), 1:1 y centrado. Más adelante lo hará
 * GX con escalado. Arriba, en el margen negro, se dibuja un texto (FPS,
 * MIPS...) con una fuente mínima de 5x7.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <string.h>
#include "wii/wii.h"
#include "frontend/app.h"

static void *console_xfb;
static u32 *game_xfb;          /* con caché: se escribe entero y se vacía */
static GXRModeObj *rmode;
static int showing_game;

#define YUY2_BLACK 0x10801080u
#define YUY2_WHITE 0xEB80EB80u

void video_init(void){
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	console_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	game_xfb = (u32 *)SYS_AllocateFramebuffer(rmode);
	console_init(console_xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(console_xfb);
	VIDEO_SetBlack(0);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

void video_clear_console(void){
	printf("\x1b[2J\x1b[H");
}

void video_show_console(void){
	if(!showing_game) return;
	showing_game = 0;
	VIDEO_SetNextFramebuffer(console_xfb);
	VIDEO_Flush();
}

/* --- Fuente mínima 5x7 para el texto superpuesto --------------------------- */

static const struct { char c; u8 rows[7]; } font[] = {
	{ '0', { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E } }, { '1', { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E } },
	{ '2', { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F } }, { '3', { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E } },
	{ '4', { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 } }, { '5', { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E } },
	{ '6', { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E } }, { '7', { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 } },
	{ '8', { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E } }, { '9', { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C } },
	{ '.', { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C } }, { '%', { 0x18,0x19,0x02,0x04,0x08,0x13,0x03 } },
	{ 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } }, { 'P', { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 } },
	{ 'S', { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E } }, { 'M', { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 } },
	{ 'I', { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E } }, { 'V', { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 } },
	{ 'E', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F } }, { 'L', { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F } },
};

/* Cada píxel de la fuente ocupa 2x2 píxeles: una palabra YUY2 por 2 líneas */
static void draw_text(u32 x_words, u32 y, const char *text){
	u32 words_per_line = rmode->fbWidth / 2;
	for(; *text; text++, x_words += 6){
		unsigned i, row, col;
		const u8 *rows = NULL;
		for(i = 0; i < sizeof(font) / sizeof(font[0]); i++)
			if(font[i].c == *text) rows = font[i].rows;
		for(row = 0; row < 7; row++)
			for(col = 0; col < 5; col++){
				u32 color = rows && (rows[row] & (0x10 >> col)) ? YUY2_WHITE : YUY2_BLACK;
				u32 *p = game_xfb + (y + row * 2) * words_per_line + x_words + col;
				p[0] = color;
				p[words_per_line] = color;
			}
	}
}

/* --- Framebuffer de la PSP -------------------------------------------------- */

static inline void psp_pixel(const u8 *p, unsigned format, int *r, int *g, int *b){
	unsigned v = p[0] | (p[1] << 8);
	switch(format){
	case 0: *r = (v & 31) << 3; *g = ((v >> 5) & 63) << 2; *b = ((v >> 11) & 31) << 3; break;
	case 1: *r = (v & 31) << 3; *g = ((v >> 5) & 31) << 3; *b = ((v >> 10) & 31) << 3; break;
	case 2: *r = (v & 15) << 4; *g = ((v >> 4) & 15) << 4; *b = ((v >> 8) & 15) << 4; break;
	default: *r = p[0]; *g = p[1]; *b = p[2]; break;
	}
}

void video_draw_psp_frame(const char *overlay){
	unsigned stride, format, x, y, bpp;
	const u8 *fb = app_get_framebuffer(&stride, &format);
	u32 words_per_line = rmode->fbWidth / 2;
	u32 total_words = words_per_line * rmode->xfbHeight;
	u32 x0 = (rmode->fbWidth - APP_SCREEN_W) / 4;           /* en palabras */
	u32 y0 = (rmode->xfbHeight - APP_SCREEN_H) / 2;

	if(!fb) return;
	if(!showing_game){
		for(x = 0; x < total_words; x++) game_xfb[x] = YUY2_BLACK;
		showing_game = 1;
	}
	bpp = format == 3 ? 4 : 2;
	for(y = 0; y < APP_SCREEN_H; y++){
		const u8 *src = fb + y * stride * bpp;
		u32 *dst = game_xfb + (y0 + y) * words_per_line + x0;
		for(x = 0; x < APP_SCREEN_W; x += 2, src += 2 * bpp){
			int r1, g1, b1, r2, g2, b2, r, g, b;
			psp_pixel(src, format, &r1, &g1, &b1);
			psp_pixel(src + bpp, format, &r2, &g2, &b2);
			int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
			int y2 = ((66 * r2 + 129 * g2 + 25 * b2 + 128) >> 8) + 16;
			r = (r1 + r2) >> 1; g = (g1 + g2) >> 1; b = (b1 + b2) >> 1;
			int cb = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
			int cr = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
			*dst++ = ((u32)y1 << 24) | ((u32)cb << 16) | ((u32)y2 << 8) | (u32)cr;
		}
	}
	if(overlay && y0 >= 30) draw_text(x0, 12, overlay);
	DCFlushRange(game_xfb, total_words * 4);
	VIDEO_SetNextFramebuffer(game_xfb);
	VIDEO_Flush();
}
