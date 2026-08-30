#!/usr/bin/env python
"""
session.py -- the front door for running a retail title under PCSX5 and
producing a machine-readable record of what actually happened.

It exists because "the process stayed alive" is not evidence that the emulator
ran anything.  A run is classified along three independent axes:

    process alive   the host process did not exit or crash
    rendering       the emulator window produced at least one frame
    progressing     consecutive frames actually differ

A frozen frame with a live process is *not* stable execution and is reported as
`frozen`, never as success.

Reuses the existing helpers in this package (screen_capture, crash_analyzer,
input_harness) and the emulator's own controller-replay mechanism
(`--play-input=`); it does not introduce a second input or capture path.

Usage
-----
    python tools/game_runner/session.py list
    python tools/game_runner/session.py kill
    python tools/game_runner/session.py run --title PPSA02929 --duration 60
    python tools/game_runner/session.py run --title PPSA21564 --duration 120 \
            --keys "space@10,enter@20" --restart-on-crash 2
    python tools/game_runner/session.py longrun --title PPSA02929 --minutes 30
    python tools/game_runner/session.py show <run-id>
    python tools/game_runner/session.py compare <run-id>
    python tools/game_runner/session.py baseline
    python tools/game_runner/session.py baseline --update <run-id>

Records land in artifacts/runtime/<run-id>/ (untracked).  The per-title baseline
lives in tests/runtime_baseline.json (tracked) -- see Rule 08.
"""
import argparse
import datetime
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import screen_capture as sc
from crash_analyzer import analyze_crash
from input_harness import send_key_press

try:
    import psutil
except ImportError:
    psutil = None

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARTIFACTS = os.path.join(REPO, "artifacts", "runtime")
BASELINE = os.path.join(REPO, "tests", "runtime_baseline.json")
GAMES = os.path.join(REPO, "Games")

CLI_CANDIDATES = [
    os.path.join(REPO, "build", "bin", "Release", "pcsx5_cli.exe"),
    os.path.join(REPO, "build", "bin", "Debug", "pcsx5_cli.exe"),
    os.path.join(REPO, "dist", "pcsx5_cli.exe"),
]

# Boot-progress markers, kept identical to tools/autorun.py so that the two
# harnesses report the same vocabulary.
MARKERS = [
    ("first-draw", "First guest draw executed"),
    ("shaders", "Translating shaders"),
    ("pthreads", "scePthreadCreate"),
    ("content", "Done load"),
    ("menus", "menu"),
]

# Formats emitted by src/kernel/kernel.cpp.  Parsed, never guessed.
RE_RIP = re.compile(r"RIP[=:]\s*(0x[0-9a-fA-F]+)")
RE_MODULE = re.compile(r"Module:\s*([^\s,]+)")
RE_THREAD = re.compile(r"OS [Tt]hread:?\s*(\d+)")
RE_CODE = re.compile(r"Code:\s*(0x[0-9A-Fa-f]+)")
RE_HWND = re.compile(r"PCSX5_WINDOW_HANDLE=(\d+)")

FATAL_SIGNATURES = [
    ("uncaught-guest-exception", "no catch handler found for thrown exception"),
    ("host-stack-overflow", "FATAL: host stack overflow"),
    ("guest-null-call", "GUEST NULL-CALL"),
    ("unhandled-exception", "VEH Unhandled Exception"),
    ("assertion", "Assertion failed"),
]


# --------------------------------------------------------------------------- #
# Environment discovery
# --------------------------------------------------------------------------- #
def find_cli():
    for c in CLI_CANDIDATES:
        if os.path.isfile(c):
            return c
    return None


def discover_titles():
    """Map title id -> eboot path by scanning Games/<TITLEID>*/eboot.bin."""
    titles = {}
    if not os.path.isdir(GAMES):
        return titles
    for entry in sorted(os.listdir(GAMES)):
        d = os.path.join(GAMES, entry)
        if not os.path.isdir(d):
            continue
        eboot = os.path.join(d, "eboot.bin")
        if os.path.isfile(eboot):
            tid = re.split(r"[-_]", entry)[0]
            titles.setdefault(tid, eboot)
    return titles


def git_revision():
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
                             capture_output=True, text=True, timeout=15)
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "unknown"


