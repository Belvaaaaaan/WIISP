/**
 * WIISP - atrac.c
 * HLE de sceAtrac3plus: la biblioteca con la que los juegos reproducen
 * música y voces en ATRAC3 / ATRAC3plus desde un búfer en memoria.
 *
 * Portado de PPSSPP (Core/HLE/sceAtrac.cpp, Core/HLE/AtracCtx2.cpp y
 * Core/Util/AtracTrack.cpp, (c) 2012- PPSSPP Project, GPLv2+), que imita
 * al libatrac3plus.prx real: el estado vive en un contexto de 256 bytes en
 * la memoria de la PSP y la lógica de búferes (todo cargado, a medias, en
 * streaming con o sin bucle y con "cola" en un segundo búfer) es la del
 * firmware, que es lo que los juegos esperan para pedir más datos.
 *
 * Todavía no hay decodificador: cada trama "decodificada" son muestras en
 * silencio, pero con los mismos tiempos y avances que en la PSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "hle/hle.h"
#include "core/memory.h"

#define MAX_IDS 6
#define CTX_SIZE 256

#define CODEC_AT3PLUS 0x1000
#define CODEC_AT3     0x1001

#define ERR_API_FAIL                 0x80630002u
#define ERR_NO_ATRACID               0x80630003u
#define ERR_INVALID_CODECTYPE        0x80630004u
#define ERR_BAD_ATRACID              0x80630005u
#define ERR_UNKNOWN_FORMAT           0x80630006u
#define ERR_WRONG_CODECTYPE          0x80630007u
#define ERR_BAD_CODEC_PARAMS         0x80630008u
#define ERR_ALL_DATA_LOADED          0x80630009u
#define ERR_NO_DATA                  0x80630010u
#define ERR_SIZE_TOO_SMALL           0x80630011u
#define ERR_SECOND_BUFFER_NEEDED     0x80630012u
#define ERR_INCORRECT_READ_SIZE      0x80630013u
#define ERR_BAD_ALIGNMENT            0x80630014u
#define ERR_BAD_SAMPLE               0x80630015u
#define ERR_BAD_FIRST_RESET_SIZE     0x80630016u
#define ERR_BAD_SECOND_RESET_SIZE    0x80630017u
#define ERR_ADD_DATA_IS_TOO_BIG      0x80630018u
#define ERR_NOT_MONO                 0x80630019u
#define ERR_NO_LOOP_INFORMATION      0x80630021u
#define ERR_SECOND_BUFFER_NOT_NEEDED 0x80630022u
#define ERR_BUFFER_IS_EMPTY          0x80630023u
#define ERR_ALL_DATA_DECODED         0x80630024u
#define ERR_IS_LOW_LEVEL             0x80630031u
#define ERR_IS_FOR_SCESAS            0x80630040u
#define ERR_AA3_OTHER_FAILURE        0x80631002u
#define ERR_AA3_INVALID_DATA         0x80631003u
#define ERR_AA3_SIZE_TOO_SMALL       0x80631004u
#define ERR_AA3_BAD_CODEC_PARAMS     0x80631005u
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR  0x800200D3u
#define SCE_KERNEL_ERROR_BUSY          0x80000021u
#define SCE_KERNEL_ERROR_OUT_OF_MEMORY 0x80000022u

enum {
	ST_UNINITIALIZED = 0, ST_NO_DATA = 1, ST_ALL_DATA_LOADED = 2, ST_HALFWAY_BUFFER = 3,
	ST_STREAMED_WITHOUT_LOOP = 4, ST_STREAMED_LOOP_FROM_END = 5, ST_STREAMED_LOOP_WITH_TRAILER = 6,
	ST_LOW_LEVEL = 8, ST_FOR_SCESAS = 16,
};
#define IS_STREAMING(s) (((s) & 4) != 0)

#define ALLDATA_IS_ON_MEMORY (-1)
#define NONLOOP_STREAM_DATA_IS_ON_MEMORY (-2)
#define LOOP_STREAM_DATA_IS_ON_MEMORY (-3)

#define RIFF_MAGIC 0x46464952u
#define WAVE_MAGIC 0x45564157u
#define FMT_MAGIC  0x20746D66u
#define FACT_MAGIC 0x74636166u
#define SMPL_MAGIC 0x6C706D73u
#define DATA_MAGIC 0x61746164u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define WAVE_FORMAT_AT3 0x270

/* SceAtracIdInfo, la mitad alta del contexto (copia en el host) */
typedef struct {
	s32 decode_pos, end_sample, loop_start, loop_end, first_valid_sample;
	u8 num_skip_frames, state, cur_buffer, num_chan;
	u16 sample_size, codec;
	s32 data_off, cur_file_off, file_data_end, loop_num, stream_data_byte, stream_off, second_stream_off;
	u32 buffer, second_buffer, buffer_byte, second_buffer_byte;
} Info;

typedef struct {
	int used;
	int output_channels;
	int skipped_on_set_data;
	u32 codec_err;
	u8 format1, format2;
	Info info;
} Atrac;

/* Lo que el analizador de pista "viejo" saca para validar (AnalyzeAtracTrack) */
typedef struct {
	u32 codec;
	int channels, joint_stereo, bytes_per_frame;
} Track;

/* Lo que usa SetData (ParseWaveAT3 / ParseAA3) */
typedef struct {
	u16 num_chans, block_align;
	u8 sample_size_maybe, tail_flag;
	u32 data_off, end_sample, wave_data_size, first_sample_offset, loop_start, loop_end;
} TrackInfo;

static Atrac atracs[MAX_IDS];
static u32 context_types[MAX_IDS];
static int max_contexts = 6;
static int inited = 1;
static u32 bss;
static u64 me_busy_until;

/* --- Media Engine ------------------------------------------------------------- */

u32 me_schedule_job(u32 us){
	u64 now = cpu_cycles, start = me_busy_until > now ? me_busy_until : now;
	me_busy_until = start + (u64)us * CYCLES_PER_US;
	return (u32)((me_busy_until - now + CYCLES_PER_US - 1) / CYCLES_PER_US);
}

static u32 init_us(u32 codec, int mono){
	if(codec == CODEC_AT3PLUS) return mono ? 524 : 646;
	if(codec == CODEC_AT3) return 210;
	return 230;
}

static u32 frame_us(const Atrac *a){
	const Info *i = &a->info;
	if(i->codec == CODEC_AT3PLUS)
		return i->num_chan == 1 ? 1500 + i->sample_size * 84u / 100u : 1815 + i->sample_size * 158u / 100u;
	return i->num_chan == 1 ? 685 : 970 + i->sample_size * 44u / 100u;
}

/* --- El contexto en la memoria de la PSP ------------------------------------------ */

static u32 ctx_addr(int id){ return bss + (u32)id * CTX_SIZE; }

static void ctx_write(int id){
	const Atrac *a = &atracs[id];
	const Info *i = &a->info;
	u32 c = ctx_addr(id), p = c + 128;
	if(!bss || !mem_valid(c, CTX_SIZE)) return;
	mem_write32(c + 0x08, a->codec_err);
	mem_write8(c + 0x28, a->format1);
	mem_write8(c + 0x29, a->format2);
	mem_write32(p + 0, (u32)i->decode_pos);
	mem_write32(p + 4, (u32)i->end_sample);
	mem_write32(p + 8, (u32)i->loop_start);
	mem_write32(p + 12, (u32)i->loop_end);
	mem_write32(p + 16, (u32)i->first_valid_sample);
	mem_write8(p + 20, i->num_skip_frames);
	mem_write8(p + 21, i->state);
	mem_write8(p + 22, i->cur_buffer);
	mem_write8(p + 23, i->num_chan);
	mem_write16(p + 24, i->sample_size);
	mem_write16(p + 26, i->codec);
	mem_write32(p + 28, (u32)i->data_off);
	mem_write32(p + 32, (u32)i->cur_file_off);
	mem_write32(p + 36, (u32)i->file_data_end);
	mem_write32(p + 40, (u32)i->loop_num);
	mem_write32(p + 44, (u32)i->stream_data_byte);
	mem_write32(p + 48, (u32)i->stream_off);
	mem_write32(p + 52, (u32)i->second_stream_off);
	mem_write32(p + 56, i->buffer);
	mem_write32(p + 60, i->second_buffer);
	mem_write32(p + 64, i->buffer_byte);
	mem_write32(p + 68, i->second_buffer_byte);
}

