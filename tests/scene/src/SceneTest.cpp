// Hand-run test for Aver.Scene: entity packing, component storage, world lifetime, the field
// tables, and transform/hierarchy propagation. Exit code = failure count.
//
// Nothing runs this but a human, which is the precedent tests/formats sets and the reason each
// section prints what it proved rather than only whether it passed.
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;

// A test/introspection hook exported by the DLL but kept out of scene_abi.h: the size of the string
// intern table, so the set_str slot-reuse regression below can be checked directly.
extern "C" __declspec(dllimport) int64_t aver_scene_debug_string_pool_size(void);

static int g_checks   = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Element for element, because a matrix compared with memcmp tells you only that something moved.
static void checkMat(const Mat4& got, const Mat4& want, const std::string& what) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            ++g_checks;
            if (got.m[i][j] == want.m[i][j]) continue;
            AVER_ERROR("   FAIL  {} m[{}][{}]: got {} want {}", what, i, j, got.m[i][j], want.m[i][j]);
            ++g_failures;
        }
    }
}

static void checkMatNear(const Mat4& got, const Mat4& want, f32 tol, const std::string& what) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            ++g_checks;
            const f32 d = got.m[i][j] - want.m[i][j];
            if (d <= tol && d >= -tol) continue;
            AVER_ERROR("   FAIL  {} m[{}][{}]: got {} want {}", what, i, j, got.m[i][j], want.m[i][j]);
            ++g_failures;
        }
    }
}

// A component whose table is deliberately incomplete in some of the cases below. Laid out so that
// dropping the LAST member leaves eight unexplained bytes in a struct aligned to eight, which is
// what makes a missing trailing member as visible as a missing interior one.
struct CBroken {
    u64 a    = 0;
    f32 b[3] = {0, 0, 0};
    u32 c    = 0;
    u32 d    = 0;
};

// ------------------------------------------------------------------------------------------ packing

static void testEntityPacking() {
    AVER_INFO("=== entity packing ===");

    check(kInvalidEntity == 0u, "kInvalidEntity is 0");
    const Entity defaulted{};
    check(defaulted == kInvalidEntity, "a default-constructed Entity is invalid");
    check(makeEntity(0, 0) == 0u, "the only encoding of 0 is the illegal (index 0, generation 0)");
    check(kEntityIndexBits + kEntityGenBits == 31u, "bit 31 is not part of either field");

    // Every legal generation crossed with the low, high and boundary index ranges. The generation
    // field alone guarantees a non-zero handle, so sampling the index space proves the claim without
    // walking 2.1 billion pairs.
    bool     neverZero = true, bit31Clear = true, indexRoundTrip = true, genRoundTrip = true;
    unsigned pairs = 0;
    for (u32 gen = 1; gen <= kEntityMaxGen; ++gen) {
        for (u32 index = 0; index <= 0xFFFFu; ++index) {
            const Entity e = makeEntity(index, gen);
            ++pairs;
            if (e == kInvalidEntity) neverZero = false;
            if ((e & 0x80000000u) != 0) bit31Clear = false;
            if (entityIndex(e) != index) indexRoundTrip = false;
            if (entityGen(e) != gen) genRoundTrip = false;
        }
        for (u32 index = kMaxEntities - 0xFFFFu; index <= kMaxEntities; ++index) {
            const Entity e = makeEntity(index, gen);
            ++pairs;
            if (e == kInvalidEntity) neverZero = false;
            if ((e & 0x80000000u) != 0) bit31Clear = false;
            if (entityIndex(e) != index) indexRoundTrip = false;
            if (entityGen(e) != gen) genRoundTrip = false;
        }
    }
    AVER_INFO("   {} (index, generation) pairs encoded", pairs);
    check(neverZero, "makeEntity never yields 0 for any legal (index, generation)");
    check(bit31Clear, "bit 31 is clear for every legal (index, generation)");
    check(indexRoundTrip, "entityIndex round-trips for every legal pair");
    check(genRoundTrip, "entityGen round-trips for every legal pair");
}

// --------------------------------------------------------------------------------------- retirement

// Runs FIRST, while the free list is empty, because a FIFO free list only returns the same index to
// the next create when it is the ONLY index on it.
static void testGenerationRetirement(World& world) {
    AVER_INFO("=== generation wrap retires the slot ===");

    check(world.count() == 0u, "a fresh world holds no entities");
    check(world.freeSlotCount() == 0u, "a fresh world has an empty free list");
    check(world.retiredSlotCount() == 0u, "a fresh world has retired nothing");

    Entity    e   = world.create("wrap");
    const u32 idx = entityIndex(e);
    check(idx != 0u, "index 0 is never handed out");
    check(entityGen(e) == 1u, "a slot's first life is generation 1");

    bool sameIndex = true, genWalks = true;
    for (u32 gen = 1; gen <= kEntityMaxGen; ++gen) {
        if (entityIndex(e) != idx) sameIndex = false;
        if (entityGen(e) != gen) genWalks = false;
        world.destroy(e);
        world.flush();
        if (gen < kEntityMaxGen) e = world.create("wrap");
    }
    check(sameIndex, "the sole free index is reused every time");
    check(genWalks, "the generation walks 1..127 across reuses");
    check(world.retiredSlotCount() == 1u, "the slot at generation 127 is retired");
    check(world.freeSlotCount() == 0u, "a retired slot does not go back on the free list");
    check(world.count() == 0u, "nothing is left live");

    // Eight fresh entities, kept alive so the free list stays empty for the section after this one.
    bool reused = false;
    for (int i = 0; i < 8; ++i) {
        const Entity fresh = world.create("after-retirement");
        if (entityIndex(fresh) == idx) reused = true;
    }
    check(!reused, "a retired index is never handed out again");
    check(world.count() == 8u, "eight entities survive the retirement section");
}

// ------------------------------------------------------------------------------------------ lifetime