def build_revision(cli):
    """Identify the binary under test: the core DLL's size + mtime.  A run
    record must say which build produced it, and the git revision alone does
    not (the tree may be dirty or the build stale)."""
    dll = os.path.join(os.path.dirname(cli), "pcsx5_core.dll")
    target = dll if os.path.isfile(dll) else cli
    st = os.stat(target)
    return {
        "binary": os.path.relpath(target, REPO).replace("\\", "/"),
        "size": st.st_size,
        "mtime": datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds"),
    }


def kill_stale():
    """Kill leftover emulator processes.  A lingering pcsx5_cli.exe holds a lock
    on pcsx5_core.dll and makes the next build fail with LNK1104."""
    killed = []
    for image in ("pcsx5_cli.exe", "pcsx5.exe"):
        r = subprocess.run(["taskkill", "/F", "/IM", image],
                           capture_output=True, text=True)
        if r.returncode == 0:
            killed.append(image)
    return killed


# --------------------------------------------------------------------------- #
# Log analysis
# --------------------------------------------------------------------------- #
def scan_log(path):
    """Extract markers, the last logged fault site, and the fatal signature.

    Caveat on `last_rip`: it is the RIP on the LAST log line that carried one,
    and the log interleaves lines from every guest thread.  Several handled
    exceptions (TLS emulation, demand-commit) also log a RIP.  So `last_rip` is
    a starting point for investigation, not a certified fatal site -- confirm it
    against the fatal line before quoting it as the crash address.  For a
    GUEST NULL-CALL the logged RIP is 0x0 (the null call target); the address
    worth investigating is the caller's."""
    result = {
        "markers": {},
        "last_rip": None,
        "last_module": None,
        "last_thread": None,
        "last_exception_code": None,
        "fatal": None,
        "fatal_line": None,
        "log_lines": 0,
    }
    if not os.path.isfile(path):
        return result
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for lineno, line in enumerate(f, 1):
            result["log_lines"] = lineno
            for name, needle in MARKERS:
                if name not in result["markers"] and needle in line:
                    result["markers"][name] = lineno
            m = RE_RIP.search(line)
            if m:
                result["last_rip"] = m.group(1)
                mm = RE_MODULE.search(line)
                if mm:
                    result["last_module"] = mm.group(1)
                mt = RE_THREAD.search(line)
                if mt:
                    result["last_thread"] = mt.group(1)
                mc = RE_CODE.search(line)
                if mc:
                    result["last_exception_code"] = mc.group(1)
            if result["fatal"] is None:
                for kind, needle in FATAL_SIGNATURES:
                    if needle in line:
                        result["fatal"] = kind
                        result["fatal_line"] = line.strip()[:400]
                        break
    return result


# --------------------------------------------------------------------------- #
# The run itself
# --------------------------------------------------------------------------- #
def parse_keys(spec):
    """'space@5,enter@12' -> [(5.0, 'space'), (12.0, 'enter')] sorted by time."""
    events = []
    if not spec:
        return events
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if "@" not in item:
            raise SystemExit("bad --keys entry '%s' (expected key@seconds)" % item)
        key, at = item.rsplit("@", 1)
        events.append((float(at), key.strip()))
    return sorted(events)


