// The gameplay framework's C ABI implementation: class registry, spawn/destroy, possession, play
// lifecycle, input and the managed dispatch seam. C++ types cross only within this DLL.

#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"

#include "aver/core/Log.hpp"
#include "aver/scene/scene_abi.h"

#include "aver/core/Hash.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/Fields.hpp"
#include "aver/scene/World.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace aver;
using namespace aver::scene;

namespace {

// The process-wide scene world.
World& world() { return World::instance(); }

// Writes a built-in component's real default struct into `out`. Script-declared components zero.
void componentDefaultBytes(u32 type, u8* out, usize n) {
    std::memset(out, 0, n);
    auto put = [&](const void* src, usize sz) { std::memcpy(out, src, sz < n ? sz : n); };
    switch (type) {
        case kComponentLocal:        { CLocal d{};        put(&d, sizeof d); break; }
        case kComponentWorld:        { CWorld d{};        put(&d, sizeof d); break; }
        case kComponentHierarchy:    { CHierarchy d{};    put(&d, sizeof d); break; }
        case kComponentName:         { CName d{};         put(&d, sizeof d); break; }
        case kComponentTags:         { CTags d{};         put(&d, sizeof d); break; }
        case kComponentMeshRenderer: { CMeshRenderer d{}; put(&d, sizeof d); break; }
        case kComponentLight:        { CLight d{};        put(&d, sizeof d); break; }
        case kComponentCamera:       { CCamera d{};       put(&d, sizeof d); break; }
        case kComponentSkeletalMesh: { CSkeletalMesh d{}; put(&d, sizeof d); break; }
        case kComponentAnimator:     { CAnimator d{};     put(&d, sizeof d); break; }
        // Every field of CParticleEmitter default-initialises to zero (see Components.hpp's own
        // comment on why kParticleEmitterStopped is a NEGATIVE flag), so this case is a behavioural
        // no-op today -- the memset above already produced the right bytes. Listed anyway, matching
        // every other built-in component here: the day one of those defaults stops being zero, this
        // switch is what keeps a directly-attached CParticleEmitter from silently reading wrong.
        case kComponentParticleEmitter: { CParticleEmitter d{}; put(&d, sizeof d); break; }
        default: break;
    }
}

// A flattened component list plus one contiguous blob of default bytes. Spawning memcpys over it.
struct Archetype {
    std::vector<u32> components;                 // scene component type ids, CLocal first
    std::vector<u32> offset;                     // parallel to components, into blob
    std::vector<u8>  blob;
    std::unordered_map<int32_t, std::string> strDefaults;   // field id -> value
    std::vector<int32_t> authoredFields;         // field ids this class wrote a non-String default for

    // Empties every list.
    void clear() {
        components.clear();
        offset.clear();
        blob.clear();
        strDefaults.clear();
        authoredFields.clear();
    }

    // Records that this class wrote a default for field `f`, de-duplicated.
    void authorField(int32_t f) {
        for (int32_t x : authoredFields)
            if (x == f) return;
        authoredFields.push_back(f);
    }

    // The index of a component in this archetype, or -1.
    int indexOf(u32 comp) const {
        for (usize i = 0; i < components.size(); ++i)
            if (components[i] == comp) return static_cast<int>(i);
        return -1;
    }

    // Appends a component's storage seeded with its real default, unless it is already present.
    void add(u32 comp) {
        if (indexOf(comp) >= 0) return;
        const usize sz = world().componentSize(comp);
        const u32   at = static_cast<u32>(blob.size());
        components.push_back(comp);
        offset.push_back(at);
        blob.resize(blob.size() + sz);
        componentDefaultBytes(comp, blob.data() + at, sz);
    }

    // The byte address of field `d` inside this blob, or nullptr when the component is not present.
    u8* fieldAddr(const FieldDesc* d) {
        const int i = indexOf(d->component);
        if (i < 0) return nullptr;
        return blob.data() + offset[static_cast<usize>(i)] + d->offset;
    }
};

// One row of the class registry.
struct ClassRecord {
    std::string name;
    u64         nameHash = 0;
    std::string parentName;                    // "" for a root; resolved by name
    int32_t     flags     = 0;
    int32_t     tickGroup = AVER_FW_TICK_PRE_PHYSICS;
    int32_t     tickOrder = 0;
    bool        sealed    = false;

    std::string defaultPawnName;
    std::string playerControllerName;
    int32_t     defaultPawn      = 0;
    int32_t     playerController = 0;

    Archetype own;       // this class's own declared components + defaults
    Archetype resolved;  // the sealed, parent-chain-flattened archetype

    // Resets the authorable state for a fresh (re)declare, keeping name, hash and lineage identity.
    void resetForDeclare() {
        flags = 0;
        tickGroup = AVER_FW_TICK_PRE_PHYSICS;
        tickOrder = 0;
        sealed = false;
        defaultPawnName.clear();
        playerControllerName.clear();
        defaultPawn = playerController = 0;
        own.clear();
        resolved.clear();
        // Every class carries CLocal, so transform defaults always have a home.
        own.add(kComponentLocal);
    }
};

// The class registry. A deque because class_name() hands out a c_str() into a record and a vector
// reallocating would dangle it; index 0 is a reserved dummy so handle 0 is invalid.
std::deque<ClassRecord>& classes() {
    static std::deque<ClassRecord> c = [] {
        std::deque<ClassRecord> d;
        d.emplace_back();   // handle 0 == invalid
        return d;
    }();
    return c;
}

// Class name -> handle.
std::unordered_map<std::string, int32_t>& classByName() {
    static std::unordered_map<std::string, int32_t> m;
    return m;
}

bool validClass(int32_t c) { return c > 0 && static_cast<usize>(c) < classes().size(); }
ClassRecord* rec(int32_t c) { return validClass(c) ? &classes()[static_cast<usize>(c)] : nullptr; }

// The handle for a class name, or 0.
int32_t findClass(std::string_view name) {
    auto& m  = classByName();
    auto  it = m.find(std::string(name));
    return it == m.end() ? 0 : it->second;
}

Entity toEntity(int32_t e) { return static_cast<Entity>(static_cast<uint32_t>(e)); }

// One entity-index slot of the class side map. The owner handle closes the reused-index hazard.
struct ClassOwner {
    Entity  owner = kInvalidEntity;
    int32_t cls   = 0;
};
// The framework-owned entity-index -> (owner, class) side map.
std::vector<ClassOwner>& classOwners() {
    static std::vector<ClassOwner> v;
    return v;
}

// Records `e` as an instance of class `c`.
void recordClass(Entity e, int32_t c) {
    const u32 idx = entityIndex(e);
    auto&     v   = classOwners();
    if (idx >= v.size()) v.resize(idx + 1);
    v[idx] = {e, c};
}

// The class of `e`, or 0 for a stale, reused or never-recorded handle.
int32_t classOfEntity(Entity e) {
    const u32 idx = entityIndex(e);
    auto&     v   = classOwners();
    if (idx >= v.size()) return 0;
    return (v[idx].owner == e && world().valid(e)) ? v[idx].cls : 0;
}

// Drops `e` from the class side map.
void forgetClass(Entity e) {
    const u32 idx = entityIndex(e);
    auto&     v   = classOwners();
    if (idx < v.size() && v[idx].owner == e) v[idx] = {kInvalidEntity, 0};
}

// The entities whose destroy is currently on the stack; membership refuses a re-entrant destroy.
std::unordered_set<Entity>& destroyInFlight() {
    static std::unordered_set<Entity> s;
    return s;
}

// The controllers whose possession dispatch is in flight; membership refuses a re-entrant possess.
std::unordered_set<Entity>& possessInFlight() {
    static std::unordered_set<Entity> s;
    return s;
}

// Controller -> possessed pawn.
std::unordered_map<Entity, Entity>& pawnByController() {
    static std::unordered_map<Entity, Entity> m;
    return m;
}
// Pawn -> possessing controller.
std::unordered_map<Entity, Entity>& controllerByPawn() {
    static std::unordered_map<Entity, Entity> m;
    return m;
}

// True when entity `e` is an actor whose class carries every bit of `flag`.
bool classHasFlags(Entity e, int32_t flag) {
    const int32_t c = classOfEntity(e);
    const ClassRecord* r = rec(c);
    return r && (r->flags & flag) == flag;
}

// The one table the framework calls up into managed gameplay code through, held by value.
AvManagedDispatch& managedDispatch() { static AvManagedDispatch d{}; return d; }
// True while a managed dispatch table is installed.
bool& managedInstalled()             { static bool b = false; return b; }

// Flattens class `c`'s parent chain root-first into `outResolved`, applying defaults field-level so
// a subclass inherits fields it never authored. False on a cycle or an undeclared parent.
bool flatten(int32_t c, Archetype& outResolved) {
    std::vector<int32_t> chain;
    std::unordered_map<int32_t, bool> seen;
    int32_t cur = c;
    while (cur != 0) {
        if (!validClass(cur)) return false;      // dangling handle
        if (seen[cur]) return false;             // cycle in the parent chain
        seen[cur] = true;
        chain.push_back(cur);
        const ClassRecord& r = classes()[static_cast<usize>(cur)];
        if (r.parentName.empty()) break;
        const int32_t p = findClass(r.parentName);
        if (p == 0) return false;                // named parent was never declared
        cur = p;
    }

    outResolved.clear();
    // Root-first so CLocal (seeded first on every class) leads the component order.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const ClassRecord& r = classes()[static_cast<usize>(*it)];
        for (u32 comp : r.own.components) outResolved.add(comp);
    }
    // add() already seeded every component's built-in default; re-apply authored fields root->leaf.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const ClassRecord& r = classes()[static_cast<usize>(*it)];
        for (const int32_t fid : r.own.authoredFields) {
            const FieldDesc* d = world().field(static_cast<u32>(fid));
            if (!d) continue;
            const int dst = outResolved.indexOf(d->component);
            if (dst < 0) continue;                       // component not in the resolved union
            const int src = r.own.indexOf(d->component);
            if (src < 0) continue;                       // authored field's component vanished (reset)
            const usize fsz = fieldByteSize(d->kind);
            std::memcpy(outResolved.blob.data() + outResolved.offset[static_cast<usize>(dst)] + d->offset,
                        r.own.blob.data()        + r.own.offset[static_cast<usize>(src)] + d->offset,
                        fsz);
        }
        for (const auto& kv : r.own.strDefaults) outResolved.strDefaults[kv.first] = kv.second;
    }
    return true;
}

