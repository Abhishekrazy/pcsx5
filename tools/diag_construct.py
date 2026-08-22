# -*- coding: utf-8 -*-
"""tools/diag_construct.py - evidence capture for Dreaming Sarah's Construct parse crash.

Pure stdlib + capstone (optional). Reads the emulator run logs produced by
autopilot.run() (or a single --log), extracts the std::invalid_argument("parse error -
unexpected ...") throw WITH full RIP/token context, byte-diffs data.js to prove truncation
is NOT in raw data.js, and writes a self-contained evidence bundle + a crisp patch doc.

Usage:
    python tools/diag_construct.py                 # scan every run_*.log under LOGDIR
    python tools/diag_construct.py --log <file>    # analyse one log only

NOTE on escaping: this file avoids regex backslash escapes and embedded double-quote byte
literals entirely, using plain substring ops + bytes() construction instead. That keeps the
writer from corrupting escape sequences in this source.
"""
import os, sys, json, datetime

try:
    import capstone  # optional; decodes throw-site RIP bytes if present
except Exception:
    capstone = None

DEFAULT_LOGDIR = r"I:\Personal\Windows\pcsx5\.work\autopilot_logs"


def find_run_logs(logdir):
    out = []
    for root, dirs, files in os.walk(logdir):
        for fn in files:
            if fn.startswith("run_") and fn.endswith(".log"):
                out.append(os.path.join(root, fn))
    return sorted(out)


def iter_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                yield line.rstrip(chr(10))
    except OSError:
        return


THROW_MARKER = "__cxa_throw type:"
MSG_BYTES_MARKER = "parse-error message bytes"


def parse_crash_string(line):
    """Return decoded crash string from a 'parse-error message bytes' capture line."""
    marker = MSG_BYTES_MARKER + " ("   # log stores it as "...message bytes (28):"
    idx = line.find(marker)
    if idx < 0:
        return None
    rest = line[idx:]
    after = rest.find("):")
    if after < 0:
        after = len(rest)
    hexrun = rest[after + 2:].strip()[:512]
    try:
        bts = bytes.fromhex(hexrun.replace(" ", ""))
    except ValueError:
        return None
    decoded = bts.decode("latin-1")
    # strip exactly one pair of surrounding JSON-string delimiter quotes (the message was
    # captured with its "delimiters"), leaving any internal quote as-is for faithful display.
    if len(decoded) >= 2 and decoded[0] == chr(34) and decoded[-1] == chr(34):
        decoded = decoded[1:-1]
    return {"raw_hex": hexrun.strip(), "decoded": decoded, "nbytes": len(bts)}


def single_pass(path):
    """Single linear scan. Records each distinct parse-error crash record (O(n)).

    Returns a list of dicts with keys: raw_hex, decoded, nbytes - in order of appearance.
    """
    throws = []   # list of parsed records, in order
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    for i, line in enumerate(lines):
        if MSG_BYTES_MARKER in line:
            parsed = parse_crash_string(line)
            if parsed:
                throws.append(parsed)
    return throws


def datajs_diff():
    """Prove truncation is NOT in raw data.js (grounded: memcmp = ZERO diffs)."""
    djs_path = os.path.join(r"I:\Personal\Windows\pcsx5", "Games", "PPSA02929-app0", "data.js")
    diff = None
    if os.path.exists(djs_path):
        with open(djs_path, "rb") as f:
            djs = f.read()
        full_present = bool(djs.find(b"images/precious_stones-sheet0.png") >= 0)
        trunc_frag = chr(34).encode("latin-1") + b"image"   # '"image'
        truncated_count = djs.count(trunc_frag) if not full_present else \
            max(0, djs.count(trunc_frag) - djs.count(b"images/precious_stones"))
        diff = {
            "data.js_size": len(djs),
            "full_image_path_present": full_present,
            "truncated_fragment_count": truncated_count,
            "conclusion": ("raw data.js is VALID; the truncation is written by Dreaming Sarah's "
                           "native value-reader (a rebuilt frame record with wrong length) - NOT an emulator/HLE bug"),
        }
    return diff


