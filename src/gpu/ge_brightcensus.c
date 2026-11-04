/**
 * WIISP - gpu/ge_brightcensus.c
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

#define BC_CUADROS 4
#define BC_BUFERES 16
#define BC_MAX_W 512
#define BC_MAX_H 512
#define BC_BLOQUE 16
#define BC_BLOQUES ((BC_MAX_W / BC_BLOQUE) * (BC_MAX_H / BC_BLOQUE))
#define BC_CAMPOS 32

typedef struct {
	u32 off, stride;
	int fmt, bw, bh, usado;
	u8 rgba[BC_BLOQUES][4];
} BcBufer;

int ge_bc_on;
int (*ge_brightcensus_pixeles)(u32 off, int fmt, u32 stride, int w, int h, u32 *raw);

static unsigned bc_desde, bc_cuadro;
static FILE *bc_f;
static char bc_ruta[600];
static u32 bc_dibujo, bc_dibujos, bc_bytes, bc_prims, bc_con_tex;
static float bc_min[5], bc_max[5];
static BcBufer bc_buf[BC_BUFERES];
static u32 *bc_raw;
static u8 bc_nuevo[BC_BLOQUES][4];

static void bc_poner(const u32 *v, int n){
	int i;
	for(i = 0; i < n; i++){
		u8 b[4];
		wr_le32(b, v[i]);
		fwrite(b, 1, 4, bc_f);
	}
	bc_bytes += 4u * (u32)n;
}

static void bc_vaciar_rango(void){
	int i;
	bc_prims = 0;
	for(i = 0; i < 5; i++){ bc_min[i] = 3.0e38f; bc_max[i] = -3.0e38f; }
}

static void bc_cerrar(void){
	if(!bc_f) return;
	fclose(bc_f);
	bc_f = NULL;
	hle_log("[COTEJO] cuadros %u-%u: %u dibujos registrados (%u con textura), %u KB en %s\n", bc_desde, bc_desde + BC_CUADROS - 1,
	        (unsigned)bc_dibujos, (unsigned)bc_con_tex, (unsigned)(bc_bytes >> 10), bc_ruta);
}

void ge_brightcensus_fijar(unsigned desde){ bc_desde = desde; }

void ge_brightcensus_cerrar(void){
	bc_cerrar();
	ge_bc_on = 0;
}

void ge_brightcensus_cuadro(unsigned cuadro, const char *prefijo){
	const int dentro = opt_on[OPT_TEST_BRIGHTNESSCENSUS] && bc_desde && cuadro >= bc_desde && cuadro < bc_desde + BC_CUADROS;
	bc_cuadro = cuadro;
	bc_dibujo = 0;
	if(!dentro){
		if(bc_f) bc_cerrar();
		ge_bc_on = 0;
		return;
	}
	if(!bc_f){
		static const char cab[8] = { 'W', 'I', 'I', 'S', 'P', 'B', 'C', '1' };
		u32 v[4];
		snprintf(bc_ruta, sizeof(bc_ruta), "%s.dibujos.bin", prefijo && prefijo[0] ? prefijo : "wiisp");
		bc_f = fopen(bc_ruta, "wb");
		if(!bc_f){ hle_log("[COTEJO] no se puede escribir %s\n", bc_ruta); ge_bc_on = 0; return; }
		if(!bc_raw) bc_raw = malloc(BC_MAX_W * BC_MAX_H * 4);
		memset(bc_buf, 0, sizeof(bc_buf));
		v[0] = 1; v[1] = BC_BLOQUE; v[2] = BC_CAMPOS; v[3] = ge_hw ? 1 : 0;
		fwrite(cab, 1, 8, bc_f);
		bc_bytes = 8;
		bc_poner(v, 4);
		bc_dibujos = bc_con_tex = 0;
	}
	ge_bc_on = bc_raw != NULL;
	bc_vaciar_rango();
}

void ge_brightcensus_prim(const GeVertex *const *v, int n){
	int i;
	for(i = 0; i < n; i++){
		const float c[5] = { v[i]->s, v[i]->t, v[i]->q, (float)v[i]->x, (float)v[i]->y };
		int k;
		for(k = 0; k < 5; k++){
			if(c[k] < bc_min[k]) bc_min[k] = c[k];
			if(c[k] > bc_max[k]) bc_max[k] = c[k];
		}
	}
	bc_prims++;
}

static void bc_leer(u32 off, int fmt, u32 stride, int w, int h){
	const int bpp = fmt == GE_FMT_8888 ? 4 : 2;
	int x, y;
	if(ge_brightcensus_pixeles && ge_brightcensus_pixeles(off, fmt, stride, w, h, bc_raw)) return;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++){
			const u32 o = (off + ((u32)y * stride + (u32)x) * (u32)bpp) & (PSP_VRAM_SIZE - 1);
			const u8 *p = psp_mem.vram + o;
			bc_raw[y * w + x] = bpp == 4 ? rd_le32(psp_mem.vram + (o & ~3u)) : ((u32)p[0] | ((u32)p[1] << 8));
		}
}

void ge_brightcensus_dibujo_fin(u32 prim){
	const u32 off = ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0u, stride = ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FCu;
	const int fmt = (int)(ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3);
	int w = (int)(ge.cmd[GE_SCISSOR2] & 0x3FF) + 1, h = (int)((ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF) + 1, bw, bh, b, x, y, i;
	u32 campos[BC_CAMPOS], cambios = 0;
	BcBufer *buf = NULL;
	int todos = 0;
	float rango[10];
	u32 rango_u[10];
	if(!bc_f || !bc_raw) return;
	if(ge_ch_n) ge_raster_charola_vaciar();
	if(stride && w > (int)stride) w = (int)stride;
	if(w > BC_MAX_W) w = BC_MAX_W;
	if(h > BC_MAX_H) h = BC_MAX_H;
	bw = (w + BC_BLOQUE - 1) / BC_BLOQUE;
	bh = (h + BC_BLOQUE - 1) / BC_BLOQUE;
	if(!stride) bw = bh = 0;
	if(bw && bh) bc_leer(off, fmt, stride, w, h);
	for(b = 0; b < bw * bh; b++){
		const int bx = b % bw, by = b / bw;
		u32 s[4] = { 0, 0, 0, 0 }, n = 0;
		for(y = by * BC_BLOQUE; y < (by + 1) * BC_BLOQUE && y < h; y++)
			for(x = bx * BC_BLOQUE; x < (bx + 1) * BC_BLOQUE && x < w; x++){
				u8 c[4];
				ge_decode_color((u32)fmt, bc_raw[y * w + x], c);
				for(i = 0; i < 4; i++) s[i] += c[i];
				n++;
			}
		for(i = 0; i < 4; i++) bc_nuevo[b][i] = (u8)(n ? (s[i] + n / 2) / n : 0);
	}
	for(i = 0; i < BC_BUFERES; i++){
		const BcBufer *q = &bc_buf[i];
		if(q->usado && q->off == off && q->stride == stride && q->fmt == fmt && q->bw == bw && q->bh == bh){ buf = &bc_buf[i]; break; }
	}
	if(!buf)
		for(i = 0; i < BC_BUFERES; i++)
			if(!bc_buf[i].usado){
				buf = &bc_buf[i];
				buf->usado = 1; buf->off = off; buf->stride = stride; buf->fmt = fmt; buf->bw = bw; buf->bh = bh;
				todos = 1;
				break;
			}
	if(!buf) todos = 1;
	memset(campos, 0, sizeof(campos));
	campos[0] = bc_cuadro; campos[1] = bc_dibujo; campos[2] = prim; campos[3] = PSP_VRAM_BASE | off;
	campos[4] = (u32)fmt; campos[5] = stride; campos[6] = (u32)w; campos[7] = (u32)h;
	campos[8] = ge.cmd[GE_CLEARMODE] & 0xFFFFFFu; campos[9] = ge.cmd[GE_MASKRGB] & 0xFFFFFFu; campos[10] = ge.cmd[GE_MASKALPHA] & 0xFF;
	campos[11] = (ge.cmd[GE_LOGICOPENABLE] & 1) | ((ge.cmd[GE_LOGICOP] & 0xF) << 4);
	campos[12] = (ge.cmd[GE_ALPHABLENDENABLE] & 1) | ((ge.cmd[GE_BLENDMODE] & 0x7FFu) << 4);
	campos[13] = ge.cmd[GE_BLENDFIXEDA] & 0xFFFFFFu; campos[14] = ge.cmd[GE_BLENDFIXEDB] & 0xFFFFFFu;
	campos[15] = (ge.cmd[GE_ALPHATESTENABLE] & 1) | ((ge.cmd[GE_ALPHATEST] & 0xFFFFFFu) << 4);
	campos[16] = (ge.cmd[GE_STENCILTESTENABLE] & 1) | ((ge.cmd[GE_STENCILTEST] & 0xFFFFFFu) << 4);
	campos[17] = ge.cmd[GE_STENCILOP] & 0xFFFFFFu;
	campos[18] = (ge.cmd[GE_ZTESTENABLE] & 1) | ((ge.cmd[GE_ZTEST] & 7) << 1) | ((ge.cmd[GE_ZWRITEDISABLE] & 1) << 4) | ((ge.cmd[GE_DITHERENABLE] & 1) << 5);
	campos[19] = ge.cmd[GE_TEXTUREMAPENABLE] & 1;
	campos[20] = (ge.cmd[GE_TEXADDR0] & 0xFFFFF0u) | ((ge.cmd[GE_TEXBUFWIDTH0] << 8) & 0x0F000000u);
	campos[21] = (ge.cmd[GE_TEXFORMAT] & 0xF) | ((ge.cmd[GE_TEXBUFWIDTH0] & 0x7FFu) << 8) | ((ge.cmd[GE_TEXMODE] & 1) << 20);
	campos[22] = ge.cmd[GE_TEXSIZE0] & 0xFFFFu;
	campos[23] = ge.cmd[GE_TEXFUNC] & 0xFFFFFFu;
	campos[24] = ge.cmd[GE_TEXMAPMODE] & 0xFFFFFFu;
	campos[25] = (ge.cmd[GE_TEXWRAP] & 0xFFFFu) | ((ge.cmd[GE_TEXFILTER] & 0xFFFFu) << 16);
	campos[26] = ge.cmd[GE_CLUTFORMAT] & 0xFFFFFFu;
	campos[27] = (ge.cmd[GE_CLUTADDR] & 0xFFFFF0u) | ((ge.cmd[GE_CLUTADDRUPPER] << 8) & 0x0F000000u);
	campos[28] = ge.cmd[GE_VERTEXTYPE] & 0xFFFFFFu;
	campos[29] = (ge.cmd[GE_MATERIALAMBIENT] & 0xFFFFFFu) | ((ge.cmd[GE_MATERIALALPHA] & 0xFF) << 24);
	campos[30] = (ge.cmd[GE_LIGHTINGENABLE] & 1) | ((ge.cmd[GE_FOGENABLE] & 1) << 1) | ((ge.cmd[GE_TEXSHADELS] & 0xFFFFu) << 8);
	campos[31] = bc_prims;
	for(i = 0; i < 5; i++){ rango[i] = bc_prims ? bc_min[i] : 0.0f; rango[5 + i] = bc_prims ? bc_max[i] : 0.0f; }
	if(todos) cambios = (u32)(bw * bh);
	else for(b = 0; b < bw * bh; b++) if(memcmp(buf->rgba[b], bc_nuevo[b], 4)) cambios++;
	{
		const u32 cab[3] = { 0x44494255u, (u32)bw | ((u32)bh << 16), cambios };
		memcpy(rango_u, rango, sizeof(rango_u));
		bc_poner(cab, 3);
		bc_poner(campos, BC_CAMPOS);
		bc_poner(rango_u, 10);
	}
	for(b = 0; b < bw * bh; b++){
		u8 r[6];
		if(!todos && !memcmp(buf->rgba[b], bc_nuevo[b], 4)) continue;
		r[0] = (u8)b; r[1] = (u8)(b >> 8);
		memcpy(r + 2, bc_nuevo[b], 4);
		fwrite(r, 1, 6, bc_f);
		bc_bytes += 6;
	}
	if(buf) memcpy(buf->rgba, bc_nuevo, (size_t)(bw * bh) * 4);
	bc_dibujos++;
	bc_con_tex += campos[19];
	bc_dibujo++;
	bc_vaciar_rango();
}