// The AVER_FW_CLASS_* bits a child inherits from its ancestors' rows rather than declaring itself.
// ABSTRACT, MANAGED, TICKS and TICK_IN_EDITOR are each a per-declarer decision (an abstract base's
// children are not themselves unspawnable; a C# class's own Configure/Ticks call, or a graph class's
// own OnStart/OnTick scan, decides ticking independent of its parent) and so are deliberately NOT in
// this mask -- only the four "what KIND of row is this" bits propagate.
constexpr int32_t kInheritableKindFlags =
    AVER_FW_CLASS_PAWN | AVER_FW_CLASS_CONTROLLER | AVER_FW_CLASS_GAME_MODE | AVER_FW_CLASS_GAME_INSTANCE;

// The kind flags `c` inherits from its ANCESTORS (never including c's own row) -- the union of every
// ancestor's kind bits, walking the same parentName chain flatten() already validated for this seal.
// A cycle or dangling parent simply stops the walk early rather than failing it a second time; seal
// already refused those via flatten() before this ever runs, so this walk exists to gather flags, not
// to re-validate the chain.
//
// WHY THIS EXISTS: a graph class parented to "GameMode" (or "Character", or any other base row)
// carries none of that base's kind bits on its own -- DeclareGraphClasses only ever ORs in MANAGED
// (HostBridge.cs), and a C# class's own ResolveClassIdentity/BaseFlagsOf pair only reads the C# TYPE
// hierarchy, which a graph class has none of. Without this, aver_fw_find_class_with_flags(GAME_MODE)
// can never find a graph-declared GameMode (see beginPlayIfGameModeDeclared's own comment on why that
// query is what decides whether a play session ever starts), and aver_fw_possess refuses a
// graph-declared Pawn/Controller outright. This is called from sealClass, which every declared class
// -- C# or graph -- already passes through, so one fix covers every declarer, not just graphs.
int32_t inheritedKindFlags(int32_t c) {
    int32_t flags = 0;
    std::unordered_map<int32_t, bool> seen;
    int32_t cur = c;
    while (validClass(cur) && !seen[cur]) {
        seen[cur] = true;
        const ClassRecord& r = classes()[static_cast<usize>(cur)];
        if (r.parentName.empty()) break;
        const int32_t p = findClass(r.parentName);
        if (p == 0) break;
        flags |= (classes()[static_cast<usize>(p)].flags & kInheritableKindFlags);
        cur = p;
    }
    return flags;
}

// Flattens the class and resolves its GameMode wiring by name. False on a bad parent chain.
bool sealClass(int32_t c) {
    ClassRecord* r = rec(c);
    if (!r) return false;
    if (!flatten(c, r->resolved)) return false;
    r->flags |= inheritedKindFlags(c);
    r->defaultPawn      = r->defaultPawnName.empty()      ? 0 : findClass(r->defaultPawnName);
    r->playerController = r->playerControllerName.empty() ? 0 : findClass(r->playerControllerName);
    r->sealed = true;
    return true;
}

// The shared body of the typed default setters: resolves the field, kind-checks it and writes the
// value into the class's own blob. False on a wrong kind, unknown, read-only or absent field.
bool setDefaultBytes(int32_t c, int32_t f, FieldKind wantKind, const void* src, usize sz) {
    ClassRecord* r = rec(c);
    if (!r) return false;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return false;
    if (d->kind != wantKind) return false;
    u8* addr = r->own.fieldAddr(d);
    if (!addr) return false;
    std::memcpy(addr, src, sz);
    r->own.authorField(f);
    r->sealed = false;
    return true;
}

}  // namespace

extern "C" {

// This binary's framework ABI version.
int32_t aver_fw_abi_version(void) {
    return AVER_FW_ABI_VERSION;
}

// The scene ABI version from the HEADER this DLL compiled against, not from the loaded scene DLL.
int32_t aver_fw_scene_abi_version(void) {
    return AVER_SCENE_ABI_VERSION;
}

// 1 when the loaded Aver.Scene reports the same MAJOR as this binary. The one call into Aver.Scene.
int32_t aver_fw_scene_abi_matches(void) {
    const int32_t loaded = aver_scene_abi_version() >> 16;
    return loaded == AVER_SCENE_ABI_VERSION_MAJOR ? 1 : 0;
}

// Declares a class. Idempotent by name: an existing name keeps its handle and its live instances.
int32_t aver_fw_class_declare(const char* name, const char* parentName) {
    if (!name || !*name) return 0;
    const std::string n  = name;
    const std::string pn = (parentName && *parentName) ? parentName : std::string();

    if (const int32_t existing = findClass(n)) {
        ClassRecord& r = classes()[static_cast<usize>(existing)];
        r.resetForDeclare();
        r.parentName = pn;
        return existing;
    }

    const int32_t handle = static_cast<int32_t>(classes().size());
    classes().emplace_back();
    ClassRecord& r = classes()[static_cast<usize>(handle)];
    r.name       = n;
    r.nameHash   = fnv1a64(std::string_view(n));
    r.resetForDeclare();
    r.parentName = pn;
    classByName()[n] = handle;
    return handle;
}

// The handle for a class name, or 0 when unknown.
int32_t aver_fw_class_find(const char* name) {
    return (name && *name) ? findClass(name) : 0;
}

// The class's name, or "" for an invalid handle. The caller must not free it.
const char* aver_fw_class_name(int32_t c) {
    const ClassRecord* r = rec(c);
    return r ? r->name.c_str() : "";
}

// The class's parent handle, or 0 for a root.
int32_t aver_fw_class_parent(int32_t c) {
    const ClassRecord* r = rec(c);
    return (r && !r->parentName.empty()) ? findClass(r->parentName) : 0;
}

// Clears the class's components and defaults, keeping its lineage.
int32_t aver_fw_class_reset(int32_t c) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const std::string keepParent = r->parentName;
    r->resetForDeclare();
    r->parentName = keepParent;
    return 1;
}

// Adds a registered scene component to the class's archetype.
int32_t aver_fw_class_add_component(int32_t c, int32_t component) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const u32 type = static_cast<u32>(component);
    if (type == 0 || world().pool(type) == nullptr) return 0;
    r->own.add(type);
    r->sealed = false;
    return 1;
}

