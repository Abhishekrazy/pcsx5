#!/usr/bin/env python
"""orchestrator.py - Dreaming Sarah bring-up autopilot (patch->build->run->classify->keep/revert)."""
import os, re, json, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(REPO, "build", "bin", "Release", "pcsx5_cli.exe")
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
CFG = os.path.join(REPO, "pcsx5_config")
LOGDIR = os.path.join(REPO, ".work", "orex")
HIST = os.path.join(REPO, ".work", "orex_history.jsonl")

RANK = {
    "build-fail": 0, "boot-fail": 1, "guest-crash": 2, "uncaught-exception": 3,
    "ran": 4, "first-draw": 5, "shaders": 6, "pthreads": 7, "content": 8,
    "menus": 9, "ingame": 10, "win": 11,
}

def rank(o):
    return RANK.get(o, 1)

def log(*a, **kw):
    print(*a, flush=kw.get("flush", True))

def sh(cmd, timeout=None):
    return subprocess.run(cmd, shell=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout, cwd=REPO)

def build():
    subprocess.run("taskkill /F /IM pcsx5_cli.exe >NUL 2>&1", shell=True, cwd=REPO)
    log("[build] compiling ...", flush=True)
    t = time.time()
    p = sh("cmd /c build_msvc_build.bat")
    if p.returncode != 0:
        log("[build] FAILED", flush=True)
        for line in p.stdout.splitlines():
            if re.search(r"error C\d+|error LNK\d+|fatal error", line):
                log("   " + line.strip(), flush=True)
        return False
    log("[build] OK %.0fs" % (time.time() - t), flush=True)
    return True

MARKERS = [
    ("win", "ingame"), ("menus", "menu"), ("content", "Done load"),
    ("pthreads", "scePthreadCreate"), ("shaders", "Translating shaders"),
    ("first-draw", "First guest draw executed"),
]

def classify(txt):
    if "no catch handler found for thrown exception" in txt:
        return "uncaught-exception", "St16invalid_argument parse error"
    if "GUEST APPLICATION CRASHED" in txt or "VEH Unhandled Exception" in txt:
        return "guest-crash", ""
    for name, marker in MARKERS:
        if marker in txt:
            return name, ""
    return "ran", ""

def run(timeout=120):
    if not os.path.exists(CLI):
        return {"outcome": "boot-fail", "detail": "cli missing", "rc": -1, "dur": 0.0}
    os.makedirs(LOGDIR, exist_ok=True)
    lp = os.path.join(LOGDIR, "run_%s.log" % time.strftime("%H%M%S"))
    argv = [CLI, "--headless", "--title-id=PPSA02929",
            "--config-dir=" + CFG, "--log-file=" + lp, EBOOT]
    t = time.time()
    try:
        p = subprocess.run(argv, cwd=REPO, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        rc = p.returncode
        out = p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        rc, out = -2, ""
    dt = time.time() - t
    txt = open(lp, encoding="utf-8", errors="replace").read() if os.path.exists(lp) else out
    if not txt:
        txt = out
    outcome, detail = classify(txt)
    return {"outcome": outcome, "detail": detail, "rc": rc, "dur": round(dt, 1), "log": lp}

def git_restore(path):
    sh('git checkout -- "%s"' % path)

def exp_precommit_relocate():
    """Move the 256MB pre-commit off the eboot preferred base 0x800000000."""
    path = os.path.join(REPO, "src", "memory", "memory.cpp")
    src = open(path, encoding="utf-8").read()
    marker = "guest_addr_t pre_base = 0x800000000ULL;"
    if marker not in src:
        return None
    open(path, "w", encoding="utf-8").write(src.replace(
        marker, "guest_addr_t pre_base = 0x820000000ULL; // EXP-RELOC"))
    return ("raised pre_base to 0x820000000", lambda: git_restore(path))

def exp_strict_dylib():
    """pcsx5 equivalent of SharpEmu StrictDynlibResolution=true."""
    path = os.path.join(REPO, "pcsx5_config", "titles", "PPSA02929.json")
    src = open(path, encoding="utf-8").read()
    if "strict_imports" in src:
        return None
    new = src.replace('"cpu"', '"hle": {\n    "strict_imports": true\n  },\n  "cpu"', 1)
    if new == src:
        return None
    open(path, "w", encoding="utf-8").write(new)
    return ("strict_imports=true for PPSA02929", lambda: git_restore(path))

EXPERIMENTS = [
    ("precommit-relocate", exp_precommit_relocate),
    ("strict-dylib", exp_strict_dylib),
]

def record(name, base, after, kept):
    os.makedirs(os.path.dirname(HIST), exist_ok=True)
    with open(HIST, "a", encoding="utf-8") as f:
        f.write(json.dumps({"exp": name, "ts": time.strftime("%Y%m%d-%H%M%S"),
                            "before": base["outcome"], "after": after["outcome"],
                            "kept": kept}) + "\n")

def run_experiment(name):
    fn = dict(EXPERIMENTS).get(name)
    if not fn:
        log("unknown experiment:", name); return
    if not build():
        return
    base = run()
    log("[exp %s] baseline: %s" % (name, base["outcome"]))
    r = fn()
    if not r:
        log("[exp %s] apply failed / already applied" % name); return
    msg, revert = r
    log("[exp %s] applied: %s" % (name, msg))
    if not build():
        revert(); record(name, base, {"outcome": "build-fail"}, False)
        log("[exp %s] build failed, reverted" % name); return
    after = run()
    better = rank(after["outcome"]) > rank(base["outcome"])
    if better:
        record(name, base, after, True)
        log("[exp %s] IMPROVEMENT %s -> %s (kept)" % (name, base["outcome"], after["outcome"]))
    else:
        revert()
        record(name, base, after, False)
        log("[exp %s] no improvement %s -> %s (reverted)" % (name, base["outcome"], after["outcome"]))

def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    c = a[0]
    if c == "exp":
        if len(a) > 1 and a[1] == "list":
            for n, _ in EXPERIMENTS: print("  " + n)
        elif len(a) > 2 and a[1] == "run":
            run_experiment(a[2])
        elif len(a) > 1 and a[1] == "all":
            for n, _ in EXPERIMENTS: run_experiment(n)
    elif c == "baseline":
        if build():
            print(run())
    else:
        print("unknown:", c); print(__doc__)

if __name__ == "__main__":
    main()