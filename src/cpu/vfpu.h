/**
 * WIISP - vfpu.h
 * La VFPU del Allegrex: 128 registros float organizados en 8 matrices 4x4,
 * con prefijos de swizzle/saturación y operaciones vectoriales y de
 * matrices. La usan casi todos los juegos para su matemática 3D.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_VFPU_H
#define WIISP_VFPU_H

#include "cpu/cpu.h"

/* Registros de control (mfvc/mtvc 128 + n) */
enum {
	VFPU_CTRL_SPREFIX = 0, VFPU_CTRL_TPREFIX, VFPU_CTRL_DPREFIX, VFPU_CTRL_CC,
	VFPU_CTRL_INF4, VFPU_CTRL_RSV5, VFPU_CTRL_RSV6, VFPU_CTRL_REV,
	VFPU_CTRL_RCX0, VFPU_CTRL_RCX1, VFPU_CTRL_RCX2, VFPU_CTRL_RCX3,
	VFPU_CTRL_RCX4, VFPU_CTRL_RCX5, VFPU_CTRL_RCX6, VFPU_CTRL_RCX7,
	VFPU_CTRL_MAX
};

/* Índice en cpu.v[] del registro de 7 bits (matriz, columna, fila): las
   columnas quedan contiguas, como en PPSSPP */
#define VFPU_INDEX(reg) (((((reg) >> 2) & 7) << 4) | (((reg) & 3) << 2) | (((reg) >> 5) & 3))

/* Estado inicial de la VFPU de un hilo */
void vfpu_reset(CpuState *c);

/* Ejecuta una instrucción de la VFPU (COP2 salvo las ramas, VFPU0-7,
   lv/sv). cpu.pc ya apunta a la siguiente. */
void vfpu_exec(u32 op, u32 pc);

/* Funciones de la VFPU, exactas al hardware (para pruebas) */
float vfpu_sin(float x);
float vfpu_cos(float x);
float vfpu_asin(float x);
float vfpu_rcp(float x);
float vfpu_rsqrt(float x);
float vfpu_sqrt(float x);
float vfpu_exp2(float x);
float vfpu_log2(float x);

#endif
