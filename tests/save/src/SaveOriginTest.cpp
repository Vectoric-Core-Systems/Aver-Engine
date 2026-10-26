// SaveOriginTest: a save taken at one floating origin restores at another with every root in the same absolute place
// (docs/plans/wave11/LARGE_WORLD.md 2.6.4, 5.2). Origin-relative f32 positions are meaningless without the origin they were
// measured from, so the origin travels with the save (the additive ORIG chunk) and restore re-expresses each root against the
// origin the world has now. Also: a save with no ORIG is origin zero (every save made before this), children are untouched
// (parent-relative), and the capture skip predicate leaves chunk-streamed scenery out (with a control that saves it).
// No GPU, no framework. Exit code = failure count.
#include "aver/save/SaveWorld.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

static constexpr i64 kStep = 1638400;   // one region: the unit every origin is a multiple of

// The absolute x / y of an entity's local position at origin (ox, oy), in f64.
static f64 absX(scene::World& w, scene::Entity e, i64 ox) { return static_cast<f64>(w.localTransform(e).position.x) + static_cast<f64>(ox); }
static f64 absY(scene::World& w, scene::Entity e, i64 oy) { return static_cast<f64>(w.localTransform(e).position.y) + static_cast<f64>(oy); }

// A SkipFn: entities in the set, reached through `user`, are streamed scenery.
static bool skipStreamed(scene::Entity e, void* user) {
    return static_cast<const std::set<scene::Entity>*>(user)->count(e) != 0;
}

