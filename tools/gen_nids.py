#!/usr/bin/env python3
"""
WIISP - gen_nids.py
Genera src/hle/nid_names.c (NID -> nombre de función) a partir de los
archivos de stubs del PSPSDK (https://github.com/pspdev/pspsdk, licencia BSD).

    python3 tools/gen_nids.py <ruta a pspsdk> > src/hle/nid_names.c

SPDX-License-Identifier: GPL-2.0-or-later
"""
import pathlib, re, sys

root = pathlib.Path(sys.argv[1])
pat = re.compile(r'IMPORT_FUNC\s+"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)')
names = {}
for path in sorted(root.rglob('*.S')):
    for lib, nid, name in pat.findall(path.read_text(errors='replace')):
        names.setdefault(int(nid, 16), name)

# NIDs de firmwares posteriores que no están en los stubs del PSPSDK
# (nombres según la tabla HLE de PPSSPP y uOFW).
EXTRA = {
    0x342061E5: 'sceKernelSetCompiledSdkVersion370',
    0x315AD3A0: 'sceKernelSetCompiledSdkVersion380_390',
    0xEBD5C3E6: 'sceKernelSetCompiledSdkVersion395',
    0x057E7380: 'sceKernelSetCompiledSdkVersion401_402',
    0xF77D77CB: 'sceKernelSetCompilerVersion',
    0x91DE343C: 'sceKernelSetCompiledSdkVersion500_505',
    0x7893F79A: 'sceKernelSetCompiledSdkVersion507',
    0x35669D4C: 'sceKernelSetCompiledSdkVersion600_602',
    0x1B4217BC: 'sceKernelSetCompiledSdkVersion603_605',
    0x358CA1BB: 'sceKernelSetCompiledSdkVersion606',
    0xACBD88CA: 'sceKernelTotalMemSize',
    0xA6848DF8: 'sceKernelSetUsersystemLibWork',
    0x6231A71D: 'sceKernelSetPTRIG',
    0x39F49610: 'sceKernelGetPTRIG',
    0xDB83A952: 'sceKernelGetMemoryBlockAddr',
    0x50F61D8A: 'sceKernelFreeMemoryBlock',
    0xFE707FDF: 'sceKernelAllocMemoryBlock',
}
for nid, name in EXTRA.items():
    names.setdefault(nid, name)

out = sys.stdout
out.write('/**\n * WIISP - nid_names.c\n'
          ' * GENERADO por tools/gen_nids.py a partir de los stubs del PSPSDK\n'
          ' * (pspdev/pspsdk, (c) 2005 adresd, Marcus R. Brown, James Forshaw,\n'
          ' * John Kelley, Jesper Svennevid y otros; licencia BSD). No editar a mano.\n'
          ' *\n * SPDX-License-Identifier: GPL-2.0-or-later\n**/\n\n'
          '#include "hle/nid_names.h"\n\n'
          'const NidName nid_names[] = {\n')
for nid in sorted(names):
    out.write('\t{ 0x%08X, "%s" },\n' % (nid, names[nid]))
out.write('};\n\nconst u32 nid_names_count = %d;\n' % len(names))
