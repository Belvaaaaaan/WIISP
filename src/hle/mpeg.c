/**
 * WIISP - mpeg.c
 * HLE de sceMpeg: los videos PMF (H.264 + ATRAC3plus) de los juegos.
 *
 * La interfaz sigue a PPSSPP (Core/HLE/sceMpeg.cpp, (c) 2012- PPSSPP
 * Project, GPLv2+): el contexto, el ringbuffer en la memoria de la PSP, el
 * callback del juego que lo llena desde el archivo, las AUs y los tiempos
 * de cada llamada. Lo que no hay es decodificador: los paquetes que el juego
 * mete en el ringbuffer se dan por consumidos al momento y no sale ningún
 * fotograma, así que el video "termina" en cuanto el juego acaba de leer
 * el archivo. Es la forma de saltarse los videos sin que el juego se quede
 * esperando.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "hle/hle.h"
#include "core/memory.h"

#define MAX_CTX 4
#define MAX_STREAMS 16
#define ES_BUFFERS 2

#define PSMF_MAGIC 0x464D5350u
#define PACKET 2048
#define MPEG_AVC_ES_SIZE 2048
#define MPEG_ATRAC_ES_SIZE 2112
#define MPEG_ATRAC_ES_OUTPUT_SIZE 8192
#define MPEG_PCM_ES_SIZE 320
#define MPEG_PCM_ES_OUTPUT_SIZE 320
#define MPEG_MEMSIZE 0x10000

enum { ST_AVC = 0, ST_ATRAC = 1, ST_PCM = 2, ST_DATA = 3, ST_AUDIO = 15 };

#define ERR_NO_DATA         0x80618001u
#define ERR_BAD_VERSION     0x80610002u
#define ERR_IN_INTERRUPT    0x80618008u
#define ERR_NOT_YET_INIT    0x80618009u
#define ERR_NO_MEMORY       0x80610022u
#define ERR_INVALID_VALUE   0x806101FEu
#define ERR_AVC_DECODE_FATAL 0x80628002u
#define SCE_KERNEL_ERROR_ILLEGAL_ADDRESS 0x8002006Au

/* SceMpegRingBuffer */
#define RB_PACKETS     0
#define RB_READ        4
#define RB_WRITE_POS   8
#define RB_AVAIL       12
#define RB_PACKET_SIZE 16
#define RB_DATA        20
#define RB_CALLBACK    24
#define RB_CB_ARG      28
#define RB_UPPER       32
#define RB_SEMA        36
#define RB_MPEG        40
#define RB_GP          44

typedef struct {
	int used;
	u32 handle;        /* lo que el juego guarda en *mpeg */
	u32 ringbuffer;
	u32 stream_offset, stream_size;
	u64 first_ts, last_ts;
	int frame_width;
	int es_buffers[ES_BUFFERS];
	int stream_type[MAX_STREAMS];   /* -1 = libre */
	u32 bytes_put;
	int eof, ended, logged;
} Mpeg;

static Mpeg ctxs[MAX_CTX];
static u32 stream_id_gen;
static int inited;

void mpeg_init(void){
	memset(ctxs, 0, sizeof(ctxs));
	stream_id_gen = 1;
	inited = 0;
}

static Mpeg *get_ctx(u32 mpeg_addr){
	u32 h;
	int i;
	if(!mem_valid(mpeg_addr, 4)) return NULL;
	h = mem_read32(mpeg_addr);
	for(i = 0; i < MAX_CTX; i++) if(ctxs[i].used && ctxs[i].handle == h) return &ctxs[i];
	return NULL;
}

static Mpeg *ctx_of_ringbuffer(u32 rb){
	return mem_valid(rb, 48) ? get_ctx(mem_read32(rb + RB_MPEG)) : NULL;
}

