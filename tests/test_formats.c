/**
 * WIISP - tests/test_formats.c
 * Pruebas de la criptografía (AES, SHA-1, KIRK), la descompresión y las
 * imágenes de disco.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "core/types.h"
#include "loader/kirk.h"
#include "loader/prx_decrypt.h"
#include "loader/inflate.h"
#include "loader/disc.h"
#include "vectors.h"

static int hex_eq(const u8 *p, const char *hex){
	unsigned i;
	for(i = 0; hex[2 * i]; i++){
		unsigned v;
		if(sscanf(hex + 2 * i, "%2x", &v) != 1 || p[i] != v) return 0;
	}
	return 1;
}

static void test_aes_sha1(void){
	/* FIPS-197, apéndice C.1 */
	static const u8 key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	static const u8 pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	AesKey k;
	u8 ct[16], back[16], buf[64], buf2[64];
	Sha1 s;
	u8 h[20];
	int i;

	printf("AES-128 y SHA-1\n");
	aes_set_key(&k, key);
	aes_encrypt_block(&k, pt, ct);
	CHECK(hex_eq(ct, "69c4e0d86a7b0430d8cdb78070b4c55a"));
	aes_decrypt_block(&k, ct, back);
	CHECK(!memcmp(back, pt, 16));

	/* CBC de ida y vuelta, también descifrando sobre el mismo búfer
	   desplazado hacia atrás (como el comando 1 del KIRK) */
	for(i = 0; i < 64; i++) buf[i] = (u8)(i * 7 + 3);
	aes_cbc_encrypt(&k, buf, buf2, 64);
	CHECK(memcmp(buf, buf2, 64) != 0);
	{
		u8 big[96];
		memcpy(big + 32, buf2, 64);
		aes_cbc_decrypt(&k, big + 32, big, 64);
		CHECK(!memcmp(big, buf, 64));
	}

	sha1_init(&s);
	sha1_update(&s, "abc", 3);
	sha1_final(&s, h);
	CHECK(hex_eq(h, "a9993e364706816aba3e25717850c26c9cd0d89d"));
	sha1_init(&s);
	for(i = 0; i < 1000; i++) sha1_update(&s, "aaaaaaaaaa", 10);  /* 10.000 'a' */
	sha1_final(&s, h);
	CHECK(hex_eq(h, "a080cbda64850abb7b7f67ee875ba068074ff6fe"));
	sha1_init(&s);
	sha1_update(&s, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
	sha1_final(&s, h);
	CHECK(hex_eq(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));

	/* KIRK 7 deshace al 4 con la misma clave */
	CHECK(kirk4(buf2, buf, 64, 0x5C) == 0);
	CHECK(kirk7(buf2, buf2, 64, 0x5C) == 0);
	CHECK(!memcmp(buf, buf2, 64));
	CHECK(kirk7(buf2, buf, 16, 0x10) != 0);   /* clave fuera de la tabla */
}

