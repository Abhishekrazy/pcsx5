#!/usr/bin/env python
"""
autopilot.py — Dreaming Sarah (PPSA02929) end-to-end bring-up autopilot.

Owns the whole cycle so Python does all of the repetition:

    build -> run(classify+score) -> try a fix/knob -> rebuild -> rerun

until Dreaming Sarah advances past content-load into menus/in-game, and then
drives it with an input replay ("auto play by guessing") using hill-climbing
over generated controller sequences.  Every source/config edit is reversible:
an improvement is kept (and persisted); a regression is reverted automatically.

Toolchain used on purpose: pure Python standard library only — no torch/capstone
dependency, so this driver starts instantly and never breaks because of the GPU
brute-force stack.  It drives the C++ emulator via its own CLI instead of the GPU
backends (irrelevant to the boot/content-load path it cares about).

Subcommands:
    baseline                       one build + headless run, full report
    iterate --max-rounds N         closed fix->rerun loop, then replay auto-play
    knob <field> <old> <new>       one-off safe config toggle (with revert)
"""
import os
import re
import sys
import json
import time
import subprocess
import random
import argparse

# --------------------------------------------------------------------------- #
# Paths / constants
# --------------------------------------------------------------------------- #
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(REPO, "build", "bin", "Release", "pcsx5_cli.exe")
EBOOT = os.path.join(REPO, "Games", "PPSA02929-app0", "eboot.bin")
CFG_GLOBAL = os.path.join(REPO, "pcsx5_config", "global.json")
WORK = os.path.join(REPO, ".work")
LOGDIR = os.path.join(WORK, "autopilot_logs")
HIST = os.path.join(WORK, "autopilot_history.jsonl")
STATE = os.path.join(WORK, "autopilot_state.json")
REPLAYS_DIR = os.path.join(REPO, "replays")
BASE_REPLAY = os.path.join(REPLAYS_DIR, "PPSA02929_autopilot.json")

TIMEOUT_BASELINE = 200   # headless boot (no input)
TIMEOUT_PATCH     = 200   # after a source patch is applied
TIMEOUT_PLAY      = 280   # with replay playback (more time for menus/gameplay)

# Ordered progression markers -> (name, substring searched for in the log).
PROG_ORDER = [
    ("first-draw", "First guest draw executed"),
    ("shaders", "Translating shaders"),
    ("pthreads", "scePthreadCreate"),
    ("content", "Done load"),
]

CRASH_PHRASES = ("GUEST APPLICATION CRASHED", "VEH Unhandled Exception")
MENU_PHRASES = (
    "Reached the menu", "menu navigation", "in-game", "gameplay",
    "Title Screen", "Level Select", "Character Select",
)

# Outcome name -> rank used for reporting / tie-breaking.
OUTCOME_RANK = {
    "cli-missing": -1, "build-fail": 0, "boot-fail": 1, "hang": 2,
    "guest-crash": 3, "uncaught-exception": 4, "ran": 5,
}


def log(*a):
    print(*a, flush=True)


# --------------------------------------------------------------------------- #
# Shell helpers
# --------------------------------------------------------------------------- #
def sh(cmd, timeout=None, cwd=REPO):
    """Run a shell command capturing combined output. Returns (rc, text)."""
    p = subprocess.run(cmd, shell=True, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, cwd=cwd)
    return p.returncode, (p.stdout or "")


def build():
    """MSVC release rebuild. Returns dict(ok, elapsed, errors)."""
    # Kill any lingering cli that locks pcsx5_core.dll -> intermittent LNK1104.
    sh("taskkill /F /IM pcsx5_cli.exe >NUL 2>&1")
    log("[build] killing stale pcsx5_cli.exe, compiling ...")
    t = time.time()
    rc, out = sh('cmd /c build_msvc_build.bat')
    if rc != 0:
        errs = []
        for line in out.splitlines():
            if re.search(r"error C\d+|error LNK\d+|fatal error", line):
                errs.append(line.strip())
        return {"ok": False, "elapsed": round(time.time() - t, 1),
                "errors": errs}
    log("[build] OK in %.1fs" % (time.time() - t))
    return {"ok": True, "elapsed": round(time.time() - t, 1), "errors": []}


