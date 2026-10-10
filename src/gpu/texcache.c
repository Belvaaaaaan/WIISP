/**
 * WIISP - gpu/texcache.c
 * Cuándo hay que volver a leer una textura (ver texcache.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <string.h>
#include "gpu/texcache.h"
#include "gpu/ge.h"
#include "core/memory.h"

enum { TC_OK, TC_NOTICE, TC_DUE, TC_SAMPLE };

/* --- Hash ----------------------------------------------------------------- */

#define P1 0x9E3779B1u
#define P2 0x85EBCA77u
#define P3 0xC2B2AE3Du

static inline u32 rotl(u32 x, int r){ return (x << r) | (x >> (32 - r)); }
static inline u32 load32(const u8 *p){ u32 v; memcpy(&v, p, 4); return v; }

/* Cuatro carriles independientes de una multiplicación por palabra (como
   xxHash32 pero más barato): cambiar una sola palabra cambia siempre el
   resultado de su carril */
u32 tc_hash(const u8 *p, u32 n, u32 seed){
	u32 a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1, h, i = 0;
	for(; i + 16 <= n; i += 16){
		a = rotl(a + load32(p + i) * P2, 13);
		b = rotl(b + load32(p + i + 4) * P2, 13);
		c = rotl(c + load32(p + i + 8) * P2, 13);
		d = rotl(d + load32(p + i + 12) * P2, 13);
	}
	h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18) + n;
	for(; i + 4 <= n; i += 4) h = rotl(h + load32(p + i) * P3, 17) * P1;
	for(; i < n; i++) h = rotl(h + p[i] * 0x165667B1u, 11) * P1;
	h ^= h >> 15; h *= P2;
	h ^= h >> 13; h *= P3;
	h ^= h >> 16;
	return h;
}

/* --- Tabla --------------------------------------------------------------- */

static u32 key_bucket(const TcKey *k){
	u32 h = k->addr * P1;
	h ^= k->fmt * P2 ^ k->size * P3 ^ k->bufw ^ k->clutfmt ^ k->clut_hash ^ k->mipkey ^ k->levels;
	h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 13;
	return h & (TC_BUCKETS - 1);
}

static int key_eq(const TcKey *a, const TcKey *b){
	return a->addr == b->addr && a->fmt == b->fmt && a->size == b->size && a->bufw == b->bufw &&
	       a->clutfmt == b->clutfmt && a->clut_hash == b->clut_hash && a->mipkey == b->mipkey &&
	       a->levels == b->levels;
}

void tc_init(TexCache *tc){
	int i;
	memset(tc->e, 0, sizeof(tc->e));
	for(i = 0; i < TC_MAX; i++) tc->e[i].hnext = -1;
	for(i = 0; i < TC_BUCKETS; i++) tc->bucket[i] = -1;
}

void tc_free(TexCache *tc, int idx){
	TcEntry *e = &tc->e[idx];
	int *link;
	if(!e->used) return;
	for(link = &tc->bucket[key_bucket(&e->key)]; *link >= 0; link = &tc->e[*link].hnext)
		if(*link == idx){ *link = e->hnext; break; }
	memset(e, 0, sizeof(*e));
	e->hnext = -1;
}

int tc_unstable_count(const TexCache *tc){
	int i, n = 0;
	for(i = 0; i < TC_MAX; i++) n += tc->e[i].used && tc->e[i].current && tc->e[i].unstable;
	return n;
}

/* --- Comprobar ------------------------------------------------------------ */

/* Puntero a los bytes sin pasar por los ganchos de la VRAM (solo cuando se
   sabe que la GPU no tiene nada más nuevo encima) */
static const u8 *peek(u32 addr, u32 len){
	u32 a = addr & PSP_ADDR_MASK;
	if(a >= PSP_RAM_BASE) return mem_ptr_r(a, len);
	if(a >= PSP_VRAM_BASE && a < PSP_VRAM_MIRROR_END && !(((a - PSP_VRAM_BASE) >> 21) & 1)){
		u32 off = (a - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1);
		return len <= PSP_VRAM_SIZE - off ? psp_mem.vram + off : NULL;
	}
	return NULL;
}

/* El vistazo: la primera y la última palabra del nivel 0 y seis repartidas
   entre ellas, en sitios distintos en cada comprobación */
static void take_samples(TcEntry *e){
	u32 len = e->lv_len[0] & ~3u, n, step, jit, k;
	const u8 *p = e->direct ? peek(e->lv_addr[0], len) : NULL;
	e->nsamples = 0;
	if(!p || len < 4) return;
	n = len / 4 < TC_SAMPLES ? len / 4 : TC_SAMPLES;
	step = (len / n) & ~3u;
	jit = n > 2 && step > 4 ? ((e->checks * P1) >> 8) % (step / 4) * 4 : 0;
	for(k = 0; k < n; k++){
		u32 off = k == 0 ? 0 : k == n - 1 ? len - 4 : k * step + jit;
		if(off > len - 4) off = len - 4;
		e->sample_off[k] = off;
		e->sample[k] = load32(p + off);
	}
	e->nsamples = n;
}

static int samples_match(const TcEntry *e){
	const u8 *p = peek(e->lv_addr[0], e->lv_len[0] & ~3u);
	u32 k;
	if(!p) return 0;
	for(k = 0; k < e->nsamples; k++)
		if(load32(p + e->sample_off[k]) != e->sample[k]) return 0;
	return 1;
}

