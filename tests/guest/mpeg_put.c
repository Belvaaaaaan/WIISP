/**
 * WIISP - tests/guest/mpeg_put.c
 * Reproduce cómo leen sus videos GTA LCS y VCS: el callback de
 * sceMpegRingbufferPut no lee él mismo, sino que avisa a UmdStreamThread
 * (prioridad más baja) con un event flag y espera a que termine.
 * Dentro del syscall eso es imposible: el callback tiene que correr como
 * código normal del hilo (PPSSPP hleEnqueueCall).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include "guest.h"

#define PACKETS 16

static SceUID sema, evf;
static int reads, callbacks;
static char rb[64] __attribute__((aligned(64)));
static char mpeg[64] __attribute__((aligned(64)));
static char mpeg_data[0x10000] __attribute__((aligned(64)));
static char rb_data[PACKETS * (2048 + 104)] __attribute__((aligned(64)));

/* Como UmdStreamThread: espera el bit 1, "lee" y pone el bit 2 */
static int umd_stream_thread(int args, void *argp){
	for(;;){
		sceKernelWaitEventFlag(evf, 1, EVF_WAITOR | EVF_WAITCLEAR, 0, 0);
		sceKernelDelayThread(200);   /* la lectura tarda */
		reads++;
		sceKernelSetEventFlag(evf, 2);
	}
	return 0;
}

/* El callback del ringbuffer, como el de GTA */
static int read_callback(void *data, int packets, void *arg){
	int r;
	callbacks++;
	printf("  callback %d: %d paquetes, arg %08X, en el buffer +%d\n", callbacks, packets, (u32)arg,
	       (int)((char *)data - rb_data) / 2048);
	sceKernelWaitSema(sema, 1, 0);
	sceKernelSetEventFlag(evf, 1);
	r = sceKernelWaitEventFlag(evf, 2, EVF_WAITAND | EVF_WAITCLEAR, 0, 0);
	sceKernelSignalSema(sema, 1);
	printf("  WaitEventFlag dentro del callback: %08X (lecturas %d)\n", r, reads);
	return r < 0 ? r : packets;
}

static int mpeg_read_thread(int args, void *argp){
	int r;
	printf("AvailableSize: %d\n", sceMpegRingbufferAvailableSize(rb));
	r = sceMpegRingbufferPut(rb, 10, 10);
	printf("Put 10: %d\n", r);
	/* Quedan 6 hasta el final del buffer: dos rondas, 6 y 4 */
	r = sceMpegRingbufferPut(rb, 10, 10);
	printf("Put 10 (da la vuelta): %d\n", r);
	return sceKernelExitDeleteThread(0);
}

int main(int args, void *argp){
	SceUID umd, rd;
	int r;
	sema = sceKernelCreateSema("UmdStreamSema", 0, 1, 1, 0);
	evf = sceKernelCreateEventFlag("UmdStreamEventFlag", EVF_ATTR_MULTI, 0, 0);
	umd = sceKernelCreateThread("UmdStreamThread", umd_stream_thread, 0x20, 0x4000, 0, 0);
	sceKernelStartThread(umd, 0, 0);

	sceMpegInit();
	r = sceMpegRingbufferConstruct(rb, PACKETS, rb_data, sizeof(rb_data), read_callback, (void *)0x1234);
	printf("RingbufferConstruct: %08X\n", r);
	r = sceMpegCreate(mpeg, mpeg_data, sizeof(mpeg_data), rb, 512, 0, 0);
	printf("Create: %08X\n", r);

	rd = sceKernelCreateThread("MPEGreadThread", mpeg_read_thread, 0x12, 0x4000, 0, 0);
	sceKernelStartThread(rd, 0, 0);
	r = sceKernelWaitThreadEnd(rd, 0);
	printf("Fin del hilo lector: %08X, %d lecturas, %d callbacks\n", r, reads, callbacks);
	sceMpegDelete(mpeg);
	sceMpegFinish();
	return 0;
}