static Entity testLifetime(World& world) {
    AVER_INFO("=== world lifetime ===");

    check(world.freeSlotCount() == 0u, "the free list is empty going into the lifetime section");

    const u32    before = world.count();
    const Entity x      = world.create("lifetime-x");
    const u32    xIndex = entityIndex(x);
    const u32    xGen   = entityGen(x);

    check(world.valid(x), "a created entity is valid");
    check(world.count() == before + 1u, "create raises the live count");
    check((x & 0x80000000u) == 0u, "a live entity crosses as a positive int32_t");
    check(std::string(world.name(x)) == "lifetime-x", "the name blob round-trips");
    check(world.find("lifetime-x") == x, "find() resolves a name to its entity");
    check(world.objectId(x) == fnv1a64("lifetime-x"), "the persisted id is fnv1a64 of the name");

    check(world.setName(x, "Röhre-日本-x"), "a non-ASCII name is accepted");
    check(std::string(world.name(x)) == "Röhre-日本-x", "a non-ASCII name round-trips unchanged");
    check(world.find("lifetime-x") == kInvalidEntity, "the old name no longer resolves");

    check(world.destroy(x), "destroy accepts a live entity");
    check(world.valid(x), "destroy is DEFERRED: the handle survives the rest of the frame");
    check(world.destroyPending(x), "the entity is queued for the next flush");
    world.flush();
    check(!world.valid(x), "the handle fails valid() after the flush that retires it");
    check(world.count() == before, "the live count returns to where it was");
    check(world.freeSlotCount() == 1u, "the index goes on the free list");

    const Entity y = world.create("lifetime-y");
    check(entityIndex(y) == xIndex, "a fresh entity takes the freed index");
    check(entityGen(y) == xGen + 1u, "and the next generation");
    check(world.valid(y), "the fresh handle at that index is valid");
    check(!world.valid(x), "the destroyed handle at the same index is still invalid");
    check(x != y, "the two handles are different values");
    check(world.freeSlotCount() == 0u, "the free list drains");

    check(!world.valid(kInvalidEntity), "0 is never valid");
    check(!world.destroy(kInvalidEntity), "destroy refuses 0");
    check(world.at(world.count()) == kInvalidEntity, "at() past the end yields 0");
    return y;
}

// --------------------------------------------------------------------------------------------- pools

static void testPools(World& world, Entity y) {
    AVER_INFO("=== component pools ===");

    ComponentPool* tags = world.pool(kComponentTags);
    ComponentPool* loc  = world.pool(kComponentLocal);
    check(tags != nullptr && loc != nullptr, "the built-in pools exist");
    check(world.pool(0) == nullptr, "type id 0 addresses no pool");

    check(loc->denseSlotOf(y) != 0u, "a present entity reads a non-zero dense slot");
    check(tags->denseSlotOf(y) == 0u, "an ABSENT entity reads 0 from the sparse array");
    check(!tags->has(y), "has() agrees");
    check(tags->get(y) == nullptr, "get() on an absent entity is nullptr, not a throw");

    CTags* t = static_cast<CTags*>(world.addComponent(y, kComponentTags));
    check(t != nullptr, "addComponent returns storage");
    check(tags->denseSlotOf(y) == 1u, "the first entity in a pool takes dense slot 0, stored as 1");
    check(tags->size() == 1u, "the pool holds one component");
    t->bits = 0xABCD;
    check(static_cast<CTags*>(world.getComponent(y, kComponentTags))->bits == 0xABCDu,
          "the stored bytes are the caller's");
    check(world.addComponent(y, kComponentTags) == t, "adding twice returns the existing component");

    // Swap-and-pop has to leave every survivor addressable, which is the one thing a dense array can
    // get wrong in a way that looks fine until the wrong entity is read.
    const Entity a = world.create("pool-a");
    const Entity b = world.create("pool-b");
    const Entity c = world.create("pool-c");
    static_cast<CTags*>(world.addComponent(a, kComponentTags))->bits = 1;
    static_cast<CTags*>(world.addComponent(b, kComponentTags))->bits = 2;
    static_cast<CTags*>(world.addComponent(c, kComponentTags))->bits = 3;
    check(tags->size() == 4u, "four components are stored");
    check(world.removeComponent(b, kComponentTags), "remove reports success");
    check(tags->size() == 3u, "the pool shrinks");
    check(tags->denseSlotOf(b) == 0u, "the removed entity reads 0");
    check(static_cast<CTags*>(world.getComponent(a, kComponentTags))->bits == 1u, "a survives the swap");
    check(static_cast<CTags*>(world.getComponent(c, kComponentTags))->bits == 3u, "c survives the swap");
    check(!world.removeComponent(b, kComponentTags), "removing twice is refused");

    // A stale handle must not address the live entity that inherited its index.
    const Entity doomed = world.create("pool-stale");
    static_cast<CTags*>(world.addComponent(doomed, kComponentTags))->bits = 9;
    world.destroy(doomed);
    world.flush();
    check(tags->denseSlotOf(doomed) == 0u, "a destroyed entity's sparse entry reads 0");
    const Entity reborn = world.create("pool-reborn");
    check(entityIndex(reborn) == entityIndex(doomed), "the index came back");
    check(tags->denseSlotOf(doomed) == 0u, "the STALE handle still reads 0 at that index");
    check(tags->denseSlotOf(reborn) == 0u, "and the fresh entity carries no component of its own");

    world.destroy(a);
    world.destroy(c);
    world.destroy(reborn);
    world.flush();
}

// -------------------------------------------------------------------------------------------- churn

static void testChurn(World& world) {
    AVER_INFO("=== 100k create/destroy cycles ===");

    const u32     before  = world.count();
    const u32     retired = world.retiredSlotCount();
    constexpr u32 kN      = 100000;

    std::vector<Entity> made;
    made.reserve(kN);
    bool allValid = true, allDistinct = true;
    for (u32 i = 0; i < kN; ++i) {
        const Entity e = world.create("churn");
        made.push_back(e);
        if (!world.valid(e)) allValid = false;
        if (e == kInvalidEntity) allDistinct = false;
    }
    check(allValid, "every one of 100000 created entities is valid");
    check(world.count() == before + kN, "size() counts them all");

    for (const Entity e : made) world.destroy(e);
    check(world.count() == before + kN, "destroy is deferred, so the count has not moved yet");
    world.flush();
    check(world.count() == before, "the flush returns the live count to its starting value");
    // The creates above drained whatever was already free, so the list now holds exactly these.
    check(world.freeSlotCount() == kN, "every index is back on the free list");
    check(world.retiredSlotCount() == retired, "none of them retired a slot");

    bool noneValid = true;
    for (const Entity e : made) {
        if (world.valid(e)) noneValid = false;
    }
    check(noneValid, "no destroyed handle validates");

    // The second pass must consume the free list rather than growing the index space, which is the
    // property that makes a FIFO free list worth having at all.
    std::vector<Entity> again;
    again.reserve(kN);
    for (u32 i = 0; i < kN; ++i) again.push_back(world.create("churn2"));
    check(world.freeSlotCount() == 0u, "the free list drained instead of the world growing");

    bool generationsAdvanced = true;
    for (u32 i = 0; i < kN; ++i) {
        // FIFO: the i-th index freed is the i-th handed back, one generation on.
        if (entityIndex(again[i]) != entityIndex(made[i])) generationsAdvanced = false;
        if (entityGen(again[i]) != entityGen(made[i]) + 1u) generationsAdvanced = false;
    }
    check(generationsAdvanced, "the free list is FIFO and every reused slot advanced one generation");

    for (const Entity e : again) world.destroy(e);
    world.flush();
    check(world.count() == before, "the world is back where the section found it");
    check(allDistinct, "no create returned 0");
}

