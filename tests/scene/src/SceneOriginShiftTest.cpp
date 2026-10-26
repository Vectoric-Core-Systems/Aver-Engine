// SceneOriginShiftTest: World::shiftRoots, the scene half of a floating-origin shift (docs/plans/wave11/LARGE_WORLD.md 2.2.5).
// Every root loses the shift, children (parent-relative) are untouched, the next flush recomposes everything once, and the
// absolute position of each entity is preserved. No GPU, no physics. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool bitEqual(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof a) == 0; }
static bool vecBitEqual(const Vec3& a, const Vec3& b) { return bitEqual(a.x, b.x) && bitEqual(a.y, b.y) && bitEqual(a.z, b.z); }

static Vec3 worldPos(World& w, Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

static Entity make(World& w, const char* name, Vec3 p, Entity parent = kInvalidEntity) {
    Transform xf;
    xf.position = p;
    return parent == kInvalidEntity ? w.create(name, kInvalidEntity, xf) : w.create(name, parent, xf);
}

int main() {
    AVER_INFO("SceneOriginShiftTest");
    World& w = World::instance();
    const u32 baseCount = w.count();

    // A root far out, a five-deep chain under it, a second root with one child, and a lone root.
    const Entity r1 = make(w, "r1", Vec3{1700000.0f, -3300000.0f, 500.0f});
    Entity chain[5];
    chain[0] = make(w, "c0", Vec3{10.0f, 20.0f, 30.0f}, r1);
    for (int i = 1; i < 5; ++i) chain[i] = make(w, "c", Vec3{10.0f * static_cast<f32>(i + 1), -5.0f, 2.0f}, chain[i - 1]);
    const Entity r2 = make(w, "r2", Vec3{100.0f, 250.0f, 100.0f});
    const Entity r2c = make(w, "r2c", Vec3{7.0f, 8.0f, 9.0f}, r2);
    const Entity r3 = make(w, "r3", Vec3{-4096.0f, 2048.0f, -64.0f});
    std::vector<Entity> all{r1, chain[0], chain[1], chain[2], chain[3], chain[4], r2, r2c, r3};
    const std::vector<Entity> roots{r1, r2, r3};
    const std::vector<Entity> kids{chain[0], chain[1], chain[2], chain[3], chain[4], r2c};
    w.flush();
    check(w.count() == baseCount + all.size(), "the fixture has the entities it was built with");

    std::vector<Vec3> beforeLocal, beforeWorld;
    std::vector<u32> beforeRev;
    for (const Entity e : all) {
        beforeLocal.push_back(w.localTransform(e).position);
        beforeWorld.push_back(worldPos(w, e));
        beforeRev.push_back(w.worldRevision(e));
    }
    const u32 countBefore = w.count();

    // ---- the shift: one region step on X, a quarter on Y --------------------------------------------------------
    const u32 gen0 = w.originGeneration();
    const Vec3 delta{1638400.0f, 409600.0f, 0.0f};
    const u32 moved = w.shiftRoots(delta);
    check(moved >= roots.size(), "shiftRoots reports at least the roots it moved");
    check(w.originGeneration() == gen0 + 1, "originGeneration advances by one");
    check(w.count() == countBefore, "the entity count is unchanged");

    for (usize i = 0; i < all.size(); ++i) {
        const Entity e = all[i];
        const bool isRoot = w.parent(e) == kInvalidEntity;
        const Vec3& after = w.localTransform(e).position;
        if (isRoot) {
            const Vec3 want{beforeLocal[i].x - delta.x, beforeLocal[i].y - delta.y, beforeLocal[i].z - delta.z};
            check(vecBitEqual(after, want), std::string("root '") + w.name(e) + "' is exactly p - delta (plain f32)");
            check(bitEqual(after.z, beforeLocal[i].z), std::string("root '") + w.name(e) + "' keeps its Z bit for bit");
        } else {
            check(vecBitEqual(after, beforeLocal[i]), std::string("child '") + w.name(e) + "' is untouched (parent-relative)");
        }
    }

    // ---- the flush recomposes everything once --------------------------------------------------------------------
    const u32 recomposed = w.flush();
    check(recomposed == w.count(), "one flush recomposes every shifted root and every descendant (every entity in the world)");
    check(w.flush() == 0, "and the next flush has nothing left to do");
    for (usize i = 0; i < all.size(); ++i) {
        const Entity e = all[i];
        check(w.worldRevision(e) != beforeRev[i], std::string("'") + w.name(e) + "' world revision moved");
        const Vec3 now = worldPos(w, e);
        const Vec3 want = beforeWorld[i] - delta;
        // The far root sat on a 0.125 cm grid before the shift; everything else is exact or a few ULP at 1e4.
        check((now - want).size() <= 0.5f, std::string("absolute position of '") + w.name(e) + "' is preserved: before - delta");
    }

    // ---- a second shift stacks, an empty world part moves nothing ------------------------------------------------
    const Vec3 again{-1638400.0f, -409600.0f, 0.0f};
    w.shiftRoots(again);
    w.flush();
    check(w.originGeneration() == gen0 + 2, "a second shift advances the generation again");
    for (usize i = 0; i < all.size(); ++i)
        if (w.parent(all[i]) == kInvalidEntity)
            check((w.localTransform(all[i]).position - beforeLocal[i]).size() <= 0.5f,
                  std::string("shifting back returns '") + w.name(all[i]) + "' to where it was");

    // ---- a root that gains a parent stops being shifted, a child set free starts ---------------------------------
    {
        const Entity a = make(w, "reparentA", Vec3{1000.0f, 1000.0f, 0.0f});
        const Entity b = make(w, "reparentB", Vec3{50.0f, 50.0f, 0.0f});
        w.flush();
        check(w.setParent(b, a, false), "a root takes a parent");
        w.flush();
        const Vec3 aLocal = w.localTransform(a).position, bLocal = w.localTransform(b).position;
        w.shiftRoots(Vec3{600.0f, 0.0f, 0.0f});
        check(bitEqual(w.localTransform(a).position.x, aLocal.x - 600.0f), "the parent shifted");
        check(vecBitEqual(w.localTransform(b).position, bLocal), "the new child did not");
        w.setParent(b, kInvalidEntity, false);
        const Vec3 freeLocal = w.localTransform(b).position;
        w.shiftRoots(Vec3{-600.0f, 0.0f, 0.0f});
        check(bitEqual(w.localTransform(b).position.x, freeLocal.x + 600.0f), "a child set free is a root again and shifts");
        w.destroy(a);
        w.destroy(b);
        w.flush();
    }

    for (const Entity e : all) if (w.valid(e)) w.destroy(e);
    w.flush();

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
