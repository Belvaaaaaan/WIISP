/**
 * WIISP - app.c
 * Lógica de la aplicación compartida entre plataformas (ver app.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "frontend/app.h"
#include "frontend/guion.h"
#include "core/opt.h"
#include "core/prof.h"
#include "cpu/cpu.h"
#include "core/memory.h"
#include "loader/loader.h"
#include "loader/disc.h"
#include "loader/sfo.h"
#include "hle/hle.h"
#include "gpu/ge_internal.h"
#include "gpu/ge_math.h"

#define MAX_EXEC_SIZE (64u * 1024 * 1024)

static PspModule module;
static char exec_path[256];
static char host_dir[256];
static char root_dir[256];
static AppOutputFunc output_func;
static int verbose = 1;

void app_set_verbose(int v){ verbose = v; }

int app_init(void){
	u8 *ram = plat_alloc_big(PSP_RAM_SIZE_32MB);
	u8 *vram = plat_alloc_big(PSP_VRAM_SIZE);
	u8 *scratch = plat_alloc_big(PSP_SCRATCH_SIZE);
	opt_reset();
	if(!ram || !vram || !scratch){
		printf("Error: no hay memoria para la RAM de la PSP\n");
		return -1;
	}
	return mem_init(ram, PSP_RAM_SIZE_32MB, vram, scratch);
}

/* Lee el archivo entero. Devuelve NULL si falla. */
static u8 *read_file(const char *path, u32 *len){
	FILE *f = fopen(path, "rb");
	long size;
	u8 *buf = NULL;

	if(!f) return NULL;
	if(fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 &&
	   (unsigned long)size <= MAX_EXEC_SIZE && fseek(f, 0, SEEK_SET) == 0){
		buf = malloc((size_t)size);
		if(buf && fread(buf, 1, (size_t)size, f) != (size_t)size){
			free(buf);
			buf = NULL;
		}
		*len = (u32)size;
	}
	fclose(f);
	return buf;
}

int app_is_disc_image(const char *path){
	const char *dot = strrchr(path, '.');
	return dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".cso") || !strcasecmp(dot, ".zso"));
}

/* Un UMD: el ejecutable es PSP_GAME/SYSDIR/EBOOT.BIN (normalmente cifrado);
   si no se puede usar, BOOT.BIN, que algunos discos traen sin cifrar */
static int load_disc(const char *path){
	static const char *const execs[] = { "PSP_GAME/SYSDIR/EBOOT.BIN", "PSP_GAME/SYSDIR/BOOT.BIN" };
	char title[sizeof(module.title)] = "", disc_id[sizeof(module.disc_id)] = "";
	u32 len, i;
	u8 *buf;
	int err = LOADER_ERR_FORMAT;

	if(disc_open(path)){
		printf("Error: no es una imagen de UMD valida (ISO/CSO/ZSO)\n");
		return -1;
	}
	if(verbose) printf("Imagen %s: %u sectores\n", disc_format(), disc_sectors());
	buf = disc_read_file("PSP_GAME/PARAM.SFO", &len, 64 * 1024);
	if(buf){
		SfoFile sfo;
		if(sfo_parse(buf, len, &sfo) == LOADER_OK){
			sfo_get_string(&sfo, "TITLE", title, sizeof(title));
			sfo_get_string(&sfo, "DISC_ID", disc_id, sizeof(disc_id));
		}
		free(buf);
	}
	for(i = 0; i < 2; i++){
		u32 k;
		buf = disc_read_file(execs[i], &len, MAX_EXEC_SIZE);
		if(!buf) continue;
		for(k = 0; k < len && !buf[k]; k++);
		if(k == len){ free(buf); continue; }   /* BOOT.BIN vacío */
		mem_reset();
		err = loader_load_inplace(buf, len, 0, &module);
		free(buf);
		if(err == LOADER_OK){
			if(verbose && i) printf("EBOOT.BIN no se pudo usar: se carga BOOT.BIN\n");
			break;
		}
	}
	if(err){
		printf("Error: %s\n", loader_strerror(err));
		if(err == LOADER_ERR_ENCRYPTED) printf("Etiqueta de cifrado: 0x%08X\n", loader_last_tag);
		return -1;
	}
	memcpy(module.title, title, sizeof(title));
	memcpy(module.disc_id, disc_id, sizeof(disc_id));
	snprintf(exec_path, sizeof(exec_path), "disc0:/%s", execs[i]);
	return 0;
}

