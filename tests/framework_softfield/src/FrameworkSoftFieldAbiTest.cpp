// The soft-body field C ABI (framework_softfield_abi.h) end to end through the Aver.Framework DLL: with no provider every call
// answers 0; with a provider every call forwards its exact arguments under the right op code and relays the out values; a spec
// with a value that is not finite, or a kind or shape out of range, is refused before the provider; clearing the provider
// restores the zeros. The provider is a fake, so no soft-body module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_softbody_abi.h"
#include "aver/framework/framework_softfield_abi.h"

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
    int32_t op = 0, entity = -1;
    float in[32] = {};
    int32_t inCount = -1;
    bool hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 1;
static int32_t g_outI0 = 0;

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, const float* in, int32_t inCount, float*, int32_t* outI, int32_t cap, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.inCount = inCount; c.hasOutI = outI != nullptr && cap > 0; c.user = user; c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 32 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (outI && cap > 0) outI[0] = g_outI0;
    return g_return;
}

static void fillSpec(float* s) {
    for (int i = 0; i < AVER_FW_SF_SPEC_COUNT; ++i) s[i] = static_cast<float>(i) + 0.5f;
    s[0] = 2;    // kind: damage
    s[1] = 1;    // shape: box
}

int main() {
    AVER_INFO("FrameworkSoftFieldAbiTest");
    check(aver_fw_softfield_abi_version() == 1 && AVER_FW_SOFTFIELD_ABI_VERSION == 1, "the soft-field ABI is version 1");
    check(AVER_FW_SF_SPEC_COUNT == 24, "a spec is 24 floats");
    check(AVER_FW_SF_OP_PULSE == 1 && AVER_FW_SF_OP_START == 2 && AVER_FW_SF_OP_STOP == 3 && AVER_FW_SF_OP_IMPACT == 4 && AVER_FW_SF_OP_SPAWN == 5, "the op codes are pinned");
    check(aver_fw_softbody_abi_version() == 2 && AVER_FW_SOFTBODY_ABI_VERSION == 2, "the soft-body ABI is version 2");
    check(AVER_FW_SB_OP_ASLEEP == 11 && AVER_FW_SB_OP_CLEAR_DEBRIS == 12 && AVER_FW_SB_OP_EFFECTS == 13 && AVER_FW_SB_OP_DENTS == 14 && AVER_FW_SB_OP_REPAIR_ALL == 15,
          "and its five new ops are 11 to 15");

    float spec[AVER_FW_SF_SPEC_COUNT];
    fillSpec(spec);

    AVER_INFO("=== no provider ===");
    {
        check(aver_fw_softfield_pulse(spec, 24) == 0 && aver_fw_softfield_start(spec, 24) == 0 && aver_fw_softfield_stop(3) == 0 &&
                  aver_fw_softfield_impact(0, 0, 0, 0, 0, 1, 40, 500, 0) == 0 && aver_fw_softfield_spawn(spec, 24) == 0,
              "every field call answers 0");
        float maxSet = -1.0f;
        int32_t two[2] = {-1, -1};
        check(aver_fw_softbody_dents(1, &maxSet, two) == 0 && maxSet == -1.0f && two[0] == -1, "Dents leaves its outputs untouched");
        check(aver_fw_softbody_set_asleep(1, 1) == 0 && aver_fw_softbody_clear_debris(1) == 0 && aver_fw_softbody_set_effects(1) == 0 && aver_fw_softbody_repair_all() == 0,
              "and the other version-2 calls answer 0");
    }

    int token = 0;
    check(aver_fw_set_softfield_provider(&fake, &token) == 1, "installing a provider returns 1");

    AVER_INFO("=== forwarding ===");
    {
        g_return = 1;
        g_outI0 = 7;
        check(aver_fw_softfield_pulse(spec, 24) == 7 && g_call.op == AVER_FW_SF_OP_PULSE && g_call.inCount == 24 && g_call.user == &token && g_call.hasOutI,
              "pulse: op, the 24 floats, the user pointer, the pieces in reach back");
        bool all = true;
        for (int i = 0; i < 24; ++i) all = all && g_call.in[i] == spec[i];
        check(all, "and every float of the spec arrives in order");

        g_outI0 = 42;
        check(aver_fw_softfield_start(spec, 24) == 42 && g_call.op == AVER_FW_SF_OP_START && g_call.inCount == 24, "start: the handle comes back");
        g_outI0 = 0;
        check(aver_fw_softfield_start(spec, 24) == 0, "a zero handle is a refusal");

        g_outI0 = 0;
        check(aver_fw_softfield_stop(42) == 1 && g_call.op == AVER_FW_SF_OP_STOP && g_call.inCount == 1 && g_call.in[0] == 42.0f, "stop: the handle as the first float");

        g_outI0 = 3;
        check(aver_fw_softfield_impact(1, 2, 3, 0.5f, 0.25f, -1, 40, 500, 120) == 3 && g_call.op == AVER_FW_SF_OP_IMPACT && g_call.inCount == 9 &&
                  g_call.in[0] == 1 && g_call.in[1] == 2 && g_call.in[2] == 3 && g_call.in[3] == 0.5f && g_call.in[4] == 0.25f && g_call.in[5] == -1 &&
                  g_call.in[6] == 40 && g_call.in[7] == 500 && g_call.in[8] == 120,
              "impact: point, direction, radius, energy, kick, and the pieces in reach");

        g_outI0 = 12345;
        check(aver_fw_softfield_spawn(spec, 24) == 12345 && g_call.op == AVER_FW_SF_OP_SPAWN && g_call.inCount == 24, "spawn: the entity comes back");

        g_outI0 = 9;
        check(aver_fw_softfield_pulse(spec, 30) == 9 && g_call.inCount == 24, "a longer array still sends exactly 24 floats");
    }

    AVER_INFO("=== refusals before the provider ===");
    {
        const int before = g_call.calls;
        float bad[AVER_FW_SF_SPEC_COUNT];
        fillSpec(bad);
        bad[16] = std::nanf("");
        check(aver_fw_softfield_pulse(bad, 24) == 0 && aver_fw_softfield_start(bad, 24) == 0 && aver_fw_softfield_spawn(bad, 24) == 0, "a NaN strength is refused");
        fillSpec(bad);
        bad[5] = INFINITY;
        check(aver_fw_softfield_pulse(bad, 24) == 0, "an infinite centre is refused");
        fillSpec(bad);
        bad[0] = 6;
        check(aver_fw_softfield_pulse(bad, 24) == 0, "kind 6 is out of range");
        fillSpec(bad);
        bad[0] = -1;
        check(aver_fw_softfield_pulse(bad, 24) == 0, "kind -1 is out of range");
        fillSpec(bad);
        bad[1] = 4;
        check(aver_fw_softfield_start(bad, 24) == 0, "shape 4 is out of range");
        check(aver_fw_softfield_pulse(spec, 23) == 0 && aver_fw_softfield_pulse(nullptr, 24) == 0, "a short or null spec is refused");
        check(aver_fw_softfield_impact(0, 0, 0, 0, 0, 1, std::nanf(""), 500, 0) == 0, "an impact with a NaN radius is refused");
        check(g_call.calls == before, "none of these reached the provider");
    }

    AVER_INFO("=== the soft-body ops of version 2 ===");
    {
        // The soft-body provider has its own fake; install one here to see the argument layout.
        struct Sb {
            static int32_t AVER_FW_CALL fn(int32_t op, int32_t entity, const float* in, int32_t inCount, float* outF, int32_t* outI, int32_t cap, void*) {
                g_call.op = op; g_call.entity = entity; g_call.inCount = inCount;
                if (in && inCount > 0) g_call.in[0] = in[0];
                if (op == AVER_FW_SB_OP_DENTS && outF && outI && cap >= 2) { outF[0] = 1.5f; outI[0] = 6; outI[1] = 4; }
                if (op == AVER_FW_SB_OP_REPAIR_ALL && outI && cap >= 1) outI[0] = 9;
                return 1;
            }
        };
        aver_fw_set_softbody_provider(&Sb::fn, nullptr);
        check(aver_fw_softbody_set_asleep(5, 1) == 1 && g_call.op == AVER_FW_SB_OP_ASLEEP && g_call.entity == 5 && g_call.in[0] == 1.0f, "set_asleep: entity and flag");
        check(aver_fw_softbody_set_asleep(5, 0) == 1 && g_call.in[0] == 0.0f, "and 0 releases");
        check(aver_fw_softbody_clear_debris(6) == 1 && g_call.op == AVER_FW_SB_OP_CLEAR_DEBRIS && g_call.entity == 6, "clear_debris: the entity");
        check(aver_fw_softbody_set_effects(0) == 1 && g_call.op == AVER_FW_SB_OP_EFFECTS && g_call.in[0] == 0.0f, "set_effects: the switch");
        float maxSet = 0.0f;
        int32_t two[2] = {};
        check(aver_fw_softbody_dents(6, &maxSet, two) == 1 && g_call.op == AVER_FW_SB_OP_DENTS && maxSet == 1.5f && two[0] == 6 && two[1] == 4, "dents: max set, dented edges, debris pieces");
        check(aver_fw_softbody_repair_all() == 9 && g_call.op == AVER_FW_SB_OP_REPAIR_ALL, "repair_all: how many");
        aver_fw_set_softbody_provider(nullptr, nullptr);
    }

    AVER_INFO("=== removal ===");
    {
        aver_fw_set_softfield_provider(nullptr, nullptr);
        const int before = g_call.calls;
        check(aver_fw_softfield_pulse(spec, 24) == 0 && g_call.calls == before, "clearing the provider restores the zeros and calls nothing");
    }

    if (g_failures == 0) AVER_INFO("FrameworkSoftFieldAbiTest: {} checks passed", g_checks);
    else                 AVER_ERROR("FrameworkSoftFieldAbiTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