// Sets the class's AVER_FW_CLASS_* flags.
int32_t aver_fw_class_set_flags(int32_t c, int32_t flags) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    r->flags = flags;
    return 1;
}

// The class's AVER_FW_CLASS_* flags.
int32_t aver_fw_class_get_flags(int32_t c) {
    const ClassRecord* r = rec(c);
    return r ? r->flags : 0;
}

// Sets the class's tick group and order. Rejects an out-of-range group.
int32_t aver_fw_class_set_tick(int32_t c, int32_t tickGroup, int32_t tickOrder) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    if (tickGroup < 0 || tickGroup >= AVER_FW_TICK_COUNT) return 0;
    r->tickGroup = tickGroup;
    r->tickOrder = tickOrder;
    return 1;
}

// Flattens the parent chain into the resolved archetype. 0 on a cycle or an undeclared parent.
int32_t aver_fw_class_seal(int32_t c) {
    return sealClass(c) ? 1 : 0;
}

// Sets an F32 field's class default.
int32_t aver_fw_class_set_default_f32(int32_t c, int32_t f, float v) {
    return setDefaultBytes(c, f, FieldKind::F32, &v, sizeof v) ? 1 : 0;
}

// Sets an I32 or Bool field's class default; a bool rides an i32 in the same 4-byte slot.
int32_t aver_fw_class_set_default_i32(int32_t c, int32_t f, int32_t v) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return 0;
    if (d->kind != FieldKind::I32 && d->kind != FieldKind::Bool) return 0;
    u8* addr = r->own.fieldAddr(d);
    if (!addr) return 0;
    std::memcpy(addr, &v, sizeof v);
    r->own.authorField(f);
    r->sealed = false;
    return 1;
}

// Sets an I64 field's class default.
int32_t aver_fw_class_set_default_i64(int32_t c, int32_t f, int64_t v) {
    return setDefaultBytes(c, f, FieldKind::I64, &v, sizeof v) ? 1 : 0;
}

// Sets any float-kind field's class default. `v` must hold the kind's arity in floats.
int32_t aver_fw_class_set_default_vec(int32_t c, int32_t f, const float* v) {
    if (!v) return 0;
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return 0;
    const u8 arity = canonicalArity(d->kind);
    if (arity == 0) return 0;
    u8* addr = r->own.fieldAddr(d);
    if (!addr) return 0;
    std::memcpy(addr, v, static_cast<usize>(arity) * sizeof(float));
    r->own.authorField(f);
    r->sealed = false;
    return 1;
}

// Sets a String field's class default. Held out of band: the 8-byte slot is an intern id into the
// scene's private pool, so it is re-applied at spawn through aver_scene_set_str.
int32_t aver_fw_class_set_default_str(int32_t c, int32_t f, const char* v) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return 0;
    if (d->kind != FieldKind::String) return 0;
    r->own.strDefaults[f] = v ? v : "";
    r->sealed = false;
    return 1;
}

// Names the GameMode's default pawn class, resolved at seal.
int32_t aver_fw_class_set_default_pawn(int32_t gameMode, const char* pawnClassName) {
    ClassRecord* r = rec(gameMode);
    if (!r) return 0;
    r->defaultPawnName = (pawnClassName && *pawnClassName) ? pawnClassName : "";
    r->sealed = false;
    return 1;
}

// Names the GameMode's player controller class, resolved at seal.
int32_t aver_fw_class_set_player_controller(int32_t gameMode, const char* controllerClassName) {
    ClassRecord* r = rec(gameMode);
    if (!r) return 0;
    r->playerControllerName = (controllerClassName && *controllerClassName) ? controllerClassName : "";
    r->sealed = false;
    return 1;
}

// The spawn body: attaches the sealed archetype, lays down defaults, applies transform overrides and
// runs the managed BEGIN edge. `dispatchBeginPlay` false gives a preview bind + build_models only.
static int32_t spawnActor(int32_t c, const char* name,
                          const float* pos3, const float* quat4, const float* scale3,
                          bool dispatchBeginPlay) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    // An ABSTRACT class is a lineage anchor, never an instance.
    if (r->flags & AVER_FW_CLASS_ABSTRACT) return 0;
    if (!r->sealed && !sealClass(c)) return 0;

    const Entity e = world().create(name ? std::string_view(name) : std::string_view());
    if (e == kInvalidEntity) return 0;

    const Archetype& a = r->resolved;
    for (usize i = 0; i < a.components.size(); ++i) {
        const u32 comp = a.components[i];
        void*     base = world().addComponent(e, comp);
        if (!base) continue;
        std::memcpy(base, a.blob.data() + a.offset[i], world().componentSize(comp));
    }
    // String defaults ride the scene's intern path rather than the blob.
    for (const auto& kv : a.strDefaults) aver_scene_set_str(e, kv.first, kv.second.c_str());

    // Rotation crosses as a quaternion; a null channel keeps the class default.
    if (CLocal* loc = static_cast<CLocal*>(world().getComponent(e, kComponentLocal))) {
        if (pos3)   loc->xf.position = {pos3[0], pos3[1], pos3[2]};
        if (quat4)  loc->xf.rotation = {quat4[0], quat4[1], quat4[2], quat4[3]};
        if (scale3) loc->xf.scale    = {scale3[0], scale3[1], scale3[2]};
        // Written through the pool directly, so the revision must be bumped by hand.
        world().touchLocal(e);
    }

    recordClass(e, c);

    // The BEGIN edge: bind(nameHash, e) -> build_models(e) -> beginPlay(e, SPAWN). bind is the gate.
    if ((r->flags & AVER_FW_CLASS_MANAGED) && managedInstalled()) {
        AvManagedDispatch& d = managedDispatch();
        const aver_entity  ae = static_cast<aver_entity>(e);
        const bool bound = d.bind && d.bind(static_cast<int64_t>(r->nameHash), ae) == 1;
        if (bound) {
            if (d.build_models) d.build_models(ae);
            if (dispatchBeginPlay && d.beginPlay) d.beginPlay(ae, AVER_FW_BEGIN_SPAWN);
        }
    }

    return static_cast<int32_t>(e);
}

// The teardown body: runs the managed END edge, drops possession and the class, then destroys the
// entity. `endReason` distinguishes a destroy from a play-stop; `dispatchEndPlay` false skips the hook.
static int32_t destroyActor(int32_t e, int32_t endReason, bool dispatchEndPlay = true) {
    const Entity ent = toEntity(e);
    if (!world().valid(ent)) return 0;

    auto& inFlight = destroyInFlight();
    if (inFlight.count(ent)) return 0;
    inFlight.insert(ent);

    // The END edge: endPlay while the entity is still live, then unbind. unbind fires even for a
    // preview, which bound an instance it never received a hook for.
    if (classHasFlags(ent, AVER_FW_CLASS_MANAGED) && managedInstalled()) {
        AvManagedDispatch& d = managedDispatch();
        const aver_entity  ae = static_cast<aver_entity>(e);
        if (dispatchEndPlay && d.endPlay) d.endPlay(ae, endReason);
        if (d.unbind)  d.unbind(ae);
    }

    if (const int32_t pawn = aver_fw_controlled_pawn(e)) {
        controllerByPawn().erase(toEntity(pawn));
    }
    // Read the controller before the erases below retire it.
    if (const int32_t ctrl = aver_fw_controller_of(e)) {
        pawnByController().erase(toEntity(ctrl));
    }
    pawnByController().erase(ent);
    controllerByPawn().erase(ent);
    forgetClass(ent);
    const int32_t destroyed = world().destroy(ent) ? 1 : 0;
    inFlight.erase(ent);
    return destroyed;
}

// Destroys an actor, dispatching OnEndPlay(DESTROY).
int32_t aver_fw_destroy(int32_t e) { return destroyActor(e, AVER_FW_END_DESTROY); }

// Spawns an actor and dispatches its full BEGIN edge.
int32_t aver_fw_spawn(int32_t c, const char* name,
                      const float* pos3, const float* quat4, const float* scale3) {
    return spawnActor(c, name, pos3, quat4, scale3, /*dispatchBeginPlay=*/true);
}