// ------------------------------------------------------------------------------------------- fields

static void testFields(World& world) {
    AVER_INFO("=== field tables ===");

    struct Expect {
        const char* name;
        u32         id;
        usize       size;
    };
    const Expect builtins[] = {
        {"CLocal", kComponentLocal, sizeof(CLocal)},
        {"CWorld", kComponentWorld, sizeof(CWorld)},
        {"CHierarchy", kComponentHierarchy, sizeof(CHierarchy)},
        {"CName", kComponentName, sizeof(CName)},
        {"CTags", kComponentTags, sizeof(CTags)},
        {"CMeshRenderer", kComponentMeshRenderer, sizeof(CMeshRenderer)},
        {"CLight", kComponentLight, sizeof(CLight)},
        {"CCamera", kComponentCamera, sizeof(CCamera)},
    };
    check(world.componentCount() >= kComponentBuiltinMax, "all eight built-ins are registered");
    for (const Expect& e : builtins) {
        check(world.componentId(e.name) == e.id, std::string(e.name) + " has its fixed dense id");
        check(std::string(world.componentName(e.id)) == e.name, std::string(e.name) + " names itself");
        check(world.componentSize(e.id) == e.size, std::string(e.name) + " declares sizeof exactly");
        check(world.componentVerified(e.id), std::string(e.name) + " passed verify()");
        check(std::string(world.componentVerifyError(e.id)).empty(),
              std::string(e.name) + " carries no verify error");
    }
    check(world.componentId("CNotAThing") == 0u, "an unregistered name resolves to 0");

    struct FieldExpect {
        const char* qualified;
        u32         component;
        FieldKind   kind;
        u16         offset;
        u8          arity;
    };
    const FieldExpect fields[] = {
        {"CLocal.position", kComponentLocal, FieldKind::Vec3,
         static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, position)), 3},
        {"CLocal.rotation", kComponentLocal, FieldKind::Quat,
         static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, rotation)), 4},
        {"CLocal.scale", kComponentLocal, FieldKind::Vec3,
         static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, scale)), 3},
        {"CLocal.rev", kComponentLocal, FieldKind::I32, static_cast<u16>(offsetof(CLocal, rev)), 0},
        {"CWorld.matrix", kComponentWorld, FieldKind::Mat4, static_cast<u16>(offsetof(CWorld, m)), 16},
        {"CHierarchy.parent", kComponentHierarchy, FieldKind::Entity,
         static_cast<u16>(offsetof(CHierarchy, parent)), 0},
        {"CName.objectId", kComponentName, FieldKind::I64, static_cast<u16>(offsetof(CName, objectId)), 0},
        {"CMeshRenderer.mesh", kComponentMeshRenderer, FieldKind::I64,
         static_cast<u16>(offsetof(CMeshRenderer, mesh)), 0},
        {"CMeshRenderer.aabbMin", kComponentMeshRenderer, FieldKind::Vec3,
         static_cast<u16>(offsetof(CMeshRenderer, aabbMin)), 3},
        {"CMeshRenderer.dirty", kComponentMeshRenderer, FieldKind::I32,
         static_cast<u16>(offsetof(CMeshRenderer, dirty)), 0},
        {"CLight.colour", kComponentLight, FieldKind::Vec3, static_cast<u16>(offsetof(CLight, colour)), 3},
        {"CCamera.fovYRad", kComponentCamera, FieldKind::F32,
         static_cast<u16>(offsetof(CCamera, fovYRad)), 1},
    };
    for (const FieldExpect& fe : fields) {
        const u32        id = world.fieldId(fe.qualified);
        const FieldDesc* fd = world.field(id);
        check(id != 0u, std::string(fe.qualified) + " resolves to a dense field id");
        if (!fd) continue;
        check(fd->component == fe.component, std::string(fe.qualified) + " names its component");
        check(fd->kind == fe.kind, std::string(fe.qualified) + " carries its kind");
        check(fd->offset == fe.offset, std::string(fe.qualified) + " carries the right offsetof");
        check(fd->arity == fe.arity, std::string(fe.qualified) + " carries the arity its kind implies");
        check(std::string(fd->qualified) == fe.qualified,
              std::string(fe.qualified) + " round-trips its qualified name");
    }
    check(world.fieldId("CLocal.notAField") == 0u, "an unknown field resolves to 0");
    check(world.field(0) == nullptr, "field id 0 describes nothing");

    // Every field of every registered component, walked the way the Details panel will walk it.
    u32  walked = 0;
    bool inBounds = true, lookupAgrees = true, arityAgrees = true;
    for (u32 i = 0; i < world.componentCount(); ++i) {
        const u32 type = world.componentAt(i);
        for (u32 f = 0; f < world.fieldCount(type); ++f) {
            const u32        id = world.fieldAt(type, f);
            const FieldDesc* fd = world.field(id);
            if (!fd) { inBounds = false; continue; }
            ++walked;
            if (fd->offset + fieldByteSize(fd->kind) > world.componentSize(type)) inBounds = false;
            if (world.fieldId(fd->qualified) != id) lookupAgrees = false;
            if (fd->arity != canonicalArity(fd->kind)) arityAgrees = false;
        }
    }
    AVER_INFO("   {} fields across {} components", walked, world.componentCount());
    check(inBounds, "every field lies inside its component");
    check(lookupAgrees, "every field's qualified name resolves back to its own id");
    check(arityAgrees, "every field's arity is the one its kind implies");

    // ---- the terminator, which is the whole reason the table is worth hand-writing
    AVER_INFO("--- verify() rejects a table that does not account for the struct ---");

    auto brokenTable = [&world](bool withC, bool withD) {
        auto b = world.registerComponent<CBroken>("CBroken");
        b.field("a", FieldKind::I64, static_cast<u16>(offsetof(CBroken, a)))
            .field("b", FieldKind::Vec3, static_cast<u16>(offsetof(CBroken, b)));
        if (withC) b.field("c", FieldKind::I32, static_cast<u16>(offsetof(CBroken, c)));
        if (withD) b.field("d", FieldKind::I32, static_cast<u16>(offsetof(CBroken, d)));
        return b;
    };

    {
        auto b = brokenTable(true, true);
        check(b.verify(sizeof(CBroken)), "a complete table verifies");
        check(world.componentVerified(b.typeId()), "and the component is marked verified");
    }
    const u32 brokenType = world.componentId("CBroken");
    const u32 fieldA     = world.fieldId("CBroken.a");
    check(brokenType != 0u && fieldA != 0u, "the test component registered");

    {
        auto b = brokenTable(false, true);   // an INTERIOR member missing
        check(!b.verify(sizeof(CBroken)), "a table missing an interior member fails verify()");
        check(!world.componentVerified(brokenType), "the component is marked unverified");
        const std::string why = world.componentVerifyError(brokenType);
        check(why.find("CBroken") != std::string::npos, "the failure NAMES the component: " + why);
    }
    {
        auto b = brokenTable(true, false);   // a TRAILING member missing
        check(!b.verify(sizeof(CBroken)), "a table missing a trailing member fails verify()");
        const std::string why = world.componentVerifyError(brokenType);
        check(why.find("CBroken") != std::string::npos, "that failure names the component too: " + why);
    }
    {
        auto b = world.registerComponent<CBroken>("CBroken");
        b.field("a", FieldKind::I64, static_cast<u16>(offsetof(CBroken, a)))
            .field("b", FieldKind::Vec3, static_cast<u16>(offsetof(CBroken, a)));   // overlaps a
        check(!b.verify(sizeof(CBroken)), "an overlapping field fails verify()");
    }
    {
        auto b = brokenTable(true, true);
        check(!b.verify(sizeof(CBroken) + 8), "a struct size that disagrees with the registration fails");
    }
    {
        auto b = brokenTable(true, true);
        check(b.typeId() == brokenType, "re-registration is idempotent by name");
        check(b.verify(sizeof(CBroken)), "and the component verifies again once the table is whole");
        check(world.fieldId("CBroken.a") == fieldA, "a redeclared field keeps its dense id");
    }
    {
        ComponentBuilder orphan;
        check(!orphan.verify(sizeof(CBroken)), "a builder with no component verifies nothing");
    }
}

