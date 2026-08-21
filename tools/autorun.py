#!/usr/bin/env python
"""
autorun.py - fully automated build -> run -> capture -> analyze -> repeat loop
for the Dreaming Sarah (PPSA02929) bring-up.

Features:
  - Auto-rebuild C++ core (kills lingering pcsx5_cli.exe that locks the DLL).
  - Headless boot with/without the guest tracer / string-copy probe.
  - Captures the full log + a trimmed 'signal' summary to .work/autologs/.
  - Classifies the outcome (uncaught-exception / guest-crash / progress marker).
  - Persists a per-run verdict in .work/autorun_history.jsonl (append-only).
  - Runs N iterations or until a target marker is reached.

Usage:
  python tools/autorun.py run           # one build+run, print verdict
  python tools/autorun.py loop --max 50 # build+run repeatedly
  python tools/autorun.py loop --max 50 --until "menus"   # stop when a marker appears
  python tools/autorun.py history      # print the run history
  python tools/autorun.py grep <regex> # grep the latest captured log

Env toggles (forwarded to the emulator):
  PCSX5_GUEST_TRACE=1       guest INT3 tracer
  PCSX5_STR_COPY_PROBE=1    HLE memcpy string probe
"""
import os, re, subprocess, sys, time, json, glob

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(REPO, "build", "bin", "Release", "pcsx5_cli.exe")
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
CFG = os.path.join(REPO, "pcsx5_config")
LOGDIR = os.path.join(REPO, ".work", "autologs")
HIST = os.path.join(REPO, ".work", "autorun_history.jsonl")

MARKERS = {
    "first-draw": "First guest draw executed",
    "shaders": "Translating shaders",
    "pthreads": "scePthreadCreate",
    "content": "Done load",
    "menus": "menu",
}


def sh(cmd, timeout=None, cwd=REPO):
    return subprocess.run(cmd, shell=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout, cwd=cwd)


def build():
    os.makedirs(LOGDIR, exist_ok=True)
    subprocess.run("taskkill /F /IM pcsx5_cli.exe >NUL 2>&1", shell=True, cwd=REPO)
    print("[build] compiling ...", flush=True)
    t = time.time()
    p = sh("cmd /c build_msvc_build.bat")
    if p.returncode != 0:
        print("[build] FAILED rc=%d" % p.returncode, flush=True)
        for line in p.stdout.splitlines():
            if re.search(r"error C\d+|error LNK\d+|fatal error", line):
                print("   " + line.strip(), flush=True)
        return False
    print("[build] OK %.1fs" % (time.time() - t), flush=True)
    return True


def classify(txt):
    if "no catch handler found for thrown exception" in txt:
        m = re.search(r"__cxa_throw type: '(\w+)'", txt)
        o = re.search(r'obj\[\+16\] -> string: "([^"]*)"', txt)
        return "uncaught-exception", "type=%s msg=%s" % (
            m.group(1) if m else "?", o.group(1) if o else "?")
    if "GUEST APPLICATION CRASHED" in txt or "VEH Unhandled Exception" in txt:
        return "guest-crash", ""
    for name, marker in MARKERS.items():
        if marker in txt:
            return "progress", name
    return "ran", ""


def run_once(idx, trace=False, probe=False):
    ts = time.strftime("%Y%m%d_%H%M%S")
    lp = os.path.join(LOGDIR, "run_%04d_%s.log" % (idx, ts))
    env = dict(os.environ)
    if trace:
        env["PCSX5_GUEST_TRACE"] = "1"
    if probe:
        env["PCSX5_STR_COPY_PROBE"] = "1"
    # Use list-form argv (no shell quoting issues); run with cwd=REPO so
    # relative assets/nid_db.txt / pcsx5_config resolve correctly.
    argv = [CLI, "--headless", "--title-id=PPSA02929",
            "--config-dir=" + CFG, "--log-file=" + lp, EBOOT]
    t = time.time()
    try:
        p = subprocess.run(argv, cwd=REPO, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=200)
        rc = p.returncode
        out = p.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        rc = -1
        out = ""
    dt = time.time() - t
    txt = ""
    if os.path.exists(lp):
        txt = open(lp, "r", encoding="utf-8", errors="replace").read()
    if not txt and out:
        txt = out
    if out:
        # always persist captured stdout too (belt and suspenders)
        open(lp + ".out", "w", encoding="utf-8").write(out)
    outcome, detail = classify(txt)
    rec = {"index": idx, "ts": ts, "rc": rc, "dur_s": round(dt, 1),
           "outcome": outcome, "detail": detail, "trace": trace, "probe": probe,
           "log": os.path.basename(lp)}
    with open(HIST, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    print("[run %04d] %.1fs rc=%d %-22s %s" % (idx, dt, rc, outcome, detail), flush=True)
    return rec


def loop(max_iter=50, until=None, trace=False, probe=False):
    for i in range(1, max_iter + 1):
        print("=== iteration %d/%d ===" % (i, max_iter), flush=True)
        if not build():
            return
        rec = run_once(i, trace=trace, probe=probe)
        if until and (until in rec["outcome"] or until in rec["detail"]):
            print("[loop] reached target '%s' -> stopping." % until, flush=True)
            return


def history(n=20):
    if not os.path.exists(HIST):
        print("no history yet"); return
    recs = [json.loads(l) for l in open(HIST, encoding="utf-8")]
    for r in recs[-n:]:
        print("%4d  %-22s  %6.1fs  %s" % (r["index"], r["outcome"], r["dur_s"], r["detail"]))


def grep(regex, n=-1):
    logs = sorted(glob.glob(os.path.join(LOGDIR, "run_*.log")))
    if not logs:
        print("no logs"); return
    latest = logs[-1]
    lines = open(latest, encoding="utf-8", errors="replace").read().splitlines()
    hits = [l for l in lines if re.search(regex, l)]
    print("grep '%s' in %s (%d hits):" % (regex, os.path.basename(latest), len(hits)))
    for h in hits[:n if n > 0 else len(hits)]:
        print("  " + h)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    c = a[0]
    os.makedirs(LOGDIR, exist_ok=True)
    if c == "run":
        build() and run_once(1)
    elif c == "loop":
        m = 0; until = None; trace = False; probe = False
        for i in range(1, len(a)):
            if a[i] == "--max" and i + 1 < len(a): m = int(a[i + 1])
            elif a[i] == "--until" and i + 1 < len(a): until = a[i + 1]
            elif a[i] == "--trace": trace = True
            elif a[i] == "--probe": probe = True
        loop(m or 50, until, trace, probe)
    elif c == "history":
        n = int(a[1]) if len(a) > 1 else 20
        history(n)
    elif c == "grep":
        grep(a[1], int(a[2]) if len(a) > 2 else -1)
    else:
        print("unknown: %s" % c); print(__doc__)


if __name__ == "__main__":
    main()