/* La memoria de la PSP manda: un juego puede tocar el contexto a mano */
static void ctx_read(int id){
	Atrac *a = &atracs[id];
	Info *i = &a->info;
	u32 c = ctx_addr(id), p = c + 128;
	if(!bss || !mem_valid(c, CTX_SIZE)) return;
	a->codec_err = mem_read32(c + 0x08);
	i->decode_pos = (s32)mem_read32(p + 0);
	i->end_sample = (s32)mem_read32(p + 4);
	i->loop_start = (s32)mem_read32(p + 8);
	i->loop_end = (s32)mem_read32(p + 12);
	i->first_valid_sample = (s32)mem_read32(p + 16);
	i->num_skip_frames = mem_read8(p + 20);
	i->state = mem_read8(p + 21);
	i->cur_buffer = mem_read8(p + 22);
	i->num_chan = mem_read8(p + 23);
	i->sample_size = mem_read16(p + 24);
	i->codec = mem_read16(p + 26);
	i->data_off = (s32)mem_read32(p + 28);
	i->cur_file_off = (s32)mem_read32(p + 32);
	i->file_data_end = (s32)mem_read32(p + 36);
	i->loop_num = (s32)mem_read32(p + 40);
	i->stream_data_byte = (s32)mem_read32(p + 44);
	i->stream_off = (s32)mem_read32(p + 48);
	i->second_stream_off = (s32)mem_read32(p + 52);
	i->buffer = mem_read32(p + 56);
	i->second_buffer = mem_read32(p + 60);
	i->buffer_byte = mem_read32(p + 64);
	i->second_buffer_byte = mem_read32(p + 68);
}

static void ensure_bss(void){
	if(!bss){
		bss = kernel_alloc(MAX_IDS * CTX_SIZE, 1, "AtracContext");
		if(bss == 0xFFFFFFFFu) bss = 0;
	}
	if(bss){
		u8 *p = mem_ptr(bss, MAX_IDS * CTX_SIZE);
		if(p) memset(p, 0, MAX_IDS * CTX_SIZE);
	}
}

void atrac_notify_load(u32 addr){
	int i;
	bss = addr;
	if(bss){
		u8 *p = mem_ptr(bss, MAX_IDS * CTX_SIZE);
		if(p) memset(p, 0, MAX_IDS * CTX_SIZE);
	}
	for(i = 0; i < MAX_IDS; i++) if(atracs[i].used) ctx_write(i);
}

void atrac_init(void){
	memset(atracs, 0, sizeof(atracs));
	max_contexts = 6;
	inited = 1;
	bss = 0;
	me_busy_until = 0;
	/* Dos de cada uno en este orden, como el firmware */
	context_types[0] = context_types[1] = CODEC_AT3PLUS;
	context_types[2] = context_types[3] = CODEC_AT3;
	context_types[4] = context_types[5] = 0;
}

void atrac_shutdown(void){
	memset(atracs, 0, sizeof(atracs));
}

/* --- Análisis de la pista -------------------------------------------------------- */

static u32 r32(const u8 *b, u32 o){ return rd_le32(b + o); }
static u16 r16(const u8 *b, u32 o){ return rd_le16(b + o); }

/* Cuántos bytes desde addr se pueden leer, hasta len (las regiones son contiguas) */
static u32 clamp_valid(u32 addr, u32 len){
	u32 lo = 0, hi = len;
	if(len == 0 || mem_valid(addr, len)) return len;
	while(lo < hi){
		u32 mid = lo + (hi - lo + 1) / 2;
		if(mem_valid(addr, mid)) lo = mid; else hi = mid - 1;
	}
	return lo;
}

/* AnalyzeAtracTrack: la validación que hacen los SetData antes de nada */
static u32 analyze_track(u32 addr, u32 size, Track *t){
	const u8 *b;
	u32 off = 8, max_size, n;
	int found_data = 0;
	memset(t, 0, sizeof(*t));
	t->channels = 2;
	if(size < 72) return ERR_SIZE_TOO_SMALL;
	n = clamp_valid(addr, size);
	if(n < 72) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	b = mem_ptr_r(addr, n);
	if(r32(b, 0) != RIFF_MAGIC) return ERR_UNKNOWN_FORMAT;
	while(r32(b, off) != WAVE_MAGIC){
		s32 chunk = (s32)r32(b, off - 4);
		if(chunk < 0 || (u64)off + (u64)chunk + (chunk & 1) + 12 > n) return ERR_SIZE_TOO_SMALL;
		off += (u32)chunk + (u32)(chunk & 1);
		if(r32(b, off) != RIFF_MAGIC) return ERR_UNKNOWN_FORMAT;
		off += 8;
	}
	off += 4;
	/* El tamaño RIFF puede ser mayor que lo leído: se mira hasta donde haya memoria */
	max_size = r32(b, off - 8) + 8;
	if(max_size < size) max_size = size;
	max_size = clamp_valid(addr, max_size);
	b = mem_ptr_r(addr, max_size);
	while(max_size >= off + 8 && !found_data){
		u32 magic = r32(b, off), chunk = r32(b, off + 4);
		chunk += chunk & 1;
		off += 8;
		if(chunk > max_size - off && magic != DATA_MAGIC) break;
		switch(magic){
		case FMT_MAGIC: {
			u16 tag = r16(b, off);
			if(t->codec) return ERR_UNKNOWN_FORMAT;
			if(chunk < 32 || (tag == WAVE_FORMAT_EXTENSIBLE && chunk < 52)) return ERR_UNKNOWN_FORMAT;
			if(tag == WAVE_FORMAT_AT3) t->codec = CODEC_AT3;
			else if(tag == WAVE_FORMAT_EXTENSIBLE) t->codec = CODEC_AT3PLUS;
			else return ERR_UNKNOWN_FORMAT;
			t->channels = r16(b, off + 2);
			if(t->channels != 1 && t->channels != 2) return ERR_UNKNOWN_FORMAT;
			if(r32(b, off + 4) != 44100) return ERR_UNKNOWN_FORMAT;
			t->bytes_per_frame = r16(b, off + 12);
			if(!t->bytes_per_frame) return ERR_UNKNOWN_FORMAT;
			if(tag == WAVE_FORMAT_AT3) t->joint_stereo = r16(b, off + 24);
			break;
		}
		case DATA_MAGIC:
			found_data = 1;
			break;
		default:
			break;
		}
		off += chunk;
	}
	if(!t->codec) return ERR_UNKNOWN_FORMAT;
	if(!found_data) return ERR_SIZE_TOO_SMALL;
	return 0;
}

static const u8 at3_checkbytes[16] = {
	0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62,
};

