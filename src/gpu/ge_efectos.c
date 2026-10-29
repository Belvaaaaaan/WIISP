/**
 * WIISP - gpu/ge_efectos.c
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

#define EF_ESCR 64
#define EF_LECT 32
#define EF_VOLCADOS 6

typedef struct {
	u32 fb, fmt, borrado, sten, stest, sop, malpha, ztest, mezcla;
	u32 dibujos, vertices;
} EfEscr;

typedef struct {
	u32 tex, tfmt, tsize, bufw, swz, clutfmt, clutaddr, fb, fmt, mezcla, blend, fixa, fixb, tfunc, through, sten, stest;
	u32 dibujos, vertices;
} EfLect;

static EfEscr escr[EF_ESCR];
static EfLect lect[EF_LECT];
static u32 n_escr, n_lect, dibujos, escr_fuera, lect_fuera;
static int volcar;
static unsigned vol_cuadro;
static char vol_prefijo[480];
static u32 vol_hechos, vol_archivos, vol_clave[EF_VOLCADOS][3];
static EfectosDatos datos;
void (*ge_efectos_sincronizar)(void);

static u32 dir_fb(void){ return PSP_VRAM_BASE | (ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0u); }
static u32 dir_tex(void){ return (ge.cmd[GE_TEXADDR0] & 0xFFFFF0u) | ((ge.cmd[GE_TEXBUFWIDTH0] << 8) & 0x0F000000u); }
static int en_vram(u32 a){ return ((a & 0x0FFFFFFFu) - PSP_VRAM_BASE) < PSP_VRAM_SIZE; }

//Arreglo
static void anotar_escritura(u32 count){
	EfEscr k;
	u32 i;
	memset(&k, 0, sizeof(k));
	k.fb = dir_fb();
	k.fmt = ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
	k.borrado = ge.cmd[GE_CLEARMODE] & 0x701u;
	k.sten = ge.cmd[GE_STENCILTESTENABLE] & 1;
	k.stest = ge.cmd[GE_STENCILTEST] & 0xFFFFFFu;
	k.sop = ge.cmd[GE_STENCILOP] & 0xFFFFFFu;
	k.malpha = ge.cmd[GE_MASKALPHA] & 0xFF;
	k.ztest = (ge.cmd[GE_ZTESTENABLE] & 1) | ((ge.cmd[GE_ZTEST] & 7) << 1);
	k.mezcla = ge.cmd[GE_ALPHABLENDENABLE] & 1;
	for(i = 0; i < n_escr; i++)
		if(!memcmp(&escr[i], &k, offsetof(EfEscr, dibujos))) break;
	if(i == n_escr){
		if(n_escr == EF_ESCR){ escr_fuera++; return; }
		escr[n_escr++] = k;
	}
	escr[i].dibujos++;
	escr[i].vertices += count;
}

//Arreglo
static void escribir_bmp(const char *ruta, int w, int h, const u32 *rgb){
	FILE *f = fopen(ruta, "wb");
	u8 cab[54], *fila;
	const u32 paso = ((u32)w * 3 + 3) & ~3u, tam = paso * (u32)h + 54;
	int x, y;
	if(!f) return;
	memset(cab, 0, sizeof(cab));
	cab[0] = 'B'; cab[1] = 'M';
	cab[2] = (u8)tam; cab[3] = (u8)(tam >> 8); cab[4] = (u8)(tam >> 16); cab[5] = (u8)(tam >> 24);
	cab[10] = 54; cab[14] = 40;
	cab[18] = (u8)w; cab[19] = (u8)(w >> 8); cab[22] = (u8)h; cab[23] = (u8)(h >> 8);
	cab[26] = 1; cab[28] = 24;
	fwrite(cab, 1, 54, f);
	fila = calloc(1, paso);
	for(y = h - 1; fila && y >= 0; y--){
		for(x = 0; x < w; x++){
			const u32 c = rgb[(u32)y * (u32)w + (u32)x];
			fila[x * 3] = (u8)(c >> 16); fila[x * 3 + 1] = (u8)(c >> 8); fila[x * 3 + 2] = (u8)c;
		}
		fwrite(fila, 1, paso, f);
	}
	free(fila);
	fclose(f);
	vol_archivos++;
}

static u32 rgb16(u32 v, int fmt){
	u32 r, g, b;
	switch(fmt){
	case GE_FMT_565: r = (v & 31) << 3; g = ((v >> 5) & 63) << 2; b = ((v >> 11) & 31) << 3; break;
	case GE_FMT_5551: r = (v & 31) << 3; g = ((v >> 5) & 31) << 3; b = ((v >> 10) & 31) << 3; break;
	default: r = (v & 15) * 17; g = ((v >> 4) & 15) * 17; b = ((v >> 8) & 15) * 17; break;
	}
	return r | (g << 8) | (b << 16);
}

static u32 texel(u32 base, u32 bufw, int bits, u32 x, u32 y){
	const u32 bit = (y * bufw + x) * (u32)bits, o = (base - PSP_VRAM_BASE + bit / 8) & (PSP_VRAM_SIZE - 1);
	const u8 *p = psp_mem.vram + o;
	switch(bits){
	case 4: return (p[0] >> (bit & 4)) & 15;
	case 8: return p[0];
	case 16: return (u32)p[0] | ((u32)p[1] << 8);
	default: return rd_le32(psp_mem.vram + (o & ~3u));
	}
}

static u32 entrada_paleta(u32 i, int cfmt){
	if(cfmt == GE_FMT_8888) return rd_le32(ge.clut + (i & 0x1FF) * 4) & 0xFFFFFFu;
	return rgb16(rd_le16(ge.clut + (i & 0x3FF) * 2), cfmt);
}

//Arreglo
static void volcar_lectura(const EfLect *k){
	static const char *const nf[4] = { "565", "5551", "4444", "8888" };
	const int tf = (int)k->tfmt, bits = tf == GE_TFMT_CLUT4 ? 4 : tf == GE_TFMT_CLUT8 ? 8 : tf == GE_TFMT_CLUT16 || tf <= GE_FMT_4444 ? 16 : 32;
	const int clut = tf >= GE_TFMT_CLUT4 && tf <= GE_TFMT_CLUT32, cfmt = (int)(k->clutfmt & 3);
	const u32 shift = (k->clutfmt >> 2) & 0x1F, mask = (k->clutfmt >> 8) & 0xFF, off = ((k->clutfmt >> 16) & 0x1F) << 4;
	u32 tw = 1u << (k->tsize & 0xF), th = 1u << ((k->tsize >> 8) & 0xF), x, y, i, *img, *hist, distintas = 0, max_idx = mask | off;
	char ruta[560], l[1500];
	size_t len;
	if(tw > k->bufw && k->bufw) tw = k->bufw;
	if(tw > 512) tw = 512;
	if(th > 272 && tw >= 480) th = 272;
	if(th > 512) th = 512;
	vol_hechos++;
	if(k->swz || !tw || !th){
		hle_log("[EFECTOS] volcado %u (cuadro %u): textura %08X sin volcar (%s)\n", (unsigned)vol_hechos, vol_cuadro, (unsigned)k->tex,
		        k->swz ? "swizzle" : "vacia");
		return;
	}
	img = malloc(tw * th * 4);
	hist = calloc(512, 4);
	if(!img || !hist){ free(img); free(hist); return; }
	for(y = 0; y < th; y++)
		for(x = 0; x < tw; x++){
			const u32 v = texel(k->tex, k->bufw, bits, x, y);
			img[y * tw + x] = bits == 32 ? v & 0xFFFFFFu : bits == 16 ? rgb16(v, tf <= GE_FMT_4444 ? tf : GE_FMT_565) : (v * 255u / (bits == 4 ? 15u : 255u)) * 0x010101u;
		}
	snprintf(ruta, sizeof(ruta), "%s.ef%u.%u.fuente.bmp", vol_prefijo, vol_cuadro, (unsigned)vol_hechos);
	escribir_bmp(ruta, (int)tw, (int)th, img);
	if(bits == 32){
		for(y = 0; y < th; y++) for(x = 0; x < tw; x++) img[y * tw + x] = (texel(k->tex, k->bufw, 32, x, y) >> 24) * 0x010101u;
		snprintf(ruta, sizeof(ruta), "%s.ef%u.%u.alfa.bmp", vol_prefijo, vol_cuadro, (unsigned)vol_hechos);
		escribir_bmp(ruta, (int)tw, (int)th, img);
	}
	len = (size_t)snprintf(l, sizeof(l), "[EFECTOS] volcado %u (cuadro %u): textura %08X (%ux%u, %u bits por texel, ancho de fila %u)",
	                       (unsigned)vol_hechos, vol_cuadro, (unsigned)k->tex, (unsigned)tw, (unsigned)th, (unsigned)bits, (unsigned)k->bufw);
	if(clut){
		if(!max_idx) max_idx = 1;
		for(y = 0; y < th; y++)
			for(x = 0; x < tw; x++){
				const u32 v = texel(k->tex, k->bufw, bits, x, y), idx = (((v >> shift) & mask) | off) & 511;
				hist[idx]++;
				img[y * tw + x] = (idx > max_idx ? 255u : idx * 255u / max_idx) * 0x010101u;
			}
		snprintf(ruta, sizeof(ruta), "%s.ef%u.%u.indices.bmp", vol_prefijo, vol_cuadro, (unsigned)vol_hechos);
		escribir_bmp(ruta, (int)tw, (int)th, img);
		for(y = 0; y < th; y++)
			for(x = 0; x < tw; x++){
				const u32 v = texel(k->tex, k->bufw, bits, x, y);
				img[y * tw + x] = entrada_paleta(((v >> shift) & mask) | off, cfmt);
			}
		snprintf(ruta, sizeof(ruta), "%s.ef%u.%u.paleta.bmp", vol_prefijo, vol_cuadro, (unsigned)vol_hechos);
		escribir_bmp(ruta, (int)tw, (int)th, img);
		len += (size_t)snprintf(l + len, sizeof(l) - len, " como CLUT%u: indice = (texel >> %u) & %02X | %u, paleta %s en %08X; indices "
		                        "(los 16 mas frecuentes, %% de los texeles):", (unsigned)bits, (unsigned)shift, (unsigned)mask,
		                        (unsigned)off, nf[cfmt], (unsigned)k->clutaddr);
		datos.vol_top_idx = 0;
		datos.vol_top_n = 0;
		for(i = 0; i < 16 && len + 40 < sizeof(l); i++){
			u32 m = 512, j;
			for(j = 0; j < 512; j++) if(hist[j] && (m == 512 || hist[j] > hist[m])) m = j;
			if(m == 512) break;
			if(!i){ datos.vol_top_idx = m; datos.vol_top_n = hist[m]; }
			len += (size_t)snprintf(l + len, sizeof(l) - len, " %02X %.2f%%", (unsigned)m, 100.0 * (double)hist[m] / (double)(tw * th));
			hist[m] = 0;
		}
		for(i = 0; i <= (mask | off) && i < 512; i++) if(entrada_paleta(i, cfmt)) distintas++;
		len += (size_t)snprintf(l + len, sizeof(l) - len, "; paleta (RGB): %u de %u entradas no son negras; las primeras:",
		                        (unsigned)distintas, (unsigned)((mask | off) + 1));
		for(i = off; i <= (mask | off) && i < off + 32 && len + 12 < sizeof(l); i++)
			len += (size_t)snprintf(l + len, sizeof(l) - len, " %06X", (unsigned)entrada_paleta(i, cfmt));
	}
	hle_log("%s\n", l);
	free(img);
	free(hist);
}

//Arreglo
static void anotar_lectura(u32 count){
	EfLect k;
	u32 i;
	memset(&k, 0, sizeof(k));
	k.tex = dir_tex();
	k.tfmt = ge.cmd[GE_TEXFORMAT] & 0xF;
	k.tsize = ge.cmd[GE_TEXSIZE0] & 0xF0Fu;
	k.bufw = ge.cmd[GE_TEXBUFWIDTH0] & 0x7FFu;
	k.swz = ge.cmd[GE_TEXMODE] & 1;
	k.clutfmt = ge.cmd[GE_CLUTFORMAT] & 0xFFFFFFu;
	k.clutaddr = (ge.cmd[GE_CLUTADDR] & 0xFFFFF0u) | ((ge.cmd[GE_CLUTADDRUPPER] << 8) & 0x0F000000u);
	k.fb = dir_fb();
	k.fmt = ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
	k.mezcla = ge.cmd[GE_ALPHABLENDENABLE] & 1;
	k.blend = ge.cmd[GE_BLENDMODE] & 0xFFFFFFu;
	k.fixa = ge.cmd[GE_BLENDFIXEDA] & 0xFFFFFFu;
	k.fixb = ge.cmd[GE_BLENDFIXEDB] & 0xFFFFFFu;
	k.tfunc = ge.cmd[GE_TEXFUNC] & 0xFFFFFFu;
	k.through = (ge.cmd[GE_VERTEXTYPE] >> 23) & 1;
	k.sten = ge.cmd[GE_STENCILTESTENABLE] & 1;
	k.stest = ge.cmd[GE_STENCILTEST] & 0xFFFFFFu;
	if(volcar && vol_hechos < EF_VOLCADOS){
		for(i = 0; i < vol_hechos; i++)
			if(vol_clave[i][0] == k.tex && vol_clave[i][1] == k.tfmt && vol_clave[i][2] == k.clutfmt) break;
		if(i == vol_hechos){
			vol_clave[i][0] = k.tex; vol_clave[i][1] = k.tfmt; vol_clave[i][2] = k.clutfmt;
			if(ge_efectos_sincronizar) ge_efectos_sincronizar();
			volcar_lectura(&k);
		}
	}
	for(i = 0; i < n_lect; i++)
		if(!memcmp(&lect[i], &k, offsetof(EfLect, dibujos))) break;
	if(i == n_lect){
		if(n_lect == EF_LECT){ lect_fuera++; return; }
		lect[n_lect++] = k;
	}
	lect[i].dibujos++;
	lect[i].vertices += count;
}

//Arreglo
void ge_efectos_dibujo(u32 prim, u32 count){
	(void)prim;
	dibujos++;
	anotar_escritura(count);
	if((ge.cmd[GE_TEXTUREMAPENABLE] & 1) && !(ge.cmd[GE_CLEARMODE] & 1) && en_vram(dir_tex())) anotar_lectura(count);
}

//Arreglo
void ge_efectos_volcado(const char *prefijo, unsigned cuadro){
	if(!prefijo){ volcar = 0; return; }
	snprintf(vol_prefijo, sizeof(vol_prefijo), "%s", prefijo);
	vol_cuadro = cuadro;
	vol_hechos = 0;
	volcar = 1;
}

//Arreglo
static const char *nota_gx(const EfEscr *e, char *b, size_t n){
	static const char *const op[8] = { "KEEP", "ZERO", "REPLACE", "INVERT", "INCR", "DECR", "op6", "op7" };
	const u32 func = e->stest & 7, sf = e->sop & 7, zf = (e->sop >> 8) & 7, zp = (e->sop >> 16) & 7;
	size_t len = 0;
	b[0] = 0;
	if(e->fmt == GE_FMT_565) return "565: sin stencil";
	if(e->borrado & 1) return (e->borrado & 0x200) ? "borrado con stencil: GX lo hace" : "borrado sin stencil";
	if(!e->sten) return e->malpha == 0xFF ? "sin stencil" : "sin stencil y alfa escribible";
	if(func != CMP_ALWAYS) len += (size_t)snprintf(b + len, n - len, "GX no hace la prueba (func %u); ", (unsigned)func);
	if(sf != SOP_KEEP) len += (size_t)snprintf(b + len, n - len, "GX no hace sfail %s; ", op[sf]);
	if(zf != SOP_KEEP) len += (size_t)snprintf(b + len, n - len, "GX no hace zfail %s; ", op[zf]);
	if(zp != SOP_KEEP && zp != SOP_ZERO && zp != SOP_REPLACE) len += (size_t)snprintf(b + len, n - len, "GX no hace zpass %s; ", op[zp]);
	else if(zp != SOP_KEEP && e->malpha != 0xFF) len += (size_t)snprintf(b + len, n - len, "GX escribe zpass %s; ", op[zp]);
	if(!len) snprintf(b, n, "nada que hacer en GX");
	return b;
}

//Arreglo
void ge_efectos_informe(void){
	static const char *const nf[4] = { "565", "5551", "4444", "8888" };
	static const char *const ntf[11] = { "565", "5551", "4444", "8888", "CLUT4", "CLUT8", "CLUT16", "CLUT32", "DXT1", "DXT3", "DXT5" };
	char l[1800], nota[200];
	size_t len;
	u32 i, k, suma = 0;
	for(i = 0; i < n_escr; i++) suma += escr[i].dibujos;
	datos.dibujos = dibujos;
	datos.n_escr = n_escr;
	datos.n_lect = n_lect;
	datos.cuadra = suma + escr_fuera == dibujos;
	len = (size_t)snprintf(l, sizeof(l), "[TIEMPOS] Efectos (prof_efectos): %u dibujos en %u estados de escritura (cuadra: %s); "
	                       "los que mas (dibujos, vertices; framebuffer, borrado, stencil func/ref/mascara, sfail/zfail/zpass, "
	                       "mascara de alfa, z, mezcla; lo que hace GX):", (unsigned)dibujos, (unsigned)n_escr, datos.cuadra ? "si" : "NO");
	for(k = 0; k < 14 && len + 200 < sizeof(l); k++){
		u32 m = n_escr;
		for(i = 0; i < n_escr; i++) if(escr[i].dibujos && (m == n_escr || escr[i].dibujos > escr[m].dibujos)) m = i;
		if(m == n_escr) break;
		len += (size_t)snprintf(l + len, sizeof(l) - len, " [%u, %u; %08X %s, %s, %s %u/%02X/%02X, %u/%u/%u, %02X, z%s%u, %s; %s]",
		                        (unsigned)escr[m].dibujos, (unsigned)escr[m].vertices, (unsigned)escr[m].fb, nf[escr[m].fmt],
		                        (escr[m].borrado & 1) ? "borrado" : "dibujo", escr[m].sten ? "stencil" : "sin stencil",
		                        (unsigned)(escr[m].stest & 7), (unsigned)((escr[m].stest >> 8) & 0xFF), (unsigned)((escr[m].stest >> 16) & 0xFF),
		                        (unsigned)(escr[m].sop & 7), (unsigned)((escr[m].sop >> 8) & 7), (unsigned)((escr[m].sop >> 16) & 7),
		                        (unsigned)escr[m].malpha, (escr[m].ztest & 1) ? " " : " off ", (unsigned)(escr[m].ztest >> 1),
		                        escr[m].mezcla ? "mezcla" : "sin mezcla", nota_gx(&escr[m], nota, sizeof(nota)));
		escr[m].dibujos = 0;
	}
	hle_log("%s\n", l);
	len = (size_t)snprintf(l, sizeof(l), "[TIEMPOS] Efectos, texturas leidas de la VRAM: %u formas (dibujos, vertices; textura, formato, "
	                       "tamano, paleta, destino, mezcla src/dst/eq y fix A/B, funcion de textura, through):", (unsigned)n_lect);
	for(k = 0; k < 10 && len + 220 < sizeof(l); k++){
		u32 m = n_lect;
		for(i = 0; i < n_lect; i++) if(lect[i].dibujos && (m == n_lect || lect[i].dibujos > lect[m].dibujos)) m = i;
		if(m == n_lect) break;
		len += (size_t)snprintf(l + len, sizeof(l) - len, " [%u, %u; %08X %s %ux%u ancho %u%s, paleta %s >>%u &%02X |%u en %08X, a %08X %s, %s %u/%u/%u %06X/%06X, tfunc %06X%s]",
		                        (unsigned)lect[m].dibujos, (unsigned)lect[m].vertices, (unsigned)lect[m].tex, ntf[lect[m].tfmt <= 10 ? lect[m].tfmt : 0],
		                        1u << (lect[m].tsize & 0xF), 1u << ((lect[m].tsize >> 8) & 0xF), (unsigned)lect[m].bufw, lect[m].swz ? " swizzle" : "",
		                        nf[lect[m].clutfmt & 3], (unsigned)((lect[m].clutfmt >> 2) & 0x1F), (unsigned)((lect[m].clutfmt >> 8) & 0xFF),
		                        (unsigned)(((lect[m].clutfmt >> 16) & 0x1F) << 4), (unsigned)lect[m].clutaddr,
		                        (unsigned)lect[m].fb, nf[lect[m].fmt], lect[m].mezcla ? "mezcla" : "sin mezcla",
		                        (unsigned)(lect[m].blend & 0xF), (unsigned)((lect[m].blend >> 4) & 0xF), (unsigned)((lect[m].blend >> 8) & 7),
		                        (unsigned)lect[m].fixa, (unsigned)lect[m].fixb, (unsigned)lect[m].tfunc, lect[m].through ? ", through" : "");
		lect[m].dibujos = 0;
	}
	if(escr_fuera || lect_fuera) snprintf(l + len, sizeof(l) - len, " (sin sitio en las tablas: %u y %u dibujos)", (unsigned)escr_fuera, (unsigned)lect_fuera);
	hle_log("%s\n", l);
	n_escr = n_lect = 0;
	dibujos = escr_fuera = lect_fuera = 0;
}

//Arreglo
void ge_efectos_datos(EfectosDatos *d){
	u32 i, m = 0;
	datos.dibujos = dibujos;
	datos.n_escr = n_escr;
	datos.n_lect = n_lect;
	for(i = 1; i < n_escr; i++) if(escr[i].dibujos > escr[m].dibujos) m = i;
	datos.escr_dibujos = n_escr ? escr[m].dibujos : 0;
	datos.escr_vertices = n_escr ? escr[m].vertices : 0;
	datos.lect_dibujos = n_lect ? lect[0].dibujos : 0;
	datos.volcados = vol_hechos;
	datos.archivos = vol_archivos;
	*d = datos;
}
