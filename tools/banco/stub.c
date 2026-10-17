/* libogc de mentira para correr gx_ge.c bajo qemu (banco de pruebas) */
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
typedef union { unsigned char U8; signed char S8; unsigned short U16; short S16; unsigned int U32; int S32; float F32; } WGPipeS;
static volatile WGPipeS pipe_mem;
volatile WGPipeS *const wgPipe = &pipe_mem;
#define NOP(n) void n(void){}
NOP(GX_ClearVtxDesc) NOP(GX_CopyDisp) NOP(GX_CopyTex) NOP(GX_DrawDone) NOP(GX_InitTexObj) NOP(GX_InitTexObjCI)
NOP(GX_InitTexObjFilterMode) NOP(GX_InitTexObjLOD) NOP(GX_InitTexObjWrapMode) NOP(GX_InitTlutObj) NOP(GX_InvalidateTexAll)
NOP(GX_LoadPosMtxImm) NOP(GX_LoadProjectionMtx) NOP(GX_LoadTexMtxImm) NOP(GX_LoadTexObj) NOP(GX_LoadTlut) NOP(GX_PixModeSync)
NOP(GX_SetAlphaCompare) NOP(GX_SetAlphaUpdate) NOP(GX_SetBlendMode) NOP(GX_SetChanCtrl) NOP(GX_SetClipMode) NOP(GX_SetCoPlanar)
NOP(GX_SetColorUpdate) NOP(GX_SetCopyFilter) NOP(GX_SetCullMode) NOP(GX_SetCurrentMtx) NOP(GX_SetDispCopyDst) NOP(GX_SetDispCopySrc)
NOP(GX_SetDither) NOP(GX_SetDstAlpha) NOP(GX_SetLineWidth) NOP(GX_SetNumChans) NOP(GX_SetNumIndStages) NOP(GX_SetNumTevStages)
NOP(GX_SetNumTexGens) NOP(GX_SetPixelFmt) NOP(GX_SetPointSize) NOP(GX_SetScissor) NOP(GX_SetTevAlphaIn) NOP(GX_SetTevAlphaOp)
NOP(GX_SetTevColorIn) NOP(GX_SetTevColorOp) NOP(GX_SetTevKAlphaSel) NOP(GX_SetTevKColor) NOP(GX_SetTevKColorSel) NOP(GX_SetTevOp)
NOP(GX_SetTevOrder) NOP(GX_SetTexCoordGen) NOP(GX_SetTexCopyDst) NOP(GX_SetTexCopySrc) NOP(GX_SetViewport) NOP(GX_SetVtxAttrFmt)
NOP(GX_SetVtxDesc) NOP(GX_SetZCompLoc) NOP(GX_SetZMode) NOP(GX_SetZTexture) NOP(PPCDCacheFlushAsync) NOP(PPCDCacheInvalidate)
NOP(ps_guMtxIdentity) NOP(ps_guMtxScale)
/* Testigos (gx_paralelo): la GPU de mentira acaba en el acto */
static unsigned short draw_sync;
void GX_SetDrawSync(unsigned short tok){ draw_sync = tok; }
unsigned short GX_GetDrawSync(void){ return draw_sync; }
/* GX_Begin como el de libogc, sin estado sucio */
void GX_Begin(unsigned char prim, unsigned char fmt, unsigned short n){ wgPipe->U8 = prim | (fmt & 7); wgPipe->U16 = n; }

/* SIGILL (mfmsr, mtspr, psq_l...): ../ppcemu/sigill_ppc.c */
