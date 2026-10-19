// Particle C ABI: a thin relay to the provider a composition root installs.
// See framework_particles_abi.h and docs/PARTICLES.md.

#include "aver/framework/framework_particles_abi.h"

namespace {

aver_fw_particles_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, int64_t h0, int64_t h1, const float* in = nullptr, float* outF = nullptr,
             int32_t* outI = nullptr) {
    return g_provider ? g_provider(op, entity, h0, h1, in, outF, outI, g_providerUser) : 0;
}

} // namespace

int32_t aver_fw_particles_abi_version(void) { return AVER_FW_PARTICLES_ABI_VERSION; }

int32_t aver_fw_set_particles_provider(aver_fw_particles_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_particles_spawn_at(int64_t effect, float px, float py, float pz, float dx, float dy, float dz,
                                   int32_t count) {
    if (count <= 0) return 0;
    const float in[17] = {px, py, pz, dx, dy, dz, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                          static_cast<float>(count)};
    return call(AVER_FW_PARTICLES_OP_SPAWN_AT, 0, effect, 0, in);
}

int32_t aver_fw_particles_spawn_at_ex(int64_t effect, const float* args16, int32_t count) {
    if (!args16 || count <= 0) return 0;
    float in[17];
    for (int i = 0; i < 16; ++i) in[i] = args16[i];
    in[16] = static_cast<float>(count);
    return call(AVER_FW_PARTICLES_OP_SPAWN_AT, 0, effect, 0, in);
}

int32_t aver_fw_particles_set_param(int32_t entity, int64_t nameHash, int32_t kind, float v0, float v1, float v2,
                                    float v3) {
    if (kind < 0 || kind > 2) return 0;
    const float in[4] = {v0, v1, v2, v3};
    return call(AVER_FW_PARTICLES_OP_SET_PARAM, entity, nameHash, kind, in);
}

int32_t aver_fw_particles_burst(int32_t entity, int32_t count) {
    if (count <= 0) return 0;
    const float in[1] = {static_cast<float>(count)};
    return call(AVER_FW_PARTICLES_OP_BURST, entity, 0, 0, in);
}

int32_t aver_fw_particles_stop(int32_t entity, int32_t killParticles) {
    const float in[1] = {killParticles ? 1.0f : 0.0f};
    return call(AVER_FW_PARTICLES_OP_STOP, entity, 0, 0, in);
}

int32_t aver_fw_particles_set_modifiers(int32_t entity, float rateScale, float sizeScale, float speedScale) {
    const float in[3] = {rateScale, sizeScale, speedScale};
    return call(AVER_FW_PARTICLES_OP_MODIFIERS, entity, 0, 0, in);
}

int32_t aver_fw_particles_emitter_state(int32_t entity, int32_t* outAlive, int32_t* outTier) {
    if (!outAlive || !outTier) return 0;
    int32_t out[2] = {0, 0};
    if (!call(AVER_FW_PARTICLES_OP_STATE, entity, 0, 0, nullptr, nullptr, out)) return 0;
    *outAlive = out[0];
    *outTier = out[1];
    return 1;
}

int32_t aver_fw_particles_stats(int32_t* outSix) {
    if (!outSix) return 0;
    int32_t out[6] = {0, 0, 0, 0, 0, 0};
    if (!call(AVER_FW_PARTICLES_OP_STATS, 0, 0, 0, nullptr, nullptr, out)) return 0;
    for (int i = 0; i < 6; ++i) outSix[i] = out[i];
    return 1;
}

int32_t aver_fw_particles_set_budget(int32_t maxGpu, int32_t maxCpu, int32_t maxSpawnAtPerFrame) {
    const float in[3] = {static_cast<float>(maxGpu), static_cast<float>(maxCpu), static_cast<float>(maxSpawnAtPerFrame)};
    return call(AVER_FW_PARTICLES_OP_BUDGET, 0, 0, 0, in);
}

int32_t aver_fw_particles_set_gpu_mode(int32_t mode) {
    const float in[1] = {static_cast<float>(mode)};
    return call(AVER_FW_PARTICLES_OP_GPU_MODE, 0, 0, 0, in);
}
