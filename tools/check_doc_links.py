"""Fail when a documented repository path stops resolving.

The engineering rules discharge into documents: "record it in the owning audit",
"write the task spec", "cite the walkthrough". None of `docs/tasks`,
`docs/audits`, `docs/walkthroughs` or `docs/evidence` existed, and CLAUDE.md
pointed at `docs/RUNTIME_LIFECYCLE.md` and `docs/PS5_BOOT_PIPELINE.md` while both
live under `architecture/`. Seven of the paths every session is told to read led
nowhere, so rules that depend on them were unenforceable and nobody noticed,
because nothing checked.

Usage:
    python tools/check_doc_links.py            # report and exit non-zero on a miss
    python tools/check_doc_links.py --list     # list every path checked
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files whose references are checked. Missing files are skipped rather than
# failed: CLAUDE.md and .claude/** are local tooling and are deliberately absent
# from the published repository, so a fresh clone must still pass.
SOURCES = [
    "CLAUDE.md",
    "GEMINI.md",
    "README.md",
    "TASKS.md",
    "docs/EVIDENCE_POLICY.md",
    "docs/GOALS.md",
    "architecture/README.md",
    "architecture/RUNTIME_LIFECYCLE.md",
    "architecture/PS5_BOOT_PIPELINE.md",
]

def _collect_sources():
    out = [s for s in SOURCES if os.path.exists(os.path.join(REPO, s))]
    for base in (".claude", "docs", "architecture"):
        root = os.path.join(REPO, base)
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                if f.endswith(".md"):
                    rel = os.path.relpath(os.path.join(dirpath, f), REPO)
                    rel = rel.replace(os.sep, "/")
                    if rel not in out:
                        out.append(rel)
    return out

# A repo-relative path under a directory we control. Deliberately narrow: prose
# is full of path-like text, and a checker that guesses produces noise, which is
# how checkers end up disabled.
PATH_RE = re.compile(r"(?<![\w./-])((?:docs|architecture|src|tools|tests|assets|\.claude|\.gemini)/[A-Za-z0-9_./+-]+)")

def _resolves(p):
    """True if `p` names something in the repository.

    Prose cites files by stem and rules by number: "src/gpu/gal" means gal.h and
    gal.cpp, ".gemini/rules/04" means 04-<name>.md. Those are references, not
    broken links, so an extension-less path resolves when any sibling begins with
    it. Without this the checker reports 39 false misses beside the one real
    one, and a checker that cries wolf gets switched off.
    """
    full = os.path.join(REPO, p)
    if os.path.exists(full):
        return True
    if "." in os.path.basename(p):
        return False  # an explicit filename that is simply absent
    parent = os.path.dirname(full)
    stem = os.path.basename(p)
    if not os.path.isdir(parent):
        return False
    return any(name.startswith(stem) for name in os.listdir(parent))


def _is_template(p):
    """Naming patterns, not real paths: TASK-YYYY-MM-DD-<slug>.md and friends."""
    return ("YYYY" in p or "<" in p or "*" in p or p.endswith("-"))

def main():
    listing = "--list" in sys.argv
    misses = []
    checked = 0
    for src in _collect_sources():
        try:
            text = open(os.path.join(REPO, src), encoding="utf-8").read()
        except Exception:
            continue
        for raw in set(PATH_RE.findall(text)):
            # Strip a trailing "file.cpp:1234" line reference and any trailing
            # punctuation the prose left attached.
            p = raw.split(":")[0].rstrip(".,;)`")
            if _is_template(p):
                continue
            checked += 1
            if listing:
                print("  %s -> %s" % (src, p))
            if not _resolves(p):
                misses.append((src, p))

    if misses:
        print("Documented paths that do not resolve (%d):" % len(misses))
        for src, p in sorted(set(misses)):
            print("  %-44s cited by %s" % (p, src))
        print("\nA rule that discharges into a document cannot be followed if the "
              "document's directory does not exist.")
        return 1
    print("check_doc_links: %d referenced paths, all resolve." % checked)
    return 0

if __name__ == "__main__":
    sys.exit(main())