int main() {
    AVER_INFO("SaveOriginTest");
    scene::World& w = scene::World::instance();

    // ---- capture at O1, restore at O2 ------------------------------------------------------------------------------------
    {
        clearWorld(w);
        const i64 o1x = 2 * kStep, o1y = -kStep;
        // Positions are origin-relative: the player stands 4,000 cm and 250.5 cm from where the origin is; a crate is 600 m out.
        Transform pxf; pxf.position = Vec3{4000.0f, 250.5f, 120.0f};
        const scene::Entity player = w.create("Player", scene::kInvalidEntity, pxf);
        Transform cxf; cxf.position = Vec3{10.0f, -20.0f, 30.0f};
        const scene::Entity hand = w.create("Hand", player, cxf);
        Transform bxf; bxf.position = Vec3{-60000.0f, 8192.5f, 5.0f};
        const scene::Entity crate = w.create("Crate", scene::kInvalidEntity, bxf);
        w.flush();

        const f64 playerAbsX = absX(w, player, o1x), playerAbsY = absY(w, player, o1y);
        const f64 crateAbsX = absX(w, crate, o1x), crateAbsY = absY(w, crate, o1y);
        const Vec3 handLocal = w.localTransform(hand).position;

        fmt::OcSaveData snap;
        std::string why;
        save::CaptureOptions co;
        co.originX = o1x;
        co.originY = o1y;
        check(save::capture(w, snap, co, &why), "the world captures at origin O1: " + why);
        check(snap.originX == o1x && snap.originY == o1y, "and the snapshot carries O1");

        std::vector<u8> bytes;
        check(fmt::writeOcSave(snap, bytes, &why), "it writes: " + why);
        fmt::OcSaveData reloaded;
        check(fmt::parseOcSave(bytes.data(), bytes.size(), reloaded, &why), "and parses: " + why);
        check(reloaded.originX == o1x && reloaded.originY == o1y, "the origin comes back through the file");

        // Restore into a world whose origin is somewhere else entirely.
        const i64 o2x = 5 * kStep, o2y = 3 * kStep;
        clearWorld(w);
        save::RestoreOptions ro;
        ro.originX = o2x;
        ro.originY = o2y;
        check(save::restore(reloaded, w, ro, &why), "it restores at origin O2: " + why);

        const scene::Entity rPlayer = w.find("Player"), rHand = w.find("Hand"), rCrate = w.find("Crate");
        check(rPlayer != scene::kInvalidEntity && rHand != scene::kInvalidEntity && rCrate != scene::kInvalidEntity, "all three come back");
        if (rPlayer != scene::kInvalidEntity && rHand != scene::kInvalidEntity && rCrate != scene::kInvalidEntity) {
            check(std::fabs(absX(w, rPlayer, o2x) - playerAbsX) < 0.01 && std::fabs(absY(w, rPlayer, o2y) - playerAbsY) < 0.01,
                  "the player is at the same absolute place");
            check(std::fabs(absX(w, rCrate, o2x) - crateAbsX) < 0.01 && std::fabs(absY(w, rCrate, o2y) - crateAbsY) < 0.01,
                  "and so is the crate");
            const Vec3 hl = w.localTransform(rHand).position;
            check(hl.x == handLocal.x && hl.y == handLocal.y && hl.z == handLocal.z, "the child is parent-relative and not shifted");
            check(w.localTransform(rPlayer).position.z == 120.0f, "Z is never shifted");
            // A negative control by arithmetic: restoring WITHOUT telling it the origin leaves the roots at the old local numbers,
            // which at O2 is a different absolute place.
            check(std::fabs((static_cast<f64>(4000.0f) + static_cast<f64>(o2x)) - playerAbsX) > 1000000.0,
                  "(control) the saved local number alone would land 49 km from where the player was");
        }

        // The same snapshot at the origin it was taken at is a byte-for-byte placement (adds 0.0).
        clearWorld(w);
        save::RestoreOptions same;
        same.originX = o1x;
        same.originY = o1y;
        check(save::restore(reloaded, w, same, &why), "restoring at O1 itself: " + why);
        const scene::Entity sp = w.find("Player");
        check(sp != scene::kInvalidEntity && w.localTransform(sp).position.x == 4000.0f && w.localTransform(sp).position.y == 250.5f,
              "leaves the saved numbers exactly");
    }

    // ---- no ORIG chunk is origin zero -------------------------------------------------------------------------------------
    {
        clearWorld(w);
        Transform xf; xf.position = Vec3{1000.0f, 2000.0f, 50.0f};
        w.create("Old", scene::kInvalidEntity, xf);
        w.flush();
        fmt::OcSaveData snap;
        std::string why;
        check(save::capture(w, snap, {}, &why), "a capture with no origin given: " + why);
        check(snap.originX == 0 && snap.originY == 0, "is origin zero");
        std::vector<u8> bytes;
        check(fmt::writeOcSave(snap, bytes, &why), "and writes: " + why);
        bool sawOrig = false;
        for (usize i = 0; i + 4 <= bytes.size(); ++i)
            if (bytes[i] == 'O' && bytes[i + 1] == 'R' && bytes[i + 2] == 'I' && bytes[i + 3] == 'G') sawOrig = true;
        check(!sawOrig, "with no ORIG chunk in the file");
        fmt::OcSaveData back;
        check(fmt::parseOcSave(bytes.data(), bytes.size(), back, &why) && back.originX == 0 && back.originY == 0, "which reads back as origin zero");

        // An old save loaded into a world that has rebased since: its roots were at origin zero, so they move by -O.
        clearWorld(w);
        save::RestoreOptions ro;
        ro.originX = 3 * kStep;
        ro.originY = 0;
        check(save::restore(back, w, ro, &why), "an old save restores into a world at O=(3 regions, 0): " + why);
        const scene::Entity e = w.find("Old");
        check(e != scene::kInvalidEntity && std::fabs(absX(w, e, ro.originX) - 1000.0) < 0.01 && std::fabs(absY(w, e, 0) - 2000.0) < 0.01,
              "and keeps its absolute position (the origin-zero place it was saved at)");
    }

    // ---- the skip predicate: chunk-streamed scenery is not part of a save -----------------------------------------------
    {
        clearWorld(w);
        std::set<scene::Entity> streamed;
        Transform xf;
        w.create("Keeper", scene::kInvalidEntity, xf);
        for (int i = 0; i < 5; ++i) streamed.insert(w.create("Tree", scene::kInvalidEntity, xf));
        // A child of streamed scenery goes with it. The predicate names it too: capture reaches it only through its parent, but
        // restore's teardown asks the predicate about every entity, so a host's owns() must cover descendants.
        streamed.insert(w.create("Branch", *streamed.begin(), xf));
        w.flush();

        fmt::OcSaveData withSkip, control;
        std::string why;
        save::CaptureOptions co;
        co.skip = &skipStreamed;
        co.host.user = &streamed;
        check(save::capture(w, withSkip, co, &why), "a capture with the streamed-scenery predicate: " + why);
        check(withSkip.entities.size() == 1 && withSkip.entities[0].name == "Keeper", "saves only what the player owns (streamed trees and their children left out)");
        check(save::capture(w, control, {}, &why), "(control) a capture without it: " + why);
        check(control.entities.size() == 7, "(control) saves every tree and the branch as plain entities");

        // Restore with the same predicate leaves the live streamed scenery alone.
        save::RestoreOptions ro;
        ro.skip = &skipStreamed;
        ro.host.user = &streamed;
        check(save::restore(withSkip, w, ro, &why), "restoring with the same predicate: " + why);
        bool streamedAlive = true;
        for (const scene::Entity e : streamed) streamedAlive = streamedAlive && w.valid(e) && !w.destroyPending(e);
        check(streamedAlive, "leaves the streamed scenery alive (it is not torn down and not duplicated)");
        check(w.count() == 7, "and the world holds exactly the streamed set, its child and the restored keeper");
    }

    clearWorld(w);
    AVER_INFO("SaveOriginTest: {} failure(s)", g_failures);
    return g_failures;
}
