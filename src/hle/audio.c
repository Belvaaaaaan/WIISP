/**
 * WIISP - audio.c
 * HLE de sceAudio: los 8 canales del mezclador y el canal SRC.
 *
 * Sigue el modelo de PPSSPP (Core/HLE/sceAudio.cpp y __sceAudio.cpp,
 * GPLv2+): cada canal tiene un único búfer en curso; el mezclador del
 * hardware consume 64 muestras cada 64/44100 s mientras algo suena. Una
 * salida bloqueante con el canal ocupado deja al hilo esperando hasta que
 * su búfer entra al acabar el anterior: así es como los juegos llevan el
 * ritmo. La mezcla de cada bloque va a hle_audio_sink, si la plataforma
 * pone uno (si no, el sonido se descarta pero los tiempos son los reales).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include "hle/hle.h"
#include "core/memory.h"
#include "core/prof.h"

#define NUM_CHANNELS 8
#define HW_RATE 44100
#define HW_BLOCK 64
#define SAMPLE_MAX 0xFFC0

#define ERR_CHANNEL_NOT_INIT     0x80260001u
#define ERR_CHANNEL_BUSY         0x80260002u
#define ERR_INVALID_CHANNEL      0x80260003u
#define ERR_NO_CHANNELS          0x80260005u
#define ERR_SIZE_NOT_ALIGNED     0x80260006u
#define ERR_INVALID_FORMAT       0x80260007u
#define ERR_CHANNEL_NOT_RESERVED 0x80260008u
#define ERR_INVALID_FREQUENCY    0x8026000Au
#define ERR_INVALID_VOLUME       0x8026000Bu
#define ERR_ALREADY_RESERVED     0x80268002u
#define ERR_INVALID_SIZE         0x80000104u
#define ERR_SRC_FORMAT_4         0x80000003u
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT 0x80020064u
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT    0x800201A7u

#define FORMAT_STEREO 0x00
#define FORMAT_MONO   0x10

typedef struct {
	int reserved;
	u32 sample_count, format;
	int lvol, rvol;
	u32 addr, remaining;          /* búfer en curso */
	int waiting;                  /* hay un hilo esperando (salida bloqueante) */
	u32 wait_addr;
	int wait_lvol, wait_rvol;
} Channel;

/* Canal SRC (sceAudioSRC* / sceAudioOutput2*): dos búferes en cola */
typedef struct {
	int reserved;
	u32 sample_count, format, freq;
	int lvol, rvol;
	u32 buf[2], count;
	u32 played, frac;
	int waiting;
} SrcChannel;

static Channel chans[NUM_CHANNELS];
static SrcChannel src;
static int mixer_running;
static s32 mix[HW_BLOCK * 2];

HleAudioSink hle_audio_sink;

static u64 block_cycles(void){ return (u64)CYCLES_PER_US * 1000000ull * HW_BLOCK / HW_RATE; }

static inline s32 apply_volume(s16 sample, int vol){ return ((s32)sample * vol) >> 15; }

static int channel_playing(const Channel *c){ return c->addr != 0; }

static int any_playing(void){
	int i;
	if(src.count) return 1;
	for(i = 0; i < NUM_CHANNELS; i++) if(channel_playing(&chans[i])) return 1;
	return 0;
}

static void mixer_tick(u64 userdata);

/* Arrancar el DMA mezcla el primer bloque en el acto (audio/blocking/restlen) */
static void start_mixer(void){
	if(mixer_running) return;
	mixer_running = 1;
	mixer_tick(0);
}

/* Entrega un búfer al canal. Devuelve el número de muestras o un error */
static u32 enqueue(Channel *c, u32 addr, int lvol, int rvol){
	if(!c->reserved) return ERR_CHANNEL_NOT_INIT;
	if(channel_playing(c)) return ERR_CHANNEL_BUSY;
	c->remaining = c->sample_count;
	if(lvol >= 0) c->lvol = lvol;
	if(rvol >= 0) c->rvol = rvol;
	c->addr = addr;
	if(addr) start_mixer();
	return c->sample_count;
}

static void channel_finished(int ch){
	Channel *c = &chans[ch];
	c->addr = 0;
	c->remaining = 0;
	if(!c->waiting) return;
	c->waiting = 0;
	kernel_wake_object(KWAIT_AUDIO, (u32)ch, enqueue(c, c->wait_addr, c->wait_lvol, c->wait_rvol));
}

