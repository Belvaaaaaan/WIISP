/* Banco: listas al azar para comprobar ge_charolas y ge_charolas_envio con el
   renderizador de GX (gx_ge.c con -DWIISP_FIFO_TRAZA sobre stub_estado.c).
   bench_charolas SEMILLA LLAMADAS [opcion=valor...]
   Entre llamada y llamada cambia, a veces, lo que lee la charola (destinos,
   máscaras, LIGHTMODE, textura, paleta, sombreado, mezcla, tipo de vértice,
   primitiva...) y la lista se corre por tramos: entre tramos, lo que haría la
   CPU (escribir la textura con o sin aviso, otro periodo, un aviso de vaciar
   la caché). Al final, la huella del estado de GX y la del FIFO
   (stub_estado.c), los contadores del GE y los de la caché de texturas: con
   y sin los interruptores tienen que salir iguales (salvo el número de
   GX_Begin con ge_charolas_envio: comparar con --fifo, ver banco_charolas.sh).
   CAMBIOS=N en el entorno: N % de llamadas con algún cambio antes (40).
   REGISTRO=archivo: el registro (las líneas [VERIF] de ge_verificar).
   La textura puede estar en la VRAM, dentro o fuera de un búfer en el que
   se dibuja, y las máscaras pueden dejar la llamada sin dibujar nada.
   Algunos sprites no tienen ancho (no dibujan ni buscan la textura). */
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
void gx_ge_texture_report(char *buf, size_t size);
void host_renderer_hook(void);
void bench_run_list(u32 pc, u32 end);
void ge_vdec_verif_report(char *buf, size_t size);
void hle_set_log_file(const char *path);
static u32 f24(float f){ u32 b; memcpy(&b, &f, 4); return b >> 8; }
static u32 rs;
static u32 rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static u32 rn(u32 n){ return rnd() % n; }
static u32 lp;
static void cmd(u32 c, u32 a){ mem_write32(lp, (c << 24) | (a & 0xFFFFFF)); lp += 4; }

#define TEX_A 0x08900000u
#define TEX_B 0x08904000u
#define CLUT_1 0x080F0000u
#define CLUT_2 0x080F0400u
#define VTX 0x08940000u
#define TEX_V1 0x04020000u   /* dentro del búfer 0 */
#define TEX_V2 0x041A0000u   /* fuera de los búferes */

