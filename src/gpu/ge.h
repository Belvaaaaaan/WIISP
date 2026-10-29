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
	u32 vertices_ps;   //Arreglo

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
	u32 bb_cmds, bb_vertices, bb_visibles, bb_count0, bb_sin_probar, bb_huesos;   //Arreglo
	u64 bb_ticks;   //Arreglo
	u32 bj_total, bj_sin_caja, bj_tomados, bj_llamadas, bj_vertices, bj_huesos, bj_con_llamadas, bj_raros;   //Arreglo
	u32 ob_n, ob_vacios, ob_llamadas, ob_vertices, ob_nada, ob_nada_llamadas, ob_nada_vertices, ob_nada_huesos, ob_nada_huesos_vertices, ob_fuera;   //Arreglo
	u64 ob_ticks, ob_nada_ticks;   //Arreglo
	u32 sc_llamadas, sc_vertices, sc_nada, sc_nada_vertices, sc_nada_huesos, sc_nada_huesos_vertices, sc_fuera, sc_fuera_vertices;   //Arreglo
	u64 sc_ticks, sc_nada_ticks, sc_fuera_ticks;   //Arreglo
	u32 cp_verif, cp_verif_bad;   //Arreglo
	u32 bbv_cajas, bbv_malas;   //Arreglo
	u32 bx_cajas, bx_fuera, bx_detras, bx_llamadas, bx_formato, bx_matrices, bx_fuera_caja, bx_saltadas, bx_ligeras, bx_vertices;   //Arreglo
	u32 bx_verif, bx_verif_bad;   //Arreglo
	u32 bx_pantalla, bx_vistas, bx_dentro, bx_dudosas, bx_cruzan;   //Arreglo
	u32 bx_ll_probadas, bx_ll_vista, bx_ll_fuera, bx_ll_pantalla, bx_ll_saltadas, bx_ll_vertices, bx_cache_mal;   //Arreglo
	u64 bx_ll_ticks;   //Arreglo
	u64 bx_ll_caja_ticks, bx_t_ticks;   //Arreglo
	u32 bx_ll_cajas, bx_ll_leidos;   //Arreglo
	u32 bx_guarda, bx_ll_guarda, bx_g_probadas, bx_ll_tri, bx_t_probadas;   //Arreglo
	u32 bx_nd_ll, bx_nd_vert, bx_nd_prof, bx_nd_razon[9], bx_nd_v[5];   //Arreglo
	u32 bx_nd_t[5], bx_nd_tri, bx_nd_tri_vert;   //Arreglo
	u32 ob_nada_clase[5], ob_nada_clase_vertices[5];   //Arreglo
	u32 hu_datos, hu_iguales, hu_mat, hu_mat_iguales, hu_rachas, hu_racha_datos, hu_rachas_largas, hu_llamadas;   //Arreglo
	u32 cm_verif, cm_verif_bad;   //Arreglo
	u32 ch_rachas, ch_cmds, ch_verif, ch_verif_cmds, ch_verif_bad;   //Arreglo
	u32 ch_llamadas, ch_siguen, ch_sin, ch_clave, ch_textura, chv_llamadas, chv_malas;   //Arreglo
	u32 ce_tandas, ce_juntos, ce_sueltos;   //Arreglo
	u32 rp_cuadros, rp_llamadas, rp_vertices, rp_vert_huesos, rp_igual_ll, rp_igual_v, rp_igual_vh;   //Arreglo
	u32 rp_huesos_ll, rp_huesos_v, rp_datos_ll, rp_datos_v, rp_anterior_ll, rp_anterior_v, rp_lleno, rp_huesos_vh;   //Arreglo
} GeStats;
void ge_get_stats(GeStats *out);
void ge_repetidas_report(char *buf, size_t size);   //Arreglo

/* Censo de formatos de vértice desde el último informe (los más usados
   primero) en una línea, y lo pone a cero */
void ge_vtype_census(char *buf, size_t size);
/* prof_estado: el censo de estado desde g0 hasta g en una línea por cada \n */
void ge_state_census_report(const GeStats *g, const GeStats *g0, char *buf, size_t size);
void ge_bbox_census_report(const GeStats *g, const GeStats *g0, u64 hz, char *buf, size_t size);   //Arreglo
void ge_cmd_census_report(char *buf, size_t size);   //Arreglo
/* ge_verificar: vértices y descartes comparados y distintos desde el último informe */
void ge_vdec_verif_report(char *buf, size_t size);
int ge_vdec_ps_disponible(void);   //Arreglo

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

//Arreglo
typedef struct {
	u32 dibujos, n_escr, n_lect, escr_dibujos, escr_vertices, lect_dibujos, volcados, archivos, vol_top_idx, vol_top_n;
	int cuadra;
} EfectosDatos;
void ge_efectos_dibujo(u32 prim, u32 count);   //Arreglo
void ge_efectos_informe(void);   //Arreglo
void ge_efectos_volcado(const char *prefijo, unsigned cuadro);   //Arreglo
void ge_efectos_datos(EfectosDatos *d);   //Arreglo
extern void (*ge_efectos_sincronizar)(void);   //Arreglo

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
