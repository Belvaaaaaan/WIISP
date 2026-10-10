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

/* Versión de WIISP (la misma que dist/apps/wiisp/meta.xml): sale en el
   menú y al principio de wiisp.log */
#define WIISP_VERSION "0.5.6"

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
#define HLE_SYSCALL_THREAD_RETURN   0xFFF00u  /* un hilo volvió de su función */
#define HLE_SYSCALL_CALLBACK_RETURN 0xFFF01u  /* volvió una llamada de kernel_call_guest */
#define HLE_SYSCALL_THREAD_CB_RETURN 0xFFF02u /* volvió un callback de un hilo */
#define HLE_SYSCALL_GUEST_CALL_RETURN 0xFFF03u /* volvió una llamada de kernel_enqueue_call */

/* Direcciones en la zona de kernel (no la usa el juego) */
#define HLE_KERNEL_TRAMPOLINE  0x08000000u  /* syscall THREAD_RETURN; nop */
#define HLE_CALLBACK_TRAMPOLINE 0x08000008u /* syscall CALLBACK_RETURN; nop */
#define HLE_THREAD_CB_TRAMPOLINE 0x08000020u /* syscall THREAD_CB_RETURN; nop */
#define HLE_GUEST_CALL_TRAMPOLINE 0x08000028u /* syscall GUEST_CALL_RETURN; nop */
#define HLE_INTERRUPT_STACK_TOP 0x08010000u /* pila para las llamadas tipo interrupción */

/* Errores del kernel de la PSP */
#define SCE_KERNEL_ERROR_ERROR               0x80020001u
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR        0x800200D3u
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY    0x80020193u
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR        0x80020191u
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
/* Otro módulo cargado (sceKernelLoadModule): sus imports al HLE y el enlace
   de exportaciones entre módulos. Cargarlo con loader_syscall_base =
   hle_next_syscall(). */
int  hle_link_module(const PspModule *m);
void hle_unlink_module(const PspModule *m);
u32  hle_next_syscall(void);
void hle_shutdown(void);

/* Ejecuta hasta el siguiente vblank. Devuelve 0 si el programa sigue
   vivo, 1 si terminó. */
int  hle_run_frame(void);

/* Salida de texto del programa (stdout, Kprintf, devctl de emulador) */
void hle_set_output(HleOutputFunc func);
void hle_output(const char *text, u32 len);

