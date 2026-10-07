/**
 * WIISP - sas.c
 * HLE de sceSasCore: el mezclador de voces que la PSP ejecuta en el Media
 * Engine (efectos de sonido de casi todos los juegos).
 *
 * Portado de PPSSPP (Core/HLE/sceSas.cpp y Core/HW/SasAudio.cpp,
 * (c) 2012- PPSSPP Project, GPLv2+): 32 voces VAG (ADPCM de Sony), PCM o
 * ruido, con tono por interpolación lineal, envolvente ADSR y volúmenes;
 * __sceSasCore mezcla un "grano" de muestras en el búfer del juego. La
 * reverberación (efecto "wet") y las voces ATRAC3 aún no suenan.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include "hle/hle.h"
#include "core/memory.h"

#define VOICES_MAX 32
#define VOL_MAX 0x1000
#define MAX_GRAIN 2048
#define PITCH_BASE 0x1000
#define PITCH_MASK 0xFFF
#define PITCH_SHIFT 12
#define PITCH_MAX 0x4000
#define ENV_HEIGHT_MAX 0x40000000

#define ERR_INVALID_GRAIN        0x80420001u
#define ERR_INVALID_MAX_VOICES   0x80420002u
#define ERR_INVALID_OUTPUT_MODE  0x80420003u
#define ERR_INVALID_SAMPLE_RATE  0x80420004u
#define ERR_BAD_ADDRESS          0x80420005u
#define ERR_INVALID_VOICE        0x80420010u
#define ERR_INVALID_NOISE_FREQ   0x80420011u
#define ERR_INVALID_PITCH        0x80420012u
#define ERR_INVALID_ADSR_MODE    0x80420013u
#define ERR_INVALID_PARAMETER    0x80420014u
#define ERR_INVALID_LOOP_POS     0x80420015u
#define ERR_VOICE_PAUSED         0x80420016u
#define ERR_INVALID_VOLUME       0x80420018u
#define ERR_INVALID_ADSR_RATE    0x80420019u
#define ERR_INVALID_PCM_SIZE     0x8042001Au
#define ERR_REV_INVALID_TYPE     0x80420020u
#define ERR_REV_INVALID_FEEDBACK 0x80420021u
#define ERR_REV_INVALID_DELAY    0x80420022u
#define ERR_REV_INVALID_VOLUME   0x80420023u
#define ERR_ATRAC3_ALREADY_SET   0x80420040u
#define ERR_ATRAC3_NOT_SET       0x80420041u
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT 0x800201A7u
#define SCE_KERNEL_ERROR_BAD_ARGUMENT 0x80000004u

enum { CURVE_LINEAR_INC, CURVE_LINEAR_DEC, CURVE_LINEAR_BENT, CURVE_EXP_DEC, CURVE_EXP_INC, CURVE_DIRECT };
enum { ST_KEYON_STEP = -42, ST_KEYON = -2, ST_OFF = -1, ST_ATTACK = 0, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
enum { VT_OFF, VT_VAG, VT_NOISE, VT_TRIWAVE, VT_PULSEWAVE, VT_PCM, VT_ATRAC3 };

typedef struct {
	int attack_rate, decay_rate, sustain_rate, sustain_level, release_rate;
	int attack_type, decay_type, sustain_type, release_type;
	int state;
	s64 height;
} Envelope;

typedef struct {
	s16 samples[28];
	int cur_sample;
	u32 data, read;
	int cur_block, loop_start_block, num_blocks;
	int s1, s2;
	int loop_enabled, loop_at_next_block, end;
} VagDecoder;

typedef struct {
	int playing, paused, on;
	int type;
	u32 vag_addr, vag_size;
	u32 pcm_addr;
	int pcm_size, pcm_index, pcm_loop_pos;
	u32 sample_frac;
	int pitch, loop, noise_freq;
	int vol_l, vol_r, eff_l, eff_r;
	s16 hist[2];
	Envelope env;
	VagDecoder vag;
} Voice;

static struct {
	int grain, output_mode;
	int effect_type, effect_delay, effect_feedback, effect_l, effect_r, dry_on, wet_on;
	s32 mix[MAX_GRAIN * 2], send[MAX_GRAIN * 2];
	s16 temp[MAX_GRAIN * 4 + 2 + 16];
	Voice voices[VOICES_MAX];
} sas;

static inline s16 clamp16(s32 v){ return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* --- VAG ------------------------------------------------------------------------ */