int main(int argc, char **argv){
	u8 *ram = calloc(1, PSP_RAM_SIZE_32MB), *vram = calloc(1, PSP_VRAM_SIZE), *scr = calloc(1, PSP_SCRATCH_SIZE);
	int i, k, j, n = argc > 2 ? atoi(argv[2]) : 400, cambios = getenv("CAMBIOS") ? atoi(getenv("CAMBIOS")) : 40;
	u32 cortes[64], tcorte[64], ncortes = 0, start, end, tex = TEX_A;
	u8 *p;
	char rep[512];
	rs = argc > 1 ? (u32)strtoul(argv[1], NULL, 0) | 1u : 1u;
	mem_init(ram, PSP_RAM_SIZE_32MB, vram, scr);
	opt_reset();
	for(i = 3; i < argc; i++) opt_parse(argv[i]);
	if(getenv("REGISTRO")) hle_set_log_file(getenv("REGISTRO"));   /* las líneas [VERIF], a ese archivo */
	host_renderer_hook();
	ge.cmd[GE_VIEWPORTXSCALE] = f24(240); ge.cmd[GE_VIEWPORTYSCALE] = f24(-136); ge.cmd[GE_VIEWPORTZSCALE] = f24(-32000);
	ge.cmd[GE_VIEWPORTXCENTER] = f24(2048); ge.cmd[GE_VIEWPORTYCENTER] = f24(2048); ge.cmd[GE_VIEWPORTZCENTER] = f24(32000);
	ge.cmd[GE_OFFSETX] = (2048 - 240) * 16; ge.cmd[GE_OFFSETY] = (2048 - 136) * 16;
	ge.cmd[GE_TEXSCALEU] = f24(1); ge.cmd[GE_TEXSCALEV] = f24(1);
	ge.cmd[GE_TEXTUREMAPENABLE] = 1; ge.cmd[GE_SHADEMODE] = 1;
	ge.cmd[GE_SCISSOR2] = 479 | (271 << 10); ge.cmd[GE_REGION2] = 479 | (271 << 10);
	ge.cmd[GE_FRAMEBUFPTR] = 0; ge.cmd[GE_FRAMEBUFWIDTH] = 512; ge.cmd[GE_FRAMEBUFPIXFORMAT] = 3;
	ge.cmd[GE_ZBUFPTR] = 0x110000; ge.cmd[GE_ZBUFWIDTH] = 512;
	ge.cmd[GE_ZTESTENABLE] = 1; ge.cmd[GE_ZTEST] = 6;
	ge.cmd[GE_TEXSIZE0] = 6 | (6 << 8);
	ge.cmd[GE_TEXFORMAT] = 5; ge.cmd[GE_TEXFUNC] = 0; ge.cmd[GE_TEXFILTER] = 1 | (1 << 8);
	ge.cmd[GE_CLUTFORMAT] = 3 | (0xFF << 8);
	for(i = 0; i < 12; i++) ge.world[i] = ge.view[i] = (i % 4 == 0) ? 1.0f : 0.0f;
	ge.view[11] = -5.0f;
	ge.proj[0] = 1.5f; ge.proj[5] = 2.0f; ge.proj[10] = -1.0f; ge.proj[11] = -1.0f; ge.proj[14] = -1.0f;
	for(k = 0; k < 8; k++) for(i = 0; i < 12; i++) ge.bone[k][i] = (i % 4 == 0) ? 1.0f : 0.01f * (float)i;
	p = mem_ptr(TEX_A, 0x8000);
	for(i = 0; i < 0x8000; i++) p[i] = (u8)rnd();
	p = mem_ptr(CLUT_1, 0x800);
	for(i = 0; i < 0x800; i++) p[i] = (u8)rnd();
	/* Vértices: 0x11A (uv16 c4444 p16), 0xC301 (uv8 p16, 4 pesos) y 2D 0x80011A, en la pantalla */
	p = mem_ptr(VTX, 0x10000);
	for(i = 0; i < 0x10000; i++) p[i] = (u8)(rnd() & 0x3F);
	for(i = 0; i < 0x4000; i += 12){   /* 2D: x 0..479, y 0..271 */
		u8 *q = mem_ptr(VTX + 0x8000 + (u32)i, 12);
		u32 x = rn(480), y = rn(272);
		q[6] = (u8)x; q[7] = (u8)(x >> 8); q[8] = (u8)y; q[9] = (u8)(y >> 8); q[10] = (u8)rn(256); q[11] = 0;
	}
	for(i = 0; i < 0x1000; i += 24){   /* sprites de ancho 0, en 3D y en 2D */
		memcpy(mem_ptr(VTX + 0xC000 + (u32)i, 12), mem_ptr(VTX + (u32)i, 12), 12);
		memcpy(mem_ptr(VTX + 0xC000 + (u32)i + 12, 12), mem_ptr(VTX + (u32)i, 12), 12);
		memcpy(mem_ptr(VTX + 0xD000 + (u32)i, 12), mem_ptr(VTX + 0x8000 + (u32)i, 12), 12);
		memcpy(mem_ptr(VTX + 0xD000 + (u32)i + 12, 12), mem_ptr(VTX + 0x8000 + (u32)i, 12), 12);
	}

	k = (int)rn(10);   /* la textura del principio, también al azar */
	tex = k < 3 ? TEX_A : k < 6 ? TEX_B : k < 9 ? TEX_V1 : TEX_V2;
	ge.cmd[GE_TEXADDR0] = tex & 0xFFFFFF; ge.cmd[GE_TEXBUFWIDTH0] = 64 | ((tex >> 8) & 0x0F0000);
	start = lp = 0x08A00000;
	cmd(GE_CLUTADDR, CLUT_1 & 0xFFFFFF); cmd(GE_CLUTADDRUPPER, (CLUT_1 >> 8) & 0x0F0000); cmd(GE_LOADCLUT, 8);
	for(i = 0; i < n; i++){
		u32 r = rn(100), vt, prim, count;
		if((int)r < cambios){
			switch(rn(19)){
			case 0: cmd(GE_FRAMEBUFPTR, rn(2) ? 0 : 0x44000); break;
			case 1: cmd(GE_ZBUFPTR, rn(2) ? 0x110000 : 0x154000); break;
			case 2: k = (int)rn(4); cmd(GE_MASKRGB, k < 2 ? 0 : k == 2 ? 0xFF : 0xFFFFFF); break;
			case 3: cmd(GE_MASKALPHA, rn(2) ? 0 : 0xFF); break;
			case 4: cmd(GE_LIGHTMODE, rn(2)); break;
			case 5:
				k = (int)rn(10);
				tex = k < 3 ? TEX_A : k < 6 ? TEX_B : k < 9 ? TEX_V1 : TEX_V2;
				cmd(GE_TEXADDR0, tex & 0xFFFFFF); cmd(GE_TEXBUFWIDTH0, 64 | ((tex >> 8) & 0x0F0000)); cmd(GE_TEXFLUSH, 0);
				break;
			case 6: k = (int)rn(2); cmd(GE_CLUTADDR, (k ? CLUT_1 : CLUT_2) & 0xFFFFFF); cmd(GE_LOADCLUT, rn(3) ? 8 : 4); break;
			case 7: cmd(GE_SHADEMODE, rn(2)); break;
			case 8: cmd(GE_ALPHABLENDENABLE, rn(2)); cmd(GE_BLENDMODE, rn(11) | (rn(11) << 4)); break;
			case 9: cmd(GE_ALPHATESTENABLE, rn(2)); break;
			case 10: cmd(GE_TEXTUREMAPENABLE, rn(2)); break;
			case 11: cmd(GE_TEXFUNC, rn(5)); break;
			case 12: cmd(GE_ZTESTENABLE, rn(2)); break;
			case 13: cmd(GE_FRAMEBUFPIXFORMAT, rn(4)); break;
			case 14: cmd(GE_LOADCLUT, rn(3) ? 8 : 4); break;   /* la misma paleta o la mitad */
			case 15: cmd(GE_TEXFORMAT, rn(2) ? 5 : 3); break;
			case 16: cmd(GE_FRAMEBUFWIDTH, rn(2) ? 512 : 480); break;
			case 17: k = (int)rn(2); cmd(GE_MASKRGB, k ? 0xFFFFFF : 0); cmd(GE_ZWRITEDISABLE, (u32)k); break;   /* sin dibujar nada */
			default: cmd(GE_ZWRITEDISABLE, rn(2)); break;
			}
		}
		k = (int)rn(10);
		vt = k < 5 ? 0x11A : k < 8 ? 0xC301 : 0x80011A;
		if(vt == 0xC301){
			for(k = 0; k < 4; k++){
				cmd(GE_BONEMATRIXNUMBER, (u32)k * 12);
				for(j = 0; j < 12; j++) cmd(GE_BONEMATRIXDATA, f24(ge.bone[k][j] + (rn(4) == 0 ? 0.001f * (float)rn(9) : 0.0f)));
			}
		}
		k = (int)rn(20);
		prim = k < 12 ? GE_PRIM_TRIANGLE_STRIP : k < 15 ? GE_PRIM_TRIANGLE_FAN : k < 18 ? GE_PRIM_TRIANGLES : GE_PRIM_RECTANGLES;
		count = prim == GE_PRIM_TRIANGLES ? 3 * (1 + rn(8)) : prim == GE_PRIM_RECTANGLES ? 2 * (1 + rn(3)) : 3 + rn(20);
		cmd(GE_VERTEXTYPE, vt);
		cmd(GE_BASE, ((VTX >> 8) & 0x0F0000));
		if(prim == GE_PRIM_RECTANGLES && vt != 0xC301 && rn(3) == 0) cmd(GE_VADDR, (VTX + (vt == 0x11A ? 0xC000 : 0xD000) + rn(0x80) * 24) & 0xFFFFFF);
		else cmd(GE_VADDR, (vt == 0x80011A ? VTX + 0x8000 : VTX) + rn(0x400) * 4 & 0xFFFFFF);
		cmd(GE_PRIM, (prim << 16) | count);
		if(rn(8) == 0 && ncortes < 64){ tcorte[ncortes] = tex; cortes[ncortes++] = lp; }
	}
	end = lp;
	/* Por tramos: entre uno y otro, lo que haría la CPU */
	{
		u32 a = start;
		for(k = 0; k <= (int)ncortes; k++){
			u32 b = k < (int)ncortes ? cortes[k] : end;
			bench_run_list(a, b);
			a = b;
			if(getenv("TROZOS")){ gx_ge_texture_report(rep, sizeof(rep)); fprintf(stderr, "trozo %d: %u llamadas; %s\n", k, ge_stats.draws, rep); }
			u32 tt = k < (int)ncortes ? tcorte[k] : tex;
			switch(rn(8)){
			case 0: { u8 *t = mem_ptr(TEX_A + rn(0x1000), 4); t[0] ^= 0x55; mem_note_write(TEX_A, 0x1000); if(getenv("TROZOS")) fprintf(stderr, "  A con aviso\n"); } break;
			case 1: { u8 *t = mem_ptr(TEX_B + rn(0x1000), 4); t[0] ^= 0x33; } break;
			case 2: ge_sync_domain++; break;
			case 3: mem_note_write_all(); break;
			case 4: {   /* una paleta, dentro de lo que se carga, con aviso */
				u32 c = rn(2) ? CLUT_1 : CLUT_2;
				u8 *t = mem_ptr(c + rn(0x100), 4);
				t[0] ^= 0x11; mem_note_write(c, 0x400);
			} break;
			case 5: case 6: {   /* la textura en uso, sin aviso (en todo su tamaño); a veces con otro periodo */
				u8 *q = mem_ptr(tt, 0x1000);
				for(j = 0; j < 0x1000; j += 64) q[j + (int)rn(64)] ^= 0x5A;
				if(rn(2)) ge_sync_domain++;
				if(getenv("TROZOS")) fprintf(stderr, "  %08X sin aviso\n", (unsigned)tt);
			} break;
			default: break;
			}
		}
	}
	gx_ge_texture_report(rep, sizeof(rep));
	printf("%s\n", rep);
	printf("GE: %u comandos, %u vertices, %u triangulos (%u dibujados, %u fuera, %u de espaldas), %u primitivas, %u llamadas\n",
	       ge_stats.commands, ge_stats.vertices, ge_stats.tris, ge_stats.tris_drawn, ge_stats.tris_outside, ge_stats.tris_back,
	       ge_stats.primitives, ge_stats.draws);
	if(opt_on[OPT_GE_VERIF]){   /* con ge_verificar, lo comparado y lo distinto (en otra línea: lo demás tiene que salir igual) */
		static char vr[512];
		ge_vdec_verif_report(vr, sizeof(vr));
		printf("VERIF charolas %u/%u, estado de GX %u/%u (%u sin libogc), triangulos sin preparar %u/%u; %s\n",
		       ge_stats.chv_llamadas, ge_stats.chv_malas, ge_stats.es_gx_verif, ge_stats.es_gx_verif_bad, ge_stats.es_gx_verif_sin,
		       ge_stats.es_tri_verif, ge_stats.es_tri_verif_bad, vr);
	}
	fprintf(stderr, "charolas: %u llamadas, %u siguieron, %u sin charola, %u otra clave, %u textura\n", ge_stats.ch_llamadas,
	        ge_stats.ch_siguen, ge_stats.ch_sin, ge_stats.ch_clave, ge_stats.ch_textura);
	return 0;
}
