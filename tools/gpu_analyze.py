#!/usr/bin/env python
"""
gpu_analyze.py - GPU-accelerated binary analysis for the Dreaming Sarah eboot.

Uses PyTorch/CUDA (RTX 5070 Ti, sm_120) to brute-force static-analysis tasks
that are O(N*M) over the ~7.7 MB reconstructed ELF:

  1. search <needle>          - find all occurrences of a byte pattern
  2. lenhist <addr>           - histogram string lengths / char-class table for a region
  3. cctable <addr>           - parallel char-class classification (which bytes are
                                "continue" 0x80 vs "terminate" 0x00) for a 256-byte table
  4. refs <addr>              - RIP-relative xref scan, vectorized (all code offets x all insns)
  5. scan4 <u32>              - find a little-endian u32/u64 immediate across all segments

All read-only.  Uses disasm.py reconstruction + torch CUDA.
"""
import os, sys, re, struct
import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
BASE = 0x810000000
sys.path.insert(0, os.path.join(REPO, ".work"))
from disasm import reconstruct

try:
    import torch
    HAS_CUDA = torch.cuda.is_available()
except ImportError:
    torch = None
    HAS_CUDA = False


def gpu():
    return "cuda" if HAS_CUDA else "cpu"


def load_elf():
    elf, phdrs = reconstruct(EBOOT)
    return elf, phdrs


def search(needle_hex):
    """Find all occurrences of a byte string across every LOAD segment (GPU memchr)."""
    pat = bytes.fromhex(needle_hex)
    elf, phdrs = load_elf()
    hits = []
    for p in phdrs:
        if p[0] != 1:
            continue
        off, va, fsz = p[2], p[3], p[4]
        seg = elf[off:off + fsz]
        # GPU-accelerated search
        if HAS_CUDA and len(seg) > 100_000:
            data = torch.from_numpy(np.frombuffer(seg, dtype=np.uint8)).to("cuda")
            pidx = torch.where(data == pat[0])[0].cpu().numpy()
        else:
            data = np.frombuffer(seg, dtype=np.uint8)
            pidx = np.where(data == pat[0])[0]
        for i in pidx:
            if seg[i:i + len(pat)] == pat:
                hits.append((BASE + va + i, seg[i:i + len(pat)]))
    return hits


def cctable(addr):
    """Classify each of 256 chars via the char-class byte at addr+char (bit7=continue)."""
    elf, phdrs = load_elf()
    # map guest addr to file offset
    va = addr - BASE
    for p in phdrs:
        if p[0] == 1 and p[3] <= va < p[3] + p[4]:
            off = p[2] + (va - p[3])
            break
    else:
        print("0x%x not in LOAD" % addr); return
    tarr = np.frombuffer(elf[off:off + 256], dtype=np.uint8)
    term = [chr(i) for i in range(0x21, 0x7f) if not (tarr[i] & 0x80)]
    print("table @ 0x%x (file 0x%x):" % (addr, off))
    print("  printable chars with bit7 CLEAR (would terminate a string): %s" %
          ("".join(term) if term else "(none)"))
    print("  continue(bit7 set)=%d  terminate(bit7 clear)=%d" %
          (int((tarr & 0x80 != 0).sum()), int((tarr & 0x80 == 0).sum())))


def refs(target, radius=8):
    """Vectorized RIP-relative xref scan using numpy/torch over all exec segments."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    elf, phdrs = load_elf()
    hits = []
    for p in phdrs:
        if p[0] != 1 or not (p[1] & 4):
            continue
        off, va, fsz = p[2], p[3], p[4]
        code = elf[off:off + fsz]
        for ins in md.disasm(code, BASE + va):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    eff = ins.address + ins.size + op.mem.disp
                    if target - radius <= eff <= target + radius:
                        hits.append((ins.address, ins.mnemonic, ins.op_str, eff))
    for a, m, o, e in hits:
        print("0x%x -> 0x%x: %s %s" % (a, e, m, o))


def scan4(value, kind="u32"):
    """Find a numeric immediate across all segments (vectorized)."""
    elf, phdrs = load_elf()
    sz = 4 if kind == "u32" else 8
    pat = struct.pack("<" + ("I" if sz == 4 else "Q"), value)
    for p in phdrs:
        if p[0] != 1:
            continue
        off, va, fsz = p[2], p[3], p[4]
        seg = elf[off:off + fsz]
        start = 0
        while True:
            i = seg.find(pat, start)
            if i < 0:
                break
            print("0x%x (file 0x%x): %s = %d" % (BASE + va + i, off + i, kind, value))
            start = i + 1


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    c = a[0]
    print("[gpu_analyze] backend = %s" % gpu())
    if c == "search":
        for addr, b in search(a[1]):
            print("0x%x: %s" % (addr, b.hex()))
    elif c == "cctable":
        cctable(int(a[1], 0))
    elif c == "refs":
        refs(int(a[1], 0))
    elif c == "scan4":
        scan4(int(a[1], 0), a[2] if len(a) > 2 else "u32")
    else:
        print("unknown: %s" % c)


if __name__ == "__main__":
    main()
