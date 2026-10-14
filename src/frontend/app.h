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

/* Teclado en pantalla de la PSP (sceUtilityOsk). La función del frontend
   bloquea (modal) hasta que el usuario acepta o cancela.
     desc: título que pone el juego; initial: texto de partida (ambos UTF-16
     terminados en 0, pueden estar vacíos); out: aquí va el texto final, con
     sitio para limit + 1 palabras; limit: máximo de caracteres; lines: 1 =
     una línea; inputtype: tipos de carácter que pide el juego (0 = todos).
   Devuelve 1 si el usuario aceptó (out relleno) y 0 si canceló. Sin función
   (NULL), el diálogo devuelve el texto inicial sin cambios. */
typedef int (*AppOskFunc)(const unsigned short *desc, const unsigned short *initial,
                          unsigned short *out, unsigned limit, unsigned lines,
                          unsigned inputtype);
void app_set_osk(AppOskFunc func);

/* Perfilado: el GE hace toda la geometría pero no dibuja nada */
void app_set_null_renderer(void);
/* Turbo: el GE no dibuja (ver ge_turbo en gpu/ge.h) */
void app_set_turbo(int on);
int  app_turbo(void);
/* Geometría con float normal, como con el backend GX */
void app_set_fast_math(int on);
int  app_fast_math(void);
/* La carpeta que hace de ms0:/ si el ejecutable está dentro de ella (si
   no, es la del ejecutable) */
void app_set_root(const char *dir);

/* Perfilado: reloj del anfitrión y tiempo acumulado dentro del GE */
void app_set_host_clock(unsigned long long (*clock)(void));
unsigned long long app_ge_host_ticks(void);

/* --- Escenas de prueba repetibles ----------------------------------------
   Un guion (frontend/guion.h) dice qué botones hay pulsados en cada cuadro
   emulado. Se activa después de app_load y antes de app_start, y vale para
   esa ejecución:
     - reproducir: el mando real se ignora; las capturas van a
       <prefijo>.c<cuadro>.bmp y las huellas a <prefijo>.huellas. Al llegar a
       "fin", app_run_frame devuelve 1 ("fin del guion").
     - grabar: lo que llega por app_set_input se escribe en el guion (con el
       stick redondeado) y las huellas van a <guion>.huellas.
   Mientras hay guion, el teclado en pantalla devuelve el texto inicial: lo
   que escriba alguien no quedaría grabado. */
int  app_guion_reproducir(const char *guion, const char *prefijo, char *err, size_t errlen);
int  app_guion_grabar(const char *guion);
/* Termina el guion (al grabar, escribe la línea "fin") */
void app_guion_parar(void);
/* 0 = sin guion, 1 = reproduciendo, 2 = grabando */
int  app_guion_activo(void);
/* Cuadro de la línea "fin" del guion que se reproduce (0 si no hay) */
unsigned app_guion_fin(void);
/* Se llama antes de cada captura del guion (en el Wii, bajar a la VRAM
   emulada lo que solo está en la GPU) */
void app_set_before_capture(void (*fn)(void));
/* Con guion (grabando o reproduciendo) o con app_capturas_sin_guion, una
   captura cada N cuadros además de las que pida el guion (por defecto 400;
   0 = ninguna). Cada captura se
   apunta en <prefijo>.capturas.txt con las opciones con que corría el
   juego. Ojo: guardar un BMP en la SD tarda, así que para medir velocidad
   conviene ponerlo a 0. */
void app_guion_capturas_cada(unsigned cuadros);
/* Las mismas capturas periódicas jugando sin guion: <prefijo>.c<cuadro>.bmp
   y <prefijo>.capturas.txt. Se llama después de app_load y antes de
   app_start; app_guion_parar las termina. */
void app_capturas_sin_guion(const char *prefijo);
/* El frontend describe sus opciones (renderizador, MR/ME, TN/TS...) para
   las capturas y las huellas */
void app_set_mode_text(void (*fn)(char *buf, size_t size));

/* Partidas: carpeta que hace de ms0:/PSP/SAVEDATA */
void app_set_savedata_dir(const char *dir);
/* Partidas de prueba: vacía dir (debe terminar en ".partidas"), copia
   dentro lo que haya en base y la usa como SAVEDATA. 0 = correcto. */
int  app_preparar_partidas(const char *dir, const char *base);

/* Interruptores de optimización (core/opt.h): "nombre=0|1" o "nombre 0|1".
   0 = cambiado, -1 = nombre o valor desconocido, 1 = todavía no existe. */
int  app_opt_set(const char *texto);
/* Los interruptores implementados y su valor, en una línea */
void app_opt_describe(char *buf, size_t size);

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
