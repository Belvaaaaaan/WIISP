/**
 * WIISP - ge_texture.c
 * GE: lectura de texels en todos los formatos de la PSP.
 *
 *  - 565, 5551, 4444, 8888 directos
 *  - CLUT4/8/16/32: índice = ((valor >> shift) & mask) | (csa << 4), con la
 *    paleta cargada por LOADCLUT en formato CLUTFORMAT
 *  - DXT1/3/5 con la disposición de la PSP: primero los índices de color
 *    (4 bytes) y los dos colores 565, y después el alfa (al revés que en PC)
 *  - "swizzle": la textura se guarda en bloques de 16 bytes x 8 filas
 *
 * La disposición DXT y el redondeo de los colores intermedios siguen a
 * PPSSPP (GPU/Common/TextureDecoder.cpp, GPLv2+), comprobados en hardware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "gpu/ge_internal.h"
#include "core/memory.h"

static const u8 bits_per_texel[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };

void ge_texture_setup(GeTexture *tex){
	int i;
	tex->format = ge.cmd[GE_TEXFORMAT] & 0xF;
	tex->swizzled = ge.cmd[GE_TEXMODE] & 1;
	tex->levels = (int)((ge.cmd[GE_TEXMODE] >> 16) & 7) + 1;
	for(i = 0; i < 8; i++){
		u32 size = ge.cmd[GE_TEXSIZE0 + i];
		u32 we = size & 0xF, he = (size >> 8) & 0xF;
		tex->level_addr[i] = (ge.cmd[GE_TEXADDR0 + i] & 0xFFFFF0u) |
		                     ((ge.cmd[GE_TEXBUFWIDTH0 + i] << 8) & 0x0F000000u);
		tex->level_stride[i] = ge.cmd[GE_TEXBUFWIDTH0 + i] & 0x7FF;
		tex->level_w[i] = 1u << (we > 9 ? 9 : we);
		tex->level_h[i] = 1u << (he > 9 ? 9 : he);
	}
	tex->addr = tex->level_addr[0];
	tex->stride = tex->level_stride[0];
	tex->width = tex->level_w[0];
	tex->height = tex->level_h[0];
}

/* El desplazamiento csa se recorta a la CLUT de 1 KB (256 entradas de 32
   bits o 512 de 16). Con el bit 8 de TEXMODE cada mipmap usa su propia
   parte de la paleta. */
static void clut_lookup(u32 index, int lvl, u8 rgba[4]){
	u32 cf = ge.cmd[GE_CLUTFORMAT];
	u32 fmt = cf & 3, shift = (cf >> 2) & 0x1F, mask = (cf >> 8) & 0xFF, csa = (cf >> 16) & 0x1F;
	u32 share = 0;
	index = ((index >> shift) & mask) | ((csa << 4) & (fmt == GE_FMT_8888 ? 0xFFu : 0x1FFu));
	if(ge.cmd[GE_TEXMODE] & 0x100)
		share = (ge.cmd[GE_TEXFORMAT] & 0xF) == GE_TFMT_CLUT4 ? (u32)lvl * 16 : (u32)(lvl & 1) * 256;
	index += share;
	if(fmt == GE_FMT_8888) ge_decode_color(fmt, rd_le32(ge.clut + (index & 0x1FF) * 4), rgba);
	else ge_decode_color(fmt, rd_le16(ge.clut + (index & 0x3FF) * 2), rgba);
}

/* Colores de un bloque DXT (los colores 565 tienen el rojo en los bits altos).
   La regla c1 > c2 vale para los tres formatos: en DXT3/5 el color 3 con
   c1 <= c2 también es negro (el alfa sale luego del bloque de alfa). */