static u32 rd_be32(const u8 *p){ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static u64 timestamp(const u8 *p){
	return ((u64)p[0] << 40) | ((u64)p[1] << 32) | ((u64)p[2] << 24) | ((u64)p[3] << 16) | ((u64)p[4] << 8) | p[5];
}

static int version_ok(u32 raw){
	return raw == 0x32313030u || raw == 0x33313030u || raw == 0x34313030u || raw == 0x35313030u;
}

/* La cabecera PSMF: 0 si es válida (y rellena el contexto si hay uno) */
static u32 analyze(u32 addr, Mpeg *m, u32 *offset, u32 *size){
	const u8 *b = mem_ptr_r(addr, 0x60);
	if(!b) return ERR_INVALID_VALUE;
	*offset = rd_be32(b + 8);
	*size = rd_be32(b + 12);
	if(rd_le32(b) != PSMF_MAGIC) return ERR_INVALID_VALUE;
	if(!version_ok(rd_le32(b + 4))) return ERR_BAD_VERSION;
	if(m){
		m->stream_offset = *offset;
		m->stream_size = *size;
		m->first_ts = timestamp(b + 0x54);
		m->last_ts = timestamp(b + 0x5A);
	}
	return 0;
}

static void au_write_ts(u32 au, u64 pts, u64 dts){
	if(!mem_valid(au, 24)) return;
	mem_write32(au + 0, (u32)(pts >> 32));
	mem_write32(au + 4, (u32)pts);
	mem_write32(au + 8, (u32)(dts >> 32));
	mem_write32(au + 12, (u32)dts);
}

/* Fin del video: el juego ya metió todo el archivo (o su callback dijo
   que no hay más) */
static int video_end(Mpeg *m){
	if(!m->ended && (m->eof || (m->stream_size && m->bytes_put >= m->stream_size))){
		m->ended = 1;
		if(!m->logged){
			hle_log("[MPEG] video omitido (%u bytes leidos)\n", m->bytes_put);
			m->logged = 1;
		}
	}
	return m->ended;
}

/* --- Llamadas -------------------------------------------------------------------- */

static void sceMpegInit(void){
	inited = 1;
	RETURN(0);
	hle_delay_us(750);
}

static void sceMpegFinish(void){
	inited = 0;
	RETURN(0);
	hle_delay_us(250);
}

static void sceMpegQueryMemSize(void){ RETURN(MPEG_MEMSIZE); }

static void sceMpegRingbufferQueryMemSize(void){ RETURN((u32)((s32)ARG(0) * (104 + PACKET))); }

static void sceMpegRingbufferQueryPackNum(void){ RETURN(ARG(0) / (104 + PACKET)); }

static void sceMpegRingbufferConstruct(void){
	u32 rb = ARG(0), packets = ARG(1), data = ARG(2), size = ARG(3), cb = ARG(4), cb_arg = ARG(5);
	if(!mem_valid(rb, 48)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDRESS); return; }
	if((s32)size < 0){ RETURN(ERR_NO_MEMORY); return; }
	if(packets * (104u + PACKET) > size && packets < 0x00100000u){ RETURN(ERR_NO_MEMORY); return; }
	mem_write32(rb + RB_PACKETS, packets);
	mem_write32(rb + RB_READ, 0);
	mem_write32(rb + RB_WRITE_POS, 0);
	mem_write32(rb + RB_AVAIL, 0);
	mem_write32(rb + RB_PACKET_SIZE, PACKET);
	mem_write32(rb + RB_DATA, data);
	mem_write32(rb + RB_CALLBACK, cb);
	mem_write32(rb + RB_CB_ARG, cb_arg);
	mem_write32(rb + RB_UPPER, data + packets * PACKET);
	mem_write32(rb + RB_MPEG, 0);
	mem_write32(rb + RB_GP, kernel_module_gp());   /* libmpeg 0105 o posterior */
	RETURN(0);
}

static void sceMpegRingbufferDestruct(void){ RETURN(0); }

static void sceMpegCreate(void){
	u32 mpeg = ARG(0), data = ARG(1), size = ARG(2), rb = ARG(3), width = ARG(4), handle;
	int i;
	if(kernel_in_interrupt()){ RETURN(ERR_IN_INTERRUPT); return; }
	if(!mem_valid(mpeg, 4)){ RETURN(0xFFFFFFFFu); return; }
	if(size < MPEG_MEMSIZE){ RETURN(ERR_NO_MEMORY); return; }
	handle = data + 0x30;
	if(mem_valid(rb, 48)){
		u32 ps = mem_read32(rb + RB_PACKET_SIZE);
		mem_write32(rb + RB_AVAIL, ps ? mem_read32(rb + RB_PACKETS) - (mem_read32(rb + RB_UPPER) - mem_read32(rb + RB_DATA)) / ps : 0);
		mem_write32(rb + RB_MPEG, mpeg);
	}
	mem_write32(mpeg, handle);
	if(mem_valid(handle, 24)){
		memcpy(mem_ptr(handle, 12), "LIBMPEG\0" "001\0", 12);
		mem_write32(handle + 12, 0xFFFFFFFFu);
		if(mem_valid(rb, 48)){
			mem_write32(handle + 16, rb);
			mem_write32(handle + 20, mem_read32(rb + RB_UPPER));
		}
	}
	for(i = 0; i < MAX_CTX; i++) if(ctxs[i].used && ctxs[i].handle == handle) break;
	if(i == MAX_CTX) for(i = 0; i < MAX_CTX && ctxs[i].used; i++);
	if(i == MAX_CTX){ RETURN(ERR_NO_MEMORY); return; }
	memset(&ctxs[i], 0, sizeof(ctxs[i]));
	ctxs[i].used = 1;
	ctxs[i].handle = handle;
	ctxs[i].ringbuffer = rb;
	ctxs[i].frame_width = (int)width;
	memset(ctxs[i].stream_type, 0xFF, sizeof(ctxs[i].stream_type));
	RETURN(0);
	hle_delay_us(29000);
}

