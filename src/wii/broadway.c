/**
 * WIISP - broadway.c
 * Qué tiene configurado el CPU del Wii al arrancar, para wiisp.log.
 *
 * Sirve para decidir antes de diseñar dos optimizaciones de la hoja de ruta:
 *   - 2.5 (mem_bat): mapear la RAM de la PSP en 0x08000000 con dos BAT de
 *     datos. Hace falta saber qué pares DBAT deja libres libogc (Vs = Vp = 0
 *     en la mitad alta) y si los BAT 4-7 están activos (HID4[SBE]).
 *   - 1.2b (ge_vdec_psq): convertir vértices con psq_l. Hace falta saber
 *     qué GQR usa ya alguien (GQR0 debe seguir en 0 para el compilador).
 * También se apuntan SDR1 (tabla de páginas: 0 = ninguna) y las arenas de
 * MEM1 y MEM2.
 *
 * Solo lee registros: no cambia nada.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <stdio.h>
#include <gccore.h>
#include "wii/wii.h"
#include "hle/hle.h"

#define MFSPR(n) ({ u32 v_; __asm__ volatile("mfspr %0, %1" : "=r"(v_) : "i"(n)); v_; })

#ifndef HID4_SBE
#define HID4_SBE 0x02000000u   /* bit 6: BAT 4-7 activos */
#endif

/* Una pareja BAT: dirección efectiva, tamaño, física, WIMG y protección */
static void log_bat(const char *name, int i, u32 up, u32 lo, int *libre){
	u32 bl = (up >> 2) & 0x7FF, size_kb = (bl + 1) * 128;
	if(!(up & 3)){
		hle_log_quiet("[BROADWAY] %s%d %08X %08X  libre\n", name, i, up, lo);
		*libre = 1;
		return;
	}
	*libre = 0;
	hle_log_quiet("[BROADWAY] %s%d %08X %08X  %08X-%08X (%u %s) -> fisica %08X, WIMG %c%c%c%c, PP %u, %s%s\n",
	              name, i, up, lo, up & 0xFFFE0000u, (up & 0xFFFE0000u) + size_kb * 1024u - 1,
	              size_kb >= 1024 ? size_kb / 1024 : size_kb, size_kb >= 1024 ? "MB" : "KB", lo & 0xFFFE0000u,
	              lo & 0x40 ? 'W' : '-', lo & 0x20 ? 'I' : '-', lo & 0x10 ? 'M' : '-', lo & 0x08 ? 'G' : '-',
	              lo & 3, up & 2 ? "Vs" : "", up & 1 ? " Vp" : "");
}

void broadway_report(void){
	u32 hid4 = MFSPR(1011), msr;
	u32 dbat[8][2], ibat[8][2], gqr[8];
	int n = (hid4 & HID4_SBE) ? 8 : 4, i, libre;
	char dfree[64] = "", ifree[64] = "";
	size_t dl = 0, il = 0;

	__asm__ volatile("mfmsr %0" : "=r"(msr));
	dbat[0][0] = MFSPR(536); dbat[0][1] = MFSPR(537); dbat[1][0] = MFSPR(538); dbat[1][1] = MFSPR(539);
	dbat[2][0] = MFSPR(540); dbat[2][1] = MFSPR(541); dbat[3][0] = MFSPR(542); dbat[3][1] = MFSPR(543);
	ibat[0][0] = MFSPR(528); ibat[0][1] = MFSPR(529); ibat[1][0] = MFSPR(530); ibat[1][1] = MFSPR(531);
	ibat[2][0] = MFSPR(532); ibat[2][1] = MFSPR(533); ibat[3][0] = MFSPR(534); ibat[3][1] = MFSPR(535);
	if(n == 8){
		/* Solo se pueden leer con HID4[SBE] activo */
		dbat[4][0] = MFSPR(568); dbat[4][1] = MFSPR(569); dbat[5][0] = MFSPR(570); dbat[5][1] = MFSPR(571);
		dbat[6][0] = MFSPR(572); dbat[6][1] = MFSPR(573); dbat[7][0] = MFSPR(574); dbat[7][1] = MFSPR(575);
		ibat[4][0] = MFSPR(560); ibat[4][1] = MFSPR(561); ibat[5][0] = MFSPR(562); ibat[5][1] = MFSPR(563);
		ibat[6][0] = MFSPR(564); ibat[6][1] = MFSPR(565); ibat[7][0] = MFSPR(566); ibat[7][1] = MFSPR(567);
	}
	gqr[0] = MFSPR(912); gqr[1] = MFSPR(913); gqr[2] = MFSPR(914); gqr[3] = MFSPR(915);
	gqr[4] = MFSPR(916); gqr[5] = MFSPR(917); gqr[6] = MFSPR(918); gqr[7] = MFSPR(919);

	hle_log_quiet("[BROADWAY] PVR %08X MSR %08X HID0 %08X HID1 %08X HID2 %08X HID4 %08X (BAT 4-7 %s) L2CR %08X SDR1 %08X%s\n",
	              MFSPR(287), msr, MFSPR(1008), MFSPR(1009), MFSPR(920), hid4, n == 8 ? "activos" : "apagados",
	              MFSPR(1017), MFSPR(25), MFSPR(25) ? "" : " (sin tabla de paginas)");
	for(i = 0; i < n; i++){
		log_bat("DBAT", i, dbat[i][0], dbat[i][1], &libre);
		if(libre && dl + 4 < sizeof(dfree)) dl += (size_t)snprintf(dfree + dl, sizeof(dfree) - dl, " %d", i);
	}
	for(i = 0; i < n; i++){
		log_bat("IBAT", i, ibat[i][0], ibat[i][1], &libre);
		if(libre && il + 4 < sizeof(ifree)) il += (size_t)snprintf(ifree + il, sizeof(ifree) - il, " %d", i);
	}
	hle_log_quiet("[BROADWAY] BAT libres: DBAT%s; IBAT%s\n", dl ? dfree : " ninguno", il ? ifree : " ninguno");
	hle_log_quiet("[BROADWAY] GQR0-7: %08X %08X %08X %08X %08X %08X %08X %08X%s\n",
	              gqr[0], gqr[1], gqr[2], gqr[3], gqr[4], gqr[5], gqr[6], gqr[7],
	              gqr[0] ? " (GQR0 no es 0!)" : "");
	hle_log_quiet("[BROADWAY] MEM1 libre %08X-%08X, MEM2 libre %08X-%08X\n",
	              (u32)SYS_GetArena1Lo(), (u32)SYS_GetArena1Hi(), (u32)SYS_GetArena2Lo(), (u32)SYS_GetArena2Hi());
	hle_log_sync();
}
