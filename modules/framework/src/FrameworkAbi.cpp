#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"

#include "aver/core/Log.hpp"
#include "aver/scene/scene_abi.h"

// The C ABI's translation unit. Every EXPORT here is `extern "C"` and holds no C++ in its signature,
// so this stays a stable binary surface as the registry behind it grows. Unlike a header-only shim it
// includes the C++ World: void* and std types cross only WITHIN this DLL, between this TU and the scene
// it was compiled against — never across the C ABI, exactly as SceneAbi.cpp does it one module down.
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

// ================================================================================================
// The class registry lives entirely in this module. The scene is deliberately gameplay-agnostic
// (scene_abi.h forbids the words actor/pawn/spawn), so "class" cannot be a scene concept and the
// framework owns every byte of it. The arrow points DOWN: this file calls World, never the reverse.
// ================================================================================================

namespace {

World& world() { return World::instance(); }

// The eight built-in component structs default to values that are NOT zero and are load-bearing:
// CLocal.scale is {1,1,1}, CMeshRenderer.flags has bit0 (visible) and dirty is 1, CLight is bright.
// addComponent() zero-fills, so an archetype that seeded its blob with zero would spawn invisible or
// zero-scaled actors. The framework co-compiles with Components.hpp, so it can seed each component's
// blob with the REAL default struct; a script-declared component (id > 8) has no struct here and falls
// back to zero, which matches exactly what the scene's addComponent would have written anyway.
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
        default: break;   // script-declared component: zero, as addComponent would leave it
    }
}

// A flattened component list plus one contiguous blob of default bytes with a per-component offset.
// Spawning is a loop of memcpy over this. `strDefaults` holds String-kind defaults out of band — see
// the note in aver_fw_class_set_default_str for why a String default cannot ride the blob.
struct Archetype {
    std::vector<u32> components;                 // scene component type ids, CLocal first
    std::vector<u32> offset;                     // parallel to components, into blob
    std::vector<u8>  blob;
    std::unordered_map<int32_t, std::string> strDefaults;   // field id -> value
    // The field ids this class actually AUTHORED a (non-String) default for. Default inheritance is
    // field-level: flatten() re-applies only these bytes over an inherited blob, so a subclass that
    // does not touch an inherited field keeps the parent's value instead of clobbering the whole
    // component with its own un-authored built-in defaults. String defaults ride strDefaults, which is
    // already merged field-by-field, so they are tracked there rather than here.
    std::vector<int32_t> authoredFields;

    void clear() {
        components.clear();
        offset.clear();
        blob.clear();
        strDefaults.clear();
        authoredFields.clear();
    }

    // Record that this class wrote a default for field `f`, de-duplicated so a class that sets the same
    // field twice does not grow the list unboundedly.
    void authorField(int32_t f) {
        for (int32_t x : authoredFields)
            if (x == f) return;
        authoredFields.push_back(f);
    }

    int indexOf(u32 comp) const {
        for (usize i = 0; i < components.size(); ++i)
            if (components[i] == comp) return static_cast<int>(i);
        return -1;
    }

    // Append a component's storage seeded with its real default, unless it is already present.
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

struct ClassRecord {
    std::string name;
    u64         nameHash = 0;
    std::string parentName;                    // "" for a root; resolved by name (see below)
    int32_t     flags     = 0;
    int32_t     tickGroup = AVER_FW_TICK_PRE_PHYSICS;
    int32_t     tickOrder = 0;
    bool        sealed    = false;

    // GameMode wiring, resolved by name at seal.
    std::string defaultPawnName;
    std::string playerControllerName;
    int32_t     defaultPawn      = 0;
    int32_t     playerController = 0;

    Archetype own;       // this class's own declared components + defaults
    Archetype resolved;  // the sealed, parent-chain-flattened archetype

    // Reset the authorable state for a fresh (re)declare, keeping identity: name, hash and the
    // class-of records of any live instances stay put, which is the whole of hot-reload identity.
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
        // Every class carries CLocal so transform defaults (position/rotation/scale) have a home and
        // spawn always has a CLocal region to lay down — the entity is born with CLocal regardless.
        own.add(kComponentLocal);
    }
};

