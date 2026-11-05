/**
 * WIISP - tools/banco/mezcla_exhaustiva.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
//Arreglo
#include <stdio.h>
#include <stdlib.h>
#include "wii/gx_mezcla.h"

int main(void){
	GmPrograma p;
	long malos = 0, alfa_mal = 0, total = 0;
	int swsub, a, d, s, dd, sal[4];
	for(swsub = 0; swsub < 2; swsub++){
		long m = 0;
		gm_programa_doble(&p, swsub);
		for(d = -8; d <= 7; d++){
			uint8_t tram[4], copia[4] = { 0, 0, 0, 0 };
			gm_texel_tramado(d, swsub, tram);
			for(a = 0; a < 256; a++)
				for(s = 0; s < 256; s++)
					for(dd = 0; dd < 256; dd += 3){
						const int sv[3] = { s, s, s };
						int k;
						copia[0] = (uint8_t)dd; copia[1] = (uint8_t)(dd + 1 < 256 ? dd + 1 : 0); copia[2] = (uint8_t)(dd + 2 < 256 ? dd + 2 : 0);
						gm_ejecutar(&p, swsub, sv, a, copia, tram, sal);
						for(k = 0; k < 3; k++){
							total++;
							if(sal[k] != gm_psp_doble(s, copia[k], a, d)){
								if(m < 5) printf("swsub %d: alfa %d tramado %d S %d D %d: TEV %d, PSP %d\n", swsub, a, d, s, copia[k], sal[k],
								                 gm_psp_doble(s, copia[k], a, d));
								m++;
							}
						}
						if(sal[3] != a) alfa_mal++;
					}
		}
		printf("doble, swsub %d: %ld distintos\n", swsub, m);
		malos += m;
	}
	gm_programa_abs(&p);
	{
		long m = 0;
		uint8_t tram[4] = { 0, 0, 0, 0 };
		for(s = 0; s < 256; s++)
			for(dd = 0; dd < 256; dd++){
				const int sv[3] = { s, 255 - s, s ^ 0x5A };
				uint8_t copia[4] = { (uint8_t)dd, (uint8_t)dd, (uint8_t)(255 - dd), 0 };
				int k;
				gm_ejecutar(&p, 0, sv, 77, copia, tram, sal);
				for(k = 0; k < 3; k++){ total++; if(sal[k] != abs(sv[k] - copia[k])) m++; }
				if(sal[3] != 77) alfa_mal++;
			}
		printf("diferencia absoluta: %ld distintos\n", m);
		malos += m;
	}
	printf("%ld comparaciones, %ld distintas, alfa de salida distinto %ld\n", total, malos, alfa_mal);
	return malos || alfa_mal;
}
