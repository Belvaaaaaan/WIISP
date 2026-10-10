/**
 * WIISP - tests/test_texcache.c
 * Avisos de escritura de memory.c y la caché de texturas de gpu/texcache.c
 * (la parte que decide cuándo hay que volver a leer una textura; el
 * renderizador GX solo corre en el Wii, así que aquí se prueba con un dueño
 * de mentira que cuenta las decodificaciones).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <string.h>
#include "core/memory.h"
#include "gpu/ge.h"
#include "gpu/texcache.h"
#include "test.h"

/* --- Avisos de escritura ------------------------------------------------ */

static int hook_calls;
static void count_hook(u32 off, u32 len, int write){ (void)off; (void)len; (void)write; hook_calls++; }

static void test_write_notes(void){
	u32 s;
	printf("Avisos de escritura\n");

	s = mem_write_stamp_take();
	CHECK(!mem_written_since(0x08900000u, 0x1000, s));
	mem_note_write(0x08900100u, 4);
	CHECK(mem_written_since(0x08900000u, 0x1000, s));
	CHECK(mem_written_since(0x48900000u, 0x200, s));          /* sin caché: la misma memoria */
	CHECK(!mem_written_since(0x08901000u, 0x1000, s));        /* otra página */
	CHECK(!mem_written_since(0x088FF000u, 0x1000, s));
	CHECK(mem_written_since(0x088FFFFCu, 8, s));              /* cruza a la página escrita */

	/* Varios avisos sin nadie que guarde el sello comparten sello */
	s = mem_write_stamp_take();
	mem_note_write(0x08A00000u, 16);
	mem_note_write(0x08A10000u, 16);
	CHECK(mem_written_since(0x08A00000u, 16, s));
	CHECK(mem_written_since(0x08A10000u, 16, s));
	s = mem_write_stamp_take();
	CHECK(!mem_written_since(0x08A00000u, 16, s));
	CHECK(!mem_written_since(0x08A10000u, 16, s));

	/* VRAM: todos los espejos son la misma memoria física */
	s = mem_write_stamp_take();
	mem_note_write(0x04012340u, 4);
	CHECK(mem_written_since(0x04012000u, 0x100, s));
	CHECK(mem_written_since(0x44012000u, 0x100, s));
	CHECK(mem_written_since(0x04412000u, 0x100, s));          /* espejo 2 */
	CHECK(!mem_written_since(0x04013000u, 0x100, s));
	/* Con swizzle, todo su bloque de 64 KB */
	s = mem_write_stamp_take();
	mem_note_write(0x04234560u, 4);
	CHECK(mem_written_since(0x04030000u, 4, s));
	CHECK(mem_written_since(0x0403F000u, 4, s));
	CHECK(!mem_written_since(0x04040000u, 4, s));
	/* La CPU escribiendo en la VRAM avisa sola */
	s = mem_write_stamp_take();
	mem_write32(0x44100000u, 0x12345678u);
	CHECK(mem_written_since(0x04100000u, 4, s));
	CHECK(!mem_written_since(0x04101000u, 4, s));
	/* Fuera de la RAM y la VRAM no hay nada que avisar */
	s = mem_write_stamp_take();
	mem_note_write(0x00010000u, 64);
	mem_note_write(0x0C000000u, 64);
	CHECK(!mem_written_since(0x08800000u, 0x01000000u, s));
	CHECK(!mem_written_since(0x00010000u, 64, s));
	/* Un rango enorme se recorta a la región */
	mem_note_write(0x09FFF000u, 0xFFFFFFFFu);
	CHECK(mem_written_since(0x09FFF800u, 4, s));

	/* Sin rango: solo cuenta */
	{
		u32 h = mem_write_all_hints;
		mem_note_write_all();
		CHECK_EQ(mem_write_all_hints, h + 1);
	}

	/* mem_valid solo comprueba: ni ganchos de la VRAM ni avisos */
	s = mem_write_stamp_take();
	mem_vram_hook = count_hook;
	mem_vram_watch_lo[0] = mem_vram_watch_lo[1] = 0;
	mem_vram_watch_hi[0] = mem_vram_watch_hi[1] = PSP_VRAM_SIZE;
	hook_calls = 0;
	CHECK(mem_valid(0x04000000u, 0x1000));
	CHECK(mem_valid(0x44000000u, PSP_VRAM_SIZE));
	CHECK(!mem_valid(0x04000000u, PSP_VRAM_SIZE + 1));
	CHECK(mem_valid(0x04600000u, 0x100));
	CHECK(!mem_valid(0x041FFFF0u, 0x20));
	CHECK(mem_valid(0x08000000u, PSP_RAM_SIZE_32MB));
	CHECK(!mem_valid(0x08000000u, PSP_RAM_SIZE_32MB + 1));
	CHECK(!mem_valid(0x0A000000u, 4));
	CHECK(mem_valid(0x00013FFCu, 4));
	CHECK(!mem_valid(0x00013FFCu, 8));
	CHECK(!mem_valid(0x0000FFFCu, 4));
	CHECK(!mem_valid(0x04800000u, 4));
	CHECK(mem_valid(0x08800000u, 0));
	CHECK_EQ(hook_calls, 0);
	CHECK(!mem_written_since(0x04000000u, PSP_VRAM_SIZE, s));
	/* Una lectura de verdad sí pasa por el gancho */
	mem_read32(0x04000100u);
	CHECK_EQ(hook_calls, 1);
	mem_vram_hook = NULL;
	mem_vram_watch_hi[0] = mem_vram_watch_hi[1] = 0;

	/* mem_reset: todo cambió */
	s = mem_write_stamp_take();
	mem_reset();
	CHECK(mem_written_since(0x08800000u, 4, s));
	CHECK(mem_written_since(0x04000000u, 4, s));
}

