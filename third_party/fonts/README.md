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
