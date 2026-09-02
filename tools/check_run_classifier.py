"""Lock the run classifier against the runs already stored on disk.

The classifier decides whether a title is progressing, and "progressing" is the
one verdict this project treats as success. Its thresholds are four numbers. The
failure mode being guarded against is not a bug -- it is an edit: loosening a
threshold to turn a red run green, which Rule 07 forbids and which nothing would
otherwise detect, because the classifier has no test and its verdicts are only
ever compared against themselves.

Two things are checked.

1. Records that are plainly stuck must not classify as progressing. The corpus
   contains real examples judged by an older classifier that said they were:
   PPSA21564_20260901_005357_r2 has 3 distinct images across 29 samples and sits
   frozen for 42.3s of a 60.3s run, and is stored as "progressing".

2. Every threshold must be load-bearing. Loosening one, on its own, must change
   at least one verdict somewhere in the corpus. A threshold that can be relaxed
   without changing any stored verdict is not protecting anything, and a test
   that passes either way would not notice it being edited.

Usage:
    python tools/check_run_classifier.py          # exit non-zero on failure
    python tools/check_run_classifier.py --list   # per-record verdicts
"""
import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "game_runner"))

import session  # noqa: E402

# Runs whose stored verdict is wrong and must not be reproduced. Each is a real
# archived run; the numbers are from its own record.
MUST_NOT_PROGRESS = {
    "PPSA21564_20260901_005357_r2":
        "3 distinct frames in 29 samples, frozen 42.3s of 60.3s",
}


def _records():
    for path in sorted(glob.glob(os.path.join(REPO, "artifacts", "runtime",
                                              "*", "record.json"))):
        try:
            with open(path, encoding="utf-8") as fh:
                rec = json.load(fh)
        except Exception:
            continue
        if rec.get("frames"):
            yield os.path.basename(os.path.dirname(path)), rec


def _replay(rec, **overrides):
    """Re-decide a stored run with the current classifier."""
    ex = rec.get("execution", {})
    failures = ex.get("capture_failures", 0) or 0
    saved = {}
    for name, value in overrides.items():
        saved[name] = getattr(session, name)
        setattr(session, name, value)
    try:
        progressing, _coverage = session.classify_progression(
            rec["frames"],
            ex.get("longest_freeze_s", 0.0) or 0.0,
            rec.get("duration_s", 0.0) or 0.0,
            failures)
    finally:
        for name, value in saved.items():
            setattr(session, name, value)
    return progressing


def main():
    listing = "--list" in sys.argv
    corpus = list(_records())
    if not corpus:
        print("check_run_classifier: no stored records found; nothing to lock against.")
        return 0

    failures = []

    # 1. Known-stuck runs must not be called progressing.
    seen = set()
    for run_id, rec in corpus:
        verdict = _replay(rec)
        if listing:
            print("  %-38s progressing=%s (stored %s)"
                  % (run_id, verdict, rec.get("execution", {}).get("progressing")))
        if run_id in MUST_NOT_PROGRESS:
            seen.add(run_id)
            if verdict:
                failures.append("%s classifies as progressing, but it is %s"
                                % (run_id, MUST_NOT_PROGRESS[run_id]))
    for run_id in MUST_NOT_PROGRESS:
        if run_id not in seen:
            print("  note: fixture %s is not present in artifacts/runtime; skipped"
                  % run_id)

    # 2. Each threshold must change a verdict when loosened.
    base = [(_replay(rec), run_id) for run_id, rec in corpus]
    loosenings = {
        "FREEZE_FRACTION_MAX": 1.01,   # allow a freeze covering the whole run
        "DISTINCT_RATIO_MIN": 0.0,     # allow every frame to be identical
    }
    # COVERAGE_MIN is deliberately not checked against the corpus: every
    # archived record predates capture_failures, so all of them replay at 100%
    # coverage and the threshold cannot bite on any of them. Asserting it there
    # would fail for a reason that says nothing about the threshold. It gets a
    # synthetic fixture below instead, which is the honest way to cover a guard
    # no stored run exercises.
    corpus = list(corpus)
    for name, loosened in loosenings.items():
        changed = sum(1 for (was, _), (run_id, rec) in zip(base, corpus)
                      if _replay(rec, **{name: loosened}) != was)
        if listing:
            print("  loosening %-22s changes %d verdict(s)" % (name, changed))
        if changed == 0:
            failures.append(
                "%s can be loosened to %s without changing any stored verdict, "
                "so it is not protecting anything" % (name, loosened))

    # 3. The coverage guard, on a constructed run: every other condition passes,
    #    and only the missing captures should reject it. Without this the guard
    #    added after a harness defect -- refused captures biasing the verdict
    #    toward progressing -- would have no test at all.
    good_frames = [{"hash": "h%d" % i, "change": 0.9} for i in range(10)]
    lively = {"frames": good_frames, "duration_s": 60.0,
              "execution": {"longest_freeze_s": 1.0, "capture_failures": 0}}
    starved = {"frames": good_frames, "duration_s": 60.0,
               "execution": {"longest_freeze_s": 1.0, "capture_failures": 30}}
    if not _replay(lively):
        failures.append("a run with distinct frames, no freeze and full coverage "
                        "should classify as progressing; it does not")
    if _replay(starved):
        failures.append("a run with 10 captures and 30 refusals (25% coverage) "
                        "classifies as progressing; the coverage guard is inert")
    if not _replay(starved, COVERAGE_MIN=0.0):
        failures.append("with COVERAGE_MIN removed the starved run still fails, "
                        "so coverage is not what rejected it")

    if failures:
        print("check_run_classifier: %d problem(s) across %d stored runs:"
              % (len(failures), len(corpus)))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("check_run_classifier: %d stored runs replayed; known-stuck runs "
          "rejected and all thresholds load-bearing." % len(corpus))
    return 0


if __name__ == "__main__":
    sys.exit(main())
