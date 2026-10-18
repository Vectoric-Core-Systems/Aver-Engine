// The blend-shape C ABI (framework_morph_abi.h) end to end through the Aver.Framework DLL: with no provider every call
// answers 0 and writes nothing; with a provider every call forwards its exact arguments under the right op code and relays
// the return and the out values; clearing the provider restores the zeros. The provider is a fake, so no animation module is
// involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_morph_abi.h"

#include <cmath>
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
    int64_t h0 = 0, h1 = 0, h2 = 0;
    float in[8] = {};
    int32_t inCount = -1;
    bool hasOutF = false, hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 0;

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, int64_t h0, int64_t h1, int64_t h2, const float* in, int32_t inCount,
                                 float* outF, int64_t* outI, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.h0 = h0; c.h1 = h1; c.h2 = h2; c.inCount = inCount;
    c.hasOutF = outF != nullptr; c.hasOutI = outI != nullptr;
    c.user = user; c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 8 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (op == AVER_FW_MORPH_OP_GET_WEIGHT && outF) outF[0] = 0.375f;
    if (op == AVER_FW_MORPH_OP_LIPSYNC_STATE && outF) { outF[0] = 1.5f; outF[1] = 4.0f; outF[2] = 0.75f; }
    if (op == AVER_FW_MORPH_OP_INFO) {
        if (outI) { outI[0] = 16; outI[1] = 3; outI[2] = 0x7777; }
        if (outF) outF[0] = 1.25f;
    }
    return g_return;
}