static int need_check(TexCache *tc, const TcEntry *e){
	u32 i;
	if(!e->direct) return TC_DUE;
	if(mem_write_all_hints - e->all_hints >= TC_ALL_HINTS) return TC_NOTICE;
	for(i = 0; i < e->key.levels; i++){
		if(mem_written_since(e->lv_addr[i], e->lv_len[i], e->stamp)) return TC_NOTICE;
		if(tc->gpu_newer && tc->gpu_newer(tc->ctx, e->lv_addr[i], e->lv_len[i])) return TC_NOTICE;
	}
	if(e->domain == ge_sync_domain) return TC_OK;
	if(e->unstable || !tc->backoff || (s32)(tc->frame - e->next_check) >= 0) return TC_DUE;
	if(!samples_match(e)) return TC_SAMPLE;
	return TC_OK;
}

/* Lee todos los niveles. Sin puntero contiguo (espejos con swizzle) el
   hash cambia en cada cuadro: se decodifica de nuevo, como antes. */
static u32 hash_levels(TexCache *tc, const TcQuery *q, int *direct){
	u32 h = 0x811C9DC5u, i;
	*direct = 1;
	for(i = 0; i < q->key.levels; i++){
		const u8 *p = q->lv_len[i] ? mem_ptr_r(q->lv_addr[i], q->lv_len[i]) : NULL;
		if(p){
			h = tc_hash(p, q->lv_len[i], h);
			tc->stats.check_bytes += q->lv_len[i];
		} else {
			h ^= tc->frame * P1;
			*direct = 0;
		}
	}
	return h;
}

/* Recién comprobada en este cuadro y periodo */
static void verified(TexCache *tc, TcEntry *e, const TcQuery *q, int idx, u32 stamp, int direct){
	e->direct = direct;
	memcpy(e->lv_addr, q->lv_addr, sizeof(e->lv_addr));
	memcpy(e->lv_len, q->lv_len, sizeof(e->lv_len));
	e->stamp = stamp;
	e->all_hints = mem_write_all_hints;
	e->domain = ge_sync_domain;
	e->used_frame = tc->frame;
	e->checks++;
	e->next_check = tc->frame + e->interval + (e->interval >= TC_CHECK_MAX ? (u32)(idx & 3) : 0);
	take_samples(e);
}

int tc_lookup(TexCache *tc, const TcQuery *q){
	u32 b = key_bucket(&q->key), hash, stamp;
	int i, cur = -1, idx = -1, why = TC_DUE, direct, unstable = 0;
	TcEntry *e;

	tc->stats.lookups++;
	for(i = tc->bucket[b]; i >= 0; i = tc->e[i].hnext)
		if(tc->e[i].current && key_eq(&tc->e[i].key, &q->key)){ cur = i; break; }
	if(cur >= 0){
		why = need_check(tc, &tc->e[cur]);
		if(why == TC_OK){
			tc->e[cur].used_frame = tc->frame;
			tc->stats.quick++;
			return cur;
		}
		if(why == TC_SAMPLE) tc->stats.sample_misses++;
	}

	/* Leerla entera. Puede bajar framebuffers de la GPU (avisos y periodo
	   nuevo): el sello y el periodo se toman después. */
	hash = hash_levels(tc, q, &direct);
	stamp = mem_write_stamp_take();
	tc->stats.checks++;
	for(i = tc->bucket[b]; i >= 0; i = tc->e[i].hnext)
		if(tc->e[i].hash == hash && key_eq(&tc->e[i].key, &q->key)){ idx = i; break; }

	if(idx >= 0 && idx == cur){
		/* Sin cambios: se espacia la próxima vez */
		e = &tc->e[idx];
		if(e->unstable){
			if(++e->calm >= TC_CALM){ e->unstable = 0; e->interval = 1; }
		} else if(tc->backoff){
			e->interval = e->interval ? e->interval * 2 : 1;
			if(e->interval > TC_CHECK_MAX) e->interval = TC_CHECK_MAX;
		} else e->interval = 1;
		verified(tc, e, q, idx, stamp, direct);
		return idx;
	}

	if(cur >= 0){
		/* Cambió. Sin aviso, se vigila en cada periodo a partir de ahora. */
		TcEntry *old = &tc->e[cur];
		if(old->direct && direct){
			if(why == TC_NOTICE) tc->stats.changes_notified++;
			else tc->stats.changes_silent++;
			unstable = old->unstable || why != TC_NOTICE;
		}
		old->current = 0;
	}
	if(idx < 0){
		/* Contenido nuevo */
		idx = tc->alloc(tc, tc->ctx);
		if(idx < 0) return -1;
		e = &tc->e[idx];
		memset(e, 0, sizeof(*e));
		e->used = 1;
		e->key = q->key;
		e->hash = hash;
		e->used_frame = tc->frame;
		e->hnext = tc->bucket[b];
		tc->bucket[b] = idx;
		if(!tc->decode(tc, tc->ctx, idx)){ tc_free(tc, idx); return -1; }
		tc->stats.decodes++;
	}
	e = &tc->e[idx];
	e->current = 1;
	e->unstable = unstable;
	e->calm = 0;
	e->interval = 1;
	verified(tc, e, q, idx, stamp, direct);
	return idx;
}
