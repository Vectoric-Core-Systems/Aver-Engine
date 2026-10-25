// The strand hair C ABI (framework_hair_abi.h) end to end through the Aver.Framework DLL: with no provider every call answers 0; with a provider
// every call forwards its exact arguments under the right op code and relays the out values; the refusals that need no provider never reach it;
// clearing the provider restores the zeros. The provider is a fake, so no hair module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_hair_abi.h"

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
    int32_t op = 0, entity = -1;
    float in[16] = {};
    int32_t inCount = -1, cap = -1;
    std::string text;
    bool hasText = false, hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 1;
static int32_t g_outI[16] = {};

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, const float* in, int32_t inCount, const char* text, float* outF, int32_t* outI, int32_t cap, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.inCount = inCount; c.cap = cap; c.hasOutI = outI != nullptr; c.user = user;
    c.hasText = text != nullptr;
    if (text) c.text = text;
    c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 16 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (outI && cap > 0)
        for (int i = 0; i < cap && i < 16; ++i) outI[i] = g_outI[i];
    return g_return;
}

int main() {
    AVER_INFO("FrameworkHairAbiTest");
    check(aver_fw_hair_abi_version() == 1 && AVER_FW_HAIR_ABI_VERSION == 1, "the hair ABI is version 1");
    check(AVER_FW_HAIR_OP_TELEPORT == 1 && AVER_FW_HAIR_OP_ADD_COLLIDER == 2 && AVER_FW_HAIR_OP_CLEAR_COLLIDERS == 3 && AVER_FW_HAIR_OP_STATE == 4, "the op codes are pinned");
    check(AVER_FW_HAIR_SHAPE_SPHERE == 0 && AVER_FW_HAIR_SHAPE_CAPSULE == 1 && AVER_FW_HAIR_SHAPE_BOX == 2, "the shape codes are pinned");
    check(AVER_FW_HAIR_TI_STATE == 0 && AVER_FW_HAIR_TI_LOD == 1 && AVER_FW_HAIR_TI_RENDER_STRANDS == 2 && AVER_FW_HAIR_TI_SIM_STRANDS == 3 &&
              AVER_FW_HAIR_TI_COLLIDERS == 4 && AVER_FW_HAIR_TI_BUILT == 5 && AVER_FW_HAIR_TI_COUNT == 6,
          "the state indices are pinned");

    AVER_INFO("=== no provider ===");
    {
        int32_t six[6] = {-1, -1, -1, -1, -1, -1};
        int32_t n = -1;
        check(aver_fw_hair_teleport(1) == 0 && aver_fw_hair_add_collider(1, 0, 5, 0, 0, 0, 0, 0, "head", &n) == 0 && aver_fw_hair_clear_colliders(1) == 0,
              "every call answers 0");
        check(aver_fw_hair_state(1, six) == 0 && six[0] == -1 && n == -1, "the reads leave their outputs untouched");
    }

    int token = 0;
    check(aver_fw_set_hair_provider(&fake, &token) == 1, "installing a provider returns 1");

    AVER_INFO("=== forwarding ===");
    {
        g_return = 1;
        check(aver_fw_hair_teleport(7) == 1 && g_call.op == AVER_FW_HAIR_OP_TELEPORT && g_call.entity == 7 && g_call.user == &token, "teleport: op, entity, the user pointer");

        g_outI[0] = 2;
        int32_t count = 0;
        check(aver_fw_hair_add_collider(7, AVER_FW_HAIR_SHAPE_CAPSULE, 4, 6, 0, 1, 2, 3, "Head", &count) == 1 && g_call.op == AVER_FW_HAIR_OP_ADD_COLLIDER && g_call.inCount == 7 &&
                  g_call.in[0] == 1 && g_call.in[1] == 4 && g_call.in[2] == 6 && g_call.in[4] == 1 && g_call.in[6] == 3 && g_call.hasText && g_call.text == "Head" && g_call.hasOutI && count == 2,
              "add_collider: shape, size, offset, the bone as text and the count back");
        check(aver_fw_hair_add_collider(7, 0, 4, 0, 0, 0, 0, 0, "Head", nullptr) == 1, "a null count output is allowed");
        const int before = g_call.calls;
        check(aver_fw_hair_add_collider(7, 0, 4, 0, 0, 0, 0, 0, nullptr, nullptr) == 0 && aver_fw_hair_add_collider(7, 0, 4, 0, 0, 0, 0, 0, "", nullptr) == 0 && g_call.calls == before,
              "a null or empty bone never reaches the provider");

        check(aver_fw_hair_clear_colliders(7) == 1 && g_call.op == AVER_FW_HAIR_OP_CLEAR_COLLIDERS && g_call.entity == 7, "clear_colliders: op and entity");

        for (int i = 0; i < 6; ++i) g_outI[i] = 10 + i;
        int32_t six[6] = {};
        check(aver_fw_hair_state(7, six) == 1 && g_call.op == AVER_FW_HAIR_OP_STATE && g_call.cap == AVER_FW_HAIR_TI_COUNT && g_call.hasOutI && six[0] == 10 && six[5] == 15,
              "state: the six ints come back");
        check(aver_fw_hair_state(7, nullptr) == 1, "a null output is allowed");

        g_return = 0;
        for (int i = 0; i < 6; ++i) six[i] = -9;
        check(aver_fw_hair_state(7, six) == 0 && six[0] == -9, "a refusal leaves the output alone");
        check(aver_fw_hair_teleport(7) == 0 && aver_fw_hair_clear_colliders(7) == 0, "a refusal is relayed as 0");
        g_return = 1;
    }

    AVER_INFO("=== removal ===");
    {
        aver_fw_set_hair_provider(nullptr, nullptr);
        const int before = g_call.calls;
        check(aver_fw_hair_teleport(7) == 0 && g_call.calls == before, "clearing the provider restores the zeros and calls nothing");
    }

    if (g_failures == 0) AVER_INFO("FrameworkHairAbiTest: {} checks passed", g_checks);
    else                 AVER_ERROR("FrameworkHairAbiTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
