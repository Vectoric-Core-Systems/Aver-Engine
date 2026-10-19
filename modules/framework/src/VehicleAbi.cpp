// Vehicle C ABI: a thin relay to the provider a composition root installs.
// See framework_vehicle_abi.h and docs/VEHICLES.md.

#include "aver/framework/framework_vehicle_abi.h"

#include <cstring>

namespace {

aver_fw_vehicle_fn g_provider = nullptr;
void* g_providerUser = nullptr;

int32_t call(int32_t op, int32_t entity, const float* in = nullptr, int32_t inCount = 0, const char* text = nullptr,
             float* outF = nullptr, int32_t* outI = nullptr, int32_t cap = 0) {
    return g_provider ? g_provider(op, entity, in, inCount, text, outF, outI, cap, g_providerUser) : 0;
}

// An integer argument travels as the bit pattern of an int32 in a float slot (see the header).
float bits(int32_t v) {
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

// The provider writes the whole block into scratch arrays of this size (so the two caps agree), and the caller's
// arrays get the first nF / nI entries.
constexpr int32_t kScratch = 32;

}  // namespace

int32_t aver_fw_vehicle_abi_version(void) { return AVER_FW_VEHICLE_ABI_VERSION; }

int32_t aver_fw_set_vehicle_provider(aver_fw_vehicle_fn fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
    return 1;
}

int32_t aver_fw_vehicle_spawn(const char* text, float x, float y, float z, float yawDeg, int32_t* outEntity, int32_t* outVehicle) {
    if (!text) return 0;
    const float in[4] = {x, y, z, yawDeg};
    int32_t out[2] = {0, 0};
    if (!call(AVER_FW_VEH_OP_SPAWN, 0, in, 4, text, nullptr, out, 2)) return 0;
    if (outEntity) *outEntity = out[0];
    if (outVehicle) *outVehicle = out[1];
    return 1;
}

int32_t aver_fw_vehicle_enter(int32_t vehicle, int32_t driver) {
    const float in[1] = {bits(driver)};
    return call(AVER_FW_VEH_OP_ENTER, vehicle, in, 1);
}

int32_t aver_fw_vehicle_exit(int32_t vehicle, int32_t* outDriver) {
    int32_t d = 0;
    if (!call(AVER_FW_VEH_OP_EXIT, vehicle, nullptr, 0, nullptr, nullptr, &d, 1)) return 0;
    if (outDriver) *outDriver = d;
    return 1;
}

int32_t aver_fw_vehicle_shift(int32_t vehicle, int32_t delta) {
    const float in[1] = {bits(delta)};
    return call(AVER_FW_VEH_OP_SHIFT, vehicle, in, 1);
}

int32_t aver_fw_vehicle_set_gear(int32_t vehicle, int32_t gear) {
    const float in[1] = {bits(gear)};
    return call(AVER_FW_VEH_OP_SET_GEAR, vehicle, in, 1);
}

int32_t aver_fw_vehicle_set_manual(int32_t vehicle, int32_t manual) {
    const float in[1] = {bits(manual ? 1 : 0)};
    return call(AVER_FW_VEH_OP_SET_MANUAL, vehicle, in, 1);
}

int32_t aver_fw_vehicle_flip(int32_t vehicle, float liftCm) {
    const float in[1] = {liftCm};
    return call(AVER_FW_VEH_OP_FLIP, vehicle, in, 1);
}

int32_t aver_fw_vehicle_repair(int32_t vehicle) { return call(AVER_FW_VEH_OP_REPAIR, vehicle); }

int32_t aver_fw_vehicle_damage(int32_t vehicle, float amount01) {
    const float in[1] = {amount01};
    return call(AVER_FW_VEH_OP_DAMAGE, vehicle, in, 1);
}

int32_t aver_fw_vehicle_tune(int32_t vehicle, const char* key, float value) {
    if (!key) return 0;
    const float in[1] = {value};
    return call(AVER_FW_VEH_OP_TUNE, vehicle, in, 1, key);
}

int32_t aver_fw_vehicle_telemetry(int32_t vehicle, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    float f[kScratch] = {};
    int32_t i[kScratch] = {};
    if (!call(AVER_FW_VEH_OP_TELEMETRY, vehicle, nullptr, 0, nullptr, f, i, kScratch)) return 0;
    if (outF) for (int32_t k = 0; k < nF && k < AVER_FW_VEH_TF_COUNT; ++k) outF[k] = f[k];
    if (outI) for (int32_t k = 0; k < nI && k < AVER_FW_VEH_TI_COUNT; ++k) outI[k] = i[k];
    return 1;
}

int32_t aver_fw_vehicle_wheel(int32_t vehicle, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    const float in[1] = {bits(wheel)};
    float f[kScratch] = {};
    int32_t i[kScratch] = {};
    if (!call(AVER_FW_VEH_OP_WHEEL, vehicle, in, 1, nullptr, f, i, kScratch)) return 0;
    if (outF) for (int32_t k = 0; k < nF && k < AVER_FW_VEH_WF_COUNT; ++k) outF[k] = f[k];
    if (outI) for (int32_t k = 0; k < nI && k < AVER_FW_VEH_WI_COUNT; ++k) outI[k] = i[k];
    return 1;
}

int32_t aver_fw_vehicle_drain_events(int32_t* outI, float* outF, int32_t max) {
    if (!outI || !outF || max <= 0) return 0;
    return call(AVER_FW_VEH_OP_EVENTS, 0, nullptr, 0, nullptr, outF, outI, max);
}