// A deque, not a vector: class_name() hands back a c_str() into a record, and a vector reallocating on
// the next declare would dangle every such pointer. deque never moves an element once pushed. Index 0
// is a reserved dummy so a class handle of 0 is invalid with no special case at every call site.
std::deque<ClassRecord>& classes() {
    static std::deque<ClassRecord> c = [] {
        std::deque<ClassRecord> d;
        d.emplace_back();   // handle 0 == invalid
        return d;
    }();
    return c;
}

std::unordered_map<std::string, int32_t>& classByName() {
    static std::unordered_map<std::string, int32_t> m;
    return m;
}

bool validClass(int32_t c) { return c > 0 && static_cast<usize>(c) < classes().size(); }
ClassRecord* rec(int32_t c) { return validClass(c) ? &classes()[static_cast<usize>(c)] : nullptr; }

int32_t findClass(std::string_view name) {
    auto& m  = classByName();
    auto  it = m.find(std::string(name));
    return it == m.end() ? 0 : it->second;
}

Entity toEntity(int32_t e) { return static_cast<Entity>(static_cast<uint32_t>(e)); }

// ---- class identity: a framework-owned side map, index -> (owning entity, class) ----------------
// DECISION: entity->class is a framework side map, NOT a scene component. Two reasons. (1) The scene
// forbids gameplay vocabulary; a "class" component would push a gameplay id into render/physics-
// agnostic storage and invert the dependency arrow. (2) A side map keeps 0 == invalid trivially. The
// generational hazard a raw index map would have — a destroyed actor's index reused by a plain scene
// entity — is closed by storing the OWNING entity handle beside the class: class_of returns the class
// only when the recorded owner equals the queried handle, so a reused index and a never-recorded
// scene entity both read 0, which is exactly the contract the test asserts.
struct ClassOwner {
    Entity  owner = kInvalidEntity;
    int32_t cls   = 0;
};
std::vector<ClassOwner>& classOwners() {
    static std::vector<ClassOwner> v;
    return v;
}

void recordClass(Entity e, int32_t c) {
    const u32 idx = entityIndex(e);
    auto&     v   = classOwners();
    if (idx >= v.size()) v.resize(idx + 1);
    v[idx] = {e, c};
}

int32_t classOfEntity(Entity e) {
    const u32 idx = entityIndex(e);
    auto&     v   = classOwners();
    if (idx >= v.size()) return 0;
    // Owner match closes the reused-index hazard; the valid() check closes the STALE-but-not-reused
    // one. aver_fw_destroy only forgets the single handle it is given, but world().destroy takes the
    // whole subtree — so a child destroyed via its parent (or via the scene C ABI) keeps its side-map
    // row until its index is reused. Consulting the world makes class_of read 0 the instant the handle
    // dies, matching the "0 for a stale handle" contract for every destroy path, not just the direct one.
    return (v[idx].owner == e && world().valid(e)) ? v[idx].cls : 0;
}

void forgetClass(Entity e) {
    const u32 idx = entityIndex(e);
    auto&     v   = classOwners();
    if (idx < v.size() && v[idx].owner == e) v[idx] = {kInvalidEntity, 0};
}

// The set of entities whose aver_fw_destroy is currently on the stack. A managed OnEndPlay is free to
// call Destroy(Self) on its own entity, but destroy fires endPlay BEFORE forgetClass and world().destroy
// is deferred to the next flush — so a naive re-entrant call would still read MANAGED + valid(e) and fire
// endPlay a SECOND time, and Destroy(Self) in every OnEndPlay would recurse to a stack overflow. Membership
// here means "already tearing this one down"; the re-entrant call sees it and refuses, blocking the
// double-fire while leaving the outer call to finish its single teardown. (spawn's beginPlay-last ordering
// is re-entrancy-safe for free; destroy fires endPlay early and needs this guard to match.)
std::unordered_set<Entity>& destroyInFlight() {
    static std::unordered_set<Entity> s;
    return s;
}

// ---- possession maps (read straight from the world; never a source of truth about liveness) ------
std::unordered_map<Entity, Entity>& pawnByController() {
    static std::unordered_map<Entity, Entity> m;
    return m;
}
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

