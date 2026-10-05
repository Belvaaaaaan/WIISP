/**
 * WIISP - pbp.c
 * Contenedor EBOOT.PBP (ver pbp.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include "loader/pbp.h"
#include "loader/loader.h"

#define PBP_HEADER_SIZE 40

static const char *const section_names[PBP_NUM_SECTIONS] = {
	"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG",
	"PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR"
};

const char *pbp_section_name(int section){
	if(section < 0 || section >= PBP_NUM_SECTIONS) return "?";
	return section_names[section];
}

int pbp_is_pbp(const u8 *buf, u32 len){
	return len >= 4 && buf[0] == 0 && buf[1] == 'P' && buf[2] == 'B' && buf[3] == 'P';
}

int pbp_parse(const u8 *buf, u32 len, PbpFile *out){
	u32 offsets[PBP_NUM_SECTIONS + 1];
	int i;

	memset(out, 0, sizeof(*out));
	if(len < PBP_HEADER_SIZE || !pbp_is_pbp(buf, len)) return LOADER_ERR_FORMAT;

	out->version = rd_le32(buf + 4);
	for(i = 0; i < PBP_NUM_SECTIONS; i++)
		offsets[i] = rd_le32(buf + 8 + i*4);
	offsets[PBP_NUM_SECTIONS] = len;

	for(i = 0; i < PBP_NUM_SECTIONS; i++){
		u32 start = offsets[i], end = offsets[i+1];
		if(start < PBP_HEADER_SIZE || start > len || end > len || end < start)
			return LOADER_ERR_CORRUPT;
		out->data[i] = start < end ? buf + start : NULL;
		out->size[i] = end - start;
	}
	return LOADER_OK;
}
