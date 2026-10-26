// EditorOriginTest: the editor's side of a floating-origin shift (docs/plans/wave11/LARGE_WORLD.md 2.7, phase 2 step B).
//
//   Rebase          EditorOriginRebase.hpp against stand-ins that carry SandboxApp::EditCmd's member names: every kind that holds an
//                   origin-relative position moves by exactly -delta (world positions always; local positions only for a root),
//                   every kind that does not (strokes, water records, rename...) is left alone; the clipboard; the Play snapshot
//   Undo, end to end  a real scene::World and the real game::OriginParticipants: an undo recorded before a shift, rebased by the
//                   editor participant, puts the entity back at the same ABSOLUTE position; (control) without the rebase it lands a
//                   region away
//   Gesture gate    EditorOriginParticipant as OriginBindings::editorPrepare: an open gesture refuses the shift, nothing moves, the
//                   reason is recorded and the retry goes through; an urgent shift ends the gesture (restoring the pre-gesture
//                   transform) and commits; the rebasers run at commit, after the scene had shifted, in registration order
//   Overlay key     the collider overlay's signature changes with the origin generation and only with it
//   Console         world.origin / rebase / margin / enable / policy / tp against the real participants, including the 50 km and
//                   100 km teleports landing on the exact absolute position and a refused move reporting why
//   Readout         the Show > World Origin model: region, camera in km, distance to the next shift, the counters, per participant
//
// No GPU, no window, no ImGui. Exit code = failure count.
#include "EditorOriginParticipant.hpp"
#include "EditorOriginRebase.hpp"
#include "WorldConsole.hpp"
#include "WorldOriginReadout.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static bool contains(const std::vector<std::string>& lines, const char* needle) {
    for (const std::string& l : lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}

constexpr f32 kStep = 1638400.0f;   // one region, cm
constexpr i64 kStepI = world::kOriginStepCm;

// ---- stand-ins with SandboxApp::EditCmd's member names ----------------------------------------------------------------

namespace fake {
using Entity = u32;
constexpr Entity kInvalid = 0xFFFFFFFFu;

struct Xf { Vec3 pos{}, rotDeg{}, scale{1.0f, 1.0f, 1.0f}; };

struct PrefabState {
    struct Node { Transform local; Entity parent = kInvalid; };
    std::vector<Node> instances, plains;
};
struct PrefabEdit { PrefabState before, after; };

struct Cmd {
    enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj, LandscapeStroke, FoliageStroke, Reparent, Material, Rename,
                      RemoveComponent, Visibility, Collision, Animation, Prefab, LevelInstance, LandscapeWeights, WaterBodies, PlacedComponent };
    Kind kind = Kind::Transform;
    Xf before{}, after{};
    struct AlsoMoved { u32 id = 0; Transform beforeLocal, afterLocal; bool root = true; };
    std::vector<AlsoMoved> alsoMoved;
    u32 parentId = 0, reparentOldParentId = 0, reparentNewParentId = 0;
    struct Obj { Vec3 pos{}; } objSnapshot;
    std::shared_ptr<PrefabEdit> prefab;
};

struct Clipboard {
    struct Ent { Xf xform; };
    std::vector<Ent> entities;
    bool hasObject = false;
    struct Obj { Vec3 pos{}; } object;
};

struct Saved { Entity e; Transform xf; bool visible = true; };
} // namespace fake

static Transform xfAt(f32 x, f32 y, f32 z) { Transform t; t.position = Vec3{x, y, z}; return t; }

// ---- Rebase ------------------------------------------------------------------------------------------------------------

