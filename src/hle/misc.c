/**
 * WIISP - misc.c
 * HLE de módulos pequeños: sceRtc (reloj), sceSuspendForUser, sceDmac
 * (copias DMA) y sceNetInet (sin red). sceUtility está en utility.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "hle/hle.h"
#include "core/memory.h"

#include <string.h>

/* --- sceRtc ------------------------------------------------------------ */
/* Los ticks del RTC son microsegundos desde el 1 de enero del año 1. */

#define RTC_UNIX_EPOCH_TICKS 62135596800000000ull

static u64 unix_now_us(void){
	return (u64)HLE_EPOCH_BASE * 1000000ull + hle_now_us();
}

static void sceRtcGetTickResolution(void){ RETURN(1000000); }

static void sceRtcGetCurrentTick(void){
	u32 p = ARG(0);
	u64 tick = RTC_UNIX_EPOCH_TICKS + unix_now_us();
	if(!mem_valid(p, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(p, (u32)tick);
	mem_write32(p + 4, (u32)(tick >> 32));
	RETURN(0);
}

/* ScePspDateTime: u16 año, mes, día, hora, minuto, segundo; u32 microsegundo */
void hle_write_datetime(u32 p, u64 unix_us){
	u64 secs = unix_us / 1000000ull;
	s64 days = (s64)(secs / 86400ull);
	u32 rem = (u32)(secs % 86400ull);
	/* Fecha civil a partir de días desde 1970 (algoritmo de H. Hinnant) */
	s64 z = days + 719468, era = z / 146097;
	u32 doe = (u32)(z - era * 146097);
	u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	u32 mp = (5 * doy + 2) / 153;
	u32 day = doy - (153 * mp + 2) / 5 + 1;
	u32 month = mp < 10 ? mp + 3 : mp - 9;
	u32 year = (u32)(yoe + era * 400) + (month <= 2);

	mem_write16(p, (u16)year);
	mem_write16(p + 2, (u16)month);
	mem_write16(p + 4, (u16)day);
	mem_write16(p + 6, (u16)(rem / 3600));
	mem_write16(p + 8, (u16)(rem / 60 % 60));
	mem_write16(p + 10, (u16)(rem % 60));
	mem_write32(p + 12, (u32)(unix_us % 1000000ull));
}

static void sceRtcGetCurrentClock(void){
	if(!mem_valid(ARG(0), 16)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	hle_write_datetime(ARG(0), unix_now_us() + (u64)((s64)(s32)ARG(1) * 60 * 1000000));
	RETURN(0);
}

static void sceRtcGetCurrentClockLocalTime(void){
	if(!mem_valid(ARG(0), 16)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	hle_write_datetime(ARG(0), unix_now_us());
	RETURN(0);
}

/* Días desde 1970 de una fecha civil (H. Hinnant) */
static s64 days_from_civil(s64 y, u32 m, u32 d){
	s64 era;
	u32 yoe, doy, doe;
	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (u32)(y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (s64)doe - 719468;
}

static u64 read_tick(u32 p){ return ((u64)mem_read32(p + 4) << 32) | mem_read32(p); }
static void write_tick(u32 p, u64 t){ mem_write32(p, (u32)t); mem_write32(p + 4, (u32)(t >> 32)); }

/* sceRtcGetTick(ScePspDateTime *, u64 *tick) */
static void sceRtcGetTick(void){
	u32 dt = ARG(0), out = ARG(1);
	s64 days;
	u64 us;
	if(!mem_valid(dt, 16) || !mem_valid(out, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	days = days_from_civil(mem_read16(dt), mem_read16(dt + 2), mem_read16(dt + 4));
	us = (u64)((days * 86400 + mem_read16(dt + 6) * 3600 + mem_read16(dt + 8) * 60 + mem_read16(dt + 10)) * 1000000ll)
	     + mem_read32(dt + 12);
	write_tick(out, RTC_UNIX_EPOCH_TICKS + us);
	RETURN(0);
}

/* sceRtcSetTick(ScePspDateTime *, const u64 *tick) */
static void sceRtcSetTick(void){
	u32 dt = ARG(0), in = ARG(1);
	if(!mem_valid(dt, 16) || !mem_valid(in, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	hle_write_datetime(dt, read_tick(in) - RTC_UNIX_EPOCH_TICKS);
	RETURN(0);
}

static void sceRtcCompareTick(void){
	u64 a, b;
	if(!mem_valid(ARG(0), 8) || !mem_valid(ARG(1), 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	a = read_tick(ARG(0)); b = read_tick(ARG(1));
	RETURN(a > b ? 1 : a < b ? (u32)-1 : 0);
}

static void tick_add(u64 mult, int wide){
	u32 dst = ARG(0), src = ARG(1);
	s64 n = wide ? (s64)(((u64)ARG(3) << 32) | ARG(2)) : (s64)(s32)ARG(2);
	if(!mem_valid(dst, 8) || !mem_valid(src, 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	write_tick(dst, read_tick(src) + (u64)(n * (s64)mult));
	RETURN(0);
}

static void sceRtcTickAddTicks(void){ tick_add(1, 1); }
static void sceRtcTickAddMicroseconds(void){ tick_add(1, 1); }
static void sceRtcTickAddSeconds(void){ tick_add(1000000ull, 1); }
static void sceRtcTickAddMinutes(void){ tick_add(60000000ull, 1); }
static void sceRtcTickAddHours(void){ tick_add(3600000000ull, 0); }
static void sceRtcTickAddDays(void){ tick_add(86400000000ull, 0); }

static int leap(u32 y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static void sceRtcIsLeapYear(void){ RETURN(leap(ARG(0))); }
static void sceRtcGetDaysInMonth(void){
	static const u8 days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	u32 m = ARG(1);
	if(m < 1 || m > 12){ RETURN(0x80000102u); return; }
	RETURN(days[m - 1] + (m == 2 && leap(ARG(0))));
}
static void sceRtcGetDayOfWeek(void){
	s64 d = days_from_civil(ARG(0), ARG(1), ARG(2));
	RETURN((u32)(((d % 7) + 7 + 4) % 7));   /* 1970-01-01 fue jueves */
}

/* Hora local = UTC en WIISP */
static void sceRtcConvertTick(void){
	if(!mem_valid(ARG(0), 8) || !mem_valid(ARG(1), 8)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	write_tick(ARG(1), read_tick(ARG(0)));
	RETURN(0);
}

/* --- sceSuspendForUser ----------------------------------------------- */
/* La "memoria volátil" son 4 MB en 0x08400000 que el sistema presta al
   juego cuando no los necesita. */

static void sceKernelVolatileMemLock(void){
	if(ARG(1)) mem_write32(ARG(1), 0x08400000u);
	if(ARG(2)) mem_write32(ARG(2), 0x00400000u);
	RETURN(0);
}

static void return_zero(void){ RETURN(0); }

/* --- sceUtility ---------------------------------------------------------- */

/* --- sceDmac: copias por DMA ------------------------------------------- */
/* Se copia al instante; el hilo espera lo que tardaría el DMA real
   (unos 236 bytes/us medidos por PPSSPP) y mientras tanto TryMemcpy
   devuelve "ocupado". */

#define SCE_KERNEL_ERROR_BUSY            0x80000021u
#define SCE_KERNEL_ERROR_PRIV_REQUIRED   0x80000023u
#define SCE_KERNEL_ERROR_INVALID_POINTER 0x80000103u
#define SCE_KERNEL_ERROR_INVALID_SIZE    0x80000104u

static u64 dmac_deadline;

static void dmac_copy(int try_only){
	u32 dst = ARG(0), src = ARG(1), size = ARG(2);
	u8 *d; const u8 *s;
	if(size == 0){ RETURN(SCE_KERNEL_ERROR_INVALID_SIZE); return; }
	if(!mem_ptr(dst, 1) || !mem_ptr(src, 1)){ RETURN(SCE_KERNEL_ERROR_INVALID_POINTER); return; }
	if(size >= 0x80000000u || dst + size >= 0x80000000u || src + size >= 0x80000000u){
		RETURN(SCE_KERNEL_ERROR_PRIV_REQUIRED);
		return;
	}
	if(try_only && dmac_deadline > cpu_cycles){ RETURN(SCE_KERNEL_ERROR_BUSY); return; }
	d = mem_ptr(dst, size);
	s = mem_ptr_r(src, size);
	if(d && s) memmove(d, s, size);
	else {
		u32 i;
		for(i = 0; i < size; i++) mem_write8(dst + i, mem_read8(src + i));
	}
	RETURN(0);
	if(size >= 272){
		dmac_deadline = cpu_cycles + (u64)(size / 236) * CYCLES_PER_US;
		kernel_wait_until(dmac_deadline);
	}
}

static void sceDmacMemcpy(void){ dmac_copy(0); }
static void sceDmacTryMemcpy(void){ dmac_copy(1); }

/* --- sceNetInet: el Wii no expone red a la PSP emulada ----------------- */

#define PSP_ENETDOWN 50

static void net_fail(void){ RETURN(0xFFFFFFFFu); }
static void sceNetInetGetErrno(void){ RETURN(PSP_ENETDOWN); }

static const HleFunction rtc[] = {
	{ "sceRtcGetTickResolution", sceRtcGetTickResolution },
	{ "sceRtcGetCurrentTick", sceRtcGetCurrentTick },
	{ "sceRtcGetCurrentClock", sceRtcGetCurrentClock },
	{ "sceRtcGetCurrentClockLocalTime", sceRtcGetCurrentClockLocalTime },
	{ "sceRtcGetTick", sceRtcGetTick },
	{ "sceRtcSetTick", sceRtcSetTick },
	{ "sceRtcCompareTick", sceRtcCompareTick },
	{ "sceRtcTickAddTicks", sceRtcTickAddTicks },
	{ "sceRtcTickAddMicroseconds", sceRtcTickAddMicroseconds },
	{ "sceRtcTickAddSeconds", sceRtcTickAddSeconds },
	{ "sceRtcTickAddMinutes", sceRtcTickAddMinutes },
	{ "sceRtcTickAddHours", sceRtcTickAddHours },
	{ "sceRtcTickAddDays", sceRtcTickAddDays },
	{ "sceRtcIsLeapYear", sceRtcIsLeapYear },
	{ "sceRtcGetDaysInMonth", sceRtcGetDaysInMonth },
	{ "sceRtcGetDayOfWeek", sceRtcGetDayOfWeek },
	{ "sceRtcConvertUtcToLocalTime", sceRtcConvertTick },
	{ "sceRtcConvertLocalTimeToUTC", sceRtcConvertTick },
};

static const HleFunction suspend[] = {
	{ "sceKernelVolatileMemLock", sceKernelVolatileMemLock },
	{ "sceKernelVolatileMemTryLock", sceKernelVolatileMemLock },
	{ "sceKernelVolatileMemUnlock", return_zero },
	{ "sceKernelPowerLock", return_zero },
	{ "sceKernelPowerUnlock", return_zero },
	{ "sceKernelPowerTick", return_zero },
};

static const HleFunction dmac[] = {
	{ "sceDmacMemcpy", sceDmacMemcpy },
	{ "sceDmacTryMemcpy", sceDmacTryMemcpy },
};

static const HleFunction net_inet[] = {
	{ "sceNetInetInit", return_zero },
	{ "sceNetInetTerm", return_zero },
	{ "sceNetInetClose", return_zero },
	{ "sceNetInetSocket", net_fail },
	{ "sceNetInetConnect", net_fail },
	{ "sceNetInetBind", net_fail },
	{ "sceNetInetListen", net_fail },
	{ "sceNetInetAccept", net_fail },
	{ "sceNetInetRecv", net_fail },
	{ "sceNetInetRecvfrom", net_fail },
	{ "sceNetInetSend", net_fail },
	{ "sceNetInetSendto", net_fail },
	{ "sceNetInetSetsockopt", net_fail },
	{ "sceNetInetGetsockopt", net_fail },
	{ "sceNetInetGetErrno", sceNetInetGetErrno },
};

const HleLibrary hle_misc_libs[] = {
	HLE_LIBRARY("sceRtc", rtc),
	HLE_LIBRARY("sceSuspendForUser", suspend),
	HLE_LIBRARY("sceDmac", dmac),
	HLE_LIBRARY("sceNetInet", net_inet),
};
const u32 hle_misc_libs_count = sizeof(hle_misc_libs) / sizeof(hle_misc_libs[0]);
