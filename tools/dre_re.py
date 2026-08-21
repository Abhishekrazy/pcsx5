#!/usr/bin/env python
"""
dre_re.py - static reverse-engineering of the Dreaming Sarah (PPSA02929) eboot.

Focus: find WHY the Construct runtime's binary frame-record serializer emits
"image" (5 bytes) instead of "images/precious_stones-sheet0.png" (33 bytes).

Tools in this file:
  - callgraph(gva, depth):  BFS the call graph from a guest address, showing
    every callee (so we can map which helper the serializer calls).
  - xrefs(s):               find code that references a guest address/string.
  - find_strings():         list printable ASCII strings in the eboot .text/.rodata.
  - trace_flow(gva):        linear-flow disasm of a function body.

All read-only (no build/run).  Uses capstone + the SELF->ELF reconstruction in
work/disasm.py.
"""
import os, re, sys, struct

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
BASE = 0x810000000

import capstone  # noqa: E402
sys.path.insert(0, os.path.join(REPO, ".work"))  # noqa: E402
from disasm import reconstruct  # noqa: E402

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def load():
    elf, phdrs = reconstruct(EBOOT)
    return elf, phdrs


def gva_to_off(gva, elf, phdrs):
    va = gva - BASE
    for p in phdrs:
        if p[0] == 1 and p[3] <= va < p[3] + p[4]:
            return p[2] + (va - p[3])
    return None


def off_to_gva(off, phdrs):
    for p in phdrs:
        if p[0] == 1 and p[2] <= off < p[2] + p[4]:
            return BASE + p[3] + (off - p[2])
    return None


def disasm_bytes(elf, off, gva, nbytes):
    code = elf[off:off + nbytes]
    insns = []
    for ins in md.disasm(code, gva):
        insns.append(ins)
    return insns


def call_target(ins, next_ins):
    """Resolve a direct call target from a capstone insn."""
    if ins.mnemonic == "call":
        op = ins.op_str.strip()
        m = re.match(r"^(0x[0-9a-f]+)$", op)
        if m:
            return int(m.group(1), 0)
    return None


def callgraph(gva, depth=3, max_nodes=120):
    elf, phdrs = load()
    seen = set()
    q = [(gva, 0)]
    order = []
    while q and len(seen) < max_nodes:
        cur, d = q.pop(0)
        if cur in seen or d > depth:
            continue
        seen.add(cur)
        order.append((cur, d))
        off = gva_to_off(cur, elf, phdrs)
        if off is None:
            continue
        insns = disasm_bytes(elf, off, cur, 0x400)
        for i, ins in enumerate(insns):
            tgt = call_target(ins, None)
            if tgt is not None and tgt not in seen:
                q.append((tgt, d + 1))
    for cur, d in order:
        print(("  " * d) + ("0x%x" % cur))


def xrefs(target_gva):
    """Scan all code for RIP-relative or immediate references to a target."""
    elf, phdrs = load()
    texts = [p for p in phdrs if p[0] == 1 and (p[1] & 4)]  # executable LOAD segs
    hits = []
    for p in texts:
        off, va, fsz = p[2], p[3], p[4]
        code = elf[off:off + fsz]
        for ins in md.disasm(code, BASE + va):
            # RIP-relative effective address
            if ins.mnemonic.startswith("lea") or ins.mnemonic.startswith("mov")                or ins.mnemonic.startswith("call") or ins.mnemonic.startswith("jmp"):
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        addr = ins.address + ins.size + op.mem.disp
                        if target_gva - 0x1000 <= addr <= target_gva + 0x1000:
                            hits.append((ins.address, ins.mnemonic, ins.op_str))
    for h in hits[:40]:
        print("0x%x: %s %s" % h)


def find_strings(minlen=6):
    """Extract printable ASCII strings from the eboot image."""
    elf, phdrs = load()
    texts = [p for p in phdrs if p[0] == 1]
    found = set()
    for p in texts:
        off, va, fsz = p[2], p[3], p[4]
        blob = elf[off:off + fsz]
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, blob):
            s = m.group().decode()
            if "image" in s.lower() or "precious" in s or "parse" in s or "frame" in s:
                found.add((BASE + va + m.start(), s))
    for addr, s in sorted(found):
        print("0x%x: %s" % (addr, s))


def trace_flow(gva, count=80):
    elf, phdrs = load()
    off = gva_to_off(gva, elf, phdrs)
    if off is None:
        print("0x%x not in LOAD" % gva); return
    insns = disasm_bytes(elf, off, gva, count * 20)
    for ins in insns[:count]:
        print("  0x%09x: %s\t%s" % (ins.address, ins.mnemonic, ins.op_str))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    c = a[0]
    if c == "callgraph":
        callgraph(int(a[1], 0), int(a[2]) if len(a) > 2 else 3)
    elif c == "xrefs":
        xrefs(int(a[1], 0))
    elif c == "strings":
        find_strings(int(a[1]) if len(a) > 1 else 6)
    elif c == "flow":
        trace_flow(int(a[1], 0), int(a[2], 0) if len(a) > 2 else 80)


if __name__ == "__main__":
    main()