int app_load(const char *path, int max_imports, const char *imports_out){
	u32 len = 0;
	u8 *buf;
	const char *slash;
	int err;

	if(verbose) printf("Cargando %s\n", path);
	hle_log_new_program();
	hle_log("[WIISP] cargando %s\n", path);
	hle_shutdown();
	loader_free(&module);
	disc_close();
	loader_syscall_base = 0;

	if(app_is_disc_image(path)){
		if(load_disc(path)) return -1;
	} else {
		buf = read_file(path, &len);
		if(!buf){
			printf("Error: no se pudo leer el archivo\n");
			return -1;
		}
		mem_reset();
		err = loader_load_inplace(buf, len, 0, &module);
		free(buf);
		if(err){
			printf("Error: %s\n", loader_strerror(err));
			if(err == LOADER_ERR_ENCRYPTED) printf("Etiqueta de cifrado: 0x%08X\n", loader_last_tag);
			return -1;
		}
	}
	if(verbose) loader_print_info(&module, max_imports);
	hle_log("[WIISP] %s%s%s%s, %u imports\n", module.title[0] ? module.title : module.name,
	        module.disc_id[0] ? " (" : "", module.disc_id, module.disc_id[0] ? ")" : "", module.num_imports);

	/* La carpeta del ejecutable (o de la imagen) hace de ms0:/ y host0:/ */
	snprintf(host_dir, sizeof(host_dir), "%s", path);
	slash = strrchr(host_dir, '/');
	{
		char *bs = strrchr(host_dir, '\\');   /* rutas de Windows */
		if(bs && (!slash || bs > slash)) slash = bs;
	}
	if(slash) host_dir[slash - host_dir] = 0;
	else strcpy(host_dir, ".");
	if(!disc_is_open()) snprintf(exec_path, sizeof(exec_path), "ms0:/PSP/GAME/WIISP/%s", slash ? slash + 1 : path);
	if(root_dir[0] && !strncmp(path, root_dir, strlen(root_dir)) && path[strlen(root_dir)] == '/'){
		snprintf(host_dir, sizeof(host_dir), "%s", root_dir);
		if(!disc_is_open()) snprintf(exec_path, sizeof(exec_path), "ms0:%s", path + strlen(root_dir));
	}

	if(imports_out){
		if(hle_write_imports_report(&module, path, imports_out) == 0)
			printf("Informe de imports: %s\n", imports_out);
		else
			printf("Aviso: no se pudo escribir %s\n", imports_out);
	}
	return 0;
}

static void forward_output(const char *text, u32 len){
	if(output_func) output_func(text, len);
	else fwrite(text, 1, len, stdout);
}

static unsigned frames_run;
static u64 executed_at_start;

static char screenshot_path[512];

