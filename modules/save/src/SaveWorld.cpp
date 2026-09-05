#include "aver/save/SaveWorld.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/Fields.hpp"
#include "aver/scene/scene_abi.h"

#include <cstring>
#include <unordered_map>

namespace aver::save {
namespace {

using scene::FieldKind;

// THE DRIFT CHECK OcSave.cpp's own mirror comment promises. That file duplicates these values as
// plain u32 because Aver.Formats may not reach Aver.Scene; this module links BOTH, so it is the one
// place the two can be compared. A mirror with no assert anywhere is how three copies of one
// cbuffer layout became a standing hazard in this engine's RHI.
static_assert(AVER_SCENE_KIND_F32    == static_cast<u32>(FieldKind::F32),    "OcSave F32 drift");
static_assert(AVER_SCENE_KIND_VEC3   == static_cast<u32>(FieldKind::Vec3),   "OcSave VEC3 drift");
static_assert(AVER_SCENE_KIND_QUAT   == static_cast<u32>(FieldKind::Quat),   "OcSave QUAT drift");
static_assert(AVER_SCENE_KIND_I32    == static_cast<u32>(FieldKind::I32),    "OcSave I32 drift");
static_assert(AVER_SCENE_KIND_BOOL   == static_cast<u32>(FieldKind::Bool),   "OcSave BOOL drift");
static_assert(AVER_SCENE_KIND_I64    == static_cast<u32>(FieldKind::I64),    "OcSave I64 drift");
static_assert(AVER_SCENE_KIND_ENTITY == static_cast<u32>(FieldKind::Entity), "OcSave ENTITY drift");
static_assert(AVER_SCENE_KIND_STRING == static_cast<u32>(FieldKind::String), "OcSave STRING drift");
static_assert(AVER_SCENE_KIND_MAT4   == static_cast<u32>(FieldKind::Mat4),   "OcSave MAT4 drift");

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// Components a snapshot deliberately never writes, each for a stated reason. A SMALL NAMED LIST
// rather than an opt-in flag on the component, because the default must be "a component nobody
// GRAPH-LOCAL VARIABLES, as a SYNTHETIC component riding the same OcSaveComponent/OcSaveField
// shape every real component already uses -- not a new chunk, not a new list on OcSaveEntity, so
// every reader this format already has (including one built before this existed) round-trips a
// save that carries one unchanged. `$` cannot start a registered component's name (every real one
// is a bare C identifier, "CLocal"/"CMeshRenderer"/...), so this can never collide with one, and
// restore() special-cases it BEFORE the componentId() lookup below for exactly that reason -- a
// build with no graph scripting at all still opens a save that carries this, skipping it rather
// than warning "component '$GraphVars' is not registered in this build" as though it were a
// dropped FEATURE rather than a kind of state that was never a component to begin with.
constexpr char kGraphVarsComponentType[] = "$GraphVars";
// A generous cap, not a measured one: real graphs declare a handful of VARs. Exists so one
// pathological graph cannot make a save entity unbounded; the excess is dropped with a name and a
// count, never silently.
constexpr usize kMaxGraphVarsPerEntity = 64;

// Asks the host for entity `e`'s current graph-local VAR values (GraphVarStore, entirely managed
// memory -- see GraphVarCountFn's own comment for why this is a SEPARATE seam from the generic
// field walk below) and appends them as one synthetic component, if there are any. A no-op for an
// entity with no live graph host, or when the host installs neither seam.
void captureGraphVars(scene::Entity e, fmt::OcSaveEntity& en, const CaptureOptions& opt) {
    if (!opt.host.graphVarCount || !opt.host.graphVarAt) return;
    const i32 n = opt.host.graphVarCount(e, opt.host.user);
    if (n <= 0) return;

    const i32 want = n > static_cast<i32>(kMaxGraphVarsPerEntity)
                   ? static_cast<i32>(kMaxGraphVarsPerEntity) : n;
    fmt::OcSaveComponent sc;
    sc.type = kGraphVarsComponentType;
    char nameBuf[64];
    for (i32 i = 0; i < want; ++i) {
        u32 kind = 0; f32 fv = 0.0f; i32 iv = 0;
        nameBuf[0] = '\0';
        if (!opt.host.graphVarAt(e, i, nameBuf, static_cast<i32>(sizeof(nameBuf)), &kind, &fv, &iv,
                                 opt.host.user))
            continue;
        if (nameBuf[0] == '\0') continue;

        fmt::OcSaveField f;
        f.name = nameBuf;
        f.kind = kind;
        // F32 carries its value in `.f` (one element, matching ocSaveFloatCount's own F32 == 1);
        // I32 and BOOL both carry theirs in `.i`, the same split every real field above already
        // uses. A VAR is never any other kind (Aver.Graph's own PinType restriction), so anything
        // else here is a provider bug -- captured as I32 rather than silently dropped, so it is at
        // least visible in the file rather than invisible.
        if (kind == AVER_SCENE_KIND_F32) f.f = {fv};
        else                             f.i = iv;
        sc.fields.push_back(std::move(f));
    }
    if (n > static_cast<i32>(kMaxGraphVarsPerEntity))
        AVER_WARN("[Save] entity '{}' declares {} graph VAR(s), over the {} this format captures -- "
                  "the rest were dropped", en.name, n, kMaxGraphVarsPerEntity);
    if (!sc.fields.empty()) en.components.push_back(std::move(sc));
}

// Components a snapshot deliberately never writes, each for a stated reason. A SMALL NAMED LIST
// rather than an opt-in flag on the component, because the default must be "a component nobody
// thought about is saved" -- the opposite default is how a feature added later silently stops
// persisting and nobody notices until a player reports it.
bool isDerivedComponent(u32 type) {
    // Recomposed from CLocal by the hierarchy pass every frame. Writing it saves a matrix the first
    // flush after restore overwrites anyway, and reading it back could put a stale world transform
    // on screen for one frame.
    if (type == scene::kComponentWorld) return true;
    // Holds ENTITY HANDLES (parent/firstChild/nextSibling) that mean nothing in a file. Parenting
    // travels as OcSaveEntity::parent, an index, and is rebuilt by create(name, parent, xf) -- so
    // writing the component too would be a second, conflicting source of truth for one fact.
    if (type == scene::kComponentHierarchy) return true;
    // CName is {objectId, offset, len}, and BOTH of the things in it that mean anything travel
    // on OcSaveEntity explicitly. `offset`/`len` are a slice into the world's own name blob --
    // a different blob in the next process -- and are registered readOnly precisely because
    // writing them generically would be wrong. Capturing them and then failing to apply them
    // reported three "dropped fields" per entity on the first run: noise that would have
    // trained a reader to ignore the one message that matters.
    if (type == scene::kComponentName) return true;
    return false;
}

// One ENTITY-kind field, remembered so a second pass can turn its raw handle into a snapshot index.
// A SEPARATE LIST rather than stashing the handle in the field itself: a handle left behind by a bug
// in the second pass is indistinguishable from a valid index, and would silently restore a reference
// to whatever entity happened to land at that position.
struct PendingRef {
    usize entity, comp, field;
    scene::Entity raw;
};

struct Walk {
    fmt::OcSaveData& out;
    // Parallel to out.entities: the live handle each snapshot slot was captured from. This is what
    // makes the second pass exact rather than a re-derivation.
    std::vector<scene::Entity>& handles;
    std::vector<PendingRef>& pending;
};

void captureFields(const scene::World& w, scene::Entity e, fmt::OcSaveEntity& en, usize selfIndex,
                   Walk& g) {
    for (u32 ci = 0; ci < w.componentCount(); ++ci) {
        const u32 type = w.componentAt(ci);
        if (type == 0 || isDerivedComponent(type)) continue;
        const void* bytes = w.getComponent(e, type);
        if (!bytes) continue;

        fmt::OcSaveComponent sc;
        sc.type = w.componentName(type);
        const u32 fieldN = w.fieldCount(type);
        for (u32 fi = 0; fi < fieldN; ++fi) {
            const u32 id = w.fieldAt(type, fi);
            const scene::FieldDesc* d = w.field(id);
            if (!d) continue;
            // READ-ONLY FIELDS ARE NOT CAPTURED. A field marked readOnly is bookkeeping the world
            // itself owns -- CLocal.rev, CMeshRenderer.dirty, CName.offset/len -- and the generic
            // setter refuses to write one, so capturing it can only ever produce a value that is
            // then dropped. That is not harmless: it made a clean round trip report a dropped field
            // per entity on the first run, which would have trained a reader to ignore the warning
            // that exists to say "this build no longer has a field your save does".
            if (d->readOnly) continue;

            fmt::OcSaveField f;
            f.name = d->name;
            f.kind = static_cast<u32>(d->kind);
            const u8* addr = static_cast<const u8*>(bytes) + d->offset;

            switch (d->kind) {
                case FieldKind::F32:
                case FieldKind::Vec3:
                case FieldKind::Quat:
                case FieldKind::Mat4: {
                    const u32 n = fmt::ocSaveFloatCount(f.kind);
                    f.f.resize(n);
                    std::memcpy(f.f.data(), addr, n * sizeof(f32));
                    break;
                }
                case FieldKind::I32: {
                    // SIGN-EXTENDED into the i64, not zero-extended. A negative I32 read as a u32
                    // would come back as four billion.
                    i32 v; std::memcpy(&v, addr, 4); f.i = v;
                    break;
                }
                case FieldKind::Bool: {
                    i32 v; std::memcpy(&v, addr, 4); f.i = v != 0 ? 1 : 0;
                    break;
                }
                case FieldKind::I64: {
                    i64 v; std::memcpy(&v, addr, 8); f.i = v;
                    break;
                }
                case FieldKind::Entity: {
                    // The raw handle is remembered, not written: it may point at an entity this
                    // walk has not reached yet, so only a second pass can index it.
                    u32 v; std::memcpy(&v, addr, 4);
                    f.i = -1;
                    g.pending.push_back({selfIndex, en.components.size(), sc.fields.size(),
                                         static_cast<scene::Entity>(v)});
                    break;
                }
                case FieldKind::String: {
                    // THE TEXT, NEVER THE ID. A String field stores an i64 index into a
                    // process-local intern pool (SceneAbi.cpp:249-258); that number means nothing
                    // in a file, and nothing in the next process either.
                    const char* sv = aver_scene_get_str(static_cast<i32>(e), static_cast<i32>(id));
                    f.s = sv ? sv : "";
                    break;
                }
            }
            sc.fields.push_back(std::move(f));
        }
        en.components.push_back(std::move(sc));
    }
}

// Appends `e` then its children, depth first, so a parent always lands at a lower index than any of
// its descendants -- the invariant OcSaveData::valid() checks and restore relies on.
void appendSubtree(const scene::World& w, scene::Entity e, i32 parentIndex,
                   const CaptureOptions& opt, Walk& g) {
    if (!w.valid(e) || w.destroyPending(e)) return;
    if (opt.skip && opt.skip(e, opt.host.user)) return;   // and its whole subtree with it

    fmt::OcSaveEntity en;
    en.name = w.name(e);
    en.objectId = w.objectId(e);
    en.parent = parentIndex;
    if (opt.host.classOf) {
        if (const char* cn = opt.host.classOf(e, opt.host.user)) en.className = cn;
    }

    const usize selfIndex = g.out.entities.size();
    captureFields(w, e, en, selfIndex, g);
    captureGraphVars(e, en, opt);

    g.out.entities.push_back(std::move(en));
    g.handles.push_back(e);

    if (const auto* h = w.component<scene::CHierarchy>(e, scene::kComponentHierarchy)) {
        for (scene::Entity c = h->firstChild; c != scene::kInvalidEntity;) {
            const auto* ch = w.component<scene::CHierarchy>(c, scene::kComponentHierarchy);
            const scene::Entity next = ch ? ch->nextSibling : scene::kInvalidEntity;
            appendSubtree(w, c, static_cast<i32>(selfIndex), opt, g);
            c = next;
        }
    }
}

} // namespace

bool capture(const scene::World& w, fmt::OcSaveData& out, const CaptureOptions& opt, std::string* why) {
    out.entities.clear();
    std::vector<scene::Entity> handles;
    std::vector<PendingRef> pending;
    Walk g{out, handles, pending};

    // ROOTS FIRST, then each subtree depth-first. An entity whose parent is not live is a root here
    // too, so a dangling child is captured rather than dropped.
    for (u32 i = 0; i < w.count(); ++i) {
        const scene::Entity e = w.at(i);
        if (!w.valid(e) || w.destroyPending(e)) continue;
        const auto* h = w.component<scene::CHierarchy>(e, scene::kComponentHierarchy);
        if (h && h->parent != scene::kInvalidEntity && w.valid(h->parent)) continue;   // a child
        appendSubtree(w, e, -1, opt, g);
    }

    // Handle -> snapshot index, built from what was ACTUALLY EMITTED rather than from the world, so
    // a reference to a skipped entity resolves to -1 exactly as a reference to a dead one does.
    std::unordered_map<u32, i32> indexOf;
    indexOf.reserve(handles.size() * 2);
    for (usize i = 0; i < handles.size(); ++i)
        indexOf[static_cast<u32>(handles[i])] = static_cast<i32>(i);

    for (const PendingRef& p : pending) {
        const auto it = indexOf.find(static_cast<u32>(p.raw));
        out.entities[p.entity].components[p.comp].fields[p.field].i =
            it == indexOf.end() ? -1 : it->second;
    }

    if (!out.valid())
        return fail(why, "save: the captured snapshot is not internally consistent -- this is a bug "
                         "in capture(), not in the world it read");
    return true;
}

namespace {

// Writes one saved field back into live component bytes. Returns false when the field no longer
// exists or has changed kind -- both survivable, both worth counting.
bool applyField(scene::World& w, scene::Entity e, u32 type, const fmt::OcSaveComponent& sc,
                const fmt::OcSaveField& f, const std::vector<scene::Entity>& made) {
    const std::string qualified = sc.type + "." + f.name;
    const u32 id = w.fieldId(qualified);
    if (id == 0) return false;
    const scene::FieldDesc* d = w.field(id);
    if (!d) return false;
    // A KIND CHANGE IS NOT A CRASH, it is a dropped field. Reinterpreting eight saved bytes as a
    // Vec3 because someone changed a field's type between builds is how a save corrupts a world
    // while appearing to load.
    if (static_cast<u32>(d->kind) != f.kind) return false;
    if (d->readOnly) return false;

    void* bytes = w.getComponent(e, type);
    if (!bytes) return false;
    u8* addr = static_cast<u8*>(bytes) + d->offset;

    switch (d->kind) {
        case FieldKind::F32:
        case FieldKind::Vec3:
        case FieldKind::Quat:
        case FieldKind::Mat4: {
            const u32 n = fmt::ocSaveFloatCount(f.kind);
            if (f.f.size() != n) return false;
            std::memcpy(addr, f.f.data(), n * sizeof(f32));
            return true;
        }
        case FieldKind::I32: {
            const i32 v = static_cast<i32>(f.i);
            std::memcpy(addr, &v, 4);
            return true;
        }
        case FieldKind::Bool: {
            const i32 v = f.i != 0 ? 1 : 0;
            std::memcpy(addr, &v, 4);
            return true;
        }
        case FieldKind::I64: {
            const i64 v = f.i;
            std::memcpy(addr, &v, 8);
            return true;
        }
        case FieldKind::Entity: {
            // An index back into a handle. -1, and an index whose entity failed to restore, both
            // become kInvalidEntity: a reference to something that is not there must read as
            // nothing, never as entity 0.
            scene::Entity target = scene::kInvalidEntity;
            if (f.i >= 0 && static_cast<usize>(f.i) < made.size()) target = made[static_cast<usize>(f.i)];
            const u32 v = static_cast<u32>(target);
            std::memcpy(addr, &v, 4);
            return true;
        }
        case FieldKind::String:
            // Through the ABI, which owns the intern pool. Writing the i64 directly would store a
            // number pointing into a pool that has no such entry.
            return aver_scene_set_str(static_cast<i32>(e), static_cast<i32>(id), f.s.c_str()) != 0;
    }
    return false;
}

} // namespace

bool restore(const fmt::OcSaveData& in, scene::World& w, const RestoreOptions& opt, std::string* why) {
    if (!in.valid())
        return fail(why, "save: refusing to restore an inconsistent snapshot");

    // ---- teardown ---------------------------------------------------------------------------
    // Collected first, then destroyed, because destroying while walking the dense array shifts it.
    std::vector<scene::Entity> doomed;
    doomed.reserve(w.count());
    for (u32 i = 0; i < w.count(); ++i) {
        const scene::Entity e = w.at(i);
        if (!w.valid(e) || w.destroyPending(e)) continue;
        if (opt.skip && opt.skip(e, opt.host.user)) continue;
        doomed.push_back(e);
    }
    for (const scene::Entity e : doomed) {
        if (w.destroyPending(e)) continue;   // already queued as somebody's descendant
        // AN ACTOR GOES THROUGH THE FRAMEWORK so OnEndPlay runs and its managed instance is
        // released. World::destroy would take the entity out from under a live C# object.
        const char* cn = opt.host.classOf ? opt.host.classOf(e, opt.host.user) : nullptr;
        if (cn && *cn && opt.host.destroyActor) opt.host.destroyActor(e, opt.host.user);
        else                                    w.destroy(e);
    }
    w.flush();

    // ---- create -----------------------------------------------------------------------------
    std::vector<scene::Entity> made(in.entities.size(), scene::kInvalidEntity);
    u32 droppedFields = 0;

    for (usize i = 0; i < in.entities.size(); ++i) {
        const fmt::OcSaveEntity& en = in.entities[i];
        scene::Entity parent = scene::kInvalidEntity;
        if (en.parent >= 0 && static_cast<usize>(en.parent) < i) parent = made[static_cast<usize>(en.parent)];

        scene::Entity e = scene::kInvalidEntity;
        if (!en.className.empty() && opt.host.spawnClass) {
            // WITHOUT BeginPlay -- see SpawnClassFn's own comment. The fields go in below and
            // BeginPlay is dispatched after, so an actor's OnBeginPlay observes the restored world
            // rather than its class defaults.
            e = opt.host.spawnClass(en.className.c_str(), opt.host.user);
            if (e != scene::kInvalidEntity) {
                w.setName(e, en.name);
                if (parent != scene::kInvalidEntity) w.setParent(e, parent);
            }
        } else {
            if (!en.className.empty())
                AVER_WARN("[Save] '{}' is an instance of class '{}', and no spawner is installed -- "
                          "restoring it as a plain entity", en.name, en.className);
            e = w.create(en.name, parent, Transform{});
        }
        if (e == scene::kInvalidEntity) {
            // THE DOCUMENTED POSTCONDITION IS "EMPTY", and up to here this loop was not honouring it.
            // SaveWorld.hpp promises restore() "leaves the world EMPTY on a failure part-way -- a
            // half-restored world is worse than an empty one, because it looks playable", and this
            // early return used to walk away from every entity already created in made[0..i-1].
            // Reached whenever spawnClass refuses a name: saveSpawnClass returns kInvalidEntity for a
            // className this build no longer has (a class renamed or deleted since the save was
            // written), so a save that is merely OUT OF DATE -- not corrupt -- left a partial world
            // behind that the caller was told to treat as untouched.
            //
            // Torn down through the same two-branch rule the teardown block above uses, and for the
            // same reason: an actor has to go back through the framework so its managed instance is
            // released, and only a plain entity may be destroyed directly. classOf is asked rather
            // than assuming en.className, because the spawner is what decided what was actually made.
            for (usize j = 0; j < i; ++j) {
                const scene::Entity done = made[j];
                if (done == scene::kInvalidEntity || w.destroyPending(done)) continue;
                const char* cn = opt.host.classOf ? opt.host.classOf(done, opt.host.user) : nullptr;
                if (cn && *cn && opt.host.destroyActor) opt.host.destroyActor(done, opt.host.user);
                else                                    w.destroy(done);
            }
            w.flush();
            return fail(why, "save: could not recreate entity '" + en.name + "'");
        }
        made[i] = e;

        // Carried rather than rehashed, exactly as ChunkPayload.cpp:97-99 does and for the reason it
        // states: a level may hold an id that is not fnv1a64(name), and recomputing would silently
        // renumber every such entity on the first load. create() already set the default, so this
        // only writes when the saved id actually differs.
        //
        // ZERO MEANS "NOT CARRIED", NOT "SET IT TO ZERO". fnv1a64 never returns 0 for any input
        // (an empty string gives the offset basis), so a 0 here is a snapshot that predates the
        // field or was built by hand. Writing it through would clobber the correct hash and make
        // the entity unfindable by name -- World::find uses objectId as its prefilter
        // (World.cpp:413-421). The first run of the test below did exactly that.
        if (en.objectId != 0 && en.objectId != fnv1a64(en.name)) w.setObjectId(e, en.objectId);

        for (const fmt::OcSaveComponent& sc : en.components) {
            // GRAPH-LOCAL VARIABLES, special-cased BEFORE the componentId() lookup below -- this
            // name was never registered as a real component (see kGraphVarsComponentType's own
            // comment) and must not fall into the "not registered in this build" warning path,
            // which is for a FEATURE this build dropped, not a kind of state that was never a
            // component. Applied HERE, not in a later pass: the entity's live graph host already
            // exists by this point (spawnClass ran moments ago, above), seeded at its declared
            // defaults, and beginPlay has not run yet -- exactly the same window field restoration
            // already depends on, so an OnBeginPlay that reads a VAR sees the restored value, not
            // the default (see BeginPlayFn's own comment for the ordering this rests on).
            if (sc.type == kGraphVarsComponentType) {
                if (opt.host.graphVarSet) {
                    for (const fmt::OcSaveField& f : sc.fields) {
                        const f32 fv = (f.kind == AVER_SCENE_KIND_F32 && !f.f.empty()) ? f.f[0] : 0.0f;
                        if (!opt.host.graphVarSet(e, f.name.c_str(), f.kind, fv,
                                                  static_cast<i32>(f.i), opt.host.user))
                            ++droppedFields;
                    }
                } else {
                    droppedFields += static_cast<u32>(sc.fields.size());
                }
                continue;
            }
            const u32 type = w.componentId(sc.type);
            if (type == 0) {
                // A component the save knows and this build does not. The entity restores without
                // it rather than the whole load failing -- an engine that dropped a feature should
                // still open an old save.
                AVER_WARN("[Save] entity '{}' carried component '{}', which is not registered in "
                          "this build -- restored without it", en.name, sc.type);
                continue;
            }
            if (isDerivedComponent(type)) continue;
            if (!w.addComponent(e, type)) continue;
            for (const fmt::OcSaveField& f : sc.fields) {
                // ENTITY fields belong to the second pass alone -- see its own comment for why they
                // cannot be applied here. Applying them in BOTH passes was harmless for a field that
                // worked (the second write is identical to the first) but double-counted every field
                // that did not: a single renamed reference reported "2 field(s) were dropped", so the
                // one number a player or a bug report can quote was wrong by a factor of two exactly
                // when it mattered. Skipped rather than deducted, because the second pass is the pass
                // that can actually resolve a forward reference.
                if (f.kind == AVER_SCENE_KIND_ENTITY) continue;
                if (!applyField(w, e, type, sc, f, made)) ++droppedFields;
            }
        }

        // THE DIRTY BIT, or nothing downstream notices. addComponent hands back zero-filled bytes
        // and the GPU path only re-uploads when this is set -- EditorEntitySnapshot.cpp:61 records
        // the same fix for the same reason after restoring a mesh renderer.
        if (auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer))
            mr->dirty = 1;
    }

