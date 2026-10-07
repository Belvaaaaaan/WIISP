/**
 * WIISP - filelist.c
 * Listado de carpetas para el menú de selección de archivos (ver filelist.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "frontend/filelist.h"

int filelist_is_supported(const char *name){
	static const char *const exts[] = { ".pbp", ".prx", ".elf", ".bin", ".iso", ".cso", ".zso" };
	const char *dot = strrchr(name, '.');
	size_t i;
	if(!dot) return 0;
	for(i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if(!strcasecmp(dot, exts[i])) return 1;
	return 0;
}

int filelist_is_root(const char *path){
	size_t len = strlen(path);
	return len == 0 || !strcmp(path, "/") || (len >= 2 && path[len - 1] == '/' && path[len - 2] == ':');
}

void filelist_parent(const char *path, char *out, size_t out_size){
	char tmp[FILELIST_PATH_LEN];
	char *slash;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	while(len > 1 && tmp[len - 1] == '/' && !filelist_is_root(tmp)) tmp[--len] = 0;
	if(filelist_is_root(tmp)){
		snprintf(out, out_size, "%s", tmp);
		return;
	}
	slash = strrchr(tmp, '/');
	if(!slash){
		snprintf(out, out_size, "%s", tmp);
		return;
	}
	/* Si el padre es la raíz, conserva la barra: "sd:/" */
	if(slash == tmp || slash[-1] == ':') slash[1] = 0;
	else *slash = 0;
	snprintf(out, out_size, "%s", tmp);
}

void filelist_join(const char *dir, const char *name, char *out, size_t out_size){
	size_t len = strlen(dir);
	if(len && dir[len - 1] == '/') snprintf(out, out_size, "%s%s", dir, name);
	else snprintf(out, out_size, "%s/%s", dir, name);
}

static int compare(const void *a, const void *b){
	const FileEntry *x = a, *y = b;
	if(!strcmp(x->name, "..")) return -1;
	if(!strcmp(y->name, "..")) return 1;
	if(x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
	return strcasecmp(x->name, y->name);
}

static void add(FileList *fl, const char *name, int is_dir){
	FileEntry *e;
	if(fl->count >= FILELIST_MAX_ENTRIES) return;
	e = &fl->entries[fl->count++];
	snprintf(e->name, sizeof(e->name), "%s", name);
	e->is_dir = is_dir;
}

int filelist_load(FileList *fl, const char *path){
	DIR *d;
	struct dirent *ent;

	filelist_free(fl);
	snprintf(fl->path, sizeof(fl->path), "%s", path);
	fl->entries = calloc(FILELIST_MAX_ENTRIES, sizeof(FileEntry));
	if(!fl->entries) return -1;
	if(!filelist_is_root(path)) add(fl, "..", 1);

	d = opendir(path);
	if(!d) return -1;
	while((ent = readdir(d)) != NULL){
		char full[FILELIST_PATH_LEN + FILELIST_NAME_LEN];
		struct stat st;
		int is_dir;
		if(ent->d_name[0] == '.') continue; /* ".", ".." y ocultos */
		if(strlen(ent->d_name) >= FILELIST_NAME_LEN) continue;
		filelist_join(path, ent->d_name, full, sizeof(full));
		is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
		if(is_dir || filelist_is_supported(ent->d_name)) add(fl, ent->d_name, is_dir);
	}
	closedir(d);
	qsort(fl->entries, (size_t)fl->count, sizeof(FileEntry), compare);
	return 0;
}

void filelist_free(FileList *fl){
	free(fl->entries);
	fl->entries = NULL;
	fl->count = 0;
}