// Spawns an actor for a preview: bind and build_models, no OnBeginPlay.
int32_t aver_fw_spawn_preview(int32_t c, const char* name,
                              const float* pos3, const float* quat4, const float* scale3) {
    return spawnActor(c, name, pos3, quat4, scale3, /*dispatchBeginPlay=*/false);
}

// Destroys a preview actor without dispatching OnEndPlay.
int32_t aver_fw_destroy_preview(int32_t e) {
    return destroyActor(e, AVER_FW_END_DESTROY, /*dispatchEndPlay=*/false);
}

// The entity's class, or 0.
// ---- ANIMATION CURVES, RELAYED ---------------------------------------------------------------
//
// A FUNCTION POINTER, NOT A LINK EDGE, and the alternatives are worth naming. Aver.Framework could
// link Aver.Anim.Scene and call it directly -- but that module is STATIC, so linking it from here
// AND from the executable would give the process two copies of a system whose whole design is a
// process-global singleton. Making it SHARED for one query would change how every composition root
// builds. Relaying costs one indirect call on a query nothing calls in a hot loop.
//
// NOT ATOMIC, and that is a deliberate match to the rest of this file: the provider is installed
// once at startup by the composition root, on the same thread everything else here runs on.
aver_fw_anim_curve_fn g_animCurve = nullptr;
void* g_animCurveUser = nullptr;

int32_t aver_fw_set_anim_curve_provider(aver_fw_anim_curve_fn fn, void* user) {
    g_animCurve = fn;
    g_animCurveUser = user;
    return 1;
}

int32_t aver_fw_anim_curve(int32_t entity, int64_t nameHash, float* outValue) {
    // outValue is LEFT ALONE on every failure. A caller that ignores the return code and reads it
    // anyway gets whatever it initialised, which is its own default -- not a zero this function
    // invented and that reads exactly like a curve that genuinely holds zero.
    if (!g_animCurve || !outValue) return 0;
    return g_animCurve(entity, nameHash, outValue, g_animCurveUser);
}

// ---- GRAPH-LOCAL VARIABLES, RELAYED -----------------------------------------------------------
// Same shape as g_animCurve just above, for the same reason: only a composition root links both
// Aver.Framework and Aver.Graph, so this file holds a function pointer instead of an edge to it.
// NOT ATOMIC, matching every other provider in this file.
aver_fw_graph_var_count_fn g_graphVarCount = nullptr;
aver_fw_graph_var_at_fn    g_graphVarAt    = nullptr;
aver_fw_graph_var_set_fn   g_graphVarSet   = nullptr;
void* g_graphVarUser = nullptr;

int32_t aver_fw_set_graph_var_provider(aver_fw_graph_var_count_fn count, aver_fw_graph_var_at_fn at,
                                       aver_fw_graph_var_set_fn setVar, void* user) {
    g_graphVarCount = count;
    g_graphVarAt    = at;
    g_graphVarSet   = setVar;
    g_graphVarUser  = user;
    return 1;
}

int32_t aver_fw_graph_var_count(int32_t e) {
    if (!g_graphVarCount) return 0;
    return g_graphVarCount(e, g_graphVarUser);
}

int32_t aver_fw_graph_var_at(int32_t e, int32_t index, char* nameBuf, int32_t nameBufLen,
                             int32_t* outKind, float* outF, int32_t* outI) {
    if (!g_graphVarAt || !nameBuf || nameBufLen <= 0 || !outKind || !outF || !outI) return 0;
    return g_graphVarAt(e, index, nameBuf, nameBufLen, outKind, outF, outI, g_graphVarUser);
}

int32_t aver_fw_graph_var_set(int32_t e, const char* name, int32_t kind, float f, int32_t i) {
    if (!g_graphVarSet || !name || !*name) return 0;
    return g_graphVarSet(e, name, kind, f, i, g_graphVarUser);
}

// ---- SYNAPSE STEERING TARGET, RELAYED ----------------------------------------------------------
// Same shape as g_animCurve/g_graphVar* above, for the same reason: only a composition root links
// both Aver.Framework and Aver.Synapse.Scene, so this file holds a function pointer instead of an
// edge to it. NOT ATOMIC, matching every other provider in this file.
aver_fw_synapse_target_fn g_synapseTarget = nullptr;
void* g_synapseTargetUser = nullptr;

int32_t aver_fw_set_synapse_target_provider(aver_fw_synapse_target_fn fn, void* user) {
    g_synapseTarget = fn;
    g_synapseTargetUser = user;
    return 1;
}

int32_t aver_fw_synapse_target(int32_t e, float* outX, float* outY, float* outZ) {
    if (!g_synapseTarget || !outX || !outY || !outZ) return 0;
    return g_synapseTarget(e, outX, outY, outZ, g_synapseTargetUser);
}

// ---- SYNAPSE PERCEPTION, RELAYED ---------------------------------------------------------------
aver_fw_synapse_perception_fn g_synapsePerception = nullptr;
void* g_synapsePerceptionUser = nullptr;

int32_t aver_fw_set_synapse_perception_provider(aver_fw_synapse_perception_fn fn, void* user) {
    g_synapsePerception = fn;
    g_synapsePerceptionUser = user;
    return 1;
}

int32_t aver_fw_synapse_perception(int32_t e, int32_t* outCanSee, int32_t* outLastTarget,
                                   float* outTimeSinceSeen) {
    if (!g_synapsePerception || !outCanSee || !outLastTarget || !outTimeSinceSeen) return 0;
    return g_synapsePerception(e, outCanSee, outLastTarget, outTimeSinceSeen, g_synapsePerceptionUser);
}

// Dispatches OnBeginPlay on an actor spawned without it. See the header for why the split exists.
int32_t aver_fw_dispatch_begin_play(int32_t e, int32_t reason) {
    const Entity ent = static_cast<Entity>(static_cast<uint32_t>(e));
    if (!world().valid(ent)) return 0;
    if (classOfEntity(ent) == 0) return 0;   // not an actor; nothing is bound to dispatch to
    AvManagedDispatch& d = managedDispatch();
    if (!d.beginPlay) return 0;
    d.beginPlay(static_cast<aver_entity>(ent), reason);
    return 1;
}

// ---- SAVE/LOAD, RELAYED ---------------------------------------------------------------------
//
// A function pointer rather than a link edge, for the reason the header gives and the reason
// g_animCurve above gives: only a composition root links both sides. Not atomic, matching every
// other installed pointer in this file -- it is set once at startup on the one thread everything
// here runs on.
aver_fw_save_fn g_saveWrite = nullptr;
aver_fw_save_fn g_saveLoad  = nullptr;
void* g_saveUser = nullptr;

int32_t aver_fw_set_save_provider(aver_fw_save_fn write, aver_fw_save_fn load, void* user) {
    g_saveWrite = write;
    g_saveLoad  = load;
    g_saveUser  = user;
    return 1;
}

int32_t aver_fw_save_write(const char* utf8Path) {
    if (!g_saveWrite || !utf8Path || !*utf8Path) return 0;
    return g_saveWrite(utf8Path, g_saveUser);
}

int32_t aver_fw_save_load(const char* utf8Path) {
    if (!g_saveLoad || !utf8Path || !*utf8Path) return 0;
    return g_saveLoad(utf8Path, g_saveUser);
}

int32_t aver_fw_class_of(int32_t e) {
    return classOfEntity(toEntity(e));
}

// Dispatches OnPossessed to a managed pawn.
void dispatchPossessed(Entity pawn, Entity controller) {
    if (managedInstalled() && classHasFlags(pawn, AVER_FW_CLASS_MANAGED))
        if (auto& d = managedDispatch(); d.possessed)
            d.possessed(static_cast<aver_entity>(pawn), static_cast<aver_entity>(controller));
}
// Dispatches OnUnpossessed to a managed pawn.
void dispatchUnpossessed(Entity pawn) {
    if (managedInstalled() && classHasFlags(pawn, AVER_FW_CLASS_MANAGED))
        if (auto& d = managedDispatch(); d.unpossessed)
            d.unpossessed(static_cast<aver_entity>(pawn));
}
// Dispatches OnPostLogin to a managed GameMode.
void dispatchPostLogin(Entity gameMode, Entity controller) {
    if (managedInstalled() && classHasFlags(gameMode, AVER_FW_CLASS_MANAGED))
        if (auto& d = managedDispatch(); d.post_login)
            d.post_login(static_cast<aver_entity>(gameMode), static_cast<aver_entity>(controller));
}

