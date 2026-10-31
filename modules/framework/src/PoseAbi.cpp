// Pose seam C ABI: a thin relay to the provider a composition root installs.
// See framework_pose_abi.h and docs/ANIM_POSE_SEAM.md.

#include "aver/framework/framework_pose_abi.h"

namespace {

aver_fw_pose_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, int64_t h0, int64_t h1, const float* in, int32_t inCount,
             float* outF = nullptr, int64_t* outI = nullptr, char* outStr = nullptr, int32_t outStrLen = 0) {
    return g_provider ? g_provider(op, entity, h0, h1, 0, in, inCount, outF, outI, outStr, outStrLen, g_providerUser) : 0;
}

} // namespace

int32_t aver_fw_pose_abi_version(void) { return AVER_FW_POSE_ABI_VERSION; }

int32_t aver_fw_set_pose_provider(aver_fw_pose_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_pose_bone_count(int32_t entity) { return call(AVER_FW_POSE_OP_BONE_COUNT, entity, 0, 0, nullptr, 0); }

int32_t aver_fw_pose_find_bone(int32_t entity, int64_t nameHash) {
    int64_t idx = -1;
    return call(AVER_FW_POSE_OP_FIND, entity, nameHash, 0, nullptr, 0, nullptr, &idx) ? static_cast<int32_t>(idx) : -1;
}

int32_t aver_fw_pose_bone_info(int32_t entity, int32_t bone, char* nameBuf, int32_t nameBufLen, int32_t* outParent,
                               float* outRest10) {
    int64_t parent = -1;
    if (nameBuf && nameBufLen > 0) nameBuf[0] = '\0';
    float rest[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
    if (!call(AVER_FW_POSE_OP_INFO, entity, bone, 0, nullptr, 0, rest, &parent, nameBuf, nameBufLen)) return 0;
    if (outParent) *outParent = static_cast<int32_t>(parent);
    if (outRest10) for (int i = 0; i < 10; ++i) outRest10[i] = rest[i];
    return 1;
}

int32_t aver_fw_pose_read(int32_t entity, int32_t bone, int32_t readFlags, float* out10) {
    return out10 ? call(AVER_FW_POSE_OP_READ, entity, bone, readFlags, nullptr, 0, out10) : 0;
}

int32_t aver_fw_pose_write(int32_t entity, int32_t bone, int32_t flags, const float* in10, float amount) {
    if (!in10) return 0;
    float v[11];
    for (int i = 0; i < 10; ++i) v[i] = in10[i];
    v[10] = amount;
    return call(AVER_FW_POSE_OP_WRITE, entity, bone, flags, v, 11);
}

int32_t aver_fw_pose_write_range(int32_t entity, int32_t first, int32_t count, int32_t flags, const float* in) {
    if (!in || count <= 0) return 0;
    return call(AVER_FW_POSE_OP_WRITE_RANGE, entity, flags, first, in, count * 10);
}

int32_t aver_fw_pose_clear(int32_t entity, int32_t bone) { return call(AVER_FW_POSE_OP_CLEAR, entity, bone, 0, nullptr, 0); }

int32_t aver_fw_pose_set_weight(int32_t entity, float weight) {
    return call(AVER_FW_POSE_OP_WEIGHT, entity, 0, 0, &weight, 1);
}

int32_t aver_fw_pose_set_enabled(int32_t entity, int32_t enabled) {
    return call(AVER_FW_POSE_OP_ENABLE, entity, enabled ? 1 : 0, 0, nullptr, 0);
}

int32_t aver_fw_pose_override_count(int32_t entity) { return call(AVER_FW_POSE_OP_OVERRIDES, entity, 0, 0, nullptr, 0); }