/* --- Hash ------------------------------------------------------------------ */

static void test_hash(void){
	static u8 buf[4096];
	u32 i, h0, misses = 0;
	printf("Hash de texturas\n");
	for(i = 0; i < sizeof(buf); i++) buf[i] = (u8)(i * 7 + (i >> 5));
	h0 = tc_hash(buf, sizeof(buf), 1);
	CHECK_EQ(tc_hash(buf, sizeof(buf), 1), h0);
	CHECK(tc_hash(buf, sizeof(buf), 2) != h0);
	CHECK(tc_hash(buf, sizeof(buf) - 1, 1) != h0);
	/* Cambiar cualquier palabra (o byte) cambia el hash */
	for(i = 0; i < sizeof(buf); i += 13){
		buf[i] ^= 0x5A;
		misses += tc_hash(buf, sizeof(buf), 1) == h0;
		buf[i] ^= 0x5A;
	}
	CHECK_EQ(misses, 0);
	/* Dos palabras intercambiadas entre carriles */
	{
		u8 t[4];
		memcpy(t, buf, 4); memcpy(buf, buf + 4, 4); memcpy(buf + 4, t, 4);
		CHECK(tc_hash(buf, sizeof(buf), 1) != h0);
		memcpy(t, buf, 4); memcpy(buf, buf + 4, 4); memcpy(buf + 4, t, 4);
	}
	/* Colas que no llenan 16 bytes */
	CHECK(tc_hash(buf, 7, 0) != tc_hash(buf + 1, 7, 0));
	CHECK_EQ(tc_hash(buf, 0, 9), tc_hash(buf + 5, 0, 9));
}

/* --- Caché de texturas ------------------------------------------------------- */

static TexCache tc;
static int decodes, gpu_newer_on;
static u32 gpu_newer_lo, gpu_newer_hi;

static int fake_alloc(TexCache *c, void *ctx){
	int i, oldest = -1;
	(void)ctx;
	for(i = 0; i < TC_MAX; i++){
		if(!c->e[i].used) return i;
		if(c->e[i].used_frame != c->frame && (oldest < 0 || c->e[i].used_frame < c->e[oldest].used_frame)) oldest = i;
	}
	if(oldest < 0) oldest = 0;
	tc_free(c, oldest);
	return oldest;
}