// Possesses a pawn. Rejected unless the classes carry CONTROLLER and PAWN. Exclusive on both ends:
// a displaced or stolen pawn gets OnUnpossessed before the new pawn gets OnPossessed. Idempotent.
int32_t aver_fw_possess(int32_t controller, int32_t pawn) {
    const Entity ctrl = toEntity(controller);
    const Entity pwn  = toEntity(pawn);
    if (!world().valid(ctrl) || !world().valid(pwn)) return 0;
    if (!classHasFlags(ctrl, AVER_FW_CLASS_CONTROLLER)) return 0;
    if (!classHasFlags(pwn,  AVER_FW_CLASS_PAWN))       return 0;

    auto& pOf = pawnByController();
    auto& cOf = controllerByPawn();

    if (auto it = pOf.find(ctrl); it != pOf.end() && it->second == pwn) return 1;

    auto& inFlight = possessInFlight();
    if (inFlight.count(ctrl)) return 0;

    // Capture what is displaced before rewriting the maps.
    Entity     displaced = kInvalidEntity;
    const bool stolen    = cOf.find(pwn) != cOf.end();
    if (auto it = pOf.find(ctrl); it != pOf.end()) { displaced = it->second; cOf.erase(it->second); }
    if (auto it = cOf.find(pwn);  it != cOf.end()) pOf.erase(it->second);
    pOf[ctrl] = pwn;
    cOf[pwn]  = ctrl;

    inFlight.insert(ctrl);
    if (displaced != kInvalidEntity && displaced != pwn) dispatchUnpossessed(displaced);
    if (stolen) dispatchUnpossessed(pwn);
    dispatchPossessed(pwn, ctrl);
    inFlight.erase(ctrl);
    return 1;
}

// Releases whatever pawn this controller drives. 0 if it drives none.
int32_t aver_fw_unpossess(int32_t controller) {
    const Entity ctrl = toEntity(controller);
    auto& inFlight = possessInFlight();
    if (inFlight.count(ctrl)) return 0;
    auto& pOf = pawnByController();
    auto  it  = pOf.find(ctrl);
    if (it == pOf.end()) return 0;
    const Entity released = it->second;
    controllerByPawn().erase(it->second);
    pOf.erase(it);
    inFlight.insert(ctrl);
    dispatchUnpossessed(released);   // after the maps are clear, so a hook reads the settled state
    inFlight.erase(ctrl);
    return 1;
}

// The pawn this controller drives, or 0 if none or it was destroyed.
int32_t aver_fw_controlled_pawn(int32_t controller) {
    auto& pOf = pawnByController();
    auto  it  = pOf.find(toEntity(controller));
    if (it == pOf.end()) return 0;
    return world().valid(it->second) ? static_cast<int32_t>(it->second) : 0;
}

// The controller driving this pawn, or 0 if none or it was destroyed.
int32_t aver_fw_controller_of(int32_t pawn) {
    auto& cOf = controllerByPawn();
    auto  it  = cOf.find(toEntity(pawn));
    if (it == cOf.end()) return 0;
    return world().valid(it->second) ? static_cast<int32_t>(it->second) : 0;
}

// Installs the managed dispatch table by value. Refuses a stale, short or second table, and logs why.
int32_t aver_fw_install_managed_dispatch(const AvManagedDispatch* d) {
    if (!d) return 0;
    if (d->structBytes != static_cast<int32_t>(sizeof(AvManagedDispatch)) ||
        d->contractVersion != AVER_FW_DISPATCH_VERSION) {
        AVER_ERROR("aver_fw_install_managed_dispatch: rejected a dispatch table (structBytes={}, version={}; "
                   "this framework expects {} / {})",
                   d->structBytes, d->contractVersion,
                   static_cast<int32_t>(sizeof(AvManagedDispatch)), AVER_FW_DISPATCH_VERSION);
        return 0;
    }
    if (managedInstalled()) {
        AVER_ERROR("aver_fw_install_managed_dispatch: a managed dispatch is already installed; refusing a "
                   "second install (clear the first before installing another)");
        return 0;
    }
    managedDispatch() = *d;        // BY VALUE — no pointer into CLR-owned memory is retained
    managedInstalled() = true;
    return 1;
}

// Null-stores the managed dispatch table. Call before the ALC is unloaded.
int32_t aver_fw_clear_managed_dispatch(void) {
    managedDispatch() = AvManagedDispatch{};
    managedInstalled() = false;
    return 1;
}

// 1 while a managed dispatch table is live.
int32_t aver_fw_managed_dispatch_installed(void) {
    return managedInstalled() ? 1 : 0;
}

// Ticks one group: exactly one tick_all call into managed code. 1 if it fired.
int32_t aver_fw_tick(int32_t tickGroup, float dt) {
    if (managedInstalled() && managedDispatch().tick_all) {
        managedDispatch().tick_all(tickGroup, dt);
        return 1;
    }
    return 0;
}

// The process-global play state.
int32_t& playStateRef()    { static int32_t s = AVER_FW_PLAY_EDITOR; return s; }
// The session's GameInstance entity, 0 in EDITOR.
int32_t& gameInstanceRef() { static int32_t e = 0; return e; }
// The session's GameMode entity, 0 in EDITOR.
int32_t& gameModeRef()     { static int32_t e = 0; return e; }
// The session's player controller entity, 0 in EDITOR.
int32_t& playerCtrlRef()   { static int32_t e = 0; return e; }
// The session's pawn entity, 0 in EDITOR.
int32_t& playPawnRef()     { static int32_t e = 0; return e; }

// The first declared non-abstract class carrying all of `flags`, or 0. 0 flags -> 0.
int32_t aver_fw_find_class_with_flags(int32_t flags) {
    if (flags == 0) return 0;
    auto& cs = classes();
    for (int32_t c = 1; c < static_cast<int32_t>(cs.size()); ++c) {
        const int32_t f = cs[static_cast<usize>(c)].flags;
        if (f & AVER_FW_CLASS_ABSTRACT) continue;
        if ((f & flags) == flags) return c;
    }
    return 0;
}

// Begins a play session: spawns the optional GameInstance, the GameMode, and its controller and pawn
// (possessed), then fires OnPostLogin. 0 if one was already running or the GameMode was invalid.
int32_t aver_fw_begin_play(int32_t gameInstanceClass, int32_t gameModeClass) {
    if (playStateRef() != AVER_FW_PLAY_EDITOR) return 0;
    if (!validClass(gameModeClass))            return 0;

    // Set before any spawn, so every hook below observes PLAYING rather than the EDITOR it is leaving.
    playStateRef() = AVER_FW_PLAY_PLAYING;

    if (validClass(gameInstanceClass))
        gameInstanceRef() = aver_fw_spawn(gameInstanceClass, "GameInstance", nullptr, nullptr, nullptr);

    const int32_t gm = aver_fw_spawn(gameModeClass, "GameMode", nullptr, nullptr, nullptr);
    if (gm == 0) {
        if (gameInstanceRef()) { destroyActor(gameInstanceRef(), AVER_FW_END_STOP); gameInstanceRef() = 0; }
        playStateRef() = AVER_FW_PLAY_EDITOR;
        return 0;
    }
    gameModeRef() = gm;

    // A mode may legally have neither a controller nor a pawn, so an unset side is skipped.
    ClassRecord* r = rec(gameModeClass);
    const int32_t ctrl = (r && validClass(r->playerController))
                       ? aver_fw_spawn(r->playerController, "PlayerController", nullptr, nullptr, nullptr) : 0;
    const int32_t pawn = (r && validClass(r->defaultPawn))
                       ? aver_fw_spawn(r->defaultPawn, "Pawn", nullptr, nullptr, nullptr) : 0;
    playerCtrlRef() = ctrl;
    playPawnRef()   = pawn;
    if (ctrl && pawn) aver_fw_possess(ctrl, pawn);
    // After possess, so OnPostLogin already sees the controller's pawn.
    if (ctrl) dispatchPostLogin(toEntity(gm), toEntity(ctrl));

    return 1;
}