static void test_inflate(void){
	static u8 out[16384];
	u32 used = 0;
	int n;
	printf("Deflate, gzip y LZ4\n");
	n = inflate_raw(defl_stored, sizeof(defl_stored), out, sizeof(out), &used);
	CHECK_EQ(n, sizeof(defl_plain));
	CHECK(!memcmp(out, defl_plain, sizeof(defl_plain)));
	CHECK_EQ(used, sizeof(defl_stored));
	memset(out, 0, sizeof(out));
	n = inflate_raw(defl_fixed, sizeof(defl_fixed), out, sizeof(out), &used);
	CHECK_EQ(n, sizeof(defl_plain));
	CHECK(!memcmp(out, defl_plain, sizeof(defl_plain)));
	CHECK_EQ(used, sizeof(defl_fixed));
	memset(out, 0, sizeof(out));
	n = inflate_raw(defl_dynamic, sizeof(defl_dynamic), out, sizeof(out), &used);
	CHECK_EQ(n, sizeof(defl_plain));
	CHECK(!memcmp(out, defl_plain, sizeof(defl_plain)));
	memset(out, 0, sizeof(out));
	CHECK_EQ(gunzip(defl_gzip, sizeof(defl_gzip), out, sizeof(out)), sizeof(defl_plain));
	CHECK(!memcmp(out, defl_plain, sizeof(defl_plain)));
	/* Sin sitio, truncado o con basura: error, nunca fuera de los límites */
	CHECK(inflate_raw(defl_dynamic, sizeof(defl_dynamic), out, 100, NULL) < 0);
	CHECK(inflate_raw(defl_dynamic, sizeof(defl_dynamic) / 2, out, sizeof(out), NULL) < 0);
	{
		u8 junk[600];
		int i, k;
		for(k = 0; k < 200; k++){
			for(i = 0; i < (int)sizeof(junk); i++) junk[i] = (u8)((i * 131 + k * 17 + (i >> 3) * k) ^ (k << 2));
			memcpy(junk, defl_dynamic, k % 2 ? 40 : 0);
			inflate_raw(junk, sizeof(junk), out, sizeof(out), NULL);
		}
		CHECK(1);
	}
	/* LZ4: "abcabcabcabc" + "xyz" como 3 literales + copia de 9 y 3 literales */
	{
		static const u8 lz[] = { 0x35, 'a', 'b', 'c', 3, 0, 0x30, 'x', 'y', 'z' };
		n = lz4_decompress(lz, sizeof(lz), out, sizeof(out));
		CHECK_EQ(n, 15);
		CHECK(!memcmp(out, "abcabcabcabcxyz", 15));
		CHECK(lz4_decompress(lz, sizeof(lz), out, 10) < 0);
	}
}

static void test_prx_decrypt(void){
	static u8 buf[sizeof(prx_enc_gz) + sizeof(prx_enc) + 16], out[2048];
	int n;
	printf("Descifrado de PRX\n");
	memcpy(buf, prx_enc, sizeof(prx_enc));
	n = prx_decrypt(buf, buf, sizeof(prx_enc), NULL);
	CHECK_EQ(n, sizeof(prx_plain));
	CHECK(n > 0 && !memcmp(buf, prx_plain, sizeof(prx_plain)));
	/* Con gzip */
	memcpy(buf, prx_enc_gz, sizeof(prx_enc_gz));
	n = prx_decrypt(buf, buf, sizeof(prx_enc_gz), NULL);
	CHECK(n > 0);
	if(n > 0){
		CHECK_EQ(gunzip(buf, (u32)n, out, sizeof(out)), sizeof(prx_plain));
		CHECK(!memcmp(out, prx_plain, sizeof(prx_plain)));
	}
	/* Un bit cambiado en la cabecera: el SHA-1 no coincide */
	memcpy(buf, prx_enc, sizeof(prx_enc));
	buf[0x12] ^= 1;
	CHECK(prx_decrypt(buf, buf, sizeof(prx_enc), NULL) < 0);
	/* Etiqueta desconocida o archivo demasiado corto */
	memcpy(buf, prx_enc, sizeof(prx_enc));
	buf[0xD0] ^= 0x55;
	CHECK(prx_decrypt(buf, buf, sizeof(prx_enc), NULL) < 0);
	CHECK(prx_decrypt(prx_enc, buf, 0x100, NULL) < 0);
	/* Un tamaño de datos que no cabe: error, no lectura fuera */
	memcpy(buf, prx_enc, sizeof(prx_enc));
	CHECK(prx_decrypt(buf, buf, 0x160, NULL) < 0);
}

static int write_temp(const char *path, const u8 *data, u32 len){
	FILE *f = fopen(path, "wb");
	if(!f) return -1;
	fwrite(data, 1, len, f);
	fclose(f);
	return 0;
}

