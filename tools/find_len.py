#!/usr/bin/env python
"""Byte-level search for the token length idiom across the eboot:
  mov rsi, [rsi+0x70]  = 48 8b 76 70
  sub rsi, [r14+0x60]  = 49 2b 76 60 (r14) / 48 2b 76 60 (rsi)
Report every "mov _,[_+0x70]" and surrounding bytes so we can see all length computations."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".work"))
from disasm import reconstruct
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
BASE = 0x810000000
elf, phdrs = reconstruct(EBOOT)

# patterns (hex)
pats = {
  "mov.[,]+0x70 (48 8b ?? 70)": None,
}
# Generic: find all "48 8b ?? 70" (mov r64, [r64+0x70]) and "49 8b ?? 70"
import re
for name, pat in [
    ("48 8b ?? 70  (mov r,[r+0x70])", rb"\x48\x8b\x[\x00-\xbf]\x70"),
    ("49 8b ?? 70  (mov r,[r8-15+0x70])", rb"\x49\x8b\x[\x00-\xbf]\x70"),
]:
    for p in phdrs:
        if p[0] != 1 or not (p[1] & 4):
            continue
        off, va, fsz = p[2], p[3], p[4]
        code = elf[off:off+fsz]
        for m in re.finditer(pat, code, re.DOTALL):
            gva = BASE + va + m.start()
            # dump 16 bytes of context
            ctx = code[m.start():m.start()+16]
            print("0x%x: %s" % (gva, ctx.hex(" ")))
