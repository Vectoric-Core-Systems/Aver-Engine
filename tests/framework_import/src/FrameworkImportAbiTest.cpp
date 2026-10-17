// The model import C ABI (framework_import_abi.h) end to end through the Aver.Framework DLL: with no
// provider every call answers 0, says why and zeroes the counts; with a provider every call forwards its
// exact strings and flag, and the provider's message and counts come back; clearing the provider restores
// the refusal. The provider here is a fake, so no importer is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_import_abi.h"

#include <cstring>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

struct Call {
    std::string source, dest, options;
    int32_t overwrite = -1;
    int32_t cap = 0;
    bool hasMessage = false, hasCounts = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 0;

static int32_t AVER_FW_CALL fake(const char* src, const char* dest, const char* opts, int32_t overwrite,
                                 char* msg, int32_t cap, int32_t* counts, void* user) {
    g_call.source = src; g_call.dest = dest; g_call.options = opts;
    g_call.overwrite = overwrite; g_call.cap = cap;
    g_call.hasMessage = msg != nullptr; g_call.hasCounts = counts != nullptr;
    g_call.user = user;
    ++g_call.calls;
    if (msg && cap > 0) std::strncpy(msg, "imported Hero.fbx", static_cast<size_t>(cap) - 1), msg[cap - 1] = '\0';
    if (counts) { counts[0] = 2; counts[1] = 1; counts[2] = 3; counts[3] = 4; counts[4] = 5; }
    return g_return;
}

int main() {
    AVER_INFO("FrameworkImportAbiTest");
    check(aver_fw_import_abi_version() == 1 && AVER_FW_IMPORT_ABI_VERSION == 1, "the import ABI is version 1");

    AVER_INFO("=== no provider ===");
    {
        char msg[64];
        std::memset(msg, 'x', sizeof msg);
        int32_t counts[5] = {9, 9, 9, 9, 9};
        check(aver_fw_import_available() == 0, "import is unavailable");
        check(aver_fw_import_model("a.fbx", "Meshes", "", 0, msg, sizeof msg, counts) == 0, "import_model answers 0");
        check(std::string(msg) == "model import is not available in this host", "and says the host cannot import");
        check(counts[0] == 0 && counts[1] == 0 && counts[2] == 0 && counts[3] == 0 && counts[4] == 0, "and zeroes the counts");

        // A short buffer is truncated and terminated, not overrun.
        char tiny[8];
        std::memset(tiny, 'x', sizeof tiny);
        check(aver_fw_import_model("a.fbx", "Meshes", "", 0, tiny, sizeof tiny, nullptr) == 0, "a short buffer: still 0");
        check(tiny[7] == '\0' && std::strlen(tiny) == 7, "the message is cut to fit and terminated");
        check(aver_fw_import_model("a.fbx", "Meshes", "", 0, nullptr, 0, nullptr) == 0, "null outputs are accepted");
    }

    AVER_INFO("=== a provider receives exactly what was sent ===");
    {
        int token = 0;
        check(aver_fw_set_import_provider(&fake, &token) == 1, "installing returns 1");
        check(aver_fw_import_available() == 1, "import is available");
        g_return = 1;
        char msg[64];
        int32_t counts[5] = {-1, -1, -1, -1, -1};
        check(aver_fw_import_model("C:/in/Hero.fbx", "Meshes/Hero", "scale=100;rootMotion=auto", 1, msg, sizeof msg, counts) == 1,
              "import_model relays the success");
        check(g_call.source == "C:/in/Hero.fbx" && g_call.dest == "Meshes/Hero" && g_call.options == "scale=100;rootMotion=auto" &&
              g_call.overwrite == 1, "source, destination, options and overwrite arrive exactly");
        check(g_call.cap == static_cast<int32_t>(sizeof msg) && g_call.hasMessage && g_call.hasCounts, "and the output buffers");
        check(g_call.user == &token, "the user pointer comes back");
        check(std::string(msg) == "imported Hero.fbx", "the provider's message is relayed");
        check(counts[0] == 2 && counts[1] == 1 && counts[2] == 3 && counts[3] == 4 && counts[4] == 5, "and its five counts");

        aver_fw_import_model(nullptr, nullptr, nullptr, 0, nullptr, 0, nullptr);
        check(g_call.source.empty() && g_call.dest.empty() && g_call.options.empty() && g_call.overwrite == 0 && !g_call.hasMessage,
              "null strings reach the provider as empty ones");

        g_return = 0;
        check(aver_fw_import_model("a.fbx", "M", "", 0, msg, sizeof msg, counts) == 0, "a refusal is relayed as 0");
        check(std::string(msg) == "imported Hero.fbx", "and the provider's message still arrives");
        g_return = 7;
        check(aver_fw_import_model("a.fbx", "M", "", 0, msg, sizeof msg, counts) == 1, "any non-zero return reads as success");
    }

    AVER_INFO("=== clearing the provider ===");
    {
        const int before = g_call.calls;
        check(aver_fw_set_import_provider(nullptr, nullptr) == 1, "clearing returns 1");
        char msg[64];
        int32_t counts[5] = {9, 9, 9, 9, 9};
        g_return = 1;
        check(aver_fw_import_available() == 0, "import is unavailable again");
        check(aver_fw_import_model("a.fbx", "M", "", 0, msg, sizeof msg, counts) == 0, "import_model is 0 again");
        check(counts[0] == 0 && counts[4] == 0, "with zero counts");
        check(g_call.calls == before, "the old provider was not called");
    }

    AVER_INFO(g_failures == 0 ? "FrameworkImportAbiTest: PASS" : "FrameworkImportAbiTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