static u32 dxt_color(const u8 *block, int idx, int dxt1, u8 rgba[4]){
	u16 c1 = rd_le16(block + 4), c2 = rd_le16(block + 6);
	int r1 = (c1 >> 8) & 0xF8, g1 = (c1 >> 3) & 0xFC, b1 = (c1 << 3) & 0xF8;
	int r2 = (c2 >> 8) & 0xF8, g2 = (c2 >> 3) & 0xFC, b2 = (c2 << 3) & 0xF8;
	int r, g, b, a = 255;
	switch(idx){
	case 0: r = r1; g = g1; b = b1; break;
	case 1: r = r2; g = g2; b = b2; break;
	case 2:
		if(c1 > c2){ r = (2 * r1 + r2) / 3; g = (2 * g1 + g2) / 3; b = (2 * b1 + b2) / 3; }
		else { r = (r1 + r2) / 2; g = (g1 + g2) / 2; b = (b1 + b2) / 2; }
		break;
	default:
		if(c1 > c2){ r = (2 * r2 + r1) / 3; g = (2 * g2 + g1) / 3; b = (2 * b2 + b1) / 3; }
		else { r = g = b = 0; a = dxt1 ? 0 : 255; }
		break;
	}
	rgba[0] = (u8)r; rgba[1] = (u8)g; rgba[2] = (u8)b; rgba[3] = (u8)a;
	return 0;
}

static void dxt_fetch(const GeTexture *tex, int lvl, int x, int y, u8 rgba[4]){
	u32 stride = tex->level_stride[lvl];
	u32 block_size = tex->format == GE_TFMT_DXT1 ? 8 : 16;
	u32 addr = tex->level_addr[lvl] + ((u32)(y >> 2) * (stride >> 2) + (u32)(x >> 2)) * block_size;
	const u8 *b = mem_ptr(addr, block_size);
	int cx = x & 3, cy = y & 3, idx;
	if(!b){ rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
	idx = (b[cy] >> (cx * 2)) & 3;
	dxt_color(b, idx, tex->format == GE_TFMT_DXT1, rgba);
	if(tex->format == GE_TFMT_DXT3){
		u32 line = rd_le16(b + 8 + cy * 2);
		rgba[3] = (u8)(((line >> (cx * 4)) & 0xF) << 4);
	} else if(tex->format == GE_TFMT_DXT5){
		/* 48 bits de índices de 3 bits: alphadata1 (16) arriba, alphadata2 (32) abajo */
		u64 bits = ((u64)rd_le16(b + 12) << 32) | rd_le32(b + 8);
		int a1 = b[14], a2 = b[15];
		int n = (int)((bits >> (12 * cy + 3 * cx)) & 7);
		int a;
		if(n == 0) a = a1;
		else if(n == 1) a = a2;
		else if(a1 > a2) a = ((a1 * ((7 - (n - 1)) << 8)) / 7 + (a2 * ((n - 1) << 8)) / 7 + 31) >> 8;
		else if(n <= 5) a = ((a1 * ((5 - (n - 1)) << 8)) / 5 + (a2 * ((n - 1) << 8)) / 5 + 31) >> 8;
		else a = n == 6 ? 0 : 255;
		rgba[3] = (u8)a;
	}
}

void ge_texture_fetch(const GeTexture *tex, int lvl, int x, int y, u8 rgba[4]){
	u32 bpt, row_bytes, xb, off, raw;
	const u8 *p;

	if(tex->format >= GE_TFMT_DXT1){
		if(tex->format <= GE_TFMT_DXT5) dxt_fetch(tex, lvl, x, y, rgba);
		else rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
		return;
	}

	bpt = bits_per_texel[tex->format];
	row_bytes = tex->level_stride[lvl] * bpt / 8;
	xb = (u32)x * bpt / 8;
	if(tex->swizzled){
		/* Bloques de 16 bytes x 8 filas, uno tras otro por filas de bloques */
		u32 blocks_per_row = row_bytes / 16 ? row_bytes / 16 : 1;
		off = (((u32)y >> 3) * blocks_per_row + (xb >> 4)) * 128 + ((u32)y & 7) * 16 + (xb & 15);
	} else off = (u32)y * row_bytes + xb;

	p = mem_ptr(tex->level_addr[lvl] + off, bpt >= 8 ? bpt / 8 : 1);
	if(!p){ rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }

	switch(tex->format){
	case GE_FMT_565: case GE_FMT_5551: case GE_FMT_4444:
		ge_decode_color(tex->format, rd_le16(p), rgba);
		return;
	case GE_FMT_8888:
		ge_decode_color(GE_FMT_8888, rd_le32(p), rgba);
		return;
	case GE_TFMT_CLUT4:  raw = (x & 1) ? (p[0] >> 4) : (p[0] & 0xF); break;
	case GE_TFMT_CLUT8:  raw = p[0]; break;
	case GE_TFMT_CLUT16: raw = rd_le16(p); break;
	default:             raw = rd_le32(p); break;
	}
	clut_lookup(raw, lvl, rgba);
}
