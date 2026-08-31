"""Recover symbol names for unknown NIDs by search.

A PS5 NID is SHA1(name + fixed suffix) truncated to 8 bytes and encoded in
Sony's base64 alphabet.  The hash is one-way, so a name cannot be derived from a
NID -- but it can be *guessed and checked*, and a check is conclusive: if
hashing a candidate reproduces the NID exactly, that is the name.  There is no
judgement call and no trusting an outside claim, which is what makes searching
worthwhile here.

The search is only as good as its candidate list, so candidates come from
places real symbol names live:

  1. names already in assets/nid_db.txt
  2. names appearing in any reference source trees present locally
  3. systematic variants of those names (Get/Set swaps, common suffixes)
  4. recombination of the CamelCase tokens those names are built from, since
     PS5 exports are highly compositional -- sceAgcDcbSetUcRegisterDirectGetSize
     is just library + object + verb + noun + qualifier + size-query

Usage:
    python tools/resolve_nids.py --from-logs
    python tools/resolve_nids.py --nids aI+OeCz8xrQ,wtkt-teR1so
    python tools/resolve_nids.py --from-logs --deep
    python tools/resolve_nids.py --from-logs --deep --append

--append writes verified rows into assets/nid_db.txt.  Only verified rows are
ever written; a guess that does not hash is discarded, never recorded.
"""
import argparse
import glob
import hashlib
import itertools
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NID_DB = os.path.join(REPO, "assets", "nid_db.txt")

SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

# Suffixes and variants that occur in real PS5 export names.
NAME_SUFFIXES = ["", "A", "W", "2", "Ex", "Internal", "ForLib", "ForToolkit",
                 "GetSize", "Async", "Sync", "Callback", "Impl", "Np", "V2"]


def nid_of(name):
    digest = hashlib.sha1(name.encode("utf-8") + SUFFIX).digest()
    value = struct.unpack("<Q", digest[:8])[0] << 2      # 64 bits -> 11 * 6
    return "".join(ALPHABET[(value >> (60 - 6 * i)) & 0x3F] for i in range(11))


def self_test():
    """Refuse to run if the hash does not reproduce known-good pairs."""
    controls = [("9UK1vLZQft4", "scePthreadMutexLock"),
                ("kDh-NfxgMtE", "scePthreadCondSignal"),
                ("mqdNorrB+gI", "scePthreadRwlockWrlock")]
    bad = [(n, e) for e, n in ((nid_of(nm), nm) for _, nm in controls)
           if dict((nm, e) for e, nm in controls).get(n) not in (None, e)]
    for expect, name in controls:
        if nid_of(name) != expect:
            print("self-test FAILED: %s should hash to %s, got %s"
                  % (name, expect, nid_of(name)))
            return False
    return True


def load_db():
    rows = {}
    if not os.path.exists(NID_DB):
        return rows
    with open(NID_DB, encoding="utf-8") as fh:
        for line in fh:
            if line.strip() and not line.lstrip().startswith("#"):
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 3:
                    rows[parts[0].strip()] = (parts[1].strip(), parts[2].strip())
    return rows


def unknown_from_logs():
    """NIDs the runtime could not resolve, across every recorded run."""
    found = set()
    pattern = re.compile(r"Unresolved NID '([A-Za-z0-9+_-]{11})")
    for log in glob.glob(os.path.join(REPO, "artifacts", "runtime", "*", "run.log")):
        try:
            with open(log, encoding="utf-8", errors="replace") as fh:
                found.update(pattern.findall(fh.read()))
        except OSError:
            continue
    return found


def harvest_names():
    """Symbol-shaped identifiers from the database and any local reference trees."""
    names = set(name for _, name in load_db().values())

    sources = [
        (os.path.join(REPO, "kyty_clone", "src"), (".cpp", ".h")),
        (os.path.join(REPO, "sharpemu_clone", "src"), (".cs",)),
    ]
    ident = re.compile(r'\b(sce[A-Z][A-Za-z0-9_]{3,60})\b')
    for root, exts in sources:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, files in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in (".git", "obj", "bin")]
            for f in files:
                if f.endswith(exts):
                    try:
                        with open(os.path.join(dirpath, f), encoding="utf-8",
                                  errors="replace") as fh:
                            names.update(ident.findall(fh.read()))
                    except OSError:
                        continue
    return names


def variants(names):
    """Systematic near-misses of known names."""
    out = set()
    for n in names:
        for suf in NAME_SUFFIXES:
            out.add(n + suf)
        if n.endswith("GetSize"):
            out.add(n[: -len("GetSize")])
        # Get/Set are the most common single-token difference between exports.
        if "Get" in n:
            out.add(n.replace("Get", "Set", 1))
        if "Set" in n:
            out.add(n.replace("Set", "Get", 1))
        for a, b in (("Initialize", "Terminate"), ("Init", "Term"),
                     ("Create", "Destroy"), ("Open", "Close"),
                     ("Start", "Stop"), ("Add", "Remove"),
                     ("Register", "Unregister"), ("Enable", "Disable")):
            if a in n:
                out.add(n.replace(a, b, 1))
            if b in n:
                out.add(n.replace(b, a, 1))
    return out


