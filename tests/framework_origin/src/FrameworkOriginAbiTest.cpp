// The floating-origin C ABI (framework_origin_abi.h) through the Aver.Framework DLL: the answers with no provider, a malformed
// provider refused, every call forwarded to a fake provider, and the WorldPos / origin conversions against aver/world/WorldPos.hpp
// (the header the DLL itself uses) over a few thousand positions out to 10^8 cm. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_origin_abi.h"
#include "aver/world/WorldPos.hpp"

#include <cmath>
#include <cstring>
#include <random>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

namespace {

struct Fake {
    int64_t ox = 0, oy = 0;
    uint32_t gen = 0;
    int32_t enabled = 1;
    int shiftCalls = 0;
    int32_t lastDx = 0, lastDy = 0;
    int32_t accept = 1;
};

aver_fw_origin_provider providerFor(Fake* f) {
    aver_fw_origin_provider p{};
    p.size = static_cast<int32_t>(sizeof(p));
    p.user = f;
    p.get = [](void* u, int64_t* xy) -> int32_t { auto* f = static_cast<Fake*>(u); xy[0] = f->ox; xy[1] = f->oy; return 1; };
    p.generation = [](void* u) -> uint32_t { return static_cast<Fake*>(u)->gen; };
    p.enabled = [](void* u) -> int32_t { return static_cast<Fake*>(u)->enabled; };
    p.request_shift = [](void* u, int32_t dx, int32_t dy) -> int32_t {
        auto* f = static_cast<Fake*>(u);
        ++f->shiftCalls; f->lastDx = dx; f->lastDy = dy;
        return f->accept;
    };
    return p;
}

} // namespace

static void testNoProvider() {
    AVER_INFO("=== no provider ===");
    aver_fw_set_origin_provider(nullptr);
    check(aver_fw_origin_abi_version() == 1 && AVER_FW_ORIGIN_ABI_VERSION == 1, "the origin ABI is version 1");
    int64_t xy[2] = {7, 7};
    check(aver_fw_origin_get(xy) == 0 && xy[0] == 0 && xy[1] == 0, "get answers 0 and writes the zero origin");
    check(aver_fw_origin_get(nullptr) == 0, "get with a null out is harmless");
    check(aver_fw_origin_generation() == 0 && aver_fw_origin_enabled() == 0, "generation 0 and not enabled");
    check(aver_fw_origin_request_shift(1, 0) == 0, "a shift request is refused");
    float l[3] = {};
    aver_fw_origin_to_local(123456.5, -7890.25, 33.0, l);
    check(l[0] == 123456.5f && l[1] == -7890.25f && l[2] == 33.0f, "to_local is the identity about a zero origin");
    double a[3] = {};
    aver_fw_origin_to_abs(1.5f, -2.5f, 3.5f, a);
    check(a[0] == 1.5 && a[1] == -2.5 && a[2] == 3.5, "to_abs is the identity about a zero origin");
    aver_fw_origin_to_local(1, 2, 3, nullptr);
    aver_fw_origin_to_abs(1, 2, 3, nullptr);
    aver_fw_worldpos_from_abs(1, 2, 3, nullptr, nullptr);
    aver_fw_worldpos_to_abs(nullptr, nullptr, nullptr);
    double z[3] = {9, 9, 9};
    aver_fw_worldpos_to_abs(nullptr, nullptr, z);
    check(z[0] == 0 && z[1] == 0 && z[2] == 0, "null pointers never crash, and a missing WorldPos is the zero position");
}

static void testProvider() {
    AVER_INFO("=== provider ===");
    Fake f;
    f.ox = 3 * world::kOriginStepCm;
    f.oy = -2 * world::kOriginStepCm;
    f.gen = 5;
    aver_fw_origin_provider p = providerFor(&f);

    aver_fw_origin_provider bad = p;
    bad.size = 8;
    check(aver_fw_set_origin_provider(&bad) == 0, "a struct that is too small is refused");
    bad = p;
    bad.get = nullptr;
    check(aver_fw_set_origin_provider(&bad) == 0, "a missing member is refused");
    check(aver_fw_origin_enabled() == 0, "and the refused provider was not installed");

    check(aver_fw_set_origin_provider(&p) == 1, "a good provider installs");
    int64_t xy[2] = {};
    check(aver_fw_origin_get(xy) == 1 && xy[0] == f.ox && xy[1] == f.oy, "get forwards the origin");
    check(aver_fw_origin_generation() == 5, "generation forwards");
    check(aver_fw_origin_enabled() == 1, "enabled forwards");
    f.gen = 6; f.ox += world::kOriginStepCm; f.enabled = 0;
    check(aver_fw_origin_get(xy) == 1 && xy[0] == f.ox && aver_fw_origin_generation() == 6 && aver_fw_origin_enabled() == 0,
          "the provider is read live, not copied");

    check(aver_fw_origin_request_shift(0, 0) == 0 && f.shiftCalls == 0, "a zero request never reaches the host");
    check(aver_fw_origin_request_shift(2, -1) == 1 && f.shiftCalls == 1 && f.lastDx == 2 && f.lastDy == -1, "a request is forwarded");
    f.accept = 0;
    check(aver_fw_origin_request_shift(1, 1) == 0, "a host refusal is returned");

    // A copy was taken: the caller's struct can go away.
    p.get = nullptr;
    check(aver_fw_origin_get(xy) == 1, "the installed provider is a copy");

    check(aver_fw_set_origin_provider(nullptr) == 1 && aver_fw_origin_get(xy) == 0 && aver_fw_origin_enabled() == 0, "null clears it");
}

