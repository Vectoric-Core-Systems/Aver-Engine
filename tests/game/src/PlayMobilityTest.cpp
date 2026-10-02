// PlayMobility: which entities count as moving during a play session.
//
// movable() runs once per visible mesh entity per frame, so it is written to cost almost nothing: an
// entity already movable returns at once, the matrix is hashed only when World::worldRevision says it
// was recomposed, and "is this under the pawn" is a lookup in a per-frame mark rather than an ancestor
// walk. None of that is allowed to change an answer, so the second half of this test replays the rule
// the way it was first written -- hash every matrix every frame, walk every entity's ancestors -- as an
// oracle and compares both over a few hundred frames of random moves, no-op writes, spawns, deaths,
// reparenting and pawn changes. The first half pins the rule's own cases by hand.
#include "aver/game/PlayMobility.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aver;
using aver::game::PlayMobility;
using aver::scene::Entity;
using aver::scene::World;

static int g_checks   = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

// The rule before movable() was made cheap, kept as the oracle. Same states, same transitions; the
// differences from PlayMobility are exactly the three this test exists to prove harmless.
class Oracle {
public:
    enum class State { Unknown, Seeded, Static, Provisional, Movable };
    struct Slot {
        Entity e = scene::kInvalidEntity;
        State state = State::Unknown;
        u32 stillFrames = 0;
        u64 lastSeenFrame = 0;
        u64 worldHash = 0;
    };

    void begin(const World& w) {
        slots_.clear();
        root_ = scene::kInvalidEntity;
        frame_ = 1;
        count_ = 0;
        active_ = true;
        for (u32 i = 0; i < w.count(); ++i) {
            Slot& s = slots_[scene::entityIndex(w.at(i))];
            s.e = w.at(i);
            s.state = State::Seeded;
        }
    }
    void seedMovable(const std::vector<Entity>& entities) {
        for (const Entity e : entities) {
            const auto it = slots_.find(scene::entityIndex(e));
            if (e == scene::kInvalidEntity || it == slots_.end() || it->second.e != e) continue;
            if (it->second.state == State::Movable) continue;
            it->second.state = State::Movable;
            ++count_;
        }
    }
    void beginFrame(Entity root) { ++frame_; root_ = root; }
    u32 movableCount() const { return count_; }

    bool movable(const World& w, Entity e, const Mat4& world) {
        if (!active_ || e == scene::kInvalidEntity) return false;
        Slot& s = slots_[scene::entityIndex(e)];
        if (s.e != e) { s = Slot{}; s.e = e; s.state = State::Provisional; }
        if (s.lastSeenFrame == frame_) return s.state == State::Provisional || s.state == State::Movable;
        const u64 h = hash(world);
        const bool firstSight = s.lastSeenFrame == 0;
        const bool moved = !firstSight && h != s.worldHash;
        s.lastSeenFrame = frame_;
        s.worldHash = h;
        if (s.state != State::Movable && underRoot(w, e)) { s.state = State::Movable; ++count_; return true; }
        switch (s.state) {
            case State::Seeded:
            case State::Unknown: s.state = State::Static; return false;
            case State::Static:
                if (moved) { s.state = State::Movable; ++count_; return true; }
                return false;
            case State::Provisional:
                s.stillFrames = (moved || firstSight) ? 0 : s.stillFrames + 1;
                if (s.stillFrames >= PlayMobility::kSettleFrames) { s.state = State::Static; return false; }
                return true;
            case State::Movable: return true;
        }
        return false;
    }

private:
    bool underRoot(const World& w, Entity e) const {
        if (root_ == scene::kInvalidEntity) return false;
        for (Entity a = e; w.valid(a); a = w.parent(a))
            if (a == root_) return true;
        return false;
    }
    static u64 hash(const Mat4& m) {
        u64 h = 1469598103934665603ull;
        const f32* f = &m.m[0][0];
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &f[i], sizeof(bits));
            h ^= static_cast<u64>(bits);
            h *= 1099511628211ull;
        }
        return h;
    }

    std::unordered_map<u32, Slot> slots_;
    Entity root_ = scene::kInvalidEntity;
    u64 frame_ = 0;
    u32 count_ = 0;
    bool active_ = false;
};

// One walk's question for one entity, the way drawWorld asks it: the world's own matrix.
bool ask(PlayMobility& pm, World& w, Entity e) { return pm.movable(w, e, w.worldMatrix(e)); }

// A frame: flush, the pawn root, then one ask per live entity.
void frame(PlayMobility& pm, World& w, Entity root) {
    w.flush();
    pm.beginFrame(root);
}

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
    u32 below(u32 n) { return next() % n; }
};

} // namespace

// ---- the rule, case by case --------------------------------------------------------------------------