// Ends the running session: OnEndPlay(STOP) and destroys the four roots plus every actor the session
// spawned, then returns to EDITOR. 0 if nothing was running.
int32_t aver_fw_end_play(void) {
    if (playStateRef() == AVER_FW_PLAY_EDITOR) return 0;

    // Reverse of spawn order, so OnEndPlay mirrors OnBeginPlay for the roots.
    if (playerCtrlRef()) aver_fw_unpossess(playerCtrlRef());
    if (playPawnRef())     destroyActor(playPawnRef(),     AVER_FW_END_STOP);
    if (playerCtrlRef())   destroyActor(playerCtrlRef(),   AVER_FW_END_STOP);
    if (gameModeRef())     destroyActor(gameModeRef(),     AVER_FW_END_STOP);
    if (gameInstanceRef()) destroyActor(gameInstanceRef(), AVER_FW_END_STOP);
    playPawnRef() = playerCtrlRef() = gameModeRef() = gameInstanceRef() = 0;

    // Then sweep every other actor the session spawned from a hook. Destroy is deferred to flush, so
    // the dense walk does not shift under us.
    World& w = world();
    const u32 count = w.count();
    for (u32 i = 0; i < count; ++i) {
        const Entity e = w.at(i);
        if (classOfEntity(e) != 0 && !w.destroyPending(e))
            destroyActor(static_cast<int32_t>(e), AVER_FW_END_STOP);
    }

    playStateRef() = AVER_FW_PLAY_EDITOR;
    return 1;
}

// Freezes or resumes the tick without tearing the session down. 0 outside a running session.
int32_t aver_fw_set_paused(int32_t paused) {
    int32_t& s = playStateRef();
    if (s == AVER_FW_PLAY_EDITOR) return 0;
    s = paused ? AVER_FW_PLAY_PAUSED : AVER_FW_PLAY_PLAYING;
    return 1;
}

// The session's GameInstance entity, 0 in EDITOR.
int32_t aver_fw_game_instance(void)             { return gameInstanceRef(); }
// The session's GameMode entity, 0 in EDITOR.
int32_t aver_fw_game_mode(void)                 { return gameModeRef(); }
// The player controller for index `i`; only player 0 exists.
int32_t aver_fw_player_controller(int32_t i)    { return i == 0 ? playerCtrlRef() : 0; }
// The current AVER_FW_PLAY_* state.
int32_t aver_fw_play_state(void)                { return playStateRef(); }

// Per-process input the app pushes each frame. cur/prev give edge detection. Frame thread only.
//
// curVk/prevVk and prevMouse are ADDITIVE to the original cur/prev/mouse trio -- see framework_abi.h's
// own RAW WIN32 VK and NAMED ACTIONS sections for why each exists. Both new arrays are rolled by the
// SAME aver_fw_input_new_frame below, so there is still exactly one "once a frame" tick for every
// flavour of input state in this file, not a second one a caller could forget to call.
struct InputState {
    unsigned char cur[AVER_FW_KEY_COUNT]  = {};
    unsigned char prev[AVER_FW_KEY_COUNT] = {};
    float         mouse[3]                = {0, 0, 0};   // dx, dy, wheel
    float         prevMouse[3]            = {0, 0, 0};   // last frame's dx, dy, wheel -- see
                                                           // NAMED ACTIONS below for why a mouse-
                                                           // sourced action needs this to detect an
                                                           // edge, the same thing cur/prev give a key
    unsigned char curVk[AVER_FW_VK_COUNT]  = {};          // raw Win32 VK twin, framework_abi.h's own
    unsigned char prevVk[AVER_FW_VK_COUNT] = {};          // RAW WIN32 VK section explains why
};
// The process-wide input state.
InputState& inputState() { static InputState s; return s; }

// Rolls current key state into previous and clears the mouse deltas. Call once a frame.
void aver_fw_input_new_frame(void) {
    InputState& s = inputState();
    std::memcpy(s.prev, s.cur, sizeof(s.cur));
    std::memcpy(s.prevVk, s.curVk, sizeof(s.curVk));
    // prevMouse snapshots THIS (about-to-be-cleared) frame's delta, not last frame's -- order
    // matters here. It has to run BEFORE the zero three lines down, or a mouse-sourced action would
    // always read its own previous frame as "nothing moved", and WasPressed/WasReleased on a mouse
    // binding would never fire.
    s.prevMouse[0] = s.mouse[0]; s.prevMouse[1] = s.mouse[1]; s.prevMouse[2] = s.mouse[2];
    s.mouse[0] = s.mouse[1] = s.mouse[2] = 0.0f;
}
// Sets the held state of a key. Out-of-range keys are ignored.
void aver_fw_input_set_key(int32_t key, int32_t down) {
    if (key < 0 || key >= AVER_FW_KEY_COUNT) return;
    inputState().cur[key] = down ? 1 : 0;
}
// Sets this frame's mouse delta and wheel notches.
void aver_fw_input_set_mouse(float dx, float dy, float wheel) {
    InputState& s = inputState();
    s.mouse[0] = dx; s.mouse[1] = dy; s.mouse[2] = wheel;
}
// 1 while the key is held.
int32_t aver_fw_input_key(int32_t key) {
    return (key >= 0 && key < AVER_FW_KEY_COUNT) ? inputState().cur[key] : 0;
}
// 1 on the frame the key went down.
int32_t aver_fw_input_key_pressed(int32_t key) {
    if (key < 0 || key >= AVER_FW_KEY_COUNT) return 0;
    const InputState& s = inputState();
    return (s.cur[key] && !s.prev[key]) ? 1 : 0;
}
// 1 on the frame the key went up.
int32_t aver_fw_input_key_released(int32_t key) {
    if (key < 0 || key >= AVER_FW_KEY_COUNT) return 0;
    const InputState& s = inputState();
    return (!s.cur[key] && s.prev[key]) ? 1 : 0;
}
// Writes {dx, dy, wheel} into out3.
void aver_fw_input_mouse(float* out3) {
    if (!out3) return;
    const InputState& s = inputState();
    out3[0] = s.mouse[0]; out3[1] = s.mouse[1]; out3[2] = s.mouse[2];
}

// ---- RAW WIN32 VK, ADDITIVE TWIN TO AVER_FW_KEY_* ----------------------------------------------
// See framework_abi.h's own RAW WIN32 VK section for why this exists instead of growing the named
// enum. Mechanically identical to the cur/prev pair above, just indexed by vk instead of by
// AVER_FW_KEY_*, and backed by the SAME InputState (curVk/prevVk), rolled by the SAME new_frame.

// Sets the held state of a raw Win32 VK. Out-of-range is ignored.
void aver_fw_input_set_vk(int32_t vk, int32_t down) {
    if (vk < 0 || vk >= AVER_FW_VK_COUNT) return;
    inputState().curVk[vk] = down ? 1 : 0;
}
// 1 while the raw VK is held.
int32_t aver_fw_input_vk(int32_t vk) {
    return (vk >= 0 && vk < AVER_FW_VK_COUNT) ? inputState().curVk[vk] : 0;
}
// 1 on the frame the raw VK went down.
int32_t aver_fw_input_vk_pressed(int32_t vk) {
    if (vk < 0 || vk >= AVER_FW_VK_COUNT) return 0;
    const InputState& s = inputState();
    return (s.curVk[vk] && !s.prevVk[vk]) ? 1 : 0;
}
// 1 on the frame the raw VK went up.
int32_t aver_fw_input_vk_released(int32_t vk) {
    if (vk < 0 || vk >= AVER_FW_VK_COUNT) return 0;
    const InputState& s = inputState();
    return (!s.curVk[vk] && s.prevVk[vk]) ? 1 : 0;
}

// ---- GAMEPAD, SHAPE ONLY -- NO POLLING ----------------------------------------------------------
// See framework_abi.h's own GAMEPAD section for why this stops at "state in, state out" and does not
// open XInput itself: zero consumers today, and a real poller's hotplug/dead-zone/rumble cost is not
// something to take on speculatively.

// Gamepad button/axis state. No cur/prev pair -- unlike keys, nothing above asks for gamepad edge
// detection (aver_fw_input_gamepad_button is a plain level read), so there is nothing to roll and
// aver_fw_input_new_frame does not touch this struct at all.
struct GamepadState {
    unsigned char buttons[AVER_FW_GAMEPAD_BUTTON_COUNT] = {};
    float         axes[AVER_FW_GAMEPAD_AXIS_COUNT]      = {};
};
// The process-wide gamepad state. Single pad (index 0) -- see aver_fw_input_set_gamepad_button's own
// comment for why every function here rejects any other `pad` value.
GamepadState& gamepadState() { static GamepadState g; return g; }

