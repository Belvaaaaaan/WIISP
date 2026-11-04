/**
 * WIISP - gpu/ge_rastreo_fb.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "core/memory.h"
#include "core/opt.h"
#include "hle/hle.h"

#define RF_ESCR 64
#define RF_LECT 32
#define RF_MAPAS 4
#define RF_CONTENIDOS 24
#define RF_LINEA 900
#define RF_NADA 0xFFFFFFFFu

typedef struct {
	u32 fb, fmt, fbw, prim, borrado, mrgb, malpha, logica, mezcla, fixa, fixb, alfa_t, color_t, sten, stest, sop, z, tramado;
	u32 tex, texaddr, tfmt, tfunc, colfmt, luces, material, through;
	u32 dibujos, vertices, col_y, col_o, col_ok;
} RfEscr;

typedef struct {
	u32 tex, tfmt, tsize, bufw, swz, clutfmt, clutaddr, fb, fmt, mezcla, fixa, fixb, tfunc, through, donde;
	u32 dibujos, vertices, camino[RF_CAMINOS], muestras, texeles, bits[32];
} RfLect;

static u32 r_off = RF_NADA, r_len = 0x2000;
static unsigned traza_desde, cuadro;
static RfEscr escr[RF_ESCR];
static RfLect lect[RF_LECT];
static u32 n_escr, n_lect, escrituras, lecturas, escr_fuera, lect_fuera, transferencias;
static u32 ev[RG_N], bajadas_motivo[16], bajada_pend, bajadas_con, bajadas_sin;
static int lect_pend = -1;
static char lect_pend_txt[RF_LINEA];
static char ult[RF_LINEA];
static u32 ult_n;
static unsigned mapas, contenidos;
static RfDatos datos;
u32 (*ge_rastreo_fb_donde)(u32 off, u32 len, char *texto, size_t n);

static int en_vram(u32 a){ return ((a & 0x0FFFFFFFu) - PSP_VRAM_BASE) < (PSP_VRAM_MIRROR_END - PSP_VRAM_BASE); }
static u32 off_vram(u32 a){ return ((a & 0x0FFFFFFFu) - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1); }
static u32 dir_tex(void){ return (ge.cmd[GE_TEXADDR0] & 0xFFFFF0u) | ((ge.cmd[GE_TEXBUFWIDTH0] << 8) & 0x0F000000u); }
static int bits_tfmt(u32 tf){
	return tf == GE_TFMT_CLUT4 ? 4 : tf == GE_TFMT_CLUT8 ? 8 : (tf == GE_TFMT_CLUT16 || tf <= GE_FMT_4444) ? 16 : 32;
}
static int traza(void){ return traza_desde && cuadro >= traza_desde && cuadro <= traza_desde + 1; }

int ge_rastreo_fb_activo(void){ return opt_on[OPT_PROF_RASTREO_FB] && r_off != RF_NADA; }

void ge_rastreo_fb_fijar(u32 addr, u32 len){
	r_off = addr ? off_vram(addr) : RF_NADA;
	r_len = len ? len : 0x2000;
}

void ge_rastreo_fb_traza(unsigned desde){ traza_desde = desde; }

int ge_rastreo_fb_toca(u32 off, u32 len){
	if(r_off == RF_NADA || !len) return 0;
	return off < r_off + r_len && r_off < off + len;
}

static void emitir(void){
	if(!ult_n) return;
	if(ult_n > 1) hle_log_quiet("%s (x%u)\n", ult, (unsigned)ult_n);
	else hle_log_quiet("%s\n", ult);
	ult_n = 0;
}

static void linea(const char *t){
	if(ult_n && !strcmp(ult, t)){ ult_n++; return; }
	emitir();
	snprintf(ult, sizeof(ult), "%s", t);
	ult_n = 1;
}

static void cerrar_lectura(int camino){
	static const char *const nc[RF_CAMINOS] = { "sin elegir textura en GX", "copia en la GPU", "paleta en la GPU",
	                                            "decodificada de la VRAM", "de la cache de texturas", "GX no dibuja",
	                                            "VRAM (software)" };
	char t[RF_LINEA + 64];
	if(lect_pend < 0) return;
	if(camino > 0 && camino < RF_CAMINOS) lect[lect_pend].camino[camino]++;
	if(traza() && lect_pend_txt[0]){
		snprintf(t, sizeof(t), "%s | camino: %s", lect_pend_txt, nc[camino > 0 && camino < RF_CAMINOS ? camino : 0]);
		linea(t);
	}
	lect_pend = -1;
	lect_pend_txt[0] = 0;
}

void ge_rastreo_fb_cuadro(unsigned c){
	cerrar_lectura(0);
	emitir();
	if(c != cuadro){ mapas = 0; contenidos = 0; }
	cuadro = c;
}

static size_t desc_escr(const RfEscr *e, char *b, size_t n){
	static const char *const nf[4] = { "565", "5551", "4444", "8888" };
	static const char *const ncol[8] = { "sin color", "?", "?", "?", "565", "5551", "4444", "8888" };
	size_t len = (size_t)snprintf(b, n, "fb %08X %s ancho %u, prim %u, %s, mascara RGB %06X alfa %02X, logica %s %u, mezcla %s %u/%u/%u %06X/%06X, "
	                              "alfa %s %06X, color %s %u, stencil %s %u/%02X/%02X %u/%u/%u, z %s%u%s, tramado %u, textura %s",
	                              (unsigned)e->fb, nf[e->fmt & 3], (unsigned)e->fbw, (unsigned)e->prim, (e->borrado & 1) ? "borrado" : "dibujo",
	                              (unsigned)e->mrgb, (unsigned)e->malpha, (e->logica & 1) ? "si" : "no", (unsigned)(e->logica >> 4),
	                              (e->mezcla & 1) ? "si" : "no", (unsigned)((e->mezcla >> 4) & 0xF), (unsigned)((e->mezcla >> 8) & 0xF),
	                              (unsigned)((e->mezcla >> 12) & 7), (unsigned)e->fixa, (unsigned)e->fixb,
	                              (e->alfa_t & 1) ? "si" : "no", (unsigned)(e->alfa_t >> 4), (e->color_t & 1) ? "si" : "no",
	                              (unsigned)(e->color_t >> 4), e->sten ? "si" : "no", (unsigned)(e->stest & 7), (unsigned)((e->stest >> 8) & 0xFF),
	                              (unsigned)((e->stest >> 16) & 0xFF), (unsigned)(e->sop & 7), (unsigned)((e->sop >> 8) & 7), (unsigned)((e->sop >> 16) & 7),
	                              (e->z & 1) ? "" : "off ", (unsigned)((e->z >> 1) & 7), (e->z & 16) ? " sin escribir" : "",
	                              (unsigned)e->tramado, e->tex ? "si" : "no");
	if(e->tex && len < n)
		len += (size_t)snprintf(b + len, n - len, " %08X fmt %u tfunc %06X", (unsigned)e->texaddr, (unsigned)e->tfmt, (unsigned)e->tfunc);
	if(len < n)
		len += (size_t)snprintf(b + len, n - len, ", vertices %s%s, material %08X, luces %s, color Y %08X O %08X%s",
		                        ncol[e->colfmt & 7], e->through ? " through" : "", (unsigned)e->material, e->luces ? "si" : "no",
		                        (unsigned)e->col_y, (unsigned)e->col_o, e->col_ok ? "" : " (sin leer)");
	if(len < n && !(e->borrado & 1)){
		const u32 m = e->mrgb;
		if(m && m != 0xFFFFFFu) len += (size_t)snprintf(b + len, n - len, "; GX: ignora la mascara parcial (escribe todo el color)");
		if((e->logica & 1) && len < n) len += (size_t)snprintf(b + len, n - len, "; GX: operacion logica sobre el EFB");
	}
	return len;
}

static void desc_donde(u32 d, char *b, size_t n){
	static const char *const nf[5] = { "565", "5551", "4444", "8888", "Z" };
	static const char *const ne[4] = { "igual", "GPU", "VRAM", "?" };
	static const char *const na[4] = { "uniforme", "VRAM", "GPU", "?" };
	size_t len;
	int i, k;
	if(d == RF_NADA){ snprintf(b, n, "sin GX"); return; }
	k = (int)(d & 3);
	len = (size_t)snprintf(b, n, "%d bufer%s", k, k == 1 ? "" : "es");
	for(i = 0; i < 2 && i < k && len < n; i++){
		const u32 s = (d >> (4 + 12 * i)) & 0xFFFu;
		len += (size_t)snprintf(b + len, n - len, "%s %s %s, alfa %s%s%s%s%s", i ? ";" : ":", nf[(s & 7) > 4 ? 4 : (s & 7)],
		                        ne[(s >> 3) & 3], na[(s >> 5) & 3], (s & 0x80) ? ", hueco" : "", (s & 0x100) ? ", misma direccion" : "",
		                        (s & 0x200) ? ", en el EFB" : "", (s & 0x400) ? " (EFB mas nuevo)" : "");
	}
}

//Arreglo
static void anotar_contenido(RfLect *l, const char *origen){
	const u32 tf = ge.cmd[GE_TEXFORMAT] & 0xF, base = dir_tex(), bufw = ge.cmd[GE_TEXBUFWIDTH0] & 0x7FFu;
	const int bits = bits_tfmt(tf);
	u32 tw = 1u << (ge.cmd[GE_TEXSIZE0] & 0xF), th = 1u << ((ge.cmd[GE_TEXSIZE0] >> 8) & 0xF), x, y, cuenta[32], n = 0;
	const u32 b0 = off_vram(base);
	int i, mapa;
	char t[RF_LINEA];
	size_t len;
	if(bufw && tw > bufw) tw = bufw;
	if(tw > 512) tw = 512;
	if(th > 512) th = 512;
	if(ge.cmd[GE_TEXMODE] & 1) return;
	memset(cuenta, 0, sizeof(cuenta));
	mapa = traza() && mapas < RF_MAPAS;
	if(mapa){
		emitir();
		mapas++;
		hle_log_quiet("[RASTREO] c%u mapa %u (%s): textura %08X, bits 0-3 de cada texel en hexadecimal, las primeras %u filas y %u columnas del rango vigilado\n",
		              cuadro, mapas, origen, (unsigned)base, th < 64 ? (unsigned)th : 64u, tw < 64 ? (unsigned)tw : 64u);
	}
	for(y = 0; y < th; y++){
		char fila[72];
		u32 nf = 0;
		for(x = 0; x < tw; x++){
			const u32 bit = (y * bufw + x) * (u32)bits, o = (b0 + bit / 8) & (PSP_VRAM_SIZE - 1);
			const u8 *p = psp_mem.vram + o;
			u32 v;
			if(!ge_rastreo_fb_toca(o, (u32)(bits + 7) / 8)) continue;
			switch(bits){
			case 4: v = (p[0] >> (bit & 4)) & 15; break;
			case 8: v = p[0]; break;
			case 16: v = (u32)p[0] | ((u32)p[1] << 8); break;
			default: v = rd_le32(psp_mem.vram + (o & ~3u)); break;
			}
			n++;
			for(i = 0; i < bits; i++) cuenta[i] += (v >> i) & 1;
			if(mapa && y < 64 && nf < 64) fila[nf++] = "0123456789ABCDEF"[v & 15];
		}
		if(mapa && y < 64 && nf){
			fila[nf] = 0;
			hle_log_quiet("[RASTREO] c%u m%u %02u %s\n", cuadro, mapas, (unsigned)y, fila);
		}
	}
	if(l && n){
		l->muestras++;
		l->texeles += n;
		for(i = 0; i < bits && i < 32; i++) l->bits[i] += cuenta[i];
	}
	if(traza() && contenidos < RF_CONTENIDOS){
		contenidos++;
		len = (size_t)snprintf(t, sizeof(t), "[RASTREO] c%u contenido (%s): textura %08X, %u texeles en el rango; texeles con cada bit (0 en adelante):",
		                       cuadro, origen, (unsigned)base, (unsigned)n);
		for(i = 0; i < bits && i < 32 && len + 8 < sizeof(t); i++) len += (size_t)snprintf(t + len, sizeof(t) - len, " %u", (unsigned)cuenta[i]);
		emitir();
		hle_log_quiet("%s\n", t);
	}
}

//Arreglo
int ge_rastreo_fb_interesa(void){
	u32 fb, fbw, alto, bpp, r = 0;
	if(!ge_rastreo_fb_activo()) return 0;
	fb = ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0u;
	fbw = ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FCu;
	bpp = (ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3) == GE_FMT_8888 ? 4 : 2;
	alto = ((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF) + 1;
	if(ge_rastreo_fb_toca(fb & (PSP_VRAM_SIZE - 1), (fbw ? fbw : 1) * bpp * alto)) r |= 1;
	if((ge.cmd[GE_TEXTUREMAPENABLE] & 1) && !(ge.cmd[GE_CLEARMODE] & 1) && en_vram(dir_tex())){
		const u32 tf = ge.cmd[GE_TEXFORMAT] & 0xF, bufw = ge.cmd[GE_TEXBUFWIDTH0] & 0x7FFu;
		const u32 th = 1u << ((ge.cmd[GE_TEXSIZE0] >> 8) & 0xF);
		if(ge_rastreo_fb_toca(off_vram(dir_tex()), ((bufw ? bufw : 1) * th * (u32)bits_tfmt(tf) + 7) / 8)) r |= 2;
	}
	return (int)r;
}

//Arreglo
static void anotar_escritura(u32 prim, u32 count, u32 col_y, u32 col_o, int col_ok){
	RfEscr k;
	u32 i;
	char t[RF_LINEA];
	memset(&k, 0, sizeof(k));
	k.fb = PSP_VRAM_BASE | (ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0u);
	k.fmt = ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
	k.fbw = ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FCu;
	k.prim = prim & 7;
	k.borrado = ge.cmd[GE_CLEARMODE] & 0x701u;
	k.mrgb = ge.cmd[GE_MASKRGB] & 0xFFFFFFu;
	k.malpha = ge.cmd[GE_MASKALPHA] & 0xFF;
	k.logica = (ge.cmd[GE_LOGICOPENABLE] & 1) | ((ge.cmd[GE_LOGICOP] & 0xF) << 4);
	k.mezcla = (ge.cmd[GE_ALPHABLENDENABLE] & 1) | ((ge.cmd[GE_BLENDMODE] & 0x7FFu) << 4);
	k.fixa = ge.cmd[GE_BLENDFIXEDA] & 0xFFFFFFu;
	k.fixb = ge.cmd[GE_BLENDFIXEDB] & 0xFFFFFFu;
	k.alfa_t = (ge.cmd[GE_ALPHATESTENABLE] & 1) | ((ge.cmd[GE_ALPHATEST] & 0xFFFFFFu) << 4);
	k.color_t = (ge.cmd[GE_COLORTESTENABLE] & 1) | ((ge.cmd[GE_COLORTEST] & 3) << 4);
	k.sten = ge.cmd[GE_STENCILTESTENABLE] & 1;
	k.stest = ge.cmd[GE_STENCILTEST] & 0xFFFFFFu;
	k.sop = ge.cmd[GE_STENCILOP] & 0xFFFFFFu;
	k.z = (ge.cmd[GE_ZTESTENABLE] & 1) | ((ge.cmd[GE_ZTEST] & 7) << 1) | ((ge.cmd[GE_ZWRITEDISABLE] & 1) << 4);
	k.tramado = ge.cmd[GE_DITHERENABLE] & 1;
	k.tex = ge.cmd[GE_TEXTUREMAPENABLE] & 1;
	if(k.tex){
		k.texaddr = dir_tex();
		k.tfmt = ge.cmd[GE_TEXFORMAT] & 0xF;
		k.tfunc = ge.cmd[GE_TEXFUNC] & 0xFFFFFFu;
	}
	k.colfmt = (ge.cmd[GE_VERTEXTYPE] >> 2) & 7;
	k.through = (ge.cmd[GE_VERTEXTYPE] >> 23) & 1;
	k.luces = ge.cmd[GE_LIGHTINGENABLE] & 1;
	k.material = (ge.cmd[GE_MATERIALAMBIENT] & 0xFFFFFFu) | ((ge.cmd[GE_MATERIALALPHA] & 0xFF) << 24);
	k.col_y = col_y;
	k.col_o = col_o;
	k.col_ok = (u32)col_ok;
	escrituras++;
	if(bajada_pend){ bajadas_sin++; bajada_pend = 0; }
	if(traza()){
		size_t len = (size_t)snprintf(t, sizeof(t), "[RASTREO] c%u E ", cuadro);
		desc_escr(&k, t + len, sizeof(t) - len);
		linea(t);
	}
	for(i = 0; i < n_escr; i++) if(!memcmp(&escr[i], &k, offsetof(RfEscr, dibujos))) break;
	if(i == n_escr){
		if(n_escr == RF_ESCR){ escr_fuera++; return; }
		escr[n_escr++] = k;
	}
	if(!escr[i].dibujos){
		escr[i].col_y = k.col_y;
		escr[i].col_o = k.col_o;
		escr[i].col_ok = k.col_ok;
	} else {
		escr[i].col_y &= k.col_y;
		escr[i].col_o |= k.col_o;
		escr[i].col_ok &= k.col_ok;
	}
	escr[i].dibujos++;
	escr[i].vertices += count;
}

//Arreglo
static void anotar_lectura(u32 count){
	RfLect k;
	u32 i;
	memset(&k, 0, sizeof(k));
	k.tex = dir_tex();
	k.tfmt = ge.cmd[GE_TEXFORMAT] & 0xF;
	k.tsize = ge.cmd[GE_TEXSIZE0] & 0xF0Fu;
	k.bufw = ge.cmd[GE_TEXBUFWIDTH0] & 0x7FFu;
	k.swz = ge.cmd[GE_TEXMODE] & 1;
	k.clutfmt = ge.cmd[GE_CLUTFORMAT] & 0xFFFFFFu;
	k.clutaddr = (ge.cmd[GE_CLUTADDR] & 0xFFFFF0u) | ((ge.cmd[GE_CLUTADDRUPPER] << 8) & 0x0F000000u);
	k.fb = PSP_VRAM_BASE | (ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0u);
	k.fmt = ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
	k.mezcla = (ge.cmd[GE_ALPHABLENDENABLE] & 1) | ((ge.cmd[GE_BLENDMODE] & 0x7FFu) << 4);
	k.fixa = ge.cmd[GE_BLENDFIXEDA] & 0xFFFFFFu;
	k.fixb = ge.cmd[GE_BLENDFIXEDB] & 0xFFFFFFu;
	k.tfunc = ge.cmd[GE_TEXFUNC] & 0xFFFFFFu;
	k.through = (ge.cmd[GE_VERTEXTYPE] >> 23) & 1;
	k.donde = ge_rastreo_fb_donde ? ge_rastreo_fb_donde(off_vram(k.tex), r_len, NULL, 0) : RF_NADA;
	lecturas++;
	if(bajada_pend){ bajadas_con++; bajada_pend = 0; }
	for(i = 0; i < n_lect; i++) if(!memcmp(&lect[i], &k, offsetof(RfLect, dibujos))) break;
	if(i == n_lect){
		if(n_lect == RF_LECT){ lect_fuera++; return; }
		lect[n_lect++] = k;
	}
	lect[i].dibujos++;
	lect[i].vertices += count;
	lect_pend = (int)i;
	lect_pend_txt[0] = 0;
	if(traza()){
		static const char *const nf[4] = { "565", "5551", "4444", "8888" };
		char d[300];
		if(ge_rastreo_fb_donde) ge_rastreo_fb_donde(off_vram(k.tex), r_len, d, sizeof(d));
		else snprintf(d, sizeof(d), "sin GX");
		snprintf(lect_pend_txt, sizeof(lect_pend_txt),
		         "[RASTREO] c%u L tex %08X fmt %u %ux%u ancho %u%s, paleta %s >>%u &%02X |%u en %08X, a %08X %s, mezcla %s %u/%u/%u %06X/%06X, tfunc %06X%s | donde: %s",
		         cuadro, (unsigned)k.tex, (unsigned)k.tfmt, 1u << (k.tsize & 0xF), 1u << ((k.tsize >> 8) & 0xF), (unsigned)k.bufw,
		         k.swz ? " swizzle" : "", nf[k.clutfmt & 3], (unsigned)((k.clutfmt >> 2) & 0x1F), (unsigned)((k.clutfmt >> 8) & 0xFF),
		         (unsigned)(((k.clutfmt >> 16) & 0x1F) << 4), (unsigned)k.clutaddr, (unsigned)k.fb, nf[k.fmt],
		         (k.mezcla & 1) ? "si" : "no", (unsigned)((k.mezcla >> 4) & 0xF), (unsigned)((k.mezcla >> 8) & 0xF),
		         (unsigned)((k.mezcla >> 12) & 7), (unsigned)k.fixa, (unsigned)k.fixb, (unsigned)k.tfunc, k.through ? ", through" : "", d);
	}
	if(!ge_hw){
		anotar_contenido(&lect[i], "software, al leer");
		cerrar_lectura(RF_SOFT);
	}
}

//Arreglo
void ge_rastreo_fb_dibujo(u32 prim, u32 count, int interesa, u32 col_y, u32 col_o, int col_ok){
	cerrar_lectura(0);
	if(interesa & 1) anotar_escritura(prim, count, col_y, col_o, col_ok);
	if(interesa & 2) anotar_lectura(count);
}

//Arreglo
void ge_rastreo_fb_camino(int camino){ cerrar_lectura(camino); }

//Arreglo
void ge_rastreo_fb_contenido(const char *origen){
	if(!ge_rastreo_fb_activo() || lect_pend < 0) return;
	anotar_contenido(&lect[lect_pend], origen);
}

//Arreglo
u32 ge_rastreo_fb_largo(void){ return r_len; }

//Arreglo
void ge_rastreo_fb_transferencia(u32 src, u32 dst, u32 w, u32 h, u32 bpp){
	char t[200];
	transferencias++;
	if(traza()){
		snprintf(t, sizeof(t), "[RASTREO] c%u T transferencia de %08X a %08X, %ux%u de %u bytes", cuadro, (unsigned)src, (unsigned)dst,
		         (unsigned)w, (unsigned)h, (unsigned)bpp);
		linea(t);
	}
}

//Arreglo
void ge_rastreo_fb_gx(int evento, int motivo, const char *desc){
	static const char *const ne[RG_N] = { "crear", "soltar", "bajar", "bajar filas", "subir", "subir filas", "copia del EFB",
	                                      "promover el alfa", "hueco", "escritura de la CPU" };
	char t[RF_LINEA];
	if(evento < 0 || evento >= RG_N) return;
	ev[evento]++;
	if(evento == RG_BAJAR || evento == RG_BAJAR_FILAS){
		if(bajada_pend) bajadas_sin++;
		bajada_pend = 0;
		if(lect_pend >= 0) bajadas_con++;
		else bajada_pend = 1;
		if(motivo >= 0 && motivo < 16) bajadas_motivo[motivo]++;
	}
	if(traza()){
		if(motivo >= 0) snprintf(t, sizeof(t), "[RASTREO] c%u G %s (motivo %d): %s", cuadro, ne[evento], motivo, desc ? desc : "");
		else snprintf(t, sizeof(t), "[RASTREO] c%u G %s: %s", cuadro, ne[evento], desc ? desc : "");
		linea(t);
	}
}

//Arreglo
void ge_rastreo_fb_informe(void){
	static const char *const ntf[11] = { "565", "5551", "4444", "8888", "CLUT4", "CLUT8", "CLUT16", "CLUT32", "DXT1", "DXT3", "DXT5" };
	static const char *const nf[4] = { "565", "5551", "4444", "8888" };
	char l[1600], d[300];
	size_t len;
	u32 i, k, suma_e = 0, suma_l = 0, sin;
	cerrar_lectura(0);
	emitir();
	if(r_off == RF_NADA){
		hle_log("[TIEMPOS] Rastreo (prof_rastreo_fb): sin direccion (falta 'rastreo_fb DIRECCION' en el autotest)\n");
		return;
	}
	for(i = 0; i < n_escr; i++) suma_e += escr[i].dibujos;
	for(i = 0; i < n_lect; i++) suma_l += lect[i].dibujos;
	datos.escrituras = escrituras;
	datos.lecturas = lecturas;
	datos.n_escr = n_escr;
	datos.n_lect = n_lect;
	datos.cuadra = suma_e + escr_fuera == escrituras && suma_l + lect_fuera == lecturas;
	len = (size_t)snprintf(l, sizeof(l), "[TIEMPOS] Rastreo de %08X-%08X (prof_rastreo_fb): %u escrituras en %u estados, %u lecturas en %u formas "
	                       "(cuadra: %s), %u transferencias; GX: crear %u, soltar %u, bajar %u, bajar filas %u, subir %u, subir filas %u, "
	                       "copias del EFB %u, promover el alfa %u, hueco %u, escrituras de la CPU %u; bajadas con lectura despues %u, sin lectura %u; "
	                       "motivos de las bajadas:",
	                       (unsigned)(PSP_VRAM_BASE + r_off), (unsigned)(PSP_VRAM_BASE + r_off + r_len - 1), (unsigned)escrituras,
	                       (unsigned)n_escr, (unsigned)lecturas, (unsigned)n_lect, datos.cuadra ? "si" : "NO", (unsigned)transferencias,
	                       (unsigned)ev[RG_CREAR], (unsigned)ev[RG_SOLTAR], (unsigned)ev[RG_BAJAR], (unsigned)ev[RG_BAJAR_FILAS],
	                       (unsigned)ev[RG_SUBIR], (unsigned)ev[RG_SUBIR_FILAS], (unsigned)ev[RG_COPIA_EFB], (unsigned)ev[RG_PROMOVER],
	                       (unsigned)ev[RG_HUECO], (unsigned)ev[RG_CPU], (unsigned)bajadas_con, (unsigned)bajadas_sin);
	for(i = 0, k = 0; i < 16 && len + 16 < sizeof(l); i++)
		if(bajadas_motivo[i]){ len += (size_t)snprintf(l + len, sizeof(l) - len, " %u:%u", (unsigned)i, (unsigned)bajadas_motivo[i]); k++; }
	if(!k && len + 10 < sizeof(l)) len += (size_t)snprintf(l + len, sizeof(l) - len, " ninguna");
	if(escr_fuera || lect_fuera) snprintf(l + len, sizeof(l) - len, " (sin sitio en las tablas: %u y %u dibujos)", (unsigned)escr_fuera, (unsigned)lect_fuera);
	hle_log("%s\n", l);
	for(k = 0; k < n_escr; k++){
		u32 m = n_escr;
		for(i = 0; i < n_escr; i++) if(escr[i].dibujos && (m == n_escr || escr[i].dibujos > escr[m].dibujos)) m = i;
		if(m == n_escr) break;
		len = (size_t)snprintf(l, sizeof(l), "[TIEMPOS] Rastreo, escritura %u (%u dibujos, %u vertices): ", (unsigned)(k + 1),
		                       (unsigned)escr[m].dibujos, (unsigned)escr[m].vertices);
		desc_escr(&escr[m], l + len, sizeof(l) - len);
		hle_log("%s\n", l);
		escr[m].dibujos = 0;
	}
	for(k = 0; k < n_lect; k++){
		u32 m = n_lect;
		for(i = 0; i < n_lect; i++) if(lect[i].dibujos && (m == n_lect || lect[i].dibujos > lect[m].dibujos)) m = i;
		if(m == n_lect) break;
		desc_donde(lect[m].donde, d, sizeof(d));
		sin = lect[m].dibujos;
		for(i = 1; i < RF_CAMINOS; i++) sin -= lect[m].camino[i];
		len = (size_t)snprintf(l, sizeof(l), "[TIEMPOS] Rastreo, lectura %u (%u dibujos, %u vertices): tex %08X %s %ux%u ancho %u%s, paleta %s >>%u &%02X |%u en %08X, "
		                       "a %08X %s, mezcla %s %u/%u/%u %06X/%06X, tfunc %06X%s; al empezar el dibujo: %s; camino: sin elegir %u, copia GPU %u, "
		                       "paleta GPU %u, decodificada %u, cache %u, GX no dibuja %u, software %u",
		                       (unsigned)(k + 1), (unsigned)lect[m].dibujos, (unsigned)lect[m].vertices, (unsigned)lect[m].tex,
		                       ntf[lect[m].tfmt <= 10 ? lect[m].tfmt : 0], 1u << (lect[m].tsize & 0xF), 1u << ((lect[m].tsize >> 8) & 0xF),
		                       (unsigned)lect[m].bufw, lect[m].swz ? " swizzle" : "", nf[lect[m].clutfmt & 3], (unsigned)((lect[m].clutfmt >> 2) & 0x1F),
		                       (unsigned)((lect[m].clutfmt >> 8) & 0xFF), (unsigned)(((lect[m].clutfmt >> 16) & 0x1F) << 4), (unsigned)lect[m].clutaddr,
		                       (unsigned)lect[m].fb, nf[lect[m].fmt], (lect[m].mezcla & 1) ? "si" : "no", (unsigned)((lect[m].mezcla >> 4) & 0xF),
		                       (unsigned)((lect[m].mezcla >> 8) & 0xF), (unsigned)((lect[m].mezcla >> 12) & 7), (unsigned)lect[m].fixa,
		                       (unsigned)lect[m].fixb, (unsigned)lect[m].tfunc, lect[m].through ? ", through" : "", d, (unsigned)sin,
		                       (unsigned)lect[m].camino[RF_COPIA_GPU], (unsigned)lect[m].camino[RF_PALETA_GPU], (unsigned)lect[m].camino[RF_DECODIFICADA],
		                       (unsigned)lect[m].camino[RF_CACHE], (unsigned)lect[m].camino[RF_NO_DIBUJA], (unsigned)lect[m].camino[RF_SOFT]);
		if(lect[m].muestras && lect[m].texeles){
			len += (size_t)snprintf(l + len, sizeof(l) - len, "; contenido en %u lecturas (%% de texeles con cada bit, 0 en adelante):",
			                        (unsigned)lect[m].muestras);
			for(i = 0; i < (u32)bits_tfmt(lect[m].tfmt) && i < 32 && len + 10 < sizeof(l); i++)
				len += (size_t)snprintf(l + len, sizeof(l) - len, " %.2f", 100.0 * (double)lect[m].bits[i] / (double)lect[m].texeles);
		}
		hle_log("%s\n", l);
		lect[m].dibujos = 0;
	}
	n_escr = n_lect = 0;
	escrituras = lecturas = escr_fuera = lect_fuera = transferencias = 0;
	bajadas_con = bajadas_sin = 0;
	memset(ev, 0, sizeof(ev));
	memset(bajadas_motivo, 0, sizeof(bajadas_motivo));
	lect_pend = -1;
}

//Arreglo
void ge_rastreo_fb_datos(RfDatos *d){
	datos.escrituras = escrituras;
	datos.lecturas = lecturas;
	datos.n_escr = n_escr;
	datos.n_lect = n_lect;
	datos.transferencias = transferencias;
	datos.bajadas_con = bajadas_con;
	datos.bajadas_sin = bajadas_sin;
	datos.lect_muestras = n_lect ? lect[0].muestras : 0;
	datos.lect_bit0 = n_lect ? lect[0].bits[0] : 0;
	datos.lect_bit1 = n_lect ? lect[0].bits[1] : 0;
	datos.lect_soft = n_lect ? lect[0].camino[RF_SOFT] : 0;
	datos.escr_mrgb = n_escr ? escr[0].mrgb : 0;
	datos.escr_col_y = n_escr ? escr[0].col_y : 0;
	datos.escr_col_o = n_escr ? escr[0].col_o : 0;
	*d = datos;
}

//Arreglo
void ge_rastreo_fb_reiniciar(void){
	n_escr = n_lect = 0;
	escrituras = lecturas = escr_fuera = lect_fuera = transferencias = 0;
	bajadas_con = bajadas_sin = bajada_pend = 0;
	memset(ev, 0, sizeof(ev));
	memset(bajadas_motivo, 0, sizeof(bajadas_motivo));
	memset(&datos, 0, sizeof(datos));
	lect_pend = -1;
	lect_pend_txt[0] = 0;
	ult_n = 0;
	mapas = contenidos = 0;
	cuadro = 0;
}