/* ParseWaveAT3: devuelve el códec o un error */
static u32 parse_wave(const u8 *d, u32 len, TrackInfo *t){
	u32 ret = ERR_UNKNOWN_FORMAT, off = 0;
	int modified_offset = 0;
	t->loop_start = t->loop_end = 0xFFFFFFFFu;
	t->first_sample_offset = 0;
	t->end_sample = 0;
	t->wave_data_size = 0;
	for(;;){
		u32 size;
		if(off + 0xC >= len) return ERR_SIZE_TOO_SMALL;
		if(r32(d, off) != RIFF_MAGIC) return ERR_UNKNOWN_FORMAT;
		size = (r32(d, off + 4) + 1) & ~1u;
		if(r32(d, off + 8) == WAVE_MAGIC){ off += 12; break; }
		off += 12;
		if(size < 4 || (u64)off + size - 4 > len) return ERR_SIZE_TOO_SMALL;
		off += size - 4;
	}
	for(;;){
		u32 id, next;
		int size;
		if(off + 8 >= len) return ERR_SIZE_TOO_SMALL;
		id = r32(d, off);
		size = (int)((r32(d, off + 4) + 1) & ~1u);
		off += 8;
		next = off + (u32)size;
		if(off + (u32)size > len && id != DATA_MAGIC) return ERR_SIZE_TOO_SMALL;
		switch(id){
		case DATA_MAGIC:
			t->wave_data_size = (u32)size;
			t->data_off = off;
			if(!t->first_sample_offset) t->first_sample_offset = ret == CODEC_AT3 ? 0x400 : 0x800;
			if(modified_offset && ret == CODEC_AT3PLUS){
				t->first_sample_offset -= 0xB8;
				if(t->loop_end != 0xFFFFFFFFu){ t->loop_end -= 0xB8; t->loop_start -= 0xB8; }
			}
			return ret;
		case FMT_MAGIC: {
			u16 tag;
			if(ret != ERR_UNKNOWN_FORMAT) return ERR_UNKNOWN_FORMAT;
			if(size < 0x20) return ERR_UNKNOWN_FORMAT;
			tag = r16(d, off);
			t->num_chans = r16(d, off + 2);
			if(t->num_chans != 1 && t->num_chans != 2) return ERR_UNKNOWN_FORMAT;
			if(r32(d, off + 4) != 44100) return ERR_UNKNOWN_FORMAT;
			t->block_align = r16(d, off + 12);
			if(!t->block_align) return ERR_UNKNOWN_FORMAT;
			if(tag == WAVE_FORMAT_AT3){
				u16 tail;
				if(r16(d, off + 18) != 1) return ERR_UNKNOWN_FORMAT;   /* joint stereo */
				tail = r16(d, off + 24);
				t->sample_size_maybe = (u8)tail;
				t->tail_flag = (u8)(tail >> 8);
				if(tail != r16(d, off + 26)) return ERR_UNKNOWN_FORMAT;
				if(r32(d, off + 28) != 1) return ERR_UNKNOWN_FORMAT;
				ret = CODEC_AT3;
			} else if(tag == WAVE_FORMAT_EXTENSIBLE){
				if(size < 0x34) return ERR_UNKNOWN_FORMAT;
				if(memcmp(d + off + 14 + 10, at3_checkbytes, 16) != 0) return ERR_UNKNOWN_FORMAT;
				t->sample_size_maybe = d[off + 14 + 0x1C];
				t->tail_flag = d[off + 14 + 0x1D];
				if((u32)((t->sample_size_maybe << 27) >> 29) != t->num_chans) return ERR_UNKNOWN_FORMAT;
				ret = CODEC_AT3PLUS;
			} else return ERR_UNKNOWN_FORMAT;
			break;
		}
		case SMPL_MAGIC:
			if((s32)t->loop_start < 0){
				u32 n;
				if(size < 0x20) return ERR_UNKNOWN_FORMAT;
				n = r32(d, off + 0x1C);
				if(n){
					if(size < 0x34) return ERR_SIZE_TOO_SMALL;
					t->loop_start = r32(d, off + 0x2C);
					t->loop_end = r32(d, off + 0x30);
					if((s32)t->loop_end <= (s32)t->loop_start) return ERR_BAD_CODEC_PARAMS;
				}
			}
			break;
		case FACT_MAGIC:
			if(size < 4) return ERR_UNKNOWN_FORMAT;
			t->end_sample = r32(d, off);
			if(size - 4 == 4) t->first_sample_offset = r32(d, off + 4);
			else if(size - 4 >= 8){ t->first_sample_offset = r32(d, off + 8); modified_offset = 1; }
			break;
		default:
			break;
		}
		off = next;
	}
}

/* Cabeceras AA3 (OMA): ParseAA3 */
static u32 parse_aa3(const u8 *d, u32 len, u32 file_size, TrackInfo *t){
	u32 off = 0, tag_size = 0, data_off = 0, extra, params, codec_type;
	if(len < 10) return ERR_AA3_SIZE_TOO_SMALL;
	if((d[0] == 'e' && d[1] == 'a' && d[2] == '3') || (d[0] == 'I' && d[1] == 'D' && d[2] == '3')){
		if(d[3] != 3 || d[4] != 0) return ERR_AA3_OTHER_FAILURE;
		tag_size = ((u32)(d[6] & 0x7F) << 21) | ((u32)(d[7] & 0x7F) << 14) | ((u32)(d[8] & 0x7F) << 7) | (d[9] & 0x7F);
		off = 10 + tag_size;
		if(off >= len) return ERR_AA3_SIZE_TOO_SMALL;
		if(d[off] == 0) off += 0x10;
		data_off = off;
	}
	if(off + 0x22 > len) return ERR_AA3_SIZE_TOO_SMALL;
	if(d[off] != 'E' || d[off + 1] != 'A' || d[off + 2] != '3') return ERR_AA3_INVALID_DATA;
	off += 4;
	extra = ((u32)d[off] << 8) | d[off + 1];
	off += 2;
	if(d[off] != 0xFF || d[off + 1] != 0xFF) return ERR_AA3_INVALID_DATA;
	off += 2 + 24;
	codec_type = d[off];
	params = ((u32)d[off] << 24) | ((u32)d[off + 1] << 16) | ((u32)d[off + 2] << 8) | d[off + 3];
	if(codec_type > 1) return ERR_AA3_INVALID_DATA;
	data_off += extra;
	t->loop_start = t->loop_end = 0xFFFFFFFFu;
	t->wave_data_size = file_size - data_off;
	t->data_off = data_off;
	t->num_chans = 2;
	t->end_sample = 0;
	if(codec_type == 0){
		if((params & 0xE000) != 0x2000) return ERR_AA3_BAD_CODEC_PARAMS;
		t->first_sample_offset = 0x400;
		t->block_align = (u16)((params & 0x3FF) << 3);
		t->sample_size_maybe = (params & 0x20000) ? 1 : 0;
		t->tail_flag = 0;
		return CODEC_AT3;
	}
	if((params & 0x1C00) != 0x800 || (params & 0xE000) != 0x2000) return ERR_AA3_BAD_CODEC_PARAMS;
	t->first_sample_offset = 0x800;
	t->block_align = (u16)((params & 0x3FF) * 8 + 8);
	t->sample_size_maybe = (u8)(((params >> 8) & 3) | 0x28);
	t->tail_flag = (u8)(params & 0xFF);
	return CODEC_AT3PLUS;
}

/* AnalyzeAA3Track: la validación previa de los SetAA3* */
static u32 analyze_aa3(u32 addr, u32 size, Track *t){
	const u8 *b;
	u32 tag;
	memset(t, 0, sizeof(*t));
	if(size < 10) return ERR_AA3_SIZE_TOO_SMALL;
	b = mem_ptr_r(addr, 10);
	if(!b) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if(b[0] != 'e' || b[1] != 'a' || b[2] != '3') return ERR_AA3_INVALID_DATA;
	tag = b[9] | ((u32)b[8] << 7) | ((u32)b[7] << 14) | ((u32)b[6] << 21);
	if(size < tag + 46) return ERR_AA3_SIZE_TOO_SMALL;
	b = mem_ptr_r(addr, tag + 46);
	if(!b) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	b += 10 + tag;
	if(b[0] != 'E' || b[1] != 'A' || b[2] != '3') return ERR_AA3_INVALID_DATA;
	switch(b[32]){
	case 0: t->codec = CODEC_AT3; t->channels = 2; t->bytes_per_frame = (int)(((b[33] | (b[34] << 8)) & 0x3FF) * 8); break;
	case 1: t->codec = CODEC_AT3PLUS; t->channels = (b[34] >> 2) & 7; break;
	default: return ERR_AA3_INVALID_DATA;
	}
	return 0;
}

/* --- La lógica de AtracCtx2 ------------------------------------------------------ */

static int samples_per_frame(const Info *i){ return i->codec == CODEC_AT3PLUS ? 0x800 : 0x400; }
static int skip_samples(const Info *i){ return i->codec == CODEC_AT3PLUS ? 0x170 : 0x45; }

static int round_down(int size, int grain){ return size - (size % grain); }

static int round_down_off(int offset, int size, int grain){
	return size > offset ? ((size - offset) / grain) * grain + offset : size;
}

static int skip_frames_at(const Info *i, int pos){
	return (pos & (samples_per_frame(i) - 1)) < skip_samples(i) ? 2 : 1;
}

static int file_offset_at(const Info *i, int pos){
	int off = ((pos / samples_per_frame(i)) - 1) * i->sample_size;
	if((pos & (samples_per_frame(i) - 1)) < skip_samples(i) && off != 0) off -= i->sample_size;
	return off + i->data_off;
}

static int loop_end_file_offset(const Info *i, int pos){
	return (pos / samples_per_frame(i) + 1) * i->sample_size + i->data_off;
}

