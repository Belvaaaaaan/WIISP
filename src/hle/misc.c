/**
 * WIISP - misc.c
 * HLE de módulos pequeños: sceRtc (reloj), sceSuspendForUser, sceUtility
 * (parámetros del sistema y carga de módulos), sceDmac (copias DMA) y
 * sceNetInet (sin red).
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

#define PSP_SYSTEMPARAM_ID_INT_LANGUAGE        8
#define PSP_SYSTEMPARAM_ID_INT_UNKNOWN         9   /* botón de confirmar */
#define SCE_UTILITY_ERROR_INVALID_ID           0x80110103u

static void sceUtilityGetSystemParamInt(void){
	u32 id = ARG(0), out = ARG(1), value;
	switch(id){
	case 2: case 3: case 4: case 5: case 6: case 7: value = 0; break;
	case PSP_SYSTEMPARAM_ID_INT_LANGUAGE: value = 1; break; /* inglés */
	case PSP_SYSTEMPARAM_ID_INT_UNKNOWN: value = 1; break;  /* X confirma */
	default: RETURN(SCE_UTILITY_ERROR_INVALID_ID); return;
	}
	if(out) mem_write32(out, value);
	RETURN(0);
}

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
};

static const HleFunction suspend[] = {
	{ "sceKernelVolatileMemLock", sceKernelVolatileMemLock },
	{ "sceKernelVolatileMemTryLock", sceKernelVolatileMemLock },
	{ "sceKernelVolatileMemUnlock", return_zero },
	{ "sceKernelPowerLock", return_zero },
	{ "sceKernelPowerUnlock", return_zero },
	{ "sceKernelPowerTick", return_zero },
};

static const HleFunction utility[] = {
	{ "sceUtilityGetSystemParamInt", sceUtilityGetSystemParamInt },
	{ "sceUtilityLoadModule", return_zero },
	{ "sceUtilityUnloadModule", return_zero },
	{ "sceUtilityLoadNetModule", return_zero },
	{ "sceUtilityUnloadNetModule", return_zero },
	{ "sceUtilityLoadAvModule", return_zero },
	{ "sceUtilityUnloadAvModule", return_zero },
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
	HLE_LIBRARY("sceUtility", utility),
	HLE_LIBRARY("sceDmac", dmac),
	HLE_LIBRARY("sceNetInet", net_inet),
};
const u32 hle_misc_libs_count = sizeof(hle_misc_libs) / sizeof(hle_misc_libs[0]);
