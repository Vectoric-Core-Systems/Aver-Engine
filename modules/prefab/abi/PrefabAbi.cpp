// Aver.Prefab.Abi: forwards the aver_prefab_* calls to whatever host installed itself. No state of
// its own beyond the copied function table.
#include "aver/prefab/prefab_abi.h"

namespace {

AverPrefabHost g_host{};
bool g_installed = false;

} // namespace

extern "C" {

int32_t aver_prefab_abi_version(void) { return AVER_PREFAB_ABI_VERSION; }

void aver_prefab_set_host(const AverPrefabHost* host) {
    if (host) { g_host = *host; g_installed = true; }
    else      { g_host = AverPrefabHost{}; g_installed = false; }
}

int32_t aver_prefab_spawn(const char* path, int32_t parent, float x, float y, float z,
                          float yaw, float pitch, float roll, float scale) {
    if (!g_installed || !g_host.spawn || !path || !*path) return 0;
    const float xyz[3] = {x, y, z};
    const float ypr[3] = {yaw, pitch, roll};
    return g_host.spawn(g_host.user, path, parent, xyz, ypr, scale);
}

int32_t aver_prefab_destroy(int32_t root) {
    return g_installed && g_host.destroy ? g_host.destroy(g_host.user, root) : 0;
}

int32_t aver_prefab_root_of(int32_t entity) {
    return g_installed && g_host.root_of ? g_host.root_of(g_host.user, entity) : 0;
}

int32_t aver_prefab_is_instance(int32_t entity) { return aver_prefab_root_of(entity) != 0 ? 1 : 0; }

int32_t aver_prefab_find(int32_t root, const char* nodePath) {
    return g_installed && g_host.find ? g_host.find(g_host.user, root, nodePath ? nodePath : "") : 0;
}

int32_t aver_prefab_revert(int32_t root) {
    return g_installed && g_host.revert ? g_host.revert(g_host.user, root) : 0;
}

int32_t aver_prefab_override_count(int32_t root) {
    return g_installed && g_host.override_count ? g_host.override_count(g_host.user, root) : 0;
}

} // extern "C"