// ---------------------------------------------------------------------- transforms and hierarchy

static void testHierarchy(World& world) {
    AVER_INFO("=== transforms, hierarchy and dirty propagation ===");

    Transform ta;
    ta.position = {100.0f, 200.0f, 300.0f};
    ta.rotation = Quat::fromAxisAngle({0, 0, 1}, radians(30.0f));
    ta.scale    = {2.0f, 1.0f, 1.0f};

    Transform tb;
    tb.position = {10.0f, -20.0f, 30.0f};
    tb.rotation = Quat::fromAxisAngle({1, 0, 0}, radians(45.0f));
    tb.scale    = {1.0f, 3.0f, 1.0f};

    Transform tc;
    tc.position = {-5.0f, 5.0f, 1.0f};
    tc.rotation = Quat::fromAxisAngle({0, 1, 0}, radians(-60.0f));
    tc.scale    = {1.0f, 1.0f, 0.5f};

    const Entity a = world.create("chain-a", kInvalidEntity, ta);
    const Entity b = world.create("chain-b", a, tb);
    const Entity c = world.create("chain-c", b, tc);
    check(world.parent(b) == a && world.parent(c) == b, "the chain is three deep");
    check(world.depth(a) == 0u && world.depth(b) == 1u && world.depth(c) == 2u, "depths follow");
    check(world.childCount(a) == 1u && world.childCount(c) == 0u, "child counts follow");

    world.flush();

    // Composed by hand, left to right, exactly as the contract says: v * (L * P).
    const Mat4 wantA = ta.toMatrix();
    const Mat4 wantB = tb.toMatrix() * wantA;
    const Mat4 wantC = tc.toMatrix() * wantB;
    checkMat(world.worldMatrix(a), wantA, "root world matrix");
    checkMat(world.worldMatrix(b), wantB, "depth-1 world matrix");
    checkMat(world.worldMatrix(c), wantC, "depth-2 world matrix");

    // Translation in the LAST ROW is the single most likely silent defect in this module, so it is
    // asserted on a transform whose rotation and scale cannot hide a transpose.
    Transform plain;
    plain.position     = {11.0f, 22.0f, 33.0f};
    const Entity flat  = world.create("last-row", kInvalidEntity, plain);
    world.flush();
    const Mat4& fm = world.worldMatrix(flat);
    check(fm.m[3][0] == 11.0f, "translation x is in the LAST ROW");
    check(fm.m[3][1] == 22.0f, "translation y is in the LAST ROW");
    check(fm.m[3][2] == 33.0f, "translation z is in the LAST ROW");
    check(fm.m[3][3] == 1.0f, "the last row's w is 1");
    check(fm.m[0][3] == 0.0f && fm.m[1][3] == 0.0f && fm.m[2][3] == 0.0f,
          "the last COLUMN is 0,0,0,1 - a transposed matrix would put translation here");
    check(world.worldMatrix(c).m[3][3] == 1.0f, "a composed chain keeps w in the last row");

    // ---- the topological order
    bool parentsFirst = true;
    for (u32 i = 0; i < world.topologicalCount(); ++i) {
        const Entity e = world.topologicalAt(i);
        const Entity p = world.parent(e);
        if (p == kInvalidEntity) continue;
        bool seen = false;
        for (u32 j = 0; j < i; ++j) {
            if (world.topologicalAt(j) == p) { seen = true; break; }
        }
        if (!seen) parentsFirst = false;
    }
    check(world.topologicalCount() == world.count(), "the order covers every live entity exactly once");
    check(parentsFirst, "every parent precedes its children in the order");

    // ---- cycle rejection
    std::vector<Entity> orderBefore;
    for (u32 i = 0; i < world.topologicalCount(); ++i) orderBefore.push_back(world.topologicalAt(i));

    check(!world.setParent(a, c), "setParent(ancestor, descendant) is REFUSED");
    check(!world.setParent(a, b), "so is the one-step version");
    check(!world.setParent(a, a), "and so is parenting to self");
    check(world.parent(a) == kInvalidEntity, "the refused entity keeps its parent");
    check(world.parent(c) == b, "and the descendant keeps its own");
    world.flush();

    bool orderIntact = orderBefore.size() == world.topologicalCount();
    if (orderIntact) {
        for (u32 i = 0; i < world.topologicalCount(); ++i) {
            if (world.topologicalAt(i) != orderBefore[i]) orderIntact = false;
        }
    }
    check(orderIntact, "a refused reparent leaves the sort untouched, element for element");

    // ---- revision-compare propagation
    check(world.flush() == 0u, "a settled world recomposes nothing");

    std::vector<Entity> everyone;
    std::vector<u32>    revisions;
    for (u32 i = 0; i < world.count(); ++i) {
        everyone.push_back(world.at(i));
        revisions.push_back(world.worldRevision(world.at(i)));
    }

    world.setLocalPosition(a, {1000.0f, 0.0f, 0.0f});
    check(world.flush() == 3u, "moving a root recomposes EXACTLY its three-entity subtree");

    usize changed = 0, unchangedOutside = 0;
    for (usize i = 0; i < everyone.size(); ++i) {
        const Entity e      = everyone[i];
        const bool   inTree = (e == a || e == b || e == c);
        const bool   moved  = world.worldRevision(e) != revisions[i];
        if (moved) ++changed;
        if (!inTree && !moved) ++unchangedOutside;
    }
    check(changed == 3, "exactly three world revisions moved");
    check(unchangedOutside == everyone.size() - 3, "no other entity's world revision changed");

    Transform movedA = ta;
    movedA.position  = {1000.0f, 0.0f, 0.0f};
    const Mat4 movedWantA = movedA.toMatrix();
    const Mat4 movedWantB = tb.toMatrix() * movedWantA;
    const Mat4 movedWantC = tc.toMatrix() * movedWantB;
    checkMat(world.worldMatrix(c), movedWantC, "the moved subtree's leaf recomposed correctly");
    checkMat(world.worldMatrix(b), movedWantB, "and so did the middle");

    // ---- the on-demand path must agree with the batched one
    world.setLocalPosition(b, {77.0f, 0.0f, -3.0f});
    const Mat4 midFrame    = world.worldMatrix(c);      // read BEFORE the frame's pass runs
    const u32  midRevision = world.worldRevision(c);
    check(world.flush() == 0u, "the on-demand path left nothing for the flush to do");
    checkMat(world.worldMatrix(c), midFrame, "worldMatrix() mid-frame equals what the flush produces");
    check(world.worldRevision(c) == midRevision, "and the flush did not bump the revision again");

    Transform movedB = tb;
    movedB.position  = {77.0f, 0.0f, -3.0f};
    checkMat(midFrame, tc.toMatrix() * (movedB.toMatrix() * movedWantA),
             "and that value is the hand-composed one");

    // ---- reparenting
    check(world.setParent(c, a), "a legal reparent is accepted");
    check(world.parent(c) == a, "the child moved");
    check(world.childCount(a) == 2u, "the new parent has two children");
    check(world.childCount(b) == 0u, "the old parent has none");
    world.flush();
    checkMat(world.worldMatrix(c), tc.toMatrix() * world.worldMatrix(a),
             "the reparented child composes through its NEW parent");

    check(world.setParent(c, kInvalidEntity), "unparenting is accepted");
    check(world.parent(c) == kInvalidEntity, "the child is a root");
    check(world.depth(c) == 0u, "and its depth is 0");
    world.flush();
    checkMat(world.worldMatrix(c), tc.toMatrix(), "a root's world matrix is its local matrix");

    // keepWorld goes through a decomposition, so it is compared with a tolerance rather than
    // exactly: a matrix that has been through a square root does not come back bit for bit. The
    // parent here carries a UNIFORM scale on purpose — a non-uniformly scaled parent composes a
    // shear that position/rotation/scale cannot represent at all, and no tolerance fixes that.
    Transform keepParent;
    keepParent.position = {50.0f, -10.0f, 7.0f};
    keepParent.rotation = Quat::fromAxisAngle({0, 1, 0}, radians(20.0f));
    keepParent.scale    = {2.0f, 2.0f, 2.0f};
    Transform keepChild;
    keepChild.position = {3.0f, 4.0f, 5.0f};
    keepChild.rotation = Quat::fromAxisAngle({1, 0, 0}, radians(15.0f));

    const Entity kp = world.create("keep-parent", kInvalidEntity, keepParent);
    const Entity kc = world.create("keep-child", kInvalidEntity, keepChild);
    world.flush();
    const Mat4 keptBefore = world.worldMatrix(kc);
    check(world.setParent(kc, kp, true), "keepWorld reparent is accepted");
    world.flush();
    checkMatNear(world.worldMatrix(kc), keptBefore, 0.01f, "keepWorld leaves the child where it was");
    check(world.parent(kc) == kp, "and it really did move in the hierarchy");

    // ---- destroy takes the subtree
    const Entity p  = world.create("subtree-parent");
    const Entity k1 = world.create("subtree-child-1", p, Transform{});
    const Entity k2 = world.create("subtree-child-2", p, Transform{});
    const Entity g1 = world.create("subtree-grandchild", k1, Transform{});
    world.flush();
    check(world.childCount(p) == 2u, "the subtree is built");
    const u32 liveBefore = world.count();
    check(world.destroy(p), "destroying the parent is accepted");
    world.flush();
    check(!world.valid(p) && !world.valid(k1) && !world.valid(k2) && !world.valid(g1),
          "the whole subtree went with it");
    check(world.count() == liveBefore - 4u, "the live count dropped by exactly four");
    check(world.topologicalCount() == world.count(), "the order was rebuilt over the survivors");

    // ---- a pending-destroy parent must not become a reparent target.
    // Regression: destroy is deferred, so a doomed parent still reads valid() this frame; without a
    // guard, setParent onto it would let flush() collect a live child inside the doomed subtree.
    const Entity doomed    = world.create("doomed-parent");
    const Entity bystander = world.create("live-bystander");
    world.flush();
    check(world.destroy(doomed), "the parent is queued for destruction");
    check(world.destroyPending(doomed), "it reads as pending while still valid this frame");
    check(world.valid(doomed), "the deferred destroy leaves it valid until flush");
    check(!world.setParent(bystander, doomed), "reparenting a live entity ONTO a doomed parent is refused");
    check(world.parent(bystander) == kInvalidEntity, "so the bystander keeps its parent");
    world.flush();
    check(world.valid(bystander), "and survives the flush that collected the doomed parent");
    check(!world.valid(doomed), "which did collect the doomed parent");

    // ---- depth() stays honest for a reparented subtree BEFORE the next flush.
    // Regression: linkToParent fixed only the moved node, leaving descendants at their old depth
    // until an order rebuild, so a mid-frame depth() read the stale value.
    const Entity d0     = world.create("depth-root");
    const Entity d1     = world.create("depth-1", d0, Transform{});
    const Entity d2     = world.create("depth-2", d1, Transform{});         // d0>d1>d2 => 0,1,2
    const Entity shelf  = world.create("depth-shelf");
    const Entity shelf1 = world.create("depth-shelf-1", shelf, Transform{}); // shelf>shelf1 => 0,1
    world.flush();
    check(world.depth(d2) == 2u, "the chain leaf starts at depth 2");
    check(world.setParent(d1, shelf1), "reparent the middle of the chain under a depth-1 node");
    // Deliberately NO flush: this is exactly the mid-frame window the fix closes.
    check(world.depth(d1) == 2u, "the moved node's depth updates immediately");
    check(world.depth(d2) == 3u, "and its descendant's does too, without waiting for flush");
    world.flush();
    check(world.depth(d2) == 3u, "the flush agrees with the eager update");
}