static int space_used(const Info *i){
	if(i->decode_pos > i->loop_end && i->cur_buffer == 1){
		int space = (int)i->second_buffer_byte;
		if(i->second_stream_off < space) space = round_down_off(i->second_stream_off, (int)i->second_buffer_byte, i->sample_size);
		if(i->second_stream_off <= space && space - i->second_stream_off < i->stream_data_byte)
			return i->stream_data_byte - (space - i->second_stream_off);
		return 0;
	}
	return i->stream_data_byte;
}

static int remain_stream(const Info *i){
	int r;
	if(i->stream_data_byte >= i->file_data_end - i->cur_file_off) return NONLOOP_STREAM_DATA_IS_ON_MEMORY;
	r = i->stream_data_byte / i->sample_size - i->num_skip_frames;
	return r > 0 ? r : 0;
}

static int remain_looped(const Info *i){
	int ls = file_offset_at(i, i->loop_start), le = loop_end_file_offset(i, i->loop_end);
	int write_off = i->cur_file_off + i->stream_data_byte, left = write_off - le, remain;
	if(write_off <= le) remain = i->stream_data_byte / i->sample_size;
	else {
		int skip = skip_frames_at(i, i->loop_start), first = le - ls, second = left % first;
		remain = (le - i->cur_file_off) / i->sample_size + (left / first) * (first / i->sample_size - skip);
		if(second > skip * i->sample_size) remain += second / i->sample_size - skip;
	}
	remain -= i->num_skip_frames;
	if(remain < 0) remain = 0;
	if(i->loop_num < 0) return remain;
	if(write_off >= le){
		int loops = (write_off - le) / (le - ls);
		if(i->loop_num <= loops) return LOOP_STREAM_DATA_IS_ON_MEMORY;
	}
	return remain;
}

static int remaining_frames(const Info *i){
	switch(i->state){
	case ST_UNINITIALIZED: case ST_NO_DATA: return 0;
	case ST_ALL_DATA_LOADED: return ALLDATA_IS_ON_MEMORY;
	case ST_HALFWAY_BUFFER: {
		int w = i->data_off + i->stream_data_byte, r;
		if(i->cur_file_off >= w) return 0;
		r = (w - i->cur_file_off) / i->sample_size - i->num_skip_frames;
		return r > 0 ? r : 0;
	}
	case ST_STREAMED_WITHOUT_LOOP: return remain_stream(i);
	case ST_STREAMED_LOOP_FROM_END: return remain_looped(i);
	case ST_STREAMED_LOOP_WITH_TRAILER: return i->decode_pos <= i->loop_end ? remain_looped(i) : remain_stream(i);
	default: return (int)ERR_BAD_ATRACID;
	}
}

static int next_samples(const Info *i){
	int mask = samples_per_frame(i) - 1, end = i->decode_pos | mask;
	int rem = end - i->end_sample > 0 ? end - i->end_sample : 0;
	int adj = (i->decode_pos & mask) + rem;
	return samples_per_frame(i) - adj > 0 ? samples_per_frame(i) - adj : 0;
}

/* Una trama: avanza el estado como el firmware (y deja silencio en out) */
static u32 decode_internal(Atrac *a, u32 out, int *num, int *finish){
	Info *i = &a->info;
	int todo = next_samples(i), next_off = i->cur_file_off + i->sample_size;
	u32 buf, soff;
	if(next_off > i->file_data_end || i->decode_pos > i->end_sample){ *finish = 1; return ERR_ALL_DATA_DECODED; }
	if(IS_STREAMING(i->state) && i->stream_data_byte < i->sample_size){ *finish = 0; return ERR_BUFFER_IS_EMPTY; }
	if(i->state == ST_HALFWAY_BUFFER && i->data_off + i->stream_data_byte < next_off){ *finish = 0; return ERR_BUFFER_IS_EMPTY; }
	if(!IS_STREAMING(i->state)){ buf = i->buffer; soff = (u32)i->cur_file_off; }
	else if((i->cur_buffer & 1) == 0){ buf = i->buffer; soff = (u32)i->stream_off; }
	else { buf = i->second_buffer; soff = (u32)i->second_stream_off; }
	if(!mem_valid(buf + soff, 1)) return ERR_API_FAIL;
	/* Sin decodificador, al menos lo que rechazaría el de verdad: el bit de
	   inicio de ATRAC3plus debe ser 0 y la unidad de sonido de ATRAC3 empieza
	   por 0x28 (6 bits). Así fallan igual los datos basura. */
	{
		u8 b0 = mem_read8(buf + soff);
		int bad = i->codec == CODEC_AT3PLUS ? (b0 & 0x80) != 0 : (b0 >> 2) != 0x28;
		if(bad){
			*finish = 0;
			a->codec_err = 0x20B;
			return ERR_API_FAIL;
		}
	}
	if(bss){
		mem_write32(ctx_addr((int)(a - atracs)) + 0x18, buf + soff);
		mem_write32(ctx_addr((int)(a - atracs)) + 0x20, out);
	}
	a->codec_err = 0;
	i->cur_file_off += i->sample_size;
	if(i->num_skip_frames == 0){
		if(num) *num = todo;
		*finish = i->end_sample < i->decode_pos + todo ? i->loop_num == 0 : 0;
		if(out && todo){
			u8 *p = mem_ptr(out, (u32)(todo * a->output_channels * 2));
			if(p) memset(p, 0, (size_t)(todo * a->output_channels * 2));
		}
		i->decode_pos += todo;
		if(i->loop_end != 0 && i->loop_num != 0 && i->decode_pos > i->loop_end){
			i->cur_file_off = file_offset_at(i, i->loop_start);
			i->num_skip_frames = (u8)skip_frames_at(i, i->loop_start);
			i->decode_pos = i->loop_start;
			if(i->loop_num > 0) i->loop_num--;
		}
	} else i->num_skip_frames--;

	if(IS_STREAMING(i->state)){
		i->stream_data_byte -= i->sample_size;
		if(i->cur_buffer == 1){
			int n = i->second_stream_off + i->sample_size;
			if((int)i->second_buffer_byte < n + i->sample_size){
				i->stream_off = 0;
				i->second_stream_off = 0;
				i->cur_buffer = 2;
			} else i->second_stream_off = n;
		} else {
			int n = i->stream_off + i->sample_size;
			i->stream_off = n + i->sample_size > (int)i->buffer_byte ? 0 : n;
			if(i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->cur_buffer == 0 &&
			   (i->loop_end == 0 || (i->loop_num == 0 && i->loop_end < i->decode_pos)) &&
			   i->cur_file_off >= loop_end_file_offset(i, i->loop_end)){
				u32 len = i->second_buffer_byte % i->sample_size;
				u8 *dst;
				const u8 *src;
				i->cur_buffer = 1;
				i->stream_data_byte = (s32)i->second_buffer_byte;
				i->second_stream_off = 0;
				if(len > i->buffer_byte) len = i->buffer_byte;
				dst = mem_ptr(i->buffer, len);
				src = mem_ptr_r(i->second_buffer + (i->second_buffer_byte - i->second_buffer_byte % i->sample_size), len);
				if(dst && src && len) memmove(dst, src, len);
			}
		}
	}
	return 0;
}

static u32 skip_frames(Atrac *a, int *count){
	int fin;
	*count = 0;
	while(a->info.num_skip_frames){
		u32 r = decode_internal(a, 0, NULL, &fin);
		if(r){
			if(r == ERR_API_FAIL) (*count)++;
			return r;
		}
		(*count)++;
	}
	return 0;
}

static void wrap_last_packet(Atrac *a){
	Info *i = &a->info;
	int dist;
	if(!IS_STREAMING(i->state)) return;
	dist = round_down((int)i->buffer_byte - i->stream_off, i->sample_size);
	if(i->stream_data_byte < dist){
		u8 *p = mem_ptr(i->buffer, 128);
		if(p) memset(p, 0, 128);
	} else {
		int start = i->stream_off + dist, len = (int)i->buffer_byte - start;
		u8 *d = mem_ptr(i->buffer, (u32)(len > 0 ? len : 1));
		const u8 *s = mem_ptr_r(i->buffer + (u32)start, (u32)(len > 0 ? len : 1));
		if(len > 0 && d && s) memmove(d, s, (size_t)len);
	}
}

