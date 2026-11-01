// FrameworkRenderAbiTest -- P17 and P18: the render settings C ABI (framework_render_abi.h) through the Aver.Framework DLL (set, get, clamp, snap, clear, the
// revision counter, refusals, the statistics request and publish), and the shared apply of a script's keys onto a voxi::Settings
// (aver/voxi/ScriptRenderSettings.hpp) with a fake Api: a key lands, a released key restores the project's value, an untouched Settings is not changed.
// The store is process-wide, so each section starts with aver_fw_render_reset. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_render_abi.h"

#ifdef AVER_TEST_WITH_VOXI
#  define AVER_SCRIPTRENDER_HAS_ABI 1
#  include "aver/voxi/ScriptRenderSettings.hpp"
#endif

#include <cmath>
#include <limits>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

static void testKeysAndRanges() {
    AVER_INFO("=== keys, ranges, snapping ===");
    aver_fw_render_reset();
    check(aver_fw_render_abi_version() == 1 && AVER_FW_RENDER_ABI_VERSION == 1, "the render ABI is version 1");
    check(AVER_FW_RENDER_SCALE == 1 && AVER_FW_RENDER_SHARPNESS == 2 && AVER_FW_RENDER_TAA == 3 && AVER_FW_RENDER_MSAA == 4 && AVER_FW_RENDER_LOD_THRESHOLD == 5 &&
              AVER_FW_RENDER_OCCLUSION == 6 && AVER_FW_RENDER_SHADOW_CASCADES == 7 && AVER_FW_RENDER_SHADOW_SPLIT == 8 && AVER_FW_RENDER_SHADOW_DISTANCE == 9 &&
              AVER_FW_RENDER_SHADOW_FILTER == 10 && AVER_FW_RENDER_VOLFOG_ANISO == 11 && AVER_FW_RENDER_VOLFOG_LENGTH == 12 && AVER_FW_RENDER_VOLFOG_TEMPORAL == 13 &&
              AVER_FW_RENDER_VOLFOG_INTENSITY == 14 && AVER_FW_RENDER_KEY_COUNT == 15,
          "the key numbers are pinned");
    float v = -1.0f;
    check(aver_fw_render_get(AVER_FW_RENDER_SCALE, &v) == 0 && v == -1.0f, "a key nobody set reads as not overridden and leaves out alone");
    check(aver_fw_render_set(AVER_FW_RENDER_SCALE, 0.5f) == 1 && aver_fw_render_get(AVER_FW_RENDER_SCALE, &v) == 1 && approx(v, 0.5f), "a key sets and reads back");
    aver_fw_render_set(AVER_FW_RENDER_SCALE, 9.0f); aver_fw_render_get(AVER_FW_RENDER_SCALE, &v);
    check(approx(v, 2.0f), "the scale clamps at 2");
    aver_fw_render_set(AVER_FW_RENDER_SCALE, 0.0f); aver_fw_render_get(AVER_FW_RENDER_SCALE, &v);
    check(approx(v, 0.25f), "and at 0.25");
    aver_fw_render_set(AVER_FW_RENDER_MSAA, 3.0f); aver_fw_render_get(AVER_FW_RENDER_MSAA, &v);
    check(approx(v, 2.0f), "MSAA snaps down to a power of two (3 -> 2)");
    aver_fw_render_set(AVER_FW_RENDER_MSAA, 100.0f); aver_fw_render_get(AVER_FW_RENDER_MSAA, &v);
    check(approx(v, 8.0f), "MSAA tops out at 8");
    aver_fw_render_set(AVER_FW_RENDER_SHADOW_CASCADES, 2.6f); aver_fw_render_get(AVER_FW_RENDER_SHADOW_CASCADES, &v);
    check(approx(v, 3.0f), "the cascade count rounds to a whole number (2.6 -> 3)");
    aver_fw_render_set(AVER_FW_RENDER_SHADOW_CASCADES, 0.0f); aver_fw_render_get(AVER_FW_RENDER_SHADOW_CASCADES, &v);
    check(approx(v, 1.0f), "and is at least 1");
    aver_fw_render_set(AVER_FW_RENDER_TAA, 0.7f); aver_fw_render_get(AVER_FW_RENDER_TAA, &v);
    check(approx(v, 1.0f), "TAA is a switch (0.7 is on)");
    aver_fw_render_set(AVER_FW_RENDER_VOLFOG_ANISO, -5.0f); aver_fw_render_get(AVER_FW_RENDER_VOLFOG_ANISO, &v);
    check(approx(v, -0.95f), "the fog anisotropy clamps at -0.95");
    aver_fw_render_set(AVER_FW_RENDER_VOLFOG_LENGTH, 1.0f); aver_fw_render_get(AVER_FW_RENDER_VOLFOG_LENGTH, &v);
    check(approx(v, 100.0f), "the fog length clamps at 100 cm");

    check(aver_fw_render_set(0, 1.0f) == 0 && aver_fw_render_set(AVER_FW_RENDER_KEY_COUNT, 1.0f) == 0 && aver_fw_render_set(-3, 1.0f) == 0, "an unknown key is refused");
    check(aver_fw_render_set(AVER_FW_RENDER_SCALE, std::numeric_limits<float>::quiet_NaN()) == 0 && aver_fw_render_set(AVER_FW_RENDER_SCALE, std::numeric_limits<float>::infinity()) == 0,
          "a non-finite value is refused");
    aver_fw_render_get(AVER_FW_RENDER_SCALE, &v);
    check(approx(v, 0.25f), "and leaves the stored value alone");
    check(aver_fw_render_get(AVER_FW_RENDER_SCALE, nullptr) == 1, "a null out pointer is fine");
    aver_fw_render_reset();
}

