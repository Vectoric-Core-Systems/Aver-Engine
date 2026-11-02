#pragma once
// WOFF 1.0 web fonts (parity P37): the container is an sfnt whose tables are each stored as they are or zlib-compressed, so
// loading one is rebuilding the plain sfnt from it. Aver.UI is Core-only and has no inflate of its own: the HOST lends one
// (aver::inflateRaw of Aver.Platform, which is the stb_image decoder the engine already carries, so nothing is added),
// the way it lends the font loader. Without one a WOFF is refused with a message that says so.
//
// WOFF2 compresses its tables with Brotli, which the engine does not carry for fonts, so it is recognised and refused with a
// message that says to convert the font.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::ui {

// The largest sfnt a WOFF may rebuild to, and the most tables it may name. Beyond these a file is refused, so a font of
// a few kilobytes cannot ask for gigabytes.
constexpr usize kUiWoffMaxSfntBytes = 64u * 1024u * 1024u;
constexpr u32 kUiWoffMaxTables = 64;

// Decodes a raw deflate stream (RFC 1951, no zlib header) that must fill exactly `outSize` bytes. False for anything else.
using UiInflateFn = bool (*)(const u8* in, usize inSize, u8* out, usize outSize);
// The host's inflate (null clears it). Process wide; set it before fonts load.
void uiSetInflate(UiInflateFn fn);
UiInflateFn uiInflate();

bool uiIsWoff(const u8* data, usize size);    // 'wOFF'
bool uiIsWoff2(const u8* data, usize size);   // 'wOF2'

// Rebuilds the sfnt a WOFF 1.0 file holds into `out` (replaced), inflating with `inflate` (null: the host's, uiInflate()). False
// with the reason in `err` for a header or table directory that is not valid (offsets and lengths are all checked against the
// file, tables must be in tag order and not overlap, the sizes must add up to the header's), a table that does not inflate to
// exactly its declared size, no inflate to use for a compressed table, or a size over the caps above. The tables' own checksums
// are not verified: the sfnt that comes out goes through uiValidateSfnt like any font.
bool uiWoffToSfnt(const u8* data, usize size, std::vector<u8>& out, std::string* err = nullptr, UiInflateFn inflate = nullptr);

} // namespace aver::ui
