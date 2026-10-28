// The environment-effects C ABI (framework_envfx_abi.h) through the Aver.Framework DLL: fog volumes (create by key, set, enable, the registry cap, the density
// query, the host's pack and revision counter, the nearest-first order and the 16-volume cut) and reflection probes (create, the setters, selection and
// the box-projected direction), plus the refusals for bad arguments and the session reset. The store is process-wide; each section ends with a reset. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_envfx_abi.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol * (std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f); }

static void identityAt(float* w, float x, float y, float z) {
    const float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z};
    std::memcpy(w, m, sizeof(m));
}

static void testFog() {
    AVER_INFO("=== fog volumes ===");
    aver_fw_envfx_reset();
    check(aver_fw_envfx_abi_version() == 1 && AVER_FW_ENVFX_ABI_VERSION == 1, "the ABI is version 1");
    check(aver_fw_fogvol_create(nullptr) == 0 && aver_fw_fogvol_create("") == 0, "a null or empty key is refused");
    const int64_t rev0 = aver_fw_fogvol_revision();
    const int32_t a = aver_fw_fogvol_create("Level/Fog");
    check(a > 0 && aver_fw_fogvol_count() == 1 && aver_fw_fogvol_revision() > rev0, "a volume is made and the revision moves");
    check(aver_fw_fogvol_create("Level/Fog") == a && aver_fw_fogvol_count() == 1, "the same key returns the same id");
    check(aver_fw_fogvol_find("Level/Fog") == a && aver_fw_fogvol_find("nope") == 0, "find by key");

    float w[12]; identityAt(w, 0, 0, 0);
    const float half[3] = {100, 100, 100};
    float block[388];
    check(aver_fw_fogvol_pack(0, 0, 0, block, 388) == 0 && block[0] == 0.0f, "a created but unset volume packs nothing");
    check(aver_fw_fogvol_set(a, AVER_FW_FOGVOL_BOX, w, half, 2.0f, nullptr, nullptr, 0.0f, 0.1f) == 1, "set takes a valid box");
    check(near(aver_fw_fogvol_density_at(0, 0, 0), 0.02f), "density 2 per metre reads 0.02 per cm");
    check(aver_fw_fogvol_density_at(500, 0, 0) == 0.0f, "none outside");
    check(aver_fw_fogvol_pack(0, 0, 0, block, 388) == 1 && block[0] == 1.0f, "the host packs one volume");
    check(aver_fw_fogvol_pack(0, 0, 0, block, 100) == 0, "a buffer too small is refused");

    const int64_t rev1 = aver_fw_fogvol_revision();
    aver_fw_fogvol_set_enabled(a, 0);
    check(aver_fw_fogvol_revision() > rev1 && aver_fw_fogvol_density_at(0, 0, 0) == 0.0f && aver_fw_fogvol_pack(0, 0, 0, block, 388) == 0, "disabled: no density and nothing packed");
    const int64_t rev2 = aver_fw_fogvol_revision();
    aver_fw_fogvol_set_enabled(a, 0);
    check(aver_fw_fogvol_revision() == rev2, "disabling twice changes nothing");
    aver_fw_fogvol_set_enabled(a, 1);

    float zero[12] = {};
    check(aver_fw_fogvol_set(a, 3, zero, half, 1.0f, nullptr, nullptr, 0, 0.1f) == 0, "a singular matrix is refused");
    check(aver_fw_fogvol_set(a, 3, w, half, std::numeric_limits<float>::quiet_NaN(), nullptr, nullptr, 0, 0.1f) == 0, "a NaN density is refused");
    check(aver_fw_fogvol_set(999, 3, w, half, 1.0f, nullptr, nullptr, 0, 0.1f) == 0, "an unknown id is refused");
    check(aver_fw_fogvol_set(a, 3, nullptr, half, 1.0f, nullptr, nullptr, 0, 0.1f) == 0, "a null matrix is refused");

    // A second volume carves the first.
    const int32_t b = aver_fw_fogvol_create("Level/Carve");
    aver_fw_fogvol_set(b, AVER_FW_FOGVOL_ELLIPSOID, w, half, -1.0f, nullptr, nullptr, 0, 0.1f);
    check(near(aver_fw_fogvol_density_at(0, 0, 0), 0.01f), "a negative volume subtracts from the sum");

    check(aver_fw_fogvol_destroy(b) == 1 && aver_fw_fogvol_destroy(b) == 0 && aver_fw_fogvol_count() == 1, "destroy once");
    aver_fw_envfx_reset();
    check(aver_fw_fogvol_count() == 0, "reset forgets every volume");
}

