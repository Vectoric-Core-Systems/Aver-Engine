// Data layers C ABI: a thin relay to the provider the host installs. See framework_datalayer_abi.h.

#include "aver/framework/framework_datalayer_abi.h"

namespace {

aver_fw_datalayer_count_fn g_count = nullptr;
aver_fw_datalayer_name_fn g_name = nullptr;
aver_fw_datalayer_get_fn g_get = nullptr;
aver_fw_datalayer_set_fn g_set = nullptr;
aver_fw_datalayer_poll_fn g_poll = nullptr;
void* g_user = nullptr;

} // namespace

int32_t aver_fw_datalayer_abi_version(void) { return AVER_FW_DATALAYER_ABI_VERSION; }

int32_t aver_fw_set_datalayer_provider(aver_fw_datalayer_count_fn c, aver_fw_datalayer_name_fn n, aver_fw_datalayer_get_fn g,
                                       aver_fw_datalayer_set_fn s, aver_fw_datalayer_poll_fn p, void* user) {
    g_count = c; g_name = n; g_get = g; g_set = s; g_poll = p; g_user = user;
    return 1;
}

int32_t aver_fw_datalayer_count(void) { return g_count ? g_count(g_user) : 0; }

int32_t aver_fw_datalayer_name(int32_t index, char* buf, int32_t cap) { return g_name ? g_name(index, buf, cap, g_user) : 0; }

int32_t aver_fw_datalayer_get_state(const char* name, int32_t* outState) { return g_get && name ? g_get(name, outState, g_user) : 0; }

int32_t aver_fw_datalayer_set_state(const char* name, int32_t state) { return g_set && name ? g_set(name, state, g_user) : 0; }

int32_t aver_fw_datalayer_poll_completed(char* nameBuf, int32_t nameCap, int32_t* outState) {
    return g_poll ? g_poll(nameBuf, nameCap, outState, g_user) : 0;
}
