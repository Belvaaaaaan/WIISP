/**
 * WIISP - inflate.c
 * Deflate, gzip y LZ4 (ver inflate.h).
 *
 * Huffman canónico: para cada código se guardan cuántos símbolos hay de
 * cada longitud y los símbolos en orden; los 9 primeros bits se resuelven
 * con una tabla y los códigos más largos bit a bit. Todo dato de entrada
 * se valida: un archivo roto da un error, nunca una lectura fuera.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "loader/inflate.h"

#define MAXBITS 15
#define FAST_BITS 9

typedef struct {
	u16 count[MAXBITS + 1];
	u16 symbol[320];
	/* Tabla rápida: símbolo << 4 | longitud, 0 = código más largo */
	u16 fast[1 << FAST_BITS];
} Huff;

typedef struct {
	const u8 *in;
	u32 in_size, in_pos;
	u32 bitbuf;
	int bitcnt;
	u32 pad;       /* bytes a cero añadidos tras el final de la entrada */
	u8 *out;
	u32 out_size, out_pos;
} Stream;

/* Se puede leer por adelantado pasado el final (la tabla rápida pide 9
   bits); solo es un error si se llegan a consumir esos bits */
static inline void need(Stream *s, int n){
	while(s->bitcnt < n){
		u32 b = 0;
		if(s->in_pos < s->in_size) b = s->in[s->in_pos++];
		else s->pad++;
		s->bitbuf |= b << s->bitcnt;
		s->bitcnt += 8;
	}
}

static inline int overread(const Stream *s){ return (int)(s->pad * 8) > s->bitcnt; }

static inline u32 bits(Stream *s, int n){
	u32 v;
	if(!n) return 0;
	need(s, n);
	v = s->bitbuf & ((1u << n) - 1);
	s->bitbuf >>= n;
	s->bitcnt -= n;
	return v;
}

/* Construye la tabla. Devuelve 0, o < 0 si el código está sobresuscrito
   (los incompletos se permiten: solo fallan si se usa un código que falta) */
