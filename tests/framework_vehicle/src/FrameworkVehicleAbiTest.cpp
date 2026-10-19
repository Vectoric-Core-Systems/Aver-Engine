// The vehicle C ABI (framework_vehicle_abi.h) end to end through the Aver.Framework DLL: with no provider every call answers 0
// and writes nothing; with a provider every call forwards its arguments under the right op code (integers as the bit pattern of
// an int32 in a float slot), the text of spawn and tune arrives intact, and the outs round-trip. The provider is a fake, so no
// vehicle module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_vehicle_abi.h"

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
    float in[8] = {};
    int32_t inCount = -1;
    std::string text;
    bool hasText = false;
    int32_t cap = -1;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 1;

static int32_t bitsOf(float f) {
    int32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, const float* in, int32_t inCount, const char* text, float* outF, int32_t* outI,
                                 int32_t cap, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.inCount = inCount; c.cap = cap; c.user = user; c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 8 && in; ++i) c.in[i] = in[i];
    c.hasText = text != nullptr;
    if (text) c.text = text;
    g_call = c;
    if (op == AVER_FW_VEH_OP_SPAWN && outI && cap >= 2) { outI[0] = 4242; outI[1] = 17; }
    if (op == AVER_FW_VEH_OP_EXIT && outI) outI[0] = 99;
    if (op == AVER_FW_VEH_OP_TELEMETRY && outF && outI) {
        for (int k = 0; k < AVER_FW_VEH_TF_COUNT; ++k) outF[k] = static_cast<float>(k) + 0.5f;
        for (int k = 0; k < AVER_FW_VEH_TI_COUNT; ++k) outI[k] = 100 + k;
    }
    if (op == AVER_FW_VEH_OP_WHEEL && outF && outI) {
        for (int k = 0; k < AVER_FW_VEH_WF_COUNT; ++k) outF[k] = static_cast<float>(k) * 2.0f;
        for (int k = 0; k < AVER_FW_VEH_WI_COUNT; ++k) outI[k] = 200 + k;
    }
    if (op == AVER_FW_VEH_OP_EVENTS && outF && outI) {
        for (int k = 0; k < AVER_FW_VEH_EVENT_INTS; ++k) outI[k] = 10 + k;
        for (int k = 0; k < AVER_FW_VEH_EVENT_FLOATS; ++k) outF[k] = 0.25f * static_cast<float>(k);
        return 1;
    }
    return g_return;
}