static const u8 vag_f[16][2] = {
	{ 0, 0 }, { 60, 0 }, { 115, 52 }, { 98, 55 }, { 122, 60 },
	/* Desde el 5, el hardware lee fuera de su tabla (PPSSPP) */
	{ 0, 0 }, { 0, 0 }, { 52, 0 }, { 55, 2 }, { 60, 125 }, { 0, 0 }, { 0, 91 }, { 0, 0 }, { 2, 216 }, { 125, 6 }, { 0, 151 },
};

static void vag_start(VagDecoder *v, u32 data, u32 size, int loop){
	memset(v, 0, sizeof(*v));
	v->loop_enabled = loop;
	v->loop_start_block = -1;
	v->num_blocks = (int)(size / 16);
	v->data = v->read = data;
	v->cur_sample = 28;
	v->cur_block = -1;
}

static void vag_decode_block(VagDecoder *v, const u8 **rp){
	const u8 *p = *rp;
	int predict, shift, flags, s1 = v->s1, s2 = v->s2, c1, c2, i;
	if(v->cur_block == v->num_blocks - 1){ v->end = 1; return; }
	predict = *p++;
	shift = predict & 0xF;
	predict >>= 4;
	flags = *p++;
	if(flags == 7){ v->end = 1; return; }
	if(flags == 6) v->loop_start_block = v->cur_block;
	else if(flags == 3 && v->loop_enabled) v->loop_at_next_block = 1;
	c1 = vag_f[predict][0];
	c2 = -vag_f[predict][1];
	for(i = 0; i < 28; i += 2){
		u8 d = *p++;
		int a = (s16)((d & 0xF) << 12) >> shift, b = (s16)((d & 0xF0) << 8) >> shift;
		s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
		s1 = clamp16(b + ((s2 * c1 + s1 * c2) >> 6));
		v->samples[i] = (s16)s2;
		v->samples[i + 1] = (s16)s1;
	}
	v->s1 = s1;
	v->s2 = s2;
	v->cur_sample = 0;
	v->cur_block++;
	*rp = p;
}

static void vag_get(VagDecoder *v, s16 *out, int n){
	const u8 *p, *orig;
	int i;
	if(v->end){ memset(out, 0, (size_t)n * 2); return; }
	p = mem_ptr_r(v->read, (u32)(v->num_blocks * 16) > v->read - v->data ? (u32)(v->num_blocks * 16) - (v->read - v->data) : 16);
	if(!p) return;
	orig = p;
	for(i = 0; i < n; i++){
		if(v->cur_sample == 28){
			if(v->loop_at_next_block){
				v->read = v->data + 16 * (u32)v->loop_start_block + 16;
				p = orig = mem_ptr_r(v->read, 16);
				if(!p){ v->end = 1; memset(out + i, 0, (size_t)(n - i) * 2); return; }
				v->cur_block = v->loop_start_block;
				v->loop_at_next_block = 0;
			}
			vag_decode_block(v, &p);
			if(v->end){ memset(out + i, 0, (size_t)(n - i) * 2); break; }
		}
		out[i] = v->samples[v->cur_sample++];
	}
	v->read += (u32)(p - orig);
}

/* --- ADSR ---------------------------------------------------------------------- */

static void env_walk(Envelope *e, int type, int rate){
	s64 d;
	switch(type){
	case CURVE_LINEAR_INC: e->height += rate; break;
	case CURVE_LINEAR_DEC: e->height -= rate; break;
	case CURVE_LINEAR_BENT:
		if(e->height <= (s64)ENV_HEIGHT_MAX * 3 / 4) e->height += rate;
		else e->height += rate / 4;
		break;
	case CURVE_EXP_DEC:
		d = e->height - ENV_HEIGHT_MAX;
		d += (-d * rate) >> 32;
		e->height = d + ENV_HEIGHT_MAX - (s64)(((u32)rate + 3u) / 4u);
		break;
	case CURVE_EXP_INC:
		d = e->height - ENV_HEIGHT_MAX;
		d += (-d * rate) >> 32;
		e->height = d + 0x4000 + ENV_HEIGHT_MAX;
		break;
	case CURVE_DIRECT: e->height = rate; break;
	}
}

static void env_set_state(Envelope *e, int st){
	if(e->height > ENV_HEIGHT_MAX) e->height = ENV_HEIGHT_MAX;
	e->state = st;
}