int main() {
    AVER_INFO("FrameworkMorphAbiTest");
    check(aver_fw_morph_abi_version() == 1 && AVER_FW_MORPH_ABI_VERSION == 1, "the morph ABI is version 1");
    check(AVER_FW_MORPH_OP_SET_TARGET == 1 && AVER_FW_MORPH_OP_SET_POSE == 2 && AVER_FW_MORPH_OP_CLEAR == 3 &&
              AVER_FW_MORPH_OP_GET_WEIGHT == 4 && AVER_FW_MORPH_OP_SET_RIG == 5 && AVER_FW_MORPH_OP_LIPSYNC_PLAY == 6 &&
              AVER_FW_MORPH_OP_LIPSYNC_STOP == 7 && AVER_FW_MORPH_OP_LIPSYNC_TIME == 8 && AVER_FW_MORPH_OP_LIPSYNC_QUEUE == 9 &&
              AVER_FW_MORPH_OP_LIPSYNC_STATE == 10 && AVER_FW_MORPH_OP_INFO == 11,
          "the op codes are pinned");

    AVER_INFO("=== no provider ===");
    {
        float w = -1.0f, s3[3] = {-1, -1, -1}, m = -1.0f;
        int64_t i3[3] = {-1, -1, -1};
        check(aver_fw_morph_set_target(1, 2, 0.5f, 0.1f) == 0 && aver_fw_morph_set_pose(1, 2, 0.5f, 0.1f) == 0 &&
                  aver_fw_morph_clear(1, 0, 3, 0.0f) == 0 && aver_fw_morph_set_rig(1, 2) == 0,
              "the weight and rig calls answer 0");
        check(aver_fw_morph_lipsync_play(1, 2, 1, 0, 1) == 0 && aver_fw_morph_lipsync_stop(1, 0.1f) == 0 &&
                  aver_fw_morph_lipsync_set_time(1, 0.5f) == 0 && aver_fw_morph_lipsync_queue(1, 2, 0, 0.1f, 1) == 0,
              "the lip-sync calls answer 0");
        check(aver_fw_morph_get_weight(1, 2, &w) == 0 && w == -1.0f, "GET_WEIGHT leaves its output untouched");
        check(aver_fw_morph_lipsync_state(1, s3) == 0 && s3[0] == -1 && s3[2] == -1, "LIPSYNC_STATE leaves its output untouched");
        check(aver_fw_morph_info(1, i3, &m) == 0 && i3[0] == -1 && m == -1.0f, "INFO leaves its outputs untouched");
    }

    int token = 0;
    check(aver_fw_set_morph_provider(&fake, &token) == 1, "installing a provider returns 1");
    g_return = 1;

    AVER_INFO("=== weights ===");
    {
        check(aver_fw_morph_set_target(7, 0x1111, 0.25f, 0.5f) == 1 && g_call.op == AVER_FW_MORPH_OP_SET_TARGET && g_call.entity == 7 &&
                  g_call.h0 == 0x1111 && g_call.inCount == 2 && g_call.in[0] == 0.25f && g_call.in[1] == 0.5f && g_call.user == &token,
              "set_target: op, entity, hash, weight and blend, the user pointer");
        aver_fw_morph_set_pose(7, 0x2222, 1.0f, 0.0f);
        check(g_call.op == AVER_FW_MORPH_OP_SET_POSE && g_call.h0 == 0x2222 && g_call.inCount == 2 && g_call.in[0] == 1.0f && g_call.in[1] == 0.0f,
              "set_pose: hash, weight, blend");
        aver_fw_morph_clear(7, 0x3333, 2, 0.2f);
        check(g_call.op == AVER_FW_MORPH_OP_CLEAR && g_call.h0 == 0x3333 && g_call.h1 == 2 && g_call.inCount == 1 && g_call.in[0] == 0.2f,
              "clear: name hash, kind mask, blend");
        aver_fw_morph_clear(7, 0, 3, 0.0f);
        check(g_call.h0 == 0 && g_call.h1 == 3, "clear: hash 0 and mask 3 mean every override");
        float w = 0.0f;
        check(aver_fw_morph_get_weight(7, 0x4444, &w) == 1 && g_call.op == AVER_FW_MORPH_OP_GET_WEIGHT && g_call.h0 == 0x4444 && g_call.hasOutF &&
                  g_call.inCount == 0 && w == 0.375f,
              "get_weight relays the final weight");
        aver_fw_morph_set_rig(7, 0x5555);
        check(g_call.op == AVER_FW_MORPH_OP_SET_RIG && g_call.h0 == 0x5555 && g_call.inCount == 0, "set_rig: the asset id");
    }

    AVER_INFO("=== lip-sync ===");
    {
        aver_fw_morph_lipsync_play(3, 0xAAAA, 1.5f, 0.25f, 0.8f);
        check(g_call.op == AVER_FW_MORPH_OP_LIPSYNC_PLAY && g_call.h0 == 0xAAAA && g_call.inCount == 3 && g_call.in[0] == 1.5f &&
                  g_call.in[1] == 0.25f && g_call.in[2] == 0.8f,
              "play: track, rate, start, strength");
        aver_fw_morph_lipsync_stop(3, 0.3f);
        check(g_call.op == AVER_FW_MORPH_OP_LIPSYNC_STOP && g_call.inCount == 1 && g_call.in[0] == 0.3f, "stop: blend-out seconds");
        aver_fw_morph_lipsync_set_time(3, 2.5f);
        check(g_call.op == AVER_FW_MORPH_OP_LIPSYNC_TIME && g_call.inCount == 1 && g_call.in[0] == 2.5f, "set_time: seconds");
        aver_fw_morph_lipsync_queue(3, 0xBBBB, 0.1f, 0.2f, 0.9f);
        check(g_call.op == AVER_FW_MORPH_OP_LIPSYNC_QUEUE && g_call.h0 == 0xBBBB && g_call.inCount == 3 && g_call.in[0] == 0.1f &&
                  g_call.in[1] == 0.2f && g_call.in[2] == 0.9f,
              "queue: symbol, start, duration, intensity");
        float st[3] = {};
        check(aver_fw_morph_lipsync_state(3, st) == 1 && g_call.op == AVER_FW_MORPH_OP_LIPSYNC_STATE && g_call.hasOutF && st[0] == 1.5f &&
                  st[1] == 4.0f && st[2] == 0.75f,
              "state relays time, length and strength");
    }

    AVER_INFO("=== info ===");
    {
        int64_t i3[3] = {};
        float m = 0.0f;
        check(aver_fw_morph_info(3, i3, &m) == 1 && g_call.op == AVER_FW_MORPH_OP_INFO && g_call.hasOutF && g_call.hasOutI && i3[0] == 16 &&
                  i3[1] == 3 && i3[2] == 0x7777 && m == 1.25f,
              "info relays the target count, rig pose count, rig hash and master weight");
    }

    AVER_INFO("=== refusals and removal ===");
    {
        g_return = 0;
        check(aver_fw_morph_set_target(7, 1, 1, 0) == 0, "a provider refusal is relayed as 0");
        g_return = 1;
        aver_fw_set_morph_provider(nullptr, nullptr);
        const int before = g_call.calls;
        check(aver_fw_morph_set_target(7, 1, 1, 0) == 0 && g_call.calls == before, "clearing the provider restores the zeros and calls nothing");
    }

    if (g_failures == 0) AVER_INFO("FrameworkMorphAbiTest: {} checks passed", g_checks);
    else                 AVER_ERROR("FrameworkMorphAbiTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