static void mix_channel(int ch){
	Channel *c = &chans[ch];
	u32 count, stride, s;
	const u8 *p;
	if(!c->addr) return;
	if(c->remaining == 0){ channel_finished(ch); return; }
	count = c->remaining < HW_BLOCK ? c->remaining : HW_BLOCK;
	stride = c->format == FORMAT_MONO ? 2 : 4;
	p = mem_ptr_r(c->addr, count * stride);
	if(p && hle_audio_sink){
		for(s = 0; s < count; s++){
			s16 l = (s16)rd_le16(p + s * stride), r = c->format == FORMAT_MONO ? l : (s16)rd_le16(p + s * stride + 2);
			mix[s * 2] += apply_volume(l, c->lvol);
			mix[s * 2 + 1] += apply_volume(r, c->rvol);
		}
	}
	c->addr += count * stride;
	c->remaining -= count;
	if(c->remaining == 0) channel_finished(ch);
}

static void mix_src(void){
	u32 out, stride = src.format == FORMAT_MONO ? 2 : 4;
	u32 rate = src.freq ? src.freq : HW_RATE;
	u32 ratio = (u32)(((u64)rate << 16) / HW_RATE);
	for(out = 0; out < HW_BLOCK && src.count; out++){
		const u8 *p = mem_ptr_r(src.buf[0] + src.played * stride, stride);
		u32 step;
		if(p && hle_audio_sink){
			s16 l = (s16)rd_le16(p), r = src.format == FORMAT_MONO ? l : (s16)rd_le16(p + 2);
			mix[out * 2] += apply_volume(l, src.lvol);
			mix[out * 2 + 1] += apply_volume(r, src.rvol);
		}
		src.frac += ratio;
		step = src.frac >> 16;
		src.frac &= 0xFFFF;
		while(step && src.count){
			u32 take = src.sample_count - src.played < step ? src.sample_count - src.played : step;
			src.played += take;
			step -= take;
			if(src.played >= src.sample_count){
				src.buf[0] = src.buf[1];
				src.count--;
				src.played = 0;
				if(src.waiting){
					src.waiting = 0;
					kernel_wake_object(KWAIT_AUDIO, NUM_CHANNELS, 0);
				}
			}
		}
	}
}