static void testRebase() {
    AVER_INFO("=== Rebase ===");
    using fake::Cmd;
    using K = Cmd::Kind;
    const Vec3 d{kStep, 0.0f, 0.0f};
    const f32 X = 1100000.0f;   // 11 km out: where a shift is due

    {   // Transform: before / after are WORLD positions and always move; alsoMoved are LOCAL and move only for a root
        Cmd c;
        c.kind = K::Transform;
        c.before.pos = Vec3{X, 400.0f, 90.0f};
        c.after.pos = Vec3{X + 250.0f, 400.0f, 90.0f};
        c.alsoMoved.push_back({1, xfAt(X + 10.0f, 5.0f, 0.0f), xfAt(X + 60.0f, 5.0f, 0.0f), true});
        c.alsoMoved.push_back({2, xfAt(30.0f, 0.0f, 0.0f), xfAt(80.0f, 0.0f, 0.0f), false});
        editor::originRebase::rebaseCmd(c, d, fake::kInvalid);
        check(same(c.before.pos, Vec3{X - kStep, 400.0f, 90.0f}) && same(c.after.pos, Vec3{X + 250.0f - kStep, 400.0f, 90.0f}),
              "Transform: the world before and after move by exactly -delta on X (Y and Z alone)");
        check(same(c.alsoMoved[0].beforeLocal.position, Vec3{X + 10.0f - kStep, 5.0f, 0.0f}) &&
              same(c.alsoMoved[0].afterLocal.position, Vec3{X + 60.0f - kStep, 5.0f, 0.0f}), "Transform: a moved ROOT's local pair moves");
        check(same(c.alsoMoved[1].beforeLocal.position, Vec3{30.0f, 0.0f, 0.0f}) && same(c.alsoMoved[1].afterLocal.position, Vec3{80.0f, 0.0f, 0.0f}),
              "Transform: a moved CHILD's parent-relative local pair does not");
    }
    {   // Create / Destroy: `after` is the local at capture; a root moves, a child does not
        Cmd root; root.kind = K::Destroy; root.after.pos = Vec3{X, 1.0f, 2.0f}; root.parentId = 0;
        Cmd kid;  kid.kind = K::Create;   kid.after.pos = Vec3{X, 1.0f, 2.0f};  kid.parentId = 7;
        editor::originRebase::rebaseCmd(root, d, fake::kInvalid);
        editor::originRebase::rebaseCmd(kid, d, fake::kInvalid);
        check(same(root.after.pos, Vec3{X - kStep, 1.0f, 2.0f}), "Destroy of a root: the captured local moves");
        check(same(kid.after.pos, Vec3{X, 1.0f, 2.0f}), "Create of a child: its parent-relative local does not");
    }
    {   // Reparent: both ends are local; a root on that end moves
        Cmd c; c.kind = K::Reparent;
        c.before.pos = Vec3{X, 0.0f, 0.0f}; c.reparentOldParentId = 0;   // was a root
        c.after.pos = Vec3{12.0f, 0.0f, 0.0f}; c.reparentNewParentId = 9;   // becomes a child
        editor::originRebase::rebaseCmd(c, d, fake::kInvalid);
        check(same(c.before.pos, Vec3{X - kStep, 0.0f, 0.0f}) && same(c.after.pos, Vec3{12.0f, 0.0f, 0.0f}),
              "Reparent: the root end moves, the child end does not");
    }
    {   // the placeholder objects
        Cmd a; a.kind = K::CreateObj; a.objSnapshot.pos = Vec3{X, 3.0f, 4.0f};
        Cmd b; b.kind = K::DestroyObj; b.objSnapshot.pos = Vec3{X, 3.0f, 4.0f};
        editor::originRebase::rebaseCmd(a, d, fake::kInvalid);
        editor::originRebase::rebaseCmd(b, d, fake::kInvalid);
        check(same(a.objSnapshot.pos, Vec3{X - kStep, 3.0f, 4.0f}) && same(b.objSnapshot.pos, Vec3{X - kStep, 3.0f, 4.0f}), "CreateObj and DestroyObj move");
    }
    {   // prefab states: a root instance / plain subtree moves, one hanging from a parent does not
        Cmd c; c.kind = K::Prefab;
        c.prefab = std::make_shared<fake::PrefabEdit>();
        c.prefab->before.instances.push_back({xfAt(X, 0.0f, 0.0f), fake::kInvalid});
        c.prefab->before.instances.push_back({xfAt(5.0f, 0.0f, 0.0f), 3});
        c.prefab->after.plains.push_back({xfAt(X, 7.0f, 0.0f), fake::kInvalid});
        c.prefab->after.plains.push_back({xfAt(6.0f, 0.0f, 0.0f), 4});
        editor::originRebase::rebaseCmd(c, d, fake::kInvalid);
        check(same(c.prefab->before.instances[0].local.position, Vec3{X - kStep, 0.0f, 0.0f}) &&
              same(c.prefab->before.instances[1].local.position, Vec3{5.0f, 0.0f, 0.0f}), "Prefab: a root instance moves, a parented one does not");
        check(same(c.prefab->after.plains[0].local.position, Vec3{X - kStep, 7.0f, 0.0f}) &&
              same(c.prefab->after.plains[1].local.position, Vec3{6.0f, 0.0f, 0.0f}), "and plain subtrees the same");
    }
    {   // kinds with no origin-relative position are left exactly as they were
        const K others[] = {K::LandscapeStroke, K::FoliageStroke, K::Material, K::Rename, K::RemoveComponent, K::Visibility, K::Collision,
                            K::Animation, K::LevelInstance, K::LandscapeWeights, K::WaterBodies, K::PlacedComponent};
        bool allUntouched = true;
        for (const K k : others) {
            Cmd c; c.kind = k; c.before.pos = Vec3{X, 1.0f, 2.0f}; c.after.pos = Vec3{X, 3.0f, 4.0f};
            editor::originRebase::rebaseCmd(c, d, fake::kInvalid);
            allUntouched = allUntouched && same(c.before.pos, Vec3{X, 1.0f, 2.0f}) && same(c.after.pos, Vec3{X, 3.0f, 4.0f});
        }
        check(allUntouched, "strokes, water records, level instances, visibility, rename, material, components: untouched");
    }
    {   // both stacks, and an undo depth of hundreds stays one pass
        std::vector<Cmd> undo, redo;
        for (int i = 0; i < 200; ++i) { Cmd c; c.kind = K::Transform; c.before.pos = Vec3{X, float(i), 0.0f}; c.after.pos = Vec3{X + 1.0f, float(i), 0.0f}; undo.push_back(c); }
        for (int i = 0; i < 50; ++i) { Cmd c; c.kind = K::Transform; c.before.pos = Vec3{X, float(i), 0.0f}; c.after.pos = Vec3{X + 1.0f, float(i), 0.0f}; redo.push_back(c); }
        editor::originRebase::rebaseStacks(undo, redo, d, fake::kInvalid);
        bool ok = true;
        for (const Cmd& c : undo) ok = ok && c.before.pos.x == X - kStep && c.after.pos.x == X + 1.0f - kStep;
        for (const Cmd& c : redo) ok = ok && c.before.pos.x == X - kStep && c.after.pos.x == X + 1.0f - kStep;
        check(ok, "the undo stack (200) and the redo stack (50) are both rebased");
    }
    {   // clipboard and Play snapshot
        fake::Clipboard cb;
        cb.entities.push_back({fake::Xf{Vec3{X, 0.0f, 0.0f}, {}, {1, 1, 1}}});
        cb.entities.push_back({fake::Xf{Vec3{X + 300.0f, 50.0f, 0.0f}, {}, {1, 1, 1}}});
        cb.hasObject = true; cb.object.pos = Vec3{X, 9.0f, 0.0f};
        const f32 gapBefore = cb.entities[1].xform.pos.x - cb.entities[0].xform.pos.x;
        editor::originRebase::rebaseClipboard(cb, d);
        check(cb.entities[0].xform.pos.x == X - kStep && cb.entities[1].xform.pos.y == 50.0f && cb.object.pos.x == X - kStep, "the clipboard moves");
        check(cb.entities[1].xform.pos.x - cb.entities[0].xform.pos.x == gapBefore, "and the layout between its entries (what a paste uses) is unchanged");

        std::vector<fake::Saved> snap = {{1, xfAt(X, 0.0f, 0.0f)}, {2, xfAt(40.0f, 0.0f, 0.0f)}};
        editor::originRebase::rebasePlaySnapshot(snap, d, [](fake::Entity e) { return e == 1; });   // 2 is a child
        check(snap[0].xf.position.x == X - kStep && snap[1].xf.position.x == 40.0f, "the Play snapshot: a root moves, a child does not");
    }
}

