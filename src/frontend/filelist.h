/**
 * WIISP - filelist.h
 * Listado de carpetas para el menú de selección de archivos.
 *
 * Es código portátil (solo stdio/dirent) para poder probarlo en PC; el
 * frontend de cada plataforma solo lo dibuja.
 *
 * Rutas al estilo libogc: "sd:/", "sd:/wiisp", "usb:/juegos/psp".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#ifndef WIISP_FILELIST_H
#define WIISP_FILELIST_H

#include <stddef.h>

#define FILELIST_MAX_ENTRIES 1024
#define FILELIST_NAME_LEN    128
#define FILELIST_PATH_LEN    512

typedef struct {
	char name[FILELIST_NAME_LEN];
	int  is_dir;
} FileEntry;

typedef struct {
	char path[FILELIST_PATH_LEN];
	FileEntry *entries;
	int count;
} FileList;

/* ¿Es un ejecutable que WIISP sabe abrir? (.pbp, .prx, .elf) */
int  filelist_is_supported(const char *name);

/* ¿Es la raíz de un dispositivo? ("sd:/", "usb:/", "/") */
int  filelist_is_root(const char *path);

/* Lee la carpeta: primero ".." (si no es la raíz), después las carpetas y
   luego los ejecutables, ordenados sin distinguir mayúsculas.
   Devuelve 0 si la carpeta se pudo abrir. */
int  filelist_load(FileList *fl, const char *path);
void filelist_free(FileList *fl);

/* Ruta de la carpeta padre ("sd:/wiisp/psp" -> "sd:/wiisp", "sd:/wiisp" -> "sd:/") */
void filelist_parent(const char *path, char *out, size_t out_size);

/* Une carpeta y nombre ("sd:/" + "wiisp" -> "sd:/wiisp") */
void filelist_join(const char *dir, const char *name, char *out, size_t out_size);

#endif
