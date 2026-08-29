#!/usr/bin/env python
"""
dream_tool.py - automation for the Dreaming Sarah (PPSA21564) bring-up loop.

Collapses the repetitive build / headless-run / trace-analyze / disassemble
cycle into one script so an agent (or human) fires one command instead of many.

Usage:
  python dream_tool.py build                          # rebuild C++ core (MSVC)
  python dream_tool.py run [--trace] [--timeout N]    # headless boot, print outcome
  python dream_tool.py analyze [trace.log]             # summarize a guest_trace.log
  python dream_tool.py disasm <gva> [count]            # disassemble around guest VA
  python dream_tool.py segments                        # print eboot LOAD segment map
  python dream_tool.py tls                             # print eboot PT_TLS info
  python dream_tool.py loop [--max N]                  # build+run until crash, N iterations
"""
import os, re, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(REPO, "build", "bin", "Release", "pcsx5_cli.exe")
EBOOT = os.path.join(REPO, "Games", "PPSA21564-app", "eboot.bin")
CFG = os.path.join(REPO, "pcsx5_config")
WORK = os.path.join(REPO, ".work")
BASE = 0x810000000

try:
    import capstone
except ImportError:
    capstone = None


def log(*a):
    print(*a, flush=True)


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)


def build():
    bat = os.path.join(REPO, "build_msvc_build.bat")
    subprocess.run("taskkill /F /IM pcsx5_cli.exe >NUL 2>&1", shell=True)
    log("[build] compiling ...")
    t = time.time()
    p = sh("cmd /c \"%s\"" % bat)
    if p.returncode != 0:
        log("[build] FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines():
            if re.search(r"error C\d+|error LNK\d+|fatal error", line):
                log("  " + line.strip())
        return 1
    log("[build] OK in %.1fs" % (time.time() - t))
    return 0


def summarize_log(lp):
    try:
        txt = open(lp, "r", encoding="utf-8", errors="replace").read()
    except OSError:
        return "no-log", ""
    if "no catch handler found for thrown exception" in txt:
        m = re.search(r"__cxa_throw type: '(\w+)'", txt)
        o = re.search(r'obj\[\+16\] -> string: "([^"]*)"', txt)
        return "uncaught-exception", "type=%s msg=%s" % (
            m.group(1) if m else "?", o.group(1) if o else "?")
    if "VEH Unhandled Exception" in txt or "GUEST APPLICATION CRASHED" in txt:
        return "guest-crash", ""
    if "First guest draw executed" in txt:
        return "first-draw", ""
    if "Translating shaders" in txt:
        return "shaders", ""
    return "ran", ""


def boot(trace=False, timeout=100, logpath=None):
    if not os.path.exists(CLI):
        log("[run] pcsx5_cli.exe not found - build first"); return 1
    tr = os.path.join(REPO, "guest_trace.log")
    if os.path.exists(tr):
        os.remove(tr)
    lp = logpath or os.path.join(WORK, "dreamboot.log")
    cmd = ('cmd /c set PCSX5_GUEST_TRACE=%s&& "%s" --headless --title-id=PPSA21564 '
           '--config-dir="%s" --log-file="%s" "%s" 2>&1' %
           ("1" if trace else "", CLI, CFG, lp, EBOOT))
    t = time.time()
    p = sh(cmd, timeout=timeout)
    dt = time.time() - t
    outcome, detail = summarize_log(lp)
    log("[run] %.1fs rc=%d outcome=%s" % (dt, p.returncode, outcome))
    if detail:
        log("      " + detail)
    if os.path.exists(tr):
        log("      trace -> %s (%d bytes)" % (tr, os.path.getsize(tr)))
    return 0


def analyze(tracepath):
    tp = os.path.join(REPO, tracepath) if not os.path.isabs(tracepath) else tracepath
    if not os.path.exists(tp):
        log("[analyze] no trace at %s" % tp); return 1
    lines = open(tp, "r", encoding="utf-8", errors="replace").read().splitlines()
    log("[analyze] %d lines" % len(lines))
    tids, imgs, short = {}, [], []
    workers = 0
    for ln in lines:
        m = re.match(r"tid=([0-9a-f]+) .*len=(\d+) \"([^\"]*)\"", ln)
        if m:
            tids[m.group(1)] = tids.get(m.group(1), 0) + 1
            if "image" in m.group(3):
                imgs.append((int(m.group(2)), m.group(3)))
            if int(m.group(2)) < 10 and m.group(3):
                short.append((m.group(1), int(m.group(2)), m.group(3)))
        if ln.startswith("WORKER"):
            workers += 1
    log("  distinct string-read threads: %d" % len(tids))
    for tid in sorted(tids):
        log("    tid=%s reads=%d" % (tid, tids[tid]))
    log("  image reads: %d" % len(imgs))
    for l, s in imgs[:10]:
        log("    len=%-3d %s" % (l, s))
    log("  short reads (len<10): %d" % len(short))
    for tid, l, s in short[:10]:
        log('    tid=%s len=%d "%s"' % (tid, l, s))
    log("  worker dispatch hits: %d" % workers)
    return 0


def disasm(gva, count=40):
    if capstone is None:
        log("capstone not installed"); return
    sys.path.insert(0, WORK)
    from disasm import reconstruct
    elf, phdrs = reconstruct(EBOOT)
    va = gva - BASE
    seg = None
    for p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz in phdrs:
        if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
            seg = (p_offset, p_vaddr); break
    if not seg:
        log("0x%x not in any LOAD seg (base 0x%x)" % (gva, BASE)); return
    foff = seg[0] + (va - seg[1])
    code = elf[foff:foff + count * 20]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for ins in md.disasm(code, gva):
        mark = "=>" if ins.address == gva else "  "
        print("%s 0x%09x: %s\t%s" % (mark, ins.address, ins.mnemonic, ins.op_str))
        count -= 1
        if count <= 0:
            break


def segments():
    sys.path.insert(0, WORK)
    from disasm import reconstruct
    elf, phdrs = reconstruct(EBOOT)
    names = {0: "NULL", 1: "LOAD", 2: "DYNAMIC", 6: "PHDR", 7: "TLS",
             0x6474E550: "EH_FRAME", 0x6474E552: "RELRO"}
    for p in phdrs:
        t, fl, off, va, fsz, msz = p
        nm = names.get(t, hex(t))
        print("%-9s off=0x%-8x vaddr=0x%-9x filesz=0x%-8x memsz=0x%-8x%s" %
              (nm, off, va, fsz, msz, "  <== TLS" if t == 7 else ""))


def tlsinfo():
    sys.path.insert(0, WORK)
    from disasm import reconstruct
    elf, phdrs = reconstruct(EBOOT)
    for p in phdrs:
        if p[0] == 7:
            _, fl, off, va, fsz, msz = p
            print("PT_TLS off=0x%x vaddr=0x%x filesz=0x%x memsz=0x%x (zero-init=%s)" %
                  (off, va, fsz, msz, fsz == 0))
            return
    print("no PT_TLS segment")


def loop():
    for i in range(1000000):
        log("=== iteration %d ===" % (i + 1))
        if build() != 0:
            return
        boot(trace=True)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    c = a[0]
    if c == "build":
        sys.exit(build())
    elif c == "run":
        sys.exit(boot(trace="--trace" in a))
    elif c == "analyze":
        sys.exit(analyze(a[1] if len(a) > 1 else "guest_trace.log"))
    elif c == "disasm":
        disasm(int(a[1], 0), int(a[2], 0) if len(a) > 2 else 40)
    elif c == "segments":
        segments()
    elif c == "tls":
        tlsinfo()
    elif c == "loop":
        loop()
    else:
        print("unknown: %s" % c); print(__doc__)


if __name__ == "__main__":
    main()
