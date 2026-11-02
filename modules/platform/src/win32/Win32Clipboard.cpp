// The Win32 clipboard: CF_UNICODETEXT in and out, converted to and from UTF-8. See aver/platform/Clipboard.hpp.
#include "aver/platform/Clipboard.hpp"

#include <Windows.h>

#include <vector>

namespace aver {
namespace {

// Another process can hold the clipboard open for a moment (a clipboard manager): a few short retries, never a long wait.
bool openClipboard() {
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(2);
    }
    return false;
}

struct Closer {
    ~Closer() { CloseClipboard(); }
};

} // namespace

bool clipboardGetText(std::string& out) {
    out.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !openClipboard()) return false;
    Closer closer;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) return false;
    const SIZE_T bytes = GlobalSize(h);
    if (bytes < sizeof(wchar_t) || bytes > kClipboardMaxBytes) return false;
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
    if (!w) return false;
    // The text ends at the first NUL or at the end of the block, whichever comes first: a block that is not terminated is read no further than it goes.
    const usize maxChars = bytes / sizeof(wchar_t);
    usize n = 0;
    while (n < maxChars && w[n] != 0) ++n;
    bool ok = true;
    if (n > 0) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        if (need <= 0 || static_cast<usize>(need) > kClipboardMaxBytes) ok = false;
        else {
            out.resize(static_cast<usize>(need));
            ok = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), out.data(), need, nullptr, nullptr) == need;
            if (!ok) out.clear();
        }
    }
    GlobalUnlock(h);
    return ok;
}

bool clipboardSetText(std::string_view utf8) {
    if (utf8.size() > kClipboardMaxBytes) return false;
    int wide = 0;
    std::vector<wchar_t> buf;
    if (!utf8.empty()) {
        wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wide <= 0) return false;   // not valid UTF-8
        buf.resize(static_cast<usize>(wide));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), buf.data(), wide) != wide) return false;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wide) + 1) * sizeof(wchar_t));
    if (!mem) return false;
    wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
    if (!dst) { GlobalFree(mem); return false; }
    for (int i = 0; i < wide; ++i) dst[i] = buf[static_cast<usize>(i)];
    dst[wide] = 0;
    GlobalUnlock(mem);
    if (!openClipboard()) { GlobalFree(mem); return false; }
    Closer closer;
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) { GlobalFree(mem); return false; }   // on success the system owns the block
    return true;
}

} // namespace aver
