/**
 * WIISP - net.c
 * sceNet, sceNetAdhoc*, sceWlanDrv y scePower.
 *
 * Red: WIISP no conecta la PSP emulada a ninguna red. El interruptor de
 * WLAN aparece apagado (los juegos no ofrecen el modo multijugador) y las
 * funciones de inicio funcionan para que el juego siga; crear conexiones
 * devuelve error.
 *
 * Energía: la batería siempre llena y enchufada, a 222/111 MHz como una
 * PSP recién encendida.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include "hle/hle.h"
#include "core/memory.h"

static void return_zero(void){ RETURN(0); }

/* --- sceNet ------------------------------------------------------------------ */

static const u8 fake_mac[6] = { 0x00, 0x16, 0xFE, 0x57, 0x11, 0x5E };

static void sceNetGetLocalEtherAddr(void){
	u32 p = ARG(0), i;
	if(!mem_valid(p, 6)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	for(i = 0; i < 6; i++) mem_write8(p + i, fake_mac[i]);
	RETURN(0);
}

static void sceNetEtherNtostr(void){
	u32 mac = ARG(0), out = ARG(1);
	char s[18];
	if(!mem_valid(mac, 6) || !mem_valid(out, 18)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", mem_read8(mac), mem_read8(mac + 1), mem_read8(mac + 2),
	         mem_read8(mac + 3), mem_read8(mac + 4), mem_read8(mac + 5));
	memcpy(mem_ptr(out, 18), s, 18);
	RETURN(0);
}

static void sceNetEtherStrton(void){
	char s[18];
	unsigned v[6];
	u32 i;
	if(mem_read_cstr(ARG(0), s, sizeof(s)) || !mem_valid(ARG(1), 6) ||
	   sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6){
		RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
		return;
	}
	for(i = 0; i < 6; i++) mem_write8(ARG(1) + i, (u8)v[i]);
	RETURN(0);
}

/* Errores de ad hoc: "no hay conexión" */
#define ADHOC_ERROR_NOT_CONNECTED 0x8041070Cu
#define ADHOCCTL_ERROR_WLAN_OFF   0x80410B0Eu

static void adhoc_fail(void){ RETURN(ADHOC_ERROR_NOT_CONNECTED); }
static void adhocctl_fail(void){ RETURN(ADHOCCTL_ERROR_WLAN_OFF); }

static const HleFunction net[] = {
	{ "sceNetInit", return_zero },
	{ "sceNetTerm", return_zero },
	{ "sceNetGetLocalEtherAddr", sceNetGetLocalEtherAddr },
	{ "sceNetEtherNtostr", sceNetEtherNtostr },
	{ "sceNetEtherStrton", sceNetEtherStrton },
	{ "sceNetFreeThreadinfo", return_zero },
	{ "sceNetThreadAbort", return_zero },
};

static const HleFunction net_adhoc[] = {
	{ "sceNetAdhocInit", return_zero },
	{ "sceNetAdhocTerm", return_zero },
	{ "sceNetAdhocPdpCreate", adhoc_fail },
	{ "sceNetAdhocPdpDelete", return_zero },
	{ "sceNetAdhocPdpSend", adhoc_fail },
	{ "sceNetAdhocPdpRecv", adhoc_fail },
	{ "sceNetAdhocGetPdpStat", adhoc_fail },
	{ "sceNetAdhocPtpOpen", adhoc_fail },
	{ "sceNetAdhocPtpListen", adhoc_fail },
	{ "sceNetAdhocPtpAccept", adhoc_fail },
	{ "sceNetAdhocPtpConnect", adhoc_fail },
	{ "sceNetAdhocPtpSend", adhoc_fail },
	{ "sceNetAdhocPtpRecv", adhoc_fail },
	{ "sceNetAdhocPtpFlush", adhoc_fail },
	{ "sceNetAdhocPtpClose", return_zero },
	{ "sceNetAdhocGetPtpStat", adhoc_fail },
	{ "sceNetAdhocPollSocket", adhoc_fail },
};

static const HleFunction net_adhocctl[] = {
	{ "sceNetAdhocctlInit", return_zero },
	{ "sceNetAdhocctlTerm", return_zero },
	{ "sceNetAdhocctlAddHandler", return_zero },
	{ "sceNetAdhocctlDelHandler", return_zero },
	{ "sceNetAdhocctlDisconnect", return_zero },
	{ "sceNetAdhocctlScan", adhocctl_fail },
	{ "sceNetAdhocctlConnect", adhocctl_fail },
	{ "sceNetAdhocctlJoin", adhocctl_fail },
	{ "sceNetAdhocctlCreate", adhocctl_fail },
	{ "sceNetAdhocctlCreateEnterGameMode", adhocctl_fail },
	{ "sceNetAdhocctlJoinEnterGameMode", adhocctl_fail },
	{ "sceNetAdhocctlGetScanInfo", adhocctl_fail },
	{ "sceNetAdhocctlGetNameByAddr", adhocctl_fail },
	{ "sceNetAdhocctlGetPeerList", adhocctl_fail },
	{ "sceNetAdhocctlGetState", return_zero },
};

static const HleFunction net_adhoc_matching[] = {
	{ "sceNetAdhocMatchingInit", return_zero },
	{ "sceNetAdhocMatchingTerm", return_zero },
	{ "sceNetAdhocMatchingCreate", adhoc_fail },
	{ "sceNetAdhocMatchingStart", adhoc_fail },
	{ "sceNetAdhocMatchingStop", return_zero },
	{ "sceNetAdhocMatchingDelete", return_zero },
	{ "sceNetAdhocMatchingSelectTarget", adhoc_fail },
	{ "sceNetAdhocMatchingCancelTarget", adhoc_fail },
	{ "sceNetAdhocMatchingSetHelloOpt", adhoc_fail },
};

/* --- sceWlanDrv: interruptor apagado ------------------------------------------- */

static void sceWlanGetEtherAddr(void){ sceNetGetLocalEtherAddr(); }

static const HleFunction wlan[] = {
	{ "sceWlanGetSwitchState", return_zero },
	{ "sceWlanDevIsPowerOn", return_zero },
	{ "sceWlanGetEtherAddr", sceWlanGetEtherAddr },
};

/* --- scePower ---------------------------------------------------------------- */

static u32 cpu_mhz = 222, bus_mhz = 111;

static void return_float(float f){ cpu.fpr[0].f = f; RETURN(0); }

/* Callbacks de energía: 16 ranuras del usuario. Al registrar uno se le
   avisa en el acto del estado (con cargador y batería llena), como en
   PPSSPP scePower.cpp. */
#define POWER_SLOTS 16
#define POWER_SLOTS_PRIVATE 32
static u32 power_cb[POWER_SLOTS];

void power_init(void){ memset(power_cb, 0, sizeof(power_cb)); }

static void scePowerRegisterCallback(void){
	s32 slot = (s32)ARG(0);
	u32 cb = ARG(1);
	int i;
	if(slot < -1 || slot >= POWER_SLOTS_PRIVATE){ RETURN(0x80000102u); return; }   /* INVALID_SLOT */
	if(slot >= POWER_SLOTS){ RETURN(0x80000023u); return; }                         /* PRIV_REQUIRED */
	if(!cb){ RETURN(0x80000100u); return; }                                          /* INVALID_CB */
	if(slot == -1){
		for(i = 0; i < POWER_SLOTS && power_cb[i]; i++);
		if(i == POWER_SLOTS){ RETURN(0x80000022u); return; }                         /* SLOTS_FULL */
		slot = i;
		RETURN((u32)i);
	} else {
		if(power_cb[slot]){ RETURN(0x80000020u); return; }                          /* TAKEN_SLOT */
		RETURN(0);
	}
	power_cb[slot] = cb;
	kernel_notify_callback(cb, 0x00001000 | 0x00000080 | 0x00000064);   /* AC, batería, llena */
}

static void scePowerUnregisterCallback(void){
	s32 slot = (s32)ARG(0);
	if(slot < 0 || slot >= POWER_SLOTS_PRIVATE){ RETURN(0x80000102u); return; }
	if(slot >= POWER_SLOTS){ RETURN(0x80000023u); return; }
	if(!power_cb[slot]){ RETURN(0x80000025u); return; }                             /* EMPTY_SLOT */
	power_cb[slot] = 0;
	RETURN(0);
}

static void scePowerSetClockFrequency(void){
	if(ARG(1) >= 1 && ARG(1) <= 333) cpu_mhz = ARG(1);
	if(ARG(2) >= 1 && ARG(2) <= 166) bus_mhz = ARG(2);
	RETURN(0);
}

static void scePowerSetCpuClockFrequency(void){ if(ARG(0) >= 1 && ARG(0) <= 333) cpu_mhz = ARG(0); RETURN(0); }
static void scePowerSetBusClockFrequency(void){ if(ARG(0) >= 1 && ARG(0) <= 166) bus_mhz = ARG(0); RETURN(0); }
static void scePowerGetCpuClockFrequency(void){ RETURN(cpu_mhz); }
static void scePowerGetBusClockFrequency(void){ RETURN(bus_mhz); }
static void scePowerGetPllClockFrequencyInt(void){ RETURN(bus_mhz * 2); }
static void scePowerGetCpuClockFrequencyFloat(void){ return_float((float)cpu_mhz); }
static void scePowerGetBusClockFrequencyFloat(void){ return_float((float)bus_mhz); }
static void scePowerGetPllClockFrequencyFloat(void){ return_float((float)(bus_mhz * 2)); }
static void return_one(void){ RETURN(1); }
static void return_100(void){ RETURN(100); }
static void scePowerGetBatteryLifeTime(void){ RETURN(5 * 60); }   /* minutos */
static void scePowerGetBatteryTemp(void){ RETURN(28); }
static void scePowerGetBatteryVolt(void){ RETURN(4135); }

static const HleFunction power[] = {
	{ "scePowerRegisterCallback", scePowerRegisterCallback },
	{ "scePowerUnregisterCallback", scePowerUnregisterCallback },
	{ "scePowerUnregitserCallback", scePowerUnregisterCallback },
	{ "scePowerSetClockFrequency", scePowerSetClockFrequency },
	{ "scePowerSetCpuClockFrequency", scePowerSetCpuClockFrequency },
	{ "scePowerSetBusClockFrequency", scePowerSetBusClockFrequency },
	{ "scePowerGetCpuClockFrequency", scePowerGetCpuClockFrequency },
	{ "scePowerGetCpuClockFrequencyInt", scePowerGetCpuClockFrequency },
	{ "scePowerGetBusClockFrequency", scePowerGetBusClockFrequency },
	{ "scePowerGetBusClockFrequencyInt", scePowerGetBusClockFrequency },
	{ "scePowerGetPllClockFrequencyInt", scePowerGetPllClockFrequencyInt },
	{ "scePowerGetCpuClockFrequencyFloat", scePowerGetCpuClockFrequencyFloat },
	{ "scePowerGetBusClockFrequencyFloat", scePowerGetBusClockFrequencyFloat },
	{ "scePowerGetPllClockFrequencyFloat", scePowerGetPllClockFrequencyFloat },
	{ "scePowerIsPowerOnline", return_one },
	{ "scePowerIsBatteryExist", return_one },
	{ "scePowerIsBatteryCharging", return_zero },
	{ "scePowerIsLowBattery", return_zero },
	{ "scePowerGetBatteryLifePercent", return_100 },
	{ "scePowerGetBatteryLifeTime", scePowerGetBatteryLifeTime },
	{ "scePowerGetBatteryTemp", scePowerGetBatteryTemp },
	{ "scePowerGetBatteryVolt", scePowerGetBatteryVolt },
	{ "scePowerTick", return_zero },
	{ "scePowerLock", return_zero },
	{ "scePowerUnlock", return_zero },
};

const HleLibrary hle_net_libs[] = {
	HLE_LIBRARY("sceNet", net),
	HLE_LIBRARY("sceNetAdhoc", net_adhoc),
	HLE_LIBRARY("sceNetAdhocctl", net_adhocctl),
	HLE_LIBRARY("sceNetAdhocMatching", net_adhoc_matching),
	HLE_LIBRARY("sceWlanDrv", wlan),
	HLE_LIBRARY("scePower", power),
};
const u32 hle_net_libs_count = sizeof(hle_net_libs) / sizeof(hle_net_libs[0]);
