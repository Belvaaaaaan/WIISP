/**
 * WIISP - core/prof.h
 * Desglose del tiempo real: a qué dedica el emulador cada segundo de verdad.
 *
 * El tiempo se reparte en cubetas exclusivas que suman el 100 %: en cada
 * momento solo cuenta una, la de lo que se esté haciendo (ejecutar
 * instrucciones, un syscall, procesar una lista del GE...). Al entrar en
 * algo se cambia de cubeta y al salir se vuelve a la anterior:
 *
 *     int old = prof_switch(PROF_GE);
 *     ...
 *     prof_switch(old);
 *
 * Solo se mide en puntos gruesos (syscalls, listas del GE, texturas,
 * framebuffers, lecturas...), nunca por instrucción ni por vértice, así que
 * puede ir siempre activo. hle.c escribe el resumen en wiisp.log cada 30 s
 * reales ([TIEMPOS]).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_PROF_H
#define WIISP_PROF_H

#include "core/types.h"

enum {
	PROF_OTRO,      /* lo que no está en otra cubeta (bucle principal, mando...) */
	PROF_CPU,       /* el intérprete ejecutando instrucciones de la PSP */
	PROF_HLE,       /* dentro de syscalls (sin el GE, la E/S ni el audio) */
	PROF_GE,        /* listas del GE: comandos, vértices y envío a GX */
	PROF_RASTER,    /* rasterizado por software (modo exacto) */
	PROF_TEXTURAS,  /* buscar (hash) y decodificar texturas */
	PROF_FB,        /* framebuffers: bajar/subir entre GX y la memoria de la PSP */
	PROF_PRESENTAR, /* componer y mostrar la imagen en la tele */
	PROF_ESPERA_GX, /* esperar a que la GPU del Wii termine (GX_DrawDone) */
	PROF_ES,        /* leer y escribir archivos (ISO, partidas) */
	PROF_AUDIO,     /* mezcla de sceAudio y sceSas */
	PROF_REGISTRO,  /* escribir wiisp.log */
	PROF_N
};

extern u64 (*prof_clock)(void);
extern u64 prof_hz;
extern u64 prof_acc[PROF_N];
extern u64 prof_last;
extern volatile int prof_cur;   /* volatile: lo lee el temporizador del muestreo */

/* Reloj del anfitrión (ticks y ticks por segundo); sin él no se mide */
void prof_set_clock(u64 (*clock)(void), u64 hz);

/* --- Contadores del procesador (0.2, prof_pmc) -----------------------------
   Con el interruptor y un anfitrión que los tenga (el Broadway del Wii), en
   cada cambio de cubeta o de fase del GE se leen cuatro contadores (ciclos,
   instrucciones terminadas, fallos de la caché de datos L1 y saltos mal
   predichos) y lo contado desde la lectura anterior se suma a lo que se
   estaba haciendo: cada cubeta y, dentro del GE, cada fase. Da los ciclos
   exactos por vértice, por triángulo o por instrucción de la PSP. */
enum { PMC_CICLOS, PMC_INSTR, PMC_FALLOS_L1D, PMC_SALTOS_MAL, PMC_N };
#define PROF_PMC_SLOTS (PROF_N + 7)   /* las cubetas y las fases del GE (GEF_N) */

extern int prof_pmc_on;                   /* hay contadores y están encendidos */
extern void (*prof_pmc_read)(u32 *out);   /* del anfitrión: PMC_N valores */
extern u64 prof_pmc_acc[PROF_PMC_SLOTS][PMC_N];
void prof_pmc_account(void);              /* lo contado, a la cubeta o fase actual */
void prof_pmc_enable(int on);             /* sin prof_pmc_read no hace nada */
//arreglo
/* Qué cuentan el tercero y el cuarto (0 = fallos de L1D y saltos mal
   predichos; 1 = fallos de datos en L2 y expulsiones de L2; 2 = ciclos
   esperando LR o CTR y saltos; 3 = ciclos de fallos de carga y saltos).
   Sin prof_pmc_modo_fn (el anfitrión no sabe cambiarlo) se queda en 0. */
