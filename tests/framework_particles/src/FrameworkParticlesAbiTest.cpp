// The particle C ABI (framework_particles_abi.h) end to end through the Aver.Framework DLL: with no provider every call
// answers 0 and writes nothing; with a provider every call forwards its exact arguments under the right op code and
// relays the return and the out values; bad arguments return 0 without reaching the provider; clearing the provider
// restores the zeros. The provider here is a fake, so no particle module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_particles_abi.h"

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
    int64_t h0 = 0, h1 = 0;
    float in[17] = {};
    bool hadIn = false, hasOutF = false, hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 0;

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, int64_t h0, int64_t h1, const float* in, float* outF,
                                 int32_t* outI, void* user) {
    const int calls = g_call.calls + 1;
    g_call = Call{};
    g_call.calls = calls;
    g_call.op = op; g_call.entity = entity; g_call.h0 = h0; g_call.h1 = h1;
    g_call.hadIn = in != nullptr;
    if (in) {
        const int n = op == AVER_FW_PARTICLES_OP_SPAWN_AT ? 17 : (op == AVER_FW_PARTICLES_OP_SET_PARAM ? 4 : 3);
        for (int i = 0; i < n; ++i) g_call.in[i] = in[i];
    }
    g_call.hasOutF = outF != nullptr;
    g_call.hasOutI = outI != nullptr;
    g_call.user = user;
    if (op == AVER_FW_PARTICLES_OP_STATE && outI) { outI[0] = 321; outI[1] = 1; }
    if (op == AVER_FW_PARTICLES_OP_STATS && outI) for (int i = 0; i < 6; ++i) outI[i] = 100 + i;
    return g_return;
}