static int fake_decode(TexCache *c, void *ctx, int idx){
	(void)c; (void)ctx; (void)idx;
	decodes++;
	return 1;
}

static int fake_gpu_newer(void *ctx, u32 addr, u32 len){
	u32 a = addr & PSP_ADDR_MASK;
	(void)ctx;
	return gpu_newer_on && a < gpu_newer_hi && a + len > gpu_newer_lo;
}

static void cache_reset(int backoff){
	tc_init(&tc);
	tc.alloc = fake_alloc;
	tc.decode = fake_decode;
	tc.gpu_newer = fake_gpu_newer;
	tc.backoff = backoff;
	tc.frame = 100;
	memset(&tc.stats, 0, sizeof(tc.stats));
	decodes = 0;
	gpu_newer_on = 0;
}

/* Textura de 64x64 en 8 bits (4 KB) */
static TcQuery query(u32 addr, u32 clut_hash){
	TcQuery q;
	memset(&q, 0, sizeof(q));
	q.key.addr = addr;
	q.key.fmt = 5;
	q.key.size = 64 | (64 << 16);
	q.key.bufw = 64;
	q.key.clut_hash = clut_hash;
	q.key.levels = 1;
	q.lv_addr[0] = addr;
	q.lv_len[0] = 4096;
	return q;
}

static void fill(u32 addr, u32 len, u8 seed){
	u8 *p = mem_ptr(addr, len);
	u32 i;
	for(i = 0; i < len; i++) p[i] = (u8)(seed + i * 3 + (i >> 7));
}

/* Avanza un cuadro (y un periodo, como al presentar) */
static void next_frame(void){
	tc.frame++;
	ge_sync_domain++;
}

static int fail_decode(TexCache *c, void *ctx, int idx){ (void)c; (void)ctx; (void)idx; return 0; }

/* Un byte que no mire el vistazo de e */
static u32 unsampled_byte(const TcEntry *e){
	u32 off, k;
	for(off = 1; off < e->lv_len[0]; off++){
		for(k = 0; k < e->nsamples; k++)
			if(off >= e->sample_off[k] && off < e->sample_off[k] + 4) break;
		if(k == e->nsamples) return off;
	}
	return 0;
}

static void test_texcache_basic(void){
	TcQuery q = query(0x08A00000u, 0);
	u32 nchk;
	int a, b;
	printf("Cache de texturas: periodos\n");

	fill(q.key.addr, 4096, 1);
	cache_reset(0);
	a = tc_lookup(&tc, &q);
	CHECK(a >= 0);
	CHECK_EQ(decodes, 1);
	nchk = tc.stats.checks;
	/* Mismo periodo: ni se lee */
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(tc.stats.checks, nchk);
	CHECK_EQ(tc.stats.quick, 2);
	/* Periodo nuevo (sin espaciado): se lee una vez */
	ge_sync_domain++;
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(tc.stats.checks, nchk + 1);
	CHECK_EQ(decodes, 1);

	/* Un aviso obliga a comprobar aunque sea el mismo periodo... */
	mem_note_write(q.key.addr + 100, 4);
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(tc.stats.checks, nchk + 2);
	CHECK_EQ(decodes, 1);                    /* ...y no había cambiado */
	/* Cambia con aviso: nueva decodificación, no inestable */
	fill(q.key.addr, 4096, 2);
	mem_note_write(q.key.addr, 4096);
	b = tc_lookup(&tc, &q);
	CHECK(b >= 0 && b != a);
	CHECK_EQ(decodes, 2);
	CHECK_EQ(tc.stats.changes_notified, 1);
	CHECK(!tc.e[b].unstable);
	CHECK(!tc.e[a].current && tc.e[a].used);
	/* Vuelve el contenido anterior: se reutiliza su copia */
	fill(q.key.addr, 4096, 1);
	mem_note_write(q.key.addr, 4096);
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(decodes, 2);
	CHECK(tc.e[a].current && !tc.e[b].current);

	/* Paleta distinta: otra textura */
	{
		TcQuery q2 = query(q.key.addr, 0x1234);
		int c = tc_lookup(&tc, &q2);
		CHECK(c >= 0 && c != a && c != b);
		CHECK_EQ(decodes, 3);
		CHECK_EQ(tc_lookup(&tc, &q), a);
		CHECK_EQ(tc_lookup(&tc, &q2), c);
		CHECK_EQ(decodes, 3);
	}

	/* La CPU la cambia sin avisar: se ve al empezar otro periodo */
	fill(q.key.addr, 4096, 3);
	CHECK_EQ(tc_lookup(&tc, &q), a);          /* mismo periodo: como antes */
	ge_sync_domain++;
	b = tc_lookup(&tc, &q);
	CHECK(b != a);
	CHECK_EQ(decodes, 4);
	CHECK_EQ(tc.stats.changes_silent, 1);
	CHECK(tc.e[b].unstable);

	/* Muchos sceKernelDcacheWritebackAll también obligan */
	nchk = tc.stats.checks;
	{
		int i;
		for(i = 0; i < TC_ALL_HINTS - 1; i++) mem_note_write_all();
		tc_lookup(&tc, &q);
		CHECK_EQ(tc.stats.checks, nchk);
		mem_note_write_all();
		tc_lookup(&tc, &q);
		CHECK_EQ(tc.stats.checks, nchk + 1);
	}
}

