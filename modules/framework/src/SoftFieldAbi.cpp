// Soft-body field C ABI: a thin relay to the provider a composition root installs.
// See framework_softfield_abi.h and docs/SOFTBODY.md.

#include "aver/framework/framework_softfield_abi.h"

#include <cmath>

namespace {

aver_fw_softfield_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, const float* in, int32_t inCount, int32_t* outI = nullptr, int32_t cap = 0) {
    return g_provider ? g_provider(op, 0, in, inCount, nullptr, outI, cap, g_providerUser) : 0;
}

// A spec is refused before the provider when it is short, holds a value that is not finite, or names a kind or a shape that
// does not exist.
bool specOk(const float* spec, int32_t n) {
    if (!spec || n < AVER_FW_SF_SPEC_COUNT) return false;
    for (int32_t i = 0; i < AVER_FW_SF_SPEC_COUNT; ++i)
        if (!std::isfinite(spec[i])) return false;
    const int32_t kind = static_cast<int32_t>(std::lround(spec[0])), shape = static_cast<int32_t>(std::lround(spec[1]));
    return kind >= 0 && kind <= 5 && shape >= 0 && shape <= 3;
}

}  // namespace

int32_t aver_fw_softfield_abi_version(void) { return AVER_FW_SOFTFIELD_ABI_VERSION; }

int32_t aver_fw_set_softfield_provider(aver_fw_softfield_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_softfield_pulse(const float* spec, int32_t n) {
    if (!specOk(spec, n)) return 0;
    int32_t reach = 0;
    if (!call(AVER_FW_SF_OP_PULSE, spec, AVER_FW_SF_SPEC_COUNT, &reach, 1)) return 0;
    return reach;
}

int32_t aver_fw_softfield_start(const float* spec, int32_t n) {
    if (!specOk(spec, n)) return 0;
    int32_t handle = 0;
    if (!call(AVER_FW_SF_OP_START, spec, AVER_FW_SF_SPEC_COUNT, &handle, 1)) return 0;
    return handle;
}

int32_t aver_fw_softfield_stop(int32_t handle) {
    const float in[1] = {static_cast<float>(handle)};
    return call(AVER_FW_SF_OP_STOP, in, 1);
}

int32_t aver_fw_softfield_impact(float x, float y, float z, float dx, float dy, float dz, float radiusCm, float energyJ, float kickCmS) {
    const float in[9] = {x, y, z, dx, dy, dz, radiusCm, energyJ, kickCmS};
    for (const float v : in)
        if (!std::isfinite(v)) return 0;
    int32_t reach = 0;
    if (!call(AVER_FW_SF_OP_IMPACT, in, 9, &reach, 1)) return 0;
    return reach;
}

int32_t aver_fw_softfield_spawn(const float* spec, int32_t n) {
    if (!specOk(spec, n)) return 0;
    int32_t entity = 0;
    if (!call(AVER_FW_SF_OP_SPAWN, spec, AVER_FW_SF_SPEC_COUNT, &entity, 1)) return 0;
    return entity;
}