// Sets one button's held state. `pad` must be 0; an out-of-range pad or button is ignored.
void aver_fw_input_set_gamepad_button(int32_t pad, int32_t button, int32_t down) {
    if (pad != 0 || button < 0 || button >= AVER_FW_GAMEPAD_BUTTON_COUNT) return;
    gamepadState().buttons[button] = down ? 1 : 0;
}
// Sets one axis's value. Unclamped -- no dead zone lives in this ABI.
void aver_fw_input_set_gamepad_axis(int32_t pad, int32_t axis, float value) {
    if (pad != 0 || axis < 0 || axis >= AVER_FW_GAMEPAD_AXIS_COUNT) return;
    gamepadState().axes[axis] = value;
}
// 1 while the button is held. 0 for `pad` != 0 or an out-of-range button.
int32_t aver_fw_input_gamepad_button(int32_t pad, int32_t button) {
    if (pad != 0 || button < 0 || button >= AVER_FW_GAMEPAD_BUTTON_COUNT) return 0;
    return gamepadState().buttons[button];
}
// The axis's last-set value. 0.0 for `pad` != 0 or an out-of-range axis.
float aver_fw_input_gamepad_axis(int32_t pad, int32_t axis) {
    if (pad != 0 || axis < 0 || axis >= AVER_FW_GAMEPAD_AXIS_COUNT) return 0.0f;
    return gamepadState().axes[axis];
}

// ---- NAMED ACTIONS (Enhanced Input) -------------------------------------------------------------
// Ports scripting/csharp/Aver.Framework/EnhancedInput.cs's algorithm onto this ABI -- see
// framework_abi.h's own NAMED ACTIONS section for why the algorithm moved and what changed in the
// move (no context handle; no separate per-frame Update()).

// One declared action. Unlike EnhancedInput.cs's InputAction, `Raw`/`Prev` are NOT stored here --
// see the header comment: both are recomputed on demand from InputState's cur/prev/mouse/prevMouse
// on every query, so an action can never read a different answer than aver_fw_input_key does for the
// identical key. Only the declaration itself needs to persist: `name` for find-by-name, `valueType`
// kept for a future consumer that wants to know an action's declared shape (nothing enforces it
// today, matching EnhancedInput.cs's own ValueType, which is documentation, not a gate, there too).
struct ActionDef {
    std::string name;
    int32_t     valueType;
};
std::vector<ActionDef>& actionDefs() { static std::vector<ActionDef> v; return v; }

// One binding: an action, a source, and (for AVER_FW_ACTION_SRC_KEY) the framework key it reads.
// `priority` is this binding's context tier -- see aver_fw_action_bind's own header comment for why
// a plain int stands in for EnhancedInput.cs's InputMappingContext object.
struct ActionBinding {
    int32_t action;
    int32_t source;
    int32_t key;
    float   scale;
    int32_t component;
    int32_t priority;
};
std::vector<ActionBinding>& actionBindings() { static std::vector<ActionBinding> v; return v; }

// True when a STRICTLY HIGHER priority binding also reads this key -- EnhancedInput.cs's own
// Update() `consumed.Contains(b.Key)` check, generalized from "layer order in a priority-sorted
// list" to "priority number compared directly", because this ABI has no layer object to sort (see
// the header's own opening comment on the NAMED ACTIONS section). Bindings at the SAME priority
// never block each other, matching EnhancedInput.cs's own comment there: "Consumption is per layer,
// so two bindings in one context can share a key."
bool actionKeyConsumedByHigherPriority(int32_t key, int32_t priority) {
    const std::vector<ActionBinding>& bindings = actionBindings();
    for (usize i = 0; i < bindings.size(); ++i) {
        const ActionBinding& b = bindings[i];
        if (b.source == AVER_FW_ACTION_SRC_KEY && b.key == key && b.priority > priority) return true;
    }
    return false;
}

// Sums every binding of `action` into out[3], reading CURRENT input state when useCurrent, or the
// PREVIOUS frame's when not -- the two evaluations aver_fw_action_pressed/_released compare, exactly
// as EnhancedInput.cs's Update() compares this frame's freshly-recomputed Raw against the Prev it
// snapshotted a moment before recomputing.
void actionAccumulate(int32_t action, bool useCurrent, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    const InputState& s = inputState();
    const std::vector<ActionBinding>& bindings = actionBindings();
    for (usize i = 0; i < bindings.size(); ++i) {
        const ActionBinding& b = bindings[i];
        if (b.action != action) continue;

        float v = 0.0f;
        switch (b.source) {
            case AVER_FW_ACTION_SRC_KEY:
                // A key consumed by a higher-priority binding contributes nothing to THIS action,
                // whether or not that higher binding's own action happens to be active right now --
                // EnhancedInput.cs consumes by KEY, not by whether the consumer read a nonzero value.
                if (!actionKeyConsumedByHigherPriority(b.key, b.priority)) {
                    const bool down = useCurrent ? s.cur[b.key] != 0 : s.prev[b.key] != 0;
                    v = down ? b.scale : 0.0f;
                }
                break;
            case AVER_FW_ACTION_SRC_MOUSE_X:
                v = (useCurrent ? s.mouse[0] : s.prevMouse[0]) * b.scale;
                break;
            case AVER_FW_ACTION_SRC_MOUSE_Y:
                v = (useCurrent ? s.mouse[1] : s.prevMouse[1]) * b.scale;
                break;
            case AVER_FW_ACTION_SRC_MOUSE_WHEEL:
                v = (useCurrent ? s.mouse[2] : s.prevMouse[2]) * b.scale;
                break;
            default:
                break;
        }

        // 0=X, 1=Y, anything else=Z -- EnhancedInput.cs's own Accumulate() switch, verbatim.
        if (b.component == 0)      out[0] += v;
        else if (b.component == 1) out[1] += v;
        else                        out[2] += v;
    }
}

// True when any channel exceeds the dead zone -- EnhancedInput.cs's own Active(), pinned at 0.15
// (see framework_abi.h's own comment for why this is not a parameter).
bool actionActive(const float v[3]) {
    const float kDeadZone = 0.15f;
    return std::fabs(v[0]) > kDeadZone || std::fabs(v[1]) > kDeadZone || std::fabs(v[2]) > kDeadZone;
}

// A handle is valid iff it was handed back by aver_fw_action_register: 1-based, never past the
// number of declarations made so far. 0 (and anything negative) is always invalid.
bool actionHandleValid(int32_t action) {
    return action >= 1 && static_cast<usize>(action) <= actionDefs().size();
}

// The handle for a previously registered action name, or 0.
int32_t aver_fw_action_find(const char* name) {
    if (!name || !*name) return 0;
    const std::vector<ActionDef>& defs = actionDefs();
    for (usize i = 0; i < defs.size(); ++i)
        if (defs[i].name == name) return static_cast<int32_t>(i + 1);
    return 0;
}

// Declares a named action, idempotent by name -- see framework_abi.h's own comment for why (the same
// reason aver_fw_class_declare is idempotent: a script re-registering on every possession must not
// multiply the action).
int32_t aver_fw_action_register(const char* name, int32_t valueType) {
    if (!name || !*name) return 0;
    if (valueType != AVER_FW_ACTION_DIGITAL && valueType != AVER_FW_ACTION_AXIS1D &&
        valueType != AVER_FW_ACTION_AXIS2D) return 0;
    const int32_t existing = aver_fw_action_find(name);
    if (existing) return existing;
    std::vector<ActionDef>& defs = actionDefs();
    defs.push_back(ActionDef{name, valueType});
    return static_cast<int32_t>(defs.size());   // 1-based handle, 0 stays invalid
}

// Adds one binding. See framework_abi.h's own comment for every parameter's meaning.
void aver_fw_action_bind(int32_t action, int32_t source, int32_t key, float scale,
                         int32_t component, int32_t contextPriority) {
    if (!actionHandleValid(action)) return;
    if (source < AVER_FW_ACTION_SRC_KEY || source > AVER_FW_ACTION_SRC_MOUSE_WHEEL) return;
    if (source == AVER_FW_ACTION_SRC_KEY && (key < 0 || key >= AVER_FW_KEY_COUNT)) return;
    actionBindings().push_back(ActionBinding{action, source, key, scale, component, contextPriority});
}