/* Expansión de bits igual que pspautotests/common/common.c */
static u32 bmp_pixel(const u8 *p, u32 format){
	u32 r, g, b, a;
	if(format == 3){ r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
	else {
		u32 c = (u32)p[0] | ((u32)p[1] << 8);
		if(format == 2){
			r = (c & 15) * 17; g = ((c >> 4) & 15) * 17; b = ((c >> 8) & 15) * 17; a = ((c >> 12) & 15) * 17;
		} else if(format == 1){
			r = ((c & 31) << 3) | ((c & 31) >> 2); g = (((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2);
			b = (((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2); a = (c & 0x8000) ? 255 : 0;
		} else {
			r = ((c & 31) << 3) | ((c & 31) >> 2); g = (((c >> 5) & 63) << 2) | (((c >> 5) & 63) >> 4);
			b = (((c >> 11) & 31) << 3) | (((c >> 11) & 31) >> 2); a = 255;
		}
	}
	return b | (g << 8) | (r << 16) | (a << 24);
}

int app_write_bmp(const char *path){
	static const u8 header[54] = {
		0x42, 0x4D, 0x38, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00,
		0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x10, 0x01,
		0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x80,
		0x08, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	HleFramebuffer fb;
	FILE *f;
	u32 y, x, bpp;
	hle_get_framebuffer(&fb);
	f = fopen(path, "wb");
	if(!f) return -1;
	fwrite(header, 1, sizeof(header), f);
	bpp = fb.format == 3 ? 4 : 2;
	for(y = 0; y < 272; y++){
		u8 row[512 * 4];
		for(x = 0; x < 512; x++){
			const u8 *p = fb.addr ? mem_ptr_r(fb.addr + ((271 - y) * fb.stride + x) * bpp, bpp) : NULL;
			u32 v = p ? bmp_pixel(p, fb.format) : 0;
			wr_le32(row + x * 4, v);
		}
		fwrite(row, 1, sizeof(row), f);
	}
	fclose(f);
	return 0;
}

static void on_screenshot(void){
	if(screenshot_path[0]) app_write_bmp(screenshot_path);
}

void app_set_screenshot_path(const char *path){
	snprintf(screenshot_path, sizeof(screenshot_path), "%s", path ? path : "");
}

/* --- Guion de botones (frontend/guion.h) --------------------------------- */

enum { G_NADA, G_REPRODUCIR, G_GRABAR };

static struct {
	int modo;
	Guion g;
	u32 pos;              /* siguiente línea por aplicar */
	int terminado;        /* se llegó a "fin" */
	char prefijo[512];    /* capturas: <prefijo>.c<cuadro>.bmp */
	FILE *huellas;
	FILE *grabacion;
	u32 botones;          /* último estado grabado */
	u8 lx, ly;
	int grabado;          /* ya hay una línea de mando */
	FILE *indice;         /* <prefijo>.capturas.txt */
	u32 ultima_captura;   /* para no repetir la del mismo cuadro */
} gs;

static AppOskFunc osk_func;
static void (*before_capture)(void);
static void (*mode_text)(char *buf, size_t size);
static unsigned capturas_cada = 400;

#define HUELLA_CADA 60    /* cuadros entre huellas */

static u32 fnv(u32 h, const u8 *p, u32 n){
	while(n--){ h ^= *p++; h *= 16777619u; }
	return h;
}

static u32 fnv32(u32 h, u32 v){
	u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
	return fnv(h, b, 4);
}

/* Huella del estado emulado: si dos corridas del mismo guion se separan, la
   primera huella distinta dice en qué segundo empezó. Los valores se mezclan
   como números (no como bytes del anfitrión) para que salgan iguales en el
   PC y en el Wii. La RAM se muestrea (una palabra cada 1 KB, empezando cada
   vez en otra posición para cubrir toda la RAM con el tiempo) para que
   cueste poco; en las capturas y al final se usa entera. */
static void huella(const char *motivo, int ram_entera){
	u32 h = 2166136261u, r = 2166136261u, i;
	int old;
	if(!gs.huellas) return;
	old = prof_switch(PROF_REGISTRO);
	for(i = 0; i < 32; i++) h = fnv32(h, cpu.r[i]);
	h = fnv32(h, cpu.hi);
	h = fnv32(h, cpu.lo);
	for(i = 0; i < 32; i++) h = fnv32(h, cpu.fpr[i].u);
	h = fnv32(h, cpu.fcr31);
	if(ram_entera) r = fnv(r, psp_mem.ram, psp_mem.ram_size);
	else for(i = (frames_run / HUELLA_CADA * 4) & 1023; i < psp_mem.ram_size; i += 1024) r = fnv(r, psp_mem.ram + i, 4);
	fprintf(gs.huellas, "cuadro %u ciclos %llu pc %08X cpu %08X ram%s %08X%s%s\n", frames_run,
	        (unsigned long long)cpu_cycles, cpu.pc, h, ram_entera ? "_entera" : "", r,
	        motivo ? " " : "", motivo ? motivo : "");
	fflush(gs.huellas);
	prof_switch(old);
}

/* Con qué opciones corría el juego: las del frontend (renderizador, MR/ME,
   TN/TS...) y los interruptores de optimización */
static void describir_modo(char *buf, size_t size){
	char opts[256];
	size_t len = 0;
	buf[0] = 0;
	if(mode_text){
		mode_text(buf, size);
		len = strlen(buf);
	}
	opt_describe(opts, sizeof(opts));
	if(len < size) snprintf(buf + len, size - len, "%s[OPT] %s", len ? " " : "", opts);
}

/* Guarda la imagen actual como <prefijo>.c<cuadro>.bmp y la apunta en
   <prefijo>.capturas.txt con las opciones del momento. Las capturas que
   pide el guion llevan además una huella con la RAM entera; las periódicas
   no, porque recorrer 32 MB cada pocos segundos frenaría la escena. */
static void capturar(int pedida){
	char path[560], modo[400];
	int old;
	if(frames_run == gs.ultima_captura && frames_run) return;
	gs.ultima_captura = frames_run;
	old = prof_switch(PROF_REGISTRO);
	if(before_capture) before_capture();
	snprintf(path, sizeof(path), "%s.c%06u.bmp", gs.prefijo, frames_run);
	app_write_bmp(path);
	if(!gs.indice){
		char ip[560];
		snprintf(ip, sizeof(ip), "%s.capturas.txt", gs.prefijo);
		gs.indice = fopen(ip, "w");
		if(gs.indice) fprintf(gs.indice, "# capturas de WIISP: cuadro, archivo y opciones con las que corria\n");
	}
	if(gs.indice){
		const char *name = strrchr(path, '/');
		describir_modo(modo, sizeof(modo));
		fprintf(gs.indice, "cuadro %u  %s  %s\n", frames_run, name ? name + 1 : path, modo);
		fflush(gs.indice);
	}
	prof_switch(old);
	if(pedida) huella("captura", 1);
}

void app_set_mode_text(void (*fn)(char *buf, size_t size)){ mode_text = fn; }
void app_guion_capturas_cada(unsigned cuadros){ capturas_cada = cuadros; }

void app_capturas_sin_guion(const char *prefijo){
	if(gs.modo) return;
	app_guion_parar();
	snprintf(gs.prefijo, sizeof(gs.prefijo), "%s", prefijo);
}

static void grabar_linea(void){
	char line[160];
	int old = prof_switch(PROF_REGISTRO);
	guion_format(line, sizeof(line), frames_run, gs.botones, gs.lx, gs.ly);
	fprintf(gs.grabacion, "%s\n", line);
	fflush(gs.grabacion);
	prof_switch(old);
}

static void abrir_huellas(const char *path){
	char modo[400];
	gs.huellas = path && path[0] ? fopen(path, "w") : NULL;
	if(!gs.huellas) return;
	describir_modo(modo, sizeof(modo));
	fprintf(gs.huellas, "# huellas de WIISP " WIISP_VERSION ": una cada %u cuadros\n# %s\n", HUELLA_CADA, modo);
}

int app_guion_reproducir(const char *guion, const char *prefijo, char *err, size_t errlen){
	char path[560];
	app_guion_parar();
	if(guion_load(&gs.g, guion, err, errlen)) return -1;
	gs.modo = G_REPRODUCIR;
	snprintf(gs.prefijo, sizeof(gs.prefijo), "%s", prefijo);
	snprintf(path, sizeof(path), "%s.huellas", prefijo);
	abrir_huellas(path);
	hle_set_osk(NULL);   /* el teclado no puede depender de quien mire */
	return 0;
}

int app_guion_grabar(const char *guion){
	char path[560];
	app_guion_parar();
	gs.grabacion = fopen(guion, "w");
	if(!gs.grabacion) return -1;
	fprintf(gs.grabacion, "# guion de WIISP: cuadro y botones (ver src/frontend/guion.h)\n");
	gs.modo = G_GRABAR;
	snprintf(gs.prefijo, sizeof(gs.prefijo), "%s", guion);
	snprintf(path, sizeof(path), "%s.huellas", guion);
	abrir_huellas(path);
	hle_set_osk(NULL);
	return 0;
}

void app_guion_parar(void){
	if(gs.modo == G_GRABAR && gs.grabacion){
		fprintf(gs.grabacion, "%u fin\n", frames_run);
		huella("fin", 1);
	}
	if(gs.grabacion) fclose(gs.grabacion);
	if(gs.huellas) fclose(gs.huellas);
	if(gs.indice) fclose(gs.indice);
	guion_free(&gs.g);
	memset(&gs, 0, sizeof(gs));
	hle_set_osk(osk_func);
}

int app_guion_activo(void){ return gs.modo; }
unsigned app_guion_fin(void){ return gs.modo == G_REPRODUCIR ? guion_fin(&gs.g) : 0; }
void app_set_before_capture(void (*fn)(void)){ before_capture = fn; }

int app_preparar_partidas(const char *dir, const char *base){
	if(guion_preparar_partidas(dir, base)) return -1;
	io_set_savedata_dir(dir);
	return 0;
}
void app_set_savedata_dir(const char *dir){ io_set_savedata_dir(dir); }

/* Aplica las líneas del guion que tocan antes de emular este cuadro.
   Devuelve 1 si el guion terminó. */
static int guion_paso(void){
	while(gs.pos < gs.g.n && gs.g.l[gs.pos].cuadro <= frames_run){
		const GuionLinea *l = &gs.g.l[gs.pos++];
		if(l->flags & GUION_MANDO) hle_set_input(l->botones, l->lx, l->ly);
		if(l->flags & GUION_CAPTURA) capturar(1);
		if(l->flags & GUION_FIN){
			huella("fin", 1);
			gs.terminado = 1;
			return 1;
		}
	}
	return 0;
}

int app_start(void){
	hle_set_output(forward_output);
	hle_set_screenshot(on_screenshot);
	frames_run = 0;
	executed_at_start = cpu_executed;
	gs.pos = 0;
	gs.terminado = 0;
	gs.grabado = 0;
	gs.ultima_captura = 0;
	return hle_init(&module, host_dir, exec_path);
}

int app_run_frame(void){
	int r;
	if(gs.terminado) return 1;
	if(gs.modo == G_REPRODUCIR && guion_paso()) return 1;
	if((gs.modo || gs.prefijo[0]) && capturas_cada && frames_run && frames_run % capturas_cada == 0) capturar(0);
	frames_run++;
	r = hle_run_frame();
	if(gs.modo && r) huella("termino", 1);
	else if(gs.modo && frames_run % HUELLA_CADA == 0) huella(NULL, 0);
	return r;
}

void app_get_stats(unsigned *frames, unsigned long long *instructions){
	*frames = frames_run;
	*instructions = cpu_executed - executed_at_start;
}

void app_imports_path(const char *path, char *out, size_t out_size){
	const char *slash = strrchr(path, '/'), *name = slash ? slash + 1 : path;
	const char *dot = strrchr(name, '.');
	int dir_len = (int)(name - path);
	if(!strcasecmp(name, "EBOOT.PBP"))
		snprintf(out, out_size, "%.*simports.txt", dir_len, path);
	else
		snprintf(out, out_size, "%.*s%.*s.imports.txt", dir_len, path,
		         (int)(dot ? dot - name : (long)strlen(name)), name);
}
const char *app_exit_reason(void){ return gs.terminado ? "fin del guion" : hle_exit_reason(); }
void app_set_output(AppOutputFunc func){ output_func = func; }
void app_set_osk(AppOskFunc func){
	osk_func = func;
	if(!gs.modo) hle_set_osk(func);
}

void app_set_input(unsigned buttons, unsigned char lx, unsigned char ly){
	if(gs.modo == G_REPRODUCIR) return;   /* manda el guion */
	if(gs.modo == G_GRABAR){
		lx = guion_cuantizar(lx);
		ly = guion_cuantizar(ly);
		if(!gs.grabado || buttons != gs.botones || lx != gs.lx || ly != gs.ly){
			gs.botones = buttons;
			gs.lx = lx;
			gs.ly = ly;
			gs.grabado = 1;
			grabar_linea();
		}
	}
	hle_set_input(buttons, lx, ly);
}

int app_opt_set(const char *texto){ return opt_parse(texto); }
void app_opt_describe(char *buf, size_t size){ opt_describe(buf, size); }

const unsigned char *app_get_framebuffer(unsigned *stride, unsigned *format){
	HleFramebuffer fb;
	u32 bpp;
	hle_get_framebuffer(&fb);
	if(!fb.addr) return NULL;
	bpp = fb.format == 3 ? 4 : 2;
	*stride = fb.stride;
	*format = fb.format;
	return mem_ptr_r(fb.addr, fb.stride * (APP_SCREEN_H - 1) * bpp + APP_SCREEN_W * bpp);
}

void app_set_host_clock(unsigned long long (*clock)(void)){ ge_host_clock = clock; }

unsigned long long app_ge_host_ticks(void){
	GeStats st;
	ge_get_stats(&st);
	return st.host_ticks;
}

static void null_begin(void){}
static void null_tri(const GeVertex *a, const GeVertex *b, const GeVertex *c){ (void)a; (void)b; (void)c; }
static void null_two(const GeVertex *a, const GeVertex *b){ (void)a; (void)b; }
static void null_one(const GeVertex *a){ (void)a; }
static const GeHwRenderer null_renderer = { null_begin, null_tri, null_two, null_two, null_two, null_one, null_begin };

void app_set_turbo(int on){ ge_turbo = on; }
int  app_turbo(void){ return ge_turbo; }

void app_set_null_renderer(void){ ge_hw = &null_renderer; ge_fast_math = 1; }

void app_set_fast_math(int on){ ge_fast_math = on; }
int  app_fast_math(void){ return ge_fast_math; }

void app_set_root(const char *dir){
	size_t n;
	snprintf(root_dir, sizeof(root_dir), "%s", dir);
	n = strlen(root_dir);
	while(n > 1 && root_dir[n - 1] == '/') root_dir[--n] = 0;
}