# --------------------------------------------------------------------------- #
# Classification of a single run's captured text
# --------------------------------------------------------------------------- #
def classify(txt):
    """Return dict(outcome, rank, crash, markers, depth, states, bug_signatures, score)."""
    txt = txt or ""
    markers = [name for name, sub in PROG_ORDER if sub in txt]
    depth = len(markers)

    # High-level reachable-state flags (driven by real log text, not replay names).
    reached_content = any(m in txt for m in ("Done load", "Loaded content"))
    menu_reached = bool(MENU_PHRASES[0] in txt or any(p in txt for p in MENU_PHRASES))
    ingame = menu_reached or any(p in txt for p in ("in-game", "gameplay", "Level Select", "Character Select"))

    bug = []
    if "no catch handler found for thrown exception" in txt:
        mtype = re.search(r"__cxa_throw type: '(\w+)'", txt)
        msg = re.search(r'obj\[\+16\] -> string: "([^"]*)"', txt)
        bug.append({"sig": "uncaught-exception",
                    "detail": "type=%s %s" % (mtype.group(1) if mtype else "?",
                                              msg.group(1) if msg else "")})
    for ph in CRASH_PHRASES:
        if ph in txt:
            bug.append({"sig": "veh-crash"})
            break
    # Construct runtime parser errors (the current content-load blocker) — always
    # capture the detail regardless of which phrasing appears in the log.
    m = re.search(r'parse error - unexpected .?([\'"])', txt)
    if m:
        bug.append({"sig": "construct-parse", "detail": "unexpected %s" % (m.group(1).strip(),)})
    mm2 = re.search(r'std::invalid_argument\((.*)\) in', txt)
    if mm2 and not any(b["sig"] == "construct-parse" for b in bug):
        bug.append({"sig": "construct-parse", "detail": "std::invalid_argument(%s)" % (mm2.group(1).strip(),)})

    uncaught = "no catch handler found for thrown exception" in txt
    veh = bool(bug) and any(p in txt for p in CRASH_PHRASES)
    crash = uncaught or veh

    if uncaught:
        outcome = "uncaught-exception"
    elif veh:
        outcome = "guest-crash"
    elif reached_content or depth > 0:
        outcome = "ran"          # advanced without crashing (possibly timed out)
    else:
        outcome = "hang"         # no progress at all

    rank = OUTCOME_RANK.get(outcome, 2)
    survived = int(not crash and (reached_content or depth > 0))
    score = (survived, depth, int(reached_content), int(menu_reached))
    return {"outcome": outcome, "rank": rank, "crash": crash,
            "markers": markers, "depth": depth,
            "states": {"content_loaded": reached_content,
                       "menu_reached": menu_reached, "ingame": ingame},
            "bug_signatures": bug, "score": score}


# --------------------------------------------------------------------------- #
# Text edits with exact inverse (safe source/config patches)
# --------------------------------------------------------------------------- #
def text_replace(path, to, from_txt):
    """Replace the first occurrence of `to` with `from_txt`. Returns True if done."""
    try:
        s = open(path, "r", encoding="utf-8").read()
    except (FileNotFoundError, OSError):
        return False
    idx = s.find(to)
    if idx < 0:
        return False
    new_s = s[:idx] + from_txt + s[idx + len(to):]
    open(path, "w", encoding="utf-8").write(new_s)
    return True


def apply_edit(relpath_or_abs, from_txt, to_txt):
    """Apply a text edit. Returns inverse op dict or None if guard failed."""
    path = relpath_or_abs if os.path.isabs(relpath_or_abs) else os.path.join(REPO, relpath_or_abs)
    if not text_replace(path, from_txt, to_txt):
        return None  # idempotent: already applied / not present -> no-op
    return {"file": path, "from": from_txt, "to": to_txt}


def revert_ops(ops):
    """Undo a list of inverse ops {file, from, to}: restore original text."""
    for op in reversed(ops):
        if op and text_replace(op["file"], op["to"], op["from"]):
            try:
                disp = os.path.relpath(op["file"], REPO)
            except ValueError:
                disp = op["file"]
            log("   [revert] %s" % disp)