static void sceMpegDelete(void){
	Mpeg *m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	memset(m, 0, sizeof(*m));
	RETURN(0);
	hle_delay_us(40000);
}

static void sceMpegQueryStreamOffset(void){
	Mpeg *m;
	u32 off, size, r;
	if(!mem_valid(ARG(1), 4) || !mem_valid(ARG(2), 4)){ RETURN(0xFFFFFFFFu); return; }
	m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	r = analyze(ARG(1), m, &off, &size);
	if(!r && ((off & 2047) || off == 0)) r = ERR_INVALID_VALUE;
	mem_write32(ARG(2), r ? 0 : off);
	m->bytes_put = 0;
	m->eof = m->ended = 0;
	RETURN(r);
}

static void sceMpegQueryStreamSize(void){
	u32 off, size, r;
	if(!mem_valid(ARG(0), 4) || !mem_valid(ARG(1), 4)){ RETURN(0xFFFFFFFFu); return; }
	r = analyze(ARG(0), NULL, &off, &size);
	if(r == ERR_BAD_VERSION) r = 0;   /* aquí no se mira la versión */
	if(!r && (off & 2047)) r = ERR_INVALID_VALUE;
	mem_write32(ARG(1), r ? 0 : size);
	RETURN(r);
}

static void sceMpegRegistStream(void){
	Mpeg *m = get_ctx(ARG(0));
	u32 sid;
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	sid = stream_id_gen++;
	m->stream_type[sid % MAX_STREAMS] = (int)ARG(1);
	RETURN(sid);
}

static void sceMpegUnRegistStream(void){
	Mpeg *m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	m->stream_type[ARG(1) % MAX_STREAMS] = -1;
	RETURN(0);
}

static void sceMpegMallocAvcEsBuf(void){
	Mpeg *m = get_ctx(ARG(0));
	int i;
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	for(i = 0; i < ES_BUFFERS; i++)
		if(!m->es_buffers[i]){ m->es_buffers[i] = 1; RETURN((u32)(i + 1)); return; }
	RETURN(0);
}

static void sceMpegFreeAvcEsBuf(void){
	Mpeg *m = get_ctx(ARG(0));
	s32 b = (s32)ARG(1);
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(b == 0){ RETURN(ERR_INVALID_VALUE); return; }
	if(b >= 1 && b <= ES_BUFFERS) m->es_buffers[b - 1] = 0;
	RETURN(0);
}

static void sceMpegInitAu(void){
	Mpeg *m = get_ctx(ARG(0));
	u32 buf = ARG(1), au = ARG(2);
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(mem_valid(au, 24)){
		if(buf >= 1 && buf <= ES_BUFFERS && m->es_buffers[buf - 1]){
			au_write_ts(au, 0, 0);
			mem_write32(au + 16, 0);
			mem_write32(au + 20, MPEG_AVC_ES_SIZE);
		} else {
			au_write_ts(au, 0, (u64)-1);
			mem_write32(au + 16, 0);
			mem_write32(au + 20, MPEG_ATRAC_ES_SIZE);
		}
	}
	RETURN(0);
}

static void query_es_size(u32 es, u32 out){
	Mpeg *m;
	if(!mem_valid(ARG(1), 4) || !mem_valid(ARG(2), 4) || (ARG(1) & 3) || (ARG(2) & 3)){ RETURN(0xFFFFFFFFu); return; }
	m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	mem_write32(ARG(1), es);
	mem_write32(ARG(2), out);
	RETURN(0);
}

