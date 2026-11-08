// The physical animation C ABI (framework_physanim_abi.h) end to end through the Aver.Framework DLL: with no provider
// every call answers 0 and the state reads Alive / Upright / 0; with a provider every call forwards its exact arguments
// under the right op code and relays the return and out values; clearing the provider restores the zeros. The provider
// is a fake, so no ragdoll module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_physanim_abi.h"

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
    int64_t h0 = 0, h1 = 0;
    float in[16] = {};
    int32_t inCount = -1;
    bool hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 0;

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, int64_t h0, int64_t h1, const float* in, int32_t inCount,
                                 int64_t* outI, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.h0 = h0; c.h1 = h1; c.inCount = inCount;
    c.hasOutI = outI != nullptr;
    c.user = user; c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 16 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (op == AVER_FW_PHYSANIM_OP_STATE && outI) { outI[0] = 2; outI[1] = 3; outI[2] = 7; }
    return g_return;
}

int main() {
    AVER_INFO("FrameworkPhysAnimAbiTest");
    check(aver_fw_physanim_abi_version() == 1 && AVER_FW_PHYSANIM_ABI_VERSION == 1, "the physical animation ABI is version 1");
    check(AVER_FW_PHYSANIM_OP_HIT == 1 && AVER_FW_PHYSANIM_OP_PROFILE == 2 && AVER_FW_PHYSANIM_OP_BLEND == 3 && AVER_FW_PHYSANIM_OP_SIMULATE == 4 &&
              AVER_FW_PHYSANIM_OP_PART == 5 && AVER_FW_PHYSANIM_OP_DIE == 6 && AVER_FW_PHYSANIM_OP_GET_UP == 7 && AVER_FW_PHYSANIM_OP_STATE == 8,
          "the op codes are pinned");
    check(AVER_FW_PHYSANIM_PART_STRENGTH == 0 && AVER_FW_PHYSANIM_PART_DAMPING == 1 && AVER_FW_PHYSANIM_PART_TORQUE == 2 &&
              AVER_FW_PHYSANIM_PART_BLEND == 3 && AVER_FW_PHYSANIM_PART_PINNED == 4,
          "the part kinds are pinned");

    AVER_INFO("=== no provider ===");
    {
        int32_t life = 9, facing = 9, reacting = 9;
        check(aver_fw_physanim_hit(1, 2, 1, 2, 3, 4, 5, 6, 7, 8) == 0 && aver_fw_physanim_set_profile(1, 5) == 0 && aver_fw_physanim_blend(1, 1, 1) == 0 &&
                  aver_fw_physanim_simulate_subtree(1, 3) == 0 && aver_fw_physanim_set_part(1, 3, 0, 0.5f) == 0 && aver_fw_physanim_die(1) == 0 &&
                  aver_fw_physanim_get_up(1) == 0,
              "every call answers 0");
        check(aver_fw_physanim_state(1, &life, &facing, &reacting) == 0 && life == 0 && facing == 0 && reacting == 0,
              "state answers 0 and reads Alive, Upright, 0");
        check(aver_fw_physanim_state(1, nullptr, nullptr, nullptr) == 0, "state accepts null outputs");
    }

    int token = 0;
    check(aver_fw_set_physanim_provider(&fake, &token) == 1, "installing a provider returns 1");
    g_return = 1;

    AVER_INFO("=== calls ===");
    {
        check(aver_fw_physanim_hit(9, 0xABCD, 1, 2, 3, 4, 5, 6, 7, 8) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_HIT && g_call.entity == 9 &&
                  g_call.h0 == 0xABCD && g_call.inCount == 8 && g_call.in[0] == 1 && g_call.in[2] == 3 && g_call.in[3] == 4 && g_call.in[5] == 6 &&
                  g_call.in[6] == 7 && g_call.in[7] == 8 && g_call.user == &token,
              "hit: bone hash, impulse, point, damage, radius, user pointer");
        g_return = 0;
        check(aver_fw_physanim_hit(9, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0, "a refused hit answers 0");
        g_return = 1;
        check(aver_fw_physanim_set_profile(9, 0x1234567890ll) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_PROFILE && g_call.h0 == 0x1234567890ll, "set_profile: the 64 bit id");
        check(aver_fw_physanim_blend(9, 0.75f, 0.5f) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_BLEND && g_call.inCount == 2 && g_call.in[0] == 0.75f && g_call.in[1] == 0.5f,
              "blend: weight and seconds");
        check(aver_fw_physanim_simulate_subtree(9, 77) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_SIMULATE && g_call.h0 == 77, "simulate_subtree: the bone hash");
        check(aver_fw_physanim_set_part(9, 77, AVER_FW_PHYSANIM_PART_PINNED, 1.0f) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_PART && g_call.h0 == 77 &&
                  g_call.h1 == AVER_FW_PHYSANIM_PART_PINNED && g_call.inCount == 1 && g_call.in[0] == 1.0f,
              "set_part: bone hash, kind and value");
        check(aver_fw_physanim_die(9) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_DIE && g_call.entity == 9, "die");
        check(aver_fw_physanim_get_up(9) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_GET_UP, "get_up");
        int32_t life = -1, facing = -1, reacting = -1;
        check(aver_fw_physanim_state(9, &life, &facing, &reacting) == 1 && g_call.op == AVER_FW_PHYSANIM_OP_STATE && g_call.hasOutI &&
                  life == 2 && facing == 3 && reacting == 7,
              "state: life, facing and the reacting count come back");
    }

    AVER_INFO("=== clearing the provider ===");
    {
        const int calls = g_call.calls;
        check(aver_fw_set_physanim_provider(nullptr, nullptr) == 1, "clearing returns 1");
        check(aver_fw_physanim_die(1) == 0 && g_call.calls == calls, "and the calls answer 0 without reaching it");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
