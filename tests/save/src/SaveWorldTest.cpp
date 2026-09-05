// Capturing a live world and putting it back.
//
// The failure this exists to catch is the one that makes a save system worthless without looking
// broken: it loads, the world is populated, and something is quietly in the wrong place -- a child
// reparented to the wrong entity, a reference pointing at whoever happened to land at that index, a
// negative number come back as four billion. So every assertion here is about a VALUE surviving,
// never about a count.
#include "aver/save/SaveWorld.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.4f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.5f}, want {:.5f})", what, got, want);
}

// Destroys every entity, so each block starts from a world it fully owns.
static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

int main() {
    AVER_INFO("SaveWorldTest");
    scene::World& w = scene::World::instance();

    AVER_INFO("a world of plain entities round-trips");
    {
        clearWorld(w);

        // A parent, its child, and a second root the child REFERS to. The reference points FORWARD
        // in capture order, which is the case a single-pass restore gets wrong.
        Transform pxf; pxf.position = Vec3{100, 200, 300};
        const scene::Entity parent = w.create("Parent", scene::kInvalidEntity, pxf);
        Transform cxf; cxf.position = Vec3{10, 0, 0}; cxf.scale = Vec3{2, 3, 4};
        const scene::Entity child = w.create("Child", parent, cxf);
        const scene::Entity other = w.create("Other");

        auto* tg = static_cast<scene::CTags*>(w.addComponent(child, scene::kComponentTags));
        check(tg != nullptr, "the child takes a tags component");
        if (tg) tg->bits = 0x5u;

        auto* mr = static_cast<scene::CMeshRenderer*>(
            w.addComponent(child, scene::kComponentMeshRenderer));
        check(mr != nullptr, "and a mesh renderer");
        if (mr) {
            mr->mesh = 0xABCDEF0123456789ull;   // an I64 with the top half set
            mr->flags = scene::kMeshRendererVisible;
            mr->aabbMin[0] = -1.5f; mr->aabbMax[2] = 9.25f;
            mr->dirty = 0;
        }

        // A CAnimator, for a NEGATIVE float and a clip id -- and because it is the component whose
        // "zero means playing" contract makes a silently-zeroed restore look like working code.
        auto* an = static_cast<scene::CAnimator*>(w.addComponent(other, scene::kComponentAnimator));
        if (an) { an->clip = 0x1111ull; an->time = 2.5f; an->speed = -1.5f; an->flags = 0x2u; }

        fmt::OcSaveData snap;
        std::string why;
        check(save::capture(w, snap, {}, &why), "the world captures: " + why);
        check(snap.entities.size() == 3, "all three entities are in the snapshot");

        // PARENTS BEFORE CHILDREN is the invariant everything else rests on, so it is asserted
        // directly rather than inferred from a successful restore.
        bool ordered = true;
        for (usize i = 0; i < snap.entities.size(); ++i)
            if (snap.entities[i].parent >= static_cast<i32>(i)) ordered = false;
        check(ordered, "every parent sits at a LOWER index than its child");

        std::vector<u8> bytes;
        check(fmt::writeOcSave(snap, bytes, &why), "the snapshot writes to bytes: " + why);
        fmt::OcSaveData reloaded;
        check(fmt::parseOcSave(bytes.data(), bytes.size(), reloaded, &why), "and parses: " + why);

        // THROUGH THE FILE, not from the in-memory snapshot -- restoring what capture produced
        // would not prove the format carried it.
        clearWorld(w);
        check(w.count() == 0, "the world is empty before the restore");
        check(save::restore(reloaded, w, {}, &why), "it restores: " + why);
        check(w.count() == 3, "and the world has three entities again");

        const scene::Entity rParent = w.find("Parent");
        const scene::Entity rChild  = w.find("Child");
        const scene::Entity rOther  = w.find("Other");
        check(rParent != scene::kInvalidEntity && rChild != scene::kInvalidEntity &&
              rOther != scene::kInvalidEntity, "all three are findable by name");

        // The handles are NEW. If restore had somehow preserved them the test below would pass for
        // the wrong reason, so assert they changed.
        check(rParent != parent || rChild != child, "the restored entities are NEW handles");

        if (rChild != scene::kInvalidEntity) {
            const auto* h = w.component<scene::CHierarchy>(rChild, scene::kComponentHierarchy);
            check(h && h->parent == rParent,
                  "the child is parented to the RESTORED parent, not to a stale handle");

            const auto* loc = w.component<scene::CLocal>(rChild, scene::kComponentLocal);
            check(loc != nullptr, "the child has a local transform");
            if (loc) {
                checkNear(loc->xf.position.x, 10.0f, 1e-4f, "its position survived");
                checkNear(loc->xf.scale.y, 3.0f, 1e-4f, "and its NON-UNIFORM scale, per axis");
            }

            const auto* t2 = w.component<scene::CTags>(rChild, scene::kComponentTags);
            check(t2 && t2->bits == 0x5u, "its tag bits survived");

            const auto* m2 = w.component<scene::CMeshRenderer>(rChild, scene::kComponentMeshRenderer);
            check(m2 != nullptr, "its mesh renderer came back");
            if (m2) {
                check(m2->mesh == 0xABCDEF0123456789ull,
                      "the mesh id survived ALL 64 BITS -- a u64 truncated to 32 would still look "
                      "like a plausible asset id");
                check((m2->flags & scene::kMeshRendererVisible) != 0, "its visible flag survived");
                checkNear(m2->aabbMax[2], 9.25f, 1e-5f, "and its bounds");
                check(m2->dirty == 1,
                      "AND ITS DIRTY BIT IS SET -- without it the GPU never re-uploads and a "
                      "perfectly restored mesh is invisible");
            }
        }

        if (rOther != scene::kInvalidEntity) {
            const auto* a2 = w.component<scene::CAnimator>(rOther, scene::kComponentAnimator);
            check(a2 != nullptr, "the animator came back");
            if (a2) {
                checkNear(a2->speed, -1.5f, 1e-5f,
                          "a NEGATIVE float survived -- sign, not magnitude");
                checkNear(a2->time, 2.5f, 1e-5f, "and the clock");
                check(a2->clip == 0x1111ull, "and the clip id");
            }
        }

        const auto* pl = w.component<scene::CLocal>(rParent, scene::kComponentLocal);
        if (pl) checkNear(pl->xf.position.z, 300.0f, 1e-3f, "the root's world position survived");
    }

    AVER_INFO("a cross-entity reference is remapped, not carried");
    {
        clearWorld(w);
        // CAttachment has no ENTITY field, and no built-in component does -- so this uses the one
        // ENTITY-shaped thing that exists on every entity: the hierarchy. The forward-reference
        // case is covered by the format test; what matters here is that a restore does not leave
        // a stale handle anywhere it can be seen.
        const scene::Entity a = w.create("A");
        const scene::Entity b = w.create("B", a, Transform{});
        (void)b;

        fmt::OcSaveData snap;
        std::string why;
        check(save::capture(w, snap, {}, &why), "captures: " + why);

        clearWorld(w);
        // Burn a handle so the restored entities cannot land on the same ones by coincidence.
        const scene::Entity burn = w.create("Burn");
        w.destroy(burn);
        w.flush();

        check(save::restore(snap, w, {}, &why), "restores: " + why);
        const scene::Entity ra = w.find("A");
        const scene::Entity rb = w.find("B");
        const auto* h = w.component<scene::CHierarchy>(rb, scene::kComponentHierarchy);
        check(h && h->parent == ra, "B's parent is the restored A");
        check(h && h->parent != a,
              "and NOT the handle A used to have -- which would silently point at nothing");
    }

    AVER_INFO("skip leaves an entity alone on both sides");
    {
        clearWorld(w);
        const scene::Entity keep = w.create("Streamed");
        const scene::Entity save1 = w.create("Saved");
        (void)save1;

        // Stands in for a host's "does any ChunkWorld own this" predicate. Streamed content has its
        // own persistence, and capturing it as well would restore two copies of every tree.
        struct Ctx { scene::Entity keep; } ctx{keep};
        auto skip = [](scene::Entity e, void* u) { return e == static_cast<Ctx*>(u)->keep; };

        save::CaptureOptions co;
        co.skip = skip;
        co.host.user = &ctx;
        fmt::OcSaveData snap;
        std::string why;
        check(save::capture(w, snap, co, &why), "captures with a skip: " + why);
        check(snap.entities.size() == 1, "the skipped entity is NOT in the snapshot");

        save::RestoreOptions ro;
        ro.skip = skip;
        ro.host.user = &ctx;
        check(save::restore(snap, w, ro, &why), "restores with the same skip: " + why);
        check(w.valid(keep),
              "AND THE SKIPPED ENTITY IS STILL ALIVE -- restore must not destroy what capture "
              "deliberately never took, or a load deletes the streamed world");
        check(w.count() == 2, "with exactly the saved one beside it");
    }

    AVER_INFO("an unknown component is dropped, not fatal");
    {
        clearWorld(w);
        fmt::OcSaveData snap;
        fmt::OcSaveEntity en;
        en.name = "FromTheFuture";
        en.parent = -1;
        fmt::OcSaveComponent sc;
        sc.type = "CSomethingThisBuildNeverHeardOf";
        sc.fields.push_back({"x", AVER_SCENE_KIND_F32, {1.0f}, 0, ""});
        en.components.push_back(sc);
        snap.entities.push_back(en);

        std::string why;
        check(save::restore(snap, w, {}, &why),
              "a save naming a component this build does not have still LOADS: " + why);
        check(w.find("FromTheFuture") != scene::kInvalidEntity,
              "and the entity is there, just without that component");
    }

    AVER_INFO("a spawner that refuses part-way leaves the world EMPTY, as the header promises");
    {
        clearWorld(w);

        // THE SCENARIO IS AN OUT-OF-DATE SAVE, NOT A CORRUPT ONE. A save records each actor's class
        // by NAME, and the host's spawnClass returns kInvalidEntity for a name this build no longer
        // has -- a class renamed or deleted since the save was written. That refusal reaches
        // restore()'s create-loop, which fails the whole load. SaveWorld.hpp promises the world is
        // left EMPTY when that happens, "because a half-restored world is worse than an empty one:
        // it looks playable". It was not: every entity created before the refusal stayed.
        //
        // Three entities with the bad class SECOND, deliberately. First would have made the bug
        // invisible (nothing had been created yet to leak) and last would have made it maximal; the
        // middle is the case that distinguishes "cleans up" from "cleans up only sometimes".
        fmt::OcSaveData snap;
        for (const char* nm : {"Good1", "GoneClass", "Good2"}) {
            fmt::OcSaveEntity en;
            en.name = nm;
            en.parent = -1;
            en.className = nm;
            snap.entities.push_back(en);
        }

        // Refuses exactly one name, spawns anything else as a plain entity. Stands in for
        // saveSpawnClass's `if (aver_fw_class_find(className) == 0) return kInvalidEntity;`.
        struct Ctx { scene::World* w; } ctx{&w};
        save::RestoreOptions ro;
        ro.host.user = &ctx;
        ro.host.spawnClass = [](const char* cn, void* u) -> scene::Entity {
            if (cn && std::strcmp(cn, "GoneClass") == 0) return scene::kInvalidEntity;
            return static_cast<Ctx*>(u)->w->create(cn ? cn : "", scene::kInvalidEntity, Transform{});
        };

        std::string why;
        check(!save::restore(snap, w, ro, &why),
              "restore FAILS when the spawner refuses a class this build no longer has");
        w.flush();
        check(w.count() == 0,
              "AND THE WORLD IS EMPTY -- the entity created before the refusal is torn down, not "
              "left behind for the caller to trip over");
        check(w.find("Good1") == scene::kInvalidEntity,
              "specifically: the one that HAD been created is gone");
    }

    clearWorld(w);
    AVER_INFO(g_failures ? "SaveWorldTest: {} FAILURES" : "SaveWorldTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