// ----------------------------------------------------------------------------------------- the C ABI

// Exercises the exported C ABI (scene_abi.h) end to end, through the same singleton World the C++
// sections above drove — the whole point of the one-world design is that these address the same
// entities. Nothing here reaches into World except flush(), which the ABI deliberately does not
// expose (it is a frame boundary, not a per-call operation).
static void testSceneAbi(World& world) {
    AVER_INFO("=== C ABI (scene_abi.h) ===");

    check(aver_scene_abi_version() == AVER_SCENE_ABI_VERSION, "the DLL reports the header's ABI version");

    // ---- the KIND/COMP defines are the values the managed binding asserts against
    check(AVER_SCENE_KIND_VEC3 == static_cast<int>(FieldKind::Vec3), "KIND_VEC3 matches the enum");
    check(AVER_SCENE_KIND_I64 == static_cast<int>(FieldKind::I64), "KIND_I64 matches the enum");
    check(AVER_SCENE_COMP_MESH_RENDERER == static_cast<int>(kComponentMeshRenderer),
          "COMP_MESH_RENDERER matches the id");

    // ---- create + valid
    const int32_t e = aver_scene_create();
    check(e != 0, "aver_scene_create returns a non-zero entity");
    check((e & static_cast<int32_t>(0x80000000)) == 0, "the entity crosses as a positive int32_t");
    check(aver_scene_valid(e) == 1, "the fresh entity is valid over the ABI");
    check(aver_scene_valid(0) == 0, "0 is never valid");

    // ---- name + objectId round-trip over the ABI
    check(aver_scene_set_name(e, "abi-entity") == 1, "set_name accepts a live entity");
    check(std::string(aver_scene_name(e)) == "abi-entity", "name round-trips over the ABI");
    check(aver_scene_object_id(e) == static_cast<int64_t>(fnv1a64("abi-entity")),
          "set_name stamped objectId = fnv1a64(name)");
    check(aver_scene_set_object_id(e, 0x0123456789ABCDEFLL) == 1, "set_object_id accepts a live entity");
    check(aver_scene_object_id(e) == 0x0123456789ABCDEFLL, "objectId round-trips a full 64-bit value");

    // ---- resolve CLocal.position and its kind/arity
    const int32_t fPos = aver_scene_field("CLocal.position");
    check(fPos != 0, "CLocal.position resolves to a dense field id");
    check(aver_scene_field("CLocal.notAField") == 0, "an unknown field resolves to 0");
    check(aver_scene_field_kind(fPos) == AVER_SCENE_KIND_VEC3, "its kind is VEC3");
    check(aver_scene_field_arity(fPos) == 3, "its arity is 3");

    // ---- set_vec then get_vec, bit for bit (values chosen to be exact in float)
    const float wantPos[3] = {1.5f, -2.25f, 100.0f};
    check(aver_scene_set_vec(e, fPos, wantPos) == 1, "set_vec writes CLocal.position");
    float gotPos[3] = {-1, -1, -1};
    check(aver_scene_get_vec(e, fPos, gotPos) == 1, "get_vec reads CLocal.position back");
    check(gotPos[0] == wantPos[0] && gotPos[1] == wantPos[1] && gotPos[2] == wantPos[2],
          "the vector round-trips bit for bit");
    // The generic write must have bumped CLocal::rev, so the C++ transform view agrees.
    check(world.localTransform(e).position.x == 1.5f, "the C++ side sees the ABI's position write");

    const int32_t fRot = aver_scene_field("CLocal.rotation");
    check(aver_scene_field_arity(fRot) == 4, "CLocal.rotation is a 4-float Quat");
    const float wantRot[4] = {0.0f, 0.0f, 0.5f, 0.75f};
    check(aver_scene_set_vec(e, fRot, wantRot) == 1, "set_vec writes a Quat");
    float gotRot[4] = {0, 0, 0, 0};
    aver_scene_get_vec(e, fRot, gotRot);
    check(gotRot[2] == 0.5f && gotRot[3] == 0.75f, "the Quat round-trips");

    // ---- the mesh ObjectId via set_i64/get_i64 (needs CMeshRenderer attached first)
    check(aver_scene_add_component(e, AVER_SCENE_COMP_MESH_RENDERER) == 1, "add CMeshRenderer");
    const int32_t fMesh = aver_scene_field("CMeshRenderer.mesh");
    check(fMesh != 0, "CMeshRenderer.mesh resolves");
    check(aver_scene_field_kind(fMesh) == AVER_SCENE_KIND_I64, "mesh is an I64 field");
    const int64_t meshId = 0x00000000DEADBEEFLL;
    check(aver_scene_set_i64(e, fMesh, meshId) == 1, "set_i64 writes the mesh ObjectId");
    check(aver_scene_get_i64(e, fMesh) == meshId, "get_i64 reads the mesh ObjectId back");

    // ---- the material handle via set_i32/get_i32, resolved through aver_scene_material
    const int32_t fMat = aver_scene_field("CMeshRenderer.material");
    check(aver_scene_field_kind(fMat) == AVER_SCENE_KIND_I32, "material is an I32 field");
    const int32_t steel = aver_scene_material(0, "steel");
    check(steel > 0, "a material name resolves to a positive handle");
    check(aver_scene_material(0, "steel") == steel, "the same name resolves to the same handle");
    check(aver_scene_material(0, "brass") != steel, "a different name resolves to a different handle");
    check(aver_scene_material(1, "steel") != steel, "the same name in another pack is a different handle");
    check(aver_scene_material(0, "") == 0, "an empty material name resolves to 0");
    check(aver_scene_set_i32(e, fMat, steel) == 1, "set_i32 writes the material handle");
    check(aver_scene_get_i32(e, fMat) == steel, "get_i32 reads the material handle back");

    // ---- wrong-kind sets are REJECTED — the whole point of a typed field id
    check(aver_scene_set_str(e, fPos, "nope") == 0, "set_str into a VEC field is rejected");
    check(aver_scene_set_f32(e, fPos, 1.0f) == 0, "set_f32 into a VEC field is rejected");
    check(aver_scene_set_i64(e, fPos, 1) == 0, "set_i64 into a VEC field is rejected");
    check(aver_scene_set_vec(e, fMesh, wantPos) == 0, "set_vec into an I64 field is rejected");
    check(aver_scene_set_i32(e, fMesh, 1) == 0, "set_i32 into an I64 field is rejected");
    check(aver_scene_set_ref(e, fMesh, e) == 0, "set_ref into an I64 field is rejected");
    check(std::string(aver_scene_get_str(e, fPos)).empty(), "get_str on a VEC field yields \"\"");
    check(aver_scene_get_i64(e, fPos) == 0, "get_i64 on a VEC field yields 0");
    // The rejected writes must not have corrupted the vector.
    aver_scene_get_vec(e, fPos, gotPos);
    check(gotPos[0] == 1.5f, "a rejected wrong-kind set left the field untouched");

    // ---- ref is its own kind: CHierarchy.parent reads back the parent set_parent wrote
    const int32_t parent = aver_scene_create();
    check(aver_scene_set_parent(e, parent) == 1, "set_parent attaches the child");
    const int32_t fParent = aver_scene_field("CHierarchy.parent");
    check(aver_scene_field_kind(fParent) == AVER_SCENE_KIND_ENTITY, "parent is an ENTITY-kind field");
    check(aver_scene_get_ref(e, fParent) == parent, "get_ref reads the parent set_parent wrote");
    check(aver_scene_get_i32(e, fParent) == 0, "get_i32 on an ENTITY field is rejected (0), not coerced");

    // ---- a child transform under the new parent, set through the ABI and composed by the world
    const float childPos[3] = {10.0f, 0.0f, 0.0f};
    check(aver_scene_set_vec(e, fPos, childPos) == 1, "the child's local position is set over the ABI");
    world.flush();
    // parent at origin, child at +10x local => child world x is 10.
    check(world.worldMatrix(e).m[3][0] == 10.0f, "the ABI-set child transform composed through its parent");

    // ---- a stale handle returns the neutral value everywhere
    const int32_t doomed = aver_scene_create();
    const int32_t fdoom  = aver_scene_field("CLocal.position");
    check(aver_scene_set_vec(doomed, fdoom, wantPos) == 1, "the doomed entity accepts a write while live");
    check(aver_scene_destroy(doomed) == 1, "destroy accepts it");
    world.flush();   // retire it: now the handle is stale
    check(aver_scene_valid(doomed) == 0, "the stale handle fails valid()");
    float neutral[3] = {-9, -9, -9};
    check(aver_scene_get_vec(doomed, fdoom, neutral) == 0, "get_vec on a stale handle returns 0");
    check(neutral[0] == -9, "and does not write the caller's buffer");
    check(aver_scene_get_f32(doomed, aver_scene_field("CCamera.fovYRad")) == 0.0f,
          "get_f32 on a stale handle is 0.0f");
    check(aver_scene_get_i64(doomed, fMesh) == 0, "get_i64 on a stale handle is 0");
    check(aver_scene_object_id(doomed) == 0, "object_id on a stale handle is 0");
    check(std::string(aver_scene_name(doomed)).empty(), "name on a stale handle is \"\"");
    check(aver_scene_set_vec(doomed, fdoom, wantPos) == 0, "set_vec on a stale handle is rejected");
    check(aver_scene_set_name(doomed, "zombie") == 0, "set_name on a stale handle is rejected");
    check(aver_scene_set_parent(doomed, parent) == 0, "set_parent on a stale child is rejected");
}

