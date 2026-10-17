/**
 * WIISP - core/registro.h
 * Escribir wiisp.log desde un hilo aparte (registro_hilo).
 *
 * En la SD del Wii cada fsync tarda de 10 a 300 ms, y antes la emulación
 * se paraba todo ese tiempo. Con esto, quien escribe en el registro solo
 * copia el texto a una cola; un hilo escritor lo pasa al archivo (fwrite,
 * fflush) y hace los fsync. Mientras el escritor espera a la tarjeta (en
 * el Wii, a IOS), la emulación sigue.
 *
 * El texto y su orden no cambian: es el mismo archivo que sin el hilo. Lo
 * que decide cuándo sincronizar (una vez por segundo, las líneas de
 * diagnóstico cada 10 s, [CPU] en el acto) sigue en hle.c; aquí solo se
 * hace. Antes de tocar el archivo de otra forma (cerrarlo, abrir otro,
 * escribir sin la cola) hay que llamar a reg_esperar.
 *
 * Sin hilos en el anfitrión (Windows por ahora) reg_disponible da 0 y
 * hle.c escribe en el acto, como antes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_REGISTRO_H
#define WIISP_REGISTRO_H

#include <stdio.h>
#include "core/types.h"

#define REG_COLA (64u * 1024)   /* bytes en la cola */

/* ¿Hay hilo escritor? (lo crea la primera vez; 0 si no se puede) */
int  reg_disponible(void);
/* ¿Queda algo en la cola o a medio escribir? (sin esperar) */
int  reg_pendiente(void);
/* Pone n bytes de texto para el archivo f. Con sync, después de escribirlo
   (y todo lo anterior) el escritor hace fsync. Si la cola está llena,
   espera a que haya sitio. Si f no es el de antes, primero se termina de
   escribir lo del anterior. */
void reg_poner(FILE *f, const char *texto, u32 n, int sync);
/* Termina lo pendiente y cambia de archivo (NULL antes de cerrarlo: así
   el escritor ya no lo usa ni para un fsync) */
void reg_archivo(FILE *f);
/* Que el escritor haga fsync en cuanto acabe lo que tiene (sin esperar) */
void reg_pedir_fsync(void);
/* Espera a que todo lo puesto esté en el archivo; con fsync, sincronizado */
void reg_esperar(int fsync);

typedef struct {
	u64 bytes, lotes, fsyncs;       /* escrito por el hilo */
	u64 t_escribir, t_fsync;        /* ticks de prof_clock dentro de fwrite+fflush y fsync */
	u64 t_fsync_max;
	u64 esperas, t_esperas;         /* la emulación esperando: cola llena o reg_esperar */
	u32 max_cola;                   /* lo más lleno que estuvo la cola (bytes) */
} RegStats;
void reg_stats(RegStats *out);

/* Pruebas: capacidad de la cola (1..REG_COLA) para forzar que se llene */
void reg_test_capacidad(u32 cap);

#endif
