// The pose seam C ABI (framework_pose_abi.h) end to end through the Aver.Framework DLL: with no provider every call
// answers 0 (find_bone -1) and writes nothing; with a provider every call forwards its exact arguments under the right
// op code and relays the return and out values; clearing the provider restores the zeros. The provider is a fake, so
// no animation module is involved. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_pose_abi.h"

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
    int64_t h0 = 0, h1 = 0, h2 = 0;
    float in[32] = {};
    int32_t inCount = -1;
    bool hasOutF = false, hasOutI = false, hasStr = false;
    int32_t strLen = 0;
    void* user = nullptr;
    int calls = 0;
};
static Call g_call;
static int32_t g_return = 0;

static int32_t AVER_FW_CALL fake(int32_t op, int32_t entity, int64_t h0, int64_t h1, int64_t h2, const float* in, int32_t inCount,
                                 float* outF, int64_t* outI, char* outStr, int32_t outStrLen, void* user) {
    Call c;
    c.op = op; c.entity = entity; c.h0 = h0; c.h1 = h1; c.h2 = h2; c.inCount = inCount;
    c.hasOutF = outF != nullptr; c.hasOutI = outI != nullptr; c.hasStr = outStr != nullptr; c.strLen = outStrLen;
    c.user = user; c.calls = g_call.calls + 1;
    for (int i = 0; i < inCount && i < 32 && in; ++i) c.in[i] = in[i];
    g_call = c;
    if (op == AVER_FW_POSE_OP_FIND && outI) outI[0] = 5;
    if (op == AVER_FW_POSE_OP_INFO) {
        if (outI) outI[0] = 2;
        if (outF) for (int i = 0; i < 10; ++i) outF[i] = static_cast<float>(i) + 0.5f;
        if (outStr && outStrLen > 0) std::strncpy(outStr, "Spine", static_cast<size_t>(outStrLen - 1)), outStr[outStrLen - 1] = '\0';
    }
    if (op == AVER_FW_POSE_OP_READ && outF) for (int i = 0; i < 10; ++i) outF[i] = static_cast<float>(i);
    return g_return;
}

