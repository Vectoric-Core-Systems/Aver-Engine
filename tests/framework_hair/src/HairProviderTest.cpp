// hair::hairProvider (the framework relay's provider) on a HairSystem with no groom built and no physics world: the validation that happens
// before anything reaches the system, the script collider bookkeeping (kept per entity, capped, cleared) and the state read. Building a groom and
// moving the hulls need a physics world and a skeleton and are tests/hair and the headed pass's business.
#include "aver/core/Log.hpp"
#include "aver/hair/HairSystem.hpp"
#include "aver/scene/World.hpp"

#include <limits>
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

namespace {

// op, entity, in[], text -> the provider's answer; out[0] is written when the op fills it.
i32 call(hair::HairSystem& sys, i32 op, i32 entity, const float* in = nullptr, i32 n = 0, const char* text = nullptr, i32* out = nullptr, i32 cap = 0) {
    return hair::hairProvider(op, entity, in, n, text, nullptr, out, cap, &sys);
}

} // namespace

int main() {
    AVER_INFO("HairProviderTest");
    scene::World& w = scene::World::instance();
    hair::HairSystem::registerComponents(w);
    hair::HairSystem sys;

    const scene::Entity withHair = w.create("HairHead");
    const scene::Entity without = w.create("Plain");
    w.flush();
    check(hair::HairSystem::attachHair(w, withHair, 0x1234u) != nullptr, "the component attaches with defaults");
    check(hair::HairSystem::attachHair(w, withHair, 0x1234u) == nullptr, "a second attach is refused");

    constexpr i32 kTeleport = 1, kAdd = 2, kClear = 3, kState = 4;
    const i32 e = static_cast<i32>(withHair);

    AVER_INFO("=== refusals before the system ===");
    {
        const float sphere[7] = {0, 8, 0, 0, 0, 0, 0};
        check(hair::hairProvider(kTeleport, e, nullptr, 0, nullptr, nullptr, nullptr, 0, nullptr) == 0, "no user pointer is a refusal");
        check(call(sys, kTeleport, 0) == 0, "entity 0 is a refusal");
        check(call(sys, kTeleport, 0x7FFFFFF0) == 0, "a dead entity is a refusal");
        check(call(sys, 99, e) == 0, "an unknown op is a refusal");
        float bad[7] = {0, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0};
        check(call(sys, kAdd, e, bad, 7, "head") == 0, "a non-finite number is a refusal");
        check(call(sys, kAdd, e, sphere, 6, "head") == 0, "too few numbers is a refusal");
        check(call(sys, kAdd, e, sphere, 7, nullptr) == 0 && call(sys, kAdd, e, sphere, 7, "") == 0, "no bone is a refusal");
        check(call(sys, kAdd, static_cast<i32>(without), sphere, 7, "head") == 0, "an entity with no CHair takes no collider");
        check(sys.colliderCount(withHair) == 0, "none of the refusals left a collider behind");
    }

    AVER_INFO("=== script colliders ===");
    {
        i32 out[1] = {0};
        const float sphere[7] = {0, 8, 0, 0, 0, 0, 0};
        check(call(sys, kAdd, e, sphere, 7, "head", out, 1) == 1 && out[0] == 1 && sys.colliderCount(withHair) == 1, "a sphere is taken and counted");
        const float badShape[7] = {3, 8, 0, 0, 0, 0, 0};
        check(call(sys, kAdd, e, badShape, 7, "head") == 0, "shape 3 is refused");
        const float noRadius[7] = {0, 0, 0, 0, 0, 0, 0};
        check(call(sys, kAdd, e, noRadius, 7, "head") == 0, "a sphere without a radius is refused");
        const float flatCapsule[7] = {1, 4, 0, 0, 0, 0, 0};
        check(call(sys, kAdd, e, flatCapsule, 7, "neck") == 0, "a capsule without a half height is refused");
        const float capsule[7] = {1, 4, 6, 0, 0, 0, 0};
        check(call(sys, kAdd, e, capsule, 7, "neck", out, 1) == 1 && out[0] == 2, "a capsule is taken");
        const float flatBox[7] = {2, 5, 5, 0, 0, 0, 0};
        check(call(sys, kAdd, e, flatBox, 7, "spine") == 0, "a box without a third extent is refused");
        const float box[7] = {2, 5, 5, 5, 0, 0, 0};
        check(call(sys, kAdd, e, box, 7, "spine") == 1, "a box is taken");
        i32 count = static_cast<i32>(sys.colliderCount(withHair));
        while (count < 16) {
            check(call(sys, kAdd, e, sphere, 7, "head") == 1, "filling up to the cap");
            ++count;
        }
        check(call(sys, kAdd, e, sphere, 7, "head") == 0 && sys.colliderCount(withHair) == 16, "the seventeenth is refused");
        check(call(sys, kClear, e) == 1 && sys.colliderCount(withHair) == 0, "clearing removes them all");
        check(call(sys, kClear, e) == 0, "clearing again reports there was nothing");
    }

    AVER_INFO("=== state and teleport before a groom exists ===");
    {
        i32 six[6] = {-1, -1, -1, -1, -1, -1};
        check(call(sys, kState, e, nullptr, 0, nullptr, six, 6) == 1 && six[5] == 0 && six[4] == 0, "state: not built, no colliders");
        const float sphere[7] = {0, 8, 0, 0, 0, 0, 0};
        call(sys, kAdd, e, sphere, 7, "head");
        check(call(sys, kState, e, nullptr, 0, nullptr, six, 6) == 1 && six[4] == 1, "state: the collider is counted before the build");
        check(call(sys, kState, static_cast<i32>(without), nullptr, 0, nullptr, six, 6) == 0, "state: no CHair is a refusal");
        check(call(sys, kTeleport, e) == 0, "teleport: nothing built yet is a refusal");
        check(!sys.isBuilt(withHair) && sys.hairOf(withHair) == 0, "the system agrees the groom is not built");
        hair::HairSystem::StateOut st;
        check(sys.stateOf(w, withHair, st) && !st.built && st.colliders == 1 && st.outState == 0, "stateOf reads the component's zero-filled outputs");
    }

    if (g_failures == 0) AVER_INFO("HairProviderTest: {} checks passed", g_checks);
    else                 AVER_ERROR("HairProviderTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
