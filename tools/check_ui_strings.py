#!/usr/bin/env python
"""
check_ui_strings.py -- localization, accessibility and theming coverage report
for the WPF shell in src/ui_csharp/.

Three things rot silently in a desktop UI and none of them break a build:

  * a new control ships with an English string baked into the XAML, so it can
    never be translated even though ten locale files exist;
  * a new control ships with no AutomationProperties.Name, so a screen reader
    announces nothing;
  * a new brush is written as StaticResource or a literal colour, so it can
    never follow a theme.

This reports all three, plus locale key parity, so a UI change can be measured
against the state before it.

    python tools/check_ui_strings.py                # human-readable report
    python tools/check_ui_strings.py --json         # machine-readable
    python tools/check_ui_strings.py --baseline tests/ui_baseline.json
                                                    # fail if anything got worse

Exit code 0 = report produced (or no regression against a baseline),
1 = regression against the baseline.
"""
import argparse
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_DIR = os.path.join(REPO, "src", "ui_csharp")
LANG_DIR = os.path.join(REPO, "assets", "lang")
BASE_LOCALE = "en-US"

# Attributes whose values are shown to a user and therefore must be localizable.
USER_VISIBLE_ATTRS = ("Content", "Header", "Text", "ToolTip", "Title", "Watermark")

# Controls a keyboard or screen-reader user must be able to identify.
INTERACTIVE_TAGS = (
    "Button", "ToggleButton", "RepeatButton", "CheckBox", "RadioButton",
    "ComboBox", "ComboBoxItem", "TextBox", "PasswordBox", "Slider",
    "ListBox", "ListBoxItem", "ListView", "TabItem", "MenuItem", "Hyperlink",
)

ELEMENT_RE = re.compile(r"<([A-Za-z_][\w.]*)((?:\s+[^<>]*?)?)/?>", re.DOTALL)
ATTR_RE = re.compile(r'([\w.:]+)\s*=\s*"([^"]*)"', re.DOTALL)

# A value that is a binding/resource reference is not a hardcoded string.
# {loc:Tr Key=...} is the project's localization markup extension; a value
# using it is localized, not hardcoded.
BINDING_RE = re.compile(r"^\s*\{\s*(Binding|DynamicResource|StaticResource|x:Static|TemplateBinding|loc:Tr)")
# Values that are not really prose: numbers, single glyphs, icon code points.
NON_PROSE_RE = re.compile(r"^[\s\d\W_]*$")


def xaml_files():
    return sorted(glob.glob(os.path.join(UI_DIR, "*.xaml")))


def is_prose(value):
    if not value.strip():
        return False
    if BINDING_RE.match(value):
        return False
    if NON_PROSE_RE.match(value):
        return False
    # Require at least one run of two letters -- filters out "OK"-like glyphs? no,
    # keep those; filters out things like "#FF00AA" and "24".
    return re.search(r"[A-Za-z]{2}", value) is not None


def scan_xaml():
    hardcoded = []
    interactive_total = 0
    interactive_named = 0
    automation_props = 0
    dynamic_resources = 0
    static_resources = 0

    for path in xaml_files():
        rel = os.path.relpath(path, REPO).replace("\\", "/")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        automation_props += text.count("AutomationProperties")
        dynamic_resources += text.count("{DynamicResource")
        static_resources += text.count("{StaticResource")

        # Line number for an offset, computed once per file.
        line_starts = [0]
        for m in re.finditer("\n", text):
            line_starts.append(m.end())

        def line_of(pos):
            lo, hi = 0, len(line_starts) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if line_starts[mid] <= pos:
                    lo = mid
                else:
                    hi = mid - 1
            return lo + 1

        for m in ELEMENT_RE.finditer(text):
            tag, attr_blob = m.group(1), m.group(2) or ""
            attrs = dict(ATTR_RE.findall(attr_blob))

            if tag in INTERACTIVE_TAGS:
                interactive_total += 1
                if "AutomationProperties.Name" in attrs:
                    interactive_named += 1

            for attr in USER_VISIBLE_ATTRS:
                if attr in attrs and is_prose(attrs[attr]):
                    hardcoded.append({
                        "file": rel,
                        "line": line_of(m.start()),
                        "element": tag,
                        "attribute": attr,
                        "value": attrs[attr][:80],
                    })

    return {
        "hardcoded_strings": hardcoded,
        "hardcoded_count": len(hardcoded),
        "interactive_controls": interactive_total,
        "interactive_with_automation_name": interactive_named,
        "automation_properties_total": automation_props,
        "dynamic_resource_refs": dynamic_resources,
        "static_resource_refs": static_resources,
    }


def scan_locales():
    base_path = os.path.join(LANG_DIR, BASE_LOCALE + ".json")
    if not os.path.isfile(base_path):
        return {"error": "base locale not found: %s" % base_path, "locales": {}}
    with open(base_path, encoding="utf-8") as f:
        base = json.load(f)

    locales = {}
    for path in sorted(glob.glob(os.path.join(LANG_DIR, "*.json"))):
        name = os.path.splitext(os.path.basename(path))[0]
        if name == BASE_LOCALE:
            continue
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        identical = sorted(k for k in d if k in base and d[k] == base[k])
        locales[name] = {
            "keys": len(d),
            "missing": sorted(set(base) - set(d)),
            "extra": sorted(set(d) - set(base)),
            # Identical to English is a *signal*, not a defect: proper nouns and
            # brand names legitimately match.  It is a list to review, not a
            # count to drive to zero.
            "identical_to_base": len(identical),
            "identical_keys": identical[:20],
        }
    return {"base_locale": BASE_LOCALE, "base_keys": len(base), "locales": locales}