// ---- managed dispatch (step 10) -----------------------------------------------------------------
// The framework calls UP into managed gameplay code through ONE table, installed by value from
// outside (the executable, right after the script host bootstraps) so no CLR-owned pointer is ever
// held past an ALC unload — see the dangle argument on AvManagedDispatch. The framework links
// Core/Assets/Scene only and never learns a CLR exists: this table is the whole of the seam.
//
// Stored BY VALUE, plus a separate `installed` flag rather than sniffing a member for null: the flag
// is what the second-install refusal tests, and it lets a legitimately all-null table (were one ever
// installed) still count as "live". Clear zeroes both, which is the NULL store the call-site guards
// already handle — a tick after a clear ticks nothing rather than faulting.
AvManagedDispatch& managedDispatch() { static AvManagedDispatch d{}; return d; }
bool& managedInstalled()             { static bool b = false; return b; }

// Seal helper: flatten the parent chain root-first into `out.resolved`. Returns false on a cycle or a
// named-but-undeclared parent, writing nothing. Default inheritance is FIELD-LEVEL: each resolved
// component is seeded ONCE from its built-in default (by add()), then every class's authored fields are
// re-applied root->leaf so the leaf wins per FIELD, not per whole component. This is why a subclass
// that adds a component but authors none of its fields keeps the parent's authored defaults instead of
// clobbering them with its own un-authored built-in values — the CLocal transform channel, auto-added
// to every class, is the case that made a whole-component memcpy silently drop an inherited scale.
bool flatten(int32_t c, Archetype& outResolved) {
    // Walk leaf->root, detecting a revisit (cycle) and a missing parent.
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
    // The resolved blob already holds every component's built-in default (add() seeds each on insert).
    // Re-apply only the AUTHORED field bytes root->leaf, so an inherited field a subclass never touched
    // survives and a field the subclass did author overrides just that field. This is the fix for the
    // whole-component clobber that used to lose a parent's CLocal position/rotation/scale on every
    // subclass (CLocal is auto-added to all of them, so its built-in default would otherwise win).
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

bool sealClass(int32_t c) {
    ClassRecord* r = rec(c);
    if (!r) return false;
    if (!flatten(c, r->resolved)) return false;
    // Resolve GameMode wiring by name now (declaration order is free). Absent names stay 0.
    r->defaultPawn      = r->defaultPawnName.empty()      ? 0 : findClass(r->defaultPawnName);
    r->playerController = r->playerControllerName.empty() ? 0 : findClass(r->playerControllerName);
    r->sealed = true;
    return true;
}

// The shared body of the five typed default setters: resolve the field, kind-check it, and memcpy the
// value into the class's own blob at the field's offset. A wrong kind, an unknown field, a read-only
// field, or a component the class has not added is rejected with 0 and writes nothing.
bool setDefaultBytes(int32_t c, int32_t f, FieldKind wantKind, const void* src, usize sz) {
    ClassRecord* r = rec(c);
    if (!r) return false;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return false;
    if (d->kind != wantKind) return false;
    u8* addr = r->own.fieldAddr(d);
    if (!addr) return false;               // the class never added this component
    std::memcpy(addr, src, sz);
    r->own.authorField(f);                 // remember it so flatten re-applies it field-level
    r->sealed = false;                     // a new default makes any prior seal stale
    return true;
}

}  // namespace

