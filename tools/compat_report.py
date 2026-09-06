#!/usr/bin/env python3
"""Record a PCSX5 title's compatibility status in BOTH places at once:

  1. the emulator's local curated record  (compat_seed/titles/<id>.json), and
  2. the public compatibility database     (GitHub issues, one per title).

Standing rule: whenever we determine a title's status in PCSX5, reflect it in
the compatibility repo. If the title already has an issue, its status label is
updated (the status labels are mutually exclusive) and a comment records the
change; if it is not listed, a new compat-report issue is created.

The five tiers match the database's LABELS.md:
  playable | ingame | menus | boots | nothing
(the emulator also accepts perfect/intros/booting as aliases for its badge, but
 the database uses these five, so this tool speaks them.)

Requires the GitHub CLI (`gh`) authenticated for the repo. Use --dry-run to see
what would happen without touching anything.

Examples:
  python tools/compat_report.py --title PPSA02929 --name "Dreaming Sarah" \\
      --status boots --gpu "RTX 5070 Ti" \\
      --evidence "Boots to the Ratalaika intro splash, then freezes before the menu."
"""
import argparse
import json
import os
import subprocess
import sys

DEFAULT_REPO = "Abhishekrazy/Pcsx5-Game-Compatibility"

# tier -> the database's mutually-exclusive status label
STATUS_LABELS = {
    "playable": "status-playable",
    "ingame": "status-ingame",
    "in-game": "status-ingame",
    "menus": "status-menus",
    "boots": "status-boots",
    "nothing": "status-nothing",
}
ALL_STATUS_LABELS = set(STATUS_LABELS.values())
# emulator-badge aliases folded onto a database tier
ALIASES = {"perfect": "playable", "intros": "boots", "intro": "boots", "booting": "boots"}


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True)


def find_issue(repo, title_id):
    """The existing issue whose title contains the title id, or None."""
    r = gh("search", "issues", title_id, "--repo", repo, "--json", "number,title,labels", "--limit", "30")
    if r.returncode != 0:
        return None
    for it in json.loads(r.stdout or "[]"):
        if title_id.upper() in (it.get("title") or "").upper():
            return it
    return None


def report_body(a, tier):
    lines = [
        "## Game",
        f"- **Title:** {a.name}",
        f"- **Title ID:** {a.title}",
        "",
        "## Result",
        f"- **Status:** {tier.capitalize()}",
    ]
    if a.evidence:
        lines += ["", a.evidence]
    lines += [
        "",
        "## Test environment",
        f"- **PCSX5 version:** {a.version or 'dev build'}",
        "- **PS5 firmware:** N/A (HLE)",
        "- **OS / Platform:** Windows 11 x64",
        f"- **GPU:** {a.gpu or 'Not recorded'}",
        "- **CPU / RAM:** Not recorded",
        "",
        "_Reported via tools/compat_report.py from a PCSX5 test run._",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="Record a title's PCSX5 compatibility status in the emulator and the database.")
    ap.add_argument("--title", required=True, help="Title ID, e.g. PPSA02929")
    ap.add_argument("--name", required=True, help="Game name")
    ap.add_argument("--status", required=True, help="playable | ingame | menus | boots | nothing (perfect/intros aliases accepted)")
    ap.add_argument("--evidence", default="", help="One or two sentences of what was observed")
    ap.add_argument("--gpu", default="", help="GPU used for the test")
    ap.add_argument("--version", default="", help="PCSX5 version/commit")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--no-emulator", action="store_true", help="Skip writing the local curated record")
    ap.add_argument("--no-database", action="store_true", help="Skip the GitHub issue")
    ap.add_argument("--dry-run", action="store_true", help="Show what would happen, change nothing")
    a = ap.parse_args()

    tier = a.status.strip().lower()
    tier = ALIASES.get(tier, tier)
    if tier not in STATUS_LABELS:
        sys.exit(f"unknown status '{a.status}'. Use one of: playable, ingame, menus, boots, nothing (or perfect/intros).")
    label = STATUS_LABELS[tier]
    tier = "ingame" if tier == "in-game" else tier

    # 1) emulator curated record
    if not a.no_emulator:
        path = os.path.join("compat_seed", "titles", a.title + ".json")
        rec = {
            "title_id": a.title,
            "title": a.name,
            "curated_status": tier,
            "evidence": a.evidence,
            "schema": "pcsx5.curated.v1",
        }
        print(f"[emulator] {path} -> curated_status={tier}")
        if not a.dry_run:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(json.dumps(rec, indent=2) + "\n")

    # 2) database issue
    if not a.no_database:
        issue = find_issue(a.repo, a.title)
        body = report_body(a, tier)
        if issue:
            num = issue["number"]
            cur = [l["name"] for l in issue.get("labels", [])]
            remove = [l for l in cur if l in ALL_STATUS_LABELS and l != label]
            add = [label] + (["compat-report"] if "compat-report" not in cur else [])
            print(f"[database] update #{num} '{issue['title']}': +{add} -{remove}")
            if not a.dry_run:
                args = ["issue", "edit", str(num), "--repo", a.repo]
                for l in add:
                    args += ["--add-label", l]
                for l in remove:
                    args += ["--remove-label", l]
                r = gh(*args)
                if r.returncode != 0:
                    sys.exit(f"label update failed: {r.stderr.strip()}")
                gh("issue", "comment", str(num), "--repo", a.repo, "--body",
                   f"Status updated to **{tier.capitalize()}**.\n\n{body}")
                print(f"[database] https://github.com/{a.repo}/issues/{num}")
        else:
            print(f"[database] create new issue for {a.title} ({label})")
            if not a.dry_run:
                r = gh("issue", "create", "--repo", a.repo,
                       "--title", f"[{a.title}] {a.name}", "--body", body,
                       "--label", label, "--label", "compat-report")
                print("[database] " + (r.stdout.strip() or r.stderr.strip()))


if __name__ == "__main__":
    main()
