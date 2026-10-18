// Blend-shape C ABI: a thin relay to the provider a composition root installs.
// See framework_morph_abi.h and docs/ANIM_MORPH_FACE.md.

#include "aver/framework/framework_morph_abi.h"

namespace {

aver_fw_morph_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, int64_t h0, int64_t h1, int64_t h2, const float* in, int32_t inCount,
             float* outF = nullptr, int64_t* outI = nullptr) {
    return g_provider ? g_provider(op, entity, h0, h1, h2, in, inCount, outF, outI, g_providerUser) : 0;
}

} // namespace

int32_t aver_fw_morph_abi_version(void) { return AVER_FW_MORPH_ABI_VERSION; }

int32_t aver_fw_set_morph_provider(aver_fw_morph_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_morph_set_target(int32_t entity, int64_t target, float weight, float blendSeconds) {
    const float v[2] = {weight, blendSeconds};
    return call(AVER_FW_MORPH_OP_SET_TARGET, entity, target, 0, 0, v, 2);
}

int32_t aver_fw_morph_set_pose(int32_t entity, int64_t pose, float weight, float blendSeconds) {
    const float v[2] = {weight, blendSeconds};
    return call(AVER_FW_MORPH_OP_SET_POSE, entity, pose, 0, 0, v, 2);
}

int32_t aver_fw_morph_clear(int32_t entity, int64_t name, int32_t kindMask, float blendSeconds) {
    return call(AVER_FW_MORPH_OP_CLEAR, entity, name, kindMask, 0, &blendSeconds, 1);
}

int32_t aver_fw_morph_get_weight(int32_t entity, int64_t target, float* outWeight) {
    return call(AVER_FW_MORPH_OP_GET_WEIGHT, entity, target, 0, 0, nullptr, 0, outWeight);
}

int32_t aver_fw_morph_set_rig(int32_t entity, int64_t rigAsset) {
    return call(AVER_FW_MORPH_OP_SET_RIG, entity, rigAsset, 0, 0, nullptr, 0);
}

int32_t aver_fw_morph_lipsync_play(int32_t entity, int64_t track, float rate, float startSeconds, float strength) {
    const float v[3] = {rate, startSeconds, strength};
    return call(AVER_FW_MORPH_OP_LIPSYNC_PLAY, entity, track, 0, 0, v, 3);
}

int32_t aver_fw_morph_lipsync_stop(int32_t entity, float blendOutSeconds) {
    return call(AVER_FW_MORPH_OP_LIPSYNC_STOP, entity, 0, 0, 0, &blendOutSeconds, 1);
}

int32_t aver_fw_morph_lipsync_set_time(int32_t entity, float seconds) {
    return call(AVER_FW_MORPH_OP_LIPSYNC_TIME, entity, 0, 0, 0, &seconds, 1);
}

int32_t aver_fw_morph_lipsync_queue(int32_t entity, int64_t symbol, float startSeconds, float durationSeconds, float intensity) {
    const float v[3] = {startSeconds, durationSeconds, intensity};
    return call(AVER_FW_MORPH_OP_LIPSYNC_QUEUE, entity, symbol, 0, 0, v, 3);
}

int32_t aver_fw_morph_lipsync_state(int32_t entity, float* outTimeLengthStrength) {
    return call(AVER_FW_MORPH_OP_LIPSYNC_STATE, entity, 0, 0, 0, nullptr, 0, outTimeLengthStrength);
}

int32_t aver_fw_morph_info(int32_t entity, int64_t* outI3, float* outF1) {
    return call(AVER_FW_MORPH_OP_INFO, entity, 0, 0, 0, nullptr, 0, outF1, outI3);
}
