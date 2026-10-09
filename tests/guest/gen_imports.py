#!/usr/bin/env python3
"""
WIISP - tests/guest/gen_imports.py
Genera el sceModuleInfo y las tablas de imports (formato del PSPSDK) de los
programas de prueba de tests/guest, a partir de imports.txt. Los NID se
toman de src/hle/nid_names.c.

    python3 gen_imports.py imports.txt nid_names.c > imports.S

SPDX-License-Identifier: GPL-2.0-or-later
"""
import re, sys

nids = {}
for m in re.finditer(r'\{ 0x([0-9A-F]{8}), "([^"]+)" \}', open(sys.argv[2]).read()):
    nids.setdefault(m.group(2), int(m.group(1), 16))

libs = {}
for line in open(sys.argv[1]):
    line = line.split('#')[0].split()
    if len(line) == 2:
        libs.setdefault(line[0], []).append(line[1])

out = ['\t.set noreorder', '',
       '\t.section .rodata.sceModuleInfo,"a",@progbits',
       '\t.align 4', '\t.global module_info', 'module_info:',
       '\t.hword 0, 0x0101',
       '\t.ascii "WIISP guest"', '\t.space 28 - 11',
       '\t.word 0', '\t.word 0, 0', '\t.word __lib_stub_top, __lib_stub_bottom', '']
for i, (lib, funcs) in enumerate(libs.items()):
    out += ['\t.section .rodata.libname,"a",@progbits', 'libname_%d:' % i, '\t.asciz "%s"' % lib, '\t.align 2',
            '\t.section .lib.stub,"a",@progbits',
            '\t.word libname_%d' % i, '\t.hword 0x0011, 0x4001', '\t.byte 5, 0',
            '\t.hword %d' % len(funcs), '\t.word nids_%d' % i, '\t.word stubs_%d' % i,
            '\t.section .rodata.sceNid,"a",@progbits', 'nids_%d:' % i]
    out += ['\t.word 0x%08X\t# %s' % (nids[f], f) for f in funcs]
    out += ['\t.section .sceStub.text,"ax",@progbits', 'stubs_%d:' % i]
    for f in funcs:
        out += ['\t.global %s' % f, '%s:' % f, '\tjr $ra', '\tnop']
    out.append('')
print('\n'.join(out))