static u32 set_data(int id, u32 buffer, u32 read_size, u32 buffer_size, u32 file_size, int out_channels, int aa3){
	Atrac *a = &atracs[id];
	Info *i = &a->info;
	TrackInfo t;
	u32 r;
	int extra, num_chunks, skip_count, block_shift;
	memset(&t, 0, sizeof(t));
	if(mem_valid(buffer, 1)){
		u32 n = clamp_valid(buffer, read_size);
		const u8 *p = mem_ptr_r(buffer, n ? n : 1);
		r = aa3 ? parse_aa3(p, n, file_size, &t) : parse_wave(p, n, &t);
		if(r >= 0x80000000u) return r;
	}
	/* InitContextFromTrackInfo */
	i->num_chan = (u8)t.num_chans;
	extra = i->codec == CODEC_AT3 ? 0x45 : 0x170;
	block_shift = (0x100B - i->codec) & 0x1F;
	i->first_valid_sample = extra + (int)t.first_sample_offset;
	i->sample_size = t.block_align;
	/* InitLengthAndLoop */
	{
		int fvs = (int)t.first_sample_offset + extra, n;
		if(t.end_sample == 0) n = i->sample_size ? (int)(t.wave_data_size / i->sample_size) << block_shift : 0;
		else n = (int)t.end_sample + fvs;
		i->decode_pos = fvs;
		i->loop_num = 0;
		i->end_sample = n - 1;
		i->num_skip_frames = (u8)(fvs >> block_shift);
		if((s32)t.loop_start > -1){
			i->loop_end = (int)t.loop_end + extra;
			i->loop_start = (int)t.loop_start + extra;
		} else i->loop_end = i->loop_start = 0;
	}
	i->stream_data_byte = (int)read_size - (int)t.data_off;
	i->buffer = buffer;
	i->cur_file_off = (int)t.data_off;
	i->data_off = (int)t.data_off;
	i->file_data_end = (int)(t.wave_data_size + t.data_off);
	i->cur_buffer = 0;
	i->buffer_byte = buffer_size;
	i->stream_off = (int)t.data_off;
	if(i->sample_size == 0 || i->sample_size > buffer_size) return ERR_BAD_CODEC_PARAMS;
	if(i->loop_end > i->end_sample) return ERR_BAD_CODEC_PARAMS;
	num_chunks = i->end_sample >> block_shift;
	if(!((u32)num_chunks * i->sample_size < t.wave_data_size)) return ERR_BAD_CODEC_PARAMS;
	/* ComputeAtracStateAndInitSecondBuffer */
	if(buffer_size < (u32)i->file_data_end){
		if(i->stream_data_byte < (int)i->sample_size * 2) return ERR_SIZE_TOO_SMALL;
		if(i->loop_end == 0) i->state = ST_STREAMED_WITHOUT_LOOP;
		else if(i->loop_end == i->end_sample) i->state = ST_STREAMED_LOOP_FROM_END;
		else {
			int le = loop_end_file_offset(i, i->loop_end) - i->data_off + 1;
			i->state = ST_STREAMED_LOOP_WITH_TRAILER;
			if(le < i->stream_data_byte) i->stream_data_byte = le;
			i->second_stream_off = 0;
			i->second_buffer = 0;
			i->second_buffer_byte = 0;
		}
	} else i->state = read_size >= (u32)i->file_data_end ? ST_ALL_DATA_LOADED : ST_HALFWAY_BUFFER;
	if(i->codec != CODEC_AT3){ a->format1 = t.sample_size_maybe; a->format2 = t.tail_flag; }
	else {
		static const struct { u16 size; u8 byte, joint; } meta[5] = {
			{ 0x180, 0x04, 0 }, { 0x130, 0x06, 0 }, { 0x0C0, 0x0B, 1 }, { 0x0C0, 0x0E, 0 }, { 0x098, 0x0F, 0 },
		};
		int k;
		for(k = 4; k >= 0; k--)
			if(meta[k].size == i->sample_size && meta[k].joint == t.sample_size_maybe){
				a->format1 = meta[k].byte;
				a->format2 = 0;
				break;
			}
	}
	a->output_channels = out_channels;
	r = skip_frames(a, &skip_count);
	a->skipped_on_set_data = skip_count;
	wrap_last_packet(a);
	return r;
}

static void reset_buffer_info(const Info *i, int pos, u32 out[8]){
	memset(out, 0, 8 * sizeof(u32));
	out[0] = out[4] = i->buffer;
	switch(i->state){
	case ST_HALFWAY_BUFFER: {
		int sp = i->data_off + i->stream_data_byte;
		int fo = i->data_off + (pos / samples_per_frame(i) + 1) * i->sample_size;
		out[0] = i->buffer + (u32)sp;
		out[1] = (u32)(i->file_data_end - sp);
		out[2] = (u32)(fo - sp > 0 ? fo - sp : 0);
		out[3] = (u32)sp;
		break;
	}
	case ST_STREAMED_WITHOUT_LOOP: case ST_STREAMED_LOOP_FROM_END: {
		int fo = file_offset_at(i, pos), be = round_down((int)i->buffer_byte, i->sample_size);
		out[1] = (u32)(i->file_data_end - fo < be ? i->file_data_end - fo : be);
		out[2] = (u32)((skip_frames_at(i, pos) + 1) * i->sample_size);
		out[3] = (u32)fo;
		break;
	}
	case ST_STREAMED_LOOP_WITH_TRAILER: {
		int so = file_offset_at(i, pos), le = loop_end_file_offset(i, i->loop_end) - 1;
		int be = round_down((int)i->buffer_byte, i->sample_size);
		int skip = (skip_frames_at(i, pos) + 1) * i->sample_size;
		int sbe = round_down((int)i->second_buffer_byte, i->sample_size);
		if(so < le){
			int rem = le - so + 1;
			out[1] = (u32)(be < rem ? be : rem);
			out[2] = (u32)(skip < rem ? skip : rem);
			out[3] = (u32)so;
		} else if(le + sbe <= so){
			out[1] = (u32)(i->file_data_end - so < be ? i->file_data_end - so : be);
			out[2] = (u32)skip;
			out[3] = (u32)so;
		} else if(le + (int)i->second_buffer_byte + 1 < i->file_data_end){
			int eo = le + sbe + 1;
			out[1] = (u32)(i->file_data_end - eo < be ? i->file_data_end - eo : be);
			out[2] = (u32)(so + skip - eo > 0 ? so + skip - eo : 0);
			out[3] = (u32)eo;
		}
		break;
	}
	default:
		break;
	}
}