int main() {
    AVER_INFO("FrameworkPoseAbiTest");
    check(aver_fw_pose_abi_version() == 1 && AVER_FW_POSE_ABI_VERSION == 1, "the pose ABI is version 1");
    check(AVER_FW_POSE_OP_BONE_COUNT == 1 && AVER_FW_POSE_OP_FIND == 2 && AVER_FW_POSE_OP_INFO == 3 && AVER_FW_POSE_OP_READ == 4 &&
              AVER_FW_POSE_OP_WRITE == 5 && AVER_FW_POSE_OP_CLEAR == 6 && AVER_FW_POSE_OP_WEIGHT == 7 && AVER_FW_POSE_OP_ENABLE == 8 &&
              AVER_FW_POSE_OP_OVERRIDES == 9 && AVER_FW_POSE_OP_WRITE_RANGE == 10,
          "the op codes are pinned");
    check(AVER_FW_POSE_POSITION == 1 && AVER_FW_POSE_ROTATION == 2 && AVER_FW_POSE_SCALE == 4 && AVER_FW_POSE_ALL == 7 &&
              AVER_FW_POSE_MODEL == 8 && AVER_FW_POSE_ONCE == 16 && AVER_FW_POSE_READ_MODEL == 1 && AVER_FW_POSE_READ_BEFORE_SEAM == 2,
          "the flag bits are pinned");

    AVER_INFO("=== no provider ===");
    {
        float t[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        char name[8] = "xx";
        int32_t parent = 77;
        check(aver_fw_pose_bone_count(1) == 0 && aver_fw_pose_find_bone(1, 2) == -1, "count 0 and find -1");
        check(aver_fw_pose_read(1, 0, 0, t) == 0 && t[0] == -1 && t[9] == -1, "read answers 0 and leaves its output untouched");
        check(aver_fw_pose_bone_info(1, 0, name, sizeof name, &parent, t) == 0 && parent == 77 && name[0] == '\0', "bone_info answers 0, empties the name, leaves the rest");
        const float in[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
        check(aver_fw_pose_write(1, 0, 7, in, 1.0f) == 0 && aver_fw_pose_write_range(1, 0, 1, 7, in) == 0 && aver_fw_pose_clear(1, -1) == 0 &&
                  aver_fw_pose_set_weight(1, 1.0f) == 0 && aver_fw_pose_set_enabled(1, 1) == 0 && aver_fw_pose_override_count(1) == 0,
              "the writes answer 0");
    }

    int token = 0;
    check(aver_fw_set_pose_provider(&fake, &token) == 1, "installing a provider returns 1");
    g_return = 1;

    AVER_INFO("=== reads ===");
    {
        check(aver_fw_pose_find_bone(9, 0xABCD) == 5 && g_call.op == AVER_FW_POSE_OP_FIND && g_call.entity == 9 && g_call.h0 == 0xABCD && g_call.hasOutI &&
                  g_call.user == &token,
              "find_bone: op, entity, hash, the user pointer and the index back");
        g_return = 0;
        check(aver_fw_pose_find_bone(9, 1) == -1, "a refused find is -1");
        g_return = 3;
        check(aver_fw_pose_bone_count(9) == 3 && g_call.op == AVER_FW_POSE_OP_BONE_COUNT, "bone_count relays the provider's return");
        g_return = 1;
        char name[16] = {};
        int32_t parent = -9;
        float rest[10] = {};
        check(aver_fw_pose_bone_info(9, 4, name, sizeof name, &parent, rest) == 1 && g_call.op == AVER_FW_POSE_OP_INFO && g_call.h0 == 4 && g_call.strLen == 16 &&
                  std::strcmp(name, "Spine") == 0 && parent == 2 && rest[0] == 0.5f && rest[9] == 9.5f,
              "bone_info relays name, parent and the ten rest floats");
        float t[10] = {};
        check(aver_fw_pose_read(9, 4, AVER_FW_POSE_READ_MODEL | AVER_FW_POSE_READ_BEFORE_SEAM, t) == 1 && g_call.op == AVER_FW_POSE_OP_READ && g_call.h0 == 4 &&
                  g_call.h1 == 3 && t[3] == 3.0f,
              "read: bone, selector bits, ten floats back");
        check(aver_fw_pose_read(9, 4, 0, nullptr) == 0, "a null output is refused without reaching the provider");
    }

    AVER_INFO("=== writes ===");
    {
        float in[10];
        for (int i = 0; i < 10; ++i) in[i] = static_cast<float>(i);
        check(aver_fw_pose_write(9, 2, AVER_FW_POSE_ROTATION | AVER_FW_POSE_MODEL, in, 0.25f) == 1 && g_call.op == AVER_FW_POSE_OP_WRITE && g_call.h0 == 2 &&
                  g_call.h1 == 10 && g_call.inCount == 11 && g_call.in[9] == 9.0f && g_call.in[10] == 0.25f,
              "write: bone, flags, ten floats and the amount");
        float range[30] = {};
        range[0] = 1.0f; range[29] = 2.0f;
        check(aver_fw_pose_write_range(9, 3, 3, AVER_FW_POSE_ALL, range) == 1 && g_call.op == AVER_FW_POSE_OP_WRITE_RANGE && g_call.h0 == 7 && g_call.h1 == 3 &&
                  g_call.inCount == 30 && g_call.in[0] == 1.0f && g_call.in[29] == 2.0f,
              "write_range: flags, first bone, 10 floats per bone");
        check(aver_fw_pose_write_range(9, 0, 0, 1, range) == 0 && aver_fw_pose_write(9, 0, 1, nullptr, 1.0f) == 0, "an empty range and a null transform are refused");
        aver_fw_pose_clear(9, -1);
        check(g_call.op == AVER_FW_POSE_OP_CLEAR && g_call.h0 == -1, "clear: -1 is every bone");
        aver_fw_pose_set_weight(9, 0.5f);
        check(g_call.op == AVER_FW_POSE_OP_WEIGHT && g_call.inCount == 1 && g_call.in[0] == 0.5f, "set_weight");
        aver_fw_pose_set_enabled(9, 5);
        check(g_call.op == AVER_FW_POSE_OP_ENABLE && g_call.h0 == 1, "set_enabled normalises to 0 or 1");
        g_return = 4;
        check(aver_fw_pose_override_count(9) == 4 && g_call.op == AVER_FW_POSE_OP_OVERRIDES, "override_count relays the provider's return");
        g_return = 1;
    }

    AVER_INFO("=== clearing the provider ===");
    {
        const int calls = g_call.calls;
        check(aver_fw_set_pose_provider(nullptr, nullptr) == 1, "clearing returns 1");
        check(aver_fw_pose_clear(1, 0) == 0 && g_call.calls == calls, "and the calls answer 0 without reaching it");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
