// The SDL clipboard: SDL_GetClipboardText / SDL_SetClipboardText. See aver/platform/Clipboard.hpp.
#include "aver/platform/Clipboard.hpp"

#include <SDL3/SDL.h>

#include <cstring>

namespace aver {

bool clipboardGetText(std::string& out) {
    out.clear();
    if (!SDL_HasClipboardText()) return false;
    char* text = SDL_GetClipboardText();
    if (!text) return false;
    const usize n = std::strlen(text);
    const bool ok = n <= kClipboardMaxBytes;
    if (ok) out.assign(text, n);
    SDL_free(text);
    return ok;
}

bool clipboardSetText(std::string_view utf8) {
    if (utf8.size() > kClipboardMaxBytes) return false;
    const std::string copy(utf8);   // SDL wants a NUL-terminated string
    return SDL_SetClipboardText(copy.c_str());
}

} // namespace aver