static void sceMpegQueryAtracEsSize(void){ query_es_size(MPEG_ATRAC_ES_SIZE, MPEG_ATRAC_ES_OUTPUT_SIZE); }
static void sceMpegQueryPcmEsSize(void){ query_es_size(MPEG_PCM_ES_SIZE, MPEG_PCM_ES_OUTPUT_SIZE); }

static void sceMpegRingbufferAvailableSize(void){
	u32 rb = ARG(0);
	Mpeg *m;
	if(!mem_valid(rb, 48)){ RETURN(SCE_KERNEL_ERROR_ILLEGAL_ADDRESS); return; }
	m = ctx_of_ringbuffer(rb);
	if(!m){ RETURN(ERR_NOT_YET_INIT); return; }
	m->ringbuffer = rb;
	kernel_eat_cycles(2020);
	kernel_reschedule();
	RETURN(mem_read32(rb + RB_PACKETS) - mem_read32(rb + RB_AVAIL));
}

/* Lo que metió el callback del juego en una ronda: se da por consumido al
   instante (no hay decodificador). Devuelve 1 si hay que pedir más. */
static int put_round_done(u32 rb, Mpeg *m, s32 got, s32 want, s32 *total){
	if(got <= 0){
		m->eof = 1;
		if(got < 0 && *total == 0) *total = got;
		return 0;
	}
	if(got > want) got = want;
	mem_write32(rb + RB_READ, mem_read32(rb + RB_READ) + (u32)got);
	mem_write32(rb + RB_WRITE_POS, mem_read32(rb + RB_WRITE_POS) + (u32)got);
	m->bytes_put += (u32)got * PACKET;
	*total += got;
	return got == want;
}

/* Cuántos paquetes pide la siguiente ronda (hasta el final del buffer) */
static s32 put_round_size(u32 rb, s32 num, u32 *dest){
	s32 packets = (s32)mem_read32(rb + RB_PACKETS);
	s32 pos = (s32)(mem_read32(rb + RB_WRITE_POS) % (u32)packets), want = num;
	if(want > packets - pos) want = packets - pos;
	*dest = mem_read32(rb + RB_DATA) + (u32)pos * PACKET;
	return want;
}

/* data: ringbuffer, paquetes que faltan tras esta ronda, los de esta
   ronda y el total metido hasta ahora */
static u32 put_callback_done(u32 ret, u32 *d){
	u32 rb = d[0], dest;
	s32 left = (s32)d[1], want = (s32)d[2], total = (s32)d[3];
	Mpeg *m = ctx_of_ringbuffer(rb);
	if(!m) return total ? (u32)total : 0xFFFFFFFFu;
	if(put_round_done(rb, m, (s32)ret, want, &total) && left > 0){
		u32 next[KERNEL_CALL_DATA] = { rb, 0, 0, (u32)total, 0, 0 };
		want = put_round_size(rb, left, &dest);
		next[1] = (u32)(left - want);
		next[2] = (u32)want;
		if(!kernel_enqueue_call(mem_read32(rb + RB_CALLBACK), dest, (u32)want, mem_read32(rb + RB_CB_ARG),
		                        put_callback_done, next))
			return 0;   /* lo dará la siguiente ronda */
	}
	video_end(m);
	return (u32)total;
}

/* Llama al callback del juego para llenar el ringbuffer. Como PPSSPP, el
   callback corre como código normal del hilo al volver del syscall: suele
   leer del UMD esperando a otro hilo (en GTA, WaitEventFlag a
   UmdStreamThread), así que no puede ejecutarse dentro del syscall. */
static void sceMpegRingbufferPut(void){
	u32 rb = ARG(0), dest;
	s32 num = (s32)ARG(1), avail = (s32)ARG(2), packets, total = 0, cb, cb_arg, want;
	Mpeg *m;
	if(!mem_valid(rb, 48)){ RETURN(0xFFFFFFFFu); return; }
	packets = (s32)mem_read32(rb + RB_PACKETS);
	if(num > avail) num = avail;
	if(num > packets - (s32)mem_read32(rb + RB_AVAIL)) num = packets - (s32)mem_read32(rb + RB_AVAIL);
	if(num <= 0){ RETURN(0); return; }
	m = ctx_of_ringbuffer(rb);
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	cb = mem_read32(rb + RB_CALLBACK);
	cb_arg = mem_read32(rb + RB_CB_ARG);
	if(!cb || packets <= 0){ RETURN(0); return; }
	want = put_round_size(rb, num, &dest);
	{
		u32 d[KERNEL_CALL_DATA] = { rb, (u32)(num - want), (u32)want, 0, 0, 0 };
		if(!kernel_enqueue_call(cb, dest, (u32)want, cb_arg, put_callback_done, d)) return;
	}
	/* Desde una interrupción no se puede: el callback corre en el acto */
	while(num > 0){
		s32 got;
		/* En la pila del hilo que llama, por debajo de lo que esté usando */
		u32 sp = (cpu.r[R_SP] - 0x200) & ~0xFu;
		want = put_round_size(rb, num, &dest);
		got = (s32)kernel_call_guest_sp(cb, sp, dest, (u32)want, cb_arg);
		num -= got > 0 ? (got > want ? want : got) : 0;
		if(!put_round_done(rb, m, got, want, &total)) break;
	}
	video_end(m);
	RETURN((u32)total);
}

