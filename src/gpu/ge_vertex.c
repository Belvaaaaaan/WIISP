/**
 * WIISP - ge_vertex.c
 * GE: decodificación de vértices, transformación, luces, coordenadas de
 * textura, recorte y ensamblado de primitivas.
 *
 * VERTEXTYPE (bits):  0-1 uv, 2-4 color, 5-6 normal, 7-8 posición,
 * 9-10 pesos, 11-12 índices, 14-16 nº de pesos - 1, 18-20 nº de morph - 1,
 * 23 modo through. Cada componente se alinea a su propio tamaño y el
 * vértice completo al mayor de ellos.
 *
 * Matrices: mundo, vista, textura y huesos son 4x3 (cuatro columnas de 3
 * floats, en el orden en que sceGuSetMatrix las envía); la proyección 4x4.
 *
 * Las luces se calculan en espacio mundo como en el hardware: ambiente,
 * difusa (o "powered diffuse") y especular con atenuación y focos.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <math.h>
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"
#include "core/memory.h"

/* --- Formato de vértice ---------------------------------------------------- */

typedef struct {
	u32 vtype;
	int tc, col, nrm, pos, wt, idx, nweights, nmorph, through;
	u32 off_w, off_tc, off_col, off_nrm, off_pos;
	u32 stride;       /* de un objetivo de morph */
	u32 total;        /* de un vértice completo (todos los objetivos) */
} VFormat;

static const u8 comp_size[4] = { 0, 1, 2, 4 };

static void vformat_setup(VFormat *f, u32 vt){
	u32 off = 0, align = 1, sz;
	f->vtype = vt;
	f->tc = vt & 3;
	f->col = (vt >> 2) & 7;
	f->nrm = (vt >> 5) & 3;
	f->pos = (vt >> 7) & 3;
	f->wt = (vt >> 9) & 3;
	f->idx = (vt >> 11) & 3;
	f->nweights = ((vt >> 14) & 7) + 1;
	f->nmorph = ((vt >> 18) & 7) + 1;
	f->through = (vt >> 23) & 1;

#define PLACE(fmt_size, count, field) do { \
		sz = (fmt_size); \
		if(sz){ off = (off + sz - 1) & ~(sz - 1); f->field = off; off += sz * (count); if(sz > align) align = sz; } \
	} while(0)

	PLACE(comp_size[f->wt & 3], (u32)f->nweights, off_w);
	PLACE(comp_size[f->tc & 3], 2, off_tc);
	PLACE(f->col >= 4 ? (f->col == 7 ? 4u : 2u) : 0u, 1, off_col);
	PLACE(comp_size[f->nrm & 3], 3, off_nrm);
	PLACE(comp_size[f->pos & 3], 3, off_pos);
#undef PLACE
	f->stride = (off + align - 1) & ~(align - 1);
	f->total = f->stride * (u32)f->nmorph;
}

/* --- Decodificación --------------------------------------------------------- */

typedef struct {
	float w[8];
	float uv[2];
	float col[4];
	float nrm[3];
	float pos[3];
} RawVertex;

static inline float rd_comp(const u8 *p, int fmt, int is_signed, float scale){
	switch(fmt){
	case 1: return (is_signed ? (float)(s8)p[0] : (float)p[0]) * scale;
	case 2: { u16 v = rd_le16(p); return (is_signed ? (float)(s16)v : (float)v) * scale; }
	case 3: { u32 v = rd_le32(p); float f; memcpy(&f, &v, 4); return f; }
	default: return 0.0f;
	}
}

/* Expansión de 4/5/6 bits a 8 igual que el hardware (replica bits altos) */
static inline u32 x4(u32 v){ return (v << 4) | v; }
static inline u32 x5(u32 v){ return (v << 3) | (v >> 2); }
static inline u32 x6(u32 v){ return (v << 2) | (v >> 4); }

void ge_decode_color(u32 format, u32 raw, u8 rgba[4]){
	switch(format){
	case GE_FMT_565:
		rgba[0] = (u8)x5(raw & 31); rgba[1] = (u8)x6((raw >> 5) & 63); rgba[2] = (u8)x5((raw >> 11) & 31); rgba[3] = 255;
		break;
	case GE_FMT_5551:
		rgba[0] = (u8)x5(raw & 31); rgba[1] = (u8)x5((raw >> 5) & 31); rgba[2] = (u8)x5((raw >> 10) & 31);
		rgba[3] = (raw & 0x8000) ? 255 : 0;
		break;
	case GE_FMT_4444:
		rgba[0] = (u8)x4(raw & 15); rgba[1] = (u8)x4((raw >> 4) & 15); rgba[2] = (u8)x4((raw >> 8) & 15);
		rgba[3] = (u8)x4((raw >> 12) & 15);
		break;
	default:
		rgba[0] = (u8)raw; rgba[1] = (u8)(raw >> 8); rgba[2] = (u8)(raw >> 16); rgba[3] = (u8)(raw >> 24);
		break;
	}
}

/* El GE conserva la última coordenada de textura y la última normal: un
   vértice sin ellas usa las anteriores, incluso de otra llamada de dibujo
   (pspautotests gpu/vertices/carry). La uv se guarda ya escalada. */
static float last_uv[2], last_nrm[3];