// ---- Undo end to end, with the real scene and the real participants ----------------------------------------------------

static f64 absX(const game::OriginParticipants& op, scene::World& w, scene::Entity e) {
    return static_cast<f64>(w.localTransform(e).position.x) + static_cast<f64>(op.origin().x);
}

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

static scene::Entity spawn(scene::World& w, Vec3 p, scene::Entity parent = scene::kInvalidEntity) {
    Transform xf; xf.position = p;
    return w.create("e", parent, xf);
}

static void testUndoEndToEnd(scene::World& w) {
    AVER_INFO("=== Undo across a shift ===");
    using fake::Cmd;
    clearWorld(w);
    for (int withRebase = 1; withRebase >= 0; --withRebase) {
        const scene::Entity e = spawn(w, Vec3{1100000.0f, 400.0f, 90.0f});
        w.flush();
        // The gesture, recorded in the scene frame before the shift: dragged from x = 1,100,000 to 1,100,250.
        std::vector<Cmd> undo, redo;
        Cmd c; c.kind = Cmd::Kind::Transform;
        c.before.pos = Vec3{1100000.0f, 400.0f, 90.0f};
        c.after.pos = Vec3{1100250.0f, 400.0f, 90.0f};
        undo.push_back(c);
        w.setLocalTransform(e, xfAt(1100250.0f, 400.0f, 90.0f));
        w.flush();
        const f64 absBefore = 1100000.0, absAfter = 1100250.0;

        game::OriginParticipants op;
        Vec3 cam{1100000.0f, 400.0f, 120.0f};
        op.bindings().positions = {&cam};
        editor::EditorOriginParticipant ed;
        if (withRebase) ed.addRebaser("undo", [&](const world::ShiftEvent& ev) { editor::originRebase::rebaseStacks(undo, redo, ev.deltaF, fake::kInvalid); });
        op.bindings().editorPrepare = [&](const world::ShiftEvent& ev) { return ed.prepare(ev); };
        op.bindings().editorCommit = [&](const world::ShiftEvent& ev) { ed.commit(ev); };
        op.configure(1, -1, true);
        op.install();

        check(op.update(cam) && op.origin() == world::OriginCm{kStepI, 0}, withRebase ? "the camera past 11 km shifts one region" : "(control) the same shift, no rebase");
        w.flush();
        check(std::fabs(absX(op, w, e) - absAfter) < 0.01, "the entity kept its absolute position through the shift");

        // Ctrl+Z: applyXformTo writes the command's world "before" into the root's local.
        w.setLocalTransform(e, xfAt(undo.back().before.pos.x, undo.back().before.pos.y, undo.back().before.pos.z));
        w.flush();
        const f64 err = std::fabs(absX(op, w, e) - absBefore);
        if (withRebase) check(err < 0.01, "undo puts it back at the SAME absolute position (error " + std::to_string(err) + " cm)");
        else check(err > 1.0e6, "(control) without the rebase undo lands a region away (error " + std::to_string(err) + " cm)");
        clearWorld(w);
        op.resetForLevel();
    }
}

