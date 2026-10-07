#!/usr/bin/env python3
"""
WIISP - gen_vfpu_bench.py
Genera ejecutables de PSP (ELF) para medir la VFPU del emulador:

    python3 tools/gen_vfpu_bench.py mix    vfpu_mix.elf    [iteraciones]
    python3 tools/gen_vfpu_bench.py thread vfpu_thread.elf [iteraciones]

mix:    un bucle con la matemática típica de un juego 3D: cargar matrices,
        vmmul, vtfm, normalizar (vdot + vrsq + vscl), sumas, conversiones,
        vrot, vmin/vmax, vcmp + bvf, un prefijo y sv.q.
op:     una sola instrucción repetida 16 veces por vuelta (para medir su
        coste): int, lvq, svq, vadd, vmul, vdot, vscl, vmmul, vtfm, vmov,
        vrsq, vsin, vf2i, vcmp, vmin, mfv, mtv, pfx (vpfxs + vmov).
        python3 tools/gen_vfpu_bench.py op vadd vadd.elf [iteraciones]
thread: dos hilos de la misma prioridad que hacen un poco de VFPU y se ceden
        el turno (sceKernelRotateThreadReadyQueue) en cada vuelta: mide el
        coste de los cambios de hilo.

Al terminar llaman a sceKernelExitGame; el CLI y el modo de pruebas del Wii
dan los MIPS y el tiempo. No necesita el SDK de la PSP: lleva su propio mini
ensamblador (las instrucciones VFPU se codifican como en PPSSPP).

SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct, sys

BASE = 0x08804000
SEG = 0x2000                       # tamaño del segmento
CODE, MODINFO, STUBENT, LIBNAMES, NIDS, STUBS, STR, DATA = \
    0x0000, 0x1000, 0x1040, 0x1100, 0x1180, 0x1200, 0x1300, 0x1400

# registros MIPS
ZERO, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3 = range(12)
S0, S1, S2, S3 = 16, 17, 18, 19
SP, RA = 29, 31

# --- registros VFPU (nombres como en el ensamblador de la PSP) ---------------
def S(m, c, r): return (r << 5) | (m << 2) | c                     # S m c r
def C(m, c, r=0): return (m << 2) | c | (0x40 if r else 0)         # columna
def R(m, r, c=0): return (m << 2) | r | 0x20 | (0x40 if c else 0)  # fila
def M(m): return m << 2                                            # matriz

SZ_S, SZ_P, SZ_T, SZ_Q = 0, 1 << 7, 1 << 15, (1 << 15) | (1 << 7)

class Asm:
    def __init__(self):
        self.w = []
        self.labels = {}
        self.fix = []
    def pc(self): return BASE + CODE + 4 * len(self.w)
    def emit(self, x): self.w.append(x & 0xFFFFFFFF)
    def label(self, n): self.labels[n] = self.pc()
    # enteros
    def addiu(self, rt, rs, imm): self.emit(0x24000000 | rs << 21 | rt << 16 | (imm & 0xFFFF))
    def lui(self, rt, imm): self.emit(0x3C000000 | rt << 16 | (imm & 0xFFFF))
    def li(self, rt, v):
        self.lui(rt, ((v + 0x8000) >> 16) & 0xFFFF)
        self.addiu(rt, rt, v & 0xFFFF)
    def move(self, rd, rs): self.emit(rs << 21 | rd << 11 | 0x21)
    def sw(self, rt, off, rs): self.emit(0xAC000000 | rs << 21 | rt << 16 | (off & 0xFFFF))
    def lw(self, rt, off, rs): self.emit(0x8C000000 | rs << 21 | rt << 16 | (off & 0xFFFF))
    def nop(self): self.emit(0)
    def jal(self, target): self.emit(0x0C000000 | ((target >> 2) & 0x03FFFFFF))
    def jr_ra(self): self.emit(0x03E00008)
    def _br(self, base, label): self.fix.append((len(self.w), label)); self.emit(base)
    def bne(self, rs, rt, label): self._br(0x14000000 | rs << 21 | rt << 16, label)
    def bvf(self, cc, label): self._br(0x49000000 | cc << 18, label)
    # VFPU
    def lvq(self, vt, off, rs): self.emit(0xD8000000 | rs << 21 | (vt & 31) << 16 | (off & 0xFFFC) | (vt >> 5) & 1)
    def svq(self, vt, off, rs): self.emit(0xF8000000 | rs << 21 | (vt & 31) << 16 | (off & 0xFFFC) | (vt >> 5) & 1)
    def v3(self, base, sz, vd, vs, vt): self.emit(base | sz | vt << 16 | vs << 8 | vd)
    def v2(self, base, sz, vd, vs): self.emit(base | sz | vs << 8 | vd)
    def vpfxs(self, data): self.emit(0xDC000000 | data)
    def mfv(self, rt, vr): self.emit(0x48600000 | rt << 16 | vr)
    def mtv(self, rt, vr): self.emit(0x48E00000 | rt << 16 | vr)
    def finish(self):
        for i, lab in self.fix:
            off = (self.labels[lab] - (BASE + CODE + 4 * i + 4)) >> 2
            self.w[i] |= off & 0xFFFF
        return b''.join(struct.pack('<I', x) for x in self.w)

VMMUL, VTFM4, VDOT, VSCL, VADD, VSUB, VMUL = 0xF0000000, 0xF1800000, 0x64800000, 0x65000000, 0x60000000, 0x60800000, 0x64000000
VMOV, VRSQ, VF2IZ, VI2F, VROT, VMIN, VMAX, VCMP = 0xD0000000, 0xD0110000, 0xD2200000, 0xD2800000, 0xF3A00000, 0x6D000000, 0x6D800000, 0x6C000000
VMIDT = 0xF3830000

IMPORTS = [   # (biblioteca, NID, nombre)
    ('LoadExecForUser', 0x05572A5F, 'sceKernelExitGame'),
    ('ThreadManForUser', 0x446D8DE6, 'sceKernelCreateThread'),
    ('ThreadManForUser', 0xF475845D, 'sceKernelStartThread'),
    ('ThreadManForUser', 0x278C0DF5, 'sceKernelWaitThreadEnd'),
    ('ThreadManForUser', 0x912354A7, 'sceKernelRotateThreadReadyQueue'),
]
def stub(name):
    return BASE + STUBS + 8 * [i[2] for i in IMPORTS].index(name)

def vfpu_body(a, base_reg, out_off):
    """Una vuelta de matemática de juego; base_reg apunta a DATA."""
    for i in range(4):
        a.lvq(C(0, i), 16 * i, base_reg)          # matriz de mundo
        a.lvq(C(1, i), 64 + 16 * i, base_reg)     # matriz de vista
    a.v3(VMMUL, SZ_Q, M(2), M(0), M(1))           # vmmul.q M200, M000, M100
    a.v3(VTFM4, SZ_Q, C(3, 0), M(2), C(4, 0))     # vtfm4.q C300, M200, C400
    a.v3(VDOT, SZ_Q, S(3, 1, 0), C(3, 0), C(3, 0))   # vdot.q S310, C300, C300
    a.v2(VRSQ, SZ_S, S(3, 1, 1), S(3, 1, 0))      # vrsq.s S311, S310
    a.v3(VSCL, SZ_Q, C(3, 2), C(3, 0), S(3, 1, 1))   # vscl.q C320, C300, S311
    a.v3(VADD, SZ_Q, C(3, 3), C(3, 2), C(4, 1))   # vadd.q C330, C320, C410
    a.v3(VSUB, SZ_T, C(5, 0), C(3, 3), C(4, 1))   # vsub.t C500, C330, C410
    a.v3(VMUL, SZ_Q, C(5, 1), C(3, 3), C(4, 2))   # vmul.q C510, C330, C420
    a.vpfxs(0 | 1 << 2 | 2 << 4 | 1 << 6 | 1 << 15)  # [x, y, z, 1]
    a.v2(VMOV, SZ_Q, C(5, 2), C(3, 3))            # vmov.q C520, C330[x,y,z,1]
    a.v2(VF2IZ | 16 << 16, SZ_Q, C(6, 0), C(5, 1))   # vf2iz.q C600, C510, 1/65536
    a.v2(VI2F | 16 << 16, SZ_Q, C(6, 1), C(6, 0))    # vi2f.q C610, C600, 1/65536
    a.v2(VROT | 4 << 16, SZ_P, C(6, 2), S(4, 3, 0))  # vrot.p C620, S430, [c, s]
    a.v3(VMIN, SZ_Q, C(6, 3), C(6, 1), C(5, 2))   # vmin.q C630, C610, C520
    a.v3(VMAX, SZ_Q, C(7, 0), C(6, 1), C(5, 2))   # vmax.q C700, C610, C520
    a.v3(VCMP | 7, SZ_Q, 0, C(7, 0), C(6, 3))     # vcmp.q GT, C700, C630
    a.bvf(5, 'skip%d' % len(a.w))                 # bvf 5 (all) -> salta el vmov
    lab = 'skip%d' % (len(a.w) - 1)
    a.nop()
    a.v2(VMOV, SZ_Q, C(7, 1), C(7, 0))
    a.label(lab)
    a.svq(C(3, 3), out_off, base_reg)
    a.svq(C(5, 2), out_off + 16, base_reg)
    a.mfv(T1, S(3, 1, 0))
    a.mtv(T1, S(7, 2, 0))

SINGLE = {
    'int':   lambda a: a.emit(T2 << 21 | T3 << 16 | T4 << 11 | 0x21),          # addu t4, t2, t3
    'lvq':   lambda a: a.lvq(C(0, 1), 16, S1),
    'svq':   lambda a: a.svq(C(3, 3), 256, S1),
    'vadd':  lambda a: a.v3(VADD, SZ_Q, C(3, 3), C(4, 0), C(4, 1)),
    'vmul':  lambda a: a.v3(VMUL, SZ_Q, C(3, 3), C(4, 0), C(4, 1)),
    'vdot':  lambda a: a.v3(VDOT, SZ_Q, S(3, 1, 0), C(4, 0), C(4, 1)),
    'vscl':  lambda a: a.v3(VSCL, SZ_Q, C(3, 2), C(4, 0), S(4, 1, 0)),
    'vmmul': lambda a: a.v3(VMMUL, SZ_Q, M(2), M(0), M(1)),
    'vtfm':  lambda a: a.v3(VTFM4, SZ_Q, C(3, 0), M(2), C(4, 0)),
    'vmov':  lambda a: a.v2(VMOV, SZ_Q, C(3, 2), C(4, 0)),
    'vrsq':  lambda a: a.v2(VRSQ, SZ_S, S(3, 1, 1), S(4, 2, 0)),
    'vsin':  lambda a: a.v2(0xD0120000, SZ_S, S(3, 1, 1), S(4, 3, 0)),
    'vf2i':  lambda a: a.v2(VF2IZ | 16 << 16, SZ_Q, C(6, 0), C(4, 0)),
    'vcmp':  lambda a: a.v3(VCMP | 7, SZ_Q, 0, C(4, 0), C(4, 1)),
    'vmin':  lambda a: a.v3(VMIN, SZ_Q, C(6, 3), C(4, 0), C(4, 1)),
    'mfv':   lambda a: a.mfv(T1, S(4, 0, 0)),
    'mtv':   lambda a: a.mtv(T1, S(7, 2, 0)),
    'pfx':   lambda a: (a.vpfxs(0 | 1 << 2 | 2 << 4 | 1 << 6 | 1 << 15), a.v2(VMOV, SZ_Q, C(5, 2), C(4, 0))),
}
T4 = 12

def build(mode, iters, single=None):
    a = Asm()
    # main: guarda ra (por si el kernel vuelve) y apunta s1 a los datos
    a.addiu(SP, SP, -32)
    a.sw(RA, 0, SP)
    a.li(S1, BASE + DATA)
    a.lvq(C(4, 0), 128, S1)   # vector de vértice
    a.lvq(C(4, 1), 144, S1)
    a.lvq(C(4, 2), 160, S1)
    a.lvq(C(4, 3), 176, S1)
    if mode in ('mix', 'op'):
        for i in range(4):   # matrices M000 y M100 para vmmul/vtfm
            a.lvq(C(0, i), 16 * i, S1)
            a.lvq(C(1, i), 64 + 16 * i, S1)
            a.lvq(C(2, i), 16 * i, S1)
        a.li(S0, iters)
        a.label('loop')
        if mode == 'mix':
            vfpu_body(a, S1, 256)
        else:
            for k in range(16 if single != 'pfx' else 8):
                SINGLE[single](a)
        a.addiu(S0, S0, -1)
        a.bne(S0, ZERO, 'loop')
        a.nop()
    else:
        # dos hilos trabajadores; main espera a que terminen
        for k in range(2):
            a.li(A0, BASE + STR)                # nombre
            a.li(A1, 0)                         # entry: se parchea abajo
            entry_fix = len(a.w) - 2
            a.li(A2, 0x20)                      # misma prioridad que main
            a.li(A3, 0x4000)                    # pila
            a.li(T0, 0x80004000)                # usuario + VFPU
            a.li(T1, 0)
            a.jal(stub('sceKernelCreateThread'))
            a.nop()
            a.sw(V0, 8 + 4 * k, SP)
            a.move(A0, V0)
            a.li(A1, 0)
            a.li(A2, 0)
            a.jal(stub('sceKernelStartThread'))
            a.nop()
            if k == 0: fix0 = entry_fix
            else: fix1 = entry_fix
        for k in range(2):
            a.lw(A0, 8 + 4 * k, SP)
            a.li(A1, 0)
            a.jal(stub('sceKernelWaitThreadEnd'))
            a.nop()
    a.jal(stub('sceKernelExitGame'))
    a.nop()
    a.lw(RA, 0, SP)
    a.jr_ra()
    a.addiu(SP, SP, 32)
    if mode == 'thread':
        entry = a.pc()
        for fx in (fix0, fix1):
            hi, lo = ((entry + 0x8000) >> 16) & 0xFFFF, entry & 0xFFFF
            a.w[fx] = 0x3C000000 | A1 << 16 | hi
            a.w[fx + 1] = 0x24000000 | A1 << 21 | A1 << 16 | lo
        # hilo: s0 vueltas de VFPU y cesión del turno (s2 guarda ra)
        a.move(S2, RA)
        a.li(S1, BASE + DATA)
        a.li(S0, iters)
        a.label('tloop')
        a.v3(VMMUL, SZ_Q, M(2), M(0), M(1))
        a.v3(VTFM4, SZ_Q, C(3, 0), M(2), C(4, 0))
        a.v3(VADD, SZ_Q, C(3, 3), C(3, 0), C(4, 1))
        a.li(A0, 0)
        a.jal(stub('sceKernelRotateThreadReadyQueue'))
        a.nop()
        a.addiu(S0, S0, -1)
        a.bne(S0, ZERO, 'tloop')
        a.nop()
        a.move(RA, S2)
        a.jr_ra()
        a.move(V0, ZERO)
    code = a.finish()
    assert len(code) <= MODINFO - CODE, 'código demasiado grande'

    seg = bytearray(SEG)
    seg[CODE:CODE + len(code)] = code
    # sceModuleInfo
    struct.pack_into('<HH', seg, MODINFO, 0, 0x0101)
    seg[MODINFO + 4:MODINFO + 4 + 9] = b'VfpuBench'
    struct.pack_into('<III', seg, MODINFO + 32, BASE + 0x8000, 0, 0)    # gp, ent
    libs = []
    for lib, nid, name in IMPORTS:
        if lib not in libs: libs.append(lib)
    struct.pack_into('<II', seg, MODINFO + 44, BASE + STUBENT, BASE + STUBENT + 20 * len(libs))
    name_off, nid_off = LIBNAMES, NIDS
    first = 0
    for li_, lib in enumerate(libs):
        idx = [i for i, imp in enumerate(IMPORTS) if imp[0] == lib]
        assert idx == list(range(idx[0], idx[0] + len(idx)))
        seg[name_off:name_off + len(lib)] = lib.encode()
        e = STUBENT + 20 * li_
        struct.pack_into('<IHHBBHII', seg, e, BASE + name_off, 0x0011, 0x4001, 5, 0, len(idx),
                         BASE + NIDS + 4 * idx[0], BASE + STUBS + 8 * idx[0])
        name_off += (len(lib) + 4) & ~3
    for i, (lib, nid, name) in enumerate(IMPORTS):
        struct.pack_into('<I', seg, NIDS + 4 * i, nid)
        struct.pack_into('<II', seg, STUBS + 8 * i, 0x03E00008, 0)
    seg[STR:STR + 7] = b'worker\0'
    # datos: dos matrices, vectores y ángulos
    def mat(scale, off):
        vals = [1.0 + scale * (i % 5) * 0.01 if i % 5 == 0 else scale * 0.01 * i for i in range(16)]
        struct.pack_into('<16f', seg, DATA + off, *vals)
    mat(1.0, 0)
    mat(0.5, 64)
    struct.pack_into('<16f', seg, DATA + 128, 1.5, -2.0, 3.25, 1.0, 0.25, 0.5, 0.75, 0.0,
                     2.0, 2.0, 2.0, 2.0, 0.1, 0.2, 0.3, 0.4)

    # ELF absoluto con un PT_LOAD (p_paddr = offset del sceModuleInfo)
    hdr = bytearray(52 + 32)
    hdr[0:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', hdr, 16, 2, 8, 1, BASE + CODE, 52, 0, 0x10A23001, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<IIIIIIII', hdr, 52, 1, 0x100, BASE, 0x100 + MODINFO, SEG, SEG, 7, 16)
    return bytes(hdr) + bytes(0x100 - len(hdr)) + bytes(seg)

if __name__ == '__main__':
    args = sys.argv[1:]
    mode = args.pop(0)
    single = args.pop(0) if mode == 'op' else None
    out = args.pop(0)
    iters = int(args[0]) if args else (20000 if mode == 'thread' else 200000)
    if mode not in ('mix', 'thread', 'op') or (mode == 'op' and single not in SINGLE):
        sys.exit('modo: mix, thread u op <%s>' % '|'.join(SINGLE))
    open(out, 'wb').write(build(mode, iters, single))
