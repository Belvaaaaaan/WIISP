/* Banco: llamadas de dibujo como las de sceGu, por la lista del GE.
   bench_lista REPS VTYPE ESCENA [opcion=valor...]
   escena 0: VERTEXTYPE, BASE, VADDR, PRIM
   escena 1: + textura con paleta (10 comandos, mismos valores)
   escena 2: + 4 huesos y la matriz de mundo (65 comandos, mismos valores)
   escena 3: + 8 huesos
   escenas 4 a 10: objetos de 4 llamadas con su caja (ver LEEME.md); 7 a 9
   con el viewport al doble (los planos de +-w fuera del scissor) y 8 a 10
   con vtype 0x11A (sus propios vertices)
   Con LUCES=1 en el entorno, dos luces encendidas (vtype con normal). Con
   prof_estado=1 escribe el censo de estado al final. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/ge_internal.h"
#include "gpu/ge.h"
#include "gpu/ge_math.h"
#include "core/memory.h"
#include "core/opt.h"
void gx_ge_init(void);
void gx_ge_enable(int on);
void bench_run_list(u32 pc, u32 end);
static u32 f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }
static u32 rs = 1;
static u32 rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static u32 lp;
static void cmd(u32 c, u32 a){ mem_write32(lp, (c << 24) | (a & 0xFFFFFF)); lp += 4; }

int main(int argc, char **argv){
	u8 *ram = calloc(1, PSP_RAM_SIZE_32MB), *vram = calloc(1, PSP_VRAM_SIZE), *scr = calloc(1, PSP_SCRATCH_SIZE);
	int i, k, j, reps = argc > 1 ? atoi(argv[1]) : 200, esc = argc > 3 ? atoi(argv[3]) : 0, nb;
	u32 vt = argc > 2 ? (u32)strtoul(argv[2], NULL, 16) : 0x11A, start, end;
	u8 *p;
	mem_init(ram, PSP_RAM_SIZE_32MB, vram, scr);
	opt_reset();
	for(i = 4; i < argc; i++) opt_parse(argv[i]);
	gx_ge_init();
	gx_ge_enable(1);
	ge.cmd[GE_VIEWPORTXSCALE] = f24(240); ge.cmd[GE_VIEWPORTYSCALE] = f24(-136); ge.cmd[GE_VIEWPORTZSCALE] = f24(-32000);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048); ge.cmd[GE_VIEWPORTYCENTER] = f24(2048); ge.cmd[GE_VIEWPORTZCENTER] = f24(32000);
	ge.cmd[GE_OFFSETX] = (2048 - 240) * 16; ge.cmd[GE_OFFSETY] = (2048 - 136) * 16;
	ge.cmd[GE_TEXSCALEU] = f24(1); ge.cmd[GE_TEXSCALEV] = f24(1);
	ge.cmd[GE_TEXTUREMAPENABLE] = 1; ge.cmd[GE_SHADEMODE] = 1;
	ge.cmd[GE_SCISSOR2] = 479 | (271 << 10); ge.cmd[GE_REGION2] = 479 | (271 << 10);
	ge.cmd[GE_FRAMEBUFPTR] = 0; ge.cmd[GE_FRAMEBUFWIDTH] = 512; ge.cmd[GE_FRAMEBUFPIXFORMAT] = 3;
	ge.cmd[GE_ZBUFPTR] = 0x110000; ge.cmd[GE_ZBUFWIDTH] = 512;
	ge.cmd[GE_ZTESTENABLE] = 1; ge.cmd[GE_ZTEST] = 6; ge.cmd[GE_MASKRGB] = 0; ge.cmd[GE_MASKALPHA] = 0;
	ge.cmd[GE_TEXADDR0] = 0x900000; ge.cmd[GE_TEXBUFWIDTH0] = 64 | (0x08 << 16); ge.cmd[GE_TEXSIZE0] = 6 | (6 << 8);
	ge.cmd[GE_TEXFORMAT] = 3; ge.cmd[GE_TEXFUNC] = 0; ge.cmd[GE_TEXFILTER] = 1 | (1 << 8);
	ge.cmd[GE_VERTEXTYPE] = vt;
	if(getenv("LUCES")){ ge.cmd[GE_LIGHTINGENABLE] = 1; ge.cmd[GE_LIGHTENABLE0] = 1; ge.cmd[GE_LIGHTENABLE0 + 1] = 1; ge.cmd[GE_LIGHTTYPE0] = 0; ge.cmd[GE_LDX0 + 2] = f24(1.0f); ge.cmd[GE_LDX0 + 5] = f24(-1.0f); ge.cmd[GE_LAC0] = 0x404040; ge.cmd[GE_LAC0 + 1] = 0xFFFFFF; ge.cmd[GE_AMBIENTCOLOR] = 0x202020; ge.cmd[GE_MATERIALDIFFUSE] = 0xFFFFFF; }
	for(i = 0; i < 12; i++) ge.world[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	for(i = 0; i < 12; i++) ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	ge.view[11] = -5.0f;
	ge.proj[0] = 1.5f; ge.proj[5] = 2.0f; ge.proj[10] = -1.0f; ge.proj[11] = -1.0f; ge.proj[14] = -1.0f;
	for(k = 0; k < 8; k++) for(i = 0; i < 12; i++) ge.bone[k][i] = (i % 4 == 0) ? 1.0f : 0.01f * (float)i;
	p = mem_ptr(0x08900000, 0x10000);
	for(i = 0; i < 0x10000; i++) p[i] = (u8)(rnd() & 0x3F);
	/* La lista: reps llamadas */
	start = lp = 0x08A00000;
	nb = esc == 3 ? 8 : 4;
	if(esc >= 4){
		u8 *c = mem_ptr(0x08980000, 48);
		for(k = 0; k < 8; k++)
			for(j = 0; j < 3; j++){
				u32 v = (k >> j) & 1 ? 16384 : 0;
				c[k * 6 + j * 2] = (u8)v;
				c[k * 6 + j * 2 + 1] = (u8)(v >> 8);
			}
		if(esc == 5 || esc >= 7){ ge.proj[10] = -51.0f / 49.0f; ge.proj[14] = -100.0f / 49.0f; }
	}
	if(esc >= 7 && esc != 10){
		ge.cmd[GE_VIEWPORTXSCALE] = f24(480); ge.cmd[GE_VIEWPORTYSCALE] = f24(-272);
		ge.cmd[GE_DEPTHCLAMPENABLE] = esc == 7;
	}
	if(esc >= 8){
		vt = 0x11A;
		for(k = 0; k < 16; k++)
			for(j = 0; j < 20; j++){
				u8 *v = mem_ptr(0x08940000 + (u32)k * 0x400 + (u32)j * 12, 12);
				int c, q;
				for(c = 0; c < 3; c++){
					q = (int)(rnd() % 16384);
					if(c == 0) q = esc != 9 ? 1638 + q / 3 : 13107 + q * 3 / 16;
					v[6 + c * 2] = (u8)q;
					v[7 + c * 2] = (u8)(q >> 8);
				}
			}
	}
	for(i = 0; i < reps; i++){
		static u32 salto;
		if(esc >= 4 && i % 4 == 0){
			static const float m7[12] = { 3.0f, 0.0f, 6.0f, 0.0f, 0.02f, 0.0f, 0.0179f, 0.0f, -0.0089f, -1.0f, 0.0f, 3.0f };
			static const float m8[12] = { 8.0f, 0.0f, 0.0f, 0.0f, 8.0f, 0.0f, 0.0f, 0.0f, 4.0f, -4.0f, -2.0f, -1.0f };
			static const float m10[12] = { 24.0f, 0.0f, 0.0f, 0.0f, 8.0f, 0.0f, 0.0f, 0.0f, 4.0f, -12.0f, -2.0f, -1.0f };
			cmd(GE_WORLDMATRIXNUMBER, 0);
			if(esc >= 7) for(j = 0; j < 12; j++) cmd(GE_WORLDMATRIXDATA, f24((esc == 7 ? m7 : esc == 10 ? m10 : m8)[j] + (j == 9 ? 0.001f * (float)(i % 64) : 0.0f)));
			else for(j = 0; j < 12; j++) cmd(GE_WORLDMATRIXDATA, f24(j == 11 ? (esc == 5 ? -55.0f : 0.0f) : j == 9 ? (esc == 6 ? 30.0f : 0.0f) + 0.001f * (float)(i % 64) : ge.world[j]));
			cmd(GE_VERTEXTYPE, 0x100);
			cmd(GE_BASE, 0x080000);
			cmd(GE_VADDR, 0x980000);
			cmd(GE_BOUNDINGBOX, 8);
			cmd(GE_BASE, 0x080000);
			salto = lp;
			cmd(GE_BJUMP, 0);
		}
		if(esc >= 2 && esc <= 3){
			for(k = 0; k < nb; k++){
				cmd(GE_BONEMATRIXNUMBER, (u32)k * 12);
				for(j = 0; j < 12; j++) cmd(GE_BONEMATRIXDATA, f24(ge.bone[k][j]));
			}
			cmd(GE_WORLDMATRIXNUMBER, 0);
			for(j = 0; j < 12; j++) cmd(GE_WORLDMATRIXDATA, f24(ge.world[j]));
		}
		if(esc >= 1){
			cmd(GE_TEXMODE, 0); cmd(GE_TEXFORMAT, 3);
			cmd(GE_TEXADDR0, 0x900000); cmd(GE_TEXBUFWIDTH0, 64 | (0x08 << 16)); cmd(GE_TEXSIZE0, 6 | (6 << 8));
			cmd(GE_TEXFLUSH, 0);
			cmd(GE_CLUTFORMAT, 0xFF00); cmd(GE_CLUTADDR, 0x0F0000); cmd(GE_CLUTADDRUPPER, 0x080000); cmd(GE_LOADCLUT, 8);
		}
		cmd(GE_VERTEXTYPE, vt);
		cmd(GE_BASE, 0x080000);
		cmd(GE_VADDR, (esc >= 8 ? 0x940000 : 0x900000) + (u32)(i % 16) * 0x400);
		cmd(GE_PRIM, (GE_PRIM_TRIANGLE_STRIP << 16) | 20);
		if(esc >= 4 && i % 4 == 3) mem_write32(salto, (GE_BJUMP << 24) | (lp & 0xFFFFFF));
	}
	end = lp;
	bench_run_list(start, end);
	if(opt_on[OPT_PROF_ESTADO]){ static char b[2048]; GeStats z; memset(&z, 0, sizeof(z)); ge_state_census_report(&ge_stats, &z, b, sizeof(b)); printf("%s\n", b); }
	printf("estado de GX %u veces, %u sin emitir; triangulos %u, %u sin preparar\n", ge_stats.es_gx, ge_stats.es_gx_saltado, ge_stats.es_tri, ge_stats.es_tri_listo);
	printf("cajas %u fuera %u; llamadas con caja %u saltadas %u (formato %u matrices %u fuera de la caja %u); triangulos fuera %u\n", ge_stats.bx_cajas, ge_stats.bx_fuera, ge_stats.bx_llamadas, ge_stats.bx_saltadas, ge_stats.bx_formato, ge_stats.bx_matrices, ge_stats.bx_fuera_caja, ge_stats.tris_outside);
	printf("cajas fuera de la pantalla %u, dudosas %u (%u cruzan la camara); por su caja %u probadas, %u fuera, %u saltadas\n", ge_stats.bx_pantalla, ge_stats.bx_dudosas, ge_stats.bx_cruzan, ge_stats.bx_ll_probadas, ge_stats.bx_ll_fuera, ge_stats.bx_ll_saltadas);
	printf("%u comandos, %u vertices, %u triangulos, %u primitivas\n", ge_stats.commands, ge_stats.vertices, ge_stats.tris, ge_stats.primitives);
	return 0;
}
