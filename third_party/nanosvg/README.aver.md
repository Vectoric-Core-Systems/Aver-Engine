# NanoSVG -- vendored for Aver

**Upstream:** https://github.com/memononen/nanosvg
**Version:** no releases; commit `239e102ec2c691f2902e20ace2ed36ee4a35cfe6` (2026-07-10)
**Licence:** zlib (`LICENSE.txt`).
**Approved:** by the owner for SVG images (Godot feature parity, docs/plans/GODOT_PARITY.md), 2026-10-31.

Kept unmodified: `nanosvg.h` (parser), `nanosvgrast.h` (rasteriser), `LICENSE.txt`. Each is compiled in exactly one translation unit with its
implementation macro: `modules/platform/src/ThirdPartyNanoSvg.cpp` (warnings off for that file), wrapped by `ImageSvg.cpp`.

| file | sha256 |
|---|---|
| `nanosvg.h` | `91afc79e91d84f3f6de7662382dd3d8383869cc0152576f1b4b5f14f70a9c98b` |
| `nanosvgrast.h` | `93efb49f5166259a466aa87124e458899eb01a7f5eff099ed41382a9d0e88d64` |
