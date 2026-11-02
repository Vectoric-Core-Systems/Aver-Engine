// The clipboard seam's refusals (parity E16). The calls that touch the operating system's clipboard are not exercised: this test would overwrite whatever the person
// running it had copied. What it checks is everything decided before the OS is reached: text that is not valid UTF-8 and text over the size cap are refused without
// opening the clipboard at all.
#include "aver/core/Log.hpp"
#include "aver/platform/Clipboard.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    check(!clipboardSetText(std::string_view("\xC0\xAF", 2)), "an overlong UTF-8 encoding is refused");
    check(!clipboardSetText(std::string_view("ab\xFF", 3)), "a byte that is never UTF-8 is refused");
    check(!clipboardSetText(std::string_view("\xED\xA0\x80", 3)), "an encoded surrogate is refused");
    check(!clipboardSetText(std::string_view("\xE2\x82", 2)), "a truncated sequence is refused");
    const std::string big(kClipboardMaxBytes + 1, 'x');
    check(!clipboardSetText(big), "text over the size cap is refused");
    check(kClipboardMaxBytes == 16u * 1024u * 1024u, "the cap is 16 MiB");

    if (g_failures == 0) AVER_INFO("=== ClipboardTest passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