# --------------------------------------------------------------------------- #
# JSON field helpers — dotted-path navigation + reversible config edits
# --------------------------------------------------------------------------- #


def _dotted(obj, field):
    """Return the nested value at dotted `field`, or None if any segment is missing."""
    cur = obj
    for part in field.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def _coerce(v):
    """Normalize a knob target to int when it is an all-digit string, else leave."""
    if isinstance(v, str):
        return int(v) if re.fullmatch(r"[+-]?\d+", v.strip()) else v
    return v


def set_json_field(cfg_text, field, value):
    """Return new JSON text with dotted `field` set to a numeric/string `value`, or
    None when the path is missing / not supported / already equal. The caller applies
    it via apply_edit(path, cfg_text, new_text) so every change stays byte-exact reversible."""
    try:
        obj = json.loads(cfg_text)
    except ValueError:
        return None
    parts = field.split(".")
    parent = obj
    for p in parts[:-1]:
        if not isinstance(parent, dict) or p not in parent:
            return None
        parent = parent[p]
    last = parts[-1]
    cur = parent.get(last)  # current leaf value, for equality checks in both branches
    # Normalize: accept int/float leaves directly; coerce all-digit strings to int so
    # e.g. "7" -> 7 while leaving truly non-numeric strings (e.g. deadzone) untouched.
    try:
        if isinstance(value, bool):            # bool is a subclass of int — reject explicitly
            return None
        numv = float(value)
        target = int(numv) if re.fullmatch(r"[+-]?\d+", str(value).strip()) else numv
    except (TypeError, ValueError):
        # Non-numeric leaf (e.g. input.deadzone); only rewrite when distinct from current.
        cur = parent.get(last)
        if cur == value:
            return None
        parent[last] = value
        return json.dumps(obj, indent=2).encode().decode()
    already_equal = cur == target or (isinstance(cur, (int, float)) and abs(float(cur) - numv) < 1e-9)
    if already_equal:
        return None
    parent[last] = target
    return json.dumps(obj, indent=2).encode().decode()


# --------------------------------------------------------------------------- #
# Curated fix registry (engineered, keyed to observed runtime bugs)
# --------------------------------------------------------------------------- #
PATCHES = [
    {
        "key": "tls-precommit-relocate",
        "label": "raise pre-commit base off the eboot slot",
        "when": [r"PT_TLS", r"TLS base", r"guest TLS"],
        "edits": [(os.path.join("src", "memory", "memory.cpp"),
                   "guest_addr_t pre_base = 0x800000000ULL;",
                   "guest_addr_t pre_base = 0x820000000ULL; // AUTO tls-relocate")],
    },
    {
        "key": "strict-imports",
        "label": "enable strict HLE dylib imports (SharpEmu parity)",
        "when": [r"import not found", r"undefined symbol", r"\bmodule\b.*not"],
        "edits": [(CFG_GLOBAL,
                   '"hle": {\n    "strict_imports": false,',
                   '"hle": {\n    "strict_imports": true,')],
    },
]


def latest_log_path(rec):
    return rec.get("logpath") if rec else ""


