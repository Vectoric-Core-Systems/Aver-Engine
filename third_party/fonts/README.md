# Vendored editor fonts

Two typefaces, both **redistributed with the product** and both Apache-2.0, so the single `LICENSE`
in this directory covers them together. See each section for its own provenance.

---

# Material Icons (vendored)

`MaterialIcons-Regular.ttf` is the editor's icon font, merged into the same ImGui atlas as Roboto so
an icon can sit inline in a label. Redistributed with the product exactly as Roboto is —
`sandbox/CMakeLists.txt` stages it beside `Sandbox.exe`.

| | |
|---|---|
| Version | Material Icons 1.017 |
| Licence | Apache License 2.0 (the same `LICENSE` in this directory) |
| Upstream | <https://github.com/google/material-design-icons> |
| Path within repo | `font/MaterialIcons-Regular.ttf` |
| Copyright | Copyright 2018 Google, Inc. All Rights Reserved. |

**Why the classic static font and not Material Symbols.** The current Material Symbols release is a
10.6 MB *variable* font carrying four axes (FILL, GRAD, opsz, wght). ImGui rasterises through
stb_truetype, which does not instance variable axes — it would render one fixed default and we would
have vendored 26x Roboto's file size for nothing. The static `MaterialIcons-Regular.ttf` is 357 KB,
the same order as the Roboto files beside it.

**Why not Font Awesome**, which is the more common choice in ImGui projects: its font is SIL OFL 1.1,
and this project's licence policy below explicitly excludes OFL. Material Icons being Apache-2.0 is
the reason it fits here at all.

**The licence is NOT embedded in the file.** Unlike Roboto, this font's `name` table carries no
nameID 13/14 licence record — only copyright, family and version. The Apache-2.0 grant comes from
the upstream repository's own `LICENSE`, verified at vendoring time. So "check the name table" is not
a sufficient verification for THIS file; check the upstream repo.

## Codepoints

Glyphs live in the Private Use Area, `U+E000`–`U+F8FF`, 2188 of them. The font names its glyphs
`uniXXXX`, so the file itself cannot tell you which icon is which — the name-to-codepoint mapping is
a separate upstream file, `font/MaterialIcons-Regular.codepoints`. Every codepoint used by the
editor is listed in `sandbox/src/EditorIcons.hpp` and was checked against this font's own cmap at
the time it was added, rather than copied from documentation.

## Verifying a replacement

Must begin with the sfnt magic `00 01 00 00`, be ~350 KB, and its `name` table must read
`Material Icons` and `Version 1.017`. Then re-run the cmap check for every codepoint in
`EditorIcons.hpp` — a newer release can and does move icons.

---

# Roboto (vendored)

`Roboto-Regular.ttf` and `Roboto-Medium.ttf` are the editor UI typeface. They are **redistributed
with the product** — `sandbox/CMakeLists.txt` stages them next to `Sandbox.exe` and the editor loads
them at startup — so their licence is a shipping obligation, not just a build-time one.

| | |
|---|---|
| Version | Roboto 3.008 |
| Licence | Apache License 2.0 (see `LICENSE`) |
| Upstream | <https://github.com/googlefonts/roboto-classic> |
| Source archive | <https://github.com/googlefonts/roboto-classic/releases/download/v3.008/Roboto_v3.008.zip> |
| Path within archive | `Roboto_v3.008/unhinted/static/` |
| Copyright | Copyright 2011 Google Inc. |

`unhinted/static` is the build we want: TrueType hinting bytecode is dead weight here because ImGui
rasterises through stb_truetype, which ignores it, and the `web/` builds are charset subsets.

## Why 3.008 and not the latest release

**Roboto is no longer Apache-2.0.** Google relicensed it to the SIL Open Font License 1.1 in
January 2024; v3.009 (2024-01-25) was the first release to ship under OFL, and every release since
carries `OFL.txt` instead of `LICENSE.txt`. In `google/fonts` the font moved directory accordingly,
from `apache/roboto` to `ofl/roboto` — the old path is now a 404.

This project accepts MIT/BSD/zlib/Apache-2.0/public-domain only, and OFL 1.1 is not on that list —
it carries a Reserved Font Name clause and requires derivatives to stay under OFL. So we pin 3.008,
the last Apache-2.0 release, which keeps the typeface that was approved *and* the licence it was
approved under. This is the same vintage of Roboto that the Unreal editor ships.

**Do not bump these files to a newer Roboto without a licensing decision** — a routine "update the
vendored font" would silently move the product onto OFL.

## Verifying a replacement

Both files must begin with the sfnt magic `00 01 00 00`, be ~400 KB, and their embedded `name`
table must read `Version 3.008` and `Licensed under the Apache License, Version 2.0`.
