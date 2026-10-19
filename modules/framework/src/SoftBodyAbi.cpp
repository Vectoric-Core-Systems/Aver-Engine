// Cloth and destruction C ABI: a thin relay to the provider a composition root installs.
// See framework_softbody_abi.h and docs/SOFTBODY.md.

#include "aver/framework/framework_softbody_abi.h"

namespace {

aver_fw_softbody_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, const float* in = nullptr, int32_t inCount = 0, float* outF = nullptr,
             int32_t* outI = nullptr, int32_t cap = 0) {
    return g_provider ? g_provider(op, entity, in, inCount, outF, outI, cap, g_providerUser) : 0;
}

} // namespace

int32_t aver_fw_softbody_abi_version(void) { return AVER_FW_SOFTBODY_ABI_VERSION; }

int32_t aver_fw_set_softbody_provider(aver_fw_softbody_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_softbody_damage(int32_t entity, float x, float y, float z, float dx, float dy, float dz,
                                float radiusCm, float energyJ, int32_t kind) {
    const float in[9] = {x, y, z, dx, dy, dz, radiusCm, energyJ, static_cast<float>(kind)};
    return call(AVER_FW_SB_OP_DAMAGE, entity, in, 9);
}

int32_t aver_fw_softbody_explode(float x, float y, float z, float radiusCm, float impulseCmS, float energyJ) {
    const float in[6] = {x, y, z, radiusCm, impulseCmS, energyJ};
    int32_t touched = 0;
    if (!call(AVER_FW_SB_OP_EXPLODE, 0, in, 6, nullptr, &touched, 1)) return 0;
    return touched;
}

int32_t aver_fw_softbody_tear(int32_t entity, float x, float y, float z, float radiusCm) {
    const float in[4] = {x, y, z, radiusCm};
    int32_t broken = 0;
    if (!call(AVER_FW_SB_OP_TEAR, entity, in, 4, nullptr, &broken, 1)) return 0;
    return 1;
}

int32_t aver_fw_softbody_repair(int32_t entity) { return call(AVER_FW_SB_OP_REPAIR, entity); }

int32_t aver_fw_softbody_release(int32_t entity) { return call(AVER_FW_SB_OP_RELEASE, entity); }

int32_t aver_fw_softbody_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence) {
    const float in[6] = {vx, vy, vz, gustSpeed, gustHz, turbulence};
    return call(AVER_FW_SB_OP_WIND, 0, in, 6);
}

int32_t aver_fw_softbody_set_budget(int32_t maxPieces, int32_t maxParticles, int32_t maxRebuilds, float debrisLifetimeS) {
    const float in[4] = {static_cast<float>(maxPieces), static_cast<float>(maxParticles), static_cast<float>(maxRebuilds), debrisLifetimeS};
    return call(AVER_FW_SB_OP_BUDGET, 0, in, 4);
}

int32_t aver_fw_softbody_set_preset(int32_t entity, int32_t preset) {
    const float in[1] = {static_cast<float>(preset)};
    return call(AVER_FW_SB_OP_PRESET, entity, in, 1);
}

int32_t aver_fw_softbody_state(int32_t entity, float* outDamageMaxStrain2, int32_t* outPiecesBrokenState3) {
    float f[2] = {0, 0};
    int32_t i[3] = {0, 0, 0};
    if (!call(AVER_FW_SB_OP_STATE, entity, nullptr, 0, f, i, 3)) return 0;
    if (outDamageMaxStrain2) { outDamageMaxStrain2[0] = f[0]; outDamageMaxStrain2[1] = f[1]; }
    if (outPiecesBrokenState3) { outPiecesBrokenState3[0] = i[0]; outPiecesBrokenState3[1] = i[1]; outPiecesBrokenState3[2] = i[2]; }
    return 1;
}

int32_t aver_fw_softbody_drain_events(int32_t* outI, float* outF, int32_t max) {
    if (!outI || !outF || max <= 0) return 0;
    return call(AVER_FW_SB_OP_EVENTS, 0, nullptr, 0, outF, outI, max);
}

int32_t aver_fw_softbody_set_asleep(int32_t entity, int32_t asleep) {
    const float in[1] = {asleep ? 1.0f : 0.0f};
    return call(AVER_FW_SB_OP_ASLEEP, entity, in, 1);
}

int32_t aver_fw_softbody_clear_debris(int32_t entity) { return call(AVER_FW_SB_OP_CLEAR_DEBRIS, entity); }

int32_t aver_fw_softbody_set_effects(int32_t enabled) {
    const float in[1] = {enabled ? 1.0f : 0.0f};
    return call(AVER_FW_SB_OP_EFFECTS, 0, in, 1);
}

int32_t aver_fw_softbody_dents(int32_t entity, float* outMaxSetCm, int32_t* outDentedDebris2) {
    float f[1] = {0};
    int32_t i[2] = {0, 0};
    if (!call(AVER_FW_SB_OP_DENTS, entity, nullptr, 0, f, i, 2)) return 0;
    if (outMaxSetCm) *outMaxSetCm = f[0];
    if (outDentedDebris2) { outDentedDebris2[0] = i[0]; outDentedDebris2[1] = i[1]; }
    return 1;
}

int32_t aver_fw_softbody_repair_all(void) {
    int32_t n = 0;
    if (!call(AVER_FW_SB_OP_REPAIR_ALL, 0, nullptr, 0, nullptr, &n, 1)) return 0;
    return n;
}