static void decode_one(const VFormat *f, const u8 *p, RawVertex *v){
	int i;
	/* En modo transformado los enteros se normalizan; en "through" no */
	float s16n = f->through ? 1.0f : 1.0f / 32768.0f;

	for(i = 0; i < f->nweights && f->wt; i++)
		v->w[i] = rd_comp(p + f->off_w + i * comp_size[f->wt & 3], f->wt, 0,
		                  f->wt == 1 ? 1.0f / 128.0f : 1.0f / 32768.0f);
	if(f->tc){
		/* uv de 8 bits: siempre /128, también en modo through (hardware) */
		float sc = f->tc == 1 ? 1.0f / 128.0f : s16n;
		v->uv[0] = rd_comp(p + f->off_tc, f->tc, 0, sc);
		v->uv[1] = rd_comp(p + f->off_tc + comp_size[f->tc & 3], f->tc, 0, sc);
	}
	if(f->col >= 4){
		u8 c[4];
		u32 raw = f->col == 7 ? rd_le32(p + f->off_col) : rd_le16(p + f->off_col);
		ge_decode_color(f->col - 4, raw, c);
		for(i = 0; i < 4; i++) v->col[i] = c[i];
	}
	if(f->nrm){
		float sc = f->nrm == 1 ? 1.0f / 128.0f : 1.0f / 32768.0f;
		for(i = 0; i < 3; i++) v->nrm[i] = rd_comp(p + f->off_nrm + i * comp_size[f->nrm & 3], f->nrm, 1, sc);
	}
	if(f->pos){
		if(f->through && f->pos == 1){
			/* En modo through las posiciones de 8 bits siempre valen 0 (hardware) */
			v->pos[0] = v->pos[1] = v->pos[2] = 0.0f;
		} else if(f->through){
			/* x, y con signo; z sin signo */
			for(i = 0; i < 2; i++) v->pos[i] = rd_comp(p + f->off_pos + i * comp_size[f->pos & 3], f->pos, 1, 1.0f);
			v->pos[2] = rd_comp(p + f->off_pos + 2 * comp_size[f->pos & 3], f->pos, 0, 1.0f);
		} else {
			float sc = f->pos == 1 ? 1.0f / 128.0f : 1.0f / 32768.0f;
			for(i = 0; i < 3; i++) v->pos[i] = rd_comp(p + f->off_pos + i * comp_size[f->pos & 3], f->pos, 1, sc);
		}
	}
}

/* Lee el vértice completo (mezclando los objetivos de morph) */
static int decode_vertex(const VFormat *f, u32 addr, RawVertex *v){
	const u8 *p = mem_ptr(addr, f->total ? f->total : 1);
	memset(v, 0, sizeof(*v));
	if(!p) return 0;
	if(f->nmorph == 1){
		decode_one(f, p, v);
	} else {
		int m, i;
		for(m = 0; m < f->nmorph; m++){
			RawVertex t;
			float w = ge_f24(ge.cmd[GE_MORPHWEIGHT0 + m]);
			memset(&t, 0, sizeof(t));
			decode_one(f, p + m * f->stride, &t);
			for(i = 0; i < 8; i++) v->w[i] += t.w[i] * w;
			for(i = 0; i < 2; i++) v->uv[i] += t.uv[i] * w;
			for(i = 0; i < 4; i++) v->col[i] += t.col[i] * w;
			for(i = 0; i < 3; i++){ v->nrm[i] += t.nrm[i] * w; v->pos[i] += t.pos[i] * w; }
		}
	}
	if(f->nrm) memcpy(last_nrm, v->nrm, sizeof(last_nrm));
	else memcpy(v->nrm, last_nrm, sizeof(last_nrm));
	if(f->col < 4){
		/* Sin color en el vértice: color de material ambiente */
		u32 c = ge.cmd[GE_MATERIALAMBIENT];
		v->col[0] = (float)(c & 0xFF);
		v->col[1] = (float)((c >> 8) & 0xFF);
		v->col[2] = (float)((c >> 16) & 0xFF);
		v->col[3] = (float)(ge.cmd[GE_MATERIALALPHA] & 0xFF);
	}
	return 1;
}

/* --- Matemáticas ------------------------------------------------------------- */

static inline void mul43(const float *m, const float *v, float *out){
	out[0] = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9];
	out[1] = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10];
	out[2] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11];
}

/* Solo la parte 3x3 (para normales) */
static inline void mul33(const float *m, const float *v, float *out){
	out[0] = v[0] * m[0] + v[1] * m[3] + v[2] * m[6];
	out[1] = v[0] * m[1] + v[1] * m[4] + v[2] * m[7];
	out[2] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8];
}

static inline void mul44(const float *m, const float *v3, float *out){
	int i;
	for(i = 0; i < 4; i++)
		out[i] = v3[0] * m[i] + v3[1] * m[4 + i] + v3[2] * m[8 + i] + m[12 + i];
}

static inline float dot3(const float *a, const float *b){ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static inline void normalize3(float *v){
	float l = sqrtf(dot3(v, v));
	if(l > 0.0f){ v[0] /= l; v[1] /= l; v[2] /= l; }
}

static inline float clamp255(float v){ return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); }

/* --- Vértice transformado (antes de proyectar) --------------------------- */

typedef struct {
	float clip[4];      /* coordenadas de recorte */
	GeVertex v;         /* color, uv, niebla (x, y, z, w se rellenan al proyectar) */
	int outside;        /* fuera del rango de pantalla del GE: se descarta la primitiva */
} TVertex;

/* Matriz mundo * vista * proyección combinada con la precisión del GE */
static float combined[16];