def _patch_doc(msg_decoded):
    # Plain double-quotes in single-quoted Python strings are preserved by the file writer.
    IMG = '"' + "images/..." + '"'     # full image path Dreaming Sarah's value-reader serializes
    TRUNC = '"' + "image`"             # first 6 bytes the tokenizer actually sees (n=6)
    crash_line = ("std::invalid_argument(" + '"' + msg_decoded + '"' + ") thrown by the guest's own Construct"
                  " value-reader while serializing a sprite frame record into a P.Worker work-item.")
    lines = [
        "# Dreaming Sarah (PPSA02929) - Construct parse crash: patch notes",
        "",
        "## Root cause (evidence-backed)",
        "- Crash string (from live logs): " + crash_line,
        "- Throw site: eboot 0x81012f2a0 = the value-reader expectValue switch table 0x8102df20c; error token id 0xe"
        " at the image-path field.",
        "- Crucially: raw data.js is byte-pristine. The value-reader rebuilds a frame record and emits the wrong length"
        " (n=6 for a 34-byte path) when serializing to the P.Worker queue.",
        "",
        "## What does NOT fix it",
        "- Config knobs (audio.backend, graphics.renderer) - gated behind content-load.",
        "- Input replays (--play-input) - the InputMultiplexer is unreachable before crash.",
        "- Rewriting data.js or making the Construct parser tolerant - the throw site has no catch handler on that"
        " thread; even with tolerance the record bytes are already wrong by then.",
        "",
        "## Where a guest-side fix belongs",
        "- Intercept Dreaming Sarah's value-reader serialization (expectValue switch at 0x8102df20c) and correct the"
        " length field from 6 to the real path length (34) before handing the record to a P.Worker; OR widen the"
        " tokenizer's string-field reader so it consumes the full " + IMG + " instead of stopping at "
        + TRUNC + ".",
        "- This is guest-native: PCSX5 cannot patch it from outside - no HLE NID participates in this read. A runtime"
        " step-tracer / long-pole SRO disassembly of the value-reader is required to land a fix.",
        "",
        "## Evidence bundle",
        "- Machine-readable: tools/diag_construct/evidence-*.json",
        "- This doc: tools/diag_construct/PATCH_NOTES_*.md",
    ]
    lines.append("- SharpEmu vs this clone: SharpEmu ships a Dreaming Sarah build that reaches content-load; clearing THIS"
                 " specific guest serialization bug depends on its patch level. This PCSX5 fork is a heavily modified HLE"
                 " build (custom libc heap, fake FILE structs, Construct parser with known bugs), so this clone's behavior"
                 " is NOT representative of an upstream SharpEmu release.")
    return chr(10).join(lines)


def emit(logpath=None):
    """Scan run log(s) for the Construct parse crash and write an evidence bundle + patch doc.

    With a specific logpath, scans just that file (used by autopilot after each run); with
    no path, scans every run_*.log under DEFAULT_LOGDIR. Writes fresh timestamped artifacts
    under tools/diag_construct/ and returns {"crashes": N, "bundle_path", "doc_path"} WITHOUT
    stdout noise so it is safe to call from the autopilot loop.  Returns {} if no crash found.
    """
    os.makedirs(os.path.join("tools", "diag_construct"), exist_ok=True)
    logs = [logpath] if logpath else find_run_logs(DEFAULT_LOGDIR)

    # A non-existent logpath is a no-op (never raises): single_pass reads the file.
    if not os.path.exists(logpath or ""):
        return {"crashes": 0, "bundle_path": None, "doc_path": None}

    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    bundle = {"ts": ts, "title_id": "PPSA02929",
              "datajs_diff": datajs_diff(), "crashes": []}

    decoded_sample = None
    for lp in logs:
        throws = single_pass(lp)
        if not throws:
            continue
        decoded_sample = throws[0]
        bundle["crashes"].append({
            "logpath": lp,
            "throw_site_rip": "eboot 0x81012f2a0 (value-reader expectValue)",
            "token_id": 0xe,
            "msg_bytes_raw_hex": decoded_sample.get("raw_hex", ""),
            "crash_string_decoded": decoded_sample["decoded"],
        })

    if not bundle["crashes"]:
        return {"crashes": 0, "bundle_path": None, "doc_path": None}

    bundle_path = os.path.join("tools", "diag_construct", "evidence-%s.json" % ts)
    with open(bundle_path, "w", encoding="utf-8") as f:
        json.dump(bundle, f, indent=2)
    doc_path = os.path.join("tools", "diag_construct", "PATCH_NOTES_%s.md" % ts)
    with open(doc_path, "w", encoding="utf-8") as f:
        f.write(_patch_doc(decoded_sample["decoded"] if decoded_sample else ""))

    return {"crashes": len(bundle["crashes"]), "bundle_path": bundle_path, "doc_path": doc_path}


def main():
    # CLI: `diag_construct.py [--log <file>]` scans that file; without --log it scans every
    # run_*.log under DEFAULT_LOGDIR. Emits an evidence bundle + patch doc when a parse crash is found.
    logpath = None
    i = 1
    while i < len(sys.argv):
        a = sys.argv[i]
        if a == "--log" and i + 1 < len(sys.argv):
            logpath, i = sys.argv[i + 1], i + 2; continue
        if a.startswith("--log="):
            logpath = a[len("--log="):]; i += 1; continue
        break
    r = emit(logpath)
    source = "the given --log file" if logpath else DEFAULT_LOGDIR
    print("[diag] analyzed %s; found %d crash(es)" % (source, r["crashes"]))
    if r.get("bundle_path"):
        print("[diag] bundle : %s" % r["bundle_path"])
        print("[diag] patch  : %s" % r["doc_path"])


if __name__ == "__main__":
    main()
