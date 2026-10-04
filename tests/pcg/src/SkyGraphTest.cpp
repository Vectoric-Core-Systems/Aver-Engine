// The sky-as-a-script seam: the framework ABI a project's F# sky publishes through.
//
// WHAT THIS COVERS and what it deliberately does not. The F# graph itself (Aver.Pcg.SampleRules'
// Sky module) is pure and testable with dotnet; this is the C side of the same seam -- the request
// the host reads once a frame. It is worth its own test because the ordering property is the whole
// design: a project with NO sky script must render exactly the sky its level authored, and that is
// only true while aver_fw_sky_clouds returns 0 before anything publishes.
#include "aver/framework/framework_abi.h"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    check(std::fabs(got - want) <= tol,
          what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

int main() {
    AVER_INFO("=== nothing published: the level keeps its sky ===");
    {
        // The out params are poisoned first, so "returns 0" and "writes nothing" are separate
        // assertions rather than one that could pass by accident.
        i32 seed = 1234;
        f32 cov = 9.0f, den = 9.0f, bot = 9.0f, top = 9.0f, scl = 9.0f, wx = 9.0f, wy = 9.0f;
        const i32 has = aver_fw_sky_clouds(&seed, &cov, &den, &bot, &top, &scl, &wx, &wy);
        check(has == 0, "aver_fw_sky_clouds reports NO request before anything publishes");
        check(seed == 1234 && cov == 9.0f && bot == 9.0f,
              "and writes nothing through the out pointers, so the caller's own values survive");
    }

    AVER_INFO("=== a script publishes ===");
    {
        aver_fw_set_sky_clouds(7, 0.62f, 1.4f, 120000.0f, 300000.0f, 0.00003f, 800.0f, -240.0f);
        i32 seed = 0;
        f32 cov = 0, den = 0, bot = 0, top = 0, scl = 0, wx = 0, wy = 0;
        check(aver_fw_sky_clouds(&seed, &cov, &den, &bot, &top, &scl, &wx, &wy) == 1,
              "the request is reported");
        check(seed == 7, "the seed round-trips");
        checkNear(cov, 0.62f, 1e-6f, "coverage");
        checkNear(den, 1.4f, 1e-6f, "density");
        checkNear(bot, 120000.0f, 1e-3f, "the layer's base, in centimetres");
        checkNear(top, 300000.0f, 1e-3f, "and its top");
        checkNear(scl, 0.00003f, 1e-9f, "the feature scale");
        checkNear(wx, 800.0f, 1e-3f, "wind X");
        checkNear(wy, -240.0f, 1e-3f, "wind Y, which may be negative");
    }

    AVER_INFO("=== a later call replaces the whole request ===");
    {
        aver_fw_set_sky_clouds(11, 0.10f, 1.0f, 150000.0f, 280000.0f, 0.00002f, 0.0f, 0.0f);
        i32 seed = 0; f32 cov = 0, den = 0, bot = 0, top = 0, scl = 0, wx = 0, wy = 0;
        aver_fw_sky_clouds(&seed, &cov, &den, &bot, &top, &scl, &wx, &wy);
        check(seed == 11, "the new seed wins");
        checkNear(cov, 0.10f, 1e-6f, "and the new coverage");
        checkNear(wx, 0.0f, 1e-6f, "a value the previous call set is not left behind");
    }

    AVER_INFO("=== the host is protected from a script's arithmetic ===");
    {
        // A coverage out of range is the one that matters: it reaches the cloud raymarch directly.
        aver_fw_set_sky_clouds(1, 5.0f, -2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        i32 seed = 0; f32 cov = 0, den = 0, bot = 0, top = 0, scl = 0, wx = 0, wy = 0;
        aver_fw_sky_clouds(&seed, &cov, &den, &bot, &top, &scl, &wx, &wy);
        checkNear(cov, 1.0f, 1e-6f, "coverage above 1 is clamped, not passed through");
        checkNear(den, 0.0f, 1e-6f, "a negative density is clamped to zero");
        check(scl > 0.0f, "a zero feature scale is replaced, because the shader divides by it");

        aver_fw_set_sky_clouds(1, -3.0f, 1.0f, 0.0f, 0.0f, 0.00002f, 0.0f, 0.0f);
        aver_fw_sky_clouds(nullptr, &cov, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        checkNear(cov, 0.0f, 1e-6f, "coverage below 0 is clamped too");
    }

    AVER_INFO("=== every out pointer is optional ===");
    {
        // The host reads all eight, but a caller asking one question should not have to.
        check(aver_fw_sky_clouds(nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr) == 1,
              "passing all nulls still reports whether a request exists");
    }

    AVER_INFO("=== the script can hand the sky back ===");
    {
        aver_fw_clear_sky_clouds();
        i32 seed = 77;
        f32 cov = 7.0f;
        check(aver_fw_sky_clouds(&seed, &cov, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == 0,
              "after clear, no request is reported");
        check(seed == 77 && cov == 7.0f, "and nothing is written, so the level's sky takes over");
    }

    if (g_failures == 0) AVER_INFO("=== all sky graph seam tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