static int build(Huff *h, const u8 *lengths, int n){
	u16 offs[MAXBITS + 1];
	int sym, len, left = 1;
	memset(h->count, 0, sizeof(h->count));
	for(sym = 0; sym < n; sym++) h->count[lengths[sym]]++;
	if(h->count[0] == n){ memset(h->fast, 0, sizeof(h->fast)); return 0; }
	for(len = 1; len <= MAXBITS; len++){
		left <<= 1;
		left -= h->count[len];
		if(left < 0) return -1;
	}
	offs[1] = 0;
	for(len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
	for(sym = 0; sym < n; sym++)
		if(lengths[sym]) h->symbol[offs[lengths[sym]]++] = (u16)sym;

	/* Tabla rápida: los códigos de deflate se leen bit a bit desde el más
	   significativo del código, que en el flujo va primero */
	memset(h->fast, 0, sizeof(h->fast));
	{
		int code = 0, index = 0;
		for(len = 1; len <= FAST_BITS; len++){
			int i;
			for(i = 0; i < h->count[len]; i++, code++, index++){
				/* code con len bits, invertido para indexar por los bits del flujo */
				int rev = 0, b, fill;
				for(b = 0; b < len; b++) rev |= ((code >> b) & 1) << (len - 1 - b);
				for(fill = rev; fill < (1 << FAST_BITS); fill += 1 << len)
					h->fast[fill] = (u16)((h->symbol[index] << 4) | len);
			}
			code <<= 1;
		}
	}
	return 0;
}

static int decode(Stream *s, const Huff *h){
	int code = 0, first = 0, index = 0, len;
	need(s, FAST_BITS);
	{
		u16 e = h->fast[s->bitbuf & ((1 << FAST_BITS) - 1)];
		if(e){
			int l = e & 15;
			s->bitbuf >>= l;
			s->bitcnt -= l;
			return e >> 4;
		}
	}
	for(len = 1; len <= MAXBITS; len++){
		int count;
		code |= (int)bits(s, 1);
		count = h->count[len];
		if(code - count < first) return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(Stream *s, const Huff *lit, const Huff *dist){
	for(;;){
		int sym = decode(s, lit);
		if(sym < 0 || overread(s)) return -2;
		if(sym < 256){
			if(s->out_pos >= s->out_size) return -3;
			s->out[s->out_pos++] = (u8)sym;
		} else if(sym == 256){
			return 0;
		} else {
			u32 len, d;
			sym -= 257;
			if(sym >= 29) return -2;
			len = len_base[sym] + bits(s, len_extra[sym]);
			sym = decode(s, dist);
			if(sym < 0 || sym >= 30) return -2;
			d = dist_base[sym] + bits(s, dist_extra[sym]);
			if(overread(s)) return -2;
			if(d > s->out_pos) return -2;
			if(len > s->out_size - s->out_pos) return -3;
			{
				u8 *o = s->out + s->out_pos;
				const u8 *from = o - d;
				u32 i;
				for(i = 0; i < len; i++) o[i] = from[i];   /* puede solaparse */
			}
			s->out_pos += len;
		}
	}
}

/* Devuelve a la entrada los bytes enteros leídos por adelantado */
static void align_input(Stream *s){
	s->bitbuf >>= s->bitcnt & 7;
	s->bitcnt &= ~7;
	while(s->bitcnt >= 8){
		if(s->pad) s->pad--;
		else s->in_pos--;
		s->bitcnt -= 8;
	}
	s->bitbuf = 0;
}

static int stored(Stream *s){
	u32 len, nlen;
	align_input(s);   /* al límite de byte */
	if(s->in_size - s->in_pos < 4) return -2;
	len = s->in[s->in_pos] | ((u32)s->in[s->in_pos + 1] << 8);
	nlen = s->in[s->in_pos + 2] | ((u32)s->in[s->in_pos + 3] << 8);
	s->in_pos += 4;
	if(len != (~nlen & 0xFFFF)) return -2;
	if(len > s->in_size - s->in_pos) return -2;
	if(len > s->out_size - s->out_pos) return -3;
	memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
	s->in_pos += len;
	s->out_pos += len;
	return 0;
}

static int fixed(Stream *s){
	static Huff lit, dist;
	static int ready;
	if(!ready){
		u8 l[288];
		int i;
		for(i = 0; i < 144; i++) l[i] = 8;
		for(; i < 256; i++) l[i] = 9;
		for(; i < 280; i++) l[i] = 7;
		for(; i < 288; i++) l[i] = 8;
		build(&lit, l, 288);
		for(i = 0; i < 30; i++) l[i] = 5;
		build(&dist, l, 30);
		ready = 1;
	}
	return codes(s, &lit, &dist);
}

static int dynamic(Stream *s){
	static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	u8 lengths[320];
	Huff lencode, lit, dist;
	int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4, i;
	if(nlen > 286 || ndist > 30) return -2;
	for(i = 0; i < ncode; i++) lengths[order[i]] = (u8)bits(s, 3);
	for(; i < 19; i++) lengths[order[i]] = 0;
	if(build(&lencode, lengths, 19)) return -2;
	i = 0;
	while(i < nlen + ndist){
		int sym = decode(s, &lencode), len = 0, rep;
		if(sym < 0 || overread(s)) return -2;
		if(sym < 16){ lengths[i++] = (u8)sym; continue; }
		if(sym == 16){
			if(i == 0) return -2;
			len = lengths[i - 1];
			rep = 3 + (int)bits(s, 2);
		} else if(sym == 17) rep = 3 + (int)bits(s, 3);
		else rep = 11 + (int)bits(s, 7);
		if(i + rep > nlen + ndist) return -2;
		while(rep--) lengths[i++] = (u8)len;
	}
	if(lengths[256] == 0) return -2;
	if(build(&lit, lengths, nlen) || build(&dist, lengths + nlen, ndist)) return -2;
	return codes(s, &lit, &dist);
}

int inflate_raw(const u8 *in, u32 in_size, u8 *out, u32 out_size, u32 *in_used){
	Stream s;
	int last, err;
	memset(&s, 0, sizeof(s));
	s.in = in; s.in_size = in_size;
	s.out = out; s.out_size = out_size;
	do {
		int type;
		last = (int)bits(&s, 1);
		type = (int)bits(&s, 2);
		if(overread(&s)) return -2;
		if(type == 0) err = stored(&s);
		else if(type == 1) err = fixed(&s);
		else if(type == 2) err = dynamic(&s);
		else err = -2;
		if(err) return err;
	} while(!last);
	align_input(&s);
	if(in_used) *in_used = s.in_pos;
	return (int)s.out_pos;
}

int gunzip(const u8 *in, u32 in_size, u8 *out, u32 out_size){
	u32 pos = 10, used;
	u8 flags;
	int n;
	if(in_size < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return -1;
	flags = in[3];
	if(flags & 4){   /* FEXTRA */
		u32 xlen;
		if(pos + 2 > in_size) return -1;
		xlen = in[pos] | ((u32)in[pos + 1] << 8);
		pos += 2 + xlen;
	}
	if(flags & 8){ while(pos < in_size && in[pos]) pos++; pos++; }    /* FNAME */
	if(flags & 16){ while(pos < in_size && in[pos]) pos++; pos++; }   /* FCOMMENT */
	if(flags & 2) pos += 2;                                            /* FHCRC */
	if(pos >= in_size) return -1;
	n = inflate_raw(in + pos, in_size - pos, out, out_size, &used);
	return n;
}

int lz4_decompress(const u8 *in, u32 in_size, u8 *out, u32 out_size){
	u32 ip = 0, op = 0;
	while(ip < in_size){
		u32 token = in[ip++], lit = token >> 4, mlen, off;
		if(lit == 15){
			u32 b;
			do {
				if(ip >= in_size) return -1;
				b = in[ip++];
				lit += b;
			} while(b == 255);
		}
		if(lit > in_size - ip || lit > out_size - op) return -1;
		memcpy(out + op, in + ip, lit);
		ip += lit;
		op += lit;
		if(ip >= in_size) break;   /* la última secuencia no tiene copia */
		if(in_size - ip < 2) return -1;
		off = in[ip] | ((u32)in[ip + 1] << 8);
		ip += 2;
		if(off == 0 || off > op) return -1;
		mlen = (token & 15) + 4;
		if((token & 15) == 15){
			u32 b;
			do {
				if(ip >= in_size) return -1;
				b = in[ip++];
				mlen += b;
			} while(b == 255);
		}
		if(mlen > out_size - op) return -1;
		{
			u32 i;
			for(i = 0; i < mlen; i++) out[op + i] = out[op - off + i];
		}
		op += mlen;
	}
	return (int)op;
}
