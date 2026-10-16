/**
 * WIISP - ge.h
 * GE (Graphics Engine) de la PSP: interfaz pública.
 *
 * El GE ejecuta listas de comandos de 32 bits (8 bits de comando y 24 de
 * argumento) que el juego escribe en memoria y encola con sceGeListEnQueue.
 * Aquí se interpretan de forma síncrona: cada vez que la lista avanza
 * (encolado o actualización del "stall") se ejecuta hasta donde se pueda.
 *
 * El dibujo por software escribe directamente en la VRAM emulada, igual que
 * el hardware real, así que el framebuffer y las texturas renderizadas se
 * comportan como en la PSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_GE_H
#define WIISP_GE_H

#include "core/types.h"

void ge_init(void);
void ge_shutdown(void);

/* Estadísticas por frame (para el medidor del frontend) */
typedef struct {
	u32 commands;
	u32 primitives;
	u32 vertices;     /* vértices calculados (decodificar + transformar) */
	u64 pixels;
	u32 lists;        /* veces que se procesó la cola de listas */
	u64 host_ticks;   /* tiempo del anfitrión dentro del GE (si hay reloj) */

	/* Detalle para [TIEMPOS] */
	u32 draws;              /* llamadas de dibujo (PRIM) */
	u32 draws_indexed;      /* con índices */
	u32 draws_reuse;        /* con índices repetidos: cada vértice se calcula una vez */
	u32 draws_prim[8];      /* por tipo de primitiva (GE_PRIM_*) */
	u32 curves;             /* bezier y spline */
	u32 vertex_reads;       /* vértices que pidieron las primitivas */
	u32 vertices_skinned;   /* calculados con huesos */
	u32 vertices_morph;
	u32 vertices_through;   /* ya en coordenadas de pantalla (2D) */
	u32 vertices_lit;       /* con luces */
	u32 lights;             /* suma de luces encendidas de los iluminados */
	u32 tris;               /* triángulos recibidos */
	u32 tris_drawn;
	u32 tris_back;          /* de espaldas o sin área */
	u32 tris_outside;       /* fuera de la pantalla o de la tijera */
	u32 tris_clipped;       /* cortados por el plano cercano */
	u32 sprites;
	u32 mtx_combined;       /* mundo*vista*proyección calculadas */
	u32 mtx_reused;         /* las mismas matrices: reutilizadas (ge_mtx_cache) */
	u32 tris_cull_off;      /* sin descarte de caras: se procesan en los dos sentidos */

	/* Censo de estado (prof_estado): qué se repite de una llamada de dibujo
	   a la siguiente. "igual": el resultado salió como en la llamada
	   anterior; "limpio": ningún comando que lo afecte cambió de valor
	   desde entonces; "error": limpio pero distinto (la tabla de grupos de
	   ge.c se equivoca: tiene que ser 0). */
	u32 cs_cmds_estado;     /* comandos de estado escritos */
	u32 cs_cmds_igual;      /* ... con el valor que ya tenían */
	u32 cs_mtx, cs_mtx_igual;          /* datos de mundo, vista, proyección y textura */
	u32 cs_huesos, cs_huesos_igual;    /* datos de los huesos */
	u32 cs_paletas, cs_paletas_igual;  /* LOADCLUT con los bytes que ya había */
	u32 cs_vt;                          /* llamadas comparadas (formato y transformación) */
	u32 cs_v_igual, cs_v_limpio, cs_v_error;   /* uv y morph (el formato va aparte, por tipo de vértice) */
	u32 cs_t_igual, cs_t_limpio, cs_t_error;   /* transformación y luces */
	u32 cs_r;                           /* estados del rasterizador comparados */
	u32 cs_r_igual, cs_r_limpio, cs_r_error;   /* píxel, muestreo y tijera */
	/* Lo que apunta el renderizador de GX */
	u32 cs_dst, cs_dst_igual, cs_dst_aviso;    /* destinos y textura (setup_targets): iguales; con un aviso entre medias */
	u32 cs_tex, cs_tex_igual;                  /* con textura: la misma que la vez anterior */
	u32 cs_gx, cs_gx_igual, cs_gx_tocado;      /* estado de GX (apply_gx): mismas entradas; GX tocado por otros entre medias */
	u32 cs_tri, cs_tri_listo, cs_tri_primero, cs_tri_mitad;  /* primitivas a GX; listas sin preparar nada; primeras de su
	                                                             llamada; no listas a mitad de una llamada */

	/* ge_estado_sucio en GX: estado de GX que había que poner y cuántas veces
	   no se emitió (el mismo que el anterior); triángulos sin preparar. Con
	   ge_verificar: emisiones que se habrían saltado comprobadas contra el
	   estado interno de libogc (y si no se pudo leer), y triángulos sin
	   preparar comprobados */
	u32 es_gx, es_gx_saltado, es_tri, es_tri_listo;
	u32 es_gx_verif, es_gx_verif_bad, es_gx_verif_sin, es_tri_verif, es_tri_verif_bad;
} GeStats;
void ge_get_stats(GeStats *out);

/* Censo de formatos de vértice desde el último informe (los más usados
   primero) en una línea, y lo pone a cero */
void ge_vtype_census(char *buf, size_t size);
/* prof_estado: el censo de estado desde g0 hasta g en una línea por cada \n */
void ge_state_census_report(const GeStats *g, const GeStats *g0, char *buf, size_t size);
/* ge_verificar: vértices y descartes comparados y distintos desde el último informe */
void ge_vdec_verif_report(char *buf, size_t size);

/* Sube cada vez que la CPU se sincroniza con el GE (sceGeDrawSync,
   sceGeListSync, fin de una lista) y cuando un framebuffer de la GPU baja a
   la memoria. Una textura no puede cambiar dentro de un mismo periodo sin
   avisar (como textureSyncTimeDomain de PPSSPP): las cachés de texturas la
   comprueban como mucho una vez por periodo. */
extern u32 ge_sync_domain;

/* Turbo: las llamadas de dibujo (PRIM, bezier, spline) solo avanzan sus
   direcciones, sin calcular vértices ni dibujar. Lo demás del GE (estado,
   transferencias, CLUT, señales) sigue igual, así que el juego avanza
   mucho más rápido mientras la pantalla se queda quieta. */
extern int ge_turbo;

/* Reloj del anfitrión para medir el GE (perfilado); NULL = sin medir */
extern unsigned long long (*ge_host_clock)(void);
#ifdef WIISP_PROF
/* Perfilado (make PROF=1): ticks en vértices, backend, ge_draw_prim,
   su preparación, luces y decodificación */
extern unsigned long long ge_prof[8];
#define GE_PROF_T0 unsigned long long prof_t0 = ge_host_clock ? ge_host_clock() : 0
#define GE_PROF_ADD(n) do { if(ge_host_clock) ge_prof[n] += ge_host_clock() - prof_t0; } while(0)
#else
#define GE_PROF_T0 (void)0
#define GE_PROF_ADD(n) (void)0
#endif

#endif