static void test_texcache_backoff(void){
	TcQuery q = query(0x08B00000u, 0);
	static const u32 expect[] = { 1, 3, 7, 15, 31 };
	u32 f, first, last_check = 0, n = 0;
	int a, i;
	printf("Cache de texturas: espaciado\n");

	fill(q.key.addr, 4096, 9);
	cache_reset(1);
	a = tc_lookup(&tc, &q);
	first = tc.frame;
	/* Sin cambios, comprueba en los cuadros +1, +3, +7, +15, +31 y luego
	   cada 16 (con un desfase según la casilla) */
	for(f = 0; f < 120; f++){
		u32 c = tc.stats.checks;
		next_frame();
		CHECK_EQ(tc_lookup(&tc, &q), a);
		if(tc.stats.checks != c){
			if(n < sizeof(expect) / sizeof(expect[0])) CHECK_EQ(tc.frame - first, expect[n]);
			else CHECK_EQ(tc.frame - last_check, TC_CHECK_MAX + (u32)(a & 3));
			last_check = tc.frame;
			n++;
		}
	}
	CHECK(n >= 8);
	CHECK_EQ(decodes, 1);

	/* Un cambio sin aviso en una palabra del vistazo se ve enseguida (en un
	   cuadro sin comprobación) */
	{
		TcEntry *e = &tc.e[a];
		u8 *p;
		while((s32)(e->next_check - (tc.frame + 1)) <= 0){ next_frame(); tc_lookup(&tc, &q); }
		p = mem_ptr(q.key.addr + e->sample_off[3], 4);
		next_frame();
		p[0] ^= 0xFF;
		CHECK(tc_lookup(&tc, &q) != a);
		CHECK_EQ(decodes, 2);
		CHECK_EQ(tc.stats.sample_misses, 1);
		CHECK_EQ(tc.stats.changes_silent, 1);
	}
	/* ...y desde entonces se comprueba en cada periodo (inestable) */
	{
		int b = tc_lookup(&tc, &q);
		u32 c = tc.stats.checks;
		CHECK(tc.e[b].unstable);
		for(i = 0; i < 5; i++){ next_frame(); tc_lookup(&tc, &q); }
		CHECK_EQ(tc.stats.checks, c + 5);
		/* Tras TC_CALM comprobaciones sin cambios vuelve al espaciado */
		for(i = 0; i < TC_CALM; i++){ next_frame(); tc_lookup(&tc, &q); }
		CHECK(!tc.e[b].unstable);
		c = tc.stats.checks;
		for(i = 0; i < 10; i++){ next_frame(); tc_lookup(&tc, &q); }
		CHECK(tc.stats.checks < c + 10);
	}

	/* Un cambio sin aviso fuera del vistazo espera a su comprobación (como
	   mucho TC_CHECK_MAX + 3 cuadros) */
	{
		int b = tc_lookup(&tc, &q), seen = -1;
		TcEntry *e = &tc.e[b];
		/* Que el espaciado esté en su tope */
		for(i = 0; i < 80; i++){ next_frame(); tc_lookup(&tc, &q); }
		CHECK(e->interval == TC_CHECK_MAX);
		mem_ptr(q.key.addr, 4096)[unsampled_byte(e)] ^= 0x01;
		for(i = 0; i < (int)TC_CHECK_MAX + 4 && seen < 0; i++){
			next_frame();
			if(tc_lookup(&tc, &q) != b) seen = i;
		}
		CHECK(seen >= 0);
	}

	/* La GPU tiene bytes más nuevos encima: siempre se comprueba */
	{
		u32 c;
		gpu_newer_on = 1;
		gpu_newer_lo = q.key.addr + 0x800;
		gpu_newer_hi = q.key.addr + 0x900;
		c = tc.stats.checks;
		tc_lookup(&tc, &q);
		tc_lookup(&tc, &q);
		CHECK_EQ(tc.stats.checks, c + 2);
		gpu_newer_on = 0;
		tc_lookup(&tc, &q);
		CHECK_EQ(tc.stats.checks, c + 2);
	}
}