/* La imagen de tools/make_test_vectors.py (test_tree) */
static void check_disc(const char *path){
	DiscEntry e, d;
	u8 *buf;
	u32 size = 0, i;
	int ok;
	CHECK_EQ(disc_open(path), 0);
	if(!disc_is_open()) return;
	CHECK(disc_lookup("PSP_GAME/SYSDIR/EBOOT.BIN", &e) == 0 && !e.is_dir && e.size == 5000);
	buf = disc_read_file("/psp_game/sysdir/eboot.bin", &size, 1 << 20);
	CHECK(buf != NULL && size == 5000);
	if(buf){
		for(ok = 1, i = 0; i < size; i++) if(buf[i] != (u8)(i * 13 + 5)) ok = 0;
		CHECK(ok);
		free(buf);
	}
	buf = disc_read_file("PSP_GAME/USRDIR/DATA/A.TXT", &size, 100);
	CHECK(buf && size == 11 && !memcmp(buf, "hola disco\n", 11));
	free(buf);
	/* Lectura que cruza sectores (y bloques de 4 KB en el ZSO) */
	CHECK(disc_lookup("PSP_GAME/USRDIR/DATA/GRANDE.BIN", &e) == 0 && e.size == 9000);
	{
		u8 part[3000];
		CHECK_EQ(disc_read((u64)e.lba * DISC_SECTOR + 1500, sizeof(part), part), sizeof(part));
		for(ok = 1, i = 0; i < sizeof(part); i++) if(part[i] != (u8)((i + 1500) * 7)) ok = 0;
		CHECK(ok);
	}
	CHECK(disc_lookup("PSP_GAME/USRDIR", &d) == 0 && d.is_dir);
	CHECK(disc_dir_entry(&d, 0, &e) == 1 && !strcmp(e.name, "DATA") && e.is_dir);
	CHECK(disc_dir_entry(&d, 1, &e) == 1 && !strcmp(e.name, "VACIO"));
	CHECK(disc_dir_entry(&d, 2, &e) == 0);
	CHECK(disc_lookup("", &d) == 0 && d.is_dir);
	CHECK(disc_lookup("UMD_DATA.BIN", &e) == 0 && e.size == 21);
	CHECK(disc_lookup("PSP_GAME/NADA", &e) != 0);
	CHECK(disc_lookup("UMD_DATA.BIN/X", &e) != 0);
	{
		u8 tmp[10];
		CHECK(disc_read((u64)disc_sectors() * DISC_SECTOR, 10, tmp) == 0);
	}
	disc_close();
	CHECK(!disc_is_open());
}

static void test_disc(void){
	static u8 iso[1 << 17];
	static u8 bad[sizeof(iso_cso)];
	int n = gunzip(iso_gz, sizeof(iso_gz), iso, sizeof(iso)), k;
	printf("Imagenes de disco\n");
	CHECK(n > 0);
	if(n <= 0) return;
	CHECK(write_temp("build-pc/test.iso", iso, (u32)n) == 0);
	CHECK(write_temp("build-pc/test.cso", iso_cso, sizeof(iso_cso)) == 0);
	CHECK(write_temp("build-pc/test.zso", iso_zso, sizeof(iso_zso)) == 0);
	check_disc("build-pc/test.iso");
	check_disc("build-pc/test.cso");
	check_disc("build-pc/test.zso");
	CHECK(disc_open("build-pc/no-existe.iso") < 0);
	/* Índices y datos corruptos: errores, nunca lecturas fuera */
	for(k = 0; k < 64; k++){
		u32 i;
		memcpy(bad, iso_cso, sizeof(bad));
		for(i = 0; i < 24; i++) bad[(24 + k * 37 + i * 101) % sizeof(bad)] ^= (u8)(k * 29 + i);
		write_temp("build-pc/bad.cso", bad, sizeof(bad));
		if(disc_open("build-pc/bad.cso") == 0){
			DiscEntry e;
			u8 *b;
			u32 sz;
			disc_lookup("PSP_GAME/USRDIR/DATA/GRANDE.BIN", &e);
			b = disc_read_file("PSP_GAME/SYSDIR/EBOOT.BIN", &sz, 1 << 20);
			free(b);
			disc_close();
		}
	}
	CHECK(1);
	remove("build-pc/test.iso"); remove("build-pc/test.cso"); remove("build-pc/test.zso"); remove("build-pc/bad.cso");
}

void test_formats(void){
	test_aes_sha1();
	test_inflate();
	test_prx_decrypt();
	test_disc();
}
