# ufbx

FBX loader, single source file. Used by the FBX importer (docs/plans/wave9/FBX_IMPORT.md).

- Source: https://github.com/ufbx/ufbx, tag v0.23.1 (`ufbx.h`, `ufbx.c`, `LICENSE`), unmodified.
- License: MIT or public domain (Unlicense), at our choice; see `LICENSE`. We use it under MIT.
- Added 2026-10-17 with the owner's approval.
- Aver's notes (hashes, what was taken, how to re-vendor): `README.aver.md`. Build: `CMakeLists.txt`.
- The design says the licence file is `LICENSE.txt`; the vendored file is `LICENSE`, and the payload
  licence row (`scripts/stage-payload.ps1`) names `third_party\ufbx\LICENSE`.
