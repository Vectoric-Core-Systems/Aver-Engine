// The soft shape C ABI (framework_softshape_abi.h) end to end through the Aver.Framework DLL: with no provider every call answers 0;
// with a provider every call forwards its exact arguments under the right op code and relays the out values; clearing the provider
// restores the zeros. The provider is a fake, so no soft-body module is involved. Exits 0 or 1.
//
// NOTHING IN HERE HAS BEEN RUN: written against the header.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_softshape_abi.h"

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
    bool hasOutF = false, hasOutI = false;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 1;
static int32_t g_outI[16] = {};
static float g_outF[2] = {0.0f, 0.0f};

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, const float* in, int32_t inCount, float* outF, int32_t* outI, int32_t cap, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.inCount = inCount; c.cap = cap; c.hasOutF = outF != nullptr; c.hasOutI = outI != nullptr; c.user = user;
    c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 16 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (outF && cap >= 2) { outF[0] = g_outF[0]; outF[1] = g_outF[1]; }
    if (outI && cap > 0) {
        const int n = op == AVER_FW_SS_OP_SENSOR_EVENTS ? cap * 3 : cap;   // an event is three ints
        for (int i = 0; i < n && i < 16; ++i) outI[i] = g_outI[i];
    }
    return g_return;
}

int main() {
    AVER_INFO("FrameworkSoftShapeAbiTest");
    check(aver_fw_softshape_abi_version() == 1 && AVER_FW_SOFTSHAPE_ABI_VERSION == 1, "the soft shape ABI is version 1");
    check(AVER_FW_SS_OP_PRESSURE == 1 && AVER_FW_SS_OP_VOLUME == 2 && AVER_FW_SS_OP_GRAB == 3 && AVER_FW_SS_OP_MOVE_GRAB == 4 && AVER_FW_SS_OP_RELEASE_GRAB == 5 &&
              AVER_FW_SS_OP_BUOYANCY == 6 && AVER_FW_SS_OP_SUBMERGED == 7 && AVER_FW_SS_OP_CONTACT == 8 && AVER_FW_SS_OP_PAIR_MODE == 9 &&
              AVER_FW_SS_OP_CUT_ROPE == 10 && AVER_FW_SS_OP_CREATE_ROPE == 11 && AVER_FW_SS_OP_SENSOR_EVENTS == 12,
          "the op codes are pinned");

    AVER_INFO("=== no provider ===");
    {
        float two[2] = {-1.0f, -1.0f};
        int32_t ids[6] = {-1, -1, -1, -1, -1, -1};
        check(aver_fw_softshape_set_pressure(1, 3.0f) == 0 && aver_fw_softshape_grab(1, 0, 0, 0, 10, 0, 0, 0) == 0 &&
                  aver_fw_softshape_move_grab(1, 1, 0, 0, 0) == 0 && aver_fw_softshape_release_grab(1, 1) == 0 &&
                  aver_fw_softshape_set_buoyancy(1, 1000, 1) == 0 && aver_fw_softshape_set_contact(1, 1, 1) == 0 &&
                  aver_fw_softshape_set_pair_mode(1, 2, 1) == 0 && aver_fw_softshape_cut_rope(1, 0, 0, 0) == 0 &&
                  aver_fw_softshape_create_rope(0, 0, 0, 100, 0, 0, 0, 0) == 0 && aver_fw_softshape_drain_sensor_events(ids, 2) == 0,
              "every call answers 0");
        check(aver_fw_softshape_volume(1, two) == 0 && two[0] == -1.0f && aver_fw_softshape_submerged(1, two) == 0 && two[1] == -1.0f && ids[0] == -1,
              "the reads leave their outputs untouched");
    }

    int token = 0;
    check(aver_fw_set_softshape_provider(&fake, &token) == 1, "installing a provider returns 1");

    AVER_INFO("=== forwarding ===");
    {
        g_return = 1;
        check(aver_fw_softshape_set_pressure(7, 3.5f) == 1 && g_call.op == AVER_FW_SS_OP_PRESSURE && g_call.entity == 7 && g_call.inCount == 1 && g_call.in[0] == 3.5f &&
                  g_call.user == &token,
              "pressure: op, entity, kPa, the user pointer");

        g_outF[0] = 4000.0f; g_outF[1] = -250.0f;
        float v[2] = {0, 0};
        check(aver_fw_softshape_volume(7, v) == 1 && g_call.op == AVER_FW_SS_OP_VOLUME && v[0] == 4000.0f && v[1] == -250.0f, "volume: rest and now come back");
        check(aver_fw_softshape_volume(7, nullptr) == 1, "a null output is allowed");

        g_outI[0] = 12;
        check(aver_fw_softshape_grab(7, 1, 2, 3, 40, 4, 5, 6) == 12 && g_call.op == AVER_FW_SS_OP_GRAB && g_call.inCount == 7 && g_call.in[0] == 1 && g_call.in[3] == 40 &&
                  g_call.in[6] == 6 && g_call.hasOutI,
              "grab: point, reach, target, and the grab id back");
        g_outI[0] = 0;
        check(aver_fw_softshape_grab(7, 1, 2, 3, 40, 4, 5, 6) == 0, "a zero id is a miss");
        g_outI[0] = -4;
        check(aver_fw_softshape_grab(7, 1, 2, 3, 40, 4, 5, 6) == 0, "and a negative one is clamped to a miss");

        check(aver_fw_softshape_move_grab(7, 12, 9, 8, 7) == 1 && g_call.op == AVER_FW_SS_OP_MOVE_GRAB && g_call.inCount == 4 && g_call.in[0] == 12 && g_call.in[3] == 7,
              "move_grab: id and target");
        check(aver_fw_softshape_release_grab(7, 12) == 1 && g_call.op == AVER_FW_SS_OP_RELEASE_GRAB && g_call.inCount == 1 && g_call.in[0] == 12, "release_grab: the id");
        check(aver_fw_softshape_set_buoyancy(7, 1000.0f, 1.5f) == 1 && g_call.op == AVER_FW_SS_OP_BUOYANCY && g_call.inCount == 2 && g_call.in[0] == 1000.0f && g_call.in[1] == 1.5f,
              "buoyancy: density and drag");

        g_outF[0] = 0.4f; g_outF[1] = 2200.0f;
        float s[2] = {0, 0};
        check(aver_fw_softshape_submerged(7, s) == 1 && g_call.op == AVER_FW_SS_OP_SUBMERGED && s[0] == 0.4f && s[1] == 2200.0f, "submerged: fraction and volume come back");

        check(aver_fw_softshape_set_contact(7, 0.0f, 0.5f) == 1 && g_call.op == AVER_FW_SS_OP_CONTACT && g_call.inCount == 2 && g_call.in[0] == 0.0f && g_call.in[1] == 0.5f,
              "contact: the two scales");
        check(aver_fw_softshape_set_pair_mode(7, 99, 2) == 1 && g_call.op == AVER_FW_SS_OP_PAIR_MODE && g_call.inCount == 2 && g_call.in[0] == 99 && g_call.in[1] == 2,
              "pair mode: the other entity and the mode");

        g_outI[0] = 31;
        check(aver_fw_softshape_cut_rope(7, 10, 20, 30) == 31 && g_call.op == AVER_FW_SS_OP_CUT_ROPE && g_call.inCount == 3 && g_call.in[1] == 20, "cut: the new entity comes back");
        g_outI[0] = 0;
        check(aver_fw_softshape_cut_rope(7, 10, 20, 30) == 0, "a zero entity is a refusal");

        g_outI[0] = 55;
        check(aver_fw_softshape_create_rope(1, 2, 3, 4, 5, 6, 700, 2.5f) == 55 && g_call.op == AVER_FW_SS_OP_CREATE_ROPE && g_call.inCount == 8 && g_call.in[2] == 3 &&
                  g_call.in[6] == 700 && g_call.in[7] == 2.5f,
              "create_rope: the two ends, the length and radius, and the entity back");

        g_return = 2;
        for (int i = 0; i < 6; ++i) g_outI[i] = 100 + i;
        int32_t ev[6] = {};
        check(aver_fw_softshape_drain_sensor_events(ev, 2) == 2 && g_call.op == AVER_FW_SS_OP_SENSOR_EVENTS && g_call.cap == 2 && ev[0] == 100 && ev[5] == 105,
              "sensor events: the count back and three ints per event");
        const int before = g_call.calls;
        check(aver_fw_softshape_drain_sensor_events(nullptr, 2) == 0 && aver_fw_softshape_drain_sensor_events(ev, 0) == 0 && g_call.calls == before,
              "a null buffer or a zero max never reaches the provider");
        g_return = 1;
    }

    AVER_INFO("=== removal ===");
    {
        aver_fw_set_softshape_provider(nullptr, nullptr);
        const int before = g_call.calls;
        check(aver_fw_softshape_set_pressure(7, 3.0f) == 0 && g_call.calls == before, "clearing the provider restores the zeros and calls nothing");
    }

    if (g_failures == 0) AVER_INFO("FrameworkSoftShapeAbiTest: {} checks passed", g_checks);
    else                 AVER_ERROR("FrameworkSoftShapeAbiTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
