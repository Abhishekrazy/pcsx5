"""Cross-check NID strings registered in the HLE against the NID database.

A NID is a hash of a symbol name, so `RegisterSymbol("libkernel", "<nid>", H)`
asserts that `<nid>` means whatever `H` implements.  When those two disagree
nothing fails to build and nothing fails at load: the guest simply calls one
function and reaches another.  That is invisible until the wrong function
misbehaves somewhere unrelated.

This happened with aI+OeCz8xrQ (scePthreadSelf), which was registered to the
pthread-attribute initialiser.  scePthreadSelf takes no arguments, so the
attribute initialiser read RDI as an out-pointer and wrote to whatever it
happened to hold -- in one case a live C++ object's vtable pointer, which
turned the next virtual call on that object into a jump to address zero.

The check is deliberately loose: it compares the *stem* of the database name
against the handler identifier, so PthreadSelfImpl matches scePthreadSelf but
PthreadAttrInitImpl does not.  It reports rather than guesses, and anything it
cannot judge is left alone.

Usage:
    python tools/check_nid_registrations.py            # report
    python tools/check_nid_registrations.py --strict   # exit 1 on mismatch
"""
import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NID_DB = os.path.join(REPO, "assets", "nid_db.txt")
HLE_DIR = os.path.join(REPO, "src", "hle")

# RegisterSymbol("<module>", "<11-char NID>[#tag]", <Handler>)
REGISTRATION = re.compile(
    r'RegisterSymbol\(\s*"[^"]*"\s*,\s*"([A-Za-z0-9+_-]{11})(#[^"]*)?"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)')

# Handler identifiers that dispatch on something other than the symbol itself,
# where a name comparison says nothing useful.
GENERIC_HANDLERS = {"StubHandler", "UnimplementedStub", "NotImplemented"}

# Reviewed and intentional: a NID deliberately served by a related function.
# Keyed by NID so that re-pointing it at something else is still reported.
ACCEPTED = {
    # The "Named" variant takes an extra name argument that this implementation
    # ignores; both map direct memory and share one handler on purpose.
    "NcaWUxfMNIQ": "SceKernelMapDirectMemory",
}


def load_nid_db():
    names = {}
    if not os.path.exists(NID_DB):
        return names
    with open(NID_DB, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                names[parts[0].strip()] = parts[2].strip()
    return names


def stem(identifier):
    """Reduce a name to comparable letters: scePthreadSelf -> pthreadself."""
    s = identifier
    for prefix in ("sce", "Sce", "_"):
        if s.startswith(prefix):
            s = s[len(prefix):]
    for suffix in ("Impl", "Handler", "Func"):
        if s.endswith(suffix):
            s = s[: -len(suffix)]
    return re.sub(r"[^a-z0-9]", "", s.lower())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero when a registration disagrees with the database")
    args = ap.parse_args()

    db = load_nid_db()
    if not db:
        print("nid_db.txt not found or empty; nothing to check")
        return 0

    checked = mismatches = unknown = 0
    problems = []
    # Stems of every canonical name, so a handler can be recognised as being
    # named after some other real symbol.
    db_stems = {}
    for canonical_name in db.values():
        db_stems.setdefault(stem(canonical_name), canonical_name)

    for name in sorted(os.listdir(HLE_DIR)):
        if not name.endswith(".cpp"):
            continue
        path = os.path.join(HLE_DIR, name)
        with open(path, encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, 1):
                m = REGISTRATION.search(line)
                if not m:
                    continue
                nid, _tag, handler = m.group(1), m.group(2), m.group(3)
                if handler in GENERIC_HANDLERS:
                    continue
                canonical = db.get(nid)
                if canonical is None:
                    unknown += 1
                    continue
                checked += 1
                cs, hs = stem(canonical), stem(handler)
                if not cs or not hs:
                    continue
                if cs in hs or hs in cs:
                    continue
                # Several NIDs legitimately share one handler (TrivialOkImpl,
                # PadNoop0, and the Cx/Sh/Uc register variants), and those
                # handler names match no real symbol.  The signal worth acting
                # on is narrower: a handler named after a DIFFERENT symbol that
                # the database also knows.  That is the shape of the
                # scePthreadSelf/PthreadAttrInit swap.
                if hs not in db_stems or hs == cs:
                    continue
                if ACCEPTED.get(nid) == handler:
                    continue
                mismatches += 1
                problems.append((name, lineno, nid, canonical, handler, db_stems[hs]))

    print("NID registration cross-check")
    print("  registrations checked against the database : %d" % checked)
    print("  NIDs not present in the database           : %d" % unknown)
    print("  disagreements                              : %d" % mismatches)

    if problems:
        print("\nA registration claims a NID means one thing while the database says another.")
        print("One of the two is wrong; the guest reaches whichever the registration names.\n")
        for fname, lineno, nid, canonical, handler, looks_like in problems:
            print("  src/hle/%s:%d" % (fname, lineno))
            print("      %-14s database says: %s" % (nid, canonical))
            print("      %-14s registered to: %s, which is named after %s"
                  % ("", handler, looks_like))

    if mismatches and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
