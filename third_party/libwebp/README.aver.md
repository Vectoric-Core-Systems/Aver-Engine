# libwebp -- vendored for Aver

**Upstream:** https://github.com/webmproject/libwebp
**Version:** v1.6.0, commit `4fa21912338357f89e4fd51cf2368325b59e9bd9` (2025-07-07)
**Licence:** BSD-3-Clause (`COPYING`), with Google's patent grant (`PATENTS`).
**Approved:** by the owner for WebP image decoding (Godot feature parity, docs/plans/GODOT_PARITY.md), 2026-10-31.

Only the decoder is used. Kept from upstream, unmodified: `src/dec`, `src/dsp`, `src/utils`, `src/webp` (public headers), `COPYING`, `PATENTS`,
`AUTHORS`. `src/dsp` and `src/utils` also hold encoder sources; the build compiles the decoder's file list only (upstream's `libwebpdecoder`).

`CMakeLists.txt` in this folder is the engine's own (not upstream's): it builds that file list as the static library `webpdecoder`, warnings off, with
no defines beyond what upstream's headers derive from the compiler. It is linked into `Aver.Platform` and used only by `modules/platform/src/ImageWebp.cpp`.

| file | sha256 |
|---|---|
| `src/dec/vp8_dec.c` | `e35bbdd94e8f20ace4fa4f8c53e2fedf0acfaa7a5271e7a147e0fda1cd7f4416` |