static u32 reset_play_position(Atrac *a, int pos, int first, int second){
	Info *i = &a->info;
	u32 bi[8];
	reset_buffer_info(i, pos, bi);
	if((u32)first < bi[2] || (u32)first > bi[1]) return ERR_BAD_FIRST_RESET_SIZE;
	if((u32)second < bi[6] || (u32)second > bi[5]) return ERR_BAD_SECOND_RESET_SIZE;
	i->decode_pos = pos;
	i->num_skip_frames = (u8)skip_frames_at(i, pos);
	i->loop_num = 0;
	i->cur_file_off = file_offset_at(i, pos);
	a->codec_err = 0x20B;
	switch(i->state){
	case ST_HALFWAY_BUFFER:
		i->stream_data_byte += first;
		if(i->data_off + i->stream_data_byte >= i->file_data_end) i->state = ST_ALL_DATA_LOADED;
		break;
	case ST_STREAMED_WITHOUT_LOOP: case ST_STREAMED_LOOP_FROM_END:
		i->stream_data_byte = first;
		i->cur_buffer = 0;
		i->stream_off = 0;
		break;
	case ST_STREAMED_LOOP_WITH_TRAILER: {
		int le = loop_end_file_offset(i, i->loop_end);
		if(i->cur_file_off >= le){
			int sb = round_down((int)i->second_buffer_byte, i->sample_size);
			if(i->cur_file_off < le + sb){
				i->stream_data_byte = (le + sb - i->cur_file_off) + first;
				i->cur_buffer = 1;
				i->second_stream_off = i->cur_file_off - le;
			} else {
				i->stream_data_byte = first;
				i->cur_buffer = 2;
				i->stream_off = 0;
			}
		} else {
			i->stream_data_byte = first;
			i->cur_buffer = 0;
			i->stream_off = 0;
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

static int writable_looped(const Info *i, int ls, u32 le){
	u32 w = (u32)(i->cur_file_off + i->stream_data_byte);
	if(w >= le){
		int len = (int)le - ls;
		return len - (int)((w - le) % (u32)len);
	}
	return (int)(le - w);
}

static int inc_and_loop(int cur, int inc, int ls, int le){
	int s = cur + inc;
	return s >= le ? ls + (s - le) % (le - ls) : s;
}

static int wrap_rounded(int off, int size, int add, int grain){
	int s = off + add;
	size = round_down_off(off, size, grain);
	return size <= s ? s - size : s;
}

static void stream_data_info(const Info *i, u32 *wp, u32 *bytes, u32 *roff){
	int soff, used, left_after, pos, left, ls, le;
	switch(i->state){
	case ST_ALL_DATA_LOADED:
		*wp = i->buffer; *bytes = 0; *roff = 0;
		return;
	case ST_HALFWAY_BUFFER: {
		int fo = i->data_off + i->stream_data_byte;
		*wp = i->buffer + (u32)fo;
		*bytes = (u32)(i->file_data_end - fo);
		*roff = (u32)fo;
		return;
	}
	default:
		break;
	}
	soff = i->cur_buffer != 1 ? i->stream_off : 0;
	used = space_used(i);
	left_after = round_down_off(soff, (int)i->buffer_byte, i->sample_size);
	pos = soff + used;
	left = pos >= left_after ? left_after - used : left_after - pos;
	ls = file_offset_at(i, i->loop_start);
	le = loop_end_file_offset(i, i->loop_end);
	if(left < 0) left = 0;
	*wp = i->buffer; *bytes = 0; *roff = 0;
	switch(i->state){
	case ST_STREAMED_WITHOUT_LOOP: {
		int sfo = i->cur_file_off + i->stream_data_byte, n = i->file_data_end - sfo;
		*bytes = (u32)(n < 0 ? 0 : n > left ? left : n);
		if(sfo < i->file_data_end){
			*roff = (u32)sfo;
			*wp = i->buffer + (u32)wrap_rounded(i->stream_off, (int)i->buffer_byte, i->stream_data_byte, i->sample_size);
		}
		break;
	}
	case ST_STREAMED_LOOP_FROM_END: {
		int n = writable_looped(i, ls, (u32)le);
		*bytes = (u32)(n < left ? n : left);
		*roff = (u32)inc_and_loop(i->cur_file_off, i->stream_data_byte, ls, le);
		*wp = i->buffer + (u32)wrap_rounded(i->stream_off, (int)i->buffer_byte, i->stream_data_byte, i->sample_size);
		break;
	}
	case ST_STREAMED_LOOP_WITH_TRAILER:
		if(i->decode_pos <= i->loop_end){
			int n = writable_looped(i, ls, (u32)le);
			*bytes = (u32)(n < left ? n : left);
			*roff = (u32)inc_and_loop(i->cur_file_off, i->stream_data_byte, ls, le);
		} else {
			int sfo = i->cur_file_off + i->stream_data_byte, n = i->file_data_end - sfo;
			*bytes = (u32)(n < 0 ? 0 : n > left ? left : n);
			*roff = sfo < i->file_data_end ? (u32)sfo : 0;
		}
		if(i->decode_pos <= i->loop_end || i->cur_buffer != 1)
			*wp = i->buffer + (u32)wrap_rounded(i->stream_off, (int)i->buffer_byte, i->stream_data_byte, i->sample_size);
		else
			*wp = i->buffer + (u32)wrap_rounded(0, (int)i->buffer_byte, used, i->sample_size);
		break;
	default:
		break;
	}
}

/* --- Llamadas -------------------------------------------------------------------- */

static Atrac *get(int id){
	if(id < 0 || id >= MAX_IDS || !atracs[id].used) return NULL;
	ctx_read(id);
	return &atracs[id];
}

static u32 validate_data(const Atrac *a){
	if(!a) return ERR_BAD_ATRACID;
	if(a->info.state == ST_NO_DATA) return ERR_NO_DATA;
	return 0;
}

static u32 validate_managed(const Atrac *a){
	u32 e = validate_data(a);
	if(e) return e;
	if(a->info.state == ST_LOW_LEVEL) return ERR_IS_LOW_LEVEL;
	if(a->info.state == ST_FOR_SCESAS) return ERR_IS_FOR_SCESAS;
	return 0;
}

static int alloc_id(u32 codec){
	int i;
	for(i = 0; i < max_contexts; i++)
		if(context_types[i] == codec && !atracs[i].used){
			if(!bss) ensure_bss();
			memset(&atracs[i], 0, sizeof(atracs[i]));
			atracs[i].used = 1;
			atracs[i].output_channels = 2;
			atracs[i].info.codec = (u16)codec;
			atracs[i].info.state = ST_NO_DATA;
			ctx_write(i);
			return i;
		}
	return -1;
}

static void free_id(int id){
	memset(&atracs[id], 0, sizeof(atracs[id]));
	if(bss && mem_valid(ctx_addr(id), CTX_SIZE)){
		u8 *p = mem_ptr(ctx_addr(id), CTX_SIZE);
		if(p) memset(p, 0, CTX_SIZE);
	}
}

/* Resultado de un SetData: el hilo espera lo que tarda el ME */
static void set_data_result(int id, u32 ret, u32 value){
	Atrac *a = &atracs[id];
	u32 us;
	if(ret && ret != ERR_API_FAIL){ RETURN(ret); return; }
	if(ret == ERR_API_FAIL){
		us = init_us(a->info.codec, a->info.num_chan == 1) + (a->info.codec == CODEC_AT3PLUS ? 214 : 169);
		RETURN(ret);
	} else {
		us = init_us(a->info.codec, a->info.num_chan == 1) + (u32)a->skipped_on_set_data * frame_us(a);
		RETURN(value);
	}
	ctx_write(id);
	hle_delay_us(me_schedule_job(us));
}

static void sceAtracGetAtracID(void){
	u32 codec = ARG(0);
	int id;
	if(codec != CODEC_AT3 && codec != CODEC_AT3PLUS){ RETURN(ERR_INVALID_CODECTYPE); return; }
	id = alloc_id(codec);
	RETURN(id < 0 ? ERR_NO_ATRACID : (u32)id);
}

static void sceAtracReleaseAtracID(void){
	int id = (int)ARG(0);
	if(!get(id)){ RETURN(ERR_BAD_ATRACID); return; }
	free_id(id);
	RETURN(0);
}

static void set_and_get_id(u32 buffer, u32 read_size, u32 buffer_size, u32 file_size, int mono, int aa3){
	Track t;
	u32 r = aa3 ? analyze_aa3(buffer, read_size, &t) : analyze_track(buffer, read_size, &t);
	int id;
	if(r){ RETURN(r); return; }
	if(mono && t.channels != 1){ RETURN(ERR_NOT_MONO); return; }
	id = alloc_id(t.codec);
	if(id < 0){ RETURN(ERR_NO_ATRACID); return; }
	r = set_data(id, buffer, read_size, buffer_size, file_size, mono ? 1 : 2, aa3);
	if(r && r != ERR_NOT_MONO){
		u32 us = init_us(t.codec, t.channels == 1) + (t.codec == CODEC_AT3PLUS ? 214 : 169);
		free_id(id);
		RETURN(r);
		if(r == ERR_API_FAIL) hle_delay_us(me_schedule_job(us));
		return;
	}
	set_data_result(id, 0, (u32)id);
}

static void set_on_id(int id, u32 buffer, u32 read_size, u32 buffer_size, int mono){
	Atrac *a = get(id);
	Track t;
	u32 r;
	if(!a){ RETURN(ERR_BAD_ATRACID); return; }
	if(read_size > buffer_size){ RETURN(ERR_INCORRECT_READ_SIZE); return; }
	r = analyze_track(buffer, read_size, &t);
	if(r){ RETURN(r); return; }
	if(!mono && t.codec != context_types[id]){ RETURN(ERR_WRONG_CODECTYPE); return; }
	r = set_data(id, buffer, read_size, buffer_size, 0, mono ? 1 : 2, 0);
	if(mono && r == ERR_NOT_MONO) r = 0;
	set_data_result(id, r, r);
}

static void sceAtracSetData(void){ set_on_id((int)ARG(0), ARG(1), ARG(2), ARG(2), 0); }
static void sceAtracSetHalfwayBuffer(void){ set_on_id((int)ARG(0), ARG(1), ARG(2), ARG(3), 0); }
static void sceAtracSetMOutData(void){ set_on_id((int)ARG(0), ARG(1), ARG(2), ARG(2), 1); }
static void sceAtracSetMOutHalfwayBuffer(void){ set_on_id((int)ARG(0), ARG(1), ARG(2), ARG(3), 1); }

static void sceAtracSetDataAndGetID(void){
	u32 size = (s32)ARG(1) < 0 ? 0x10000000u : ARG(1);
	set_and_get_id(ARG(0), size, size, 0, 0, 0);
}

static void sceAtracSetHalfwayBufferAndGetID(void){
	if(ARG(1) > ARG(2)){ RETURN(ERR_INCORRECT_READ_SIZE); return; }
	set_and_get_id(ARG(0), ARG(1), ARG(2), 0, 0, 0);
}

static void sceAtracSetMOutDataAndGetID(void){ set_and_get_id(ARG(0), ARG(1), ARG(1), 0, 1, 0); }

static void sceAtracSetMOutHalfwayBufferAndGetID(void){
	if(ARG(1) > ARG(2)){ RETURN(ERR_INCORRECT_READ_SIZE); return; }
	set_and_get_id(ARG(0), ARG(1), ARG(2), 0, 1, 0);
}

static void sceAtracSetAA3DataAndGetID(void){ set_and_get_id(ARG(0), ARG(1), ARG(1), ARG(2), 0, 1); }

static void sceAtracSetAA3HalfwayBufferAndGetID(void){
	if(ARG(1) > ARG(2)){ RETURN(ERR_INCORRECT_READ_SIZE); return; }
	set_and_get_id(ARG(0), ARG(1), ARG(2), ARG(3), 0, 1);
}

static void sceAtracDecodeData(void){
	int id = (int)ARG(0);
	Atrac *a = get(id);
	u32 out = ARG(1), num_addr = ARG(2), fin_addr = ARG(3), rem_addr = ARG(4), r = 0, e;
	int num = 0, finish = 0, tries, k;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(out & 1){ RETURN(ERR_BAD_ALIGNMENT); return; }
	if(out && !mem_valid(out, 1)){ RETURN(ERR_SIZE_TOO_SMALL); return; }
	tries = a->info.num_skip_frames + 1;
	for(k = 0; k < tries; k++){
		r = decode_internal(a, out, &num, &finish);
		if(r){ num = 0; break; }
	}
	if(mem_valid(num_addr, 4)) mem_write32(num_addr, (u32)num);
	if(mem_valid(fin_addr, 4)) mem_write32(fin_addr, (u32)finish);
	if(!r && mem_valid(rem_addr, 4)) mem_write32(rem_addr, (u32)remaining_frames(&a->info));
	ctx_write(id);
	RETURN(r);
	if(r == 0 || r == ERR_API_FAIL) hle_delay_us(me_schedule_job(frame_us(a)));
}

static void sceAtracAddStreamData(void){
	int id = (int)ARG(0);
	Atrac *a = get(id);
	Info *i;
	u32 add = ARG(1), e;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	i = &a->info;
	if(i->state == ST_ALL_DATA_LOADED){ RETURN(ERR_ALL_DATA_LOADED); return; }
	if(i->state == ST_HALFWAY_BUFFER){
		int n = i->stream_data_byte + i->data_off + (int)add;
		if(n == i->file_data_end) i->state = ST_ALL_DATA_LOADED;
		else if(n > i->file_data_end){ RETURN(ERR_ADD_DATA_IS_TOO_BIG); return; }
	}
	i->stream_data_byte += (int)add;
	ctx_write(id);
	RETURN(0);
}

static void sceAtracGetStreamDataInfo(void){
	Atrac *a = get((int)ARG(0));
	u32 wp, bytes, roff, e;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	stream_data_info(&a->info, &wp, &bytes, &roff);
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), wp);
	if(mem_valid(ARG(2), 4)) mem_write32(ARG(2), bytes);
	if(mem_valid(ARG(3), 4)) mem_write32(ARG(3), roff);
	RETURN(0);
}

static void sceAtracGetRemainFrame(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	if(!mem_valid(ARG(1), 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	mem_write32(ARG(1), (u32)remaining_frames(&a->info));
	RETURN(0);
}

static void sceAtracGetBufferInfoForResetting(void){
	int id = (int)ARG(0), pos = (int)ARG(1), skipped = 0, k;
	Atrac *a = get(id);
	u32 addr = ARG(2), bi[8], e, r;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	if(!mem_valid(addr, 32)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	if(a->info.state == ST_STREAMED_LOOP_WITH_TRAILER && a->info.second_buffer_byte == 0){ RETURN(ERR_SECOND_BUFFER_NEEDED); return; }
	pos += a->info.first_valid_sample;
	if((u32)pos > (u32)a->info.end_sample){ RETURN(ERR_BAD_SAMPLE); return; }
	reset_buffer_info(&a->info, pos, bi);
	for(k = 0; k < 8; k++) mem_write32(addr + (u32)k * 4, bi[k]);
	/* Sí: esto puede saltarse tramas, como el firmware */
	r = skip_frames(a, &skipped);
	ctx_write(id);
	RETURN(r);
	if(skipped) hle_delay_us(300);
}

static void sceAtracResetPlayPosition(void){
	int id = (int)ARG(0), pos = (int)ARG(1), skipped = 0;
	Atrac *a = get(id);
	u32 e, r;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	if(a->info.state == ST_STREAMED_LOOP_WITH_TRAILER && a->info.second_buffer_byte == 0){ RETURN(ERR_SECOND_BUFFER_NEEDED); return; }
	pos += a->info.first_valid_sample;
	if((u32)pos > (u32)a->info.end_sample){ RETURN(ERR_BAD_SAMPLE); return; }
	r = reset_play_position(a, pos, (int)ARG(2), (int)ARG(3));
	if(!r) r = skip_frames(a, &skipped);
	ctx_write(id);
	RETURN(r);
	if(r){ if(skipped) hle_delay_us(200); }
	else hle_delay_us(3000);
}

static void sceAtracGetSecondBufferInfo(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	if(!mem_valid(ARG(1), 4) || !mem_valid(ARG(2), 4)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	if(a->info.state != ST_STREAMED_LOOP_WITH_TRAILER){
		mem_write32(ARG(1), 0);
		mem_write32(ARG(2), 0);
		RETURN(ERR_SECOND_BUFFER_NOT_NEEDED);
		return;
	}
	{
		int le = loop_end_file_offset(&a->info, a->info.loop_end);
		mem_write32(ARG(1), (u32)le);
		mem_write32(ARG(2), (u32)(a->info.file_data_end - le));
	}
	RETURN(0);
}

static void sceAtracSetSecondBuffer(void){
	int id = (int)ARG(0);
	Atrac *a = get(id);
	Info *i;
	u32 e, sb = ARG(1), size = ARG(2);
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	i = &a->info;
	if((u32)i->sample_size * 3 <= size || (u32)(i->file_data_end - loop_end_file_offset(i, i->loop_end)) <= size){
		if(i->state == ST_STREAMED_LOOP_WITH_TRAILER){
			i->second_buffer = sb;
			i->second_buffer_byte = size;
			i->second_stream_off = 0;
			ctx_write(id);
			RETURN(0);
		} else RETURN(ERR_SECOND_BUFFER_NOT_NEEDED);
		return;
	}
	RETURN(ERR_SIZE_TOO_SMALL);
}

static void sceAtracIsSecondBufferNeeded(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	RETURN(a->info.state == ST_STREAMED_LOOP_WITH_TRAILER ? 1 : 0);
}

static void sceAtracGetSoundSample(void){
	Atrac *a = get((int)ARG(0));
	const Info *i;
	u32 e;
	if((e = validate_managed(a)) != 0){ RETURN(e); return; }
	i = &a->info;
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)(i->end_sample - i->first_valid_sample));
	if(mem_valid(ARG(2), 4)) mem_write32(ARG(2), i->loop_end ? (u32)(i->loop_start - i->first_valid_sample) : 0xFFFFFFFFu);
	if(mem_valid(ARG(3), 4)) mem_write32(ARG(3), i->loop_end ? (u32)(i->loop_end - i->first_valid_sample) : 0xFFFFFFFFu);
	RETURN(0);
}

static void sceAtracSetLoopNum(void){
	int id = (int)ARG(0);
	Atrac *a = get(id);
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(a->info.loop_end <= 0){ RETURN(ERR_NO_LOOP_INFORMATION); return; }
	a->info.loop_num = (s32)ARG(1);
	ctx_write(id);
	RETURN(0);
}

static void sceAtracGetLoopStatus(void){
	Atrac *a = get((int)ARG(0));
	const Info *i;
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	i = &a->info;
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)i->loop_num);
	if(mem_valid(ARG(2), 4))
		mem_write32(ARG(2), i->loop_end == 0 ? 0 : i->loop_num != 0 ? 1 : i->decode_pos <= i->loop_end ? 1 : 0);
	RETURN(0);
}

static void sceAtracGetBitrate(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	int b;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	b = (int)(a->info.sample_size * 352800u / 1000u);
	if(a->info.codec == CODEC_AT3PLUS) b = ((b >> 11) + 8) & 0xFFFFFFF0;
	else b = (b + 511) >> 10;
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)b);
	RETURN(0);
}

