#!/usr/bin/env python
"""
Resolve all RIP-relative lea string references in a guest VA range and print
the actual string content at each reference (to map the path-registration table).
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".work"))
import capstone
from disasm import reconstruct

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
BASE = 0x810000000

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
elf, phdrs = reconstruct(EBOOT)

def gva_to_off(gva):
    va = gva - BASE
    for p in phdrs:
        if p[0] == 1 and p[3] <= va < p[3] + p[4]:
            return p[2] + (va - p[3])
    return None

def cstr_at(gva, maxlen=40):
    off = gva_to_off(gva)
    if off is None:
        return None
    s = bytearray()
    for i in range(maxlen):
        b = elf[off + i]
        if b == 0:
            break
        if 32 <= b < 127:
            s.append(b)
        else:
            s.append(ord('?'))
    return s.decode()

def refs_in_range(start_gva, end_gva):
    """Return list of (insn_addr, target_gva, target_str) for RIP-relative lea within range."""
    # find the load segment(s) covering the range
    results = []
    va0 = start_gva - BASE
    va1 = end_gva - BASE
    for p in phdrs:
        if p[0] != 1:
            continue
        off, va, fsz = p[2], p[3], p[4]
        if not (va <= va1 and va + fsz >= va0):
            continue
        code = elf[off:off + fsz]
        for ins in md.disasm(code, BASE + va):
            if ins.address < start_gva or ins.address >= end_gva:
                continue
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    eff = ins.address + ins.size + op.mem.disp
                    s = cstr_at(eff)
                    results.append((ins.address, eff, s))
    return results

start = int(sys.argv[1], 0)
end = int(sys.argv[2], 0)
for a, t, s in refs_in_range(start, end):
    print("0x%x -> 0x%x (%+d): \"%s\"" % (a, t, t - a, s if s else "?"))
