#!/usr/bin/env python3
"""Checks every ICON_* in sandbox/src/EditorIcons.hpp against the Material Icons font itself.

For each `#define ICON_X "<UTF-8 escapes>"   // U+E2C8 folder_open` line it verifies that
  1. the UTF-8 escape decodes to the U+ codepoint in the comment,
  2. that codepoint is in the font's cmap (a missing one renders as the atlas's notdef box),
  3. the font's own ligature table (name -> glyph) maps the commented NAME to that codepoint, so the
     picture is the one the comment says, not merely some glyph.
Needs fontTools (pip install fonttools). Exits 1 on any mismatch."""
import re, sys, pathlib
from fontTools.ttLib import TTFont

root = pathlib.Path(__file__).resolve().parent.parent
font = TTFont(root / "third_party" / "fonts" / "MaterialIcons-Regular.ttf")
cmap = font.getBestCmap()
rev = {}
for cp, g in cmap.items():
    rev.setdefault(g, cp)
digits = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"]
def letter(g):
    if g == "underscore": return "_"
    if g in digits: return str(digits.index(g))
    return g if len(g) == 1 else "?" + g
by_name = {}
for lk in font["GSUB"].table.LookupList.Lookup:
    for st in lk.SubTable:
        st = getattr(st, "ExtSubTable", st)
        for first, ligs in getattr(st, "ligatures", {}).items():
            for l in ligs:
                if l.LigGlyph in rev:
                    by_name["".join(letter(g) for g in [first] + l.Component)] = rev[l.LigGlyph]

pat = re.compile(r'#define\s+(ICON_\w+)\s+"((?:\\x[0-9A-Fa-f]{2})+)"\s*//\s*U\+([0-9A-Fa-f]{4,5})\s+(\w+)')
bad = 0
n = 0
for line in (root / "sandbox" / "src" / "EditorIcons.hpp").read_text(encoding="utf-8").splitlines():
    if not line.startswith("#define ICON_"):
        continue
    m = pat.match(line)
    if not m:
        print("UNPARSED:", line); bad += 1; continue
    macro, esc, cp, name = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
    n += 1
    got = bytes(int(h, 16) for h in re.findall(r"\\x([0-9A-Fa-f]{2})", esc)).decode("utf-8")
    if len(got) != 1 or ord(got) != cp:
        print(f"{macro}: escape decodes to {[hex(ord(c)) for c in got]}, comment says U+{cp:04X}"); bad += 1
    if cp not in cmap:
        print(f"{macro}: U+{cp:04X} is not in the font"); bad += 1
    if by_name.get(name) != cp:
        print(f"{macro}: font says '{name}' is U+{by_name.get(name, 0):04X}, not U+{cp:04X}"); bad += 1
print(f"{n} icons checked, {bad} problem(s)")
sys.exit(1 if bad else 0)
