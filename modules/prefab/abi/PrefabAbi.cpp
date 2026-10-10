// Aver.Prefab.Abi: forwards the aver_prefab_* calls to whatever host installed itself. No state of
// its own beyond the copied function table and the thread-local reason slot.
#include "aver/prefab/prefab_abi.h"

#include "aver/core/ErrorCodes.hpp"

namespace {

AverPrefabHost g_host{};
bool g_installed = false;

using aver::AbiError;

// Records `e` and returns `v`, so each entry point reports its reason in one expression.
int32_t fail(AbiError e) { aver::setAbiError(e); return 0; }
int32_t ok(int32_t v) { aver::setAbiError(AbiError::Ok); return v; }

// A host call that returns 0 for a refusal. The host carries no reason, so `refused` is the
// best this DLL can say.
int32_t reported(int32_t hostResult, AbiError refused) {
    if (hostResult == 0) return fail(refused);
    return ok(hostResult);
}

// The not-installed and missing-function guards shared by every call.
template <typename Fn>
bool hostHas(Fn fn) {
    if (!g_installed) { aver::setAbiError(AbiError::NotInitialised); return false; }
    if (!fn)          { aver::setAbiError(AbiError::Unsupported);    return false; }
    return true;
}

} // namespace

extern "C" {

int32_t aver_prefab_abi_version(void) { return AVER_PREFAB_ABI_VERSION; }

int32_t aver_prefab_last_error(void) { return static_cast<int32_t>(aver::lastAbiError()); }

void aver_prefab_set_host(const AverPrefabHost* host) {
    if (host) { g_host = *host; g_installed = true; }
    else      { g_host = AverPrefabHost{}; g_installed = false; }
}

int32_t aver_prefab_spawn(const char* path, int32_t parent, float x, float y, float z,
                          float yaw, float pitch, float roll, float scale) {
    if (!hostHas(g_host.spawn)) return 0;
    if (!path || !*path) return fail(AbiError::InvalidArgument);
    const float xyz[3] = {x, y, z};
    const float ypr[3] = {yaw, pitch, roll};
    return reported(g_host.spawn(g_host.user, path, parent, xyz, ypr, scale), AbiError::InvalidArgument);
}

int32_t aver_prefab_destroy(int32_t root) {
    if (!hostHas(g_host.destroy)) return 0;
    return reported(g_host.destroy(g_host.user, root), AbiError::BadHandle);
}

int32_t aver_prefab_root_of(int32_t entity) {
    if (!hostHas(g_host.root_of)) return 0;
    return ok(g_host.root_of(g_host.user, entity));   // 0 = not part of an instance: an answer
}

int32_t aver_prefab_is_instance(int32_t entity) {
    const int32_t root = aver_prefab_root_of(entity);
    return root != 0 ? 1 : 0;
}

int32_t aver_prefab_find(int32_t root, const char* nodePath) {
    if (!hostHas(g_host.find)) return 0;
    return reported(g_host.find(g_host.user, root, nodePath ? nodePath : ""), AbiError::BadHandle);
}

int32_t aver_prefab_revert(int32_t root) {
    if (!hostHas(g_host.revert)) return 0;
    return reported(g_host.revert(g_host.user, root), AbiError::BadHandle);
}

int32_t aver_prefab_override_count(int32_t root) {
    if (!hostHas(g_host.override_count)) return 0;
    return ok(g_host.override_count(g_host.user, root));   // 0 = no overrides: an answer, not a refusal
}

} // extern "C"
