#!/usr/bin/env python3
"""
WIISP - gen_prx_keys.py
Genera src/loader/prx_keys.h y src/loader/kirk_keys.h con las tablas de claves de PrxDecrypter.cpp
de PPSSPP (Core/ELF/PrxDecrypter.cpp, (c) PPSSPP Project, GPLv2+).

    python3 tools/gen_prx_keys.py PPSSPP/Core/ELF/PrxDecrypter.cpp PPSSPP/ext/libkirk/kirk_engine.c

Solo copia datos: las claves, semillas y tablas de etiquetas, y de
kirk_engine.c las claves del KIRK 0x40-0x6F que usan esas tablas (son
constantes del hardware, no código). La lógica está escrita a mano en
src/loader/kirk.c y src/loader/prx_decrypt.c.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import re, sys

src = open(sys.argv[1], encoding='utf-8').read().split('\n')
out = []
i = 0
skip_depth = None
while i < len(src):
    line = src[i]
    # Las estructuras y funciones de C++ se escriben en prx_decrypt.c
    if line.startswith('struct ') or line.startswith('static const TAG_INFO *') or line.startswith('static const TAG_INFO2 *'):
        while not src[i].startswith('}'):
            i += 1
        i += 1
        continue
    if line.startswith('static std::array') or line.startswith('template'):
        break
    out.append(line)
    i += 1

text = '\n'.join(out)
start = text.index('static const u8 xor_91E0A9AD')
text = text[start:]
# Cierre de la última tabla
end = text.rindex('};') + 2
text = text[:end]
text = text.replace(', true }', ', 1 }')

hdr = '''/**
 * WIISP - prx_keys.h (generado por tools/gen_prx_keys.py, no editar)
 * Claves y tablas de etiquetas para descifrar PRX y EBOOT.BIN de PSP.
 *
 * Datos de PPSSPP (Core/ELF/PrxDecrypter.cpp), (c) PPSSPP Project,
 * GPLv2 o posterior; recopilados por PSARDumper y JPCSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/

'''
kirk = open(sys.argv[2], encoding='utf-8').read()
vault = kirk[kirk.index('keyvault[0x80][0x10] = {'):]
rows = re.findall(r'\{((?:\s*0x[0-9A-Fa-f]{2},?){16})\s*\}', vault)[:0x80]
assert len(rows) == 0x80
kt = '\n/* Claves del KIRK para los comandos 4 y 7, desde la 0x40 */\n#define KIRK_KEY_FIRST 0x40\n#define KIRK_KEY_COUNT 0x30\nstatic const u8 kirk_keys[KIRK_KEY_COUNT][16] = {\n'
for r in rows[0x40:0x70]:
    kt += '\t{' + ', '.join(x.strip() for x in r.split(',') if x.strip()) + '},\n'
kt += '};\n'
k1 = re.search(r'kirk1_key\[\]\s*=\s*\{([^}]*)\}', kirk).group(1)
kt += '\n/* Clave del comando 1 (descifrado de bloques privados) */\nstatic const u8 kirk1_key[16] = {' + k1.strip() + '};\n'
open('src/loader/prx_keys.h', 'w').write(hdr + text + '\n')
khdr = hdr.replace('prx_keys.h', 'kirk_keys.h').replace(
    'Claves y tablas de etiquetas para descifrar PRX y EBOOT.BIN de PSP.\n *\n * Datos de PPSSPP (Core/ELF/PrxDecrypter.cpp), (c) PPSSPP Project,\n * GPLv2 o posterior; recopilados por PSARDumper y JPCSP.',
    'Claves del KIRK (constantes del hardware de la PSP), publicadas por la\n * escena de la PSP (Draan y otros, kirk_engine).')
open('src/loader/kirk_keys.h', 'w').write(khdr + kt.lstrip('\n'))