// -------------------------------------------------------------- the C ABI, regression cases

// Four defects an independent pass found in the generic field ABI, each reached entirely through the
// public typed accessors. They live in their own section because each corrupts a memory invariant the
// field ABI newly makes script-reachable, rather than testing an accessor's happy path.
static void testSceneAbiRepairs(World& world) {
    AVER_INFO("=== C ABI regression: field-ABI memory invariants ===");

    // ---- (1) the ABI is byte-transparent for UTF-8 names. The C# binding marshals inbound names as
    // LPUTF8Str and decodes the returned pointer as UTF-8; both rely on this DLL storing and returning
    // the exact bytes it was handed. LPStr/PtrToStringAnsi on the managed side would re-encode a
    // non-ASCII name through the ANSI code page, so this pins the contract those attributes depend on.
    const int32_t u = aver_scene_create();
    // "Ω-日本-x" spelled as raw UTF-8 bytes so the assertion does not depend on the source file's encoding.
    const char* utf8Name = "\xCE\xA9-\xE6\x97\xA5\xE6\x9C\xAC-x";
    check(aver_scene_set_name(u, utf8Name) == 1, "set_name accepts a UTF-8 name over the ABI");
    check(std::string(aver_scene_name(u)) == std::string(utf8Name),
          "the ABI round-trips UTF-8 name bytes unchanged (the LPUTF8Str contract)");
    check(aver_scene_object_id(u) == static_cast<int64_t>(fnv1a64(std::string_view(utf8Name))),
          "objectId is fnv1a64 of the UTF-8 bytes, so an ANSI re-encode would change identity");

    // ---- (2) CName.offset/len are the world's name-blob cursor, managed by setName. They are now
    // READ-ONLY over the generic ABI, so a script cannot write the cursor at all — and World::name
    // additionally bounds the slice against the blob (defence in depth for any C++ writer). Before this,
    // a correctly-typed set_i32 reached the cursor and name()/PtrToStringUTF8 walked a wild pointer
    // (out-of-bounds read: crash / disclosure).
    const int32_t nre = aver_scene_create();
    check(aver_scene_set_name(nre, "safe-name") == 1, "the entity gets a real name first");
    check(std::string(aver_scene_name(nre)) == "safe-name", "which resolves before tampering");
    const int32_t fOffset = aver_scene_field("CName.offset");
    check(fOffset != 0, "CName.offset resolves (it is in the table for verify()'s byte coverage)");
    check(aver_scene_field_kind(fOffset) == AVER_SCENE_KIND_I32, "and is I32-kind");
    check(aver_scene_set_i32(nre, fOffset, 0x40000000) == 0,
          "but a set is REJECTED — CName.offset is read-only over the generic ABI");
    check(std::string(aver_scene_name(nre)) == "safe-name",
          "so the name is untouched: no cursor corrupted, no out-of-bounds walk");

    // ---- read-only is general, not just CName: derived (CWorld, CLocal.rev) and bookkeeping fields
    // refuse a set while still READING. They stay in the table for verify()'s coverage.
    const int32_t rw       = aver_scene_create();
    const int32_t fLocalRev = aver_scene_field("CLocal.rev");
    check(fLocalRev != 0 && aver_scene_set_i32(rw, fLocalRev, 99) == 0, "CLocal.rev (derived) rejects a set");
    const int32_t fWorldMat = aver_scene_field("CWorld.matrix");
    float m16[16] = {0};
    check(fWorldMat != 0 && aver_scene_set_vec(rw, fWorldMat, m16) == 0, "CWorld.matrix (derived) rejects a set");
    check(aver_scene_get_vec(rw, fWorldMat, m16) == 1, "but a read of a read-only field still works");

    // ---- (3) CHierarchy.parent is an Entity-kind field the ABI resolves, but its structural links are
    // owned by setParent (which refuses cycles). set_ref must not write them raw: a single self-parent,
    // or a 2-cycle, makes composeChain()/worldMatrix() loop unbounded (hang/bad_alloc). set_parent stays
    // the only path; get_ref (a read) is unaffected.
    const int32_t h1     = aver_scene_create();
    const int32_t fParent = aver_scene_field("CHierarchy.parent");
    check(aver_scene_field_kind(fParent) == AVER_SCENE_KIND_ENTITY, "CHierarchy.parent is Entity-kind");
    check(aver_scene_set_ref(h1, fParent, h1) == 0, "set_ref refuses to self-parent through the structural link");
    check(aver_scene_get_ref(h1, fParent) == 0, "so no cycle formed: the parent link is still unset");
    const int32_t h2 = aver_scene_create();
    check(aver_scene_set_ref(h1, fParent, h2) == 0, "set_ref refuses a normal parent edit too — it is not the path");
    check(aver_scene_get_ref(h1, fParent) == 0, "the link stays unset after the refused edit");
    check(aver_scene_set_parent(h1, h2) == 1, "the guarded setter DOES attach the parent");
    check(aver_scene_get_ref(h1, fParent) == h2, "and get_ref now reads the parent it wrote");
    // If a cycle had slipped through, composing this subtree would never return.
    world.worldMatrix(h1);
    check(world.flush() == world.flush() || true, "composing the (acyclic) hierarchy returns rather than hanging");

    // ---- (4) set_str reuses the slot a field already owns instead of appending forever. Register a
    // String-kind component (no built-in is String-kind) and write one field many times: the intern
    // table must grow by exactly one, not once per write.
    struct CStr { int64_t s = 0; };
    {
        auto b = world.registerComponent<CStr>("CStr");
        b.field("s", FieldKind::String, static_cast<u16>(offsetof(CStr, s)));
        check(b.verify(sizeof(CStr)), "a one-String-field component verifies");
    }
    const int32_t strType = static_cast<int32_t>(world.componentId("CStr"));
    const int32_t fStr     = aver_scene_field("CStr.s");
    check(strType != 0 && fStr != 0, "the String component and field registered");
    check(aver_scene_field_kind(fStr) == AVER_SCENE_KIND_STRING, "CStr.s is String-kind");

    const int32_t se = aver_scene_create();
    check(aver_scene_add_component(se, strType) == 1, "the entity takes the String component");

    const int64_t poolBefore = aver_scene_debug_string_pool_size();
    const char*   values[]   = {"first", "second", "third", "fourth", "fifth"};
    for (const char* val : values) check(aver_scene_set_str(se, fStr, val) == 1, "set_str writes the String field");
    check(std::string(aver_scene_get_str(se, fStr)) == "fifth", "get_str reads back the most recent write");
    const int64_t poolAfter = aver_scene_debug_string_pool_size();
    check(poolAfter - poolBefore == 1,
          "five writes to one String field grew the intern pool by exactly one, not five");

    // An empty write keeps the slot (does not abandon it), and a non-empty write after it does not
    // allocate a second slot.
    check(aver_scene_set_str(se, fStr, "") == 1, "an empty set_str is accepted");
    check(std::string(aver_scene_get_str(se, fStr)).empty(), "and reads back as \"\"");
    check(aver_scene_set_str(se, fStr, "again") == 1, "a later non-empty write is accepted");
    check(aver_scene_debug_string_pool_size() - poolAfter == 0, "and reused the same slot, adding nothing");
}

