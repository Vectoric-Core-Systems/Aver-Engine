// Strand hair C ABI: a thin relay to the provider a composition root installs. See framework_hair_abi.h and docs/HAIR.md.
//
// COMPILED INTO Aver.Framework, NOT Aver.Hair.Scene: the exported aver_fw_hair_* functions must live in the framework DLL the C# layer imports from,
// and that DLL links no hair code (it only relays). The file sits under modules/hair.scene/framework/ because the hair area does not own
// modules/framework/src; the shared edit in modules/framework/CMakeLists.txt names it by path, and it can move to modules/framework/src/HairAbi.cpp
// (a git mv and one CMake line) without changing a byte of it.

#include "aver/framework/framework_hair_abi.h"

namespace {

aver_fw_hair_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, const float* in = nullptr, int32_t inCount = 0, const char* text = nullptr,
             float* outF = nullptr, int32_t* outI = nullptr, int32_t cap = 0) {
    return g_provider ? g_provider(op, entity, in, inCount, text, outF, outI, cap, g_providerUser) : 0;
}

} // namespace

int32_t aver_fw_hair_abi_version(void) { return AVER_FW_HAIR_ABI_VERSION; }

int32_t aver_fw_set_hair_provider(aver_fw_hair_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_hair_teleport(int32_t entity) { return call(AVER_FW_HAIR_OP_TELEPORT, entity); }

int32_t aver_fw_hair_add_collider(int32_t entity, int32_t shape, float sx, float sy, float sz, float ox, float oy, float oz,
                                  const char* bone, int32_t* outCount) {
    if (!bone || !*bone) return 0;
    const float in[7] = {static_cast<float>(shape), sx, sy, sz, ox, oy, oz};
    int32_t out[1] = {0};
    if (!call(AVER_FW_HAIR_OP_ADD_COLLIDER, entity, in, 7, bone, nullptr, out, 1)) return 0;
    if (outCount) *outCount = out[0];
    return 1;
}

int32_t aver_fw_hair_clear_colliders(int32_t entity) { return call(AVER_FW_HAIR_OP_CLEAR_COLLIDERS, entity); }

int32_t aver_fw_hair_state(int32_t entity, int32_t* outI6) {
    int32_t i[AVER_FW_HAIR_TI_COUNT] = {};
    if (!call(AVER_FW_HAIR_OP_STATE, entity, nullptr, 0, nullptr, nullptr, i, AVER_FW_HAIR_TI_COUNT)) return 0;
    if (outI6)
        for (int k = 0; k < AVER_FW_HAIR_TI_COUNT; ++k) outI6[k] = i[k];
    return 1;
}
