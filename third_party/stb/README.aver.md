# stb -- vendored for Aver

**Upstream:** https://github.com/nothings/stb
**Licence:** a choice of MIT or Public Domain (Unlicense); `LICENSE.txt` is kept beside the files.

| file | taken | sha256 |
|---|---|---|
| `stb_image.h`, `stb_image_write.h` | Phase 3.5/3.6 (upstream commit not recorded) | |
| `stb_vorbis.c` | v1.22, commit `1ee679ca2ef753a528db5ba6801e1067b40481b8` (the last change to the file), approved by the owner for the audio decoders 2026-10-30 | `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636` |

Files are unmodified. One known weakness of `stb_vorbis.c` 1.22 is handled around it, not in it: it trusts the 32-bit
vendor and comment lengths of the comment header (a length past the packet end becomes a negative allocation and a write
before its arena), so `modules/formats/src/AudioDecodeVorbis.cpp` checks that packet against its own size before
`stb_vorbis` sees the file. A newer `stb_vorbis.c` should be checked for the same thing before that guard is dropped. `stb_vorbis.c` is the Ogg Vorbis decoder (`docs/AUDIO.md`); before it, Ogg decoded only
where Windows Media Foundation had a Vorbis codec installed, which a stock Windows does not.
