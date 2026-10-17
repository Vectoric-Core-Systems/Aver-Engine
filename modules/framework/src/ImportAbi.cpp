// Model import C ABI: a thin relay to the provider the editor installs.
// See framework_import_abi.h and docs/plans/wave9/FBX_IMPORT.md 2.10.

#include "aver/framework/framework_import_abi.h"

#include <cstring>

namespace {

aver_fw_import_fn g_provider = nullptr;
void* g_providerUser = nullptr;

void setMessage(char* out, int32_t cap, const char* text) {
    if (!out || cap <= 0) return;
    const size_t n = std::strlen(text);
    const size_t keep = n < static_cast<size_t>(cap) - 1 ? n : static_cast<size_t>(cap) - 1;
    std::memcpy(out, text, keep);
    out[keep] = '\0';
}

void zeroCounts(int32_t* counts) {
    if (counts) for (int i = 0; i < 5; ++i) counts[i] = 0;
}

} // namespace

int32_t aver_fw_import_abi_version(void) { return AVER_FW_IMPORT_ABI_VERSION; }

int32_t aver_fw_set_import_provider(aver_fw_import_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_import_available(void) { return g_provider ? 1 : 0; }

int32_t aver_fw_import_model(const char* utf8Source, const char* utf8DestFolder, const char* utf8Options,
                             int32_t overwrite, char* outMessage, int32_t messageCap, int32_t* outCounts5) {
    zeroCounts(outCounts5);
    setMessage(outMessage, messageCap, "");
    if (!g_provider) {
        setMessage(outMessage, messageCap, "model import is not available in this host");
        return 0;
    }
    const int32_t rv = g_provider(utf8Source ? utf8Source : "", utf8DestFolder ? utf8DestFolder : "",
                                  utf8Options ? utf8Options : "", overwrite, outMessage, messageCap,
                                  outCounts5, g_providerUser);
    if (outMessage && messageCap > 0) outMessage[messageCap - 1] = '\0';   // a provider that filled the buffer
    return rv ? 1 : 0;
}
