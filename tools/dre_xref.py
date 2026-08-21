#!/usr/bin/env python
"""Find all RIP-relative references to a guest VA in the eboot code (raw scan)."""
import os, sys, struct
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
BASE = 0x810000000
sys.path.insert(0, os.path.join(REPO, ".work"))
from disasm import reconstruct
import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

elf, phdrs = reconstruct(EBOOT)
target = int(sys.argv[1], 0)

# build vaddr->(off) map for executable segments
exec_segs = [(p[2], p[3], p[4], p[1]) for p in phdrs if p[0] == 1]

hits = []
for off0, va0, fsz, fl in exec_segs:
    code = elf[off0:off0 + fsz]
    for ins in md.disasm(code, BASE + va0):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM:
                if op.mem.base == capstone.x86.X86_REG_RIP:
                    disp = op.mem.disp
                    eff = ins.address + ins.size + disp
                    if target - 8 <= eff <= target + 8:
                        hits.append((ins.address, ins.mnemonic, ins.op_str, eff))

for a, m, o, eff in hits[:60]:
    print("0x%x (+0x%x -> 0x%x): %s %s" % (a, eff - a, eff, m, o))
if not hits:
    print("no RIP-relative refs to 0x%x" % target)