// ================================================================================================
extern "C" {

int32_t aver_fw_abi_version(void) {
    return AVER_FW_ABI_VERSION;
}

int32_t aver_fw_scene_abi_version(void) {
    // The value from the HEADER this DLL compiled against, deliberately not a call to
    // aver_scene_abi_version(). Calling would report whichever scene DLL happens to be loaded,
    // which is the thing the caller is trying to detect a mismatch with.
    return AVER_SCENE_ABI_VERSION;
}

int32_t aver_fw_scene_abi_matches(void) {
    // The one call in this module that crosses into Aver.Scene, and therefore the one thing making
    // the framework genuinely import from it. Compare MAJOR only: a newer minor is additive by the
    // contract scene_abi.h states, so refusing it would reject a scene DLL that is fine.
    const int32_t loaded = aver_scene_abi_version() >> 16;
    return loaded == AVER_SCENE_ABI_VERSION_MAJOR ? 1 : 0;
}

// ---- class registry ----------------------------------------------------------------------------

int32_t aver_fw_class_declare(const char* name, const char* parentName) {
    if (!name || !*name) return 0;
    const std::string n  = name;
    const std::string pn = (parentName && *parentName) ? parentName : std::string();

    // IDEMPOTENT BY NAME: an existing name returns its handle and rewrites the row in place. The
    // handle and the class-of records of any live instances survive — that is hot-reload identity.
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

int32_t aver_fw_class_find(const char* name) {
    return (name && *name) ? findClass(name) : 0;
}

const char* aver_fw_class_name(int32_t c) {
    const ClassRecord* r = rec(c);
    return r ? r->name.c_str() : "";
}

int32_t aver_fw_class_parent(int32_t c) {
    const ClassRecord* r = rec(c);
    return (r && !r->parentName.empty()) ? findClass(r->parentName) : 0;
}

int32_t aver_fw_class_reset(int32_t c) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const std::string keepParent = r->parentName;   // reset clears components/defaults, not lineage
    r->resetForDeclare();
    r->parentName = keepParent;
    return 1;
}

int32_t aver_fw_class_add_component(int32_t c, int32_t component) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const u32 type = static_cast<u32>(component);
    if (type == 0 || world().pool(type) == nullptr) return 0;   // not a registered component
    r->own.add(type);
    r->sealed = false;
    return 1;
}

int32_t aver_fw_class_set_flags(int32_t c, int32_t flags) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    r->flags = flags;
    return 1;
}

int32_t aver_fw_class_get_flags(int32_t c) {
    const ClassRecord* r = rec(c);
    return r ? r->flags : 0;
}

int32_t aver_fw_class_set_tick(int32_t c, int32_t tickGroup, int32_t tickOrder) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    if (tickGroup < 0 || tickGroup >= AVER_FW_TICK_COUNT) return 0;
    r->tickGroup = tickGroup;
    r->tickOrder = tickOrder;
    return 1;
}

int32_t aver_fw_class_seal(int32_t c) {
    return sealClass(c) ? 1 : 0;
}

// ---- class defaults ----------------------------------------------------------------------------

int32_t aver_fw_class_set_default_f32(int32_t c, int32_t f, float v) {
    return setDefaultBytes(c, f, FieldKind::F32, &v, sizeof v) ? 1 : 0;
}

int32_t aver_fw_class_set_default_i32(int32_t c, int32_t f, int32_t v) {
    // A bool rides an i32 (there is no set_default_bool), so both kinds accept this write — matching
    // the scene's set_i32 family, whose 4-byte slot is identical for I32 and Bool.
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

int32_t aver_fw_class_set_default_i64(int32_t c, int32_t f, int64_t v) {
    return setDefaultBytes(c, f, FieldKind::I64, &v, sizeof v) ? 1 : 0;
}

int32_t aver_fw_class_set_default_vec(int32_t c, int32_t f, const float* v) {
    if (!v) return 0;
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return 0;
    // Any float kind: F32=1, Vec3=3, Quat=4, Mat4=16 floats. The buffer must hold `arity` floats.
    const u8 arity = canonicalArity(d->kind);
    if (arity == 0) return 0;              // not a vector/float kind (e.g. I32, I64, String)
    u8* addr = r->own.fieldAddr(d);
    if (!addr) return 0;
    std::memcpy(addr, v, static_cast<usize>(arity) * sizeof(float));
    r->own.authorField(f);
    r->sealed = false;
    return 1;
}

int32_t aver_fw_class_set_default_str(int32_t c, int32_t f, const char* v) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    const FieldDesc* d = world().field(static_cast<u32>(f));
    if (!d || d->readOnly) return 0;
    // Kind-checked like every other setter: a String default into CMeshRenderer.mesh (I64) or
    // .material (I32) is REJECTED here — the ABI's kind check is the resolution of that contradiction,
    // so the C# builder must resolve a path to an ObjectId and write set_default_i64 instead.
    if (d->kind != FieldKind::String) return 0;
    // A String field's 8 bytes are an intern id into the SCENE's private string pool, which this
    // module cannot reach (it is file-local to SceneAbi.cpp with no public "intern a default" call).
    // So a String default is held out of band and re-applied at spawn via aver_scene_set_str, which
    // interns into the live entity correctly. No built-in field is String-kind, so this path is
    // dormant today; see the report note for the follow-up the scene owes.
    r->own.strDefaults[f] = v ? v : "";
    r->sealed = false;
    return 1;
}