// ---- The gesture gate --------------------------------------------------------------------------------------------------

static void testGestureGate(scene::World& w) {
    AVER_INFO("=== Gesture gate ===");
    clearWorld(w);
    const scene::Entity e = spawn(w, Vec3{950000.0f, 0.0f, 0.0f});
    w.flush();

    game::OriginParticipants op;
    Vec3 cam{950000.0f, 0.0f, 50.0f};   // past the window (921,600) but not past the urgent line (1,000,000)
    op.bindings().positions = {&cam};

    // A drag: the gesture holds the entity's pre-gesture local, and cancelling restores it.
    bool dragging = false;
    Vec3 dragBefore{950000.0f, 0.0f, 0.0f};
    editor::EditorOriginParticipant ed;
    int cancels = 0;
    std::vector<std::string> log;
    ed.addGesture("a transform edit is open", [&] { return dragging; }, [&] {
        ++cancels;
        w.setLocalTransform(e, xfAt(dragBefore.x, dragBefore.y, dragBefore.z));
        dragging = false;
    });
    bool marquee = false;
    ed.addGesture("a selection rectangle is being dragged", [&] { return marquee; }, [&] { marquee = false; ++cancels; });
    ed.addRebaser("first", [&](const world::ShiftEvent&) {
        log.push_back("first");
        log.push_back(w.localTransform(e).position.x < 400000.0f ? "scene-already-shifted" : "scene-NOT-shifted");   // order 80 follows the scene's 20
    });
    ed.addRebaser("second", [&](const world::ShiftEvent&) { log.push_back("second"); });
    op.bindings().editorPrepare = [&](const world::ShiftEvent& ev) { return ed.prepare(ev); };
    op.bindings().editorCommit = [&](const world::ShiftEvent& ev) { ed.commit(ev); };
    op.configure(1, -1, true);
    op.install();

    // 1. A gizmo drag is open: the shift is refused and NOTHING moves.
    dragging = true;
    w.setLocalTransform(e, xfAt(950300.0f, 0.0f, 0.0f));   // mid-drag
    w.flush();
    check(!op.update(cam), "a shift is refused while a drag is open");
    check(op.origin() == world::OriginCm{} && op.generation() == 0 && cam.x == 950000.0f && w.localTransform(e).position.x == 950300.0f,
          "origin, generation, camera and scene are untouched");
    check(op.service().stats().deferrals == 1 && op.service().stats().lastDeferReason &&
          std::string(op.service().stats().lastDeferReason) == "a transform edit is open", "the deferral and its reason are recorded");
    check(ed.refused() == 1 && std::string(ed.lastRefusal()) == "a transform edit is open" && cancels == 0 && log.empty(), "the gate refused, cancelled nothing, rebased nothing");

    // 2. The marquee is a gesture too; with the drag over, it still refuses.
    dragging = false;
    marquee = true;
    check(!op.update(cam) && std::string(op.service().stats().lastDeferReason) == "a selection rectangle is being dragged", "a marquee refuses the shift");

    // 3. Both gone: the retry goes through, the rebasers run after the scene, in order.
    marquee = false;
    check(op.update(cam) && op.origin() == world::OriginCm{kStepI, 0}, "the gesture over, the retry shifts one region");
    const std::vector<std::string> want = {"first", "scene-already-shifted", "second"};
    check(log == want, "the rebasers ran at commit, after the scene had shifted, in registration order");
    check(ed.commits() == 1 && ed.lastGeneration() == 1, "one commit, carrying the generation");

    // 4. Urgent: a camera 12 km out cannot wait. The drag is cancelled (pre-gesture transform restored in the old frame) and the shift commits.
    op.resetForLevel();
    log.clear();
    cancels = 0;
    w.setLocalTransform(e, xfAt(1250000.0f, 0.0f, 0.0f));
    dragBefore = Vec3{1200000.0f, 0.0f, 0.0f};
    dragging = true;
    cam = Vec3{1250000.0f, 0.0f, 50.0f};   // past 1,000,000 cm on a shifted axis: urgent
    w.flush();
    check(op.update(cam), "an urgent shift is not deferred by an open drag");
    w.flush();
    check(cancels == 1 && !dragging && ed.cancelled() == 1 && std::string(ed.lastCancelled()) == "a transform edit is open", "the drag was cancelled exactly once");
    check(std::fabs(absX(op, w, e) - 1200000.0) < 0.01, "and the entity is back at its pre-gesture ABSOLUTE position (1,200,000), not where the drag had taken it");
    check(op.origin() == world::OriginCm{kStepI, 0}, "the shift committed");
    check(ed.refused() == 2, "(it was not counted as a refusal: the two earlier ones stand)");
    clearWorld(w);
    op.resetForLevel();

    // 5. The overlay signature: equal inputs, equal outputs; a new generation, a new signature.
    const u64 base = 1469598103934665603ull;
    check(editor::EditorOriginParticipant::overlaySignature(base, 3) == editor::EditorOriginParticipant::overlaySignature(base, 3), "overlay signature: deterministic");
    check(editor::EditorOriginParticipant::overlaySignature(base, 3) != editor::EditorOriginParticipant::overlaySignature(base, 4), "overlay signature: changes with the origin generation");
    check(editor::EditorOriginParticipant::overlaySignature(base, 3) != editor::EditorOriginParticipant::overlaySignature(base + 1, 3), "overlay signature: still depends on the rest of the key");
}