static void sceMpegGetAvcAu(void){
	Mpeg *m = get_ctx(ARG(0));
	u32 rb, sid = ARG(1), au = ARG(2);
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	rb = m->ringbuffer;
	if(!mem_valid(rb, 48)){ RETURN(0xFFFFFFFFu); return; }
	if(mem_read32(rb + RB_READ) == 0){
		au_write_ts(au, 0, 0);
		RETURN(ERR_NO_DATA);
		hle_delay_us(2000);
		return;
	}
	if(m->stream_type[sid % MAX_STREAMS] < 0){ RETURN(0xFFFFFFFFu); return; }
	/* Sin decodificador no hay AUs: o se acabó el video o hacen falta datos */
	au_write_ts(au, m->first_ts, video_end(m) ? (u64)-1 : 0);
	RETURN(ERR_NO_DATA);
	hle_delay_us(m->ended ? 100 : 2000);
}

static void sceMpegGetAtracAu(void){
	Mpeg *m = get_ctx(ARG(0));
	u32 au = ARG(2);
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(!mem_valid(m->ringbuffer, 48)){ RETURN(0xFFFFFFFFu); return; }
	au_write_ts(au, 0, video_end(m) ? (u64)-1 : 0);
	RETURN(ERR_NO_DATA);
	hle_delay_us(100);
}

static void sceMpegGetPcmAu(void){
	Mpeg *m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	RETURN(ERR_NO_DATA);
}

static void sceMpegAvcDecode(void){
	Mpeg *m = get_ctx(ARG(0));
	u32 buffer_addr = ARG(3), init_addr = ARG(4);
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(!mem_valid(m->ringbuffer, 48)){ RETURN(0xFFFFFFFFu); return; }
	if(!mem_valid(buffer_addr, 4) || !mem_valid(init_addr, 4)){ RETURN(0xFFFFFFFFu); return; }
	/* Nunca hay un fotograma listo */
	mem_write32(init_addr, 0);
	RETURN(ERR_AVC_DECODE_FATAL);
	hle_delay_us(320);
}

static void sceMpegAvcDecodeStop(void){
	Mpeg *m;
	if(!mem_valid(ARG(2), 4) || !mem_valid(ARG(3), 4)){ RETURN(0xFFFFFFFFu); return; }
	m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	mem_write32(ARG(3), 0);
	RETURN(0);
}

static void sceMpegAvcDecodeYCbCr(void){
	Mpeg *m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(mem_valid(ARG(3), 4)) mem_write32(ARG(3), 0);
	RETURN(ERR_AVC_DECODE_FATAL);
	hle_delay_us(320);
}

static void sceMpegAvcDecodeStopYCbCr(void){
	Mpeg *m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(mem_valid(ARG(2), 4)) mem_write32(ARG(2), 0);
	RETURN(0);
}

static void sceMpegAtracDecode(void){
	Mpeg *m = get_ctx(ARG(0));
	u32 buf = ARG(2);
	if(!m || !mem_valid(buf, 1)){ RETURN(0xFFFFFFFFu); return; }
	if(mem_valid(buf, MPEG_ATRAC_ES_OUTPUT_SIZE)) memset(mem_ptr(buf, MPEG_ATRAC_ES_OUTPUT_SIZE), 0, MPEG_ATRAC_ES_OUTPUT_SIZE);
	RETURN(0);
	hle_delay_us(3000);
}

static void sceMpegFlushAllStream(void){
	Mpeg *m = get_ctx(ARG(0));
	if(!m){ RETURN(0xFFFFFFFFu); return; }
	if(mem_valid(m->ringbuffer, 48)){
		mem_write32(m->ringbuffer + RB_AVAIL, 0);
		mem_write32(m->ringbuffer + RB_READ, 0);
		mem_write32(m->ringbuffer + RB_WRITE_POS, 0);
	}
	RETURN(0);
}