int32_t aver_fw_class_set_default_pawn(int32_t gameMode, const char* pawnClassName) {
    ClassRecord* r = rec(gameMode);
    if (!r) return 0;
    r->defaultPawnName = (pawnClassName && *pawnClassName) ? pawnClassName : "";
    r->sealed = false;
    return 1;
}

int32_t aver_fw_class_set_player_controller(int32_t gameMode, const char* controllerClassName) {
    ClassRecord* r = rec(gameMode);
    if (!r) return 0;
    r->playerControllerName = (controllerClassName && *controllerClassName) ? controllerClassName : "";
    r->sealed = false;
    return 1;
}

// ---- actors ------------------------------------------------------------------------------------

int32_t aver_fw_spawn(int32_t c, const char* name,
                      const float* pos3, const float* quat4, const float* scale3) {
    ClassRecord* r = rec(c);
    if (!r) return 0;
    if (!r->sealed && !sealClass(c)) return 0;   // spawning auto-seals; a bad chain fails here

    const Entity e = world().create(name ? std::string_view(name) : std::string_view());
    if (e == kInvalidEntity) return 0;

    // Attach every archetype component and memcpy its resolved default bytes over it. The four birth
    // components are already correct on a fresh entity; addComponent is idempotent and returns their
    // existing storage, so laying CLocal's blob down over it is a no-op on top of a no-op.
    const Archetype& a = r->resolved;
    for (usize i = 0; i < a.components.size(); ++i) {
        const u32 comp = a.components[i];
        void*     base = world().addComponent(e, comp);
        if (!base) continue;
        std::memcpy(base, a.blob.data() + a.offset[i], world().componentSize(comp));
    }
    // String defaults ride the scene's own intern path rather than the memcpy'd blob (see set_default_str).
    for (const auto& kv : a.strDefaults) aver_scene_set_str(e, kv.first, kv.second.c_str());

    // Transform overrides. A null channel keeps the class default already laid down in CLocal above.
    // Rotation crosses as a quaternion (x,y,z,w) — the author's degrees were converted higher up.
    if (CLocal* loc = static_cast<CLocal*>(world().getComponent(e, kComponentLocal))) {
        if (pos3)   loc->xf.position = {pos3[0], pos3[1], pos3[2]};
        if (quat4)  loc->xf.rotation = {quat4[0], quat4[1], quat4[2], quat4[3]};
        if (scale3) loc->xf.scale    = {scale3[0], scale3[1], scale3[2]};
        // The blob memcpy and the overrides both wrote CLocal through the pool directly, so bump the
        // transform revision or the world-matrix pass — a revision compare — never sees the write.
        world().touchLocal(e);
    }

    recordClass(e, c);

    // A managed actor's birth is dispatched UP into managed code here — the first instant the entity
    // exists, is fully built, and class_of(e) already resolves. Gate on the MANAGED flag so a native
    // or plain class fires nothing, and on an installed dispatch so a headless / no-CLR build spawns
    // exactly the same, just without the hook. Reason is SPAWN: a brand-new actor, distinct from the
    // PLAY/RELOAD entries a later stage adds, so the script can tell a fresh spawn from a reload.
    //
    // SCOPE (step 10 wires the BEGIN EDGE only): beginPlay is dispatched here but bind() and
    // build_models() are deliberately NOT — constructing the managed instance and its render models is
    // a step-11 job (the bridge that owns the instance list does not exist yet). Once a real bridge
    // fills the table, beginPlay must be preceded by bind(nameHash, e) (construct+bind, guarded on
    // bind != null, checking its 1==instance-exists return) and build_models(e); firing beginPlay with
    // no prior bind would call OnBeginPlay against an instance never constructed. Left as a comment, not
    // code, per this stage's boundary — the counterpart to the unbind note in aver_fw_destroy.
    if ((r->flags & AVER_FW_CLASS_MANAGED) && managedInstalled() && managedDispatch().beginPlay) {
        managedDispatch().beginPlay(static_cast<aver_entity>(e), AVER_FW_BEGIN_SPAWN);
    }

    return static_cast<int32_t>(e);
}

