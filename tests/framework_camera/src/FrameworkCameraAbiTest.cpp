// The gameplay camera C ABI (framework_camera_abi.h) end to end through the Aver.Framework DLL: with no provider every call
// answers 0 and writes nothing; with a provider every call forwards its exact arguments under the right op code (integers
// bit-packed into the float array) and relays the return and the out values; clearing the provider restores the zeros. The
// provider is a fake, so no camera module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_camera_abi.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

struct Call {
    int32_t op = 0, entity = 0;
    float in[16] = {};
    int32_t inCount = -1, cap = -1;
    bool hasOutF = false, hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 0;

static int32_t unpackInt(float f) {
    int32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, const float* in, int32_t inCount, float* outF, int32_t* outI, int32_t cap, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.inCount = inCount; c.cap = cap;
    c.hasOutF = outF != nullptr; c.hasOutI = outI != nullptr;
    c.user = user; c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 16 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (op == AVER_FW_CAM_OP_PUSH_MODIFIER && outI) outI[0] = 42;
    if (op == AVER_FW_CAM_OP_CLEAR_MODIFIERS && outI) outI[0] = 3;
    if (op == AVER_FW_CAM_OP_POSE && outF && outI) {
        for (int i = 0; i < 9; ++i) outF[i] = 1.0f + static_cast<float>(i);
        outI[0] = 7; outI[1] = 1;
    }
    if (op == AVER_FW_CAM_OP_RIG_STATE && outF && outI) { outF[0] = 321.5f; outF[1] = 0.25f; outI[0] = 0x11; }
    return g_return;
}

int main() {
    AVER_INFO("FrameworkCameraAbiTest");
    check(aver_fw_camera_abi_version() == 1 && AVER_FW_CAMERA_ABI_VERSION == 1, "the camera ABI is version 1");
    check(AVER_FW_CAM_OP_ATTACH == 1 && AVER_FW_CAM_OP_SET_VIEW_TARGET == 2 && AVER_FW_CAM_OP_CLEAR_VIEW_TARGET == 3 && AVER_FW_CAM_OP_PUSH_MODIFIER == 4 &&
              AVER_FW_CAM_OP_REMOVE_MODIFIER == 5 && AVER_FW_CAM_OP_CLEAR_MODIFIERS == 6 && AVER_FW_CAM_OP_SHAKE_AT == 7 && AVER_FW_CAM_OP_POSE == 8 &&
              AVER_FW_CAM_OP_RIG_STATE == 9 && AVER_FW_CAM_OP_CUT == 10,
          "the op codes are pinned");
    check(AVER_FW_CAM_MODE_CHASE == 0 && AVER_FW_CAM_MODE_FIXED == 5 && AVER_FW_CAM_PRESET_VEHICLECHASE == 1 && AVER_FW_CAM_PRESET_FIXED == 6 &&
              AVER_FW_CAM_CURVE_EASEOUT == 3 && AVER_FW_CAM_MOD_SOCKETOFFSET == 5 && AVER_FW_CAM_RIG_NOCOLLISION == 1 && AVER_FW_CAM_RIG_DISABLED == 0x40 &&
              AVER_FW_CAM_STATE_LAGGING == 0x10,
          "the enum constants are pinned");

    AVER_INFO("=== no provider ===");
    {
        int32_t id = -1, state = -1, rigState2[2] = {-1, -1};
        float pose[9], arm2[2] = {-1, -1};
        for (float& v : pose) v = -1.0f;
        check(aver_fw_camera_attach(5, 1, 6) == 0 && aver_fw_camera_set_view_target(5, 0.5f, 0, 0) == 0 && aver_fw_camera_clear_view_target(0.5f, 0) == 0 &&
                  aver_fw_camera_remove_modifier(3, -1.0f) == 0 && aver_fw_camera_clear_modifiers(0) == 0 && aver_fw_camera_shake_at(0, 0, 0, 100, 1, 2) == 0 &&
                  aver_fw_camera_cut(5) == 0,
              "every call answers 0");
        check(aver_fw_camera_push_modifier(0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, &id) == 0 && id == 0, "a refused push reports id 0");
        check(aver_fw_camera_pose(pose, rigState2) == 0 && pose[0] == -1.0f && rigState2[0] == -1, "pose leaves its outputs untouched");
        check(aver_fw_camera_rig_state(5, arm2, &state) == 0 && arm2[0] == -1.0f && state == -1, "rig_state leaves its outputs untouched");
    }

    int token = 0;
    check(aver_fw_set_camera_provider(&fake, &token) == 1, "installing a provider returns 1");
    g_return = 1;

    AVER_INFO("=== forwarding and packing ===");
    {
        check(aver_fw_camera_attach(5, 3, 987654321) == 1, "attach returns the provider's value");
        check(g_call.op == AVER_FW_CAM_OP_ATTACH && g_call.entity == 5 && g_call.inCount == 2 && unpackInt(g_call.in[0]) == 3 && unpackInt(g_call.in[1]) == 987654321 &&
                  g_call.user == &token,
              "attach: entity, then preset and target as raw int bits (a 31-bit entity id survives a float array)");

        check(aver_fw_camera_set_view_target(0x12345678, 0.8f, 1, 1) == 1, "set_view_target forwards");
        check(g_call.op == AVER_FW_CAM_OP_SET_VIEW_TARGET && g_call.entity == 0x12345678 && g_call.inCount == 3 && g_call.in[0] == 0.8f &&
                  unpackInt(g_call.in[1]) == 1 && unpackInt(g_call.in[2]) == 1,
              "set_view_target: blendSeconds as a float, curve and cut as ints");

        aver_fw_camera_clear_view_target(0.25f, 2);
        check(g_call.op == AVER_FW_CAM_OP_CLEAR_VIEW_TARGET && g_call.inCount == 2 && g_call.in[0] == 0.25f && unpackInt(g_call.in[1]) == 2, "clear_view_target: blend and curve");

        int32_t id = 0;
        check(aver_fw_camera_push_modifier(2, -3, 0.05f, 0.1f, 0.4f, 5.0f, 1.5f, -2.0f, 9.0f, 77, 1, &id) == 1 && id == 42, "push_modifier returns 1 and the provider's id");
        check(g_call.op == AVER_FW_CAM_OP_PUSH_MODIFIER && g_call.inCount == 11 && g_call.hasOutI && g_call.cap == 1 && unpackInt(g_call.in[0]) == 2 &&
                  unpackInt(g_call.in[1]) == -3 && g_call.in[2] == 0.05f && g_call.in[3] == 0.1f && g_call.in[4] == 0.4f && g_call.in[5] == 5.0f &&
                  g_call.in[6] == 1.5f && g_call.in[7] == -2.0f && g_call.in[8] == 9.0f && unpackInt(g_call.in[9]) == 77 && unpackInt(g_call.in[10]) == 1,
              "push_modifier: kind and priority ints, six floats, tag and flags ints");
        check(aver_fw_camera_push_modifier(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, nullptr) == 1, "a null outId is allowed");

        aver_fw_camera_remove_modifier(42, -1.0f);
        check(g_call.op == AVER_FW_CAM_OP_REMOVE_MODIFIER && g_call.inCount == 2 && unpackInt(g_call.in[0]) == 42 && g_call.in[1] == -1.0f, "remove_modifier: id then seconds");

        check(aver_fw_camera_clear_modifiers(9) == 3, "clear_modifiers returns the provider's count");
        check(g_call.op == AVER_FW_CAM_OP_CLEAR_MODIFIERS && g_call.inCount == 1 && unpackInt(g_call.in[0]) == 9, "...for the tag");

        aver_fw_camera_shake_at(1, 2, 3, 1500, 0.6f, 2);
        check(g_call.op == AVER_FW_CAM_OP_SHAKE_AT && g_call.inCount == 6 && g_call.in[0] == 1.0f && g_call.in[2] == 3.0f && g_call.in[3] == 1500.0f &&
                  g_call.in[4] == 0.6f && g_call.in[5] == 2.0f,
              "shake_at: position, radius, intensity, falloff");

        aver_fw_camera_cut(0);
        check(g_call.op == AVER_FW_CAM_OP_CUT && g_call.entity == 0, "cut(0) is the active rig");
    }

    AVER_INFO("=== reads ===");
    {
        float pose[9] = {};
        int32_t rs[2] = {};
        check(aver_fw_camera_pose(pose, rs) == 1, "pose returns 1");
        check(g_call.op == AVER_FW_CAM_OP_POSE && g_call.hasOutF && g_call.hasOutI && g_call.cap == 9, "...with room for nine floats and two ints");
        check(pose[0] == 1.0f && pose[4] == 5.0f && pose[8] == 9.0f && rs[0] == 7 && rs[1] == 1, "and fills nine floats and two ints");
        check(aver_fw_camera_pose(nullptr, nullptr) == 1, "null outputs are allowed");

        float arm[2] = {};
        int32_t state = 0;
        check(aver_fw_camera_rig_state(11, arm, &state) == 1 && g_call.entity == 11 && arm[0] == 321.5f && arm[1] == 0.25f && state == 0x11,
              "rig_state: arm and blend alpha, and the state bits");
    }

    AVER_INFO("=== refusal and clearing ===");
    {
        g_return = 0;
        check(aver_fw_camera_attach(5, 0, 0) == 0 && aver_fw_camera_cut(5) == 0, "a provider that refuses is relayed");
        int32_t id = 9;
        check(aver_fw_camera_push_modifier(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &id) == 0 && id == 0, "a refused push zeroes the id");
        check(aver_fw_set_camera_provider(nullptr, nullptr) == 1, "clearing the provider returns 1");
        const int calls = g_call.calls;
        check(aver_fw_camera_attach(5, 1, 0) == 0 && g_call.calls == calls, "and the zeros come back without reaching it");
    }

    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