static void testConversions() {
    AVER_INFO("=== conversions ===");
    Fake f;
    aver_fw_origin_provider p = providerFor(&f);
    std::mt19937_64 rng(0x5eed);
    std::uniform_real_distribution<double> far(-1.0e8, 1.0e8);
    std::uniform_real_distribution<double> near(-2.0e6, 2.0e6);
    std::uniform_int_distribution<int> steps(-60, 60);
    int badFrom = 0, badTo = 0, badLocal = 0, badAbs = 0, badRound = 0;
    for (int i = 0; i < 4000; ++i) {
        f.ox = steps(rng) * world::kOriginStepCm;
        f.oy = steps(rng) * world::kOriginStepCm;
        aver_fw_set_origin_provider(&p);

        const double ax = std::round(far(rng) * 4.0) / 4.0, ay = std::round(far(rng) * 4.0) / 4.0, az = std::round(near(rng) * 4.0) / 4.0;

        // from_abs is worldPosOfAbs, bit for bit, with the local on [0, 1600).
        const world::WorldPos want = world::worldPosOfAbs(ax, ay, az);
        int32_t c[3];
        float l3[3];
        aver_fw_worldpos_from_abs(ax, ay, az, c, l3);
        if (c[0] != want.chunk.x || c[1] != want.chunk.y || c[2] != want.chunk.z || std::memcmp(l3, &want.local, sizeof l3) != 0) ++badFrom;
        for (int k = 0; k < 3; ++k)
            if (!(l3[k] >= 0.0f && l3[k] < 1600.0f)) ++badFrom;

        // to_abs is absOf, and a position on the quarter-centimetre grid survives the split.
        double back[3];
        aver_fw_worldpos_to_abs(c, l3, back);
        const world::Vec3d wantAbs = world::absOf(want);
        if (back[0] != wantAbs.x || back[1] != wantAbs.y || back[2] != wantAbs.z) ++badTo;
        if (std::fabs(back[0] - ax) > 1e-3 || std::fabs(back[1] - ay) > 1e-3 || std::fabs(back[2] - az) > 1e-3) ++badRound;

        // to_local: the f64 subtraction, one rounding.
        const double lx = near(rng), ly = near(rng);
        const double wx = lx + static_cast<double>(f.ox), wy = ly + static_cast<double>(f.oy);
        float loc[3];
        aver_fw_origin_to_local(wx, wy, 12.5, loc);
        if (loc[0] != static_cast<float>(wx - static_cast<double>(f.ox)) || loc[1] != static_cast<float>(wy - static_cast<double>(f.oy)) ||
            loc[2] != 12.5f)
            ++badLocal;

        // to_abs adds O in f64 (exact), so it lands within half an f32 ulp of the absolute it names.
        double ab[3];
        aver_fw_origin_to_abs(loc[0], loc[1], loc[2], ab);
        if (ab[0] != static_cast<double>(loc[0]) + static_cast<double>(f.ox) || ab[1] != static_cast<double>(loc[1]) + static_cast<double>(f.oy) ||
            ab[2] != 12.5)
            ++badAbs;
        if (std::fabs(ab[0] - wx) > std::ldexp(1.0, std::ilogb(std::fabs(lx) + 1.0) - 23)) ++badAbs;
    }
    check(badFrom == 0, "from_abs equals worldPosOfAbs bit for bit and keeps the local in [0,1600): " + std::to_string(badFrom) + " off");
    check(badTo == 0, "to_abs equals absOf exactly: " + std::to_string(badTo) + " off");
    check(badRound == 0, "an absolute on the quarter-centimetre grid survives from_abs then to_abs: " + std::to_string(badRound) + " off");
    check(badLocal == 0, "to_local is the f64 subtraction narrowed once: " + std::to_string(badLocal) + " off");
    check(badAbs == 0, "to_abs adds O in f64: " + std::to_string(badAbs) + " off");

    // Through a WorldPos and back to the local frame the C++ authority (localOf) and to_local agree within the add's one rounding.
    f.ox = 4 * world::kOriginStepCm;
    f.oy = -3 * world::kOriginStepCm;
    const double px = static_cast<double>(f.ox) + 123456.75, py = static_cast<double>(f.oy) - 98765.5;
    float loc[3];
    aver_fw_origin_to_local(px, py, 40.0, loc);
    const Vec3 viaPos = world::localOf(world::worldPosOfAbs(px, py, 40.0), world::OriginCm{f.ox, f.oy});
    check(std::fabs(viaPos.x - loc[0]) <= 0.02f && std::fabs(viaPos.y - loc[1]) <= 0.02f, "to_local agrees with localOf of the WorldPos within the f32 grid");

    // Seams and negatives, by hand.
    int32_t c[3];
    float l3[3];
    aver_fw_worldpos_from_abs(-0.25, 1600.0, -1600.0, c, l3);
    check(c[0] == -1 && l3[0] == 1599.75f && c[1] == 1 && l3[1] == 0.0f && c[2] == -1 && l3[2] == 0.0f,
          "-0.25 is chunk -1 at 1599.75; 1600 is chunk 1; -1600 is chunk -1");
    aver_fw_worldpos_from_abs(10000000.0, 0, 0, c, l3);   // 100 km
    double back[3];
    aver_fw_worldpos_to_abs(c, l3, back);
    check(back[0] == 10000000.0 && back[1] == 0.0, "100 km is exact");
    aver_fw_set_origin_provider(nullptr);
}

int main() {
    AVER_INFO("FrameworkOriginAbiTest");
    testNoProvider();
    testProvider();
    testConversions();
    aver_fw_set_origin_provider(nullptr);
    if (g_failures) {
        AVER_ERROR("FrameworkOriginAbiTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("FrameworkOriginAbiTest: ok");
    return 0;
}
