# dr_libs (dr_flac, dr_mp3) -- vendored for Aver

**Upstream:** https://github.com/mackron/dr_libs
**Version:** commit `39ba1ae7945d562ed18db5bba89298343c121222` (2026-10-29), taken for the 2026-10-28 dr_flac seek
and validation fixes and the 2026-10-24 dr_mp3 seek-table fixes.
**Licence:** a choice of Public Domain (Unlicense) or MIT No Attribution; `LICENSE` is kept beside the files.
**Approved:** by the owner for the audio decoders (2026-10-30).

## Why it is here

FLAC and MP3 decoding on every platform (`docs/AUDIO.md`); before this, MP3 decoded only through Windows Media
Foundation and FLAC not at all.

## What was taken

The files below, unmodified. Nothing else (no dr_wav, tests or build files).

| file | sha256 |
|---|---|
| `dr_flac.h` | `898ad822e21da1127ef8065eca655bcff8dc5f6060a411724c5c48017488b1b6` |
| `dr_mp3.h` | `390e4e0ef0cff80fb86ebf1b461d6947cfa4ed699a4fe107e5a5e69a47db2950` |
| `LICENSE` | `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15` |
