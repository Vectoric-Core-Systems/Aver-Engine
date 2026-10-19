// Gameplay camera C ABI: a thin relay to the provider a composition root installs.
// See framework_camera_abi.h and docs/CAMERAS.md.

#include "aver/framework/framework_camera_abi.h"

#include <cstring>

namespace {

aver_fw_camera_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, const float* in = nullptr, int32_t inCount = 0, float* outF = nullptr,
             int32_t* outI = nullptr, int32_t cap = 0) {
    return g_provider ? g_provider(op, entity, in, inCount, outF, outI, cap, g_providerUser) : 0;
}

// An integer argument travels as the bit pattern of an int32 (a float keeps 24 bits; an entity id needs 31).
float bits(int32_t v) {
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

} // namespace

int32_t aver_fw_camera_abi_version(void) { return AVER_FW_CAMERA_ABI_VERSION; }

int32_t aver_fw_set_camera_provider(aver_fw_camera_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_camera_attach(int32_t entity, int32_t preset, int32_t targetEntity) {
    const float in[2] = {bits(preset), bits(targetEntity)};
    return call(AVER_FW_CAM_OP_ATTACH, entity, in, 2);
}

int32_t aver_fw_camera_set_view_target(int32_t entity, float blendSeconds, int32_t curve, int32_t cut) {
    const float in[3] = {blendSeconds, bits(curve), bits(cut)};
    return call(AVER_FW_CAM_OP_SET_VIEW_TARGET, entity, in, 3);
}

int32_t aver_fw_camera_clear_view_target(float blendSeconds, int32_t curve) {
    const float in[2] = {blendSeconds, bits(curve)};
    return call(AVER_FW_CAM_OP_CLEAR_VIEW_TARGET, 0, in, 2);
}

int32_t aver_fw_camera_push_modifier(int32_t kind, int32_t priority, float inS, float holdS, float outS,
                                     float a, float b, float c, float d, int32_t tag, int32_t flags, int32_t* outId) {
    const float in[11] = {bits(kind), bits(priority), inS, holdS, outS, a, b, c, d, bits(tag), bits(flags)};
    int32_t id = 0;
    if (!call(AVER_FW_CAM_OP_PUSH_MODIFIER, 0, in, 11, nullptr, &id, 1)) {
        if (outId) *outId = 0;
        return 0;
    }
    if (outId) *outId = id;
    return id != 0 ? 1 : 0;
}

int32_t aver_fw_camera_remove_modifier(int32_t id, float outSeconds) {
    const float in[2] = {bits(id), outSeconds};
    return call(AVER_FW_CAM_OP_REMOVE_MODIFIER, 0, in, 2);
}

int32_t aver_fw_camera_clear_modifiers(int32_t tag) {
    const float in[1] = {bits(tag)};
    int32_t count = 0;
    if (!call(AVER_FW_CAM_OP_CLEAR_MODIFIERS, 0, in, 1, nullptr, &count, 1)) return 0;
    return count;
}

int32_t aver_fw_camera_shake_at(float x, float y, float z, float radiusCm, float intensity, float falloffPower) {
    const float in[6] = {x, y, z, radiusCm, intensity, falloffPower};
    return call(AVER_FW_CAM_OP_SHAKE_AT, 0, in, 6);
}

int32_t aver_fw_camera_pose(float* outPose9, int32_t* outRigAndState2) {
    float f[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    int32_t i[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};   // `cap` is the capacity of BOTH out arrays
    if (!call(AVER_FW_CAM_OP_POSE, 0, nullptr, 0, f, i, 9)) return 0;
    if (outPose9) std::memcpy(outPose9, f, sizeof f);
    if (outRigAndState2) { outRigAndState2[0] = i[0]; outRigAndState2[1] = i[1]; }
    return 1;
}

int32_t aver_fw_camera_rig_state(int32_t entity, float* outArmAndBlend2, int32_t* outState) {
    float f[2] = {0, 0};
    int32_t s[2] = {0, 0};
    if (!call(AVER_FW_CAM_OP_RIG_STATE, entity, nullptr, 0, f, s, 2)) return 0;
    if (outArmAndBlend2) { outArmAndBlend2[0] = f[0]; outArmAndBlend2[1] = f[1]; }
    if (outState) *outState = s[0];
    return 1;
}

int32_t aver_fw_camera_cut(int32_t entity) { return call(AVER_FW_CAM_OP_CUT, entity); }
