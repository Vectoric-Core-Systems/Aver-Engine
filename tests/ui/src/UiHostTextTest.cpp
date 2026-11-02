// Parity E16 on the UI side: typed characters the host feeds (and the key-table characters that stop when it does), and the clipboard seam with a fake host.
// Links the DLL the C# side loads. No window, no clipboard.
#include "aver/core/Log.hpp"
#include "aver/ui/UiHostInput.hpp"
#include "aver/ui/ui_abi.h"
#include "aver/ui/ui_widget_abi.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string g_clip = "from the OS";
static int g_gets = 0, g_sets = 0;

static int32_t fakeGet(char* out, int32_t cap) {
    ++g_gets;
    if (g_clip.empty()) return -1;
    if (out && cap > 0) {
        const int32_t n = static_cast<int32_t>(std::min<size_t>(g_clip.size(), static_cast<size_t>(cap - 1)));
        std::memcpy(out, g_clip.data(), static_cast<size_t>(n));
        out[n] = '\0';
    }
    return static_cast<int32_t>(g_clip.size());
}

static int32_t fakeSet(const char* utf8) {
    ++g_sets;
    g_clip = utf8;
    return 1;
}

int main() {
    AVER_INFO("=== the mapper makes characters from keys until the host feeds text ===");
    {
        ui::UiHostInputMapper m;
        ui::UiHostSnapshot s;
        ui::UiInputFrame out;
        s.keyPressed['A'] = true;
        s.keyHeld['A'] = true;
        m.map(s, 0.016f, out);
        check(out.chars.size() == 1, "a pressed letter types a character from the US table by default");
        ui::UiHostInputMapper h;
        h.hostTypes = true;
        ui::UiInputFrame out2;
        h.map(s, 0.016f, out2);
        check(out2.chars.empty(), "with hostTypes on the same key types nothing (the window's Char event does)");
        ui::UiInputFrame out3;
        ui::UiHostSnapshot enter;
        enter.keyPressed[0x0D] = true;
        enter.keyHeld[0x0D] = true;
        h.map(enter, 0.016f, out3);
        check(!out3.editKeys.empty(), "edit keys are still made from key presses");
    }

    AVER_INFO("=== the clipboard seam ===");
    {
        char buf[32];
        check(aver_ui_clipboard_available() == 0 && aver_ui_clipboard_get(buf, sizeof buf) == -1 && aver_ui_clipboard_set("x") == 0, "with no host there is no clipboard");
        aver_ui_set_clipboard_host(&fakeGet, nullptr);
        check(aver_ui_clipboard_available() == 0, "half a host is no host");
        aver_ui_set_clipboard_host(&fakeGet, &fakeSet);
        check(aver_ui_clipboard_available() == 1, "a host with both callbacks is one");
        check(aver_ui_clipboard_get(buf, sizeof buf) == 11 && std::string(buf) == "from the OS", "get copies the text and returns its length");
        check(aver_ui_clipboard_get(nullptr, 0) == 11, "a NULL buffer asks the size");
        char small[5];
        check(aver_ui_clipboard_get(small, sizeof small) == 11 && std::string(small) == "from", "a small buffer gets a truncated, NUL-terminated copy and the full length");
        check(aver_ui_clipboard_get(nullptr, 8) == -1 && aver_ui_clipboard_get(buf, -1) == -1, "a NULL buffer with a size, or a negative size, is refused");
        check(aver_ui_clipboard_set("copied") == 1 && g_clip == "copied" && g_sets == 1, "set reaches the host");
        check(aver_ui_clipboard_set(nullptr) == 0 && g_sets == 1, "a NULL string never does");
        g_clip.clear();
        check(aver_ui_clipboard_get(buf, sizeof buf) == -1 && buf[0] == '\0', "an empty clipboard is -1");
        aver_ui_set_clipboard_host(nullptr, nullptr);
        check(aver_ui_clipboard_available() == 0, "clearing the host clears it");
    }

    if (g_failures == 0) AVER_INFO("=== all host text tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