static void sceAtracGetChannel(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), a->info.num_chan);
	RETURN(0);
}

static void sceAtracGetOutputChannel(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)a->output_channels);
	RETURN(0);
}

static void sceAtracGetMaxSample(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)samples_per_frame(&a->info));
	RETURN(0);
}

static void sceAtracGetNextSample(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), (u32)next_samples(&a->info));
	RETURN(0);
}

static void sceAtracGetNextDecodePosition(void){
	Atrac *a = get((int)ARG(0));
	const Info *i;
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(!mem_valid(ARG(1), 4)){ RETURN(0); return; }
	i = &a->info;
	if(i->decode_pos > i->end_sample || i->file_data_end - i->cur_file_off < i->sample_size){
		RETURN(ERR_ALL_DATA_DECODED);
		return;
	}
	mem_write32(ARG(1), (u32)(i->decode_pos - i->first_valid_sample));
	RETURN(0);
}

static void sceAtracGetInternalErrorInfo(void){
	Atrac *a = get((int)ARG(0));
	u32 e;
	if((e = validate_data(a)) != 0){ RETURN(e); return; }
	if(mem_valid(ARG(1), 4)) mem_write32(ARG(1), a->codec_err);
	RETURN(0);
}

static void sceAtracGetContextAddress(void){
	int id = (int)ARG(0);
	if(!get(id)){ RETURN(0); return; }
	ctx_write(id);
	RETURN(bss ? ctx_addr(id) : 0);
}

