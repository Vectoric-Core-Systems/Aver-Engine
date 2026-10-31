# TinyEXR -- vendored for Aver

**Upstream:** https://github.com/syoyo/tinyexr
**Version:** v1.0.13, commit `4946b5d92e13bcc8102ac2c8efd129596a90bf75` (2026-03-31), the last single-header release (v3 is a multi-file C rewrite).
**Licence:** BSD-3-Clause (`LICENSE`).
**Approved:** by the owner for OpenEXR images (Godot feature parity, docs/plans/GODOT_PARITY.md), 2026-10-31.

Kept unmodified: `tinyexr.h`, `exr_reader.hh`, `streamreader.hh`, `LICENSE`. Compile it in exactly one translation unit with `TINYEXR_IMPLEMENTATION`, `TINYEXR_USE_MINIZ 0` and
`TINYEXR_USE_STB_ZLIB 1`, so its deflate comes from the already vendored `third_party/stb` rather than a second zlib (miniz is not vendored).
The engine does that in `modules/platform/src/ThirdPartyTinyExr.cpp` and wraps it in `ImageExr.cpp`.

`exr_reader.hh` and `streamreader.hh` are the two small headers v1.0.13's `tinyexr.h` includes (`#include "exr_reader.hh"`, which includes
`streamreader.hh`); the first copy of this folder had only `tinyexr.h`, which cannot compile alone. Copied byte for byte from the same upstream commit.

| file | sha256 |
|---|---|
| `tinyexr.h` | `31bab3ec2827e0f7b2db28819e34691edbbb824201093669c79587a778909a03` |
| `exr_reader.hh` | `d8fba016fbab4bca34747c7cf00f6523ae89f25f06ff7e377d91fb5c6c806c3c` |
| `streamreader.hh` | `0d039e046087972a7e6cbe54ad7fa8949c877a6d61c951aba037869b825151de` |