static void testClearAndRevision() {
    AVER_INFO("=== clear and the revision counter ===");
    aver_fw_render_reset();
    const int32_t r0 = aver_fw_render_revision();
    aver_fw_render_set(AVER_FW_RENDER_SCALE, 0.5f);
    const int32_t r1 = aver_fw_render_revision();
    check(r1 != r0, "setting a key moves the revision");
    aver_fw_render_set(AVER_FW_RENDER_SCALE, 0.5f);
    check(aver_fw_render_revision() == r1, "setting the same value again does not");
    aver_fw_render_set(AVER_FW_RENDER_SCALE, 0.75f);
    check(aver_fw_render_revision() != r1, "a new value does");
    const int32_t r2 = aver_fw_render_revision();
    check(aver_fw_render_clear(AVER_FW_RENDER_SCALE) == 1 && aver_fw_render_get(AVER_FW_RENDER_SCALE, nullptr) == 0 && aver_fw_render_revision() != r2, "clearing a key releases it and moves the revision");
    const int32_t r3 = aver_fw_render_revision();
    aver_fw_render_clear(AVER_FW_RENDER_SCALE);
    check(aver_fw_render_revision() == r3, "clearing a key that is not set does not");
    aver_fw_render_set(AVER_FW_RENDER_MSAA, 4.0f);
    aver_fw_render_set(AVER_FW_RENDER_SHADOW_FILTER, 2.0f);
    check(aver_fw_render_clear(0) == 1 && aver_fw_render_get(AVER_FW_RENDER_MSAA, nullptr) == 0 && aver_fw_render_get(AVER_FW_RENDER_SHADOW_FILTER, nullptr) == 0, "key 0 releases everything");
    check(aver_fw_render_clear(99) == 0, "clearing an unknown key answers 0");
    aver_fw_render_reset();
}

static void testStats() {
    AVER_INFO("=== statistics ===");
    aver_fw_render_reset();
    check(aver_fw_render_stats_wanted() == 0, "nothing is wanted until a script asks");
    int32_t out[AVER_FW_RENDER_STAT_COUNT];
    for (int32_t& x : out) x = 77;
    check(aver_fw_render_stats(out) == 0, "asking before a frame is published answers 0");
    bool zeroed = true;
    for (int32_t x : out) zeroed = zeroed && x == 0;
    check(zeroed, "and zeroes the output");
    check(aver_fw_render_stats_wanted() == 1, "the ask turns collection on");
    const int32_t in[AVER_FW_RENDER_STAT_COUNT] = {120, 45000, 90, 4, 300, 1234};
    aver_fw_render_publish_stats(in);
    check(aver_fw_render_stats(out) == 1, "a published frame reads");
    check(out[AVER_FW_RENDER_STAT_OBJECTS] == 120 && out[AVER_FW_RENDER_STAT_TRIANGLES] == 45000 && out[AVER_FW_RENDER_STAT_DRAW_CALLS] == 90 &&
              out[AVER_FW_RENDER_STAT_TRANSLUCENT] == 4 && out[AVER_FW_RENDER_STAT_SHADOW_DRAWS] == 300 && out[AVER_FW_RENDER_STAT_FRAME] == 1234,
          "in the order of the indices");
    aver_fw_render_publish_stats(nullptr);
    check(aver_fw_render_stats(nullptr) == 1, "a null publish changes nothing, a null out pointer is fine");
    aver_fw_render_reset();
    check(aver_fw_render_stats_wanted() == 0 && aver_fw_render_stats(out) == 0, "a reset ends the request and drops the frame");
}

