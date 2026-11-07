// Nested layouts (.ocui v2 USE) on the widget C ABI: the reader a host installs, overrides and slots, the warnings of
// a build that succeeded, a missing file, a cycle and a reader that needs a bigger buffer. A fake host stands in for
// the file system. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/ui_widget_abi.h"

#include <cstring>
#include <map>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::map<std::string, std::string> g_files;
static int g_reads = 0;

static int32_t fakeRead(const char* path, char* out, int32_t cap) {
    ++g_reads;
    const auto it = g_files.find(path);
    if (it == g_files.end()) return -1;
    const int32_t n = static_cast<int32_t>(it->second.size());
    if (n < cap) std::memcpy(out, it->second.c_str(), static_cast<size_t>(n) + 1);
    return n;
}

static std::string prop(uint32_t id, const char* key) {
    const char* v = aver_ui_widget_get_prop(id, key);
    return v ? v : "";
}

int main() {
    g_files["Row.ocui"] =
        "OCUI 2\nWIDGET RowRoot panel -\nSET RowRoot layout hstack\n"
        "WIDGET Label text RowRoot\nSET Label text Default\nWIDGET Body panel RowRoot\n"
        "EXPOSE Title Label text\nSLOT Content Body\n";
    g_files["A.ocui"] = "OCUI 2\nWIDGET RA panel -\nUSE ToB B.ocui RA\n";
    g_files["B.ocui"] = "OCUI 2\nWIDGET RB panel -\nUSE ToA A.ocui RB\n";
    // Padding keeps this one well past the first 16 KB the ABI offers, so the reader is called again with a bigger buffer.
    g_files["Big.ocui"] = "OCUI 2\nWIDGET BigRoot panel -\n# " + std::string(40000, 'x') + "\nWIDGET BigLabel text BigRoot\n";

    const std::string host =
        "OCUI 2\nWIDGET Root panel -\n"
        "USE Row1 Row.ocui Root\nOVERRIDE Row1 Title \"Hello\"\n"
        "WIDGET Extra button Row1\nSET Extra slot Content\n";

    check(aver_ui_layout_open(host.c_str(), 0) != 0, "without a reader a USE still opens, as a placeholder");
    check(std::string(aver_ui_layout_warnings()).find("cannot find") != std::string::npos, "and the warnings say why");
    aver_ui_widget_destroy(aver_ui_widget_find("Root", 0));

    aver_ui_set_layout_reader(fakeRead);
    uint32_t root = aver_ui_layout_open(host.c_str(), 0);
    check(root != 0 && std::string(aver_ui_layout_warnings()).empty(), "with a reader the layout opens with no warnings");
    const uint32_t label = aver_ui_widget_find("Row1/Label", 0);
    check(label != 0 && prop(label, "text") == "Hello", "the used layout's widgets are there, prefixed, with the override applied");
    check(aver_ui_widget_parent(aver_ui_widget_find("Extra", 0)) == aver_ui_widget_find("Row1/Body", 0), "and the child sits in the slot");
    check(prop(aver_ui_widget_find("Row1", 0), "layout") == "hstack", "the instance root keeps the used layout's own properties");
    aver_ui_widget_destroy(root);

    root = aver_ui_layout_open("OCUI 2\nWIDGET Root panel -\nUSE Gone Nope.ocui Root\nUSE Row2 Row.ocui Root\nOVERRIDE Row2 Missing x\n", 0);
    const std::string warnings = aver_ui_layout_warnings();
    check(root != 0 && warnings.find("cannot find layout 'Nope.ocui'") != std::string::npos && warnings.find("exposes no 'Missing'") != std::string::npos &&
              warnings.find('\n') != std::string::npos,
          "a missing file and an unknown override are warnings, one per line");
    check(aver_ui_widget_find("Gone/Missing", 0) != 0, "the missing layout leaves a placeholder");
    aver_ui_widget_destroy(root);

    check(aver_ui_layout_open(g_files["A.ocui"].c_str(), 0) == 0 && std::string(aver_ui_layout_error()).find("cycle") != std::string::npos,
          "a cycle fails the open with an error that says so");
    check(aver_ui_widget_find("RA", 0) == 0, "and leaves nothing behind");

    g_reads = 0;
    root = aver_ui_layout_open("OCUI 2\nWIDGET Root panel -\nUSE Big Big.ocui Root\n", 0);
    check(root != 0 && aver_ui_widget_find("Big/BigLabel", 0) != 0 && g_reads == 2, "a layout bigger than the first buffer is read with a second call");
    aver_ui_widget_destroy(root);

    aver_ui_set_layout_reader(nullptr);
    root = aver_ui_layout_open(host.c_str(), 0);
    check(root != 0 && aver_ui_widget_find("Row1/Missing", 0) != 0, "clearing the reader goes back to placeholders");
    aver_ui_widget_destroy(root);

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all layout ABI checks passed");
    return 0;
}