static void testFogOrderAndCap() {
    AVER_INFO("=== the registry cap and the nearest-first cut ===");
    aver_fw_envfx_reset();
    const float half[3] = {10, 10, 10};
    float w[12], block[388];
    int made = 0;
    for (int i = 0; i < AVER_FW_FOGVOL_MAX + 5; ++i) {
        const std::string key = "V" + std::to_string(i);
        const int32_t id = aver_fw_fogvol_create(key.c_str());
        if (!id) continue;
        ++made;
        identityAt(w, float(i) * 100.0f, 0, 0);
        aver_fw_fogvol_set(id, AVER_FW_FOGVOL_BOX, w, half, 1.0f, nullptr, nullptr, 0, 0.1f);
    }
    check(made == AVER_FW_FOGVOL_MAX && aver_fw_fogvol_count() == AVER_FW_FOGVOL_MAX, "the registry stops at its cap");
    check(aver_fw_fogvol_pack(0, 0, 0, block, 388) == 16 && block[0] == 16.0f, "the block holds 16 volumes");
    // The first volume's translation is in toUnit rows: the volume at x = 0 is the nearest, the one at x = 1500 the 16th. The 17th (x = 1600) is cut.
    check(near(block[4 + 0 * 24 + 3], 0.0f), "the nearest volume (at the camera) is first: its unit X row has offset 0");
    float far16 = block[4 + 15 * 24 + 3];   // toUnit row 0, offset: -tx / half
    check(near(far16, -150.0f), "the 16th is the volume at x = 1500");
    check(aver_fw_fogvol_pack(1600, 0, 0, block, 388) == 16 && near(block[4 + 3], -160.0f), "from x = 1600 the volume there comes first");
    aver_fw_envfx_reset();
    // A world volume has no position: it always makes the cut.
    for (int i = 0; i < 20; ++i) {
        const std::string key = "N" + std::to_string(i);
        const int32_t id = aver_fw_fogvol_create(key.c_str());
        identityAt(w, 10000.0f + float(i), 0, 0);
        aver_fw_fogvol_set(id, AVER_FW_FOGVOL_BOX, w, half, 1.0f, nullptr, nullptr, 0, 0.1f);
    }
    const int32_t world = aver_fw_fogvol_create("World");
    identityAt(w, 99999, 0, 0);
    aver_fw_fogvol_set(world, AVER_FW_FOGVOL_WORLD, w, half, 1.0f, nullptr, nullptr, 0, 0.1f);
    aver_fw_fogvol_pack(0, 0, 0, block, 388);
    check(near(block[4 + 3 * 4 + 3], float(AVER_FW_FOGVOL_WORLD)), "a world volume sorts first: it is in the block");
    aver_fw_envfx_reset();
}

static void testProbes() {
    AVER_INFO("=== reflection probes ===");
    aver_fw_envfx_reset();
    const int32_t p = aver_fw_probe_create("Room/Probe");
    check(p > 0 && aver_fw_probe_count() == 1 && aver_fw_probe_find("Room/Probe") == p && aver_fw_probe_create("Room/Probe") == p, "make, find, same key");
    float w[12]; identityAt(w, 0, 0, 250);
    const float half[3] = {500, 500, 250};
    check(aver_fw_probe_set_box(p, w, half, nullptr) == 1, "the box is set");
    check(aver_fw_probe_set_look(p, 1.0f, 100.0f, 0.0f, 1, 0, 0, 256) == 1, "the look is set");
    check(aver_fw_probe_set_ambient(p, AVER_FW_PROBE_AMBIENT_COLOUR, nullptr, 1.0f) == 1, "the ambient is set");
    check(aver_fw_probe_set_masks(p, 0xFFFFFFFFu, 0xFFFFFFFFu) == 1, "the masks are set");
    int32_t ids[4]; float weights[4], rest = 0;
    int32_t n = aver_fw_probe_select(0, 0, 250, 0, 0, 0, ids, weights, &rest);
    check(n == 1 && ids[0] == p && near(weights[0], 1.0f) && near(rest, 0.0f, 1e-3f), "inside the probe: it takes the whole blend");
    n = aver_fw_probe_select(2000, 0, 250, 0, 0, 0, ids, weights, &rest);
    check(n == 0 && near(rest, 1.0f), "outside: the sky");
    float out[3];
    check(aver_fw_probe_project(p, 400, 0, 250, 0, 1, 0, out) == 1, "project answers for a known probe");
    const float len = std::sqrt(400.0f * 400.0f + 500.0f * 500.0f);
    check(near(out[0], 400 / len) && near(out[1], 500 / len) && near(out[2], 0.0f), "the box-projected direction");
    check(aver_fw_probe_project(9999, 0, 0, 0, 0, 1, 0, out) == 0 && near(out[1], 1.0f), "an unknown probe returns the direction unchanged");
    aver_fw_probe_set_enabled(p, 0);
    check(aver_fw_probe_select(0, 0, 250, 0, 0, 0, ids, weights, &rest) == 0, "a disabled probe lights nothing");
    aver_fw_probe_set_enabled(p, 1);
    aver_fw_probe_set_look(p, 1.0f, 100.0f, 500.0f, 1, 0, 0, 256);
    check(aver_fw_probe_select(0, 0, 250, 0, 0, 5000, ids, weights, &rest) == 0, "past max_distance from the camera it is off");
    const float bad[12] = {NAN};
    check(aver_fw_probe_set_box(p, bad, half, nullptr) == 0, "a NaN matrix is refused");
    const int64_t rev = aver_fw_probe_revision();
    aver_fw_probe_set_look(p, 2.0f, 100.0f, 0.0f, 0, 1, 1, 100000);
    check(aver_fw_probe_revision() > rev, "a change moves the revision");
    check(aver_fw_probe_destroy(p) == 1 && aver_fw_probe_count() == 0, "destroy");
    int made = 0;
    for (int i = 0; i < AVER_FW_PROBE_MAX + 3; ++i) if (aver_fw_probe_create(("P" + std::to_string(i)).c_str())) ++made;
    check(made == AVER_FW_PROBE_MAX, "the probe registry stops at its cap");
    aver_fw_envfx_reset();
    check(aver_fw_probe_count() == 0, "reset forgets every probe");
}

int main() {
    testFog();
    testFogOrderAndCap();
    testProbes();
    if (g_failures) {
        AVER_ERROR("FrameworkEnvFxAbiTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("FrameworkEnvFxAbiTest: ok");
    return 0;
}