// Drops every binding. Registrations survive -- see framework_abi.h's own comment.
void aver_fw_action_clear_bindings(void) { actionBindings().clear(); }

// Writes {X, Y} of the action's current value. {0, 0} for an invalid handle.
void aver_fw_action_value2(int32_t action, float* out2) {
    if (!out2) return;
    if (!actionHandleValid(action)) { out2[0] = out2[1] = 0.0f; return; }
    float raw[3];
    actionAccumulate(action, /*useCurrent=*/true, raw);
    out2[0] = raw[0]; out2[1] = raw[1];
}

// 1 while the action is active.
int32_t aver_fw_action_held(int32_t action) {
    if (!actionHandleValid(action)) return 0;
    float raw[3];
    actionAccumulate(action, true, raw);
    return actionActive(raw) ? 1 : 0;
}

// 1 on the frame the action became active.
int32_t aver_fw_action_pressed(int32_t action) {
    if (!actionHandleValid(action)) return 0;
    float raw[3], prev[3];
    actionAccumulate(action, true, raw);
    actionAccumulate(action, false, prev);
    return (actionActive(raw) && !actionActive(prev)) ? 1 : 0;
}

// 1 on the frame the action stopped being active.
int32_t aver_fw_action_released(int32_t action) {
    if (!actionHandleValid(action)) return 0;
    float raw[3], prev[3];
    actionAccumulate(action, true, raw);
    actionAccumulate(action, false, prev);
    return (!actionActive(raw) && actionActive(prev)) ? 1 : 0;
}

// The camera a possessed character asks for.
struct ViewRequest {
    int32_t mode = AVER_FW_VIEW_THIRD_PERSON; float eye = 160.0f; float boom = 450.0f;
    int32_t entity = 0;   // 0 until a character publishes one
};
// The process-wide view request.
ViewRequest& viewRequest() { static ViewRequest v; return v; }

// Publishes the wanted view mode and its eye/boom offsets.
void aver_fw_set_view(int32_t mode, float eyeHeight, float boomLength) {
    ViewRequest& v = viewRequest();
    v.mode = mode; v.eye = eyeHeight; v.boom = boomLength;
}
// Reads the published view mode and offsets.
void aver_fw_view(int32_t* outMode, float* outEyeHeight, float* outBoomLength) {
    const ViewRequest& v = viewRequest();
    if (outMode)       *outMode       = v.mode;
    if (outEyeHeight)  *outEyeHeight  = v.eye;
    if (outBoomLength) *outBoomLength = v.boom;
}

// Publishes the scene node the camera sits on.
void    aver_fw_set_view_entity(int32_t entity) { viewRequest().entity = entity; }
// The published view entity, or 0.
int32_t aver_fw_view_entity(void)               { return viewRequest().entity; }

// ---- the sky's cloud layer, published by a script -------------------------------------------
//
// SAME SHAPE AS ViewRequest, and here for the same reason: it is app-facing state a script wants to
// set and the host reads once a frame. There is no other route -- nothing under modules/scripting
// or modules/framework mentions PCG at all, so a project's sky could only ever be numbers typed
// into its .ocworld.
//
// A REQUEST, NOT THE TRUTH. `set` is what the host applies; until a script calls it, `has` is 0 and
// the host keeps whatever the level authored. That ordering matters: a project with no sky script
// must render exactly as it did before this existed.
struct SkyRequest {
    int32_t has = 0;
    int32_t seed = 0;
    float coverage = 0.45f;
    float density  = 1.0f;
    float bottom   = 150000.0f;
    float top      = 280000.0f;
    float scale    = 0.00002f;
    float windX    = 900.0f;
    float windY    = 260.0f;
};
SkyRequest& skyRequest() { static SkyRequest s; return s; }

// Publishes the cloud layer a script wants. Any later call replaces the whole request.
void aver_fw_set_sky_clouds(int32_t seed, float coverage, float density,
                            float bottomCm, float topCm, float featureScale,
                            float windXCmPerSec, float windYCmPerSec) {
    SkyRequest& s = skyRequest();
    s.has = 1;
    s.seed = seed;
    // Clamped here rather than trusted: this is the one point a script's arithmetic reaches the
    // renderer, and a NaN coverage from a bad F# expression would take the whole cloud layer with
    // it. `top <= bottom` is left to the device, which already substitutes bottom + 1.
    s.coverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
    s.density  = density  < 0.0f ? 0.0f : density;
    s.bottom   = bottomCm;
    s.top      = topCm;
    s.scale    = featureScale > 0.0f ? featureScale : 0.00002f;
    s.windX    = windXCmPerSec;
    s.windY    = windYCmPerSec;
}

// Reads the published cloud layer. Returns 1 when a script has published one, 0 otherwise, and
// writes nothing through the out pointers when it returns 0.
int32_t aver_fw_sky_clouds(int32_t* outSeed, float* outCoverage, float* outDensity,
                           float* outBottomCm, float* outTopCm, float* outFeatureScale,
                           float* outWindX, float* outWindY) {
    const SkyRequest& s = skyRequest();
    if (!s.has) return 0;
    if (outSeed)         *outSeed         = s.seed;
    if (outCoverage)     *outCoverage     = s.coverage;
    if (outDensity)      *outDensity      = s.density;
    if (outBottomCm)     *outBottomCm     = s.bottom;
    if (outTopCm)        *outTopCm        = s.top;
    if (outFeatureScale) *outFeatureScale = s.scale;
    if (outWindX)        *outWindX        = s.windX;
    if (outWindY)        *outWindY        = s.windY;
    return 1;
}

// Drops the request, so the level's own sky takes over again. Called when a level unloads, or a
// script can call it to hand the sky back.
void aver_fw_clear_sky_clouds(void) { skyRequest() = SkyRequest{}; }

// ---- FLUID VOLUME SPAWN, RELAYED ---------------------------------------------------------------
// Same shape as g_saveWrite/g_animCurve above, for the same reason: only a composition root links
// both Aver.Framework and Aver.Fluids. NOT ATOMIC, matching every other provider in this file.
aver_fw_fluid_spawn_fn g_fluidSpawn = nullptr;
void* g_fluidSpawnUser = nullptr;

int32_t aver_fw_set_fluid_spawn_provider(aver_fw_fluid_spawn_fn fn, void* user) {
    g_fluidSpawn = fn;
    g_fluidSpawnUser = user;
    return 1;
}

int32_t aver_fw_fluid_spawn(float cx, float cy, float cz, float hx, float hy, float hz,
                            float compliance, float damping, int32_t iterations, float pressure,
                            const char* name) {
    if (!g_fluidSpawn) return 0;
    return g_fluidSpawn(cx, cy, cz, hx, hy, hz, compliance, damping, iterations, pressure,
                        name && *name ? name : "unnamed", g_fluidSpawnUser);
}

// ---- FLUID VOLUME SPAWN, WITH A MATERIAL -------------------------------------------------------
// A second, independent provider slot beside g_fluidSpawn above -- see framework_abi.h's own MINOR
// 4 changelog entry for why this is additive rather than a change to the pair above. Same
// not-atomic caveat as every other provider in this file.
aver_fw_fluid_spawn_material_fn g_fluidSpawnMaterial = nullptr;
void* g_fluidSpawnMaterialUser = nullptr;

int32_t aver_fw_set_fluid_spawn_material_provider(aver_fw_fluid_spawn_material_fn fn, void* user) {
    g_fluidSpawnMaterial = fn;
    g_fluidSpawnMaterialUser = user;
    return 1;
}

int32_t aver_fw_fluid_spawn_material(float cx, float cy, float cz, float hx, float hy, float hz,
                                     float compliance, float damping, int32_t iterations,
                                     float pressure, float densityKgM3, float viscosityPaS,
                                     const char* materialPreset, const char* name) {
    if (!g_fluidSpawnMaterial) return 0;
    return g_fluidSpawnMaterial(cx, cy, cz, hx, hy, hz, compliance, damping, iterations, pressure,
                                densityKgM3, viscosityPaS, materialPreset ? materialPreset : "",
                                name && *name ? name : "unnamed", g_fluidSpawnMaterialUser);
}

}  // extern "C"