static void sceAtracReinit(void){
	int at3 = (int)ARG(0), at3p = (int)ARG(1), i, next = 0, space = max_contexts;
	for(i = 0; i < MAX_IDS; i++) if(atracs[i].used){ RETURN(SCE_KERNEL_ERROR_BUSY); return; }
	memset(context_types, 0, sizeof(context_types));
	if(at3 == 0 && at3p == 0){
		inited = 0;
		RETURN(0);
		hle_delay_us(200);
		return;
	}
	for(i = 0; i < at3p; i++){ space -= 2; if(space >= 0) context_types[next++] = CODEC_AT3PLUS; }
	for(i = 0; i < at3; i++){ space -= 1; if(space >= 0) context_types[next++] = CODEC_AT3; }
	RETURN(space >= 0 ? 0 : SCE_KERNEL_ERROR_OUT_OF_MEMORY);
	if(!inited && next != 0){ inited = 1; hle_delay_us(400); }
	inited = 1;
}

static void sceAtracLowLevelInitDecoder(void){
	int id = (int)ARG(0);
	Atrac *a = get(id);
	u32 p = ARG(1);
	if(!a){ RETURN(ERR_BAD_ATRACID); return; }
	if(!mem_valid(p, 12)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	a->info.num_chan = (u8)mem_read32(p);
	a->output_channels = (int)mem_read32(p + 4);
	a->info.sample_size = (u16)mem_read32(p + 8);
	a->info.state = ST_LOW_LEVEL;
	a->info.buffer = 0;
	a->info.decode_pos = 0;
	ctx_write(id);
	RETURN(0);
}

static void sceAtracLowLevelDecode(void){
	int id = (int)ARG(0);
	Atrac *a = get(id);
	u32 consumed = ARG(2), samples = ARG(3), bytes = ARG(4);
	int n;
	if(!a){ RETURN(ERR_BAD_ATRACID); return; }
	n = samples_per_frame(&a->info);
	if(mem_valid(samples, (u32)(n * a->output_channels * 2))){
		u8 *p = mem_ptr(samples, (u32)(n * a->output_channels * 2));
		if(p) memset(p, 0, (size_t)(n * a->output_channels * 2));
	}
	if(mem_valid(consumed, 4)) mem_write32(consumed, a->info.sample_size);
	if(mem_valid(bytes, 4)) mem_write32(bytes, (u32)(n * a->output_channels * 2));
	RETURN(0);
	hle_delay_us(me_schedule_job(frame_us(a)));
}

static void atrac_zero(void){ RETURN(0); }

static const HleFunction atrac3plus[] = {
	{ "sceAtracAddStreamData", sceAtracAddStreamData },
	{ "sceAtracDecodeData", sceAtracDecodeData },
	{ "sceAtracEndEntry", atrac_zero },
	{ "sceAtracGetAtracID", sceAtracGetAtracID },
	{ "sceAtracGetBufferInfoForReseting", sceAtracGetBufferInfoForResetting },
	{ "sceAtracGetBufferInfoForResetting", sceAtracGetBufferInfoForResetting },
	{ "sceAtracGetBitrate", sceAtracGetBitrate },
	{ "sceAtracGetChannel", sceAtracGetChannel },
	{ "sceAtracGetLoopStatus", sceAtracGetLoopStatus },
	{ "sceAtracGetInternalErrorInfo", sceAtracGetInternalErrorInfo },
	{ "sceAtracGetMaxSample", sceAtracGetMaxSample },
	{ "sceAtracGetNextDecodePosition", sceAtracGetNextDecodePosition },
	{ "sceAtracGetNextSample", sceAtracGetNextSample },
	{ "sceAtracGetRemainFrame", sceAtracGetRemainFrame },
	{ "sceAtracGetSecondBufferInfo", sceAtracGetSecondBufferInfo },
	{ "sceAtracGetSoundSample", sceAtracGetSoundSample },
	{ "sceAtracGetStreamDataInfo", sceAtracGetStreamDataInfo },
	{ "sceAtracReleaseAtracID", sceAtracReleaseAtracID },
	{ "sceAtracResetPlayPosition", sceAtracResetPlayPosition },
	{ "sceAtracSetHalfwayBuffer", sceAtracSetHalfwayBuffer },
	{ "sceAtracSetSecondBuffer", sceAtracSetSecondBuffer },
	{ "sceAtracSetData", sceAtracSetData },
	{ "sceAtracSetDataAndGetID", sceAtracSetDataAndGetID },
	{ "sceAtracStartEntry", atrac_zero },
	{ "sceAtracSetLoopNum", sceAtracSetLoopNum },
	{ "sceAtracReinit", sceAtracReinit },
	{ "sceAtracIsSecondBufferNeeded", sceAtracIsSecondBufferNeeded },
	{ "sceAtracSetHalfwayBufferAndGetID", sceAtracSetHalfwayBufferAndGetID },
	{ "sceAtracSetMOutHalfwayBuffer", sceAtracSetMOutHalfwayBuffer },
	{ "sceAtracSetMOutData", sceAtracSetMOutData },
	{ "sceAtracSetMOutDataAndGetID", sceAtracSetMOutDataAndGetID },
	{ "sceAtracSetMOutHalfwayBufferAndGetID", sceAtracSetMOutHalfwayBufferAndGetID },
	{ "sceAtracGetOutputChannel", sceAtracGetOutputChannel },
	{ "sceAtracSetAA3DataAndGetID", sceAtracSetAA3DataAndGetID },
	{ "sceAtracSetAA3HalfwayBufferAndGetID", sceAtracSetAA3HalfwayBufferAndGetID },
	{ "_sceAtracGetContextAddress", sceAtracGetContextAddress },
	{ "sceAtracLowLevelInitDecoder", sceAtracLowLevelInitDecoder },
	{ "sceAtracLowLevelDecode", sceAtracLowLevelDecode },
};

const HleLibrary hle_atrac_libs[] = {
	HLE_LIBRARY("sceAtrac3plus", atrac3plus),
	HLE_LIBRARY("sceATRAC3plus_Library", atrac3plus),
};
const u32 hle_atrac_libs_count = sizeof(hle_atrac_libs) / sizeof(hle_atrac_libs[0]);