int32_t aver_fw_destroy(int32_t e) {
    const Entity ent = toEntity(e);
    if (!world().valid(ent)) return 0;

    // Re-entrancy guard: refuse a destroy already in progress for this entity. A managed OnEndPlay can
    // call Destroy(Self); because endPlay fires below BEFORE forgetClass and world().destroy is deferred,
    // the re-entrant call would otherwise still read MANAGED + valid and fire endPlay a second time (and
    // Destroy(Self) in every OnEndPlay would stack-overflow). See destroyInFlight for the full argument.
    auto& inFlight = destroyInFlight();
    if (inFlight.count(ent)) return 0;
    inFlight.insert(ent);

    // A managed actor's death is dispatched UP into managed code BEFORE anything is torn down, so the
    // hook still sees a live entity (world().destroy below is deferred to the next flush, so `ent` is
    // valid throughout this call regardless). Reason is DESTROY. Gate on the MANAGED flag and an
    // installed dispatch, exactly as spawn does, so a native/plain actor or a no-CLR build fires
    // nothing and still destroys cleanly. NOTE (follow-up): world().destroy takes the whole SUBTREE
    // but aver_fw_destroy is only called for the single handle given, so a child destroyed via its
    // parent gets no endPlay — closing that needs a hook on the World retire path, a Play/Stop-stage job.
    //
    // SCOPE (step 10 wires the END EDGE only): endPlay is dispatched here but unbind() is deliberately
    // NOT — dropping the managed instance from the bridge's dense list is a step-11 job (the bridge that
    // owns that list does not exist yet). Once a real bridge fills the table, endPlay firing with no
    // matching unbind would leave the destroyed entity in the bridge's instance list (leak + still
    // ticked by tick_all); wiring managedDispatch().unbind(e) here — guarded on unbind != null, after
    // endPlay — is the step-11 counterpart. Left as a comment, not code, per this stage's boundary.
    if ((classHasFlags(ent, AVER_FW_CLASS_MANAGED)) && managedInstalled() && managedDispatch().endPlay) {
        managedDispatch().endPlay(static_cast<aver_entity>(e), AVER_FW_END_DESTROY);
    }

    // Drop possession both ways and forget the class before the deferred scene destroy: the handle is
    // still valid this frame, but the framework's own maps should not outlive the request.
    if (const int32_t pawn = aver_fw_controlled_pawn(e)) {
        controllerByPawn().erase(toEntity(pawn));
    }
    // Symmetric case: when the destroyed entity is a possessed PAWN, drop its controller's FORWARD
    // entry too. Without this the pawn->controller side is cleaned (below) but pawnByController[ctrl]
    // is left dangling, breaking the mutual-inverse invariant possess() promises and leaking one entry
    // per possessed-pawn destroy. Read the controller before the erases below retire cOf[ent].
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

int32_t aver_fw_class_of(int32_t e) {
    return classOfEntity(toEntity(e));
}

// ---- possession --------------------------------------------------------------------------------

int32_t aver_fw_possess(int32_t controller, int32_t pawn) {
    const Entity ctrl = toEntity(controller);
    const Entity pwn  = toEntity(pawn);
    if (!world().valid(ctrl) || !world().valid(pwn)) return 0;
    // The flag check IS the type safety: no CONTROLLER on one side or no PAWN on the other, reject.
    if (!classHasFlags(ctrl, AVER_FW_CLASS_CONTROLLER)) return 0;
    if (!classHasFlags(pwn,  AVER_FW_CLASS_PAWN))       return 0;

    // Possession is exclusive on both ends: free this controller's previous pawn and this pawn's
    // previous controller before binding the new pair, so the two maps stay mutual inverses.
    auto& pOf = pawnByController();
    auto& cOf = controllerByPawn();
    if (auto it = pOf.find(ctrl); it != pOf.end()) cOf.erase(it->second);
    if (auto it = cOf.find(pwn);  it != cOf.end()) pOf.erase(it->second);
    pOf[ctrl] = pwn;
    cOf[pwn]  = ctrl;
    return 1;
}

int32_t aver_fw_unpossess(int32_t controller) {
    const Entity ctrl = toEntity(controller);
    auto& pOf = pawnByController();
    auto  it  = pOf.find(ctrl);
    if (it == pOf.end()) return 0;   // nothing to release
    controllerByPawn().erase(it->second);
    pOf.erase(it);
    return 1;
}

int32_t aver_fw_controlled_pawn(int32_t controller) {
    auto& pOf = pawnByController();
    auto  it  = pOf.find(toEntity(controller));
    if (it == pOf.end()) return 0;
    // Read straight from the world: a pawn that was destroyed out from under the controller reads 0.
    return world().valid(it->second) ? static_cast<int32_t>(it->second) : 0;
}

int32_t aver_fw_controller_of(int32_t pawn) {
    auto& cOf = controllerByPawn();
    auto  it  = cOf.find(toEntity(pawn));
    if (it == cOf.end()) return 0;
    return world().valid(it->second) ? static_cast<int32_t>(it->second) : 0;
}

// ---- managed dispatch: install / clear / tick (step 10) ----------------------------------------

int32_t aver_fw_install_managed_dispatch(const AvManagedDispatch* d) {
    if (!d) return 0;
    // Reject a stale or short table the way the scripting bootstrap does: a bridge built against a
    // different table shape is REPORTED here, not crashed through on the first indirect call.
    if (d->structBytes != static_cast<int32_t>(sizeof(AvManagedDispatch)) ||
        d->contractVersion != AVER_FW_DISPATCH_VERSION) {
        AVER_ERROR("aver_fw_install_managed_dispatch: rejected a dispatch table (structBytes={}, version={}; "
                   "this framework expects {} / {})",
                   d->structBytes, d->contractVersion,
                   static_cast<int32_t>(sizeof(AvManagedDispatch)), AVER_FW_DISPATCH_VERSION);
        return 0;
    }
    // Refuse a SECOND install while one is live. Only the executable may wire the bridge's entries into
    // this module, once, right after Bootstrap; nothing in the build can enforce that, so the refusal
    // is the enforcement. Log it so a second wirer is found rather than silently winning or losing.
    if (managedInstalled()) {
        AVER_ERROR("aver_fw_install_managed_dispatch: a managed dispatch is already installed; refusing a "
                   "second install (clear the first before installing another)");
        return 0;
    }
    managedDispatch() = *d;        // BY VALUE — no pointer into CLR-owned memory is retained
    managedInstalled() = true;
    return 1;
}

int32_t aver_fw_clear_managed_dispatch(void) {
    // A NULL store, called BEFORE the ALC is unloaded. Every call site guards on managedInstalled()
    // and the individual pointer, so after this a tick / spawn / destroy simply fires no hook.
    managedDispatch() = AvManagedDispatch{};
    managedInstalled() = false;
    return 1;
}

int32_t aver_fw_managed_dispatch_installed(void) {
    return managedInstalled() ? 1 : 0;
}

int32_t aver_fw_tick(int32_t tickGroup, float dt) {
    // The single managed transition per tick group per frame. The bridge walks its own dense instance
    // list behind tick_all; the framework does NOT loop per managed actor. Native per-class vtable
    // ticking would also run here once that path exists (see AvActorVTable) — a documented follow-up,
    // so today this drives the managed path only. After a clear this ticks nothing rather than faulting.
    if (managedInstalled() && managedDispatch().tick_all) {
        managedDispatch().tick_all(tickGroup, dt);
        return 1;
    }
    return 0;
}

// ---- session singletons / play state — LATER STAGE (steps 10-11, 13), stubbed to 0 --------------
// These need the play lifecycle (a running GameInstance/GameMode singleton, a play-state machine)
// that step 8 does not build. They return 0 deliberately; wiring them is a later stage, and every
// registry/spawn/possession entry point above is fully implemented.
int32_t aver_fw_game_instance(void)               { return 0; }
int32_t aver_fw_game_mode(void)                    { return 0; }
int32_t aver_fw_player_controller(int32_t /*i*/)   { return 0; }
int32_t aver_fw_play_state(void)                   { return 0; }

}  // extern "C"
