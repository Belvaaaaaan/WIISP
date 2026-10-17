/**
 * WIISP - core/opt.h
 * Interruptores de optimización: uno por cada entrada de la hoja de ruta
 * (docs/HOJA_DE_RUTA.md), con el mismo nombre, para medir cada una con y
 * sin ella y poder bisectar cuando algo se rompe.
 *
 * El camino lento (el de siempre) nunca se borra: un interruptor apagado
 * vuelve a él. Leer un interruptor cuesta una carga de memoria:
 *
 *     if(opt_on[OPT_GE_SKIN_RAPIDO]) ...camino rápido...
 *     else ...camino de referencia...
 *
 * Los nombres están todos reservados desde ya; una entrada que todavía no
 * existe en el código tiene `lista = 0`, se ignora al activarla y no sale en
 * la línea [OPT] del registro.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_OPT_H
#define WIISP_OPT_H

#include <stddef.h>
#include "core/types.h"

enum {
	/* Fase 0: medir */
	OPT_PROF_ESPERA,      /* 0.1 espera separada en el desglose de tiempos */
	OPT_PROF_PMC,         /* 0.2 contadores del Broadway y muestreo */
	OPT_PROF_ESTADO,      /* censo: qué parte del estado de cada llamada de
	                         dibujo se repite (para 1.4); solo mide */
	OPT_PROF_TIRONES,     /* censo de tirones: los cuadros que tardan mucho
	                         más de lo normal y en qué se fue el tiempo */
	OPT_PROF_ES,          /* censo de las lecturas del disco: tamaños, tiempo
	                         real y cuánto podría esconder leer en otro hilo */
	//arreglo
	OPT_PROF_PMC_L2,      /* prof_pmc cuenta fallos de datos en L2 y
	                         expulsiones de L2 (en vez de L1D y saltos) */
	OPT_PROF_PMC_CTR,     /* prof_pmc cuenta los ciclos con el salto parado
	                         esperando LR o CTR (en vez de L1D) */
	OPT_PROF_PMC_CARGA,   /* prof_pmc cuenta los ciclos de fallos de carga en
	                         L1D (en vez de L1D) */
	/* Primera tanda del GE (resultado idéntico) */
	OPT_GE_SKIN_RAPIDO,   /* skinning mezclando las matrices por peso */
	OPT_GE_MTX_CACHE,     /* matrices combinadas solo cuando cambian */
	OPT_GE_COLOR_CONST,   /* color constante sin luces */
	OPT_GE_TEX_CLUT,      /* texturas CLUT16/CLUT32 decodificadas de una vez */
	/* Fase 1: GE */
	OPT_GE_RTT_GPU,       /* 1.1 render-to-texture en la GPU (texturas con
	                         paleta desde la copia del EFB: CI8 + TLUT) */
	OPT_GE_FB_RELLENO,    /* leer/escribir el relleno de las filas de un
	                         framebuffer no lo baja ni lo invalida */
	OPT_GE_FB_HUECO,      /* un búfer pequeño encima de unas filas de otro
	                         no lo saca de la GPU (hueco) */
	OPT_GE_Z_RANGO,       /* la profundidad se vigila solo en los bytes
	                         donde puede caer con su swizzle */
	OPT_GE_VDEC_ESP,      /* 1.2 decodificadores de vértices por vtype */
	OPT_GE_VDEC_PSQ,      /* 1.2b conversión con psq_l y GQR */
	OPT_GE_VERIF,         /* comprobar los caminos rápidos del GE contra los de
	                         referencia en el Wii (ge_vdec_esp, gx_envio_rapido,
	                         ge_ensamblar_rapido, ge_vdec_psq): lento, solo
	                         para pruebas */
	OPT_GX_ENVIO,         /* GX: cada triángulo calculado antes de escribirlo
	                         en el FIFO, sin preparar si no hace falta */
	OPT_GE_TRI_RAPIDO,    /* sin descarte de caras, cada triángulo se
	                         comprueba una vez y solo se envía su sentido */
	OPT_GE_ENSAMBLAR,     /* ensamblar triángulos sin copiar vértices, con
	                         los descartes de cada vértice ya calculados */
	OPT_GE_BBOX,          /* 1.3 descarte por caja envolvente */
	OPT_GE_ESTADO_SUCIO,  /* 1.4, primera parte: el estado de una llamada de
	                         dibujo (formato, transformación, rasterizador,
	                         estado de GX y la preparación de cada
	                         triángulo) solo se rehace si algo cambió */
	OPT_GE_CHAROLAS,      /* 1.4 agrupar llamadas de dibujo */
	/* Fase 2: intérprete cacheado */
	OPT_CPU_SIN_EXTRA,    /* 2.1 sin costo por instrucción */
	OPT_CPU_PREDEC,       /* 2.2 predecodificación (cpu/predec.h) */
	OPT_CPU_VERIFICAR,    /* cada bloque predecodificado comprueba al entrar
	                         que sus instrucciones siguen en la RAM */
	//arreglo
	OPT_PREDEC_COMPACTO,  /* bloques en dos partes: lo que se lee al ejecutar,
	                         alineado a 32, y lo demás en otra arena */
	OPT_PREDEC_SIN_UNIR,  /* el bucle compilado sin unir los finales de las
	                         tarjetas (-fno-crossjumping) */
	OPT_PREDEC_MEM1,      /* los bloques en MEM1 (reservada al arrancar) */
	OPT_CPU_FUSION,       /* 2.3 especializar y fusionar */
	OPT_HLE_RAPIDO,       /* 2.4a camino mínimo de CpuSuspendIntr y Memcpy */
	OPT_HLE_REEMPLAZO,    /* 2.4b reemplazo de funciones por hash */
	OPT_MEM_BAT,          /* 2.5 RAM de la PSP mapeada con BAT */
	/* Fase 3: GX transforma */
	OPT_GX_TRANSFORM,     /* 3.1 transformación por hardware */
	OPT_GX_VCACHE,        /* 3.2 caché de vértices convertidos */
	OPT_GX_DLCACHE,       /* 3.3 display lists grabadas */
	OPT_GX_TLUT,          /* 3.4 paletas como TLUT */
	OPT_GX_PARALELO,      /* 3.5 CPU y GPU en paralelo: presentar sin esperar
	                         a la GPU (la imagen sale un vblank después) */
	/* Fase 4: dynarec */
	OPT_JIT,              /* 4.1 emisor y dynarec con hilos */
	OPT_JIT_ENLACE,       /* 4.2 encadenamiento y pila de retorno */
	OPT_JIT_LOCKSTEP,     /* 4.3 comparar contra el intérprete (depuración) */
	OPT_JIT_NATIVO,       /* 4.4 traducción nativa */
	OPT_JIT_REGS,         /* 4.5a registros fijos */
	OPT_CACHE_LOCK,       /* 4.5b caché de datos bloqueada */
	OPT_JIT_IDLE,         /* 4.6a detección de idle loops */
	OPT_JIT_FASTMEM,      /* 4.6b backpatching con DSI */
	OPT_JIT_SD,           /* 4.7 bloques guardados en la SD */
	/* Fase 5: DSP */
	OPT_DSP_SALIDA,       /* 5.1 salida de audio con libaesnd */
	OPT_DSP_SAS,          /* 5.2 voces de sceSas en el DSP */
	/* Fuera de la hoja de ruta */
	OPT_REGISTRO_DIFERIDO, /* las líneas de diagnóstico no fuerzan un fsync */
	OPT_REGISTRO_HILO,    /* wiisp.log se escribe desde otro hilo (core/registro.h) */
	OPT_N
};

typedef struct {
	const char *nombre;   /* el del menú de la hoja de ruta */
	u8 lista;             /* 1 = ya está implementada */
	u8 por_defecto;       /* valor al arrancar */
} OptInfo;

extern const OptInfo opt_info[OPT_N];
/* Valor actual de cada interruptor (0 o 1) */
extern u8 opt_on[OPT_N];

/* Vuelve a los valores por defecto */
void opt_reset(void);

/* Índice de un interruptor por su nombre, o -1 */
int  opt_find(const char *nombre);

/* Cambia un interruptor por nombre.
   Devuelve 0 si se cambió, -1 si el nombre no existe y 1 si existe pero
   todavía no está implementado (no se cambia). */
int  opt_set(const char *nombre, int on);

/* Lee "nombre=0|1" o "nombre 0|1" (como en autotest.txt y --opt).
   Devuelve lo mismo que opt_set, o -1 si el texto no se entiende. */
int  opt_parse(const char *texto);

/* Línea legible con los interruptores implementados y su valor, para
   [OPT] en el registro */
void opt_describe(char *buf, size_t size);

#endif
