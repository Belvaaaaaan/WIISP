/**
 * WIISP - tests/guest/guest.h
 * Lo mínimo para escribir programas de PSP de prueba en C (sin PSPSDK):
 * las funciones del firmware de imports.txt y un printf a stdout.
 * Se compilan con el gcc MIPS de Linux en modo EABI (los argumentos 5-8 van
 * en t0-t3, como en la PSP).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#ifndef WIISP_GUEST_H
#define WIISP_GUEST_H

typedef unsigned int u32;
typedef int SceUID;
typedef int (*ThreadEntry)(int args, void *argp);

SceUID sceKernelCreateThread(const char *name, ThreadEntry entry, int prio, int stack, u32 attr, void *opt);
int sceKernelStartThread(SceUID th, int args, void *argp);
int sceKernelExitDeleteThread(int status);
int sceKernelWaitThreadEnd(SceUID th, u32 *timeout);
int sceKernelDelayThread(u32 us);
SceUID sceKernelCreateSema(const char *name, u32 attr, int init, int max, void *opt);
int sceKernelWaitSema(SceUID s, int n, u32 *timeout);
int sceKernelSignalSema(SceUID s, int n);
SceUID sceKernelCreateEventFlag(const char *name, u32 attr, u32 bits, void *opt);
int sceKernelSetEventFlag(SceUID e, u32 bits);
int sceKernelWaitEventFlag(SceUID e, u32 bits, u32 mode, u32 *out, u32 *timeout);
u32 sceKernelGetSystemTimeLow(void);
SceUID sceKernelCreateCallback(const char *name, int (*f)(int, int, void *), void *arg);
int sceKernelNotifyCallback(SceUID cb, int arg);
int sceKernelSleepThreadCB(void);
int sceKernelWakeupThread(SceUID th);
int sceKernelExtendThreadStack(int size, int (*entry)(void *), void *arg);
void *sceKernelMemset(void *dst, int c, u32 n);
int sceKernelCheckThreadStack(void);
void *sceKernelMemcpy(void *dst, const void *src, u32 n);
SceUID sceKernelAllocPartitionMemory(int part, const char *name, int type, u32 size, void *addr);
int sceKernelFreePartitionMemory(SceUID b);
void *sceKernelGetBlockHeadAddr(SceUID b);
u32 sceKernelMaxFreeMemSize(void);
u32 sceKernelTotalFreeMemSize(void);
int sceIoWrite(int fd, const void *buf, u32 len);
void sceKernelExitGame(void);

int sceMpegInit(void);
int sceMpegFinish(void);
int sceMpegQueryMemSize(int mode);
int sceMpegRingbufferQueryMemSize(int packets);
int sceMpegRingbufferConstruct(void *rb, int packets, void *data, int size, int (*cb)(void *, int, void *), void *arg);
int sceMpegRingbufferDestruct(void *rb);
int sceMpegCreate(void *mpeg, void *data, int size, void *rb, int width, int mode, int ddrtop);
int sceMpegDelete(void *mpeg);
int sceMpegRingbufferPut(void *rb, int packets, int avail);
int sceMpegRingbufferAvailableSize(void *rb);

#define EVF_WAITAND   0x00
#define EVF_WAITOR    0x01
#define EVF_WAITCLEAR 0x20
#define EVF_ATTR_MULTI 0x200

/* printf mínimo: %d %u %x %X %08x %s %c */
static void out(const char *s, u32 n){ sceIoWrite(1, s, n); }
static void printf_(const char *fmt, ...);

#include <stdarg.h>
static void printf_(const char *fmt, ...){
	char buf[256];
	u32 n = 0;
	va_list ap;
	va_start(ap, fmt);
	for(; *fmt && n < sizeof(buf) - 16; fmt++){
		int width = 0, zero = 0;
		if(*fmt != '%'){ buf[n++] = *fmt; continue; }
		fmt++;
		if(*fmt == '0'){ zero = 1; fmt++; }
		while(*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		if(*fmt == 's'){
			const char *s = va_arg(ap, const char *);
			while(*s && n < sizeof(buf) - 16) buf[n++] = *s++;
		} else if(*fmt == 'c'){
			buf[n++] = (char)va_arg(ap, int);
		} else {
			char tmp[12];
			int t = 0, neg = 0;
			u32 v = va_arg(ap, u32), base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
			if(*fmt == 'd' && (int)v < 0){ neg = 1; v = -v; }
			do { tmp[t++] = "0123456789ABCDEF"[v % base] | (*fmt == 'x' ? 0x20 : 0); v /= base; } while(v);
			if(neg) buf[n++] = '-';
			while(t < width) tmp[t++] = zero ? '0' : ' ';
			while(t) buf[n++] = tmp[--t];
		}
	}
	va_end(ap);
	out(buf, n);
}
#define printf printf_

#endif
