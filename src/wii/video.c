/**
 * WIISP - video.c
 * Vídeo del Wii: consola de texto y framebuffer de la PSP.
 *
 * El framebuffer de la PSP lo compone GX (gx_ge.c) escalado a la pantalla:
 * en 16:9 la llena y en 4:3 ocupa el ancho con bandas negras. Si GX no
 * puede, se convierte a mano al formato del XFB (YUY2: Y1 Cb Y2 Cr por cada
 * dos píxeles), 1:1 y centrado. Arriba se dibuja un texto (FPS, MIPS...)
 * con una fuente mínima de 5x7.
 *
 * Hay tres XFB para el juego: se compone en el que lleva más tiempo sin
 * verse (el que se acaba de dejar de mostrar lo puede seguir leyendo el VI
 * hasta el siguiente retrazo). Con gx_paralelo la GPU todavía está copiando
 * la imagen al XFB cuando vuelve gx_ge_present: se muestra (con su texto)
 * al principio de la presentación siguiente, después de esperar a que esté,
 * y la emulación del cuadro de en medio va a la vez que la GPU termina.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "wii/wii.h"
#include "wii/gx_ge.h"
#include "frontend/app.h"

#define GX_FIFO_SIZE (512 * 1024)

static void *console_xfb;
#define N_XFB 3
static u32 *game_xfbs[N_XFB];  /* con caché */
static u32 *game_xfb;          /* en el que se compone */
static int xfb_disp = -1, xfb_prev = -1;   /* el que se ve y el que se veía antes */
static GXRModeObj *rmode;
static int showing_game;
static void *gx_fifo;
static int xfb_pend = -1;       /* imagen que la GPU puede estar copiando (gx_paralelo) */
static char pend_overlay[64];

#define YUY2_BLACK 0x10801080u
#define YUY2_WHITE 0xEB80EB80u

void video_init(void){
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	console_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	{
		int i;
		for(i = 0; i < N_XFB; i++) game_xfbs[i] = (u32 *)SYS_AllocateFramebuffer(rmode);
	}
	game_xfb = game_xfbs[0];
	console_init(console_xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(console_xfb);
	VIDEO_SetBlack(0);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

	/* GX: dibuja el GE y compone la imagen del juego */
	gx_fifo = memalign(32, GX_FIFO_SIZE);
	memset(gx_fifo, 0, GX_FIFO_SIZE);
	GX_Init(gx_fifo, GX_FIFO_SIZE);
	GX_SetCopyClear((GXColor){ 0, 0, 0, 0xFF }, GX_MAX_Z24);
	GX_SetDispCopyYScale(GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight));
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
	GX_SetFieldMode(rmode->field_rendering, rmode->viHeight == 2 * rmode->xfbHeight ? GX_ENABLE : GX_DISABLE);
	GX_SetDispCopyGamma(GX_GM_1_0);
	gx_ge_init();
}

void video_clear_console(void){
	printf("\x1b[2J\x1b[H");
}

void video_show_console(void){
	xfb_pend = -1;   /* la imagen que faltaba ya no se muestra */
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
	{ 'A', { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 } }, { 'R', { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 } },
	{ 'T', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 } }, { 'U', { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E } },
	{ 'B', { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E } }, { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } },
	{ 'N', { 0x11,0x11,0x19,0x15,0x13,0x11,0x11 } },
};

/* Cada píxel de la fuente ocupa 2x2 píxeles: una palabra YUY2 por 2 líneas.
   Sobre un recuadro negro con margen, para leerse sobre fondos claros */
#define TEXT_PAD_X 2   /* palabras */
#define TEXT_PAD_Y 3   /* líneas */
#define TEXT_LINES (14 + 2 * TEXT_PAD_Y)