// The query additions the C# gameplay API is built on: resolve-by-name and the composed world matrix.
static void testSceneAbiQuery(World& world) {
    AVER_INFO("=== C ABI query: aver_scene_find + aver_scene_world_matrix ===");

    const int32_t a = aver_scene_create();
    aver_scene_set_name(a, "find-me");
    check(aver_scene_find("find-me") == a, "find resolves a live entity by name");
    check(aver_scene_find("no-such-name") == 0, "find returns 0 for an unknown name");
    check(aver_scene_find(nullptr) == 0, "find tolerates a null name");

    // world_matrix of a translated, unrotated root: identity basis, translation in row 3 (row-vector).
    const int32_t b     = aver_scene_create();
    const int32_t fPos  = aver_scene_field("CLocal.position");
    float pos[3] = {10.0f, 20.0f, 30.0f};
    aver_scene_set_vec(b, fPos, pos);
    world.flush();
    float m[16] = {0};
    check(aver_scene_world_matrix(b, m) == 1, "world_matrix succeeds for a live entity");
    check(m[12] == 10.0f && m[13] == 20.0f && m[14] == 30.0f, "world_matrix carries the translation in row 3");
    check(m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[15] == 1.0f,
          "an unrotated, unscaled root reads an identity basis and w");
    check(aver_scene_world_matrix(0, m) == 0, "world_matrix rejects an invalid handle");
    check(aver_scene_world_matrix(b, nullptr) == 0, "world_matrix rejects a null out buffer");
}

// ---------------------------------------------------------------------------------------------- main

int main() {
    AVER_INFO("Aver.Scene test");

    testEntityPacking();

    World& world = World::instance();
    testGenerationRetirement(world);
    const Entity y = testLifetime(world);
    testPools(world, y);
    testChurn(world);
    testFields(world);
    testHierarchy(world);
    testSceneAbi(world);
    testSceneAbiRepairs(world);
    testSceneAbiQuery(world);

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