int main() {
    AVER_INFO("FrameworkVehicleAbiTest");
    check(aver_fw_vehicle_abi_version() == 1 && AVER_FW_VEHICLE_ABI_VERSION == 1, "the vehicle ABI is version 1");
    check(AVER_FW_VEH_OP_SPAWN == 1 && AVER_FW_VEH_OP_ENTER == 2 && AVER_FW_VEH_OP_EXIT == 3 && AVER_FW_VEH_OP_SHIFT == 4 &&
              AVER_FW_VEH_OP_SET_GEAR == 5 && AVER_FW_VEH_OP_SET_MANUAL == 6 && AVER_FW_VEH_OP_FLIP == 7 && AVER_FW_VEH_OP_REPAIR == 8 &&
              AVER_FW_VEH_OP_DAMAGE == 9 && AVER_FW_VEH_OP_TUNE == 10 && AVER_FW_VEH_OP_TELEMETRY == 11 && AVER_FW_VEH_OP_WHEEL == 12 &&
              AVER_FW_VEH_OP_EVENTS == 13,
          "the op codes are pinned");

    AVER_INFO("=== no provider ===");
    {
        int32_t ent = -1, veh = -1, drv = -1, ti[AVER_FW_VEH_TI_COUNT];
        float tf[AVER_FW_VEH_TF_COUNT];
        for (float& f : tf) f = -1.0f;
        for (int32_t& i : ti) i = -1;
        check(aver_fw_vehicle_spawn("car", 0, 0, 0, 0, &ent, &veh) == 0 && ent == -1 && veh == -1, "spawn answers 0 and writes nothing");
        check(aver_fw_vehicle_enter(1, 2) == 0 && aver_fw_vehicle_exit(1, &drv) == 0 && drv == -1, "enter and exit answer 0");
        check(aver_fw_vehicle_shift(1, 1) == 0 && aver_fw_vehicle_set_gear(1, 2) == 0 && aver_fw_vehicle_set_manual(1, 1) == 0, "the gear calls answer 0");
        check(aver_fw_vehicle_flip(1, 50.0f) == 0 && aver_fw_vehicle_repair(1) == 0 && aver_fw_vehicle_damage(1, 0.5f) == 0 && aver_fw_vehicle_tune(1, "power", 2.0f) == 0,
              "flip, repair, damage and tune answer 0");
        check(aver_fw_vehicle_telemetry(1, tf, AVER_FW_VEH_TF_COUNT, ti, AVER_FW_VEH_TI_COUNT) == 0 && tf[0] == -1.0f && ti[0] == -1, "telemetry writes nothing");
        int32_t oi[6];
        float of[8];
        check(aver_fw_vehicle_drain_events(oi, of, 1) == 0, "the event drain answers 0");
    }

    int token = 0;
    check(aver_fw_set_vehicle_provider(&fake, &token) == 1, "installing a provider returns 1");
    g_return = 1;

    AVER_INFO("=== forwarding ===");
    {
        int32_t ent = 0, veh = 0;
        check(aver_fw_vehicle_spawn("Vehicles/Bus.ocvehicle", 1.0f, 2.0f, 3.0f, 90.0f, &ent, &veh) == 1, "spawn forwards and succeeds");
        check(g_call.op == AVER_FW_VEH_OP_SPAWN && g_call.hasText && g_call.text == "Vehicles/Bus.ocvehicle" && g_call.inCount == 4 &&
                  g_call.in[0] == 1.0f && g_call.in[1] == 2.0f && g_call.in[2] == 3.0f && g_call.in[3] == 90.0f && g_call.user == &token,
              "the path and the pose arrive intact, with the provider's user pointer");
        check(ent == 4242 && veh == 17, "and both outputs come back");
        const std::string inline_text = "OCVEHICLE 1\nBODY half 1 1 1\n";
        aver_fw_vehicle_spawn(inline_text.c_str(), 0, 0, 0, 0, nullptr, nullptr);
        check(g_call.text == inline_text, "inline OCVEHICLE text is passed through byte for byte");
        check(aver_fw_vehicle_spawn(nullptr, 0, 0, 0, 0, nullptr, nullptr) == 0, "a null text is refused before the provider");

        check(aver_fw_vehicle_enter(55, 77) == 1 && g_call.op == AVER_FW_VEH_OP_ENTER && g_call.entity == 55 && bitsOf(g_call.in[0]) == 77, "enter carries the driver as int bits");
        int32_t drv = 0;
        check(aver_fw_vehicle_exit(55, &drv) == 1 && g_call.op == AVER_FW_VEH_OP_EXIT && drv == 99, "exit relays the driver that left");
        aver_fw_vehicle_shift(55, -1);
        check(g_call.op == AVER_FW_VEH_OP_SHIFT && bitsOf(g_call.in[0]) == -1, "shift carries a negative delta as bits");
        aver_fw_vehicle_set_gear(55, 3);
        check(g_call.op == AVER_FW_VEH_OP_SET_GEAR && bitsOf(g_call.in[0]) == 3, "set_gear");
        aver_fw_vehicle_set_manual(55, 5);
        check(g_call.op == AVER_FW_VEH_OP_SET_MANUAL && bitsOf(g_call.in[0]) == 1, "set_manual is 0 or 1");
        aver_fw_vehicle_flip(55, 60.0f);
        check(g_call.op == AVER_FW_VEH_OP_FLIP && g_call.in[0] == 60.0f, "flip carries the lift");
        aver_fw_vehicle_repair(55);
        check(g_call.op == AVER_FW_VEH_OP_REPAIR && g_call.entity == 55, "repair");
        aver_fw_vehicle_damage(55, 0.25f);
        check(g_call.op == AVER_FW_VEH_OP_DAMAGE && g_call.in[0] == 0.25f, "damage");
        aver_fw_vehicle_tune(55, "downforce", 1.5f);
        check(g_call.op == AVER_FW_VEH_OP_TUNE && g_call.text == "downforce" && g_call.in[0] == 1.5f, "tune carries the key as text and the value");
        check(aver_fw_vehicle_tune(55, nullptr, 1.0f) == 0, "a null key is refused");
    }

    AVER_INFO("=== reads ===");
    {
        float tf[AVER_FW_VEH_TF_COUNT];
        int32_t ti[AVER_FW_VEH_TI_COUNT];
        check(aver_fw_vehicle_telemetry(9, tf, AVER_FW_VEH_TF_COUNT, ti, AVER_FW_VEH_TI_COUNT) == 1 && g_call.op == AVER_FW_VEH_OP_TELEMETRY, "telemetry relays");
        check(tf[AVER_FW_VEH_TF_HEALTH01] == 18.5f && tf[AVER_FW_VEH_TF_RPM] == 0.5f && ti[AVER_FW_VEH_TI_DRIVER] == 112 && ti[AVER_FW_VEH_TI_GEAR] == 100,
              "and the blocks come back in layout");
        float few[3];
        check(aver_fw_vehicle_telemetry(9, few, 3, nullptr, 0) == 1 && few[2] == 2.5f, "a shorter float array gets the first entries; a null int array is fine");

        float wf[AVER_FW_VEH_WF_COUNT];
        int32_t wi[AVER_FW_VEH_WI_COUNT];
        check(aver_fw_vehicle_wheel(9, 2, wf, AVER_FW_VEH_WF_COUNT, wi, AVER_FW_VEH_WI_COUNT) == 1 && g_call.op == AVER_FW_VEH_OP_WHEEL && bitsOf(g_call.in[0]) == 2, "wheel relays with the index as bits");
        check(wf[AVER_FW_VEH_WF_LOAD_N] == 4.0f && wi[AVER_FW_VEH_WI_SKIDDING] == 203, "and its blocks come back");

        int32_t oi[AVER_FW_VEH_EVENT_INTS];
        float of[AVER_FW_VEH_EVENT_FLOATS];
        check(aver_fw_vehicle_drain_events(oi, of, 1) == 1 && oi[0] == 10 && oi[5] == 15 && of[7] == 1.75f && g_call.cap == 1, "drain_events relays the capacity and the event layout");
        check(aver_fw_vehicle_drain_events(nullptr, of, 1) == 0 && aver_fw_vehicle_drain_events(oi, of, 0) == 0, "bad buffers are refused before the provider");
    }

    AVER_INFO("=== clearing ===");
    {
        check(aver_fw_set_vehicle_provider(nullptr, nullptr) == 1, "clearing returns 1");
        const int before = g_call.calls;
        check(aver_fw_vehicle_repair(1) == 0 && g_call.calls == before, "and calls answer 0 without reaching the provider");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