static void env_step(Envelope *e){
	switch(e->state){
	case ST_ATTACK:
		env_walk(e, e->attack_type, e->attack_rate);
		if(e->height >= ENV_HEIGHT_MAX || e->height < 0) env_set_state(e, ST_DECAY);
		break;
	case ST_DECAY:
		env_walk(e, e->decay_type, e->decay_rate);
		if(e->height < e->sustain_level) env_set_state(e, ST_SUSTAIN);
		break;
	case ST_SUSTAIN:
		env_walk(e, e->sustain_type, e->sustain_rate);
		if(e->height <= 0){ e->height = 0; env_set_state(e, ST_RELEASE); }
		break;
	case ST_RELEASE:
		env_walk(e, e->release_type, e->release_rate);
		if(e->height <= 0){ e->height = 0; env_set_state(e, ST_OFF); }
		break;
	case ST_KEYON:
		e->height = 0;
		env_set_state(e, ST_KEYON_STEP);
		break;
	case ST_KEYON_STEP:
		/* 32 pasos a 0 antes del ataque, como en la PSP */
		e->height++;
		if(e->height >= 31){ e->height = 0; env_set_state(e, ST_ATTACK); }
		break;
	default: break;
	}
}

static int env_height(const Envelope *e){ return (int)(e->height > ENV_HEIGHT_MAX ? ENV_HEIGHT_MAX : e->height); }
static void env_end(Envelope *e){ env_set_state(e, ST_OFF); e->height = 0; }

static int simple_rate(int n){
	int r;
	n &= 0x7F;
	if(n == 0x7F) return 0;
	r = ((7 - (n & 3)) << 26) >> (n >> 2);
	return r ? r : 1;
}

static int exponent_rate(int n){
	int r;
	n &= 0x7F;
	if(n == 0x7F) return 0;
	r = ((7 - (n & 3)) << 24) >> (n >> 2);
	return r ? r : 1;
}

static void env_set_simple(Envelope *e, u32 a1, u32 a2){
	int n;
	e->attack_rate = simple_rate((int)(a1 >> 8));
	e->attack_type = (a1 & 0x8000) ? CURVE_LINEAR_BENT : CURVE_LINEAR_INC;
	n = (int)(a1 >> 4) & 0xF;
	e->decay_rate = n == 0 ? 0x7FFFFFFF : (int)(0x80000000u >> n);
	e->decay_type = CURVE_EXP_DEC;
	e->sustain_type = (int)(a2 >> 14) & 3;
	e->sustain_rate = e->sustain_type == CURVE_EXP_DEC ? exponent_rate((int)(a2 >> 6)) : simple_rate((int)(a2 >> 6));
	e->release_type = (a2 & 0x20) ? CURVE_EXP_DEC : CURVE_LINEAR_DEC;
	n = (int)(a2 & 0x1F);
	if(n == 31) e->release_rate = 0;
	else if(e->release_type == CURVE_LINEAR_DEC) e->release_rate = n == 30 ? 0x40000000 : n == 29 ? 1 : 0x10000000 >> n;
	else e->release_rate = n == 0 ? 0x7FFFFFFF : (int)(0x80000000u >> n);
	e->sustain_level = (int)(((a1 & 0xF) + 1) << 26);
}

/* --- Voces y mezcla -------------------------------------------------------------- */

static void voice_read(Voice *v, s16 *out, int n){
	switch(v->type){
	case VT_VAG:
		vag_get(&v->vag, out, n);
		break;
	case VT_PCM: {
		int need = n;
		while(need > 0){
			int size = v->pcm_size - v->pcm_index < need ? v->pcm_size - v->pcm_index : need;
			const u8 *p;
			int i;
			if(!v->on){ v->pcm_index = 0; break; }
			p = mem_ptr_r(v->pcm_addr + (u32)v->pcm_index * 2, (u32)size * 2);
			for(i = 0; i < size; i++) out[i] = p ? (s16)rd_le16(p + i * 2) : 0;
			v->pcm_index += size;
			need -= size;
			out += size;
			if(v->pcm_index >= v->pcm_size){
				if(!v->loop) break;
				v->pcm_index = v->pcm_loop_pos;
			}
		}
		if(need > 0) memset(out, 0, (size_t)need * 2);
		break;
	}
	default:
		memset(out, 0, (size_t)n * 2);
		break;
	}
}