static void testRule() {
    AVER_INFO("=== the rule ===");
    World& w = World::instance();
    PlayMobility pm;

    const Entity solo = w.create("pm-solo");
    const Entity untouched = w.create("pm-untouched");
    const Entity parent = w.create("pm-parent");
    const Entity child = w.create("pm-child", parent, Transform{});
    w.flush();

    check(!ask(pm, w, solo), "inactive: nothing is movable");
    pm.begin(w);
    check(pm.active(), "begin() starts a session");

    for (int f = 0; f < 5; ++f) {
        frame(pm, w, scene::kInvalidEntity);
        check(!ask(pm, w, solo) && !ask(pm, w, untouched) && !ask(pm, w, child),
              "level entities are static until they move");
    }

    // A write that lands on the value already there recomposes the matrix (the revision moves) and
    // changes no bit of it: not a move.
    w.setLocalPosition(untouched, w.localTransform(untouched).position);
    frame(pm, w, scene::kInvalidEntity);
    check(!ask(pm, w, untouched), "a recompose onto the same bits is not a move");

    w.setLocalPosition(solo, {10.0f, 0.0f, 0.0f});
    frame(pm, w, scene::kInvalidEntity);
    check(ask(pm, w, solo), "an entity whose matrix changed is movable");
    check(ask(pm, w, solo), "and asked again the same frame");
    frame(pm, w, scene::kInvalidEntity);
    check(ask(pm, w, solo), "and stays movable once it has stopped (sticky)");
    check(pm.movableCount() == 1, "one entity counted so far");

    w.setLocalPosition(parent, {0.0f, 50.0f, 0.0f});
    frame(pm, w, scene::kInvalidEntity);
    check(ask(pm, w, parent) && ask(pm, w, child), "moving a parent moves its child's matrix too");
    check(!ask(pm, w, untouched), "and nothing else");

    // ---- the pawn: its whole subtree is movable from the first frame it is named, still or not
    const Entity pawn = w.create("pm-pawn", scene::kInvalidEntity, Transform{});
    const Entity body = w.create("pm-body", pawn, Transform{});
    const Entity gun = w.create("pm-gun", body, Transform{});
    const Entity bystander = w.create("pm-bystander");
    // These four were born after begin(), so the pawn rule is the only thing that can make them
    // movable once the provisional window has passed.
    for (u32 f = 0; f < PlayMobility::kSettleFrames + 3; ++f) {
        frame(pm, w, pawn);
        ask(pm, w, pawn); ask(pm, w, body); ask(pm, w, gun); ask(pm, w, bystander);
    }
    frame(pm, w, pawn);
    check(ask(pm, w, pawn) && ask(pm, w, body) && ask(pm, w, gun), "the pawn and everything under it stays movable");
    check(!ask(pm, w, bystander), "a newborn outside the pawn settles to static after kSettleFrames still frames");
    check(!ask(pm, w, untouched), "and a level entity beside the pawn is unaffected");

    // A child hung under the pawn mid-session is movable for good, not merely for the provisional window
    // every newborn gets: it is still movable long after that window, standing perfectly still.
    const Entity late = w.create("pm-late", gun, Transform{});
    for (u32 f = 0; f < PlayMobility::kSettleFrames + 3; ++f) { frame(pm, w, pawn); ask(pm, w, late); }
    check(ask(pm, w, late), "an entity parented under the pawn joins it, and stays movable while still");
    // And one reparented out stays movable: sticky.
    w.setParent(late, scene::kInvalidEntity);
    frame(pm, w, pawn);
    check(ask(pm, w, late), "an entity leaving the pawn's tree keeps its movable state");

    // ---- a root that is gone marks nothing, and does not crash
    const Entity doomed = w.create("pm-doomed");
    w.destroy(doomed);
    frame(pm, w, doomed);
    check(!ask(pm, w, untouched), "a dead pawn handle makes nothing movable");

    // ---- newborns: provisional until they hold still, and a move while provisional restarts the count
    const Entity born = w.create("pm-born");
    u32 trueFrames = 0;
    for (u32 f = 0; f < PlayMobility::kSettleFrames + 5; ++f) {
        frame(pm, w, scene::kInvalidEntity);
        if (ask(pm, w, born)) ++trueFrames;
    }
    check(trueFrames == PlayMobility::kSettleFrames,
          "a newborn counts as movable for exactly kSettleFrames frames, then static");
    check(!ask(pm, w, born), "and is static afterwards");
    w.setLocalPosition(born, {1.0f, 2.0f, 3.0f});
    frame(pm, w, scene::kInvalidEntity);
    check(ask(pm, w, born), "a static entity that then moves becomes movable for good");

    // ---- seeding, and end()
    const Entity seeded = w.create("pm-seeded");
    pm.end();
    pm.begin(w);
    pm.seedMovable({seeded, scene::kInvalidEntity});
    frame(pm, w, scene::kInvalidEntity);
    check(ask(pm, w, seeded), "a seeded entity is movable from its first frame");
    check(!ask(pm, w, untouched), "an unseeded one is not");
    pm.end();
    check(!ask(pm, w, seeded) && !pm.active(), "end() ends it");

    for (const Entity e : {solo, untouched, parent, pawn, bystander, late, born, seeded}) w.destroy(e);
    w.flush();
}