// ---- The console -------------------------------------------------------------------------------------------------------

static std::vector<std::string> run(editor::WorldConsoleHost& h, const char* name, std::vector<std::string> args, bool* okOut = nullptr) {
    std::vector<std::string> lines;
    const bool ok = editor::runWorldCommand(h, name, args, lines);
    if (okOut) *okOut = ok;
    return lines;
}

static void testConsole(scene::World& w) {
    AVER_INFO("=== world.* console ===");
    clearWorld(w);
    const scene::Entity marker = spawn(w, Vec3{500.0f, 0.0f, 0.0f});
    w.flush();

    game::OriginParticipants op;
    Vec3 cam{100.0f, 200.0f, 300.0f};
    op.bindings().positions = {&cam};
    bool gesture = false;
    int cancels = 0;
    editor::EditorOriginParticipant ed;
    ed.addGesture("a sculpt stroke is open", [&] { return gesture; }, [&] { gesture = false; ++cancels; });
    op.bindings().editorPrepare = [&](const world::ShiftEvent& ev) { return ed.prepare(ev); };
    op.bindings().editorCommit = [&](const world::ShiftEvent& ev) { ed.commit(ev); };
    op.configure(1, -1, true);
    op.install();

    editor::WorldConsoleHost h;
    h.origin = &op;
    h.camPos = &cam;
    int teleports = 0;
    h.onTeleport = [&] { ++teleports; };
    bool ok = false;

    // world.origin
    std::vector<std::string> lines = run(h, "world.origin", {}, &ok);
    check(ok && contains(lines, "region (0, 0)") && contains(lines, "floating origin on") && contains(lines, "camera absolute"), "world.origin prints the region, the state and the camera");
    check(!run(h, "WORLD.ORIGIN", {}, &ok).empty() && ok, "the name is case-insensitive");

    // world.margin
    lines = run(h, "world.margin", {}, &ok);
    check(ok && contains(lines, "102400"), "world.margin alone prints the current margin (the default, 102400 cm)");
    lines = run(h, "world.margin", {"50000"}, &ok);
    check(ok && op.service().settings().marginCm == 50000, "world.margin 50000 sets it");
    lines = run(h, "world.margin", {"999999999"}, &ok);
    check(!ok && op.service().settings().marginCm == 50000 && contains(lines, "0 to 229376"), "a margin past the sub-millimetre cap is refused, and says the range");
    lines = run(h, "world.margin", {"abc"}, &ok);
    check(!ok && contains(lines, "usage"), "a non-number is a usage error");
    run(h, "world.margin", {"102400"}, &ok);

    // world.policy / world.enable
    lines = run(h, "world.policy", {"manual"}, &ok);
    check(ok && op.service().settings().policy == world::OriginPolicy::Manual, "world.policy manual");
    check(op.service().decide(Vec3{5000000.0f, 0.0f, 0.0f}).x == 0, "(manual: the camera alone never moves the origin)");
    run(h, "world.policy", {"fixed"}, &ok);
    lines = run(h, "world.rebase", {"1", "0"}, &ok);
    check(!ok && contains(lines, "fixed"), "world.rebase under a fixed policy is refused up front, saying why");
    lines = run(h, "world.policy", {"sideways"}, &ok);
    check(!ok && contains(lines, "usage"), "an unknown policy is a usage error");
    run(h, "world.policy", {"follow"}, &ok);
    run(h, "world.enable", {"0"}, &ok);
    check(ok && !op.service().enabled(), "world.enable 0");
    lines = run(h, "world.tp", {"50", "0", "0"}, &ok);
    check(!ok && contains(lines, "off"), "world.tp with the origin off is refused, saying why");
    run(h, "world.enable", {"1"}, &ok);
    check(ok && op.service().enabled(), "world.enable 1");
    lines = run(h, "world.enable", {"2"}, &ok);
    check(!ok && contains(lines, "usage"), "world.enable 2 is a usage error");

    // world.rebase: queued, runs at S0
    lines = run(h, "world.rebase", {"0", "0"}, &ok);
    check(!ok, "world.rebase 0 0 moves nothing, and says so");
    lines = run(h, "world.rebase", {"2", "-1"}, &ok);
    check(ok && h.pending.kind == editor::WorldRequest::Kind::Rebase && op.origin() == world::OriginCm{}, "world.rebase 2 -1 only QUEUES (the UI pass must not move the world)");
    check(contains(run(h, "world.origin", {}), "queued"), "and world.origin says a move is queued");
    std::string report = editor::applyWorldRequest(h, ok);
    check(ok && op.origin() == world::OriginCm{2 * kStepI, -kStepI} && !report.empty(), "at S0 it runs: the origin is (2, -1) regions");
    w.flush();
    check(std::fabs(absX(op, w, marker) - 500.0) < 0.01, "the scene kept its absolute positions");
    check(cam.x == 100.0f - 2.0f * kStep && cam.y == 200.0f + kStep && cam.z == 300.0f, "and the camera moved by exactly -delta");
    check(editor::applyWorldRequest(h, ok).empty() && ok, "a second call has nothing to do");

    // a refused move reports why and is dropped
    gesture = true;
    run(h, "world.rebase", {"1", "0"}, &ok);
    report = editor::applyWorldRequest(h, ok);
    check(!ok && report.find("a sculpt stroke is open") != std::string::npos, "world.rebase during a stroke is refused, naming the gesture: " + report);
    check(h.pending.kind == editor::WorldRequest::Kind::None && op.origin() == world::OriginCm{2 * kStepI, -kStepI}, "the request is dropped and nothing moved");
    gesture = false;

    // world.tp: the 50 km and 100 km checks
    lines = run(h, "world.tp", {"50", "0", "20"}, &ok);
    check(ok && h.pending.kind == editor::WorldRequest::Kind::Teleport && teleports == 0, "world.tp 50 0 20 queues");
    gesture = true;   // a teleport is a decision: an open gesture is ended, not waited for
    report = editor::applyWorldRequest(h, ok);
    check(ok && cancels == 1 && !gesture, "world.tp ends an open gesture and goes: " + report);
    {
        const world::Vec3d a = world::absOf(op.service().absolute(cam));
        check(std::fabs(a.x - 5000000.0) < 0.01 && std::fabs(a.y) < 0.01 && std::fabs(a.z - 2000000.0) < 0.01,
              "the camera is at the absolute (50 km, 0, 20 km): " + std::to_string(a.x) + " " + std::to_string(a.y) + " " + std::to_string(a.z));
        check(op.origin() == world::OriginCm{3 * kStepI, 0}, "the origin went to the nearest region corner: (3, 0)");
        check(std::fabs(cam.x) <= static_cast<f32>(world::kRegionHalfCm), "so the camera sits inside its window (local x = " + std::to_string(cam.x) + ")");
    }
    check(teleports == 1, "the camera-velocity reset hook ran once");
    run(h, "world.tp", {"100", "-30", "0.5"}, &ok);
    report = editor::applyWorldRequest(h, ok);
    {
        const world::Vec3d a = world::absOf(op.service().absolute(cam));
        check(ok && std::fabs(a.x - 10000000.0) < 0.05 && std::fabs(a.y + 3000000.0) < 0.05 && std::fabs(a.z - 50000.0) < 0.01,
              "world.tp 100 -30 0.5: the camera is at the absolute (100 km, -30 km, 0.5 km): " + std::to_string(a.x) + " " + std::to_string(a.y));
        check(op.origin() == world::OriginCm{6 * kStepI, -2 * kStepI}, "origin (6, -2) regions");
    }
    w.flush();
    check(std::fabs(absX(op, w, marker) - 500.0) < 0.01, "the scene entity is still at its absolute position after two teleports and 100 km");
    run(h, "world.tp", {"0", "0", "0"}, &ok);
    editor::applyWorldRequest(h, ok);
    check(ok && op.origin() == world::OriginCm{} && same(cam, Vec3{0.0f, 0.0f, 0.0f}), "world.tp 0 0 0 comes home: origin (0, 0), camera at zero");
    lines = run(h, "world.tp", {"1", "2"}, &ok);
    check(!ok && contains(lines, "usage"), "world.tp with two numbers is a usage error");
    lines = run(h, "world.tp", {"1e20", "0", "0"}, &ok);
    check(!ok, "and a distance past ten million km is refused");
    lines = run(h, "world.nonsense", {}, &ok);
    check(!ok, "an unknown world command fails");
    editor::WorldConsoleHost none;
    run(none, "world.origin", {}, &ok);
    check(!ok, "with no origin service the commands say so");
    clearWorld(w);
    op.resetForLevel();
}

