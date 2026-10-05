/**
 * WIISP - hle.h
 * Emulación de alto nivel del sistema operativo de la PSP.
 *
 * Cada función del firmware se registra por biblioteca y NOMBRE; el NID sale
 * de la tabla del PSPSDK (nid_names.c), así no hay NIDs escritos a mano.
 * Las funciones leen sus argumentos de los registros de la CPU emulada
 * (convención de la PSP: a0-a3 y luego t0-t3) y devuelven en v0 (y v1).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_HLE_H
#define WIISP_HLE_H

#include "core/types.h"
#include "cpu/cpu.h"
#include "loader/loader.h"

typedef void (*HleFunc)(void);

typedef struct {
	const char *name;
	HleFunc func;
} HleFunction;

typedef struct {
	const char *lib;
	const HleFunction *funcs;
	u32 count;
} HleLibrary;

#define HLE_LIBRARY(lib, table) { lib, table, sizeof(table) / sizeof(table[0]) }

/* Argumentos y retorno */
static inline u32 hle_arg(int n){ return n < 4 ? cpu.r[R_A0 + n] : cpu.r[R_T0 + n - 4]; }
#define ARG(n)        hle_arg(n)
#define RETURN(v)     (cpu.r[R_V0] = (u32)(v))
#define RETURN64(v)   (cpu.r[R_V0] = (u32)(u64)(v), cpu.r[R_V1] = (u32)((u64)(v) >> 32))

/* Códigos de syscall especiales (por encima de los imports) */
#define HLE_SYSCALL_THREAD_RETURN 0xFFF00u  /* un hilo volvió de su función */

/* Direcciones en la zona de kernel (no la usa el juego) */
#define HLE_KERNEL_TRAMPOLINE  0x08000000u  /* syscall THREAD_RETURN; nop */

/* Errores del kernel de la PSP */
#define SCE_KERNEL_ERROR_ERROR               0x80020001u
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR        0x800200D3u
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY    0x80020193u
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR        0x8002013Au
#define SCE_KERNEL_ERROR_ILLEGAL_THID        0x800201A0u
#define SCE_KERNEL_ERROR_UNKNOWN_THID        0x80020198u
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID       0x80020199u
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID       0x8002019Au
#define SCE_KERNEL_ERROR_UNKNOWN_UID         0x800200CBu
#define SCE_KERNEL_ERROR_DORMANT             0x800201A2u
#define SCE_KERNEL_ERROR_NOT_DORMANT         0x800201A4u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT        0x800201A8u
#define SCE_KERNEL_ERROR_WAIT_DELETE         0x800201B5u
#define SCE_KERNEL_ERROR_SEMA_ZERO           0x800201ADu
#define SCE_KERNEL_ERROR_SEMA_OVF            0x800201AEu
#define SCE_KERNEL_ERROR_EVF_COND            0x800201AFu
#define SCE_KERNEL_ERROR_EVF_MULTI           0x800201B0u
#define SCE_KERNEL_ERROR_EVF_ILPAT           0x800201B1u
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT       0x800201BDu
#define SCE_KERNEL_ERROR_ILLEGAL_MODE        0x80020195u
#define SCE_KERNEL_ERROR_NO_MEMORY           0x80020190u
#define SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE     0x800200D2u
#define SCE_KERNEL_ERROR_ILLEGAL_PARTITION   0x800200D6u
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK    0x800200D5u
#define SCE_ERROR_FILE_NOT_FOUND             0x80010002u
#define SCE_ERROR_BAD_FILE                   0x80010009u

/* Tiempo: el Allegrex corre a 333 MHz; 1 instrucción = 1 ciclo de momento */
#define PSP_CPU_HZ          333000000ull
#define CYCLES_PER_US       333ull
#define CYCLES_PER_FRAME    (PSP_CPU_HZ * 1001ull / 60000ull)  /* 59,94 Hz */

static inline u64 hle_now_us(void){ return cpu_cycles / CYCLES_PER_US; }

/* Fecha base fija (2010-01-01 en tiempo Unix) para que las ejecuciones
   sean reproducibles */
#define HLE_EPOCH_BASE 1262304000u

/* --- API para el frontend ------------------------------------------- */

typedef void (*HleOutputFunc)(const char *text, u32 len);

/* Prepara el HLE para un módulo ya cargado: resuelve sus imports y crea
   el hilo principal. host_dir: carpeta del host que se ve como ms0:/ y
   host0:/. Devuelve 0 si todo va bien. */
int  hle_init(PspModule *mod, const char *host_dir, const char *exec_name);
void hle_shutdown(void);

/* Ejecuta hasta el siguiente vblank. Devuelve 0 si el programa sigue
   vivo, 1 si terminó. */
int  hle_run_frame(void);

/* Salida de texto del programa (stdout, Kprintf, devctl de emulador) */
void hle_set_output(HleOutputFunc func);
void hle_output(const char *text, u32 len);

/* Mensajes del emulador (no del juego) */
void hle_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Estado de salida */
int  hle_has_exited(void);
const char *hle_exit_reason(void);
void hle_exit(const char *reason);

/* Mandos: botones en el formato de la PSP (PSP_CTRL_*) */
void hle_set_input(u32 buttons, u8 lx, u8 ly);

/* Framebuffer que la PSP muestra ahora */
typedef struct {
	u32 addr;      /* 0 si no hay */
	u32 stride;    /* en píxeles */
	u32 format;    /* 0 = 565, 1 = 5551, 2 = 4444, 3 = 8888 */
} HleFramebuffer;
void hle_get_framebuffer(HleFramebuffer *fb);

/* Escribe el informe de imports (resumen por biblioteca y lista completa
   con nombre y si está implementada). Devuelve 0 si se pudo escribir. */
int  hle_write_imports_report(const PspModule *mod, const char *exec_path, const char *out_path);

/* --- Interno del HLE ------------------------------------------------- */

/* Busca la función HLE que implementa lib/nid, o NULL */
HleFunc hle_find(const char *lib, u32 nid, const char **name_out);
const char *nid_lookup(u32 nid);

/* Bibliotecas registradas (cada archivo de hle/ aporta las suyas) */
extern const HleLibrary hle_kernel_libs[];
extern const u32 hle_kernel_libs_count;
extern const HleLibrary hle_io_libs[];
extern const u32 hle_io_libs_count;
extern const HleLibrary hle_display_libs[];
extern const u32 hle_display_libs_count;
extern const HleLibrary hle_misc_libs[];
extern const u32 hle_misc_libs_count;

/* Hilos y planificador (kernel.c) */
void kernel_init(const PspModule *mod, const char *exec_path);
void kernel_shutdown(void);
void kernel_run_until(u64 target_cycles);
void kernel_vblank(void);
void kernel_thread_return(void);
int  kernel_wait_vblank(void); /* bloquea el hilo actual hasta el vblank */

/* Asignador de memoria de usuario (kernel.c) */
u32  kernel_alloc(u32 size, int from_high, const char *name);
void kernel_free(u32 addr);

/* E/S (io.c) */
void io_init(const char *host_dir);
void io_shutdown(void);

/* Escribe una ScePspDateTime (16 bytes) a partir de microsegundos Unix (misc.c) */
void hle_write_datetime(u32 addr, u64 unix_us);

/* Display y mandos (display.c) */
void display_init(void);
void display_fill_ctrl(u32 addr);

#endif