static void mpeg_ok_if_ctx(void){
	RETURN(get_ctx(ARG(0)) ? 0 : 0xFFFFFFFFu);
}

static void sceMpegAvcDecodeMode(void){
	if(!mem_valid(ARG(1), 8)){ RETURN(0xFFFFFFFFu); return; }
	mpeg_ok_if_ctx();
}

static void sceMpegAvcQueryYCbCrSize(void){
	u32 w = ARG(2), h = ARG(3), out = ARG(4);
	if((w & 15) || (h & 15) || w > 480 || h > 272){ RETURN(ERR_INVALID_VALUE); return; }
	if(mem_valid(out, 4)) mem_write32(out, (w / 2) * (h / 2) * 6 + 128);
	RETURN(0);
}

static const HleFunction mpeg[] = {
	{ "sceMpegInit", sceMpegInit },
	{ "sceMpegFinish", sceMpegFinish },
	{ "sceMpegQueryMemSize", sceMpegQueryMemSize },
	{ "sceMpegRingbufferQueryMemSize", sceMpegRingbufferQueryMemSize },
	{ "sceMpegRingbufferQueryPackNum", sceMpegRingbufferQueryPackNum },
	{ "sceMpegRingbufferConstruct", sceMpegRingbufferConstruct },
	{ "sceMpegRingbufferDestruct", sceMpegRingbufferDestruct },
	{ "sceMpegRingbufferAvailableSize", sceMpegRingbufferAvailableSize },
	{ "sceMpegRingbufferPut", sceMpegRingbufferPut },
	{ "sceMpegCreate", sceMpegCreate },
	{ "sceMpegDelete", sceMpegDelete },
	{ "sceMpegQueryStreamOffset", sceMpegQueryStreamOffset },
	{ "sceMpegQueryStreamSize", sceMpegQueryStreamSize },
	{ "sceMpegRegistStream", sceMpegRegistStream },
	{ "sceMpegUnRegistStream", sceMpegUnRegistStream },
	{ "sceMpegMallocAvcEsBuf", sceMpegMallocAvcEsBuf },
	{ "sceMpegFreeAvcEsBuf", sceMpegFreeAvcEsBuf },
	{ "sceMpegInitAu", sceMpegInitAu },
	{ "sceMpegQueryAtracEsSize", sceMpegQueryAtracEsSize },
	{ "sceMpegQueryPcmEsSize", sceMpegQueryPcmEsSize },
	{ "sceMpegGetAvcAu", sceMpegGetAvcAu },
	{ "sceMpegGetAtracAu", sceMpegGetAtracAu },
	{ "sceMpegGetPcmAu", sceMpegGetPcmAu },
	{ "sceMpegAvcDecode", sceMpegAvcDecode },
	{ "sceMpegAvcDecodeStop", sceMpegAvcDecodeStop },
	{ "sceMpegAvcDecodeYCbCr", sceMpegAvcDecodeYCbCr },
	{ "sceMpegAvcDecodeStopYCbCr", sceMpegAvcDecodeStopYCbCr },
	{ "sceMpegAvcDecodeMode", sceMpegAvcDecodeMode },
	{ "sceMpegAvcDecodeFlush", mpeg_ok_if_ctx },
	{ "sceMpegAtracDecode", sceMpegAtracDecode },
	{ "sceMpegFlushAllStream", sceMpegFlushAllStream },
	{ "sceMpegFlushStream", mpeg_ok_if_ctx },
	{ "sceMpegChangeGetAuMode", mpeg_ok_if_ctx },
	{ "sceMpegChangeGetAvcAuMode", mpeg_ok_if_ctx },
	{ "sceMpegAvcCopyYCbCr", mpeg_ok_if_ctx },
	{ "sceMpegAvcCsc", mpeg_ok_if_ctx },
	{ "sceMpegAvcInitYCbCr", mpeg_ok_if_ctx },
	{ "sceMpegAvcQueryYCbCrSize", sceMpegAvcQueryYCbCrSize },
};

const HleLibrary hle_mpeg_libs[] = {
	HLE_LIBRARY("sceMpeg", mpeg),
};
const u32 hle_mpeg_libs_count = sizeof(hle_mpeg_libs) / sizeof(hle_mpeg_libs[0]);
