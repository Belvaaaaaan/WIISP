/**
 * WIISP - app.h
 * Lógica de la aplicación compartida entre plataformas (PC y Wii).
 *
 * Este header solo usa tipos de C estándar a propósito: el frontend del Wii
 * incluye gccore.h, cuyos typedef (u8, u32...) chocarían con core/types.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_APP_H
#define WIISP_APP_H

#include <stddef.h>

/* Implementadas por cada plataforma */
/* Memoria grande y alineada a 32 bytes para la RAM/VRAM de la PSP.
   En el Wii sale de MEM2. Devuelve NULL si no hay. */
void *plat_alloc_big(size_t size);

/* Reserva la memoria de la PSP (32 MB de RAM). 0 = correcto. */
int  app_init(void);

/* 0 = no imprimir el resumen del módulo (para comparar salidas de tests) */
void app_set_verbose(int verbose);

/* Carga un EBOOT.PBP / ELF / PRX, imprime su resumen y lo deja en la
   memoria emulada. max_imports < 0 = listar todos.
   imports_out: si no es NULL, ahí se escribe el informe de imports.
   0 = correcto. */
int  app_load(const char *path, int max_imports, const char *imports_out);

/* 1 si la ruta es una imagen de UMD (.iso, .cso, .zso) */
int  app_is_disc_image(const char *path);

/* Ruta recomendada para el informe de imports de path: "imports.txt" en su
   carpeta si es un EBOOT.PBP, si no "<nombre>.imports.txt" */
void app_imports_path(const char *path, char *out, size_t out_size);

/* Prepara la ejecución del programa cargado. 0 = correcto. */
int  app_start(void);

/* Ejecuta un frame (hasta el siguiente vblank). Devuelve 1 si el programa
   terminó. */
int  app_run_frame(void);

/* Estadísticas acumuladas desde app_start: frames emulados e instrucciones
   de la PSP ejecutadas (para calcular FPS y MIPS) */
void app_get_stats(unsigned *frames, unsigned long long *instructions);
const char *app_exit_reason(void);

/* Salida de texto del programa emulado */
typedef void (*AppOutputFunc)(const char *text, unsigned len);
void app_set_output(AppOutputFunc func);

/* Perfilado: el GE hace toda la geometría pero no dibuja nada */
void app_set_null_renderer(void);
/* Geometría con float normal, como con el backend GX */
void app_set_fast_math(int on);

/* Perfilado: reloj del anfitrión y tiempo acumulado dentro del GE */
void app_set_host_clock(unsigned long long (*clock)(void));
unsigned long long app_ge_host_ticks(void);

/* Mandos en formato PSP (ver APP_BTN_*) y stick analógico (0-255) */
void app_set_input(unsigned buttons, unsigned char lx, unsigned char ly);

#define APP_BTN_SELECT   0x000001u
#define APP_BTN_START    0x000008u
#define APP_BTN_UP       0x000010u
#define APP_BTN_RIGHT    0x000020u
#define APP_BTN_DOWN     0x000040u
#define APP_BTN_LEFT     0x000080u
#define APP_BTN_LTRIGGER 0x000100u
#define APP_BTN_RTRIGGER 0x000200u
#define APP_BTN_TRIANGLE 0x001000u
#define APP_BTN_CIRCLE   0x002000u
#define APP_BTN_CROSS    0x004000u
#define APP_BTN_SQUARE   0x008000u
#define APP_BTN_HOME     0x010000u

/* Framebuffer actual de la PSP, en su memoria (little-endian).
   format: 0 = RGB565, 1 = RGBA5551, 2 = RGBA4444, 3 = RGBA8888.
   Devuelve NULL si el programa no ha configurado ninguno. */
const unsigned char *app_get_framebuffer(unsigned *stride, unsigned *format);

/* Si se indica, cuando el programa pida una captura (pspautotests) se
   escribe un BMP de 512x272 idéntico al que genera una PSP real */
void app_set_screenshot_path(const char *path);
/* Escribe ahora el framebuffer mostrado como BMP. 0 = correcto. */
int  app_write_bmp(const char *path);

#define APP_SCREEN_W 480
#define APP_SCREEN_H 272

#endif