static int voice_ended(const Voice *v){
	switch(v->type){
	case VT_VAG: return v->vag.end;
	case VT_PCM: return v->pcm_index >= v->pcm_size;
	case VT_ATRAC3: return 1;   /* aún sin decodificador */
	default: return 0;
	}
}

static void mix_voice(Voice *v){
	int delay = 0, to_read, read_pos = 2, temp_pos, i, keyon = v->env.state == ST_KEYON, interp;
	u32 frac = v->sample_frac;
	if((v->type == VT_VAG && !v->vag_addr) || (v->type == VT_PCM && !v->pcm_addr)) return;
	if(keyon){
		int ignore_pitch = v->type == VT_PCM && v->pitch > PITCH_BASE;
		delay = ignore_pitch ? 32 : (int)((32u * (u32)v->pitch) >> PITCH_SHIFT);
		if(v->type == VT_VAG) delay++;
	}
	sas.temp[0] = v->hist[0];
	sas.temp[1] = v->hist[1];
	to_read = (int)((frac + (u32)v->pitch * (u32)(sas.grain - delay > 0 ? sas.grain - delay : 0)) >> PITCH_SHIFT);
	if(to_read > (int)(sizeof(sas.temp) / 2) - 2) to_read = (int)(sizeof(sas.temp) / 2) - 2;
	if(keyon){ read_pos = 0; to_read += 2; }
	voice_read(v, &sas.temp[read_pos], to_read);
	temp_pos = read_pos + to_read;
	for(i = 0; i < delay; i++) env_step(&v->env);
	interp = v->pitch != PITCH_BASE || (frac & PITCH_MASK);
	for(i = delay; i < sas.grain; i++){
		const s16 *s = sas.temp + (frac >> PITCH_SHIFT);
		int sample = s[0], envv;
		if(interp){
			int f = (int)(frac & PITCH_MASK);
			sample = s[0] - (((s[0] - s[1]) * f) >> PITCH_SHIFT);
		}
		frac += (u32)v->pitch;
		envv = env_height(&v->env);
		env_step(&v->env);
		envv = (envv + (1 << 14)) >> 15;
		sample = ((sample * envv) + (1 << 14)) >> 15;
		sas.mix[i * 2] += (sample * v->vol_l) >> 12;
		sas.mix[i * 2 + 1] += (sample * v->vol_r) >> 12;
		sas.send[i * 2] += sample * v->eff_l >> 12;
		sas.send[i * 2 + 1] += sample * v->eff_r >> 12;
	}
	v->hist[0] = sas.temp[temp_pos - 2];
	v->hist[1] = sas.temp[temp_pos - 1];
	v->sample_frac = frac - (u32)(temp_pos - 2) * PITCH_BASE;
	if(voice_ended(v)) env_end(&v->env);
	if(v->env.state == ST_OFF){ v->playing = 0; v->on = 0; }
}

static void sas_mix(u32 out_addr, u32 in_addr, int lvol, int rvol){
	int v, i, g = sas.grain;
	u8 *out;
	const u8 *in = NULL;
	for(v = 0; v < VOICES_MAX; v++)
		if(sas.voices[v].playing && !sas.voices[v].paused) mix_voice(&sas.voices[v]);
	out = mem_ptr(out_addr, (u32)g * (sas.output_mode ? 8 : 4));
	if(in_addr) in = mem_ptr_r(in_addr, (u32)g * 4);
	if(!out){
		/* nada */
	} else if(sas.output_mode == 0){
		/* Sin reverberación todavía: el envío "wet" no suena */
		for(i = 0; i < g * 2; i += 2){
			s32 l = 0, r = 0;
			if(in){
				l = ((s16)rd_le16(in + i * 2) * lvol) >> 12;
				r = ((s16)rd_le16(in + i * 2 + 2) * rvol) >> 12;
			}
			if(sas.dry_on){ l += sas.mix[i]; r += sas.mix[i + 1]; }
			wr_le16(out + i * 2, (u16)clamp16(l));
			wr_le16(out + i * 2 + 2, (u16)clamp16(r));
		}
	} else {
		for(i = 0; i < g; i++){
			wr_le16(out + i * 2, (u16)clamp16(sas.mix[i * 2]));
			wr_le16(out + (u32)(g + i) * 2, (u16)clamp16(sas.mix[i * 2 + 1]));
			wr_le16(out + (u32)(2 * g + i) * 2, (u16)clamp16(sas.send[i * 2]));
			wr_le16(out + (u32)(3 * g + i) * 2, (u16)clamp16(sas.send[i * 2 + 1]));
		}
	}
	memset(sas.mix, 0, sizeof(sas.mix));
	memset(sas.send, 0, sizeof(sas.send));
}

