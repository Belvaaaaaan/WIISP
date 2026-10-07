/**
 * WIISP - kirk.c
 * AES-128, SHA-1 y los comandos 1, 4 y 7 del KIRK (ver kirk.h).
 *
 * El AES usa tablas T generadas al arrancar (4 KB por dirección) y el
 * descifrado es el "cifrado inverso equivalente" de FIPS-197 (5.3.5): las
 * claves de ronda intermedias pasan por InvMixColumns una vez. Un EBOOT de
 * varios MB se descifra en una fracción de segundo en Broadway.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "loader/kirk.h"
#include "loader/kirk_keys.h"

/* --- AES-128 ---------------------------------------------------------------- */

static u8 sbox[256], isbox[256];
static u32 te[4][256], td[4][256];
static int tables_ready;

static inline u8 xtime(u8 x){ return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static u8 gmul(u8 a, u8 b){
	u8 r = 0;
	while(b){
		if(b & 1) r ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static inline u32 ror8(u32 x){ return (x >> 8) | (x << 24); }

static void make_tables(void){
	int i;
	/* La caja S: inverso en GF(2^8) y la transformación afín */
	for(i = 0; i < 256; i++){
		u8 inv = 0, x, s;
		int j;
		if(i){
			for(j = 1; j < 256; j++) if(gmul((u8)i, (u8)j) == 1){ inv = (u8)j; break; }
		}
		x = inv;
		s = (u8)(x ^ ((x << 1) | (x >> 7)) ^ ((x << 2) | (x >> 6)) ^ ((x << 3) | (x >> 5)) ^ ((x << 4) | (x >> 4)) ^ 0x63);
		sbox[i] = s;
		isbox[s] = (u8)i;
	}
	for(i = 0; i < 256; i++){
		u8 s = sbox[i], is = isbox[i];
		u32 e = ((u32)gmul(s, 2) << 24) | ((u32)s << 16) | ((u32)s << 8) | gmul(s, 3);
		u32 d = ((u32)gmul(is, 14) << 24) | ((u32)gmul(is, 9) << 16) | ((u32)gmul(is, 13) << 8) | gmul(is, 11);
		te[0][i] = e; te[1][i] = ror8(e); te[2][i] = ror8(ror8(e)); te[3][i] = ror8(ror8(ror8(e)));
		td[0][i] = d; td[1][i] = ror8(d); td[2][i] = ror8(ror8(d)); td[3][i] = ror8(ror8(ror8(d)));
	}
	tables_ready = 1;
}

static inline u32 load_be(const u8 *p){ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline void store_be(u8 *p, u32 v){ p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static inline u32 sub_word(u32 w){
	return ((u32)sbox[w >> 24] << 24) | ((u32)sbox[(w >> 16) & 255] << 16) | ((u32)sbox[(w >> 8) & 255] << 8) | sbox[w & 255];
}

void aes_set_key(AesKey *k, const u8 key[16]){
	u32 rcon = 1;
	int i, r;
	if(!tables_ready) make_tables();
	for(i = 0; i < 4; i++) k->ek[i] = load_be(key + 4 * i);
	for(i = 4; i < 44; i++){
		u32 t = k->ek[i - 1];
		if(i % 4 == 0){
			t = sub_word((t << 8) | (t >> 24)) ^ (rcon << 24);
			rcon = xtime((u8)rcon);
		}
		k->ek[i] = k->ek[i - 4] ^ t;
	}
	/* Claves de descifrado: en orden inverso y con InvMixColumns en las
	   rondas intermedias (td[S[b]] deshace la caja S de td) */
	for(r = 0; r <= 10; r++)
		for(i = 0; i < 4; i++){
			u32 w = k->ek[(10 - r) * 4 + i];
			if(r > 0 && r < 10)
				w = td[0][sbox[w >> 24]] ^ td[1][sbox[(w >> 16) & 255]] ^ td[2][sbox[(w >> 8) & 255]] ^ td[3][sbox[w & 255]];
			k->dk[r * 4 + i] = w;
		}
}

void aes_encrypt_block(const AesKey *k, const u8 in[16], u8 out[16]){
	const u32 *rk = k->ek;
	u32 s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1], s2 = load_be(in + 8) ^ rk[2], s3 = load_be(in + 12) ^ rk[3];
	u32 t0, t1, t2, t3;
	int r;
	for(r = 1; r < 10; r++){
		rk += 4;
		t0 = te[0][s0 >> 24] ^ te[1][(s1 >> 16) & 255] ^ te[2][(s2 >> 8) & 255] ^ te[3][s3 & 255] ^ rk[0];
		t1 = te[0][s1 >> 24] ^ te[1][(s2 >> 16) & 255] ^ te[2][(s3 >> 8) & 255] ^ te[3][s0 & 255] ^ rk[1];
		t2 = te[0][s2 >> 24] ^ te[1][(s3 >> 16) & 255] ^ te[2][(s0 >> 8) & 255] ^ te[3][s1 & 255] ^ rk[2];
		t3 = te[0][s3 >> 24] ^ te[1][(s0 >> 16) & 255] ^ te[2][(s1 >> 8) & 255] ^ te[3][s2 & 255] ^ rk[3];
		s0 = t0; s1 = t1; s2 = t2; s3 = t3;
	}
	rk += 4;
	store_be(out, (((u32)sbox[s0 >> 24] << 24) | ((u32)sbox[(s1 >> 16) & 255] << 16) | ((u32)sbox[(s2 >> 8) & 255] << 8) | sbox[s3 & 255]) ^ rk[0]);
	store_be(out + 4, (((u32)sbox[s1 >> 24] << 24) | ((u32)sbox[(s2 >> 16) & 255] << 16) | ((u32)sbox[(s3 >> 8) & 255] << 8) | sbox[s0 & 255]) ^ rk[1]);
	store_be(out + 8, (((u32)sbox[s2 >> 24] << 24) | ((u32)sbox[(s3 >> 16) & 255] << 16) | ((u32)sbox[(s0 >> 8) & 255] << 8) | sbox[s1 & 255]) ^ rk[2]);
	store_be(out + 12, (((u32)sbox[s3 >> 24] << 24) | ((u32)sbox[(s0 >> 16) & 255] << 16) | ((u32)sbox[(s1 >> 8) & 255] << 8) | sbox[s2 & 255]) ^ rk[3]);
}

void aes_decrypt_block(const AesKey *k, const u8 in[16], u8 out[16]){
	const u32 *rk = k->dk;
	u32 s0 = load_be(in) ^ rk[0], s1 = load_be(in + 4) ^ rk[1], s2 = load_be(in + 8) ^ rk[2], s3 = load_be(in + 12) ^ rk[3];
	u32 t0, t1, t2, t3;
	int r;
	for(r = 1; r < 10; r++){
		rk += 4;
		t0 = td[0][s0 >> 24] ^ td[1][(s3 >> 16) & 255] ^ td[2][(s2 >> 8) & 255] ^ td[3][s1 & 255] ^ rk[0];
		t1 = td[0][s1 >> 24] ^ td[1][(s0 >> 16) & 255] ^ td[2][(s3 >> 8) & 255] ^ td[3][s2 & 255] ^ rk[1];
		t2 = td[0][s2 >> 24] ^ td[1][(s1 >> 16) & 255] ^ td[2][(s0 >> 8) & 255] ^ td[3][s3 & 255] ^ rk[2];
		t3 = td[0][s3 >> 24] ^ td[1][(s2 >> 16) & 255] ^ td[2][(s1 >> 8) & 255] ^ td[3][s0 & 255] ^ rk[3];
		s0 = t0; s1 = t1; s2 = t2; s3 = t3;
	}
	rk += 4;
	store_be(out, (((u32)isbox[s0 >> 24] << 24) | ((u32)isbox[(s3 >> 16) & 255] << 16) | ((u32)isbox[(s2 >> 8) & 255] << 8) | isbox[s1 & 255]) ^ rk[0]);
	store_be(out + 4, (((u32)isbox[s1 >> 24] << 24) | ((u32)isbox[(s0 >> 16) & 255] << 16) | ((u32)isbox[(s3 >> 8) & 255] << 8) | isbox[s2 & 255]) ^ rk[1]);
	store_be(out + 8, (((u32)isbox[s2 >> 24] << 24) | ((u32)isbox[(s1 >> 16) & 255] << 16) | ((u32)isbox[(s0 >> 8) & 255] << 8) | isbox[s3 & 255]) ^ rk[2]);
	store_be(out + 12, (((u32)isbox[s3 >> 24] << 24) | ((u32)isbox[(s2 >> 16) & 255] << 16) | ((u32)isbox[(s1 >> 8) & 255] << 8) | isbox[s0 & 255]) ^ rk[3]);
}

void aes_cbc_encrypt(const AesKey *k, const u8 *in, u8 *out, u32 size){
	u8 iv[16] = { 0 }, blk[16];
	u32 n, i;
	for(n = 0; n + 16 <= size; n += 16){
		for(i = 0; i < 16; i++) blk[i] = in[n + i] ^ iv[i];
		aes_encrypt_block(k, blk, iv);
		memcpy(out + n, iv, 16);
	}
}

void aes_cbc_decrypt(const AesKey *k, const u8 *in, u8 *out, u32 size){
	u8 iv[16] = { 0 }, cur[16], plain[16];
	u32 n, i;
	for(n = 0; n + 16 <= size; n += 16){
		memcpy(cur, in + n, 16);   /* antes de escribir: puede solaparse */
		aes_decrypt_block(k, cur, plain);
		for(i = 0; i < 16; i++) out[n + i] = plain[i] ^ iv[i];
		memcpy(iv, cur, 16);
	}
}

/* --- SHA-1 ------------------------------------------------------------------- */

static inline u32 rol(u32 x, int n){ return (x << n) | (x >> (32 - n)); }

static void sha1_block(Sha1 *s, const u8 *p){
	u32 w[80], a, b, c, d, e, t;
	int i;
	for(i = 0; i < 16; i++) w[i] = load_be(p + 4 * i);
	for(i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
	for(i = 0; i < 80; i++){
		u32 f, k;
		if(i < 20){ f = (b & c) | (~b & d); k = 0x5A827999; }
		else if(i < 40){ f = b ^ c ^ d; k = 0x6ED9EBA1; }
		else if(i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else { f = b ^ c ^ d; k = 0xCA62C1D6; }
		t = rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_init(Sha1 *s){
	s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
	s->len = 0;
	s->used = 0;
}

void sha1_update(Sha1 *s, const void *data, u32 len){
	const u8 *p = data;
	s->len += len;
	while(len){
		u32 n = 64 - s->used;
		if(n > len) n = len;
		memcpy(s->buf + s->used, p, n);
		s->used += n;
		p += n;
		len -= n;
		if(s->used == 64){
			sha1_block(s, s->buf);
			s->used = 0;
		}
	}
}

void sha1_final(Sha1 *s, u8 out[20]){
	u64 bits = s->len * 8;
	u8 pad = 0x80, zero = 0, lenb[8];
	int i;
	sha1_update(s, &pad, 1);
	while(s->used != 56) sha1_update(s, &zero, 1);
	for(i = 0; i < 8; i++) lenb[i] = (u8)(bits >> (56 - 8 * i));
	sha1_update(s, lenb, 8);
	for(i = 0; i < 5; i++) store_be(out + 4 * i, s->h[i]);
}

/* --- KIRK -------------------------------------------------------------------- */

static int kirk_aes(u8 *out, const u8 *in, u32 size, int code, int encrypt){
	AesKey k;
	if(code < KIRK_KEY_FIRST || code >= KIRK_KEY_FIRST + KIRK_KEY_COUNT) return -1;
	aes_set_key(&k, kirk_keys[code - KIRK_KEY_FIRST]);
	if(encrypt) aes_cbc_encrypt(&k, in, out, size);
	else aes_cbc_decrypt(&k, in, out, size);
	return 0;
}

int kirk7(u8 *out, const u8 *in, u32 size, int code){ return kirk_aes(out, in, size, code, 0); }
int kirk4(u8 *out, const u8 *in, u32 size, int code){ return kirk_aes(out, in, size, code, 1); }

int kirk_cmd1(u8 *out, const u8 *block, u32 avail){
	AesKey k;
	u8 keys[32];
	u32 data_size, data_offset;
	if(avail < KIRK_CMD1_HEADER_SIZE) return -1;
	if(rd_le32(block + 0x60) != 1) return -2;   /* modo CMD1 */
	data_size = rd_le32(block + 0x70);
	data_offset = rd_le32(block + 0x74);
	if(data_offset > avail - KIRK_CMD1_HEADER_SIZE ||
	   ((data_size + 15) & ~15u) > avail - KIRK_CMD1_HEADER_SIZE - data_offset || data_size > 0x7FFFFFF0u)
		return -3;
	/* La clave AES del bloque va cifrada con la clave del comando 1 */
	aes_set_key(&k, kirk1_key);
	aes_cbc_decrypt(&k, block, keys, 32);
	aes_set_key(&k, keys);
	aes_cbc_decrypt(&k, block + KIRK_CMD1_HEADER_SIZE + data_offset, out, (data_size + 15) & ~15u);
	return 0;
}
