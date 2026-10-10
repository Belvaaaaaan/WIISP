/**
 * WIISP - menu.c
 * Selector de archivos en la consola de texto: SD o USB, carpetas y
 * ejecutables de PSP (.pbp, .prx, .elf, .bin) e imagenes de UMD (.iso, .cso, .zso).
 *
 *   Arriba/abajo: moverse (izquierda/derecha: página)
 *   A: abrir carpeta / elegir archivo     B: carpeta anterior
 *   1 (Wiimote) o X (GameCube): cambiar entre SD y USB
 *   HOME (Wiimote) o START (GameCube): salir al Homebrew Channel
 *
 * La carpeta del último archivo ejecutado y el renderizador elegido (GX o
 * software) se guardan en un pequeño archivo de configuración.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include "wii/wii.h"
#include "hle/hle.h"
#include "frontend/filelist.h"

#define VISIBLE_ROWS 18
#define NAME_COLS    60

static const char *const devices[] = { "sd:/", "usb:/" };
#define NUM_DEVICES 2

static char config_path[256];
static char last_dir[FILELIST_PATH_LEN];
static int renderer_gx = 1;
static int lazy_textures = 1;

/* wiisp.cfg: carpeta, renderizador y comprobación de texturas */
static void save_config(void){
	FILE *f;
	if(!config_path[0]) return;
	f = fopen(config_path, "w");
	if(!f) return;
	fprintf(f, "%s\n%s\n%s\n", last_dir, renderer_gx ? "gx" : "soft", lazy_textures ? "texturas-rapidas" : "texturas-seguras");
	fclose(f);
}

void menu_set_config_path(const char *path){
	FILE *f;
	char line[32];
	snprintf(config_path, sizeof(config_path), "%s", path);
	f = fopen(config_path, "r");
	if(!f) return;
	if(fgets(last_dir, sizeof(last_dir), f)){
		last_dir[strcspn(last_dir, "\r\n")] = 0;
		if(fgets(line, sizeof(line), f)) renderer_gx = strncmp(line, "soft", 4) != 0;
		if(fgets(line, sizeof(line), f)) lazy_textures = strncmp(line, "texturas-seguras", 16) != 0;
	}
	fclose(f);
}

int menu_renderer_gx(void){ return renderer_gx; }

void menu_set_renderer_gx(int on){
	renderer_gx = on;
	save_config();
}

int menu_lazy_textures(void){ return lazy_textures; }

void menu_set_lazy_textures(int on){
	lazy_textures = on;
	save_config();
}

static int dir_exists(const char *path){
	DIR *d = opendir(path);
	if(!d) return 0;
	closedir(d);
	return 1;
}

void menu_remember(const char *file_path){
	filelist_parent(file_path, last_dir, sizeof(last_dir));
	save_config();
}

/* Carpeta inicial: la recordada, o sd:/wiisp, o la raíz de la SD o USB */
static void initial_dir(char *out, size_t size){
	int i;
	if(last_dir[0] && dir_exists(last_dir)){ snprintf(out, size, "%s", last_dir); return; }
	if(dir_exists("sd:/wiisp")){ snprintf(out, size, "sd:/wiisp"); return; }
	for(i = 0; i < NUM_DEVICES; i++)
		if(dir_exists(devices[i])){ snprintf(out, size, "%s", devices[i]); return; }
	snprintf(out, size, "sd:/");
}

static int current_device(const char *path){
	int i;
	for(i = 0; i < NUM_DEVICES; i++)
		if(!strncmp(path, devices[i], strlen(devices[i]) - 1)) return i;
	return 0;
}

static void draw(const FileList *fl, int sel, int top, int ok){
	int i, dev = current_device(fl->path);
	video_clear_console();
	printf("\n WIISP " WIISP_VERSION " - elige un programa de PSP");
	for(i = 0; i < NUM_DEVICES; i++){
		int present = dir_exists(devices[i]);
		const char *name = i == 0 ? "SD" : "USB";
		if(i == dev) printf("\x1b[33m[%s]\x1b[37m ", name);
		else printf(" %s%s ", name, present ? "" : "(no)");
	}
	printf("\n %.70s\n\n", fl->path);

	if(!ok) printf("   \x1b[31mNo se pudo abrir esta carpeta\x1b[37m\n");
	else if(fl->count == 0) printf("   (no hay carpetas ni juegos: .pbp .prx .elf .bin .iso .cso .zso)\n");
	for(i = top; i < fl->count && i < top + VISIBLE_ROWS; i++){
		const FileEntry *e = &fl->entries[i];
		const char *color = i == sel ? "\x1b[33m" : (e->is_dir ? "\x1b[36m" : "\x1b[37m");
		printf(" %s%s %s%.*s%s\x1b[37m\n", color, i == sel ? ">" : " ",
		       e->is_dir && strcmp(e->name, "..") ? "[" : "", NAME_COLS, e->name,
		       e->is_dir && strcmp(e->name, "..") ? "]" : "");
	}
	for(; i < top + VISIBLE_ROWS; i++) printf("\n");
	printf("\n A: abrir   B: atras   1/X: SD <-> USB   HOME/START: salir\n");
}

int menu_choose_file(char *out, int out_size){
	FileList fl = { "", NULL, 0 };
	char path[FILELIST_PATH_LEN];
	int sel = 0, top = 0, redraw = 1, ok;

	initial_dir(path, sizeof(path));
	video_show_console();
	ok = filelist_load(&fl, path) == 0;

	for(;;){
		Input in;
		input_read(&in);

		if(in.menu & IN_EXIT){ filelist_free(&fl); return 0; }
		if(in.menu & IN_UP && sel > 0){ sel--; redraw = 1; }
		if(in.menu & IN_DOWN && sel + 1 < fl.count){ sel++; redraw = 1; }
		if(in.menu & IN_LEFT){ sel = sel > VISIBLE_ROWS ? sel - VISIBLE_ROWS : 0; redraw = 1; }
		if(in.menu & IN_RIGHT && fl.count){
			sel = sel + VISIBLE_ROWS < fl.count ? sel + VISIBLE_ROWS : fl.count - 1;
			redraw = 1;
		}

		if(in.menu & (IN_BACK | IN_ACCEPT | IN_SWITCH)){
			char next[FILELIST_PATH_LEN] = "";
			if(in.menu & IN_SWITCH){
				int d = (current_device(path) + 1) % NUM_DEVICES;
				snprintf(next, sizeof(next), "%s", devices[d]);
			} else if(in.menu & IN_BACK){
				if(!filelist_is_root(path)) filelist_parent(path, next, sizeof(next));
			} else if(fl.count){
				const FileEntry *e = &fl.entries[sel];
				if(!strcmp(e->name, "..")) filelist_parent(path, next, sizeof(next));
				else if(e->is_dir) filelist_join(path, e->name, next, sizeof(next));
				else {
					filelist_join(path, e->name, out, (size_t)out_size);
					filelist_free(&fl);
					return 1;
				}
			}
			if(next[0]){
				snprintf(path, sizeof(path), "%s", next);
				ok = filelist_load(&fl, path) == 0;
				sel = top = 0;
				redraw = 1;
			}
		}

		if(sel < top) top = sel;
		if(sel >= top + VISIBLE_ROWS) top = sel - VISIBLE_ROWS + 1;
		if(redraw){ draw(&fl, sel, top, ok); redraw = 0; }
		VIDEO_WaitVSync();
	}
}