static void mixer_tick(u64 userdata){
	int i, old = prof_switch(PROF_AUDIO);
	(void)userdata;
	memset(mix, 0, sizeof(mix));
	for(i = 0; i < NUM_CHANNELS; i++) mix_channel(i);
	mix_src();
	if(hle_audio_sink){
		s16 out[HW_BLOCK * 2];
		for(i = 0; i < HW_BLOCK * 2; i++) out[i] = (s16)(mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
		hle_audio_sink(out, HW_BLOCK);
	}
	if(any_playing()) kernel_schedule_event(cpu_cycles + block_cycles(), mixer_tick, 0);
	else mixer_running = 0;
	prof_switch(old);
}

void audio_init(void){
	memset(chans, 0, sizeof(chans));
	memset(&src, 0, sizeof(src));
	mixer_running = 0;
}

/* --- Llamadas -------------------------------------------------------------------- */

static int bad_volume(u32 v){ return v > 0xFFFF && (s32)v >= 0; }

static void output(int blocking, int panned){
	u32 ch = ARG(0), addr = panned ? ARG(3) : ARG(2);
	int lvol = (int)ARG(1), rvol = panned ? (int)ARG(2) : lvol;
	Channel *c;
	u32 r;
	if(bad_volume((u32)lvol) || bad_volume((u32)rvol) || (panned ? 0 : (u32)lvol > 0xFFFF)){ RETURN(ERR_INVALID_VOLUME); return; }
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	c = &chans[ch];
	r = enqueue(c, addr, lvol, rvol);
	if(r != ERR_CHANNEL_BUSY || !blocking){ RETURN(r); return; }
	/* Un solo hilo puede esperar en cada canal */
	if(c->waiting){ RETURN(ERR_CHANNEL_BUSY); return; }
	if(kernel_in_interrupt()){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT); return; }
	if(!kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
	c->waiting = 1;
	c->wait_addr = addr;
	c->wait_lvol = lvol;
	c->wait_rvol = rvol;
	kernel_wait_object(KWAIT_AUDIO, ch);
}

static void sceAudioOutput(void){ output(0, 0); }
static void sceAudioOutputBlocking(void){ output(1, 0); }
static void sceAudioOutputPanned(void){ output(0, 1); }
static void sceAudioOutputPannedBlocking(void){ output(1, 1); }

static void sceAudioChReserve(void){
	s32 ch = (s32)ARG(0);
	u32 count = ARG(1), format = ARG(2);
	if(ch < 0){
		for(ch = NUM_CHANNELS - 1; ch >= 0; ch--)
			if(chans[ch].sample_count == 0 && !chans[ch].addr) break;
		if(ch < 0){ RETURN(ERR_NO_CHANNELS); return; }
	}
	if(ch >= NUM_CHANNELS || chans[ch].reserved){ RETURN(ERR_INVALID_CHANNEL); return; }
	if((count & 63) || count == 0 || count > SAMPLE_MAX){ RETURN(ERR_SIZE_NOT_ALIGNED); return; }
	if(format != FORMAT_MONO && format != FORMAT_STEREO){ RETURN(ERR_INVALID_FORMAT); return; }
	chans[ch].sample_count = count;
	chans[ch].format = format;
	chans[ch].reserved = 1;
	chans[ch].lvol = chans[ch].rvol = 0;
	RETURN((u32)ch);
}

static void sceAudioChRelease(void){
	u32 ch = ARG(0);
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	if(!chans[ch].reserved){ RETURN(ERR_CHANNEL_NOT_RESERVED); return; }
	if(chans[ch].waiting){ RETURN(ERR_CHANNEL_BUSY); return; }
	chans[ch].reserved = 0;
	chans[ch].sample_count = 0;   /* lo que suena acaba de sonar */
	RETURN(0);
}

static void sceAudioSetChannelDataLen(void){
	u32 ch = ARG(0), len = ARG(1);
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	if((len & 63) || len == 0 || len > SAMPLE_MAX){ RETURN(ERR_SIZE_NOT_ALIGNED); return; }
	if(chans[ch].waiting){ RETURN(ERR_CHANNEL_BUSY); return; }
	if(!chans[ch].reserved){ RETURN(ERR_CHANNEL_NOT_INIT); return; }
	chans[ch].sample_count = len;
	RETURN(0);
}

static void sceAudioChangeChannelConfig(void){
	u32 ch = ARG(0), format = ARG(1);
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	if(channel_playing(&chans[ch])){ RETURN(ERR_CHANNEL_BUSY); return; }
	if(!chans[ch].reserved){ RETURN(ERR_CHANNEL_NOT_RESERVED); return; }
	if(format != FORMAT_MONO && format != FORMAT_STEREO){ RETURN(ERR_INVALID_FORMAT); return; }
	chans[ch].format = format;
	RETURN(0);
}

static void sceAudioChangeChannelVolume(void){
	u32 ch = ARG(0);
	s32 l = (s32)ARG(1), r = (s32)ARG(2);
	if(l > 0xFFFF || r > 0xFFFF){ RETURN(ERR_INVALID_VOLUME); return; }
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	if(l >= 0) chans[ch].lvol = l;
	if(r >= 0) chans[ch].rvol = r;
	RETURN(0);
}

/* Lo que queda del búfer en curso, más un búfer entero si hay un hilo esperando */
static void sceAudioGetChannelRestLen(void){
	u32 ch = ARG(0);
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	RETURN(chans[ch].remaining + (chans[ch].waiting ? chans[ch].sample_count : 0));
}

static void sceAudioGetChannelRestLength(void){
	u32 ch = ARG(0);
	if(ch >= NUM_CHANNELS){ RETURN(ERR_INVALID_CHANNEL); return; }
	RETURN((chans[ch].addr ? chans[ch].remaining : 0) + (chans[ch].waiting ? chans[ch].sample_count : 0));
}

static void sceAudioOneshotOutput(void){
	s32 ch = (s32)ARG(0), count = (s32)ARG(1);
	u32 format = ARG(2), addr = ARG(5);
	s32 l = (s32)ARG(3), r = (s32)ARG(4);
	Channel *c;
	if((u32)l > 0xFFFF || (u32)r > 0xFFFF){ RETURN(ERR_INVALID_VOLUME); return; }
	if(ch < 0){
		for(ch = NUM_CHANNELS - 1; ch >= 0; ch--)
			if(chans[ch].sample_count == 0 && !chans[ch].addr) break;
		if(ch < 0){ RETURN(ERR_NO_CHANNELS); return; }
	} else if(ch >= NUM_CHANNELS || chans[ch].reserved){ RETURN(ERR_INVALID_CHANNEL); return; }
	if(count <= 0){ RETURN(ERR_SIZE_NOT_ALIGNED); return; }
	if(format != FORMAT_MONO && format != FORMAT_STEREO){ RETURN(ERR_INVALID_FORMAT); return; }
	c = &chans[ch];
	c->format = format;
	c->lvol = l;
	c->rvol = r;
	c->remaining = (u32)count;
	c->addr = addr;
	if(addr) start_mixer();
	RETURN((u32)ch);
}

/* --- Canal SRC ----------------------------------------------------------------- */

static int src_rate_ok(u32 f){
	return f == 0 || f == 44100 || f == 22050 || f == 11025 || f == 48000 || f == 32000 ||
	       f == 24000 || f == 16000 || f == 12000 || f == 8000;
}

static void sceAudioSRCChReserve(void){
	u32 count = ARG(0) & 0x7FFFFFFFu, freq = ARG(1), format = ARG(2);
	if(format == 4){ RETURN(ERR_SRC_FORMAT_4); return; }
	if(format != 2 || count < 17 || count > 4111){ RETURN(ERR_INVALID_SIZE); return; }
	if(!src_rate_ok(freq)){ RETURN(ERR_INVALID_FREQUENCY); return; }
	if(src.reserved){ RETURN(ERR_ALREADY_RESERVED); return; }
	memset(&src, 0, sizeof(src));
	src.reserved = 1;
	src.sample_count = count;
	src.freq = freq;
	src.format = FORMAT_STEREO;
	RETURN(0);
}

static void sceAudioSRCChRelease(void){
	if(!src.reserved){ RETURN(ERR_CHANNEL_NOT_RESERVED); return; }
	if(src.count){ RETURN(ERR_ALREADY_RESERVED); return; }
	src.reserved = 0;
	RETURN(0);
}

static void src_output(int blocking, int vol){
	u32 addr = ARG(1);
	if((u32)vol > 0xFFFFF){ RETURN(ERR_INVALID_VOLUME); return; }
	if(!src.reserved){ RETURN(ERR_CHANNEL_NOT_RESERVED); return; }
	if(src.count >= 2){
		if(!blocking){ RETURN(ERR_CHANNEL_BUSY); return; }
		src.waiting = 1;
		kernel_wait_object(KWAIT_AUDIO, NUM_CHANNELS);
		return;
	}
	if(addr){
		if(vol >= 0){ src.lvol = vol; src.rvol = vol; }
		if(src.count == 0){ src.played = 0; src.frac = 0; }
		src.buf[src.count++] = addr;
		start_mixer();
	}
	RETURN(0);
	kernel_eat_cycles(src.count >= 2 ? 10000 : 25000);
}

static void sceAudioSRCOutputBlocking(void){ src_output(1, (int)ARG(0)); }

static void sceAudioOutput2Reserve(void){
	u32 count = ARG(0) & 0x7FFFFFFFu;
	if(count < 17 || count > 4111){ RETURN(ERR_INVALID_SIZE); return; }
	if(src.reserved){ RETURN(ERR_ALREADY_RESERVED); return; }
	memset(&src, 0, sizeof(src));
	src.reserved = 1;
	src.sample_count = count;
	src.freq = 0;
	src.format = FORMAT_STEREO;
	RETURN(0);
}

static void sceAudioOutput2ChangeLength(void){
	u32 count = ARG(0);
	if(count - 17 >= 0xFFF){ RETURN(ERR_SIZE_NOT_ALIGNED); return; }
	if(!src.reserved){ RETURN(ERR_CHANNEL_NOT_RESERVED); return; }
	src.sample_count = count;
	RETURN(0);
}

static void sceAudioOutput2GetRestSample(void){
	if(!src.reserved){ RETURN(ERR_CHANNEL_NOT_RESERVED); return; }
	RETURN(src.count * src.sample_count);
}

static void sceAudioInit(void){ RETURN(0); }

static const HleFunction audio_funcs[] = {
	{ "sceAudioOutput", sceAudioOutput },
	{ "sceAudioOutputBlocking", sceAudioOutputBlocking },
	{ "sceAudioOutputPanned", sceAudioOutputPanned },
	{ "sceAudioOutputPannedBlocking", sceAudioOutputPannedBlocking },
	{ "sceAudioChReserve", sceAudioChReserve },
	{ "sceAudioChRelease", sceAudioChRelease },
	{ "sceAudioSetChannelDataLen", sceAudioSetChannelDataLen },
	{ "sceAudioChangeChannelConfig", sceAudioChangeChannelConfig },
	{ "sceAudioChangeChannelVolume", sceAudioChangeChannelVolume },
	{ "sceAudioGetChannelRestLen", sceAudioGetChannelRestLen },
	{ "sceAudioGetChannelRestLength", sceAudioGetChannelRestLength },
	{ "sceAudioOneshotOutput", sceAudioOneshotOutput },
	{ "sceAudioSRCChReserve", sceAudioSRCChReserve },
	{ "sceAudioSRCChRelease", sceAudioSRCChRelease },
	{ "sceAudioSRCOutputBlocking", sceAudioSRCOutputBlocking },
	{ "sceAudioOutput2Reserve", sceAudioOutput2Reserve },
	{ "sceAudioOutput2Release", sceAudioSRCChRelease },
	{ "sceAudioOutput2OutputBlocking", sceAudioSRCOutputBlocking },
	{ "sceAudioOutput2ChangeLength", sceAudioOutput2ChangeLength },
	{ "sceAudioOutput2GetRestSample", sceAudioOutput2GetRestSample },
	{ "sceAudioInit", sceAudioInit },
	{ "sceAudioEnd", sceAudioInit },
};

const HleLibrary hle_audio_libs[] = {
	HLE_LIBRARY("sceAudio", audio_funcs),
};
const u32 hle_audio_libs_count = sizeof(hle_audio_libs) / sizeof(hle_audio_libs[0]);