extern int prof_pmc_modo;
extern void (*prof_pmc_modo_fn)(int modo);
void prof_pmc_set_modo(int modo);

/* Pasa a contar en la cubeta b; devuelve la anterior para volver a ella */
static inline int prof_switch(int b){
	int old = prof_cur;
	if(prof_clock){
		u64 now = prof_clock();
		prof_acc[old] += now - prof_last;
		prof_last = now;
	}
	if(prof_pmc_on) prof_pmc_account();
	prof_cur = b;
	return old;
}

/* Cuenta lo pendiente de la cubeta actual (antes de leer prof_acc) */
void prof_flush(void);

/* --- Qué hace el GE por dentro (muestreo) ---------------------------------
   Medir con el reloj cada vértice costaría más que el propio vértice. En
   su lugar, un temporizador del anfitrión llama a prof_sample() unas 1000
   veces por segundo y apunta la cubeta actual y, dentro del GE, la fase en
   marcha. Cambiar de fase solo escribe una variable:

       int old = prof_ge_enter(GEF_LEER);
       ...
       prof_ge_leave(old);
*/
enum {
	GEF_COMANDOS,     /* leer y aplicar comandos, preparar cada dibujo */
	GEF_LEER,         /* decodificar vértices (formato, huesos, morph) */
	GEF_TRANSFORMAR,  /* transformar, luces, uv y niebla */
	GEF_ENSAMBLAR,    /* triángulos: descarte, recorte, pantalla */
	GEF_ENVIAR,       /* mandar cada primitiva al renderizador (GX) */
	GEF_ESTADO,       /* preparar búferes y estado de GX */
	GEF_CURVAS,       /* teselar curvas (bezier/spline) */
	GEF_N
};
typedef char prof_pmc_slots_ok[PROF_PMC_SLOTS == PROF_N + GEF_N ? 1 : -1];

extern volatile int prof_ge_phase;
extern volatile u32 prof_samples[PROF_N], prof_ge_samples[GEF_N];

static inline int prof_ge_enter(int f){
	int old = prof_ge_phase;
	if(prof_pmc_on) prof_pmc_account();
	prof_ge_phase = f;
	return old;
}

static inline void prof_ge_leave(int old){
	if(prof_pmc_on) prof_pmc_account();
	prof_ge_phase = old;
}

//arreglo
/* --- La CPU por dentro (muestreo, 0.5.17.1) --------------------------------
   Lo mismo para la cubeta CPU: el intérprete predecodificado marca cuándo
   está en la VFPU, en la FPU, en la memoria lenta, buscando o traduciendo
   bloques o ejecutando una instrucción con el intérprete de siempre (una
   escritura, sin leer contadores) */
enum {
	CPF_TARJETAS,   /* las tarjetas y el paso entre bloques */
	CPF_VFPU,       /* vfpu_exec */
	CPF_FPU,        /* cpu_cop1_s */
	CPF_LENTA,      /* cpu_slow_load y cpu_slow_store */
	CPF_BUSCAR,     /* buscar, traducir y comprobar bloques */
	CPF_PASO,       /* una instrucción con el intérprete de siempre */
	CPF_N
};
extern volatile int prof_cpu_phase;
extern volatile u32 prof_cpu_samples[CPF_N];

enum { VXF_PREPARAR, VXF_DECODIFICAR, VXF_HUESOS, VXF_TRANSFORMAR, VXF_PANTALLA, VXF_LUCES, VXF_GENERICO, VXF_N };   //Arreglo
extern u8 prof_vtx_on;   //Arreglo
extern volatile int prof_vtx_phase;   //Arreglo
extern volatile u32 prof_vtx_samples[VXF_N];   //Arreglo

/* Desde el temporizador (interrupción o hilo aparte) */
void prof_sample(void);

#endif