// ---- against the oracle ------------------------------------------------------------------------------

static void testAgainstOracle() {
    AVER_INFO("=== against the original rule, random session ===");
    World& w = World::instance();
    Rng rng;
    std::vector<Entity> mine;   // everything this test made, so it can clean up

    for (u32 i = 0; i < 48; ++i) {
        const Entity parent = (i % 4 != 0 && !mine.empty()) ? mine.back() : scene::kInvalidEntity;
        Transform t;
        t.position = {static_cast<f32>(i) * 10.0f, 0.0f, 0.0f};
        mine.push_back(w.create("pmr", parent, t));
    }
    w.flush();

    PlayMobility pm;
    Oracle oracle;
    pm.begin(w);
    oracle.begin(w);
    const std::vector<Entity> seeds = {mine[3], mine[17], mine[30]};
    pm.seedMovable(seeds);
    oracle.seedMovable(seeds);

    Entity root = scene::kInvalidEntity;
    std::vector<Entity> graveyard;
    u32 mismatches = 0, asked = 0, answeredTrue = 0;

    for (u32 f = 0; f < 700; ++f) {
        const u32 live = w.count();
        if (live == 0) break;
        auto pick = [&]() { return w.count() ? w.at(rng.below(w.count())) : scene::kInvalidEntity; };

        if (rng.below(100) < 45) {
            const Entity e = pick();
            w.setLocalPosition(e, {static_cast<f32>(rng.below(2000)), static_cast<f32>(rng.below(2000)), 0.0f});
        }
        if (rng.below(100) < 20) {
            const Entity e = pick();
            w.setLocalPosition(e, w.localTransform(e).position);   // recompose, same bits
        }
        if (rng.below(100) < 12 && live < 160) {
            const Entity parent = rng.below(2) ? pick() : scene::kInvalidEntity;
            Transform t;
            t.position = {static_cast<f32>(rng.below(500)), 0.0f, 0.0f};
            mine.push_back(w.create("pmr-born", parent, t));
        }
        if (rng.below(100) < 6 || live >= 160) {
            const Entity e = pick();
            graveyard.push_back(e);
            w.destroy(e);
        }
        if (rng.below(100) < 12) {
            const Entity e = pick();
            w.setParent(e, rng.below(3) ? pick() : scene::kInvalidEntity);   // a cycle is refused, fine
        }
        if (rng.below(100) < 10) {
            switch (rng.below(3)) {
                case 0: root = scene::kInvalidEntity; break;
                case 1: root = pick(); break;
                default: root = graveyard.empty() ? scene::kInvalidEntity : graveyard[rng.below(static_cast<u32>(graveyard.size()))]; break;
            }
        }
        if (f == 350) {   // a late seed: some alive, some born since, some dead
            std::vector<Entity> late = {pick(), pick(), mine.back(), graveyard.empty() ? scene::kInvalidEntity : graveyard.back()};
            pm.seedMovable(late);
            oracle.seedMovable(late);
        }

        w.flush();
        pm.beginFrame(root);
        oracle.beginFrame(root);
        for (u32 i = 0; i < w.count(); ++i) {
            const Entity e = w.at(i);
            const Mat4& m = w.worldMatrix(e);
            // Twice, as the prepass and the colour walk do.
            for (int pass = 0; pass < 2; ++pass) {
                const bool a = pm.movable(w, e, m);
                const bool b = oracle.movable(w, e, m);
                ++asked;
                if (b) ++answeredTrue;
                if (a != b && ++mismatches <= 5)
                    AVER_ERROR("  FAIL  frame {} entity {:#x} pass {}: movable()={} oracle={}", f, e, pass, a, b);
            }
        }
        if (pm.movableCount() != oracle.movableCount() && ++mismatches <= 5)
            AVER_ERROR("  FAIL  frame {}: movableCount {} vs oracle {}", f, pm.movableCount(), oracle.movableCount());
    }
    AVER_INFO("   {} answers compared", asked);
    check(mismatches == 0, "movable() agrees with the original rule on every answer and every count");
    check(answeredTrue > 0 && answeredTrue < asked, "the session exercised both outcomes");

    pm.end();
    for (const Entity e : mine) if (w.valid(e)) w.destroy(e);
    w.flush();
}

int main() {
    AVER_INFO("PlayMobilityTest");
    testRule();
    testAgainstOracle();

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}

#else

int main() {
    AVER_INFO("PlayMobilityTest: skipped, built without the scene module");
    return 0;
}

#endif