static void test_texcache_misc(void){
	TcQuery q = query(0x08C00000u, 0);
	int a, i, ok;
	printf("Cache de texturas: sin puntero y expulsiones\n");

	/* Sin puntero contiguo: una decodificación por cuadro, como antes */
	cache_reset(1);
	q.lv_len[0] = 0;
	a = tc_lookup(&tc, &q);
	CHECK(a >= 0);
	CHECK_EQ(tc_lookup(&tc, &q), a);
	CHECK_EQ(decodes, 1);
	next_frame();
	tc_lookup(&tc, &q);
	CHECK_EQ(decodes, 2);
	CHECK_EQ(tc.stats.changes_silent, 0);     /* no cuenta como cambio */

	/* Más texturas que casillas: las viejas se expulsan y vuelven */
	cache_reset(1);
	fill(0x08D00000u, 0x100000, 4);
	for(i = 0; i < TC_MAX + 40; i++){
		TcQuery t = query(0x08D00000u + (u32)i * 0x1000, 0);
		next_frame();
		CHECK(tc_lookup(&tc, &t) >= 0);
	}
	CHECK_EQ(decodes, TC_MAX + 40);
	ok = 1;
	for(i = 0; i < TC_MAX + 40; i++){
		TcQuery t = query(0x08D00000u + (u32)i * 0x1000, 0);
		int idx;
		next_frame();
		idx = tc_lookup(&tc, &t);
		if(idx < 0 || !tc.e[idx].current || tc.e[idx].key.addr != t.key.addr) ok = 0;
	}
	CHECK(ok);
	/* Ninguna clave repetida como actual */
	{
		int j, dup = 0;
		for(i = 0; i < TC_MAX; i++)
			for(j = i + 1; j < TC_MAX; j++)
				if(tc.e[i].used && tc.e[j].used && tc.e[i].current && tc.e[j].current &&
				   tc.e[i].key.addr == tc.e[j].key.addr) dup = 1;
		CHECK(!dup);
	}

	/* La decodificación falla: no queda nada a medias */
	cache_reset(0);
	tc.decode = fail_decode;
	q = query(0x08C00000u, 0);
	CHECK_EQ(tc_lookup(&tc, &q), -1);
	{
		int used = 0;
		for(i = 0; i < TC_MAX; i++) used += tc.e[i].used;
		CHECK_EQ(used, 0);
	}
}

void test_texcache(void){
	test_write_notes();
	test_hash();
	test_texcache_basic();
	test_texcache_backoff();
	test_texcache_misc();
}