// ---- The readout -------------------------------------------------------------------------------------------------------

static void testReadout(scene::World& w) {
    AVER_INFO("=== Show > World Origin readout ===");
    clearWorld(w);
    game::OriginParticipants op;
    Vec3 cam{900000.0f, -2500.0f, 80.0f};
    op.bindings().positions = {&cam};
    op.configure(1, -1, true);
    op.install();

    editor::WorldOriginReadout r = editor::makeWorldOriginReadout(op, cam);
    check(r.enabled && r.regionX == 0 && r.regionY == 0 && r.generation == 0 && r.shifts == 0, "before any shift: region (0, 0), generation 0, no shifts");
    check(std::fabs(r.camAbsKm[0] - 9.0) < 1e-9 && std::fabs(r.camAbsKm[1] + 0.025) < 1e-9 && std::fabs(r.camAbsKm[2] - 0.0008) < 1e-9, "the camera in km, in f64");
    const f64 limit = static_cast<f64>(world::kRegionHalfCm) + 102400.0;
    check(std::fabs(r.toTriggerCm - (limit - 900000.0)) < 1e-6 && r.toTriggerCm > 0.0, "the distance to the next shift is the window minus the farthest axis");
    check(r.nearTrigger && r.toTriggerCm <= editor::kOriginBandCm, "21.6 m from the trigger is inside the 3 km band");
    check(r.parts.size() == op.participantCount() && r.parts.size() == 10 && std::string(r.parts[0].name) == "gate", "one row per participant, in commit order");

    cam = Vec3{1100000.0f, 0.0f, 80.0f};
    r = editor::makeWorldOriginReadout(op, cam);
    check(r.toTriggerCm < 0.0 && r.nearTrigger, "past the window: the shift is due and the band shows");
    check(op.update(cam), "the shift happens");
    r = editor::makeWorldOriginReadout(op, cam);
    check(r.regionX == 1 && r.regionY == 0 && r.generation == 1 && r.shifts == 1 && r.lastMs >= 0.0, "afterwards: region (1, 0), generation 1, one shift, a duration");
    check(std::fabs(r.camAbsKm[0] - 11.0) < 1e-6, "and the camera's absolute position did not change (11 km)");
    check(std::fabs(r.originKmX - 16.384) < 1e-9, "the origin reads 16.384 km");
    const std::vector<std::string> text = editor::worldOriginReadoutLines(r);
    check(text.size() >= 5, "the text form has its rows");
    r.lastDeferReason = "a stroke";
    check(contains(editor::worldOriginReadoutLines(r), "a stroke"), "the last deferral reason shows");
    clearWorld(w);
    op.resetForLevel();
}

int main() {
    AVER_INFO("EditorOriginTest");
    scene::World& w = scene::World::instance();
    testRebase();
    testUndoEndToEnd(w);
    testGestureGate(w);
    testConsole(w);
    testReadout(w);
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
