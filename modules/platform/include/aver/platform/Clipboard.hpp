#pragma once
// The operating system's text clipboard (parity E16): one pair of calls, UTF-8 in and out, the same on every platform. Win32 reads and writes CF_UNICODETEXT;
// SDL builds use SDL_GetClipboardText / SDL_SetClipboardText. Main thread only, like the window layer.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver {

// The most text either call moves; a clipboard holding more is read as unavailable and a longer string is not set.
constexpr usize kClipboardMaxBytes = 16u * 1024u * 1024u;

// Reads the clipboard's text into `out` (replaced). False when it holds no text, is held open by another process, or holds more than kClipboardMaxBytes.
bool clipboardGetText(std::string& out);

// Replaces the clipboard's contents with `utf8`. False when it could not be opened or the text is over kClipboardMaxBytes or not valid UTF-8.
bool clipboardSetText(std::string_view utf8);

} // namespace aver