/* Lo que tarda el Media Engine en mezclar un grano (medido por PPSSPP) */
static u32 mix_us(void){
	float us = 110.0f + 0.49f * (float)sas.grain;
	int v;
	for(v = 0; v < VOICES_MAX; v++){
		const Voice *vo = &sas.voices[v];
		float ratio, per;
		if(!vo->playing || vo->paused) continue;
		ratio = (float)vo->pitch / PITCH_BASE;
		switch(vo->type){
		case VT_PCM: per = 0.405f + 0.0675f * ratio; break;
		case VT_NOISE: case VT_TRIWAVE: case VT_PULSEWAVE: per = 0.41f; break;
		default: per = 0.445f + 0.0675f * ratio; break;
		}
		us += per * (float)sas.grain;
	}
	if(sas.effect_type >= 0 && sas.wet_on) us += 0.64f * (float)sas.grain;
	return (u32)us;
}

void sas_init(void){
	int i;
	memset(&sas, 0, sizeof(sas));
	sas.effect_type = -1;
	sas.dry_on = 1;
	for(i = 0; i < VOICES_MAX; i++){
		Voice *v = &sas.voices[i];
		v->pitch = PITCH_BASE;
		v->vol_l = v->vol_r = v->eff_l = v->eff_r = VOL_MAX;
		v->env.state = ST_OFF;
		v->env.decay_type = CURVE_LINEAR_DEC;
		v->env.sustain_type = CURVE_LINEAR_DEC;
		v->env.release_type = CURVE_LINEAR_DEC;
	}
}

/* --- Llamadas -------------------------------------------------------------------- */

static Voice *voice_arg(int n){
	if(n < 0 || n >= VOICES_MAX){ RETURN(ERR_INVALID_VOICE); return NULL; }
	return &sas.voices[n];
}

static void sceSasInit(void){
	u32 core = ARG(0), grain = ARG(1), maxv = ARG(2), mode = ARG(3), rate = ARG(4);
	int i;
	if(!mem_valid(core, 4) || (core & 0x3F)){ RETURN(ERR_BAD_ADDRESS); return; }
	if(maxv == 0 || maxv > VOICES_MAX){ RETURN(ERR_INVALID_MAX_VOICES); return; }
	if(grain < 0x40 || grain > 0x800 || (grain & 0x1F)){ RETURN(ERR_INVALID_GRAIN); return; }
	if(mode > 1){ RETURN(ERR_INVALID_OUTPUT_MODE); return; }
	if(rate != 44100){ RETURN(ERR_INVALID_SAMPLE_RATE); return; }
	sas.grain = (int)grain;
	sas.output_mode = (int)mode;
	for(i = 0; i < VOICES_MAX; i++){ sas.voices[i].playing = 0; sas.voices[i].loop = 0; }
	RETURN(0);
}

/* El hilo espera lo que tarda la mezcla */
static void delay_result(void){
	RETURN(0);
	if(!kernel_in_interrupt()) kernel_wait_until(cpu_cycles + (u64)mix_us() * CYCLES_PER_US);
}