int main() {
    AVER_INFO("FrameworkParticlesAbiTest");
    check(aver_fw_particles_abi_version() == 1 && AVER_FW_PARTICLES_ABI_VERSION == 1, "the particles ABI is version 1");

    // The op codes are the wire format between this DLL and a provider in another module.
    check(AVER_FW_PARTICLES_OP_SPAWN_AT == 1 && AVER_FW_PARTICLES_OP_SET_PARAM == 2 && AVER_FW_PARTICLES_OP_BURST == 3 &&
              AVER_FW_PARTICLES_OP_STOP == 4 && AVER_FW_PARTICLES_OP_MODIFIERS == 5 && AVER_FW_PARTICLES_OP_STATE == 6 &&
              AVER_FW_PARTICLES_OP_STATS == 7 && AVER_FW_PARTICLES_OP_BUDGET == 8 && AVER_FW_PARTICLES_OP_GPU_MODE == 9,
          "the op codes are pinned");

    AVER_INFO("=== no provider ===");
    {
        int32_t alive = -1, tier = -1, six[6] = {-1, -1, -1, -1, -1, -1};
        check(aver_fw_particles_spawn_at(99, 1, 2, 3, 0, 0, 1, 5) == 0, "spawn_at answers 0");
        const float args[16] = {};
        check(aver_fw_particles_spawn_at_ex(99, args, 5) == 0, "spawn_at_ex answers 0");
        check(aver_fw_particles_set_param(4, 77, 0, 1, 0, 0, 0) == 0, "set_param answers 0");
        check(aver_fw_particles_burst(4, 10) == 0, "burst answers 0");
        check(aver_fw_particles_stop(4, 1) == 0, "stop answers 0");
        check(aver_fw_particles_set_modifiers(4, 1, 1, 1) == 0, "set_modifiers answers 0");
        check(aver_fw_particles_emitter_state(4, &alive, &tier) == 0, "emitter_state answers 0");
        check(aver_fw_particles_stats(six) == 0, "stats answers 0");
        check(aver_fw_particles_set_budget(1, 2, 3) == 0, "set_budget answers 0");
        check(aver_fw_particles_set_gpu_mode(1) == 0, "set_gpu_mode answers 0");
        check(alive == -1 && tier == -1 && six[0] == -1 && six[5] == -1, "and writes nothing");
    }

    AVER_INFO("=== a provider receives exactly what was sent ===");
    {
        int token = 0;
        check(aver_fw_set_particles_provider(&fake, &token) == 1, "installing returns 1");
        g_return = 12;

        check(aver_fw_particles_spawn_at(0x1122334455667788ll, 1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f, 12) == 12, "spawn_at relays the accepted count");
        check(g_call.op == AVER_FW_PARTICLES_OP_SPAWN_AT && g_call.h0 == 0x1122334455667788ll && g_call.hadIn, "spawn_at: op and effect hash");
        check(g_call.in[0] == 1.0f && g_call.in[1] == 2.0f && g_call.in[2] == 3.0f && g_call.in[4] == 1.0f, "spawn_at: position and direction");
        check(g_call.in[6] == 0.0f && g_call.in[9] == 1.0f && g_call.in[10] == 1.0f && g_call.in[11] == 1.0f && g_call.in[15] == 1.0f,
              "spawn_at: velocity 0, scales 1, tint white");
        check(g_call.in[16] == 12.0f, "spawn_at: the count rides as the 17th float");
        check(g_call.user == &token, "and the user pointer comes back");

        float args[16] = {};
        for (int i = 0; i < 16; ++i) args[i] = static_cast<float>(i) + 0.5f;
        check(aver_fw_particles_spawn_at_ex(5, args, 3) == 12, "spawn_at_ex relays");
        check(g_call.in[0] == 0.5f && g_call.in[15] == 15.5f && g_call.in[16] == 3.0f && g_call.h0 == 5, "spawn_at_ex: all 16 args and the count");

        g_return = 1;
        check(aver_fw_particles_set_param(8, 0xABCDEFll, 2, 0.1f, 0.2f, 0.3f, 0.4f) == 1, "set_param relays");
        check(g_call.op == AVER_FW_PARTICLES_OP_SET_PARAM && g_call.entity == 8 && g_call.h0 == 0xABCDEFll && g_call.h1 == 2, "set_param: entity, name hash, kind");
        check(g_call.in[0] == 0.1f && g_call.in[3] == 0.4f, "set_param: four values");

        check(aver_fw_particles_burst(8, 25) == 1 && g_call.op == AVER_FW_PARTICLES_OP_BURST && g_call.entity == 8 && g_call.in[0] == 25.0f, "burst");
        check(aver_fw_particles_stop(8, 1) == 1 && g_call.op == AVER_FW_PARTICLES_OP_STOP && g_call.in[0] == 1.0f, "stop(kill)");
        check(aver_fw_particles_stop(8, 0) == 1 && g_call.in[0] == 0.0f, "stop(keep)");
        check(aver_fw_particles_set_modifiers(8, 0.5f, 2.0f, 3.0f) == 1 && g_call.op == AVER_FW_PARTICLES_OP_MODIFIERS &&
                  g_call.in[0] == 0.5f && g_call.in[1] == 2.0f && g_call.in[2] == 3.0f,
              "set_modifiers");

        int32_t alive = 0, tier = 0;
        check(aver_fw_particles_emitter_state(8, &alive, &tier) == 1 && alive == 321 && tier == 1 && g_call.hasOutI, "emitter_state relays the out values");
        int32_t six[6] = {};
        check(aver_fw_particles_stats(six) == 1 && six[0] == 100 && six[5] == 105, "stats relays six ints");

        check(aver_fw_particles_set_budget(-1, 500, 64) == 1 && g_call.op == AVER_FW_PARTICLES_OP_BUDGET &&
                  g_call.in[0] == -1.0f && g_call.in[1] == 500.0f && g_call.in[2] == 64.0f,
              "set_budget (negative keeps)");
        check(aver_fw_particles_set_gpu_mode(2) == 1 && g_call.op == AVER_FW_PARTICLES_OP_GPU_MODE && g_call.in[0] == 2.0f, "set_gpu_mode");

        AVER_INFO("=== bad arguments never reach the provider ===");
        const int before = g_call.calls;
        int32_t a2 = 5, t2 = 5;
        check(aver_fw_particles_spawn_at(1, 0, 0, 0, 0, 0, 1, 0) == 0, "a zero count is refused");
        check(aver_fw_particles_spawn_at(1, 0, 0, 0, 0, 0, 1, -4) == 0, "a negative count is refused");
        check(aver_fw_particles_spawn_at_ex(1, nullptr, 3) == 0, "null args are refused");
        check(aver_fw_particles_set_param(8, 1, 3, 0, 0, 0, 0) == 0, "an unknown parameter kind is refused");
        check(aver_fw_particles_burst(8, 0) == 0, "a zero burst is refused");
        check(aver_fw_particles_emitter_state(8, nullptr, &t2) == 0 && aver_fw_particles_emitter_state(8, &a2, nullptr) == 0, "null outputs are refused");
        check(aver_fw_particles_stats(nullptr) == 0, "a null stats output is refused");
        check(g_call.calls == before, "none of them reached the provider");

        g_return = 0;
        check(aver_fw_particles_burst(8, 3) == 0, "a refusal is relayed as 0");
        alive = -9; tier = -9;
        check(aver_fw_particles_emitter_state(8, &alive, &tier) == 0 && alive == -9 && tier == -9, "a refused state writes nothing");

        AVER_INFO("=== clearing the provider ===");
        const int before2 = g_call.calls;
        check(aver_fw_set_particles_provider(nullptr, nullptr) == 1, "clearing returns 1");
        g_return = 1;
        check(aver_fw_particles_burst(8, 3) == 0 && aver_fw_particles_spawn_at(1, 0, 0, 0, 0, 0, 1, 3) == 0, "every call is 0 again");
        check(g_call.calls == before2, "the old provider was not called");
    }

    AVER_INFO(g_failures == 0 ? "FrameworkParticlesAbiTest: PASS" : "FrameworkParticlesAbiTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