def run_once(args, title_id, eboot, cli, run_index=0):
    os.makedirs(ARTIFACTS, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    label = "_" + args.label if getattr(args, "label", None) else ""
    suffix = "_r%d" % run_index if run_index else ""
    run_id = "%s_%s%s%s" % (title_id, stamp, label, suffix)
    run_dir = os.path.join(ARTIFACTS, run_id)
    frames_dir = os.path.join(run_dir, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    log_path = os.path.join(run_dir, "run.log")

    kill_stale()

    argv = [cli]
    if args.headless:
        argv.append("--headless")
    argv += ["--title-id=" + title_id, "--crash-dir=" + run_dir]
    if args.input:
        argv.append("--play-input=" + os.path.abspath(args.input))
    if args.strict_imports:
        argv.append("--strict-imports")
    argv += ["--report=" + os.path.join(run_dir, "import_report.json"), eboot]

    print("[session] run_id   %s" % run_id)
    print("[session] cli      %s" % " ".join(argv))
    print("[session] duration %ss  sample %ss  headless=%s"
          % (args.duration, args.sample, args.headless))

    log_file = open(log_path, "wb")
    started = time.time()
    proc = subprocess.Popen(argv, cwd=os.path.dirname(cli),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    hwnd_box = [0]

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            log_file.write(raw)
            log_file.flush()
            if hwnd_box[0] == 0:
                m = RE_HWND.search(raw.decode("utf-8", "ignore"))
                if m:
                    hwnd_box[0] = int(m.group(1))
        try:
            proc.stdout.close()
        except Exception:
            pass

    pumper = threading.Thread(target=pump, daemon=True)
    pumper.start()

    key_events = parse_keys(args.keys)
    key_index = 0
    frames = []
    prev_img = None
    first_frame_t = None
    longest_freeze = 0.0
    freeze_start = None
    cpu_samples = []
    peak_rss = 0
    ps = None
    if psutil is not None:
        try:
            ps = psutil.Process(proc.pid)
        except Exception:
            ps = None

    termination = "completed"
    next_sample = started

    while True:
        now = time.time()
        elapsed = now - started

        if proc.poll() is not None:
            termination = "process-exit"
            break
        if elapsed >= args.duration:
            termination = "duration-reached"
            break

        while key_index < len(key_events) and key_events[key_index][0] <= elapsed:
            key = key_events[key_index][1]
            print("[session] t=%6.1fs key '%s'" % (elapsed, key))
            send_key_press(key)
            key_index += 1

        if ps is not None:
            try:
                cpu_samples.append(ps.cpu_percent(interval=None))
                peak_rss = max(peak_rss, ps.memory_info().rss)
            except Exception:
                ps = None

        if now >= next_sample:
            next_sample = now + args.sample
            if not args.headless:
                idx = len(frames)
                path = os.path.join(frames_dir, "frame_%04d.png" % idx)
                # Only ever capture the emulator's own window.  Two traps this
                # avoids: capture_window() falls back to a full-screen grab when
                # no window matches, and a title match on "pcsx5" also matches an
                # editor or terminal showing the repository path.  Either would
                # report a frame -- and therefore "rendering" -- before the
                # emulator drew anything.  Resolve the HWND the emulator printed
                # on stdout, else the one owned by its own pid, else no frame.
                hwnd = hwnd_box[0] or sc.find_window_by_pid(proc.pid)
                img = sc.capture_hwnd(hwnd, path) if hwnd else None
                if img is not None:
                    if first_frame_t is None:
                        first_frame_t = elapsed
                        print("[session] t=%6.1fs FIRST FRAME" % elapsed)
                    change = None
                    if prev_img is not None:
                        change = sc.frame_diff_ratio(prev_img, img)
                        if change < args.change_threshold:
                            if freeze_start is None:
                                freeze_start = elapsed
                            longest_freeze = max(longest_freeze, elapsed - freeze_start)
                        else:
                            freeze_start = None
                    frames.append({
                        "i": idx,
                        "t": round(elapsed, 2),
                        "hash": sc.image_hash(img),
                        "change": None if change is None else round(change, 5),
                        "path": os.path.relpath(path, run_dir).replace("\\", "/"),
                    })
                    prev_img = img
                    if args.stop_on_freeze and longest_freeze >= args.stop_on_freeze:
                        termination = "frozen"
                        break

        time.sleep(0.05)

    if proc.poll() is None:
        if termination == "completed":
            termination = "duration-reached"
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            termination += "+killed"

    pumper.join(timeout=10)
    log_file.close()
    ended = time.time()

    scan = scan_log(log_path)
    crash = analyze_crash(log_path, os.path.join(run_dir, "crash_rip_dump.bin"))
    exit_code = proc.returncode

    # A process killed by an unhandled exception exits with the NTSTATUS code
    # (e.g. 0xC0000005 STATUS_ACCESS_VIOLATION -> 3221225477).  Observed on
    # PPSA02929, where the log carried no fatal signature at all: without this
    # the run would be reported as a clean `exited`.
    ntstatus = None
    if exit_code is not None and exit_code < 0:
        ntstatus = exit_code & 0xFFFFFFFF
    elif exit_code is not None and exit_code >= 0xC0000000:
        ntstatus = exit_code
    exception_exit = ntstatus is not None and ntstatus >= 0xC0000000

    changes = [f["change"] for f in frames if f["change"] is not None]
    rendering = first_frame_t is not None
    progressing = bool(changes) and max(changes) >= args.change_threshold
    if termination == "process-exit" and (scan["fatal"] or exception_exit):
        status = "crashed"
    elif termination == "process-exit":
        status = "exited"
    elif args.headless:
        status = "ran-headless"
    elif not rendering:
        status = "no-frame"
    elif not progressing:
        status = "frozen"
    else:
        status = "progressing"

    record = {
        "schema": "pcsx5-runtime-run/1",
        "run_id": run_id,
        "title_id": title_id,
        "eboot": os.path.relpath(eboot, REPO).replace("\\", "/"),
        "git_revision": git_revision(),
        "build_revision": build_revision(cli),
        "argv": argv,
        "start_time": datetime.datetime.fromtimestamp(started).isoformat(timespec="seconds"),
        "end_time": datetime.datetime.fromtimestamp(ended).isoformat(timespec="seconds"),
        "requested_duration_s": args.duration,
        "duration_s": round(ended - started, 2),
        "headless": bool(args.headless),
        "exit_code": exit_code,
        "exit_ntstatus": None if ntstatus is None else ("0x%08X" % ntstatus),
        "termination_reason": termination,
        "status": status,
        "execution": {
            "process_alive_s": round(ended - started, 2),
            "rendering": rendering,
            "progressing": progressing,
            "first_frame_s": None if first_frame_t is None else round(first_frame_t, 2),
            "frame_count": len(frames),
            "unique_frame_hashes": len(set(f["hash"] for f in frames)),
            "frame_change_mean": round(sum(changes) / len(changes), 5) if changes else None,
            "frame_change_max": round(max(changes), 5) if changes else None,
            "longest_freeze_s": round(longest_freeze, 2),
            "last_frame_index": frames[-1]["i"] if frames else None,
            "last_frame_time_s": frames[-1]["t"] if frames else None,
        },
        "process": {
            "cpu_percent_mean": round(sum(cpu_samples) / len(cpu_samples), 1) if cpu_samples else None,
            "peak_rss_mb": round(peak_rss / (1024 * 1024), 1) if peak_rss else None,
        },
        "markers": scan["markers"],
        "crash": {
            "fatal_signature": scan["fatal"] or ("exception-exit" if exception_exit else None),
            "fatal_line": scan["fatal_line"],
            "last_rip": scan["last_rip"],
            "last_module": scan["last_module"],
            "last_thread": scan["last_thread"],
            "last_exception_code": scan["last_exception_code"],
            "analyzer": crash,
        },
        "log": "run.log",
        "log_lines": scan["log_lines"],
        "frames": frames,
    }
    with open(os.path.join(run_dir, "record.json"), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)

    print_summary(record)
    return record


def print_summary(rec):
    ex = rec["execution"]
    c = rec["crash"]
    print("")
    print("  status              %s" % rec["status"])
    print("  termination         %s (exit=%s%s)"
          % (rec["termination_reason"], rec["exit_code"],
             "" if not rec.get("exit_ntstatus") else " / " + rec["exit_ntstatus"]))
    print("  duration            %ss of %ss" % (rec["duration_s"], rec["requested_duration_s"]))
    print("  rendering           %s   first frame: %ss" % (ex["rendering"], ex["first_frame_s"]))
    print("  progressing         %s   frames: %s unique: %s"
          % (ex["progressing"], ex["frame_count"], ex["unique_frame_hashes"]))
    print("  frame change        max=%s mean=%s" % (ex["frame_change_max"], ex["frame_change_mean"]))
    print("  longest freeze      %ss" % ex["longest_freeze_s"])
    print("  markers             %s" % (", ".join(rec["markers"]) or "(none)"))
    print("  fatal               %s" % (c["fatal_signature"] or "(none)"))
    if c["last_rip"]:
        print("  last RIP            %s module=%s thread=%s"
              % (c["last_rip"], c["last_module"], c["last_thread"]))
    print("  record              artifacts/runtime/%s/record.json" % rec["run_id"])
    print("")


# --------------------------------------------------------------------------- #
# Baseline database
# --------------------------------------------------------------------------- #
def load_baseline():
    if os.path.isfile(BASELINE):
        with open(BASELINE, "r", encoding="utf-8") as f:
            return json.load(f)
    return {"schema": "pcsx5-runtime-baseline/1", "updated": None,
            "git_revision": None, "titles": {}}


def save_baseline(db):
    db["updated"] = datetime.datetime.now().isoformat(timespec="seconds")
    db["git_revision"] = git_revision()
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    with open(BASELINE, "w", encoding="utf-8") as f:
        json.dump(db, f, indent=2)
        f.write("\n")


def load_record(run_id):
    path = os.path.join(ARTIFACTS, run_id, "record.json")
    if not os.path.isfile(path):
        raise SystemExit("no record for run '%s' (%s)" % (run_id, path))
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def entry_from_record(rec, previous=None):
    ex = rec["execution"]
    prev_boundary = (previous or {}).get(
        "current_boundary",
        {"summary": None, "classification": "UNKNOWN", "doc": None})
    return {
        "eboot": rec["eboot"],
        "boot_success": rec["status"] not in ("crashed", "no-frame"),
        "first_frame_s": ex["first_frame_s"],
        "sustained_runtime_s": rec["duration_s"],
        "status": rec["status"],
        "markers": sorted(rec["markers"]),
        "crash_location": {
            "signature": rec["crash"]["fatal_signature"],
            "rip": rec["crash"]["last_rip"],
            "module": rec["crash"]["last_module"],
            "thread": rec["crash"]["last_thread"],
        },
        "current_boundary": prev_boundary,
        "last_run_id": rec["run_id"],
        "git_revision": rec["git_revision"],
        "build_revision": rec["build_revision"],
        "updated": datetime.datetime.now().isoformat(timespec="seconds"),
    }


def compare_to_baseline(rec, db):
    """Answer 'did this change improve or regress the emulator?' without relying
    on anyone's memory."""
    base = db.get("titles", {}).get(rec["title_id"])
    if not base:
        return "unknown", ["no baseline recorded for this title"]

    notes = []
    state = {"verdict": "unchanged"}

    def worse(msg):
        state["verdict"] = "regressed"
        notes.append("REGRESSED: " + msg)

    def better(msg):
        if state["verdict"] != "regressed":
            state["verdict"] = "advanced"
        notes.append("ADVANCED: " + msg)

    rank = {"crashed": 0, "no-frame": 1, "exited": 2, "ran-headless": 3,
            "frozen": 4, "progressing": 5}
    new_r = rank.get(rec["status"], 1)
    old_r = rank.get(base.get("status", ""), 1)
    if new_r < old_r:
        worse("status %s -> %s" % (base.get("status"), rec["status"]))
    elif new_r > old_r:
        better("status %s -> %s" % (base.get("status"), rec["status"]))

    new_m = set(rec["markers"])
    old_m = set(base.get("markers") or [])
    lost = old_m - new_m
    gained = new_m - old_m
    if lost:
        worse("markers lost: " + ", ".join(sorted(lost)))
    if gained:
        better("markers gained: " + ", ".join(sorted(gained)))

    old_sig = (base.get("crash_location") or {}).get("signature")
    new_sig = rec["crash"]["fatal_signature"]
    if old_sig != new_sig:
        notes.append("CHANGED: fatal signature %s -> %s" % (old_sig, new_sig))
    old_rip = (base.get("crash_location") or {}).get("rip")
    if old_rip and rec["crash"]["last_rip"] and old_rip != rec["crash"]["last_rip"]:
        notes.append("CHANGED: last RIP %s -> %s" % (old_rip, rec["crash"]["last_rip"]))

    if not notes:
        notes.append("no observable difference from baseline")
    return state["verdict"], notes


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #
def resolve_title(name):
    titles = discover_titles()
    if name in titles:
        return name, titles[name]
    raise SystemExit("unknown title '%s'. Known: %s"
                     % (name, ", ".join(titles) or "(none found under Games/)"))


def cmd_run(args):
    cli = find_cli()
    if not cli:
        raise SystemExit("pcsx5_cli.exe not found. Build first: "
                         "cmake --build build --config Release")
    title_id, eboot = resolve_title(args.title)
    db = load_baseline()

    attempts = max(1, args.restart_on_crash + 1)
    last = None
    for i in range(attempts):
        last = run_once(args, title_id, eboot, cli, run_index=i)
        verdict, notes = compare_to_baseline(last, db)
        print("  vs baseline         %s" % verdict)
        for n in notes:
            print("    - %s" % n)
        print("")
        if last["status"] != "crashed" or i == attempts - 1:
            break
        print("[session] crashed; restarting (%d/%d)" % (i + 1, args.restart_on_crash))
    return 0 if last and last["status"] in ("progressing", "ran-headless") else 1


def cmd_longrun(args):
    args.duration = int(args.minutes * 60)
    args.label = args.label or ("long%dm" % int(args.minutes))
    return cmd_run(args)


def cmd_list(args):
    titles = discover_titles()
    print("Titles under Games/:")
    for tid, eboot in titles.items():
        print("  %-12s %s" % (tid, os.path.relpath(eboot, REPO)))
    print("")
    print("Recent runs in artifacts/runtime/:")
    runs = sorted(glob.glob(os.path.join(ARTIFACTS, "*", "record.json")),
                  key=os.path.getmtime, reverse=True)[:15]
    if not runs:
        print("  (none)")
    for r in runs:
        try:
            with open(r, encoding="utf-8") as f:
                rec = json.load(f)
            print("  %-44s %-14s %ss %s"
                  % (rec["run_id"], rec["status"], rec["duration_s"], rec["git_revision"]))
        except Exception:
            continue
    return 0


def cmd_show(args):
    print_summary(load_record(args.run_id))
    return 0


def cmd_compare(args):
    rec = load_record(args.run_id)
    verdict, notes = compare_to_baseline(rec, load_baseline())
    print("%s: %s" % (rec["run_id"], verdict))
    for n in notes:
        print("  - %s" % n)
    return 0


def cmd_baseline(args):
    db = load_baseline()
    if args.update:
        rec = load_record(args.update)
        tid = rec["title_id"]
        db.setdefault("titles", {})
        verdict, notes = compare_to_baseline(rec, db)
        db["titles"][tid] = entry_from_record(rec, db["titles"].get(tid))
        db["titles"][tid]["regression_state"] = verdict
        save_baseline(db)
        print("baseline updated for %s from %s (%s)" % (tid, rec["run_id"], verdict))
        for n in notes:
            print("  - %s" % n)
        return 0
    print(json.dumps(db, indent=2))
    return 0


def cmd_kill(args):
    killed = kill_stale()
    print("killed: " + (", ".join(killed) if killed else "(nothing running)"))
    return 0


def main():
    p = argparse.ArgumentParser(
        description="PCSX5 runtime session driver",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_run_args(sp):
        sp.add_argument("--title", required=True, help="title id, e.g. PPSA02929")
        sp.add_argument("--headless", action="store_true",
                        help="no window; log-only run (frame validation is impossible)")
        sp.add_argument("--input", help="controller replay JSON, passed as --play-input=")
        sp.add_argument("--keys", help='keyboard schedule, e.g. "space@10,enter@20"')
        sp.add_argument("--sample", type=float, default=2.0,
                        help="frame sample interval in seconds")
        sp.add_argument("--change-threshold", type=float, default=0.005,
                        help="fraction of pixels that must differ to count as progress")
        sp.add_argument("--stop-on-freeze", type=float, default=0.0,
                        help="abort after this many seconds of frozen frames (0 = never)")
        sp.add_argument("--restart-on-crash", type=int, default=0)
        sp.add_argument("--strict-imports", action="store_true")
        sp.add_argument("--label", help="slug appended to the run id")

    sp = sub.add_parser("run", help="run a title for a fixed duration")
    add_run_args(sp)
    sp.add_argument("--duration", type=int, default=120, help="seconds")
    sp.set_defaults(func=cmd_run)

    sp = sub.add_parser("longrun", help="long-duration soak run")
    add_run_args(sp)
    sp.add_argument("--minutes", type=float, required=True,
                    help="1, 5, 10, 30 or 60 are the standard soak durations")
    sp.set_defaults(func=cmd_longrun)

    sp = sub.add_parser("list", help="list titles and recent runs")
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("show", help="print a stored run summary")
    sp.add_argument("run_id")
    sp.set_defaults(func=cmd_show)

    sp = sub.add_parser("compare", help="compare a run against the baseline")
    sp.add_argument("run_id")
    sp.set_defaults(func=cmd_compare)

    sp = sub.add_parser("baseline", help="show or update the per-title baseline")
    sp.add_argument("--update", metavar="RUN_ID")
    sp.set_defaults(func=cmd_baseline)

    sp = sub.add_parser("kill", help="kill stale emulator processes")
    sp.set_defaults(func=cmd_kill)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
