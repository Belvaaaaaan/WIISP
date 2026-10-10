/**
 * WIISP - gpu/texcache.h
 * Cuándo hay que volver a leer una textura: la parte de la caché de
 * texturas de un renderizador por hardware (GX) que no depende de la GPU.
 *
 * Decodificar una textura de la PSP al formato de la GPU es caro, y
 * comprobar si cambió (leerla entera y calcular su hash) también. Antes
 * se comprobaba en cada llamada de dibujo; ahora:
 *
 *  1. Cada textura se lee como mucho una vez por periodo entre
 *     sincronizaciones de la CPU con el GE (ge_sync_domain), como el
 *     textureSyncTimeDomain de PPSSPP: dentro de un periodo, el juego no
 *     puede cambiarla sin avisar.
 *  2. Espaciado (tc->backoff): una textura que no cambia se vuelve a leer a
 *     los 1, 2, 4, 8 y 16 cuadros; después, cada 16 (y un desfase para que
 *     no coincidan todas).
 *  3. Redes de seguridad:
 *     - Los avisos de escritura de memory.c (archivos, DMA, copias del GE,
 *       la CPU en la VRAM, vaciar la caché de datos) obligan a comprobar.
 *     - Bytes que la GPU tiene más nuevos (un framebuffer dibujado encima):
 *       se comprueba, y al leerlos bajan de la GPU.
 *     - Durante el espaciado, un vistazo a 8 palabras de la textura en cada
 *       uso.
 *     - Una textura que cambió sin aviso pasa a "inestable": se comprueba
 *       en cada periodo hasta que pasen TC_CALM comprobaciones sin cambios.
 *
 * Si el contenido vuelve a uno ya visto (dos imágenes que se alternan en la
 * misma dirección), se reutiliza su copia sin decodificar.
 *
 * El dueño de los datos de la GPU (gx_ge.c) aporta tres funciones: dar una
 * casilla libre (expulsando con tc_free si hace falta), decodificar en una
 * casilla y decir si un rango de la VRAM es más nuevo en la GPU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_TEXCACHE_H
#define WIISP_TEXCACHE_H

#include "core/types.h"

#define TC_MAX       256   /* texturas en la caché */
#define TC_LEVELS    8     /* niveles de mipmap */
#define TC_SAMPLES   8     /* palabras del vistazo */
#define TC_BUCKETS   256
#define TC_CHECK_MAX 16    /* tope del espaciado (cuadros) */
#define TC_ALL_HINTS 16    /* sceKernelDcacheWritebackAll que obligan a comprobar */
#define TC_CALM      60    /* comprobaciones sin cambios para dejar de ser inestable */

/* Lo que identifica una textura (sin su contenido) */
typedef struct {
	u32 addr, fmt, size, bufw, clutfmt, clut_hash, mipkey;
	u32 levels;
} TcKey;

/* Una búsqueda: la clave y los bytes de cada nivel (len 0: sin puntero
   contiguo; se decodifica en cada cuadro, como antes) */
typedef struct {
	TcKey key;
	u32 lv_addr[TC_LEVELS], lv_len[TC_LEVELS];
} TcQuery;

typedef struct {
	int used;
	int current;       /* la que vale ahora para su clave */
	int direct;        /* todos sus niveles con puntero contiguo */
	int unstable;      /* cambió sin aviso: se comprueba en cada periodo */
	TcKey key;
	u32 hash;
	u32 lv_addr[TC_LEVELS], lv_len[TC_LEVELS];
	u32 stamp, all_hints, domain;   /* al comprobarla */
	u32 next_check, interval, calm, checks;
	u32 used_frame;
	u32 nsamples, sample_off[TC_SAMPLES], sample[TC_SAMPLES];
	int hnext;         /* cadena de su cubeta */
} TcEntry;

typedef struct {
	u32 lookups;            /* texturas elegidas */
	u32 quick;              /* sin leerlas */
	u32 checks;             /* leídas enteras (hash) */
	u64 check_bytes;
	u32 decodes;
	u32 changes_notified;   /* cambios con aviso */
	u32 changes_silent;     /* cambios sin aviso (pasan a inestables) */
	u32 sample_misses;      /* el vistazo vio un cambio */
} TcStats;

typedef struct TexCache {
	TcEntry e[TC_MAX];
	int bucket[TC_BUCKETS];
	int backoff;            /* espaciado activado */
	u32 frame;              /* cuadro actual (lo pone el dueño) */
	TcStats stats;
	void *ctx;
	int (*alloc)(struct TexCache *tc, void *ctx);
	int (*decode)(struct TexCache *tc, void *ctx, int idx);
	int (*gpu_newer)(void *ctx, u32 addr, u32 len);
} TexCache;

/* Vacía la caché (sin llamar al dueño) */
void tc_init(TexCache *tc);
/* Índice de la textura lista para usar, o -1 si no se pudo decodificar */
int  tc_lookup(TexCache *tc, const TcQuery *q);
/* Libera una casilla (el dueño ya soltó sus datos) */
void tc_free(TexCache *tc, int idx);
int  tc_unstable_count(const TexCache *tc);

/* Hash rápido de bytes (para comprobar cambios; el valor depende del
   orden de bytes del anfitrión) */
u32  tc_hash(const u8 *p, u32 n, u32 seed);

#endif
