// Physical animation C ABI: a thin relay to the provider a composition root installs.
// See framework_physanim_abi.h and docs/physics/PHYSICAL_ANIMATION.md.

#include "aver/framework/framework_physanim_abi.h"

namespace {

aver_fw_physanim_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, int64_t h0, int64_t h1, const float* in, int32_t inCount,
             int64_t* outI = nullptr) {
    return g_provider ? g_provider(op, entity, h0, h1, in, inCount, outI, g_providerUser) : 0;
}

} // namespace

int32_t aver_fw_physanim_abi_version(void) { return AVER_FW_PHYSANIM_ABI_VERSION; }

int32_t aver_fw_set_physanim_provider(aver_fw_physanim_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_physanim_hit(int32_t entity, int64_t boneHash, float ix, float iy, float iz,
                             float px, float py, float pz, float damage, float radius) {
    const float v[8] = {ix, iy, iz, px, py, pz, damage, radius};
    return call(AVER_FW_PHYSANIM_OP_HIT, entity, boneHash, 0, v, 8);
}

int32_t aver_fw_physanim_set_profile(int32_t entity, int64_t profileId) {
    return call(AVER_FW_PHYSANIM_OP_PROFILE, entity, profileId, 0, nullptr, 0);
}

int32_t aver_fw_physanim_blend(int32_t entity, float weight, float seconds) {
    const float v[2] = {weight, seconds};
    return call(AVER_FW_PHYSANIM_OP_BLEND, entity, 0, 0, v, 2);
}

int32_t aver_fw_physanim_simulate_subtree(int32_t entity, int64_t boneHash) {
    return call(AVER_FW_PHYSANIM_OP_SIMULATE, entity, boneHash, 0, nullptr, 0);
}

int32_t aver_fw_physanim_set_part(int32_t entity, int64_t boneHash, int32_t kind, float value) {
    return call(AVER_FW_PHYSANIM_OP_PART, entity, boneHash, kind, &value, 1);
}

int32_t aver_fw_physanim_die(int32_t entity) { return call(AVER_FW_PHYSANIM_OP_DIE, entity, 0, 0, nullptr, 0); }

int32_t aver_fw_physanim_get_up(int32_t entity) { return call(AVER_FW_PHYSANIM_OP_GET_UP, entity, 0, 0, nullptr, 0); }

int32_t aver_fw_physanim_state(int32_t entity, int32_t* outLife, int32_t* outFacing, int32_t* outReacting) {
    int64_t out[3] = {0, 0, 0};
    const int32_t r = call(AVER_FW_PHYSANIM_OP_STATE, entity, 0, 0, nullptr, 0, out);
    if (outLife) *outLife = static_cast<int32_t>(out[0]);
    if (outFacing) *outFacing = static_cast<int32_t>(out[1]);
    if (outReacting) *outReacting = static_cast<int32_t>(out[2]);
    return r;
}