    // ---- ENTITY fields, second pass ---------------------------------------------------------
    // Not folded into the loop above: a reference may point FORWARD, at an entity that did not
    // exist yet when its holder was created. (Its holder's own PARENT could not point forward -- two
    // different rules, and OcSaveData::valid() encodes both.)
    for (usize i = 0; i < in.entities.size(); ++i) {
        if (made[i] == scene::kInvalidEntity) continue;
        for (const fmt::OcSaveComponent& sc : in.entities[i].components) {
            const u32 type = w.componentId(sc.type);
            if (type == 0 || isDerivedComponent(type)) continue;
            for (const fmt::OcSaveField& f : sc.fields)
                if (f.kind == AVER_SCENE_KIND_ENTITY)
                    if (!applyField(w, made[i], type, sc, f, made)) ++droppedFields;
        }
    }

    // ---- begin play -------------------------------------------------------------------------
    // LAST, and for every actor at once, so an actor's OnBeginPlay sees a world in which every
    // other restored actor already exists. Doing it per entity as they were created would make
    // "who is already there" depend on capture order.
    if (opt.host.beginPlay) {
        for (usize i = 0; i < in.entities.size(); ++i)
            if (!in.entities[i].className.empty() && made[i] != scene::kInvalidEntity)
                opt.host.beginPlay(made[i], opt.host.user);
    }

    if (droppedFields)
        AVER_WARN("[Save] {} field(s) in the save no longer exist in this build, or changed kind, "
                  "and were dropped", droppedFields);

    AVER_INFO("[Save] restored {} entities", in.entities.size());
    return true;
}

} // namespace aver::save
