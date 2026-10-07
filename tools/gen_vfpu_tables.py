#!/usr/bin/env python3
"""
WIISP - gen_vfpu_tables.py
Genera src/cpu/vfpu_tables.h con las tablas de segmentos con que la VFPU
calcula rcp, rsqrt, sqrt, exp2, log2, sin y asin, extraídas de PPSSPP
(Core/MIPS/MIPSVFPUUtils.cpp, (c) 2012- PPSSPP Project, GPLv2+). Los
coeficientes se ajustaron para reproducir el hardware bit a bit.

    python3 tools/gen_vfpu_tables.py <ruta a ppsspp> > src/cpu/vfpu_tables.h

SPDX-License-Identifier: GPL-2.0-or-later
"""
import pathlib, re, sys

src = (pathlib.Path(sys.argv[1]) / 'Core' / 'MIPS' / 'MIPSVFPUUtils.cpp').read_text()
pat = re.compile(r'static\s+(?:constexpr|const)\s+VFPUSegment\s+vfpu_(\w+)_segments\[128\]\s*=\s*\{(.*?)\};', re.S)
ent = re.compile(r'\{\s*(-?0x[0-9A-Fa-f]+|-?\d+)\s*,\s*(-?0x[0-9A-Fa-f]+|-?\d+)\s*,\s*(-?0x[0-9A-Fa-f]+|-?\d+)\s*,\s*(-?0x[0-9A-Fa-f]+|-?\d+)\s*\}')
out = sys.stdout
out.write('/**\n * WIISP - vfpu_tables.h\n'
          ' * GENERADO por tools/gen_vfpu_tables.py a partir de PPSSPP\n'
          ' * (Core/MIPS/MIPSVFPUUtils.cpp, (c) 2012- PPSSPP Project, GPLv2+).\n'
          ' * No editar a mano.\n *\n'
          ' * Cada segmento: c0 (valor en x2 = 0), m (pendiente), n (término\n'
          ' * cuadrático) y e (binade en cuyas ulps trabaja).\n'
          ' *\n * SPDX-License-Identifier: GPL-2.0-or-later\n**/\n\n'
          '#ifndef WIISP_VFPU_TABLES_H\n#define WIISP_VFPU_TABLES_H\n\n'
          'typedef struct { s32 c0; s32 m; s16 n; s16 e; } VfpuSegment;\n')
found = 0
for name, body in pat.findall(src):
    rows = ent.findall(body)
    if len(rows) != 128:
        sys.exit('tabla %s: %d entradas' % (name, len(rows)))
    out.write('\nstatic const VfpuSegment vfpu_%s_segments[128] = {\n' % name)
    for i in range(0, 128, 2):
        out.write('\t' + ' '.join('{ %s, %s, %s, %s },' % r for r in rows[i:i + 2]) + '\n')
    out.write('};\n')
    found += 1
if found != 7:
    sys.exit('se esperaban 7 tablas, hay %d' % found)
out.write('\n#endif\n')