static void draw_text(u32 x_words, u32 y, const char *text){
	u32 words_per_line = rmode->fbWidth / 2, len = (u32)strlen(text), bx, by;
	u32 x_end = x_words + len * 6 + TEXT_PAD_X - 1;
	if(x_end > words_per_line) x_end = words_per_line;
	/* La GPU pudo escribir esas líneas: fuera lo que haya en la caché */
	DCInvalidateRange(game_xfb + (y - TEXT_PAD_Y) * words_per_line, TEXT_LINES * words_per_line * 4);
	for(by = y - TEXT_PAD_Y; by < y + 14 + TEXT_PAD_Y; by++)
		for(bx = x_words - TEXT_PAD_X; bx < x_end; bx++) game_xfb[by * words_per_line + bx] = YUY2_BLACK;
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

static void show_xfb(int i){
	VIDEO_SetNextFramebuffer(game_xfbs[i]);
	VIDEO_Flush();
	showing_game = 1;
	xfb_prev = xfb_disp;
	xfb_disp = i;
}

/* Dónde componer: ni el que se ve ni el que espera a la GPU, ni (si se
   puede) el que se acaba de dejar de ver */
static int pick_xfb(void){
	int i;
	for(i = 0; i < N_XFB; i++) if(i != xfb_disp && i != xfb_pend && i != xfb_prev) return i;
	for(i = 0; i < N_XFB; i++) if(i != xfb_disp && i != xfb_pend) return i;
	return 0;
}

/* gx_paralelo: la imagen de la presentación anterior, cuando la GPU acabe
   de copiarla, con su texto encima */
static void show_pending(void){
	u32 words_per_line = rmode->fbWidth / 2;
	int i = xfb_pend;
	if(i < 0) return;
	xfb_pend = -1;
	gx_ge_present_wait();
	game_xfb = game_xfbs[i];
	if(pend_overlay[0]){
		draw_text(16, 24, pend_overlay);
		DCFlushRange(game_xfb + (24 - TEXT_PAD_Y) * words_per_line, TEXT_LINES * words_per_line * 4);
	}
	show_xfb(i);
}

void video_draw_psp_frame(const char *overlay){
	unsigned stride, format, x, y, bpp;
	const u8 *fb;
	u32 words_per_line = rmode->fbWidth / 2;
	u32 total_words = words_per_line * rmode->xfbHeight;
	u32 x0 = (rmode->fbWidth - APP_SCREEN_W) / 4;           /* en palabras */
	u32 y0 = (rmode->xfbHeight - APP_SCREEN_H) / 2;
	int cur;

	show_pending();
	cur = pick_xfb();
	game_xfb = game_xfbs[cur];
	if(gx_ge_present(game_xfb, rmode, CONF_GetAspectRatio() == CONF_ASPECT_16_9)){
		if(gx_ge_present_pendiente()){
			xfb_pend = cur;
			snprintf(pend_overlay, sizeof(pend_overlay), "%s", overlay ? overlay : "");
			return;
		}
		if(overlay && overlay[0]){
			/* Dentro de la zona segura de la tele (sin overscan) */
			draw_text(16, 24, overlay);
			DCFlushRange(game_xfb + (24 - TEXT_PAD_Y) * words_per_line, TEXT_LINES * words_per_line * 4);
		}
		show_xfb(cur);
		return;
	}

	fb = app_get_framebuffer(&stride, &format);
	if(!fb) return;
	/* Bandas negras (la imagen va 1:1 en el centro) */
	for(y = 0; y < rmode->xfbHeight; y++)
		if(y < y0 || y >= y0 + APP_SCREEN_H)
			for(x = 0; x < words_per_line; x++) game_xfb[y * words_per_line + x] = YUY2_BLACK;
		else
			for(x = 0; x < words_per_line; x++)
				if(x < x0 || x >= x0 + APP_SCREEN_W / 2) game_xfb[y * words_per_line + x] = YUY2_BLACK;
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
	DCFlushRange(game_xfb, total_words * 4);
	if(overlay && y0 >= 24 + 14 + TEXT_PAD_Y){
		draw_text(x0 + TEXT_PAD_X, 24, overlay);
		DCFlushRange(game_xfb + (24 - TEXT_PAD_Y) * words_per_line, TEXT_LINES * words_per_line * 4);
	}
	show_xfb(cur);
}
