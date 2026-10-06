// Prefab instances in a live world: spawn, overrides, revert, apply, automatic propagation (including
// through a nested prefab), undo state, and the level records. No GPU, no framework, no editor.
//
// The failure these exist to catch is the quiet one: an edit to a prefab that reaches every instance
// and in doing so wipes what a particular instance had changed. So every propagation test sets an
// override first and asserts the override's VALUE after, never a count.
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/prefab/PrefabSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using aver::prefab::PrefabLibrary;
using aver::prefab::PrefabSystem;
using scene::Entity;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void clearWorld(scene::World& w) {
    std::vector<Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

// ---- building prefab data by hand ---------------------------------------------------------------------

static fmt::OcSaveField fI32(const char* n, i64 v) { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindI32; f.i = v; return f; }
static fmt::OcSaveField fI64(const char* n, i64 v) { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindI64; f.i = v; return f; }
static fmt::OcSaveField fEnt(const char* n, i64 uid) { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindEntity; f.i = uid; return f; }
static fmt::OcSaveField fVec3(const char* n, f32 x, f32 y, f32 z) {
    fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindVec3; f.f = {x, y, z}; return f;
}
static fmt::OcSaveComponent comp(const char* type, std::vector<fmt::OcSaveField> fields) {
    fmt::OcSaveComponent c; c.type = type; c.fields = std::move(fields); return c;
}
static fmt::OcPrefabNode node(u32 uid, u32 parent, const char* name, std::vector<fmt::OcSaveComponent> comps = {}) {
    fmt::OcPrefabNode n; n.uid = uid; n.parent = parent; n.name = name; n.components = std::move(comps); return n;
}

// Crate: root (tags 5) with a Lid (mesh 123, tags 1, a joint to the Hinge) and a Hinge at (0,0,40).
static fmt::OcPrefabData crate() {
    fmt::OcPrefabData p;
    p.name = "Crate";
    p.nodes.push_back(node(1, 0, "Crate", {comp("CTags", {fI32("bits", 5)})}));
    p.nodes.push_back(node(2, 1, "Lid", {comp("CLocal", {fVec3("position", 0, 0, 50)}),
                                         comp("CMeshRenderer", {fI64("mesh", 123)}),
                                         comp("CTags", {fI32("bits", 1)}),
                                         comp("CJoint", {fEnt("otherEntity", 3)})}));
    p.nodes.push_back(node(3, 1, "Hinge", {comp("CLocal", {fVec3("position", 0, 0, 40)})}));
    p.nextUid = 4;
    return p;
}

// Inner: root plus a Part (tags 2). Outer: root plus a nested Inner placed at (10,0,0).
static fmt::OcPrefabData inner() {
    fmt::OcPrefabData p;
    p.name = "Inner";
    p.nodes.push_back(node(1, 0, "Inner"));
    p.nodes.push_back(node(2, 1, "Part", {comp("CTags", {fI32("bits", 2)}), comp("CMeshRenderer", {fI64("mesh", 7)})}));
    p.nextUid = 3;
    return p;
}

static fmt::OcPrefabData outer() {
    fmt::OcPrefabData p;
    p.name = "Outer";
    p.nodes.push_back(node(1, 0, "Outer", {comp("CTags", {fI32("bits", 100)})}));
    fmt::OcPrefabNode n = node(2, 1, "Nested");
    n.prefab = "Prefabs/Inner.ocprefab";
    fmt::OcPrefabOverride at; at.path = ""; at.component = "CLocal"; at.value = fVec3("position", 10, 0, 0);
    n.overrides.push_back(at);
    p.nodes.push_back(std::move(n));
    p.nextUid = 3;
    return p;
}

static i32 tagsOf(scene::World& w, Entity e) {
    const auto* t = w.component<scene::CTags>(e, scene::kComponentTags);
    return t ? static_cast<i32>(t->bits) : -1;
}
static void setTags(scene::World& w, Entity e, u32 v) {
    if (auto* t = w.component<scene::CTags>(e, scene::kComponentTags)) t->bits = v;
}
static u64 meshOf(scene::World& w, Entity e) {
    const auto* m = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    return m ? m->mesh : 0;
}
static Vec3 posOf(scene::World& w, Entity e) { return w.localTransform(e).position; }
static bool near3(const Vec3& a, f32 x, f32 y, f32 z) {
    return std::fabs(a.x - x) < 1e-3f && std::fabs(a.y - y) < 1e-3f && std::fabs(a.z - z) < 1e-3f;
}
static Transform at(f32 x, f32 y, f32 z) { Transform t; t.position = Vec3{x, y, z}; return t; }

int main() {
    AVER_INFO("PrefabSystemTest");
    scene::World& w = scene::World::instance();

    AVER_INFO("an instance is built from the prefab");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        std::string why;
        const Entity root = sys.instantiate("Prefabs/Crate.ocprefab", at(100, 200, 300), scene::kInvalidEntity, {}, "Crate 1", &why);
        check(root != scene::kInvalidEntity, "it spawns: " + why);
        check(w.count() == 3, "one entity per node");
        check(std::string(w.name(root)) == "Crate 1", "the root takes the instance's name");
        check(near3(posOf(w, root), 100, 200, 300), "the root sits where the instance was placed");
        check(tagsOf(w, root) == 5, "root components come from the prefab");
        const Entity lid = sys.entityAtPath(root, "2"), hinge = sys.entityAtPath(root, "3");
        check(lid != scene::kInvalidEntity && hinge != scene::kInvalidEntity, "children are found by node path");
        check(near3(posOf(w, lid), 0, 0, 50) && w.parent(lid) == root, "a child keeps its local position and parent");
        check(meshOf(w, lid) == 123 && tagsOf(w, lid) == 1, "child components");
        const auto* jt = w.component<scene::CJoint>(lid, scene::kComponentJoint);
        check(jt && jt->otherEntity == hinge, "an entity reference points at the sibling it names");
        check(sys.isInstanceRoot(root) && sys.instanceRoot(lid) == root && sys.isLinked(hinge), "links");
        std::vector<fmt::OcPrefabOverride> ov;
        check(sys.computeOverrides(root, ov) && ov.empty(), "a fresh instance has no overrides");
    }

    AVER_INFO("an edit made to an instance is an override, and reverting puts it back");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const Entity root = sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
        const Entity lid = sys.entityAtPath(root, "2");
        setTags(w, lid, 7);
        w.setLocalPosition(lid, Vec3{1, 2, 3});
        w.setName(lid, "Top");
        std::vector<fmt::OcPrefabOverride> ov;
        check(sys.computeOverrides(root, ov), "overrides compute");
        bool tags = false, pos = false, name = false;
        for (const auto& o : ov) {
            if (o.path == "2" && o.component == "CTags" && o.value.name == "bits" && o.value.i == 7) tags = true;
            if (o.path == "2" && o.component == "CLocal" && o.value.name == "position") pos = true;
            if (o.path == "2" && o.component == fmt::kOcPrefabNodeComponent && o.value.s == "Top") name = true;
        }
        check(tags && pos && name && ov.size() == 3, "exactly the three edits, nothing else");

        fmt::OcPrefabOverride which; which.path = "2"; which.component = "CTags"; which.value.name = "bits";
        check(sys.revertOverride(root, which), "reverting one override");
        check(tagsOf(w, lid) == 1 && near3(posOf(w, lid), 1, 2, 3), "only that field went back");
        check(sys.revertNode(lid), "reverting a node");
        check(near3(posOf(w, lid), 0, 0, 50) && std::string(w.name(lid)) == "Lid", "the rest of the node went back");
        setTags(w, root, 99);
        check(sys.revertAll(root) && tagsOf(w, root) == 5, "revert all");
    }

    AVER_INFO("an override survives an edit to the prefab, and the edit still arrives");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const Entity a = sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
        const Entity b = sys.instantiate("Prefabs/Crate.ocprefab", at(500, 0, 0));
        const Entity lidA = sys.entityAtPath(a, "2"), lidB = sys.entityAtPath(b, "2");
        setTags(w, lidA, 7);                                  // instance A's own change

        fmt::OcPrefabData edited = crate();
        edited.find(2)->components[1].fields[0].i = 456;      // Lid mesh 123 -> 456
        edited.find(1)->components[0].fields[0].i = 6;        // root tags 5 -> 6
        edited.nodes.push_back(node(4, 1, "Latch", {comp("CTags", {fI32("bits", 11)})}));
        edited.nextUid = 5;
        std::string why;
        check(sys.updatePrefab("Prefabs/Crate.ocprefab", edited, &why), "the edit propagates: " + why);

        check(meshOf(w, lidA) == 456 && meshOf(w, lidB) == 456, "the prefab's edit reaches both instances");
        check(tagsOf(w, lidA) == 7, "A keeps its override");
        check(tagsOf(w, lidB) == 1, "B, which had none, still follows the prefab");
        check(tagsOf(w, a) == 6 && tagsOf(w, b) == 6, "the root's new value arrives everywhere");
        check(sys.entityAtPath(a, "2") == lidA, "the entity handle survives the rebuild");
        const Entity latchA = sys.entityAtPath(a, "4");
        check(latchA != scene::kInvalidEntity && tagsOf(w, latchA) == 11 && w.parent(latchA) == a, "a new node gets an entity in every instance");
        check(sys.entityAtPath(b, "4") != scene::kInvalidEntity, "including B");

        // Removing a node drops its entity but keeps what a user parented under it.
        const Entity extra = w.create("Extra", lidB, Transform{});
        fmt::OcPrefabData trimmed = edited;
        trimmed.nodes.erase(trimmed.nodes.begin() + 1);   // drop Lid (uid 2)
        // The joint on Lid went with it; Hinge and Latch stay.
        check(sys.updatePrefab("Prefabs/Crate.ocprefab", trimmed), "a node is removed from the prefab");
        w.flush();
        check(sys.entityAtPath(b, "2") == scene::kInvalidEntity, "the removed node's entity is gone");
        check(w.valid(extra) && w.parent(extra) == b, "an entity a user added under it survives, moved to the instance root");
    }

    AVER_INFO("apply to prefab folds an instance's overrides into the asset");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const Entity a = sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
        const Entity b = sys.instantiate("Prefabs/Crate.ocprefab", at(500, 0, 0));
        setTags(w, sys.entityAtPath(a, "2"), 7);
        w.setLocalPosition(sys.entityAtPath(a, "3"), Vec3{0, 0, 60});
        std::string why;
        check(sys.applyAll(a, &why), "apply all: " + why);
        const auto* d = lib.peek("Prefabs/Crate.ocprefab");
        check(d && d->find(2)->components[2].fields[0].i == 7, "the asset now says tags 7");
        check(tagsOf(w, sys.entityAtPath(b, "2")) == 7, "the other instance follows");
        check(near3(posOf(w, sys.entityAtPath(b, "3")), 0, 0, 60), "so does the moved node");
        std::vector<fmt::OcPrefabOverride> ov;
        check(sys.computeOverrides(a, ov) && ov.empty(), "the instance it was applied from has nothing left to override");
        check(d && d->valid(), "and the asset is still valid");

        // One override at a time.
        setTags(w, sys.entityAtPath(a, "2"), 8);
        setTags(w, a, 9);
        fmt::OcPrefabOverride one; one.path = ""; one.component = "CTags"; one.value.name = "bits"; one.value.kind = fmt::kOcPrefabKindI32; one.value.i = 9;
        check(sys.applyOverride(a, one), "apply a single override");
        check(tagsOf(w, b) == 9 && tagsOf(w, sys.entityAtPath(b, "2")) == 7, "only that one reached the asset");
        check(tagsOf(w, sys.entityAtPath(a, "2")) == 8, "the other is still A's override");
    }

    AVER_INFO("components added or removed on an instance are overrides too");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const Entity a = sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
        const Entity hinge = sys.entityAtPath(a, "3"), lid = sys.entityAtPath(a, "2");
        if (auto* t = static_cast<scene::CTags*>(w.addComponent(hinge, scene::kComponentTags))) t->bits = 4;
        w.removeComponent(lid, scene::kComponentTags);
        std::vector<fmt::OcPrefabOverride> ov;
        sys.computeOverrides(a, ov);
        bool added = false, removed = false, value = false;
        for (const auto& o : ov) {
            if (o.op == fmt::OcOverrideOp::AddComponent && o.path == "3" && o.component == "CTags") added = true;
            if (o.op == fmt::OcOverrideOp::RemoveComponent && o.path == "2" && o.component == "CTags") removed = true;
            if (o.op == fmt::OcOverrideOp::Set && o.path == "3" && o.component == "CTags" && o.value.i == 4) value = true;
        }
        check(added && removed && value, "add, remove and the added component's value");

        // They survive a prefab edit.
        fmt::OcPrefabData edited = crate();
        edited.find(2)->components[1].fields[0].i = 999;
        sys.updatePrefab("Prefabs/Crate.ocprefab", edited);
        check(w.hasComponent(hinge, scene::kComponentTags) && tagsOf(w, hinge) == 4, "the added component is still there with its value");
        check(!w.hasComponent(lid, scene::kComponentTags) && meshOf(w, lid) == 999, "the removed one is still removed, and the edit arrived");
        sys.revertAll(a);
        check(!w.hasComponent(hinge, scene::kComponentTags) && w.hasComponent(lid, scene::kComponentTags), "revert undoes both");
    }

    AVER_INFO("nested prefabs: an edit to the inner asset reaches the outer instance, overrides kept");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Inner.ocprefab", inner());
        lib.set("Prefabs/Outer.ocprefab", outer());
        PrefabSystem sys(w, lib);
        const Entity o = sys.instantiate("Prefabs/Outer.ocprefab", at(0, 0, 0));
        check(w.count() == 3, "outer root, the nested root and its part");
        const Entity nested = sys.entityAtPath(o, "2"), part = sys.entityAtPath(o, "2/2");
        check(nested != scene::kInvalidEntity && part != scene::kInvalidEntity, "nested nodes have chained paths");
        check(near3(posOf(w, nested), 10, 0, 0), "the outer prefab's override placed the nested root");
        check(w.parent(part) == nested && w.parent(nested) == o, "the hierarchy follows the nesting");
        check(std::string(w.name(nested)) == "Nested", "the outer node's name wins over the inner root's");
        check(tagsOf(w, part) == 2 && meshOf(w, part) == 7, "inner components arrive");

        setTags(w, part, 9);                                    // an override, two levels down
        std::vector<fmt::OcPrefabOverride> ov;
        sys.computeOverrides(o, ov);
        check(ov.size() == 1 && ov[0].path == "2/2" && ov[0].value.i == 9, "it is addressed by its chained path");

        fmt::OcPrefabData in2 = inner();
        in2.find(2)->components[1].fields[0].i = 77;            // Part mesh 7 -> 77
        in2.nodes.push_back(node(3, 1, "Extra", {comp("CTags", {fI32("bits", 3)})}));
        in2.nextUid = 4;
        check(sys.updatePrefab("Prefabs/Inner.ocprefab", in2), "the INNER asset is edited");
        check(meshOf(w, part) == 77, "the edit reaches the outer instance");
        check(tagsOf(w, part) == 9, "the override on the nested part survives");
        check(near3(posOf(w, nested), 10, 0, 0), "the placement from the outer prefab survives");
        const Entity extra = sys.entityAtPath(o, "2/3");
        check(extra != scene::kInvalidEntity && w.parent(extra) == nested && tagsOf(w, extra) == 3, "a node added to the inner asset appears under the nested root");
        check(w.count() == 4, "no entity was duplicated");

        // The outer asset edited while the inner is nested.
        fmt::OcPrefabData out2 = outer();
        out2.find(1)->components[0].fields[0].i = 101;
        check(sys.updatePrefab("Prefabs/Outer.ocprefab", out2) && tagsOf(w, o) == 101 && tagsOf(w, part) == 9, "an outer edit leaves nested overrides alone");

        // Apply from a nested part folds into the nested node, not the inner asset.
        std::string why;
        check(sys.applyAll(o, &why), "apply from the nested level: " + why);
        const auto* od = lib.peek("Prefabs/Outer.ocprefab");
        bool folded = false;
        if (od) for (const auto& oo : od->find(2)->overrides) if (oo.path == "2" && oo.component == "CTags" && oo.value.i == 9) folded = true;
        check(folded, "it became an override on the nested node of the OUTER prefab");
        check(lib.peek("Prefabs/Inner.ocprefab")->find(2)->components[0].fields[0].i == 2, "the inner asset is untouched");
        std::vector<fmt::OcPrefabOverride> after;
        sys.computeOverrides(o, after);
        check(after.empty() && tagsOf(w, part) == 9, "and the instance has nothing left over");
    }

    AVER_INFO("a missing nested asset is a placeholder, a cycle is refused");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Outer.ocprefab", outer());   // Inner is NOT registered
        PrefabSystem sys(w, lib);
        const Entity o = sys.instantiate("Prefabs/Outer.ocprefab", at(0, 0, 0));
        check(o != scene::kInvalidEntity && w.count() == 2, "the instance still builds, with a placeholder for the nested node");
        check(near3(posOf(w, sys.entityAtPath(o, "2")), 10, 0, 0), "and the outer prefab's override still places it");

        fmt::OcPrefabData loop;
        loop.name = "Loop";
        loop.nodes.push_back(node(1, 0, "Loop"));
        fmt::OcPrefabNode n = node(2, 1, "Self");
        n.prefab = "Prefabs/Loop.ocprefab";
        loop.nodes.push_back(n);
        loop.nextUid = 3;
        lib.set("Prefabs/Loop.ocprefab", loop);
        std::string why;
        check(sys.instantiate("Prefabs/Loop.ocprefab", at(0, 0, 0), scene::kInvalidEntity, {}, {}, &why) == scene::kInvalidEntity && !why.empty(),
              "a prefab that contains itself does not spawn: " + why);
    }

    AVER_INFO("undo state: capture, edit, restore either side");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const Entity a = sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
        const Entity b = sys.instantiate("Prefabs/Crate.ocprefab", at(500, 0, 0));
        const u32 idA = sys.instanceIdOf(a), idB = sys.instanceIdOf(b);
        check(idA != 0 && idB != 0 && idA != idB, "instances have distinct stable ids");
        setTags(w, sys.entityAtPath(a, "2"), 7);

        const prefab::PrefabState before = sys.captureState({"Prefabs/Crate.ocprefab"}, {idA}, {});
        check(sys.applyAll(a), "apply");
        const prefab::PrefabState after = sys.captureState({"Prefabs/Crate.ocprefab"}, {idA}, {});
        check(tagsOf(w, sys.entityAtPath(b, "2")) == 7, "b follows the applied edit");

        check(sys.restoreState(before), "undo");
        check(lib.peek("Prefabs/Crate.ocprefab")->find(2)->components[2].fields[0].i == 1, "the asset is back");
        check(tagsOf(w, sys.entityAtPath(b, "2")) == 1, "b is back too, though the state never named it");
        check(tagsOf(w, sys.entityAtPath(a, "2")) == 7, "a has its override again");
        std::vector<fmt::OcPrefabOverride> ov;
        sys.computeOverrides(a, ov);
        check(ov.size() == 1, "as exactly one override");

        check(sys.restoreState(after), "redo");
        check(tagsOf(w, sys.entityAtPath(b, "2")) == 7 && tagsOf(w, sys.entityAtPath(a, "2")) == 7, "both follow the asset");
        sys.computeOverrides(a, ov);
        check(ov.empty(), "and a has no override");

        // Deleting an instance and undoing it brings back the same id and its overrides.
        setTags(w, sys.entityAtPath(b, "2"), 21);
        const prefab::PrefabState withB = sys.captureState({}, {idB}, {});
        check(sys.destroyInstance(b), "destroy an instance");
        w.flush();
        check(sys.rootOfInstance(idB) == scene::kInvalidEntity, "it is gone");
        check(sys.restoreState(withB), "undo the delete");
        const Entity b2 = sys.rootOfInstance(idB);
        check(b2 != scene::kInvalidEntity && tagsOf(w, sys.entityAtPath(b2, "2")) == 21, "it is back with its override");
        check(near3(posOf(w, b2), 500, 0, 0), "at the same place");
    }

    AVER_INFO("a plain subtree becomes a prefab and an instance, and that can be undone");
    {
        clearWorld(w);
        PrefabLibrary lib;
        PrefabSystem sys(w, lib);
        const Entity root = w.create("Chair", scene::kInvalidEntity, at(40, 50, 60));
        const Entity leg = w.create("Leg", root, at(1, 2, 3));
        if (auto* t = static_cast<scene::CTags*>(w.addComponent(leg, scene::kComponentTags))) t->bits = 3;
        if (auto* m = static_cast<scene::CMeshRenderer*>(w.addComponent(leg, scene::kComponentMeshRenderer))) m->mesh = 0xABCDEF0123456789ull;
        w.flush();

        fmt::OcPrefabData data;
        std::string why;
        check(sys.captureAsPrefab(root, "Chair", data, &why), "capture: " + why);
        check(data.nodes.size() == 2 && data.nodes[0].parent == 0 && data.nodes[1].parent == data.nodes[0].uid, "two nodes, parents first");
        check(data.valid(), "valid");
        const auto* rl = &data.nodes[0].components;
        bool rootAtOrigin = false;
        for (const auto& c : *rl) if (c.type == "CLocal") for (const auto& f : c.fields) if (f.name == "position") rootAtOrigin = f.f[0] == 0 && f.f[1] == 0 && f.f[2] == 0;
        check(rootAtOrigin, "the prefab's root is normalised to the origin");

        lib.set("Prefabs/Chair.ocprefab", data);
        const u32 group = sys.registerPlain(root);
        const prefab::PrefabState before = sys.captureState({}, {}, {group});
        const Entity inst = sys.convertToInstance(root, "Prefabs/Chair.ocprefab", &why);
        check(inst != scene::kInvalidEntity, "convert: " + why);
        w.flush();
        check(!w.valid(root) && !w.valid(leg), "the originals are gone");
        check(near3(posOf(w, inst), 40, 50, 60) && std::string(w.name(inst)) == "Chair", "the instance sits where the original did");
        const Entity leg2 = sys.entityAtPath(inst, std::to_string(data.nodes[1].uid));
        check(leg2 != scene::kInvalidEntity && tagsOf(w, leg2) == 3 && meshOf(w, leg2) == 0xABCDEF0123456789ull &&
              near3(posOf(w, leg2), 1, 2, 3), "its child kept everything");
        std::vector<fmt::OcPrefabOverride> ov;
        check(sys.computeOverrides(inst, ov) && ov.empty(), "no overrides after converting");
        const u32 instId = sys.instanceIdOf(inst);
        const prefab::PrefabState after = sys.captureState({}, {instId}, {group});

        prefab::PrefabState undo = before;
        undo.instances.push_back(prefab::InstanceSnapshot{instId, false, {}, {}, {}, scene::kInvalidEntity, {}});
        check(sys.restoreState(undo), "undo the conversion");
        w.flush();
        check(sys.rootOfInstance(instId) == scene::kInvalidEntity, "the instance is gone");
        const Entity chair = w.find("Chair");
        check(chair != scene::kInvalidEntity && !sys.isLinked(chair) && near3(posOf(w, chair), 40, 50, 60), "a plain Chair is back");
        check(w.childCount(chair) == 1, "with its leg");
        (void)after;
    }

    AVER_INFO("level records: instances survive a save and a reload");
    {
        clearWorld(w);
        fmt::OcWorldData level;
        {
            PrefabLibrary lib;
            lib.set("Prefabs/Crate.ocprefab", crate());
            PrefabSystem sys(w, lib);
            Transform xf = at(120, -40, 8);
            xf.scale = Vec3{2, 2, 2};
            const Entity a = sys.instantiate("Prefabs/Crate.ocprefab", xf, scene::kInvalidEntity, {}, "Crate 7");
            setTags(w, sys.entityAtPath(a, "2"), 7);
            sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
            level.name = "L";
            level.prefabInstances = sys.captureLevelInstances();
            check(level.prefabInstances.size() == 2, "two records captured");
        }
        const std::string text = fmt::writeOcworld(level);
        clearWorld(w);
        fmt::OcWorldData loaded;
        std::string why;
        check(fmt::parseOcworld(text, loaded, &why), "the level parses: " + why);

        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const std::vector<Entity> roots = sys.instantiateLevelInstances(loaded.prefabInstances);
        check(roots.size() == 2 && w.count() == 6, "both come back, three entities each");
        Entity named = scene::kInvalidEntity;
        for (const Entity r : roots) if (std::string(w.name(r)) == "Crate 7") named = r;
        check(named != scene::kInvalidEntity, "the instance's own name survives");
        if (named != scene::kInvalidEntity) {
            check(near3(posOf(w, named), 120, -40, 8) && std::fabs(w.localTransform(named).scale.x - 2.0f) < 1e-4f, "its transform");
            check(tagsOf(w, sys.entityAtPath(named, "2")) == 7, "its override");
        }
        // A prefab missing at load time skips the record rather than failing the level.
        PrefabLibrary empty;
        PrefabSystem sys2(w, empty);
        check(sys2.instantiateLevelInstances(loaded.prefabInstances).empty(), "a missing asset skips its instances");
    }

    AVER_INFO("unlink leaves ordinary entities");
    {
        clearWorld(w);
        PrefabLibrary lib;
        lib.set("Prefabs/Crate.ocprefab", crate());
        PrefabSystem sys(w, lib);
        const Entity a = sys.instantiate("Prefabs/Crate.ocprefab", at(0, 0, 0));
        const Entity lid = sys.entityAtPath(a, "2");
        check(sys.unlink(a) && !sys.isLinked(a) && !sys.isLinked(lid), "links are gone");
        fmt::OcPrefabData edited = crate();
        edited.find(2)->components[1].fields[0].i = 1;
        sys.updatePrefab("Prefabs/Crate.ocprefab", edited);
        check(meshOf(w, lid) == 123, "and later edits to the prefab no longer reach it");
    }

    clearWorld(w);
    if (g_failures) { AVER_ERROR("PrefabSystemTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("PrefabSystemTest: all passed");
    return 0;
}