static void to4x4(const float *m43, float *out){
	int r, c;
	for(r = 0; r < 4; r++){
		for(c = 0; c < 3; c++) out[r * 4 + c] = m43[r * 3 + c];
		out[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
	}
}

static void update_combined(void){
	float w[16], v[16], wv[16];
	to4x4(ge.world, w);
	to4x4(ge.view, v);
	ge_combine_matrices(wv, w, v);
	ge_combine_matrices(combined, wv, ge.proj);
}

static inline void color24(u32 c, float *out){
	out[0] = (float)(c & 0xFF); out[1] = (float)((c >> 8) & 0xFF); out[2] = (float)((c >> 16) & 0xFF);
}

static void light_vertex(const RawVertex *raw, const float *wpos, const float *wnrm, GeVertex *out){
	float emissive[3], amb_light[3], mat_amb[4], mat_dif[3], mat_spe[3];
	float col[3], spec[3] = { 0, 0, 0 };
	float spec_coef = ge_f24(ge.cmd[GE_MATERIALSPECULARCOEF]);
	u32 upd = ge.cmd[GE_MATERIALUPDATE];
	int i, k;

	color24(ge.cmd[GE_MATERIALEMISSIVE], emissive);
	color24(ge.cmd[GE_AMBIENTCOLOR], amb_light);
	if(upd & 1){ for(k = 0; k < 4; k++) mat_amb[k] = raw->col[k]; }
	else { color24(ge.cmd[GE_MATERIALAMBIENT], mat_amb); mat_amb[3] = (float)(ge.cmd[GE_MATERIALALPHA] & 0xFF); }
	if(upd & 2){ for(k = 0; k < 3; k++) mat_dif[k] = raw->col[k]; }
	else color24(ge.cmd[GE_MATERIALDIFFUSE], mat_dif);
	if(upd & 4){ for(k = 0; k < 3; k++) mat_spe[k] = raw->col[k]; }
	else color24(ge.cmd[GE_MATERIALSPECULAR], mat_spe);

	for(k = 0; k < 3; k++) col[k] = emissive[k] + amb_light[k] * mat_amb[k] / 255.0f;
	out->a = clamp255((float)(ge.cmd[GE_AMBIENTALPHA] & 0xFF) * mat_amb[3] / 255.0f);

	for(i = 0; i < 4; i++){
		u32 type = ge.cmd[GE_LIGHTTYPE0 + i];
		u32 kind = (type >> 8) & 3, comp = type & 3;
		float L[3], att = 1.0f, ndotl, lac[3], ldc[3], lsc[3];

		if(!(ge.cmd[GE_LIGHTENABLE0 + i] & 1)) continue;
		L[0] = ge_f24(ge.cmd[GE_LX0 + i * 3]);
		L[1] = ge_f24(ge.cmd[GE_LX0 + i * 3 + 1]);
		L[2] = ge_f24(ge.cmd[GE_LX0 + i * 3 + 2]);
		if(kind != 0){
			float d;
			for(k = 0; k < 3; k++) L[k] -= wpos[k];
			d = sqrtf(dot3(L, L));
			{
				float ka = ge_f24(ge.cmd[GE_LKA0 + i * 3]), kb = ge_f24(ge.cmd[GE_LKA0 + i * 3 + 1]),
				      kc = ge_f24(ge.cmd[GE_LKA0 + i * 3 + 2]);
				float den = ka + kb * d + kc * d * d;
				att = den > 0.0f ? 1.0f / den : 1.0f;
				if(att > 1.0f) att = 1.0f;
				if(att < 0.0f) att = 0.0f;
			}
		}
		normalize3(L);
		if(kind == 2){
			float dir[3], spot;
			dir[0] = ge_f24(ge.cmd[GE_LDX0 + i * 3]);
			dir[1] = ge_f24(ge.cmd[GE_LDX0 + i * 3 + 1]);
			dir[2] = ge_f24(ge.cmd[GE_LDX0 + i * 3 + 2]);
			normalize3(dir);
			spot = -dot3(L, dir);
			if(spot >= ge_f24(ge.cmd[GE_LKO0 + i])) att *= powf(spot > 0 ? spot : 0, ge_f24(ge.cmd[GE_LKS0 + i]));
			else att = 0.0f;
		}

		color24(ge.cmd[GE_LAC0 + i * 3], lac);
		color24(ge.cmd[GE_LAC0 + i * 3 + 1], ldc);
		color24(ge.cmd[GE_LAC0 + i * 3 + 2], lsc);

		ndotl = dot3(L, wnrm);
		{
			float dif = ndotl > 0.0f ? ndotl : 0.0f;
			if(comp == 2) dif = dif > 0.0f ? powf(dif, spec_coef) : 0.0f;
			for(k = 0; k < 3; k++)
				col[k] += att * (lac[k] * mat_amb[k] / 255.0f + ldc[k] * mat_dif[k] / 255.0f * dif);
		}
		if(comp == 1 && ndotl >= 0.0f){
			float H[3] = { L[0], L[1], L[2] + 1.0f }, s;
			normalize3(H);
			s = dot3(H, wnrm);
			s = s > 0.0f ? powf(s, spec_coef) : 0.0f;
			for(k = 0; k < 3; k++) spec[k] += att * lsc[k] * mat_spe[k] / 255.0f * s;
		}
	}

	if(ge.cmd[GE_LIGHTMODE] & 1){
		/* Especular separada: va al color secundario, que se suma tras texturizar */
		out->sr = clamp255(spec[0]); out->sg = clamp255(spec[1]); out->sb = clamp255(spec[2]);
	} else {
		for(k = 0; k < 3; k++) col[k] += spec[k];
	}
	out->r = clamp255(col[0]); out->g = clamp255(col[1]); out->b = clamp255(col[2]);
}

static void tex_coords(const RawVertex *raw, const float *wnrm, int has_nrm, GeVertex *out){
	u32 mode = ge.cmd[GE_TEXMAPMODE] & 3;
	out->q = 1.0f;
	if(mode == 1){
		/* Proyección: matriz de textura sobre posición, uv o normal */
		float src[3], r[3];
		switch((ge.cmd[GE_TEXMAPMODE] >> 8) & 3){
		case 0: src[0] = raw->pos[0]; src[1] = raw->pos[1]; src[2] = raw->pos[2]; break;
		case 1: src[0] = raw->uv[0]; src[1] = raw->uv[1]; src[2] = 0.0f; break;
		case 2: src[0] = raw->nrm[0]; src[1] = raw->nrm[1]; src[2] = raw->nrm[2]; normalize3(src); break;
		default: src[0] = raw->nrm[0]; src[1] = raw->nrm[1]; src[2] = raw->nrm[2]; break;
		}
		mul43(ge.tgen, src, r);
		out->s = r[0]; out->t = r[1]; out->q = r[2];
	} else if(mode == 2){
		/* Mapa de entorno: dirección de dos luces contra la normal */
		int lu = (int)(ge.cmd[GE_TEXSHADELS] & 3), lv = (int)((ge.cmd[GE_TEXSHADELS] >> 8) & 3);
		float pu[3], pv[3], n[3] = { 0, 0, 1 };
		int k;
		for(k = 0; k < 3; k++){
			pu[k] = ge_f24(ge.cmd[GE_LX0 + lu * 3 + k]);
			pv[k] = ge_f24(ge.cmd[GE_LX0 + lv * 3 + k]);
			if(has_nrm) n[k] = wnrm[k];
		}
		normalize3(pu);
		normalize3(pv);
		out->s = (1.0f + dot3(pu, n)) * 0.5f;
		out->t = (1.0f + dot3(pv, n)) * 0.5f;
	} else {
		out->s = raw->uv[0] * ge_f24(ge.cmd[GE_TEXSCALEU]) + ge_f24(ge.cmd[GE_TEXOFFSETU]);
		out->t = raw->uv[1] * ge_f24(ge.cmd[GE_TEXSCALEV]) + ge_f24(ge.cmd[GE_TEXOFFSETV]);
	}
}

/* Transforma un vértice decodificado (modo no-through) */
static void transform(const VFormat *f, const RawVertex *raw, TVertex *out){
	float pos[3], nrm[3], wpos[3], wnrm[3] = { 0, 0, 1 }, vpos[3];
	int k;

	memcpy(pos, raw->pos, sizeof(pos));
	memcpy(nrm, raw->nrm, sizeof(nrm));

	/* Skinning: suma ponderada de las matrices de huesos */
	if(f->wt){
		float sp[3] = { 0, 0, 0 }, sn[3] = { 0, 0, 0 }, t[3];
		int i;
		for(i = 0; i < f->nweights; i++){
			if(raw->w[i] == 0.0f) continue;
			mul43(ge.bone[i], raw->pos, t);
			for(k = 0; k < 3; k++) sp[k] += t[k] * raw->w[i];
			mul33(ge.bone[i], raw->nrm, t);
			for(k = 0; k < 3; k++) sn[k] += t[k] * raw->w[i];
		}
		memcpy(pos, sp, sizeof(pos));
		memcpy(nrm, sn, sizeof(nrm));
	}

	mul43(ge.world, pos, wpos);
	mul43(ge.view, wpos, vpos);
	for(k = 0; k < 4; k++) out->clip[k] = ge_clip_component(pos, combined, k);

	memset(&out->v, 0, sizeof(out->v));
	out->v.r = raw->col[0]; out->v.g = raw->col[1]; out->v.b = raw->col[2]; out->v.a = raw->col[3];

	{
		mul33(ge.world, nrm, wnrm);
		if(ge.cmd[GE_REVERSENORMAL] & 1){ wnrm[0] = -wnrm[0]; wnrm[1] = -wnrm[1]; wnrm[2] = -wnrm[2]; }
		normalize3(wnrm);
	}
	if(ge_enabled(GE_LIGHTINGENABLE)) light_vertex(raw, wpos, wnrm, &out->v);
	if(f->tc || (ge.cmd[GE_TEXMAPMODE] & 3) != 0){
		tex_coords(raw, wnrm, f->nrm != 0, &out->v);
		if((ge.cmd[GE_TEXMAPMODE] & 3) == 0){ last_uv[0] = out->v.s; last_uv[1] = out->v.t; }
	} else {
		out->v.s = last_uv[0]; out->v.t = last_uv[1]; out->v.q = 1.0f;
	}

	if(ge_enabled(GE_FOGENABLE)){
		float fog = (ge_f24(ge.cmd[GE_FOG1]) + vpos[2]) * ge_f24(ge.cmd[GE_FOG2]);
		out->v.fog = fog < 0.0f ? 0.0f : (fog > 1.0f ? 1.0f : fog);
	} else out->v.fog = 1.0f;
}

/* Clip -> coordenadas de dibujo (píxeles), como el GE: x, y en 1/16 de
   píxel (truncado), z truncada. Marca los vértices fuera del rango de
   pantalla (0..4096): el hardware descarta la primitiva entera. */
#define SCREEN_BOUND (4095.0f + 15.5f / 16.0f)

static void project(TVertex *t){
	float sx = ge_viewport(t->clip[0], t->clip[3], ge_f24(ge.cmd[GE_VIEWPORTXSCALE]), ge_f24(ge.cmd[GE_VIEWPORTXCENTER]));
	float sy = ge_viewport(t->clip[1], t->clip[3], ge_f24(ge.cmd[GE_VIEWPORTYSCALE]), ge_f24(ge.cmd[GE_VIEWPORTYCENTER]));
	float sz = floorf(ge_viewport(t->clip[2], t->clip[3], ge_f24(ge.cmd[GE_VIEWPORTZSCALE]), ge_f24(ge.cmd[GE_VIEWPORTZCENTER])));
	int depth_clamp = ge_enabled(GE_DEPTHCLAMPENABLE);

	t->outside = 0;
	if(depth_clamp){
		if(!(t->clip[2] < -t->clip[3]) && (sx >= SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0))
			t->outside = 1;
		if(sz < 0.0f) sz = 0.0f;
		else if(sz > 65535.0f) sz = 65535.0f;
	} else if(sx > SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0 || sz < 0.0f || sz >= 65536.0f)
		t->outside = 1;

	if(t->outside) return;
	t->v.x = (float)(ge_f2i(floorf(sx * 16.0f)) - (s32)(ge.cmd[GE_OFFSETX] & 0xFFFF)) / 16.0f;
	t->v.y = (float)(ge_f2i(floorf(sy * 16.0f)) - (s32)(ge.cmd[GE_OFFSETY] & 0xFFFF)) / 16.0f;
	t->v.z = sz;
	t->v.w = t->clip[3] != 0.0f ? 1.0f / t->clip[3] : 1.0f;
}

/* Bits de los planos que deja fuera un vértice (|c| > w), sin dividir */
static int outside_mask(const float *c){
	int m = 0;
	if(c[0] > c[3]) m |= 1;
	if(-c[0] > c[3]) m |= 2;
	if(c[1] > c[3]) m |= 4;
	if(-c[1] > c[3]) m |= 8;
	if(c[2] > c[3]) m |= 16;
	if(-c[2] > c[3]) m |= 32;
	return m;
}

/* Vértice through: ya en coordenadas de dibujo; uv en texels */
static void through_vertex(const RawVertex *raw, GeVertex *v){
	u32 tw = 1u << (ge.cmd[GE_TEXSIZE0] & 0xF), th = 1u << ((ge.cmd[GE_TEXSIZE0] >> 8) & 0xF);
	memset(v, 0, sizeof(*v));
	v->x = raw->pos[0];
	v->y = raw->pos[1];
	v->z = raw->pos[2];
	v->w = 1.0f;
	v->s = raw->uv[0] / (float)tw;
	v->t = raw->uv[1] / (float)th;
	v->q = 1.0f;
	v->r = raw->col[0]; v->g = raw->col[1]; v->b = raw->col[2]; v->a = raw->col[3];
	v->fog = 1.0f;
}

/* --- Recorte en el plano cercano --------------------------------------------- */

static void lerp_tvertex(const TVertex *a, const TVertex *b, float t, TVertex *out){
	int i;
	const float *pa = &a->v.s, *pb = &b->v.s;
	float *po = &out->v.s;
	for(i = 0; i < 4; i++) out->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
	/* s, t, q, r, g, b, a, sr, sg, sb, fog: campos consecutivos */
	for(i = 0; i < 11; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

/* Recorta contra z >= -w (delante del plano cercano). Devuelve nº de vértices. */
static int clip_near(const TVertex *in, int n, TVertex *out){
	int i, m = 0;
	for(i = 0; i < n; i++){
		const TVertex *a = &in[i], *b = &in[(i + 1) % n];
		float da = a->clip[2] + a->clip[3], db = b->clip[2] + b->clip[3];
		if(da >= 0.0f) out[m++] = *a;
		if((da >= 0.0f) != (db >= 0.0f)){
			float t = da / (da - db);
			lerp_tvertex(a, b, t, &out[m++]);
		}
	}
	return m;
}

/* --- Primitivas -------------------------------------------------------------- */

static inline int cull_enabled(void){
	return ge_enabled(GE_CULLFACEENABLE);
}

/* ¿Se descarta el triángulo por su orientación? (cw = 1 si va en sentido horario) */
static int culled(const GeVertex *a, const GeVertex *b, const GeVertex *c, int flip){
	float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
	int cw = area > 0.0f;   /* y crece hacia abajo: área positiva = horario en pantalla */
	if(!cull_enabled()) return 0;
	if(area == 0.0f) return 1;
	if(flip) cw = !cw;
	/* CULL = 1 (frente horario) descarta los antihorarios y viceversa */
	return (ge.cmd[GE_CULL] & 1) ? !cw : cw;
}

static void flat_color(GeVertex *a, GeVertex *b, const GeVertex *src){
	a->r = b->r = src->r; a->g = b->g = src->g; a->b = b->b = src->b; a->a = b->a = src->a;
	a->sr = b->sr = src->sr; a->sg = b->sg = src->sg; a->sb = b->sb = src->sb;
}

static void emit_triangle(const TVertex *t0, const TVertex *t1, const TVertex *t2, int through, int flip){
	GeVertex a, b, c;
	if(through){
		a = t0->v; b = t1->v; c = t2->v;
		if(culled(&a, &b, &c, flip)) return;
		if(!(ge.cmd[GE_SHADEMODE] & 1)) flat_color(&a, &b, &c);
		ge_raster_triangle(&a, &b, &c);
		return;
	}
	{
		TVertex in[3] = { *t0, *t1, *t2 }, poly[4];
		int n, i, behind = 0, depth_clip = ge_enabled(GE_DEPTHCLAMPENABLE);
		for(i = 0; i < 3; i++){
			project(&in[i]);
			if(in[i].outside) return;
			behind += in[i].clip[2] < -in[i].clip[3];
		}
		if(in[0].clip[3] < 0.0f && in[1].clip[3] < 0.0f && in[2].clip[3] < 0.0f) return;
		if(outside_mask(in[0].clip) & outside_mask(in[1].clip) & outside_mask(in[2].clip)) return;
		if(!depth_clip){
			/* Sin recorte: la parte detrás del plano cercano se dibuja; w <= 0 descarta */
			if(!(in[0].clip[3] > 0.0f && in[1].clip[3] > 0.0f && in[2].clip[3] > 0.0f)) return;
			behind = 0;
		}
		if(!behind){
			n = 3;
			memcpy(poly, in, sizeof(in));
		} else {
			n = clip_near(in, 3, poly);
			for(i = 0; i < n; i++) project(&poly[i]);
		}
		if(n < 3) return;
		/* La orientación se decide con el triángulo recortado (conserva el orden) */
		if(culled(&poly[0].v, &poly[1].v, &poly[2].v, flip)) return;
		for(i = 1; i + 1 < n; i++){
			a = poly[0].v; b = poly[i].v; c = poly[i + 1].v;
			/* Sombreado plano: el color del último vértice del triángulo */
			if(!(ge.cmd[GE_SHADEMODE] & 1)){
				flat_color(&a, &b, &t2->v);
				c.r = a.r; c.g = a.g; c.b = a.b; c.a = a.a;
				c.sr = a.sr; c.sg = a.sg; c.sb = a.sb;
			}
			ge_raster_triangle(&a, &b, &c);
		}
	}
}

static void emit_two(const TVertex *t0, const TVertex *t1, int through, int is_rect){
	GeVertex a = t0->v, b = t1->v;
	if(!through){
		TVertex p0 = *t0, p1 = *t1;
		project(&p0); project(&p1);
		if(p0.outside || p1.outside) return;
		if(outside_mask(p0.clip) & outside_mask(p1.clip)) return;
		if(!(p0.clip[3] > 0.0f && p1.clip[3] > 0.0f)) return;
		a = p0.v; b = p1.v;
	}
	if(is_rect) ge_raster_rectangle(&a, &b);
	else {
		if(!(ge.cmd[GE_SHADEMODE] & 1)){ a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a; }
		ge_raster_line(&a, &b);
	}
}

static void emit_point(const TVertex *t, int through){
	if(through) ge_raster_point(&t->v);
	else {
		TVertex p = *t;
		project(&p);
		if(p.outside || outside_mask(p.clip)) return;
		ge_raster_point(&p.v);
	}
}

/* Lee y transforma el vértice número i de la llamada actual */
static void fetch(const VFormat *f, u32 vaddr, u32 iaddr, u32 i, TVertex *out){
	RawVertex raw;
	u32 index = i;
	if(f->idx == 1) index = mem_read8(iaddr + i);
	else if(f->idx == 2) index = mem_read16(iaddr + i * 2);
	else if(f->idx == 3) index = mem_read32(iaddr + i * 4) & 0xFFFF; /* solo 16 bits (hardware) */
	decode_vertex(f, vaddr + index * f->total, &raw);
	if(f->through){
		if(f->tc){ last_uv[0] = raw.uv[0]; last_uv[1] = raw.uv[1]; }
		else { raw.uv[0] = last_uv[0]; raw.uv[1] = last_uv[1]; }
		memset(out->clip, 0, sizeof(out->clip));
		through_vertex(&raw, &out->v);
	} else transform(f, &raw, out);
	ge_stats.vertices++;
}

/* Estado de ensamblado que conserva PRIM "continuar" (tipo 7): vértices
   pendientes de una primitiva incompleta, la tira o el abanico en curso.
   Se guardan ya transformados, así que el formato puede cambiar. */
static TVertex pend[2], fan_first;
static int npend, strip_parity, fan_have;

void ge_draw_prim(u32 prim, u32 count){
	VFormat f;
	u32 vaddr = ge.vaddr, iaddr = ge.iaddr, i;
	int cont = 0;
	TVertex cur;

	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	if(prim == GE_PRIM_CONTINUE){ prim = ge.last_prim; cont = 1; }
	if(!cont || prim != ge.last_prim){ npend = 0; strip_parity = 0; fan_have = 0; }
	ge.last_prim = prim;
	if(!count) return;
	/* Sin formato de posición el GE no dibuja nada (pero avanza la dirección) */
	if(!f.pos){
		if(f.idx) ge.iaddr = iaddr + count * comp_size[f.idx & 3];
		else ge.vaddr = vaddr + count * f.total;
		return;
	}
	if(!f.through) update_combined();
	ge_stats.primitives++;
	ge_raster_begin();

	for(i = 0; i < count; i++){
		fetch(&f, vaddr, iaddr, i, &cur);
		switch(prim){
		case GE_PRIM_POINTS:
			emit_point(&cur, f.through);
			break;
		case GE_PRIM_LINES:
		case GE_PRIM_RECTANGLES:
			if(npend == 1){
				emit_two(&pend[0], &cur, f.through, prim == GE_PRIM_RECTANGLES);
				npend = 0;
			} else pend[npend++] = cur;
			break;
		case GE_PRIM_LINE_STRIP:
			if(npend == 1) emit_two(&pend[0], &cur, f.through, 0);
			pend[0] = cur;
			npend = 1;
			break;
		case GE_PRIM_TRIANGLES:
			if(npend == 2){
				emit_triangle(&pend[0], &pend[1], &cur, f.through, 0);
				npend = 0;
			} else pend[npend++] = cur;
			break;
		case GE_PRIM_TRIANGLE_STRIP:
			if(npend == 2){
				/* Los triángulos impares van invertidos para conservar la orientación */
				if(strip_parity) emit_triangle(&pend[1], &pend[0], &cur, f.through, 0);
				else emit_triangle(&pend[0], &pend[1], &cur, f.through, 0);
				strip_parity ^= 1;
				pend[0] = pend[1];
				pend[1] = cur;
			} else pend[npend++] = cur;
			break;
		case GE_PRIM_TRIANGLE_FAN:
			if(!fan_have){ fan_first = cur; fan_have = 1; npend = 0; }
			else if(npend == 1){
				emit_triangle(&fan_first, &pend[0], &cur, f.through, 0);
				pend[0] = cur;
			} else { pend[0] = cur; npend = 1; }
			break;
		}
	}

	/* El GE avanza las direcciones tras dibujar */
	if(f.idx) ge.iaddr = iaddr + count * comp_size[f.idx & 3];
	else ge.vaddr = vaddr + count * f.total;
}

/* --- Bounding box (para BJUMP) --------------------------------------------- */

void ge_bounding_box(u32 count){
	VFormat f;
	u32 i, vaddr = ge.vaddr;
	int out_left = 1, out_right = 1, out_top = 1, out_bottom = 1, out_near = 1, out_far = 1;
	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	if(f.through || count == 0){ ge.bbox_visible = 1; return; }
	for(i = 0; i < count; i++){
		RawVertex raw;
		float wpos[3], vpos[3], c[4];
		decode_vertex(&f, vaddr + i * f.total, &raw);
		mul43(ge.world, raw.pos, wpos);
		mul43(ge.view, wpos, vpos);
		mul44(ge.proj, vpos, c);
		if(c[0] >= -c[3]) out_left = 0;
		if(c[0] <= c[3]) out_right = 0;
		if(c[1] >= -c[3]) out_bottom = 0;
		if(c[1] <= c[3]) out_top = 0;
		if(c[2] >= -c[3]) out_near = 0;
		if(c[2] <= c[3]) out_far = 0;
	}
	ge.bbox_visible = !(out_left || out_right || out_top || out_bottom || out_near || out_far);
}

/* --- Vértices inmediatos (comandos 0xF0-0xF9) ------------------------------- */

void ge_immediate_vertex(void){
	static TVertex imm[3];
	static int n;
	u32 vap = ge.cmd[GE_VAP];
	u32 prim = (vap >> 8) & 7;
	TVertex *t = &imm[n < 3 ? n : 2];
	u32 col = ge.cmd[GE_VCV];

	memset(t, 0, sizeof(*t));
	t->v.x = (float)(s16)(ge.cmd[GE_VSCX] & 0xFFFF) / 16.0f - (float)(ge.cmd[GE_OFFSETX] & 0xFFFF) / 16.0f;
	t->v.y = (float)(s16)(ge.cmd[GE_VSCY] & 0xFFFF) / 16.0f - (float)(ge.cmd[GE_OFFSETY] & 0xFFFF) / 16.0f;
	t->v.z = (float)(ge.cmd[GE_VSCZ] & 0xFFFF);
	t->v.w = 1.0f;
	t->v.s = ge_f24(ge.cmd[GE_VTCS]);
	t->v.t = ge_f24(ge.cmd[GE_VTCT]);
	t->v.q = ge_f24(ge.cmd[GE_VTCQ]);
	if(t->v.q == 0.0f) t->v.q = 1.0f;
	t->v.r = (float)(col & 0xFF); t->v.g = (float)((col >> 8) & 0xFF); t->v.b = (float)((col >> 16) & 0xFF);
	t->v.a = (float)(vap & 0xFF);
	t->v.fog = 1.0f;
	ge_raster_begin();

	if(prim == GE_PRIM_POINTS){ ge_raster_point(&t->v); n = 0; return; }
	n++;
	if((prim == GE_PRIM_LINES || prim == GE_PRIM_RECTANGLES) && n == 2){
		if(prim == GE_PRIM_RECTANGLES) ge_raster_rectangle(&imm[0].v, &imm[1].v);
		else ge_raster_line(&imm[0].v, &imm[1].v);
		n = 0;
	} else if(prim >= GE_PRIM_TRIANGLES && prim <= GE_PRIM_TRIANGLE_FAN && n == 3){
		emit_triangle(&imm[0], &imm[1], &imm[2], 1, 0);
		n = 0;
	}
}

/* --- Parches bezier y spline -------------------------------------------------- */

/* Base de Bernstein cúbica */
static void bernstein(float t, float *b){
	float it = 1.0f - t;
	b[0] = it * it * it;
	b[1] = 3.0f * t * it * it;
	b[2] = 3.0f * t * t * it;
	b[3] = t * t * t;
}

/* Combina 16 puntos de control con pesos bu[4] x bv[4] */
static void blend_patch(const RawVertex *cp[16], const float *bu, const float *bv, RawVertex *out){
	int i, j, k;
	memset(out, 0, sizeof(*out));
	for(j = 0; j < 4; j++)
		for(i = 0; i < 4; i++){
			float w = bu[i] * bv[j];
			const RawVertex *p = cp[j * 4 + i];
			for(k = 0; k < 3; k++){ out->pos[k] += p->pos[k] * w; out->nrm[k] += p->nrm[k] * w; }
			for(k = 0; k < 4; k++) out->col[k] += p->col[k] * w;
			for(k = 0; k < 2; k++) out->uv[k] += p->uv[k] * w;
		}
}

/* Dibuja una rejilla de (nu+1) x (nv+1) vértices ya evaluados */
static void draw_grid(const VFormat *f, RawVertex *grid, int nu, int nv, int gen_uv){
	u32 prim_type = ge.cmd[GE_PATCHPRIMITIVE] & 3;
	int facing = ge.cmd[GE_PATCHFACING] & 1;
	int u, v;
	ge_raster_begin();
	for(v = 0; v <= nv; v++)
		for(u = 0; u <= nu; u++){
			RawVertex *r = &grid[v * (nu + 1) + u];
			if(gen_uv){ r->uv[0] = (float)u / (float)nu; r->uv[1] = (float)v / (float)nv; }
		}
	for(v = 0; v < nv; v++)
		for(u = 0; u < nu; u++){
			TVertex q[4];
			const RawVertex *r[4] = {
				&grid[v * (nu + 1) + u], &grid[v * (nu + 1) + u + 1],
				&grid[(v + 1) * (nu + 1) + u], &grid[(v + 1) * (nu + 1) + u + 1]
			};
			int k;
			for(k = 0; k < 4; k++){
				if(f->through){ memset(q[k].clip, 0, sizeof(q[k].clip)); through_vertex(r[k], &q[k].v); }
				else transform(f, r[k], &q[k]);
			}
			if(prim_type == 0){
				if(facing){
					emit_triangle(&q[0], &q[1], &q[2], f->through, 0);
					emit_triangle(&q[1], &q[3], &q[2], f->through, 0);
				} else {
					emit_triangle(&q[0], &q[2], &q[1], f->through, 0);
					emit_triangle(&q[1], &q[2], &q[3], f->through, 0);
				}
			} else if(prim_type == 1){
				emit_two(&q[0], &q[1], f->through, 0);
				emit_two(&q[0], &q[2], f->through, 0);
			} else {
				emit_point(&q[0], f->through);
			}
		}
}

#define MAX_PATCH_DIV 64
#define MAX_CONTROL   64

void ge_draw_bezier(u32 arg){
	VFormat f;
	int ucount = (int)(arg & 0xFF), vcount = (int)((arg >> 8) & 0xFF);
	int udiv = (int)(ge.cmd[GE_PATCHDIVISION] & 0xFF), vdiv = (int)((ge.cmd[GE_PATCHDIVISION] >> 8) & 0xFF);
	int pu, pv, i, j;
	static RawVertex ctrl[MAX_CONTROL * MAX_CONTROL];
	static RawVertex grid[(MAX_PATCH_DIV + 1) * (MAX_PATCH_DIV + 1)];

	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	if(ucount < 4 || vcount < 4 || ucount > MAX_CONTROL || vcount > MAX_CONTROL) return;
	if(!f.through) update_combined();
	if(udiv < 1) udiv = 1;
	if(vdiv < 1) vdiv = 1;
	if(udiv > MAX_PATCH_DIV) udiv = MAX_PATCH_DIV;
	if(vdiv > MAX_PATCH_DIV) vdiv = MAX_PATCH_DIV;
	ge_stats.primitives++;

	for(j = 0; j < vcount; j++)
		for(i = 0; i < ucount; i++){
			u32 idx = (u32)(j * ucount + i), index = idx;
			if(f.idx == 1) index = mem_read8(ge.iaddr + idx);
			else if(f.idx == 2) index = mem_read16(ge.iaddr + idx * 2);
			else if(f.idx == 3) index = mem_read32(ge.iaddr + idx * 4) & 0xFFFF;
			decode_vertex(&f, ge.vaddr + index * f.total, &ctrl[idx]);
		}

	/* Parches de 4x4 que comparten bordes (ucount = 3n + 1) */
	for(pv = 0; pv + 3 < vcount; pv += 3)
		for(pu = 0; pu + 3 < ucount; pu += 3){
			const RawVertex *cp[16];
			int u, v;
			for(j = 0; j < 4; j++)
				for(i = 0; i < 4; i++) cp[j * 4 + i] = &ctrl[(pv + j) * ucount + pu + i];
			for(v = 0; v <= vdiv; v++)
				for(u = 0; u <= udiv; u++){
					float bu[4], bv[4];
					bernstein((float)u / (float)udiv, bu);
					bernstein((float)v / (float)vdiv, bv);
					blend_patch(cp, bu, bv, &grid[v * (udiv + 1) + u]);
					if(!f.tc){
						int npu = (ucount - 1) / 3, npv = (vcount - 1) / 3;
						grid[v * (udiv + 1) + u].uv[0] = ((float)(pu / 3) + (float)u / (float)udiv) / (float)npu;
						grid[v * (udiv + 1) + u].uv[1] = ((float)(pv / 3) + (float)v / (float)vdiv) / (float)npv;
					}
				}
			draw_grid(&f, grid, udiv, vdiv, 0);
		}
}

/* B-spline cúbica uniforme; los bordes "abiertos" repiten los extremos */
static float bspline_basis(int i, float t){
	float it = 1.0f - t;
	switch(i){
	case 0: return it * it * it / 6.0f;
	case 1: return (3.0f * t * t * t - 6.0f * t * t + 4.0f) / 6.0f;
	case 2: return (-3.0f * t * t * t + 3.0f * t * t + 3.0f * t + 1.0f) / 6.0f;
	default: return t * t * t / 6.0f;
	}
}

void ge_draw_spline(u32 arg){
	VFormat f;
	int ucount = (int)(arg & 0xFF), vcount = (int)((arg >> 8) & 0xFF);
	int udiv = (int)(ge.cmd[GE_PATCHDIVISION] & 0xFF), vdiv = (int)((ge.cmd[GE_PATCHDIVISION] >> 8) & 0xFF);
	int i, j, su, sv;
	static RawVertex ctrl[MAX_CONTROL * MAX_CONTROL];
	static RawVertex grid[(MAX_PATCH_DIV + 1) * (MAX_PATCH_DIV + 1)];

	vformat_setup(&f, ge.cmd[GE_VERTEXTYPE]);
	if(ucount < 4 || vcount < 4 || ucount > MAX_CONTROL || vcount > MAX_CONTROL) return;
	if(!f.through) update_combined();
	if(udiv < 1) udiv = 1;
	if(vdiv < 1) vdiv = 1;
	if(udiv > MAX_PATCH_DIV) udiv = MAX_PATCH_DIV;
	if(vdiv > MAX_PATCH_DIV) vdiv = MAX_PATCH_DIV;
	ge_stats.primitives++;

	for(j = 0; j < vcount; j++)
		for(i = 0; i < ucount; i++){
			u32 idx = (u32)(j * ucount + i), index = idx;
			if(f.idx == 1) index = mem_read8(ge.iaddr + idx);
			else if(f.idx == 2) index = mem_read16(ge.iaddr + idx * 2);
			else if(f.idx == 3) index = mem_read32(ge.iaddr + idx * 4) & 0xFFFF;
			decode_vertex(&f, ge.vaddr + index * f.total, &ctrl[idx]);
		}

	/* Un segmento por cada ventana de 4 puntos de control */
	for(sv = 0; sv + 3 < vcount; sv++)
		for(su = 0; su + 3 < ucount; su++){
			int u, v, k;
			for(v = 0; v <= vdiv; v++)
				for(u = 0; u <= udiv; u++){
					RawVertex *out = &grid[v * (udiv + 1) + u];
					float tu = (float)u / (float)udiv, tv = (float)v / (float)vdiv;
					memset(out, 0, sizeof(*out));
					for(j = 0; j < 4; j++)
						for(i = 0; i < 4; i++){
							float w = bspline_basis(i, tu) * bspline_basis(j, tv);
							const RawVertex *p = &ctrl[(sv + j) * ucount + su + i];
							for(k = 0; k < 3; k++){ out->pos[k] += p->pos[k] * w; out->nrm[k] += p->nrm[k] * w; }
							for(k = 0; k < 4; k++) out->col[k] += p->col[k] * w;
							for(k = 0; k < 2; k++) out->uv[k] += p->uv[k] * w;
						}
					if(!f.tc){
						out->uv[0] = ((float)su + tu) / (float)(ucount - 3);
						out->uv[1] = ((float)sv + tv) / (float)(vcount - 3);
					}
				}
			draw_grid(&f, grid, udiv, vdiv, 0);
		}
}
