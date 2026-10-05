/**
 * WIISP - types.h
 * Tipos básicos y acceso a datos little-endian independiente del host.
 *
 * La PSP es little-endian y el Wii (Broadway) es big-endian. Todo dato que
 * venga de la PSP (archivos, RAM emulada) se lee SIEMPRE con estas funciones,
 * nunca con un cast directo, para que el mismo código funcione en un PC
 * (x86, little-endian) y en el Wii.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_TYPES_H
#define WIISP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
/* unsigned int y no uint32_t: en devkitPPC uint32_t es unsigned long y
   rompería los printf("%u") entre plataformas. */
typedef unsigned int u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int      s32;
typedef int64_t  s64;

_Static_assert(sizeof(u32) == 4, "u32 debe medir 4 bytes");

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WIISP_HOST_BIG_ENDIAN 1
#else
#define WIISP_HOST_BIG_ENDIAN 0
#endif

/* En PowerPC, GCC convierte memcpy + __builtin_bswap en lwbrx/lhbrx/stwbrx,
   que cuestan lo mismo que una carga normal. */
static inline u16 rd_le16(const void *p){
	u16 v; memcpy(&v, p, 2);
#if WIISP_HOST_BIG_ENDIAN
	v = __builtin_bswap16(v);
#endif
	return v;
}

static inline u32 rd_le32(const void *p){
	u32 v; memcpy(&v, p, 4);
#if WIISP_HOST_BIG_ENDIAN
	v = __builtin_bswap32(v);
#endif
	return v;
}

static inline void wr_le16(void *p, u16 v){
#if WIISP_HOST_BIG_ENDIAN
	v = __builtin_bswap16(v);
#endif
	memcpy(p, &v, 2);
}

static inline void wr_le32(void *p, u32 v){
#if WIISP_HOST_BIG_ENDIAN
	v = __builtin_bswap32(v);
#endif
	memcpy(p, &v, 4);
}

#endif