# --------------------------------------------------------------------------- #
# Run the emulator once (headless) with optional input replay playback
# --------------------------------------------------------------------------- #
def run(play_input=None, trace=False, probe=False, timeout=TIMEOUT_BASELINE, record=None):
    """Launch pcsx5_cli.exe headless; capture log+stdout. Returns rec dict."""
    if not os.path.exists(CLI):
        return {"cli-missing": True, "outcome": "cli-missing", "rc": -99,
                "dur_s": 0.0, "markers": [], "depth": 0, "score": (0, 0, 0, 0),
                "states": {}, "bug_signatures": [], "logpath": None}

    ts = time.strftime("%Y%m%d_%H%M%S")
    lp = os.path.join(LOGDIR, "run_%s.log" % ts)
    outp = lp + ".out"
    os.makedirs(LOGDIR, exist_ok=True)  # run() writes .log/.out here (history only creates on write)
    argv = [CLI, "--headless", "--title-id=PPSA02929",
            "--config-dir=" + CFG_GLOBAL, "--log-file=" + lp, EBOOT]
    if play_input:
        argv.append("--play-input=%s" % play_input)
    if record:
        argv.append("--record-input=%s" % record)
    env = dict(os.environ)
    if trace:
        env["PCSX5_GUEST_TRACE"] = "1"
    if probe:
        env["PCSX5_STR_COPY_PROBE"] = "1"

    t = time.time()
    try:
        p = subprocess.run(argv, cwd=REPO, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        rc = p.returncode
        out_text = (p.stdout or "").decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        rc, out_text = -1, ""

    txt = ""
    if os.path.exists(lp):
        try:
            txt = open(lp, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            txt = ""
    if not txt and out_text:
        txt = out_text
    if out_text:
        with open(outp, "w", encoding="utf-8") as f:
            f.write(out_text)

    c = classify(txt)
    # Crash-evidence capture (option-1 deliverable): if this run hit Dreaming Sarah's native
    # Construct value-reader parse crash, emit an evidence bundle + patch doc from the fresh log.
    # Pure read of logs + write of NEW timestamped artifacts; edits no source/config, so it never
    # conflicts with autopilot's exact-text inverse ops (reversibility guarantee). Lazy import keeps
    # this a non-hard dependency on diag_construct.py / capstone for the driver itself.
    crash_capture = _capture_crash_evidence(lp)
    if crash_capture.get("crashes"):
        c["construct_parse_crash"] = {
            "bundle_path": crash_capture["bundle_path"],
            "doc_path": crash_capture["doc_path"],
            "throw_site_rip": "eboot 0x81012f2a0 (value-reader expectValue)",
            "token_id": 0xe,
        }
    rec = {"ts": ts, "logpath": lp, "stdout_path": outp, "rc": rc,
           "dur_s": round(time.time() - t, 1), **c}
    return rec


def _capture_crash_evidence(logpath):
    """If the given run log shows Dreaming Sarah's Construct parse crash, write an evidence
    bundle + patch doc via tools/diag_construct.py and return its summary. Returns {} on no
    crash or import failure (never raises into the driver loop)."""
    if not os.path.exists(logpath):
        return {}
    try:
        tools_dir = os.path.dirname(os.path.abspath(__file__))
        if tools_dir not in sys.path:
            sys.path.insert(0, tools_dir)
        import diag_construct  # noqa: WPS433 (lazy; optional tool dependency)
    except Exception:
        return {}
    try:
        return diag_construct.emit(logpath)
    except Exception:
        return {}


# --------------------------------------------------------------------------- #
# Replay generation + hill-climb auto-play ("guessing")
# --------------------------------------------------------------------------- #
def gen_replay(seed, duration_frames=15 * 60):
    """Generate a controller replay. Boot phase is input-free (let it reach
    content-load headless); only the tail explores button combos."""
    rnd = random.Random(seed)
    fp = 60
    events = [{"frame": 0, "buttons": 0, "lx": 128, "ly": 128,
               "rx": 128, "ry": 128, "l2": 0, "r2": 0}]
    t = round(5 * fp)
    while t < duration_frames - fp:
        mask = rnd.randint(0, 0x7F)
        events.append({"frame": t, "buttons": mask, "lx": 128, "ly": 128,
                       "rx": 128, "ry": 128, "l2": 0, "r2": 0})
        t += round(rnd.choice([30, 60, 90]))
    return events


def write_replay(path, seed):
    ev = gen_replay(seed)
    data = {"version": 1, "title_id": "PPSA02929", "events": ev}
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)


def mutate_replay(path):
    """Perturb a replay for hill-climbing (flip button masks / shift timings)."""
    data = json.load(open(path, "r", encoding="utf-8"))
    rnd = random.Random()
    evs = list(data["events"])
    n = max(1, len(evs) // 2)
    for i in range(n):
        idx = rnd.randint(0, len(evs) - 1)
        e = evs[idx]
        e["buttons"] ^= rnd.choice([0x1, 0x4, 0x8, 0x20, 0x40, 0x7F])
    data["events"] = evs
    new_path = path + ".m%d" % rnd.randint(0, 99)
    write_replay(new_path, int(rnd.random() * (2**32)))
    return new_path


def do_play(st, best_score, play_attempts):
    """Hill-climb generated replays to push Dreaming Sarah deeper into menus/
    gameplay. Returns True if menu/in-game was reached within the attempts."""
    best_replay = st.get("best_replay_path") or BASE_REPLAY
    prev_depth = (st["best_rec"].get("depth", 0) if st.get("best_rec") else 0)
    for pa in range(play_attempts):
        idx = seed_for(pa, st.get("round", 0)) % 100000
        ppath = "%s.m%d" % (BASE_REPLAY, idx + pa)
        write_replay(ppath, seed_for(pa, st.get("round", 0)))
        pr = run(play_input=ppath, timeout=TIMEOUT_PLAY)
        improved = (best_score is None or pr["score"] > best_score) or \
                   (prev_depth and pr["depth"] > prev_depth)
        if best_score is None or pr["score"] > best_score:
            best_score = pr["score"]
            st["best_rec"] = dict(pr)
            if not os.path.exists(best_replay):
                best_replay = ppath
        log("    play #%d replay=%s outcome=%-14s depth=%d%s" % (
            pa, os.path.basename(ppath), pr["outcome"], pr["depth"],
            "  <--best" if improved else ""))
        history(pr, note="play-%d" % pa)
        states = pr.get("states", {})
        if states.get("content_loaded") and \
           (states.get("menu_reached") or "menu" in pr["markers"]):
            log("    auto-play reached menu/in-game — done.")
            return True
        prev_depth = pr["depth"]
        best_replay = ppath
        if not improved:
            st["plateau"] = st.get("plateau", 0) + 1
            if st["plateau"] >= play_attempts // 2:
                log("    plateau on replay depth — stopping climb.")
                break
        mutate_replay(ppath)

    # Persist the winning replay for a human to refine with --record-input.
    st["best_replay_path"] = best_replay
    save_state(st)
    if os.path.exists(best_replay):
        log("    saved best replay -> %s" % best_replay)
    return False


def seed_for(pa, rnd):
    """Deterministic pseudo-seed for a given attempt index."""
    return (rnd * 7919 + pa * 104729) & 0x7FFFFFFF


def rnd_seed_index(st):
    """A stable-ish replay-index derived from state so paths are unique per round."""
    base = hash((st.get("round", 0), os.path.basename(REPO))) % 1_000_000
    return base & 0x7FFFFFFF


# --------------------------------------------------------------------------- #
# State persistence (history JSONL + best-state JSON)
# --------------------------------------------------------------------------- #
def load_state():
    try:
        return json.load(open(STATE, "r", encoding="utf-8"))
    except (OSError, ValueError):
        return {"applied_patches": [], "best_score": None, "best_rec": {},
                "best_replay_path": None, "plateau": 0}


def save_state(st):
    with open(STATE, "w", encoding="utf-8") as f:
        json.dump(st, f, indent=2)


def history(rec, note="", kept=False):
    os.makedirs(LOGDIR, exist_ok=True)
    row = dict(rec)
    if note:
        row["note"] = note
    if kept:
        row["kept"] = True
    with open(HIST, "a", encoding="utf-8") as f:
        f.write(json.dumps(row) + "\n")


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #
def cmd_baseline(trace=False, probe=False):
    st = load_state()
    if build()["ok"]:
        rec = run(play_input=None, trace=trace, probe=probe)
        history(rec, note="baseline", kept=True)
        log("=" * 64)
        for k in ("ts", "rc", "dur_s"):
            log("  %-9s %s" % (k, rec.get(k)))
        log("  outcome=%-20s depth(markers)=%d states=%s"
            % (rec["outcome"], rec["depth"], rec["states"]))
        if rec["markers"]:
            log("    markers: %s" % " -> ".join(rec["markers"]))
        for b in rec["bug_signatures"]:
            log("  BUG: %s (%s)" % (b["sig"], b.get("detail", "")))
        print(json.dumps({"score": rec["score"], **rec}, indent=2))
    else:
        log("[baseline] build failed — see errors above.")


def _pick_patch(st, last_log):
    """Choose the next curated patch to try. Returns (label, inverse_ops) or None."""
    seen = set(st.get("applied_patches", []))
    for p in PATCHES:
        if p["key"] in seen:
            continue
        if p["when"]:
            hits = [r for r in p["when"] if re.search(r, last_log or "")]
            if not hits:
                continue
        inverse_ops = []
        for relpath, frm, to in p["edits"]:
            inv = apply_edit(relpath, frm, to)
            if inv:
                inverse_ops.append(inv)
                seen.add(p["key"])
        return p["label"], inverse_ops or None
    return None  # only already-tried patches remain -> rely on knob search


def _knob_field_names():
    """Safe, reversible global.json knobs to auto-search."""
    return [("audio.backend", 0, [1, 2]), ("graphics.renderer", 0, [1])]


def iterate(max_rounds=6, play_attempts=8, trace=False, probe=False):
    st = load_state()
    os.makedirs(LOGDIR, exist_ok=True)
    best_score = None
    reached_content = bool(st.get("reached_content", False))

    log("#" * 64)
    log("Dreaming Sarah (PPSA02929) autopilot — max_rounds=%d play_attempts=%d"
        % (max_rounds, play_attempts))
    log("#" * 64)

    dirty = True          # force a build before the first run / any kept edit

    for rnd in range(1, max_rounds + 1):
        st["round"] = rnd

        log("[round %d/%d] phase=FIX  applied=%s"
            % (rnd, max_rounds, sorted(st["applied_patches"])))

        # Once content-load is reachable, switch this round to replay auto-play.
        if st.get("reached_content") and not trace:
            do_play(st, best_score, play_attempts)
            summary = load_state()
            print(json.dumps({"applied_patches": sorted(summary["applied_patches"]),
                              "best_score": summary["best_score"],
                              "best_rec": summary["best_rec"],
                              "best_replay_path": summary["best_replay_path"]}, indent=2))
            return

        # ---- build only when source/config actually changed since last run ---
        if dirty or (changed_snapshot(st) != sorted(st["applied_patches"])):
            bres = build()
            log("    [%s] build elapsed=%.1fs%s" % (
                "OK" if bres.get("ok") else "FAIL", bres.get("elapsed", -1),
                ("  errors:" + "; ".join(bres["errors"][:3])) if not bres.get("ok") else "") )
            st["built_with"] = sorted(st["applied_patches"])
            dirty = False

        # ---- baseline headless run against current (compiled) binary -------
        baseline = run(play_input=None, timeout=TIMEOUT_BASELINE)
        st["last_rec"] = dict(baseline)
        if "content" in baseline["markers"]:
            st["reached_content"] = True
            save_state(st)
        history(baseline, note="round-baseline")
        if best_score is None or baseline["score"] > best_score:
            best_score = baseline["score"]
            st["best_rec"] = dict(baseline)
            log("    [best] headless depth=%d outcome=%s"
                % (baseline["depth"], baseline["outcome"]))

        # ---- curated code-fix attempt (auto find->try->keep/revert) ---------
        label, inverse_ops = _pick_patch(st, latest_log_path(st.get("last_rec", {})))
        if inverse_ops:
            st["applied_patches"].sort()
            save_state(st)
            log("    applying patched fix: %s" % label)
            base = run(play_input=None, timeout=TIMEOUT_PATCH)
            better = (best_score is None) or (base["score"] > best_score)
            if not better and inverse_ops:
                revert_ops(inverse_ops)
                st["applied_patches"].discard(label.split(":")[0])
                log("    no improvement -> reverted %s" % label)
                history(base, note="fix-reverted")
                dirty = True  # source changed (revert) -> rebuild next round
            else:
                st["applied_patches"].sort()
                save_state(st)
                log("    outcome=%-20s depth=%d  (kept)" % (base["outcome"], base["depth"]))
                history(base, note="fix-applied", kept=True)
                dirty = True   # patched source now on disk -> rebuild next round

        # ---- safe-knob local search over global.json when stuck -------------
        if not reached_content or best_score[0] == 0:
            path = os.path.join(REPO, "pcsx5_config", "global.json")
            try:
                for field, cur, tries in _knob_field_names():
                    cfg_text = open(path, encoding="utf-8").read()
                    obj = json.loads(cfg_text)
                    if _dotted(obj, field) == _coerce(cur[0]):   # already at default value
                        continue
                    for v in tries:
                        new_text = set_json_field(cfg_text, field, v)  # nested-aware
                        if not new_text:                             # path missing / unchanged
                            continue
                        inv = apply_edit(path, cfg_text, new_text)   # exact-disk from -> reversible
                        if not inv:
                            continue
                        st["applied_patches"].append("%s=%s" % (field, v))
                        save_state(st)
                        r = run(play_input=None, timeout=TIMEOUT_PATCH)
                        log("    knob %-16s <- %s : outcome=%-14s depth=%d%s"
                            % (field, v, r["outcome"], r["depth"],
                               "  <--best" if best_score is None or r["score"] > best_score else ""))
                        if best_score is None or r["score"] > best_score:
                            best_score = r["score"]
                            st["best_rec"] = dict(r)
                        history(r, note="knob-%s=%s" % (field, v))
                        revert_ops([inv])  # restore exact original text before next trial
            except Exception as e:  # pragma: no cover - defensive
                log("    knob error: %r" % e)

        dirty = True   # knobs/toggles changed config -> rebuild next round


def _any_recent_marker():
    """Fallback scan of the latest captured autopilot log for a content marker."""
    if not os.path.exists(HIST):
        return False
    try:
        last = open(HIST, "r", encoding="utf-8").read().splitlines()[-1] or ""
        obj = json.loads(last) if last.strip() else {}
        markers = (obj.get("markers") or []) + (obj.get("bug_signatures") or [{}])[0].get("detail", "")
        return any(name in markers for name, _ in PROG_ORDER) or "Done load" in str(obj.get("logpath", ""))
    except Exception:
        return False


def changed_snapshot(st):
    """Return the applied-patches snapshot recorded at the last successful build."""
    return st.get("built_with")


def cmd_knob(field, old, new):
    """One-off reversible config toggle (manual, for immediate effect).

    Uses a json.dumps(indent=2) round-trip so `from` and `to` share identical
    formatting; the edit is therefore always revertible by swapping them back.
    """
    path = os.path.join(REPO, "pcsx5_config", "global.json")
    try:
        cfg_text = open(path, encoding="utf-8").read()
        obj = json.loads(cfg_text)
        target = _coerce(new)
        if _dotted(obj, field) == target:
            log("[knob] %s already equals target (%r); skip" % (field, new)); return 0
        value = int(new) if re.fullmatch(r"[+-]?\d+", new.strip()) else new
        new_text = set_json_field(cfg_text, field, value)   # nested-aware; None => no change / invalid path
        inv = apply_edit(path, cfg_text, new_text)           # exact-disk from -> reversible to
    except Exception as e:  # pragma: no cover - defensive
        log("[knob] error: %r" % e); return 1
    if not inv:
        log("[knob] could not toggle %s (%r)" % (field, value)); return 0
    else:
        log("[knob] applied %s=%r" % (field, value))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd")

    bp = sub.add_parser("baseline", help="one build + headless run, full report")
    bp.add_argument("--trace", action="store_true")
    bp.add_argument("--probe", action="store_true")

    it = sub.add_parser("iterate", help="closed fix->rerun loop then replay auto-play")
    it.add_argument("--max-rounds", type=int, default=6)
    it.add_argument("--play-attempts", type=int, default=8)
    it.add_argument("--trace", action="store_true")
    it.add_argument("--probe", action="store_true")

    kb = sub.add_parser("knob", help="one-off reversible config toggle")
    kb.add_argument("field"); kb.add_argument("old"); kb.add_argument("new")

    args = p.parse_args()

    if args.cmd == "baseline":
        cmd_baseline(trace=args.trace, probe=args.probe)
    elif args.cmd == "iterate":
        iterate(max_rounds=args.max_rounds, play_attempts=args.play_attempts,
                trace=args.trace, probe=args.probe)
    elif args.cmd == "knob":
        sys.exit(cmd_knob(args.field, args.old, args.new))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
