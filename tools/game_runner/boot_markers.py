"""Boot-progress markers, defined once.

Both harnesses (tools/game_runner/session.py and tools/autorun.py) previously
carried their own copy of this table, kept identical by a comment asking the
next editor to remember. They agreed, but only by luck, and they agreed on a
wrong entry.

Each marker is a literal substring searched for in the emulator's log. A marker
must therefore be a line the EMULATOR emits about its own progress, never a
fragment that can appear in text the guest supplies. `("menus", "menu")` broke
that rule: it matched `images/menutitle-sheet0.png`, an asset filename echoed in
an HLE log line, and so fired the moment the guest loaded a file whose name
happened to contain "menu" -- 182,790 lines before the run reached anything a
player would call a menu. It was reported as evidence of progress for a long
time. It is removed rather than narrowed, because no substring of the current
log distinguishes "a menu is on screen" from "a file with menu in its name was
opened", and a marker that cannot make that distinction is worse than no marker.

Adding one: prefer an emulator-authored line, and check it cannot match guest
text. `Done load` is safe because the emulator writes it; a bare noun is not.
"""

# (name, literal substring in the emulator log)
MARKERS = [
    ("first-draw", "First guest draw executed"),
    ("shaders", "Translating shaders"),
    ("pthreads", "scePthreadCreate"),
    ("content", "Done load"),
]

# Markers removed, and why, so a future session does not reintroduce them.
RETIRED_MARKERS = {
    "menus": "matched the bare substring 'menu', firing on asset filenames such "
             "as images/menutitle-sheet0.png rather than on a menu being reached",
}


def scan(text):
    """Return the set of marker names present in `text`."""
    return {name for name, needle in MARKERS if needle in text}