#ifdef AVER_TEST_WITH_VOXI
namespace {
struct FakeApi {
    static inline float value[voxi::kScriptRenderKeyCount] = {};
    static inline bool set[voxi::kScriptRenderKeyCount] = {};
    static inline int rev = 0;
    static bool get(int key, float* out) { if (!set[key]) return false; if (out) *out = value[key]; return true; }
    static int revision() { return rev; }
    static void put(int key, float v) { set[key] = true; value[key] = v; ++rev; }
    static void drop(int key) { set[key] = false; ++rev; }
};
}  // namespace

static void testApply() {
    AVER_INFO("=== the apply onto voxi::Settings ===");
    voxi::Settings project;   // the project's values
    project.shadowCascades = 3;
    project.shadowFilterScale = 1.5f;
    voxi::Settings live = project;
    voxi::ScriptRenderState st;

    check(!voxi::applyScriptRenderSettings<FakeApi>(live, st), "no script key changes nothing (first pass)");
    check(live.shadowCascades == 3 && live.shadowFilterScale == 1.5f && live.msaa == project.msaa, "and the Settings are as the project left them");

    FakeApi::put(voxi::kScriptRenderShadowCascades, 2.0f);
    FakeApi::put(voxi::kScriptRenderMsaa, 8.0f);
    FakeApi::put(voxi::kScriptRenderShadowFilter, 0.0f);
    FakeApi::put(voxi::kScriptRenderVolFogIntensity, 3.0f);
    check(voxi::applyScriptRenderSettings<FakeApi>(live, st), "set keys change the Settings");
    check(live.shadowCascades == 2 && live.msaa == voxi::Msaa::X8 && live.shadowFilterScale == 0.0f && live.volumetricFogIntensity == 3.0f, "each key lands on its field");
    check(!voxi::applyScriptRenderSettings<FakeApi>(live, st), "an unchanged revision is a cheap no-op");

    FakeApi::put(voxi::kScriptRenderShadowCascades, 4.0f);
    check(voxi::applyScriptRenderSettings<FakeApi>(live, st) && live.shadowCascades == 4, "a key moved again applies");
    FakeApi::drop(voxi::kScriptRenderShadowCascades);
    FakeApi::drop(voxi::kScriptRenderMsaa);
    FakeApi::drop(voxi::kScriptRenderShadowFilter);
    FakeApi::drop(voxi::kScriptRenderVolFogIntensity);
    check(voxi::applyScriptRenderSettings<FakeApi>(live, st), "released keys change the Settings back");
    check(live.shadowCascades == 3 && live.msaa == project.msaa && live.shadowFilterScale == 1.5f && live.volumetricFogIntensity == project.volumetricFogIntensity,
          "to what the project had before the script took them (3 cascades, not the 4 the script last asked)");

    // Host-only keys are not Settings fields: taking them changes nothing here.
    FakeApi::put(voxi::kScriptRenderScale, 0.5f);
    FakeApi::put(voxi::kScriptRenderLodThreshold, 4.0f);
    voxi::Settings before = live;
    check(!voxi::applyScriptRenderSettings<FakeApi>(live, st) && live.shadowCascades == before.shadowCascades, "SCALE and LOD_THRESHOLD are the host's: Settings stay put");
    FakeApi::drop(voxi::kScriptRenderScale);
    FakeApi::drop(voxi::kScriptRenderLodThreshold);
    voxi::applyScriptRenderSettings<FakeApi>(live, st);

    // The mirrored numbers are the header's.
    check(voxi::kScriptRenderScale == AVER_FW_RENDER_SCALE && voxi::kScriptRenderVolFogLength == AVER_FW_RENDER_VOLFOG_LENGTH && voxi::kScriptRenderOcclusion == AVER_FW_RENDER_OCCLUSION,
          "ScriptRenderSettings.hpp restates the ABI's key numbers");
}
#endif

int main() {
    testKeysAndRanges();
    testClearAndRevision();
    testStats();
#ifdef AVER_TEST_WITH_VOXI
    testApply();
#endif
    AVER_INFO("FrameworkRenderAbiTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
