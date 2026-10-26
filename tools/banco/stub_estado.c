/* libogc de mentira QUE GUARDA EL ESTADO, para comprobar en qemu que dos
   versiones del renderizador de GX dejan GX igual en cada dibujo: cada
   función de estado guarda una huella de sus argumentos en su casilla
   (función, índice) y GX_Begin mezcla en la huella de la corrida la del
   estado entero, la primitiva y el número de vértices. Las copias,
   esperas y cargas de TLUT entran en el orden en que llegan. Los punteros
   se cambian por el orden en que aparecen (no dependen de las direcciones).
   Con host_renderer_hook (main.c del CLI), el CLI dibuja por gx_ge.c y al
   salir escribe "[GX] N dibujos, huella X".
   Huella del FIFO (gx_ge.c compilado con -DWIISP_FIFO_TRAZA): cada vértice
   que escribe put() entra en la primitiva de su GX_Begin; los triángulos
   (GX_TRIANGLES) se cuentan de tres en tres con el estado de su GX_Begin, así
   que la huella es la misma con un GX_Begin por triángulo o uno para muchos.
   Las demás primitivas, enteras con su estado (las de blit no pasan por
   put(): solo su tipo y número de vértices). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef union { unsigned char U8; signed char S8; unsigned short U16; short S16; unsigned int U32; int S32; float F32; } WGPipeS;
static volatile WGPipeS pipe_mem;
volatile WGPipeS *const wgPipe = &pipe_mem;
typedef struct { u32 val[8]; } GXTexObj;
typedef struct { u32 val[3]; } GXTlutObj;

enum { F_VTXDESC, F_CLEARVTX, F_NUMCHANS, F_NUMTEXGENS, F_TEXCOORDGEN, F_CHANCTRL, F_VTXATTR, F_POSMTX, F_TEXMTX,
       F_CURMTX, F_CULL, F_CLIP, F_COPLANAR, F_LINEW, F_POINTS, F_NUMIND, F_COPYFILTER, F_PIXFMT, F_DISPSRC, F_DISPDST,
       F_TEXSRC, F_TEXDST, F_PROJ, F_VIEWPORT, F_SCISSOR, F_BLEND, F_ALPHACMP, F_COLORUPD, F_ALPHAUPD, F_DSTALPHA,
       F_DITHER, F_ZMODE, F_ZCOMPLOC, F_ZTEX, F_NUMTEV, F_TEVORDER, F_TEVCIN, F_TEVCOP, F_TEVAIN, F_TEVAOP, F_TEVKCSEL,
       F_TEVKASEL, F_TEVKCOLOR, F_TEVOP, F_LOADTEX, F_N };

static u32 slot[F_N][64], state_h, trace_h = 2166136261u, begins;
static u32 fifo_h = 2166136261u, fifo_tris, fifo_prims, fifo_malos;
static u32 prim_tipo, prim_n, prim_estado, prim_vistos, prim_h, tri_h;

static u32 fnv(u32 h, u32 v){ int i; for(i = 0; i < 4; i++){ h ^= (v >> (i * 8)) & 0xFF; h *= 16777619u; } return h; }
static u32 mix(int f, u32 idx, u32 h){ return fnv(fnv(fnv(0x811C9DC5u, (u32)f), idx), h) * 0x9E3779B1u; }
static void put(int f, u32 idx, u32 h){
	idx &= 63;
	state_h -= mix(f, idx, slot[f][idx]);
	slot[f][idx] = h;
	state_h += mix(f, idx, h);
}
/* Con GX_TRAZA=1 en el entorno, a stderr cada evento y, en cada dibujo, la
   huella de cada grupo de funciones: comparando dos versiones se ve en qué
   dibujo y en qué función empiezan a ser distintas */
static int traza = -1;
static int con_traza(void){ if(traza < 0) traza = getenv("GX_TRAZA") != NULL; return traza; }
static void event(u32 code, u32 a){
	trace_h = fnv(fnv(trace_h, code), a);
	if(con_traza()) fprintf(stderr, "evento %u %08X\n", code, a);
}

/* Punteros por orden de aparición: el primero que se ve es el 1, el
   segundo el 2... (la tabla se busca por la dirección, pero el número no
   depende de ella: así la huella no cambia si cambia dónde cae cada cosa en
   el montón) */