/* Mensajes del emulador (no del juego) */
void hle_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Solo a wiisp.log (lo que pasa a menudo durante el juego, como [MANDO]) */
void hle_log_quiet(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Además de stderr, el registro va a este archivo (NULL = ninguno). Se
   vacía en cada línea para que sobreviva a un cuelgue. */
void hle_set_log_file(const char *path);
/* Asegura que lo escrito llega a la SD (también lo hace solo cada segundo) */
void hle_log_sync(void);

/* Diagnóstico (wiisp.log): hilos y sus esperas; últimas llamadas al HLE */
void kernel_dump_state(void);
/* Callbacks: notificar uno (UMD, energía...) y las esperas CB de otros
   módulos (fn es la espera normal) */
int  kernel_is_callback(u32 uid);
int  kernel_notify_callback(u32 uid, s32 arg);
void kernel_cb_wait(void (*fn)(void));
void kernel_note_hle_call(const char *name);
int  kernel_current_thread(void);
void hle_dump_state(const char *why);

/* Estadísticas de rendimiento (instrucciones, VFPU, cambios de hilo) al
   registro: cada minuto de juego emulado y al terminar. En PC solo con la
   variable de entorno WIISP_STATS. */
void hle_log_stats(void);
extern u64 kernel_stat_switches, kernel_stat_vfpu_loads, kernel_stat_guest_calls;

/* Desglose del tiempo real ([TIEMPOS] en wiisp.log): cada 30 s reales
   desde hle_run_frame y al parar. Hace falta un reloj (prof_set_clock en
   core/prof.h). El frontend puede añadir sus datos (renderizador, GX). */
#ifndef HLE_PROFILE_SECONDS
#define HLE_PROFILE_SECONDS 30
#endif
extern u64 hle_stat_syscalls, hle_stat_flips;
void hle_set_profile_hook(void (*fn)(char *buf, size_t size));
void hle_profile_report(const char *why);

/* Estado de salida */
int  hle_has_exited(void);
const char *hle_exit_reason(void);
void hle_exit(const char *reason);

/* Captura pedida por el programa (devctl 0x20 de "emulator:", pspautotests) */
typedef void (*HleScreenshotFunc)(void);
void hle_set_screenshot(HleScreenshotFunc func);
void hle_screenshot(void);

/* Mandos: botones en el formato de la PSP (PSP_CTRL_*). Un botón pulsado
   entre dos lecturas del juego le llega en la siguiente aunque ya se haya
   soltado. */
void hle_set_input(u32 buttons, u8 lx, u8 ly);

/* Empieza otro programa: vuelve a haber sitio en wiisp.log (2 MB cada uno) */
void hle_log_new_program(void);

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
extern const HleLibrary hle_ge_libs[];
extern const u32 hle_ge_libs_count;
extern const HleLibrary hle_sas_libs[];
extern const u32 hle_sas_libs_count;
void sas_init(void);
extern const HleLibrary hle_atrac_libs[];
extern const u32 hle_atrac_libs_count;
void atrac_init(void);
void atrac_shutdown(void);
/* sceUtilityLoadModule del módulo de Atrac: sus contextos viven en su BSS */
void atrac_notify_load(u32 bss);
extern const HleLibrary hle_utility_libs[];
extern const u32 hle_utility_libs_count;
void utility_init(void);
extern const HleLibrary hle_mpeg_libs[];
extern const u32 hle_mpeg_libs_count;
void mpeg_init(void);
extern const HleLibrary hle_audio_libs[];
extern const u32 hle_audio_libs_count;
void audio_init(void);
/* Salida de sonido de la plataforma: cada bloque mezclado del sceAudio
   (estéreo s16, 44100 Hz). NULL = sin sonido. */
typedef void (*HleAudioSink)(const s16 *stereo, u32 frames);
extern HleAudioSink hle_audio_sink;
extern const HleLibrary hle_net_libs[];
extern const u32 hle_net_libs_count;
extern const HleLibrary hle_module_libs[];
extern const u32 hle_module_libs_count;
void module_shutdown(void);

/* Hilos y planificador (kernel.c) */
void kernel_init(const PspModule *mod, const char *exec_path);
void kernel_shutdown(void);
void kernel_run_until(u64 target_cycles);
void kernel_vblank(void);
void kernel_thread_return(void);

/* Ejecuta una función del juego como lo haría una interrupción (pila
   propia, sin poder bloquearse) y vuelve cuando termina. Devuelve su v0.
   Se puede llamar desde dentro de un syscall. */
u32  kernel_call_guest(u32 func, u32 a0, u32 a1, u32 a2);
/* Igual, pero con la pila sp (0 = la de interrupciones). Para callbacks que
   la PSP ejecuta en el hilo que llama, como el del ringbuffer de sceMpeg. */
u32  kernel_call_guest_sp(u32 func, u32 sp, u32 a0, u32 a1, u32 a2);
u32  kernel_module_gp(void);   /* gp del módulo principal */
void kernel_callback_return(void);
void kernel_thread_cb_return(void);   /* trampolín de los callbacks de hilo */

/* Llama a una función del juego como código normal del hilo actual, justo
   al volver del syscall que la pide (PPSSPP hleEnqueueCall). Puede esperar
   (semáforos, event flags...) y mientras tanto corren los demás hilos; en
   cambio kernel_call_guest la ejecuta en el acto y sin cambios de hilo.
   Al volver la función, done(lo que devolvió, data) da el valor final del
   syscall, o encadena otra llamada con kernel_enqueue_call (entonces lo
   que devuelva no cuenta). data: KERNEL_CALL_DATA palabras para done.
   Devuelve 0, o -1 si no se puede (en una interrupción, sin hilo o
   demasiadas anidadas): entonces hay que usar kernel_call_guest. */
#define KERNEL_CALL_DATA 6
typedef u32 (*KernelCallDone)(u32 ret, u32 *data);
int  kernel_enqueue_call(u32 func, u32 a0, u32 a1, u32 a2, KernelCallDone done, const u32 *data);
void kernel_guest_call_return(void);  /* trampolín de kernel_enqueue_call */
u32  kernel_enqueue_count(void);      /* llamadas encoladas desde el inicio */
/* 1 mientras se atiende una interrupción o se ejecuta kernel_call_guest */
int  kernel_in_interrupt(void);
int  kernel_wait_vblank(void); /* bloquea el hilo actual hasta el vblank */
int  kernel_wait_vblank_ret(u32 ret); /* igual, y el syscall devuelve ret al despertar */
/* Bloquea el hilo actual hasta el ciclo indicado (si no es una interrupción) */
void kernel_wait_until(u64 cycle);
/* El syscall "tarda" us microsegundos: el hilo espera (hleDelayResult) */
#define hle_delay_us(us) kernel_wait_until(cpu_cycles + (u64)(us) * CYCLES_PER_US)
/* El Media Engine hace un trabajo a la vez: devuelve cuánto esperará quien
   le encargue uno de us microsegundos (atrac.c) */
u32  me_schedule_job(u32 us);

/* Eventos programados, como CoreTiming de PPSSPP (kernel.c): fn(userdata)
   se llama cuando cpu_cycles llega a `when`. unschedule devuelve los
   ciclos que le faltaban al primero que quita (0 si no había). */
typedef void (*KernelEventFunc)(u64 userdata);
void kernel_schedule_event(u64 when, KernelEventFunc fn, u64 userdata);
s64  kernel_unschedule_event(KernelEventFunc fn, u64 userdata);
/* El tiempo avanza sin ejecutar instrucciones (coste de una llamada) */
void kernel_eat_cycles(u32 n);
/* Pide revisar qué hilo debe correr al volver del syscall */
void kernel_reschedule(void);

/* Interrupciones (kernel.c) */
#define PSP_GE_INTR      25
#define PSP_VBLANK_INTR  30
#define PSP_NUM_INTR     67
#define INTR_SUB_NONE   (-1)  /* una sola entrada, sin subinterrupción */
#define INTR_SUB_ALL    (-2)  /* todas las subinterrupciones habilitadas */

typedef struct {
	/* Prepara la interrupción. Devuelve 1 si hay que llamar a *func con
	   los argumentos a[0..2] (y luego a result), 0 si ya está atendida. */
	int  (*run)(int sub, u32 *func, u32 a[3]);
	void (*result)(int sub);
} KernelIntrHandler;

void kernel_register_intr(int intno, const KernelIntrHandler *h);
int  kernel_trigger_interrupt(int intno, int sub);
int  kernel_cancel_raised_interrupts(int intno);
int  kernel_get_subintr(int intno, int sub, u32 *handler, u32 *arg);
int  kernel_register_subintr(int intno, int sub, u32 handler, u32 arg);
int  kernel_release_subintr(int intno, int sub);
int  kernel_enable_subintr(int intno, int sub, int enable);
int  kernel_interrupts_enabled(void);
int  kernel_dispatch_enabled(void);   /* dispatch y además interrupciones */
u32  kernel_sdk_version(void);

/* El hilo actual espera a un objeto del HLE; kernel_wake_object despierta
   a todos los que esperan ese (tipo, id) por orden de llegada. */
enum { KWAIT_GE_DRAW = 1, KWAIT_GE_LIST = 2, KWAIT_AUDIO = 3, KWAIT_SAVEDATA = 4, KWAIT_UMD = 5 };
void kernel_wait_object(int type, u32 id);
/* Igual, con un límite en microsegundos (despierta con WAIT_TIMEOUT) */
void kernel_wait_object_timeout(int type, u32 id, u32 us);
/* Despierta a los que esperan un id con algún bit de mask */
int  kernel_wake_object_mask(int type, u32 mask, u32 ret);
/* Tras ejecutar callbacks, una espera del HLE se vuelve a comprobar con fn:
   si pone *done, el hilo despierta con lo que devuelve */
void kernel_set_wait_recheck(int type, u32 (*fn)(u32 id, int *done));
/* Hilos creados desde el HLE (module_start): como sceKernelCreateThread /
   StartThread. kernel_wait_module_start pone al hilo actual a esperar el
   fin del hilo thread; entonces devuelve ret y escribe su estado de salida
   en status_addr. */
u32  kernel_create_thread(const char *name, u32 entry, u32 prio, u32 stack_size, u32 attr, u32 gp);
int  kernel_start_thread(u32 uid, u32 arglen, u32 argp);
void kernel_wait_module_start(u32 thread, u32 ret, u32 status_addr);
int  kernel_wake_object(int type, u32 id, u32 ret);

/* Asignador de memoria de usuario (kernel.c) */
u32  kernel_alloc(u32 size, int from_high, const char *name);
void kernel_free(u32 addr);

/* E/S (io.c) */
/* boot_from_disc: el directorio actual empieza en disc0:/PSP_GAME/USRDIR */
void io_init(const char *host_dir, int boot_from_disc);
void io_shutdown(void);
/* Un archivo entero por su ruta de la PSP, o el resto de un descriptor
   abierto desde su posición actual (malloc; NULL si falla) */
u8  *io_load_path(const char *path, u32 *len);
/* Ruta del anfitrión de una ruta de la PSP en ms0:/host0: (-1 si no) */
int  io_host_path(const char *psp_path, char *out, u32 size);
/* Carpeta para ms0:/PSP/SAVEDATA (NULL o "" = la de ms0:/). Se conserva
   entre juegos. */
void io_set_savedata_dir(const char *dir);
u8  *io_load_fd(u32 fd, u32 *len);

/* Escribe una ScePspDateTime (16 bytes) a partir de microsegundos Unix (misc.c) */
void hle_write_datetime(u32 addr, u64 unix_us);

/* Display y mandos (display.c) */
void display_init(void);
void power_init(void);   /* net.c: ranuras de callbacks de energía */
void display_fill_ctrl(u32 addr);
/* Tras una lectura del mando del juego (olvida lo retenido, [MANDO]) */
void display_ctrl_read_done(void);
/* Cada vblank (muestra nueva del mando) */
void display_vblank(void);

#endif