static void sceSasCore(void){
	if(!mem_valid(ARG(1), 4)){ RETURN(ERR_INVALID_PARAMETER); return; }
	if(!kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
	if(!sas.grain){ RETURN(0); return; }
	sas_mix(ARG(1), 0, 0, 0);
	delay_result();
}

static void sceSasCoreWithMix(void){
	if(!mem_valid(ARG(1), 4)){ RETURN(ERR_INVALID_PARAMETER); return; }
	if(sas.output_mode == 1){ RETURN(SCE_KERNEL_ERROR_BAD_ARGUMENT); return; }
	if(!kernel_dispatch_enabled()){ RETURN(SCE_KERNEL_ERROR_CAN_NOT_WAIT); return; }
	if(!sas.grain){ RETURN(0); return; }
	sas_mix(ARG(1), ARG(1), (int)ARG(2), (int)ARG(3));
	delay_result();
}

static void sceSasGetEndFlag(void){
	u32 f = 0;
	int i;
	for(i = 0; i < VOICES_MAX; i++) if(!sas.voices[i].playing) f |= 1u << i;
	RETURN(f);
}

static void sceSasSetVoice(void){
	Voice *v = voice_arg((int)ARG(1));
	u32 addr = ARG(2);
	s32 size = (s32)ARG(3), loop = (s32)ARG(4);
	int reset = 0;
	if(!v) return;
	if(size == 0 || (size & 0xF)){ RETURN(ERR_INVALID_PARAMETER); return; }
	if(loop != 0 && loop != 1){ RETURN(ERR_INVALID_LOOP_POS); return; }
	if(!mem_valid(addr, 1)){ RETURN(0); return; }
	if(v->type == VT_ATRAC3){ RETURN(ERR_ATRAC3_ALREADY_SET); return; }
	if(size < 0) size = 0;   /* tamaños negativos: correcto pero sin sonido (PPSSPP) */
	if(v->type != VT_VAG || v->vag_addr != addr || v->vag_size != (u32)size || v->loop != loop){
		v->type = VT_VAG;
		reset = 1;
	}
	v->vag_addr = addr;
	v->vag_size = (u32)size;
	v->loop = loop;
	if(v->on) v->playing = 1;
	if(reset) vag_start(&v->vag, addr, (u32)size, loop);
	RETURN(0);
}

static void sceSasSetVoicePCM(void){
	Voice *v = voice_arg((int)ARG(1));
	u32 addr = ARG(2);
	s32 size = (s32)ARG(3), loop_pos = (s32)ARG(4);
	if(!v) return;
	if(size <= 0 || size > 0x10000){ RETURN(ERR_INVALID_PCM_SIZE); return; }
	if(loop_pos >= size){ RETURN(ERR_INVALID_LOOP_POS); return; }
	if(!mem_valid(addr, 1)){ RETURN(0); return; }
	if(v->type == VT_ATRAC3){ RETURN(ERR_ATRAC3_ALREADY_SET); return; }
	v->type = VT_PCM;
	v->pcm_addr = addr;
	v->pcm_size = size;
	v->pcm_index = 0;
	v->pcm_loop_pos = loop_pos >= 0 ? loop_pos : 0;
	v->loop = loop_pos >= 0;
	v->playing = 1;
	RETURN(0);
}

static void sceSasGetPauseFlag(void){
	u32 f = 0;
	int i;
	for(i = 0; i < VOICES_MAX; i++) if(sas.voices[i].paused) f |= 1u << i;
	RETURN(f);
}

static void sceSasSetPause(void){
	u32 bits = ARG(1);
	int pause = ARG(2) != 0, i;
	for(i = 0; i < VOICES_MAX; i++) if(bits & (1u << i)) sas.voices[i].paused = pause;
	RETURN(0);
}

static void sceSasSetVolume(void){
	Voice *v = voice_arg((int)ARG(1));
	s32 l = (s32)ARG(2), r = (s32)ARG(3), el = (s32)ARG(4), er = (s32)ARG(5);
	if(!v) return;
	if(abs(l) > VOL_MAX || abs(r) > VOL_MAX || abs(el) > VOL_MAX || abs(er) > VOL_MAX){ RETURN(ERR_INVALID_VOLUME); return; }
	v->vol_l = l; v->vol_r = r; v->eff_l = el; v->eff_r = er;
	RETURN(0);
}

static void sceSasSetPitch(void){
	Voice *v = voice_arg((int)ARG(1));
	s32 p = (s32)ARG(2);
	if(!v) return;
	if(p < 0 || p > PITCH_MAX){ RETURN(ERR_INVALID_PITCH); return; }
	v->pitch = p;
	RETURN(0);
}

static void sceSasSetKeyOn(void){
	Voice *v = voice_arg((int)ARG(1));
	if(!v) return;
	if(v->paused || v->on){ RETURN(ERR_VOICE_PAUSED); return; }
	v->env.state = ST_KEYON;
	if(v->type == VT_VAG && mem_valid(v->vag_addr, 1)) vag_start(&v->vag, v->vag_addr, v->vag_size, v->loop);
	v->playing = v->on = 1;
	v->paused = 0;
	v->sample_frac = 0;
	RETURN(0);
}

static void sceSasSetKeyOff(void){
	Voice *v = voice_arg((int)ARG(1));
	if(!v) return;
	if(v->paused || !v->on){ RETURN(ERR_VOICE_PAUSED); return; }
	v->on = 0;
	env_set_state(&v->env, ST_RELEASE);
	RETURN(0);
}

static void sceSasSetNoise(void){
	Voice *v = voice_arg((int)ARG(1));
	s32 f = (s32)ARG(2);
	if(!v) return;
	if(f < 0 || f >= 64){ RETURN(ERR_INVALID_NOISE_FREQ); return; }
	v->type = VT_NOISE;
	v->noise_freq = f;
	RETURN(0);
}

static void sceSasSetSL(void){
	Voice *v = voice_arg((int)ARG(1));
	if(!v) return;
	v->env.sustain_level = (int)ARG(2);
	RETURN(0);
}

static void sceSasSetADSR(void){
	Voice *v = voice_arg((int)ARG(1));
	int flag = (int)ARG(2), a = (int)ARG(3), d = (int)ARG(4), s = (int)ARG(5), r = (int)ARG(6);
	int invalid = (a < 0 ? 1 : 0) | (d < 0 ? 2 : 0) | (s < 0 ? 4 : 0) | (r < 0 ? 8 : 0);
	if(!v) return;
	if(invalid & flag){ RETURN(ERR_INVALID_ADSR_RATE); return; }
	if(flag & 1) v->env.attack_rate = a;
	if(flag & 2) v->env.decay_rate = d;
	if(flag & 4) v->env.sustain_rate = s;
	if(flag & 8) v->env.release_rate = r;
	RETURN(0);
}

static void sceSasSetADSRmode(void){
	Voice *v = voice_arg((int)ARG(1));
	int flag = (int)ARG(2);
	int a = (int)(ARG(3) & 0x7FFFFFFF), d = (int)(ARG(4) & 0x7FFFFFFF), s = (int)(ARG(5) & 0x7FFFFFFF), r = (int)(ARG(6) & 0x7FFFFFFF);
	int invalid = 0;
	if(!v) return;
	if(a > 5 || (a & 1)) invalid |= 1;
	if(d > 5 || !(d & 1)) invalid |= 2;
	if(s > 5) invalid |= 4;
	if(r > 5 || !(r & 1)) invalid |= 8;
	if(invalid & flag){ RETURN(ERR_INVALID_ADSR_MODE); return; }
	if(flag & 1) v->env.attack_type = a;
	if(flag & 2) v->env.decay_type = d;
	if(flag & 4) v->env.sustain_type = s;
	if(flag & 8) v->env.release_type = r;
	RETURN(0);
}

static void sceSasSetSimpleADSR(void){
	Voice *v = voice_arg((int)ARG(1));
	if(!v) return;
	if((ARG(3) >> 13) & 1){ RETURN(ERR_INVALID_ADSR_MODE); return; }
	env_set_simple(&v->env, ARG(2) & 0xFFFF, ARG(3) & 0xFFFF);
	RETURN(0);
}

static void sceSasGetEnvelopeHeight(void){
	Voice *v = voice_arg((int)ARG(1));
	if(!v) return;
	RETURN((u32)env_height(&v->env));
}

static void sceSasGetAllEnvelopeHeights(void){
	u32 p = ARG(1);
	int i;
	if(!mem_valid(p, VOICES_MAX * 4)){ RETURN(ERR_INVALID_PARAMETER); return; }
	for(i = 0; i < VOICES_MAX; i++) mem_write32(p + (u32)i * 4, (u32)env_height(&sas.voices[i].env));
	RETURN(0);
}

static void sceSasRevType(void){
	s32 t = (s32)ARG(1);
	if(t < -1 || t > 8){ RETURN(ERR_REV_INVALID_TYPE); return; }
	sas.effect_type = t;
	RETURN(0);
}

static void sceSasRevParam(void){
	s32 d = (s32)ARG(1), f = (s32)ARG(2);
	if(d < 0 || d >= 128){ RETURN(ERR_REV_INVALID_DELAY); return; }
	if(f < 0 || f >= 128){ RETURN(ERR_REV_INVALID_FEEDBACK); return; }
	sas.effect_delay = d;
	sas.effect_feedback = f;
	RETURN(0);
}

static void sceSasRevEVOL(void){
	if(ARG(1) > 0x1000 || ARG(2) > 0x1000){ RETURN(ERR_REV_INVALID_VOLUME); return; }
	sas.effect_l = (int)ARG(1);
	sas.effect_r = (int)ARG(2);
	RETURN(0);
}

static void sceSasRevVON(void){
	sas.dry_on = ARG(1) != 0;
	sas.wet_on = ARG(2) != 0;
	RETURN(0);
}

static void sceSasGetGrain(void){ RETURN((u32)sas.grain); }

static void sceSasSetGrain(void){
	u32 g = ARG(1);
	if(g < 0x40 || g > 0x800 || (g & 0x1F)){ RETURN(ERR_INVALID_GRAIN); return; }
	sas.grain = (int)g;
	RETURN(0);
}

static void sceSasGetOutputmode(void){ RETURN((u32)sas.output_mode); }

static void sceSasSetOutputmode(void){
	if(ARG(1) > 1){ RETURN(ERR_INVALID_OUTPUT_MODE); return; }
	sas.output_mode = (int)ARG(1);
	RETURN(0);
}

static void sceSasSetVoiceATRAC3(void){
	Voice *v = voice_arg((int)ARG(1));
	u32 ctx = ARG(2);
	if(!v) return;
	if(!mem_valid(ctx, 256) || (ctx & 3)){ RETURN(ERR_INVALID_PARAMETER); return; }
	if(v->type == VT_ATRAC3){ RETURN(ERR_ATRAC3_ALREADY_SET); return; }
	v->type = VT_ATRAC3;
	v->loop = 0;
	v->playing = 1;
	if(mem_valid(ARG(0) + 56 * ARG(1) + 20, 4)) mem_write32(ARG(0) + 56 * ARG(1) + 20, ctx);
	RETURN(0);
}

static void sceSasUnsetATRAC3(void){
	Voice *v = voice_arg((int)ARG(1));
	if(!v) return;
	if(v->type != VT_ATRAC3){ RETURN(ERR_ATRAC3_NOT_SET); return; }
	v->type = VT_OFF;
	v->playing = v->on = v->paused = 0;
	if(mem_valid(ARG(0) + 56 * ARG(1) + 20, 4)) mem_write32(ARG(0) + 56 * ARG(1) + 20, 0);
	RETURN(0);
}

static void sas_zero(void){ RETURN(0); }

static const HleFunction sas_core[] = {
	{ "__sceSasInit", sceSasInit },
	{ "__sceSasCore", sceSasCore },
	{ "__sceSasCoreWithMix", sceSasCoreWithMix },
	{ "__sceSasGetEndFlag", sceSasGetEndFlag },
	{ "__sceSasSetVolume", sceSasSetVolume },
	{ "__sceSasSetPitch", sceSasSetPitch },
	{ "__sceSasSetVoice", sceSasSetVoice },
	{ "__sceSasSetNoise", sceSasSetNoise },
	{ "__sceSasSetADSR", sceSasSetADSR },
	{ "__sceSasSetADSRmode", sceSasSetADSRmode },
	{ "__sceSasSetSL", sceSasSetSL },
	{ "__sceSasGetEnvelopeHeight", sceSasGetEnvelopeHeight },
	{ "__sceSasSetSimpleADSR", sceSasSetSimpleADSR },
	{ "__sceSasSetKeyOff", sceSasSetKeyOff },
	{ "__sceSasSetKeyOn", sceSasSetKeyOn },
	{ "__sceSasRevVON", sceSasRevVON },
	{ "__sceSasRevEVOL", sceSasRevEVOL },
	{ "__sceSasRevType", sceSasRevType },
	{ "__sceSasRevParam", sceSasRevParam },
	{ "__sceSasGetPauseFlag", sceSasGetPauseFlag },
	{ "__sceSasSetPause", sceSasSetPause },
	{ "__sceSasSetTrianglarWave", sas_zero },
	{ "__sceSasSetSteepWave", sas_zero },
	{ "__sceSasGetGrain", sceSasGetGrain },
	{ "__sceSasSetGrain", sceSasSetGrain },
	{ "__sceSasGetOutputmode", sceSasGetOutputmode },
	{ "__sceSasSetOutputmode", sceSasSetOutputmode },
	{ "__sceSasGetAllEnvelopeHeights", sceSasGetAllEnvelopeHeights },
	{ "__sceSasSetVoicePCM", sceSasSetVoicePCM },
	{ "__sceSasSetVoiceATRAC3", sceSasSetVoiceATRAC3 },
	{ "__sceSasConcatenateATRAC3", sas_zero },
	{ "__sceSasUnsetATRAC3", sceSasUnsetATRAC3 },
};

const HleLibrary hle_sas_libs[] = {
	HLE_LIBRARY("sceSasCore", sas_core),
};
const u32 hle_sas_libs_count = sizeof(hle_sas_libs) / sizeof(hle_sas_libs[0]);
