// DecalAbiTest -- the decal C ABI (aver_decal_*): the pooled spawn API the C# layer calls. Needs
// DecalAbi.cpp in Aver.Scene's source list. No GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/decal_abi.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;
using namespace aver::scene;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

int main() {
    AVER_INFO("=== DecalAbiTest ===");
    World& w = World::instance();

    check(aver_decal_abi_version() == AVER_DECAL_ABI_VERSION, "the ABI version is what the header says");
    check(aver_decal_capacity() == 256 && aver_decal_active_count() == 0, "the default pool holds 256 and starts empty");
    check(aver_decal_pool_set_capacity(3) == 3 && aver_decal_capacity() == 3, "capacity can be set");
    check(aver_decal_pool_set_capacity(-5) == 0 && aver_decal_pool_set_capacity(1 << 30) == 8192, "and is clamped to [0, 8192]");
    aver_decal_pool_set_capacity(3);

    AverDecalSpawnDesc d;
    std::memset(&d, 0, sizeof d);
    d.position[0] = 100; d.position[1] = 50; d.position[2] = 0;
    d.normal[2] = 1.0f;                   // sitting on a floor
    d.sizeCm[0] = 40; d.sizeCm[1] = 60; d.sizeCm[2] = 60;
    d.baseTexture = 1234;
    d.sortOrder = 4;
    d.lifetimeSec = 2.0f;
    d.fadeOutSec = 0.5f;
    d.flags = AVER_DECAL_NO_NORMAL | 0x10 /* kDecalPooled must not be settable */ | 0x1 /* nor kDecalDisabled */;

    const int32_t e = aver_decal_spawn(&d);
    check(e > 0 && w.valid(static_cast<Entity>(e)), "a spawn returns a live entity");
    const CDecal* c = w.component<CDecal>(static_cast<Entity>(e), kComponentDecal);
    check(c && c->sizeCm[1] == 60.0f && c->baseTexture == 1234 && c->sortOrder == 4 && c->lifetimeSec == 2.0f,
          "the descriptor lands on the component");
    check(c && (c->flags & kDecalNoNormal) && (c->flags & kDecalPooled) && !(c->flags & kDecalDisabled),
          "only the channel bits come from the caller; the pool sets pooled and enabled itself");
    const Vec3 x = w.localTransform(static_cast<Entity>(e)).rotation.rotate({1, 0, 0});
    check(approx(x.z, -1.0f) && approx(x.x, 0.0f), "a surface normal of +Z makes the decal look straight down");
    check(approx(w.localTransform(static_cast<Entity>(e)).position.x, 100.0f), "at the requested position");
    check(aver_decal_active_count() == 1, "one active");

    // Explicit rotation when there is no normal.
    AverDecalSpawnDesc r = d;
    std::memset(r.normal, 0, sizeof r.normal);
    r.rotation[0] = 0; r.rotation[1] = 0; r.rotation[2] = std::sin(0.7853982f); r.rotation[3] = std::cos(0.7853982f);   // 90 degrees about Z
    r.lifetimeSec = 0.0f;
    const int32_t e2 = aver_decal_spawn(&r);
    const Vec3 x2 = w.localTransform(static_cast<Entity>(e2)).rotation.rotate({1, 0, 0});
    check(e2 > 0 && approx(x2.y, 1.0f), "with no normal the rotation quaternion is used");

    const int32_t e3 = aver_decal_spawn(&r);
    check(aver_decal_active_count() == 3 && e3 > 0, "three spawns fill the pool of three");
    const int32_t e4 = aver_decal_spawn(&r);
    check(e4 == e && aver_decal_active_count() == 3, "a fourth recycles the oldest entity");

    check(aver_decal_release(e2) == 1 && aver_decal_active_count() == 2 && aver_decal_release(e2) == 0, "release works once");
    aver_decal_tick(0.25f);
    check(aver_decal_active_count() == 2, "a tick before any lifetime ends changes nothing");
    aver_decal_clear();
    check(aver_decal_active_count() == 0, "clear releases everything");

    check(aver_decal_spawn(nullptr) == 0, "a null descriptor spawns nothing");
    aver_decal_pool_set_capacity(0);
    check(aver_decal_spawn(&d) == 0, "a pool of zero spawns nothing");
    aver_decal_pool_set_capacity(256);
    w.flush();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