TOKEN = re.compile(r"[A-Z][a-z0-9]*|[a-z0-9]+")


def recombined(names, cap):
    """Rebuild names from the CamelCase tokens the corpus is made of.

    PS5 exports are compositional, so a name we have never seen is often a new
    arrangement of tokens we have seen many times.  Grouped by library prefix so
    the search stays inside one API's vocabulary instead of mixing unrelated
    ones.
    """
    by_prefix = {}
    for n in names:
        m = re.match(r"(sce[A-Z][a-z0-9]*)(.*)", n)
        if not m or not m.group(2):
            continue
        by_prefix.setdefault(m.group(1), set()).update(TOKEN.findall(m.group(2)))

    out = set()
    for prefix, tokens in by_prefix.items():
        toks = sorted(t for t in tokens if t[:1].isupper())
        if len(toks) < 2:
            continue
        for a, b in itertools.permutations(toks, 2):
            out.add(prefix + a + b)
            if len(out) >= cap:
                return out
        for a, b, c in itertools.islice(itertools.permutations(toks, 3), cap):
            out.add(prefix + a + b + c)
            if len(out) >= cap:
                return out
    return out


def search(targets, candidates, found):
    hits = 0
    for name in candidates:
        if name in found.values():
            continue
        n = nid_of(name)
        if n in targets and n not in found:
            found[n] = name
            hits += 1
    return hits


def module_for(name, db):
    """Reuse the database's own library naming rather than inventing one."""
    best, best_len = "libkernel", 0
    for mod, existing in db.values():
        i = 0
        while i < min(len(name), len(existing)) and name[i] == existing[i]:
            i += 1
        if i > best_len:
            best, best_len = mod, i
    return best if best_len >= 6 else "libkernel"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from-logs", action="store_true",
                    help="take unknown NIDs from recorded run logs")
    ap.add_argument("--nids", help="comma-separated NIDs to resolve")
    ap.add_argument("--deep", action="store_true",
                    help="also search token recombinations (slower)")
    ap.add_argument("--cap", type=int, default=4000000,
                    help="maximum recombined candidates (default 4000000)")
    ap.add_argument("--append", action="store_true",
                    help="append verified rows to assets/nid_db.txt")
    args = ap.parse_args()

    if not self_test():
        return 2

    db = load_db()
    targets = set()
    if args.from_logs:
        targets |= unknown_from_logs()
    if args.nids:
        targets |= {n.strip().split("#")[0] for n in args.nids.split(",") if n.strip()}
    if not targets:
        print("no NIDs given; use --from-logs or --nids")
        return 1

    already = {n for n in targets if n in db}
    targets -= already
    print("targets: %d unknown (%d of the requested set are already known)"
          % (len(targets), len(already)))
    if not targets:
        return 0

    found = {}
    corpus = harvest_names()
    print("corpus: %d symbol-shaped names" % len(corpus))

    print("  pass 1 - names as they appear ...", end="", flush=True)
    print(" %d resolved" % search(targets, corpus, found))

    print("  pass 2 - systematic variants ...", end="", flush=True)
    print(" %d resolved" % search(targets, variants(corpus), found))

    if args.deep:
        print("  pass 3 - token recombination (cap %d) ..." % args.cap, end="", flush=True)
        cands = recombined(corpus, args.cap)
        print(" %d candidates," % len(cands), end="", flush=True)
        print(" %d resolved" % search(targets, cands, found))

    print("\nresolved %d of %d unknown NIDs" % (len(found), len(targets)))
    if found:
        print()
        rows = []
        for n, name in sorted(found.items(), key=lambda kv: kv[1]):
            mod = module_for(name, db)
            rows.append((n, mod, name))
            print("  %s\t%s\t%s" % (n, mod, name))
        if args.append:
            with open(NID_DB, "a", encoding="utf-8", newline="\n") as fh:
                fh.write("\n# Recovered by tools/resolve_nids.py. Each row was checked by\n")
                fh.write("# recomputing the NID from the name; a candidate that did not\n")
                fh.write("# reproduce its NID was discarded rather than recorded.\n")
                for n, mod, name in rows:
                    fh.write("%s\t%s\t%s\n" % (n, mod, name))
            print("\nappended %d verified rows to assets/nid_db.txt" % len(rows))
        else:
            print("\n(--append writes these into assets/nid_db.txt)")

    unresolved = len(targets) - len(found)
    if unresolved:
        print("\n%d NIDs remain unknown. They name symbols whose text is not in the\n"
              "corpus and is not reachable by recombining it -- a wordlist of real\n"
              "PS5 export names is what would move this number." % unresolved)
    return 0


if __name__ == "__main__":
    sys.exit(main())