#define NPTR 65536
static const void *ptrs[NPTR];
static u32 ptr_num[NPTR], ptr_next;
static u32 ptr_id(const void *p){
	u32 h, i;
	if(!p) return 0;
	h = (u32)(((unsigned long)p >> 4) * 2654435761u) & (NPTR - 1);
	for(i = 0; i < NPTR; i++, h = (h + 1) & (NPTR - 1)){
		if(ptrs[h] == p) return ptr_num[h];
		if(!ptrs[h]){ ptrs[h] = p; ptr_num[h] = ++ptr_next; return ptr_num[h]; }
	}
	return 0xFFFFFFFFu;
}
static u32 fbits(float f){ u32 u; memcpy(&u, &f, 4); return u; }
static u32 args(int n, const u32 *a){ u32 h = 2166136261u; int i; for(i = 0; i < n; i++) h = fnv(h, a[i]); return h; }
#define P(f, idx, ...) do { const u32 _a[] = { __VA_ARGS__ }; put(f, idx, args((int)(sizeof(_a) / 4), _a)); } while(0)

void GX_ClearVtxDesc(void){ int i; for(i = 0; i < 64; i++) put(F_VTXDESC, (u32)i, 0); }
void GX_SetVtxDesc(u8 attr, u8 type){ P(F_VTXDESC, attr, type); }
void GX_SetNumChans(u8 n){ P(F_NUMCHANS, 0, n); }
void GX_SetNumTexGens(u32 n){ P(F_NUMTEXGENS, 0, n); }
void GX_SetTexCoordGen(u16 tc, u32 typ, u32 src, u32 mtx){ P(F_TEXCOORDGEN, tc, typ, src, mtx); }
void GX_SetChanCtrl(int ch, u8 en, u8 amb, u8 mat, u8 lit, u8 diff, u8 attn){ P(F_CHANCTRL, (u32)ch, en, amb, mat, lit, diff, attn); }
void GX_SetVtxAttrFmt(u8 fmt, u32 attr, u32 ct, u32 cs, u32 frac){ P(F_VTXATTR, fmt * 32u + attr, ct, cs, frac); }
static u32 mtx_hash(const float *m, int n){ u32 h = 2166136261u; int i; for(i = 0; i < n; i++) h = fnv(h, fbits(m[i])); return h; }
void GX_LoadPosMtxImm(const float *m, u32 idx){ put(F_POSMTX, idx, mtx_hash(m, 12)); }
void GX_LoadTexMtxImm(const float *m, u32 idx, u8 type){ put(F_TEXMTX, idx, fnv(mtx_hash(m, 12), type)); }
void GX_SetCurrentMtx(u32 m){ P(F_CURMTX, 0, m); }
void GX_SetCullMode(u8 m){ P(F_CULL, 0, m); }
void GX_SetClipMode(u8 m){ P(F_CLIP, 0, m); }
void GX_SetCoPlanar(u8 m){ P(F_COPLANAR, 0, m); }
void GX_SetLineWidth(u8 w, u8 f){ P(F_LINEW, 0, w, f); }
void GX_SetPointSize(u8 w, u8 f){ P(F_POINTS, 0, w, f); }
void GX_SetNumIndStages(u8 n){ P(F_NUMIND, 0, n); }
void GX_SetCopyFilter(u8 aa, u8 sp[12][2], u8 vf, u8 vfilt[7]){
	u32 h = fnv(fnv(2166136261u, aa), vf);
	int i;
	for(i = 0; i < 12 && sp; i++) h = fnv(h, sp[i][0] | (sp[i][1] << 8));
	for(i = 0; i < 7 && vfilt; i++) h = fnv(h, vfilt[i]);
	put(F_COPYFILTER, 0, h);
}
void GX_SetPixelFmt(u8 pf, u8 zf){ P(F_PIXFMT, 0, pf, zf); }
void GX_SetDispCopySrc(u16 l, u16 t, u16 w, u16 h){ P(F_DISPSRC, 0, l, t, w, h); }
void GX_SetDispCopyDst(u16 w, u16 h){ P(F_DISPDST, 0, w, h); }
void GX_SetTexCopySrc(u16 l, u16 t, u16 w, u16 h){ P(F_TEXSRC, 0, l, t, w, h); }
void GX_SetTexCopyDst(u16 w, u16 h, u32 fmt, u8 mip){ P(F_TEXDST, 0, w, h, fmt, mip); }
void GX_CopyTex(void *dest, u8 clear){ event(1, fnv(ptr_id(dest), clear)); }
void GX_CopyDisp(void *dest, u8 clear){ event(2, fnv(ptr_id(dest), clear)); }
void GX_DrawDone(void){ event(3, 0); }
/* Testigos (gx_paralelo): la GPU de mentira acaba en el acto */
static u16 draw_sync;
void GX_SetDrawSync(u16 tok){ draw_sync = tok; event(7, tok); }
u16 GX_GetDrawSync(void){ return draw_sync; }
void GX_PixModeSync(void){ event(4, 0); }
void GX_InvalidateTexAll(void){ event(5, 0); }
void GX_LoadProjectionMtx(const float *m, u8 type){ put(F_PROJ, 0, fnv(mtx_hash(m, 16), type)); }
void GX_SetViewport(float x, float y, float w, float h, float n, float f){ P(F_VIEWPORT, 0, fbits(x), fbits(y), fbits(w), fbits(h), fbits(n), fbits(f)); }
void GX_SetScissor(u32 x, u32 y, u32 w, u32 h){ P(F_SCISSOR, 0, x, y, w, h); }
void GX_SetBlendMode(u8 t, u8 s, u8 d, u8 op){ P(F_BLEND, 0, t, s, d, op); }
void GX_SetAlphaCompare(u8 c0, u8 r0, u8 op, u8 c1, u8 r1){ P(F_ALPHACMP, 0, c0, r0, op, c1, r1); }
void GX_SetColorUpdate(u8 e){ P(F_COLORUPD, 0, e); }
void GX_SetAlphaUpdate(u8 e){ P(F_ALPHAUPD, 0, e); }
void GX_SetDstAlpha(u8 e, u8 a){ P(F_DSTALPHA, 0, e, a); }
void GX_SetDither(u8 d){ P(F_DITHER, 0, d); }
void GX_SetZMode(u8 e, u8 f, u8 u){ P(F_ZMODE, 0, e, f, u); }
void GX_SetZCompLoc(u8 b){ P(F_ZCOMPLOC, 0, b); }
void GX_SetZTexture(u8 op, u8 fmt, u32 bias){ P(F_ZTEX, 0, op, fmt, bias); }
void GX_SetNumTevStages(u8 n){ P(F_NUMTEV, 0, n); }
void GX_SetTevOrder(u8 st, u8 tc, u32 map, u8 col){ P(F_TEVORDER, st, tc, map, col); }
void GX_SetTevColorIn(u8 st, u8 a, u8 b, u8 c, u8 d){ P(F_TEVCIN, st, a, b, c, d); }
void GX_SetTevColorOp(u8 st, u8 op, u8 bias, u8 sc, u8 cl, u8 reg){ P(F_TEVCOP, st, op, bias, sc, cl, reg); }
void GX_SetTevAlphaIn(u8 st, u8 a, u8 b, u8 c, u8 d){ P(F_TEVAIN, st, a, b, c, d); }
void GX_SetTevAlphaOp(u8 st, u8 op, u8 bias, u8 sc, u8 cl, u8 reg){ P(F_TEVAOP, st, op, bias, sc, cl, reg); }
void GX_SetTevKColorSel(u8 st, u8 sel){ P(F_TEVKCSEL, st, sel); }
void GX_SetTevKAlphaSel(u8 st, u8 sel){ P(F_TEVKASEL, st, sel); }
/* GXColor va por valor: en el ABI de PowerPC, un puntero a la copia */
void GX_SetTevKColor(u8 sel, const u8 *c){ P(F_TEVKCOLOR, sel, c[0] | (c[1] << 8) | (c[2] << 16) | ((u32)c[3] << 24)); }
void GX_SetTevOp(u8 st, u8 mode){ P(F_TEVOP, st, mode); }
/* Los objetos de textura llevan lo que se les dio (el puntero, por orden de aparición) */
void GX_InitTexObj(GXTexObj *o, void *img, u16 w, u16 h, u8 fmt, u8 ws, u8 wt, u8 mip){
	memset(o, 0, sizeof(*o));
	o->val[0] = ptr_id(img); o->val[1] = w | ((u32)h << 16); o->val[2] = fmt | (ws << 8) | (wt << 16) | ((u32)mip << 24);
}
void GX_InitTexObjCI(GXTexObj *o, void *img, u16 w, u16 h, u8 fmt, u8 ws, u8 wt, u8 mip, u32 tlut){
	GX_InitTexObj(o, img, w, h, fmt, ws, wt, mip);
	o->val[7] = tlut | 0x80000000u;
}
void GX_InitTexObjFilterMode(GXTexObj *o, u8 mn, u8 mg){ o->val[3] = mn | (mg << 8); }
void GX_InitTexObjWrapMode(GXTexObj *o, u8 ws, u8 wt){ o->val[2] = (o->val[2] & 0xFF0000FFu) | (ws << 8) | (wt << 16); }
void GX_InitTexObjLOD(GXTexObj *o, u8 mn, u8 mg, float minl, float maxl, float bias, u8 bc, u8 el, u8 an){
	o->val[3] = mn | (mg << 8) | (bc << 16) | (el << 20) | ((u32)an << 24);
	o->val[4] = fbits(minl); o->val[5] = fbits(maxl); o->val[6] = fbits(bias);
}
void GX_LoadTexObj(GXTexObj *o, u8 map){ put(F_LOADTEX, map, args(8, o->val)); }
void GX_InitTlutObj(GXTlutObj *o, void *lut, u8 fmt, u16 n){ o->val[0] = ptr_id(lut); o->val[1] = fmt | ((u32)n << 8); }
void GX_LoadTlut(GXTlutObj *o, u32 name){ event(6, fnv(args(2, o->val), name)); }
void PPCDCacheFlushAsync(void){}
void PPCDCacheInvalidate(void){}
void ps_guMtxIdentity(float *m){ int i; for(i = 0; i < 12; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
void ps_guMtxScale(float *m, float x, float y, float z){ memset(m, 0, 48); m[0] = x; m[5] = y; m[10] = z; }
static u32 fnvf(u32 h, float f){ return fnv(h, fbits(f)); }
static void prim_cierra(void){
	if(!prim_tipo && !prim_n) return;
	if(prim_vistos && prim_vistos != prim_n) fifo_malos++;
	if(prim_tipo != 0x90 || !prim_vistos){
		fifo_h = fnv(fnv(fnv(fifo_h, prim_estado), prim_tipo | (prim_n << 8)), prim_h);
		fifo_prims++;
	}
	prim_tipo = prim_n = prim_vistos = 0;
	prim_h = 2166136261u;
}
void fifo_traza_vtx(float x, float y, float z, float s, float t, float q, u32 c0, u32 c1);
void fifo_traza_vtx(float x, float y, float z, float s, float t, float q, u32 c0, u32 c1){
	u32 h = fnv(fnv(fnvf(fnvf(fnvf(fnvf(fnvf(fnvf(2166136261u, x), y), z), s), t), q), c0), c1);
	prim_vistos++;
	if(prim_tipo == 0x90){
		tri_h = (prim_vistos % 3 == 1) ? fnv(prim_estado, h) : fnv(tri_h, h);
		if(prim_vistos % 3 == 0){ fifo_h = fnv(fnv(fifo_h, 0x90u), tri_h); fifo_tris++; }
	} else prim_h = fnv(prim_h, h);
}

/* GX_Begin como el de libogc, y la huella del estado en cada dibujo */
void GX_Begin(u8 prim, u8 fmt, u16 n){
	prim_cierra();
	prim_tipo = prim;
	prim_n = n;
	prim_estado = fnv(state_h, fmt);
	prim_h = 2166136261u;
	wgPipe->U8 = prim | (fmt & 7);
	wgPipe->U16 = n;
	trace_h = fnv(fnv(trace_h, state_h), prim | (fmt << 8) | ((u32)n << 16));
	if(con_traza()){
		int f, i;
		fprintf(stderr, "dibujo %u: prim %u fmt %u n %u;", begins, prim, fmt, n);
		for(f = 0; f < F_N; f++){
			u32 h = 0;
			for(i = 0; i < 64; i++) h += mix(f, (u32)i, slot[f][i]);
			fprintf(stderr, " %d:%08X", f, h);
		}
		fprintf(stderr, "\n");
	}
	begins++;
}

void gx_ge_init(void);
void gx_ge_enable(int on);
static void report(void){
	prim_cierra();
	printf("[GX] %u dibujos, huella %08X; FIFO %u triangulos, %u primitivas, %u mal contadas, huella %08X\n", begins, trace_h,
	       fifo_tris, fifo_prims, fifo_malos, fifo_h);
	fflush(stdout);
}
void host_renderer_hook(void){
	gx_ge_init();
	gx_ge_enable(1);
	atexit(report);
}