def scan_code_automation_names():
    """Count AutomationProperties.SetName(...) calls in C#.

    Names that must be localized have to be assigned in code, so a XAML-only
    scan under-reports screen-reader coverage.  Reporting both keeps the picture
    honest without pretending a code-set name is a XAML attribute."""
    calls = 0
    for path in glob.glob(os.path.join(UI_DIR, "*.cs")):
        with open(path, encoding="utf-8") as f:
            calls += len(re.findall(r"AutomationProperties\.SetName\s*\(", f.read()))
    return calls


def scan_i18n_usage():
    calls = 0
    for path in glob.glob(os.path.join(UI_DIR, "*.cs")):
        with open(path, encoding="utf-8") as f:
            calls += len(re.findall(r"\bI18n\.", f.read()))
    return calls


def build_report():
    xaml = scan_xaml()
    loc = scan_locales()
    return {
        "schema": "pcsx5-ui-coverage/1",
        "xaml": xaml,
        "localization": loc,
        "i18n_call_sites": scan_i18n_usage(),
        "code_automation_names": scan_code_automation_names(),
    }


def print_report(r):
    x = r["xaml"]
    loc = r["localization"]
    print("=== XAML coverage (src/ui_csharp/*.xaml) ===")
    print("  hardcoded user-visible strings   %d" % x["hardcoded_count"])
    named, total = x["interactive_with_automation_name"], x["interactive_controls"]
    pct = (100.0 * named / total) if total else 0.0
    print("  interactive controls             %d" % total)
    print("  ...with AutomationProperties.Name %d (%.1f%%)" % (named, pct))
    print("  AutomationProperties references  %d" % x["automation_properties_total"])
    print("  ...set from C# code              %d" % r.get("code_automation_names", 0))
    print("  DynamicResource refs (themeable) %d" % x["dynamic_resource_refs"])
    print("  StaticResource refs (fixed)      %d" % x["static_resource_refs"])
    print("")
    print("=== Localization (assets/lang/) ===")
    if "error" in loc:
        print("  " + loc["error"])
    else:
        print("  base %s: %d keys | I18n. call sites in C#: %d"
              % (loc["base_locale"], loc["base_keys"], r["i18n_call_sites"]))
        for name, d in sorted(loc["locales"].items()):
            flag = ""
            if d["missing"] or d["extra"]:
                flag = "  <-- KEY MISMATCH"
            print("  %-8s keys=%-4d missing=%-3d extra=%-3d identical-to-base=%-3d%s"
                  % (name, d["keys"], len(d["missing"]), len(d["extra"]),
                     d["identical_to_base"], flag))
    print("")
    if x["hardcoded_count"]:
        print("First 15 hardcoded strings (these can never be translated):")
        for h in x["hardcoded_strings"][:15]:
            print("  %s:%d  <%s %s=\"%s\">"
                  % (h["file"], h["line"], h["element"], h["attribute"], h["value"]))


def compare_baseline(report, baseline_path):
    """Fail only on a *worsening*.  This is a ratchet, not a gate to be met in
    one change: a UI change must not add hardcoded strings, must not reduce
    screen-reader coverage, and must not break locale key parity."""
    with open(baseline_path, encoding="utf-8") as f:
        base = json.load(f)
    problems = []
    bx, nx = base["xaml"], report["xaml"]

    if nx["hardcoded_count"] > bx["hardcoded_count"]:
        problems.append("hardcoded user-visible strings rose %d -> %d"
                        % (bx["hardcoded_count"], nx["hardcoded_count"]))
    if nx["interactive_with_automation_name"] < bx["interactive_with_automation_name"]:
        problems.append("controls with AutomationProperties.Name fell %d -> %d"
                        % (bx["interactive_with_automation_name"],
                           nx["interactive_with_automation_name"]))
    if report.get("code_automation_names", 0) < base.get("code_automation_names", 0):
        problems.append("automation names set from code fell %d -> %d"
                        % (base.get("code_automation_names", 0),
                           report.get("code_automation_names", 0)))
    for name, d in report["localization"].get("locales", {}).items():
        if d["missing"]:
            problems.append("%s is missing %d key(s): %s"
                            % (name, len(d["missing"]), ", ".join(d["missing"][:5])))
        if d["extra"]:
            problems.append("%s has %d key(s) not in the base locale: %s"
                            % (name, len(d["extra"]), ", ".join(d["extra"][:5])))
    return problems


def main():
    p = argparse.ArgumentParser(description="WPF shell localization/accessibility report")
    p.add_argument("--json", action="store_true", help="emit the raw report")
    p.add_argument("--baseline", help="fail if coverage regressed against this report")
    p.add_argument("--write-baseline", help="write the current report to this path")
    args = p.parse_args()

    report = build_report()

    if args.write_baseline:
        with open(args.write_baseline, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
        print("baseline written: %s" % args.write_baseline)
        return 0

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report)

    if args.baseline:
        problems = compare_baseline(report, args.baseline)
        print("")
        if problems:
            for msg in problems:
                print("REGRESSION: %s" % msg)
            return 1
        print("No regression against %s" % args.baseline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
