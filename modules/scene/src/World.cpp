// World: entity lifetime, the component and field registry, hierarchy links, and the world-matrix
// propagation pass.
#include "aver/scene/World.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::scene {
namespace {

// Advances a revision counter, skipping 0 (the "no parent" sentinel).
inline void bumpRevision(u32& rev) {
    ++rev;
    if (rev == 0) rev = 1;
}

// The three components world-matrix composition reads or writes.
inline bool isTransformType(u32 type) {
    return type == kComponentLocal || type == kComponentWorld || type == kComponentHierarchy;
}

// Builds a quaternion from three normalised rotation ROWS, matching Mat4::fromQuat's convention.
Quat quatFromRows(const Vec3& r0, const Vec3& r1, const Vec3& r2) {
    const f32 trace = r0.x + r1.y + r2.z;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        return {(r1.z - r2.y) / s, (r2.x - r0.z) / s, (r0.y - r1.x) / s, 0.25f * s};
    }
    if (r0.x > r1.y && r0.x > r2.z) {
        const f32 s = std::sqrt(1.0f + r0.x - r1.y - r2.z) * 2.0f;
        return {0.25f * s, (r1.x + r0.y) / s, (r2.x + r0.z) / s, (r1.z - r2.y) / s};
    }
    if (r1.y > r2.z) {
        const f32 s = std::sqrt(1.0f + r1.y - r0.x - r2.z) * 2.0f;
        return {(r1.x + r0.y) / s, 0.25f * s, (r2.y + r1.z) / s, (r2.x - r0.z) / s};
    }
    const f32 s = std::sqrt(1.0f + r2.z - r0.x - r1.y) * 2.0f;
    return {(r2.x + r0.z) / s, (r2.y + r1.z) / s, 0.25f * s, (r0.y - r1.x) / s};
}

// Splits a row-major matrix into position, rotation and scale. Scales always come back positive.
Transform decomposeRowMajor(const Mat4& m) {
    Transform t;
    t.position = {m.m[3][0], m.m[3][1], m.m[3][2]};

    Vec3 r0{m.m[0][0], m.m[0][1], m.m[0][2]};
    Vec3 r1{m.m[1][0], m.m[1][1], m.m[1][2]};
    Vec3 r2{m.m[2][0], m.m[2][1], m.m[2][2]};
    t.scale = {r0.size(), r1.size(), r2.size()};

    t.rotation = quatFromRows(r0.getSafeNormal(), r1.getSafeNormal(), r2.getSafeNormal());
    return t;
}

const Transform kIdentityTransform{};
const Mat4      kIdentityMatrix{};

} // namespace

// ------------------------------------------------------------------------------------------------

// Every array the world owns. Index 0 of each registry array is the reserved invalid slot.
struct World::Impl {
    // One registered component type: its layout and its field table.
    struct ComponentType {
        std::string      name;
        usize            stride = 0;
        usize            align  = 0;
        std::vector<u32> fields;          // dense field ids, in declaration order
        bool             verified = false;
        std::string      verifyError;
    };

    std::vector<ComponentType>                  types{ComponentType{}};
    std::vector<std::unique_ptr<ComponentPool>> pools;
    std::vector<FieldDesc>                      fields{FieldDesc{}};
    // A deque, because a FieldDesc hands out a const char* into it.
    std::deque<std::string>                     nameStore;
    std::unordered_map<std::string, u32>        typeByName;
    std::unordered_map<std::string, u32>        fieldByName;

    // --- identity
    std::vector<u8>  generation{0};
    std::vector<u32> liveSlot{0};   // entity index -> position in `live` + 1
    std::vector<u8>  pending{0};    // 0 none, 1 queued by destroy(), 2 collected by this flush
    std::deque<u32>  freeIndices;   // FIFO: an index gets maximum distance before it is reused
    std::vector<u32> retired;

    std::vector<Entity> live;
    std::vector<Entity> destroyQueue;
    std::vector<Entity> doomed;
    std::string         nameBlob{1, '\0'};   // offset 0 is the empty name

    // --- hierarchy
    std::vector<Entity> order;
    std::vector<Entity> stack;
    std::vector<Entity> chain;
    std::vector<Entity> depthStack;   // scratch for refreshSubtreeDepth, kept off `stack`
    bool                topoDirty = false;

    // Bumped by every writer a world matrix depends on: transform writes, reparenting, adding or
    // removing CLocal/CWorld/CHierarchy, retiring entities. cleanEpoch is writeEpoch as of the last
    // flush that left every matrix composed, so equal means nothing is stale. Anything that bumps
    // CLocal::rev must go through World (touchLocal), or flush() and worldMatrix() will not see it.
    u64                 writeEpoch = 1;
    u64                 cleanEpoch = 0;

    // The entity's CHierarchy, or nullptr.
    CHierarchy* hier(Entity e) {
        return static_cast<CHierarchy*>(pools[kComponentHierarchy]->get(e));
    }
    // The entity's CLocal, or nullptr.
    CLocal* loc(Entity e) { return static_cast<CLocal*>(pools[kComponentLocal]->get(e)); }
    // The entity's CWorld, or nullptr.
    CWorld* wor(Entity e) { return static_cast<CWorld*>(pools[kComponentWorld]->get(e)); }

    // The entity's parent, or kInvalidEntity.
    Entity parentOf(Entity e) {
        const CHierarchy* h = hier(e);
        return h ? h->parent : kInvalidEntity;
    }

    // Detaches `e` from its parent's sibling chain and clears its links.
    void unlinkFromParent(Entity e) {
        CHierarchy* h = hier(e);
        if (!h) return;
        if (h->parent != kInvalidEntity) {
            if (h->prevSibling != kInvalidEntity) {
                hier(h->prevSibling)->nextSibling = h->nextSibling;
            } else if (CHierarchy* ph = hier(h->parent)) {
                ph->firstChild = h->nextSibling;
            }
            if (h->nextSibling != kInvalidEntity) hier(h->nextSibling)->prevSibling = h->prevSibling;
        }
        h->parent      = kInvalidEntity;
        h->prevSibling = kInvalidEntity;
        h->nextSibling = kInvalidEntity;
    }

    // Pushes `e` onto the front of `p`'s child list and sets its depth.
    void linkToParent(Entity e, Entity p) {
        CHierarchy* h  = hier(e);
        CHierarchy* ph = hier(p);
        if (!h || !ph) return;
        h->parent      = p;
        h->prevSibling = kInvalidEntity;
        h->nextSibling = ph->firstChild;
        if (ph->firstChild != kInvalidEntity) hier(ph->firstChild)->prevSibling = e;
        ph->firstChild = e;
        h->depth       = ph->depth + 1;
    }

    // Rewrites depth over everything beneath `root`, from root's current depth downward.
    void refreshSubtreeDepth(Entity root) {
        depthStack.clear();
        depthStack.push_back(root);
        while (!depthStack.empty()) {
            const Entity e = depthStack.back();
            depthStack.pop_back();
            CHierarchy* h = hier(e);
            if (!h) continue;
            const u32 childDepth = h->depth + 1;
            for (Entity c = h->firstChild; c != kInvalidEntity;) {
                CHierarchy* ch = hier(c);
                ch->depth      = childDepth;
                depthStack.push_back(c);
                c = ch->nextSibling;
            }
        }
    }

    // Recomposes `e`'s world matrix if its local or parent revision moved. True when it wrote one.
    // The only place world matrices are written.
    bool composeIfStale(Entity e) {
        CWorld* w = wor(e);
        CLocal* l = loc(e);
        if (!w || !l) return false;

        const Entity  p         = parentOf(e);
        const CWorld* pw        = (p != kInvalidEntity) ? wor(p) : nullptr;
        const u32     parentRev = pw ? pw->rev : 0;
        if (w->composedLocalRev == l->rev && w->composedParentRev == parentRev) return false;

        w->m                 = pw ? (l->xf.toMatrix() * pw->m) : l->xf.toMatrix();
        w->composedLocalRev  = l->rev;
        w->composedParentRev = parentRev;
        bumpRevision(w->rev);
        return true;
    }

    // Composes `e`'s whole ancestor chain, root first.
    void composeChain(Entity e) {
        chain.clear();
        for (Entity a = e; a != kInvalidEntity; a = parentOf(a)) chain.push_back(a);
        for (usize i = chain.size(); i-- > 0;) composeIfStale(chain[i]);
    }

#ifndef NDEBUG
    u32 verifyTick = 0;

    // True when composeIfStale would recompose `e`.
    bool isStale(Entity e) {
        const CWorld* w = wor(e);
        const CLocal* l = loc(e);
        if (!w || !l) return false;
        const Entity  p  = parentOf(e);
        const CWorld* pw = (p != kInvalidEntity) ? wor(p) : nullptr;
        return w->composedLocalRev != l->rev || w->composedParentRev != (pw ? pw->rev : 0);
    }

    // Debug only: a clean epoch must mean no matrix is stale. On a mismatch a writer bypassed World;
    // the epoch is bumped so the old full path runs and the frame stays correct.
    bool epochHolds(const char* where, Entity e) {
        bool ok = true;
        if (e != kInvalidEntity) {
            for (Entity a = e; a != kInvalidEntity && ok; a = parentOf(a)) ok = !isStale(a);
        } else {
            for (const Entity a : live) {
                if (!isStale(a)) continue;
                ok = false;
                break;
            }
        }
        if (!ok) {
            AVER_ERROR("Aver.Scene: {}: the write epoch says every world matrix is fresh but one is stale "
                       "- a transform writer bypassed World (touchLocal)", where);
            ++writeEpoch;
        }
        return ok;
    }
#endif

    // Rebuilds the topological order: a pre-order walk of every root, refreshing depth as it goes.
    void rebuildOrder() {
        order.clear();
        order.reserve(live.size());
        for (const Entity root : live) {
            const CHierarchy* rh = hier(root);
            if (rh && rh->parent != kInvalidEntity) continue;
            if (CHierarchy* mutableRoot = hier(root)) mutableRoot->depth = 0;

            stack.clear();
            stack.push_back(root);
            while (!stack.empty()) {
                const Entity e = stack.back();
                stack.pop_back();
                order.push_back(e);
                CHierarchy* h = hier(e);
                if (!h) continue;
                const u32 childDepth = h->depth + 1;
                for (Entity c = h->firstChild; c != kInvalidEntity;) {
                    CHierarchy* ch = hier(c);
                    ch->depth      = childDepth;
                    stack.push_back(c);
                    c = ch->nextSibling;
                }
            }
        }
        if (order.size() != live.size()) {
            AVER_ERROR("Aver.Scene: topological order covers {} of {} live entities - a parent link "
                       "points at something that is not live",
                       order.size(), live.size());
        }
        topoDirty = false;
    }

    // Strips `e` of its components, drops it from the live array, and frees or retires its slot.
    void retireSlot(Entity e) {
        const u32 idx = entityIndex(e);
        for (auto& p : pools)
            if (p) p->remove(e);

        if (const u32 slot = liveSlot[idx]) {
            const usize deadPos = slot - 1;
            const usize lastPos = live.size() - 1;
            if (deadPos != lastPos) {
                live[deadPos]                          = live[lastPos];
                liveSlot[entityIndex(live[deadPos])]   = static_cast<u32>(deadPos + 1);
            }
            live.pop_back();
            liveSlot[idx] = 0;
        }
        pending[idx] = 0;

        const u32 gen = generation[idx];
        if (gen >= kEntityMaxGen) {
            retired.push_back(idx);
        } else {
            generation[idx] = static_cast<u8>(gen + 1);
            freeIndices.push_back(idx);
        }
    }

    // Expands the destroy queue to whole subtrees, then unlinks and retires every entity in it.
    void collectAndRetire() {
        ++writeEpoch;
        doomed.clear();
        for (const Entity queued : destroyQueue) {
            stack.clear();
            stack.push_back(queued);
            while (!stack.empty()) {
                const Entity e   = stack.back();
                const u32    idx = entityIndex(e);
                stack.pop_back();
                if (pending[idx] == 2) continue;
                pending[idx] = 2;
                doomed.push_back(e);
                if (CHierarchy* h = hier(e)) {
                    for (Entity c = h->firstChild; c != kInvalidEntity;) {
                        CHierarchy* ch = hier(c);
                        stack.push_back(c);
                        c = ch->nextSibling;
                    }
                }
            }
        }
        for (const Entity e : doomed) unlinkFromParent(e);
        for (const Entity e : doomed) retireSlot(e);
        destroyQueue.clear();
    }
};

// ------------------------------------------------------------------------------------------------

// Creates the world and registers the built-in components.
World::World() : impl_(new Impl()) {
    impl_->pools.push_back(nullptr);   // parallel to types[0], the reserved invalid slot
    detail::registerBuiltinComponents(*this);
}

World::~World() { delete impl_; }

// The process-global world.
World& World::instance() {
    static World world;
    return world;
}

// ---- entity lifetime ---------------------------------------------------------------------------

// Creates an entity carrying CName, CLocal, CWorld and CHierarchy. kInvalidEntity when exhausted.
Entity World::create(std::string_view nm) {
    Impl& d = *impl_;

    u32 idx;
    if (!d.freeIndices.empty()) {
        idx = d.freeIndices.front();
        d.freeIndices.pop_front();
    } else {
        if (d.generation.size() > kMaxEntities) {
            AVER_ERROR("Aver.Scene: the index space is exhausted ({} live, {} retired)",
                       d.live.size(), d.retired.size());
            return kInvalidEntity;
        }
        idx = static_cast<u32>(d.generation.size());
        d.generation.push_back(1);
        d.liveSlot.push_back(0);
        d.pending.push_back(0);
    }

    const Entity e  = makeEntity(idx, d.generation[idx]);
    d.liveSlot[idx] = static_cast<u32>(d.live.size() + 1);
    d.live.push_back(e);

    // addComponent zero-fills; these four need their defaults instead.
    *static_cast<CName*>(addComponent(e, kComponentName))           = CName{};
    *static_cast<CLocal*>(addComponent(e, kComponentLocal))         = CLocal{};
    *static_cast<CWorld*>(addComponent(e, kComponentWorld))         = CWorld{};
    *static_cast<CHierarchy*>(addComponent(e, kComponentHierarchy)) = CHierarchy{};

    setName(e, nm);
    d.order.push_back(e);
    return e;
}

// Creates an entity, gives it `local`, then parents it.
Entity World::create(std::string_view nm, Entity parentEntity, const Transform& local) {
    const Entity e = create(nm);
    if (e == kInvalidEntity) return e;
    setLocalTransform(e, local);
    if (parentEntity != kInvalidEntity) setParent(e, parentEntity);
    return e;
}

// Queues `e` for destruction at the next flush(). Idempotent.
bool World::destroy(Entity e) {
    if (!valid(e)) return false;
    Impl&     d   = *impl_;
    const u32 idx = entityIndex(e);
    if (d.pending[idx] != 0) return true;
    d.pending[idx] = 1;
    d.destroyQueue.push_back(e);
    return true;
}

// True once destroy() has queued `e`, until the flush that retires it.
bool World::destroyPending(Entity e) const {
    const u32 idx = entityIndex(e);
    return idx < impl_->pending.size() && impl_->pending[idx] != 0;
}

// True when the handle addresses a live entity: right index, right generation, and a live slot.
bool World::valid(Entity e) const {
    const Impl& d = *impl_;
    if (e == kInvalidEntity) return false;
    const u32 idx = entityIndex(e);
    if (idx == 0 || idx >= d.generation.size()) return false;
    if (d.generation[idx] != entityGen(e)) return false;
    return d.liveSlot[idx] != 0;
}

u32    World::count() const                { return static_cast<u32>(impl_->live.size()); }
Entity World::at(u32 denseIndex) const     { return denseIndex < impl_->live.size() ? impl_->live[denseIndex] : kInvalidEntity; }
u32    World::retiredSlotCount() const     { return static_cast<u32>(impl_->retired.size()); }
u32    World::freeSlotCount() const        { return static_cast<u32>(impl_->freeIndices.size()); }

// ---- identity ----------------------------------------------------------------------------------

// The entity's name, or "" for a stale handle or an out-of-range blob cursor.
const char* World::name(Entity e) const {
    const CName* n = component<CName>(e, kComponentName);
    return (n && n->offset < impl_->nameBlob.size()) ? impl_->nameBlob.c_str() + n->offset : "";
}

// Appends the name to the blob, repoints the entity's cursor, and rehashes its object id.
bool World::setName(Entity e, std::string_view nm) {
    CName* n = component<CName>(e, kComponentName);
    if (!n) return false;
    Impl& d = *impl_;
    n->offset = static_cast<u32>(d.nameBlob.size());
    n->len    = static_cast<u32>(nm.size());
    d.nameBlob.append(nm);
    d.nameBlob.push_back('\0');   // so name() can hand out a C string without a copy
    n->objectId = fnv1a64(nm);
    return true;
}

// The entity's persisted identity, or 0.
u64 World::objectId(Entity e) const {
    const CName* n = component<CName>(e, kComponentName);
    return n ? n->objectId : 0;
}

// Overrides the persisted identity.
bool World::setObjectId(Entity e, u64 id) {
    CName* n = component<CName>(e, kComponentName);
    if (!n) return false;
    n->objectId = id;
    return true;
}

// The first live entity with this name, or kInvalidEntity. A linear scan over the CName pool.
Entity World::find(std::string_view nm) const {
    const ComponentPool* names = impl_->pools[kComponentName].get();
    const u64            want  = fnv1a64(nm);
    for (usize i = 0; i < names->size(); ++i) {
        const CName* n = static_cast<const CName*>(names->dataAt(i));
        if (n->objectId != want || n->len != nm.size()) continue;
        if (nm.compare(0, nm.size(), impl_->nameBlob.c_str() + n->offset, n->len) == 0)
            return names->entityAt(i);
    }
    return kInvalidEntity;
}

// ---- component registry ------------------------------------------------------------------------

// Registers a component type by name, idempotently, and returns a builder for its field table.
ComponentBuilder World::registerComponent(std::string_view nm, usize stride, usize align) {
    Impl&             d = *impl_;
    const std::string key(nm);

    if (const auto it = d.typeByName.find(key); it != d.typeByName.end()) {
        Impl::ComponentType& t = d.types[it->second];
        if (t.stride != stride || t.align != align) {
            AVER_ERROR("Aver.Scene: component '{}' re-registered with a different layout "
                       "({}/{} -> {}/{} bytes); refusing, because the live entities carrying it "
                       "were laid out the old way",
                       key, t.stride, t.align, stride, align);
            return ComponentBuilder(nullptr, 0);
        }
        t.fields.clear();
        t.verified = false;
        t.verifyError.clear();
        return ComponentBuilder(this, it->second);
    }

    if (stride == 0 || align == 0 || key.empty()) {
        AVER_ERROR("Aver.Scene: refusing to register component '{}' with stride {} align {}",
                   key, stride, align);
        return ComponentBuilder(nullptr, 0);
    }

    const u32 id = static_cast<u32>(d.types.size());
    d.types.push_back(Impl::ComponentType{key, stride, align, {}, false, {}});
    d.pools.push_back(std::make_unique<ComponentPool>(id, stride, align));
    d.typeByName.emplace(key, id);
    return ComponentBuilder(this, id);
}

// The dense id of a registered component type, or 0.
u32 World::componentId(std::string_view nm) const {
    const auto it = impl_->typeByName.find(std::string(nm));
    return it == impl_->typeByName.end() ? 0 : it->second;
}

u32 World::componentCount() const { return static_cast<u32>(impl_->types.size()) - 1; }

// The component type id at an enumeration index.
u32 World::componentAt(u32 index) const {
    return index < componentCount() ? index + 1 : 0;   // dense ids start at 1
}

// The registered name of a component type.
const char* World::componentName(u32 type) const {
    return (type > 0 && type < impl_->types.size()) ? impl_->types[type].name.c_str() : "";
}

// The byte stride of a component type.
usize World::componentSize(u32 type) const {
    return (type > 0 && type < impl_->types.size()) ? impl_->types[type].stride : 0;
}

// True when the type's field table passed verification.
bool World::componentVerified(u32 type) const {
    return type > 0 && type < impl_->types.size() && impl_->types[type].verified;
}

// Why verification failed, or "" when it did not.
const char* World::componentVerifyError(u32 type) const {
    return (type > 0 && type < impl_->types.size()) ? impl_->types[type].verifyError.c_str() : "";
}

// Adds one field to a component's table. Returns the dense field id, 0 on rejection.
u32 World::addField(u32 type, const char* fieldName, FieldKind kind, u16 offset, u8 arity, bool readOnly) {
    Impl& d = *impl_;
    if (type == 0 || type >= d.types.size() || !fieldName || !*fieldName) return 0;

    Impl::ComponentType& t        = d.types[type];
    const u8             canonical = canonicalArity(kind);
    if (arity != 0 && arity != canonical) {
        AVER_WARN("Aver.Scene: field '{}.{}' declares arity {} but its kind implies {}; the kind "
                  "wins, because the accessors size their buffers from the kind",
                  t.name, fieldName, arity, canonical);
    }

    const std::string qualified = t.name + "." + fieldName;
    if (const auto it = d.fieldByName.find(qualified); it != d.fieldByName.end()) {
        FieldDesc& f = d.fields[it->second];
        f.component  = type;
        f.kind       = kind;
        f.offset     = offset;
        f.arity      = canonical;
        f.readOnly   = readOnly;
        t.fields.push_back(it->second);
        return it->second;
    }

    d.nameStore.push_back(std::string(fieldName));
    const char* namePtr = d.nameStore.back().c_str();
    d.nameStore.push_back(qualified);
    const char* qualPtr = d.nameStore.back().c_str();

    const u32 id = static_cast<u32>(d.fields.size());
    d.fields.push_back(FieldDesc{namePtr, qualPtr, type, kind, offset, canonical, readOnly});
    d.fieldByName.emplace(qualified, id);
    t.fields.push_back(id);
    return id;
}

// Checks the type's field table covers `structBytes` with no gap and no overlap. Logs what is wrong.
bool World::verifyComponent(u32 type, usize structBytes) {
    Impl& d = *impl_;
    if (type == 0 || type >= d.types.size()) return false;

    Impl::ComponentType& t = d.types[type];
    t.verified             = false;
    t.verifyError.clear();

    const auto fail = [&t](std::string why) {
        t.verifyError = "component '" + t.name + "': " + why;
        AVER_ERROR("Aver.Scene: {}", t.verifyError);
        return false;
    };

    if (t.stride != structBytes) {
        return fail("registered as " + std::to_string(t.stride) + " bytes but sizeof is " +
                    std::to_string(structBytes));
    }

    std::vector<u32> ids = t.fields;
    std::sort(ids.begin(), ids.end(), [&d](u32 a, u32 b) {
        return d.fields[a].offset < d.fields[b].offset;
    });

    usize cursor = 0;
    for (const u32 id : ids) {
        const FieldDesc& f  = d.fields[id];
        const usize      sz = fieldByteSize(f.kind);
        if (f.offset < cursor) {
            return fail(std::string("field '") + f.name + "' at byte " + std::to_string(f.offset) +
                        " overlaps the field before it");
        }
        if (f.offset > cursor) {
            return fail("bytes " + std::to_string(cursor) + ".." + std::to_string(f.offset) +
                        " are not covered by any field - a member is missing from the table before '" +
                        f.name + "'");
        }
        cursor = f.offset + sz;
        if (cursor > structBytes) {
            return fail(std::string("field '") + f.name + "' runs past the end of the struct");
        }
    }

    // Only trailing padding smaller than the struct's own alignment may be left unexplained.
    if (structBytes - cursor >= t.align) {
        return fail("the table covers " + std::to_string(cursor) + " of " +
                    std::to_string(structBytes) + " bytes - a member is missing from the table");
    }

    t.verified = true;
    return true;
}

// ---- field lookup ------------------------------------------------------------------------------

// Resolves "Component.field" to a dense field id, or 0.
u32 World::fieldId(std::string_view qualifiedName) const {
    const auto it = impl_->fieldByName.find(std::string(qualifiedName));
    return it == impl_->fieldByName.end() ? 0 : it->second;
}

// The descriptor for a field id, or nullptr.
const FieldDesc* World::field(u32 id) const {
    return (id > 0 && id < impl_->fields.size()) ? &impl_->fields[id] : nullptr;
}

// Number of fields declared on a component type.
u32 World::fieldCount(u32 type) const {
    return (type > 0 && type < impl_->types.size())
               ? static_cast<u32>(impl_->types[type].fields.size())
               : 0;
}

// The field id at an index within a component's table.
u32 World::fieldAt(u32 type, u32 index) const {
    if (type == 0 || type >= impl_->types.size()) return 0;
    const auto& f = impl_->types[type].fields;
    return index < f.size() ? f[index] : 0;
}

// ---- component storage -------------------------------------------------------------------------

// The pool storing a component type, or nullptr.
ComponentPool* World::pool(u32 type) {
    return (type > 0 && type < impl_->pools.size()) ? impl_->pools[type].get() : nullptr;
}

// Attaches zero-filled storage for `type` to `e`, or returns what it already has.
void* World::addComponent(Entity e, u32 type) {
    ComponentPool* p = pool(type);
    if (isTransformType(type)) ++impl_->writeEpoch;
    return (p && valid(e)) ? p->add(e) : nullptr;
}

// The component bytes, or nullptr when absent or the handle is stale.
void* World::getComponent(Entity e, u32 type) {
    ComponentPool* p = pool(type);
    return p ? p->get(e) : nullptr;
}

const void* World::getComponent(Entity e, u32 type) const {
    const ComponentPool* p = (type > 0 && type < impl_->pools.size()) ? impl_->pools[type].get() : nullptr;
    return p ? p->get(e) : nullptr;
}

// True when `e` carries this component.
bool World::hasComponent(Entity e, u32 type) const {
    const ComponentPool* p = (type > 0 && type < impl_->pools.size()) ? impl_->pools[type].get() : nullptr;
    return p && p->has(e);
}

// Drops the component from `e`.
bool World::removeComponent(Entity e, u32 type) {
    ComponentPool* p = pool(type);
    if (isTransformType(type)) ++impl_->writeEpoch;
    return p && p->remove(e);
}

// ---- hierarchy ---------------------------------------------------------------------------------

// Reparents `e`. Refuses a cycle, a self-parent, or a parent already queued for destruction.
// keepWorld recomputes the local transform so `e` does not visibly move.
bool World::setParent(Entity e, Entity newParent, bool keepWorld) {
    Impl& d = *impl_;
    if (!valid(e)) return false;
    if (newParent != kInvalidEntity && !valid(newParent)) return false;
    if (newParent == e) return false;

    if (newParent != kInvalidEntity && destroyPending(newParent)) return false;

    for (Entity a = newParent; a != kInvalidEntity; a = d.parentOf(a)) {
        if (a == e) return false;
    }

    Mat4 keep;
    if (keepWorld) keep = worldMatrix(e);

    ++d.writeEpoch;
    d.unlinkFromParent(e);
    if (newParent != kInvalidEntity) {
        d.linkToParent(e, newParent);
    } else if (CHierarchy* h = d.hier(e)) {
        h->depth = 0;
    }
    d.refreshSubtreeDepth(e);

    if (CWorld* w = d.wor(e)) w->composedLocalRev = 0;
    d.topoDirty = true;

    if (keepWorld) {
        const Mat4 parentWorld =
            (newParent != kInvalidEntity) ? worldMatrix(newParent) : Mat4::identity();
        setLocalTransform(e, decomposeRowMajor(keep * parentWorld.inverse()));
    }
    return true;
}

// The immediate parent, or kInvalidEntity for a root.
Entity World::parent(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->parent : kInvalidEntity;
}

// The first child, or kInvalidEntity.
Entity World::firstChild(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->firstChild : kInvalidEntity;
}

// The next sibling under the same parent, or kInvalidEntity.
Entity World::nextSibling(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->nextSibling : kInvalidEntity;
}

// Number of immediate children.
u32 World::childCount(Entity e) const {
    u32 n = 0;
    for (Entity c = firstChild(e); c != kInvalidEntity; c = nextSibling(c)) ++n;
    return n;
}

// Distance from the root; 0 for a root.
u32 World::depth(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->depth : 0;
}

u32    World::topologicalCount() const        { return static_cast<u32>(impl_->order.size()); }
Entity World::topologicalAt(u32 index) const  { return index < impl_->order.size() ? impl_->order[index] : kInvalidEntity; }

// ---- transforms --------------------------------------------------------------------------------

// The entity's local transform, or identity for a stale handle.
const Transform& World::localTransform(Entity e) const {
    const CLocal* l = component<CLocal>(e, kComponentLocal);
    return l ? l->xf : kIdentityTransform;
}

// Replaces the local transform and bumps its revision.
bool World::setLocalTransform(Entity e, const Transform& xf) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf = xf;
    bumpRevision(l->rev);
    ++impl_->writeEpoch;
    return true;
}

// Sets local position and bumps the revision.
bool World::setLocalPosition(Entity e, const Vec3& p) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf.position = p;
    bumpRevision(l->rev);
    ++impl_->writeEpoch;
    return true;
}

// Sets local rotation and bumps the revision.
bool World::setLocalRotation(Entity e, const Quat& q) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf.rotation = q;
    bumpRevision(l->rev);
    ++impl_->writeEpoch;
    return true;
}

// Sets local scale and bumps the revision.
bool World::setLocalScale(Entity e, const Vec3& s) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf.scale = s;
    bumpRevision(l->rev);
    ++impl_->writeEpoch;
    return true;
}

// Bumps CLocal::rev after a caller wrote the transform through the pool directly.
bool World::touchLocal(Entity e) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    bumpRevision(l->rev);
    ++impl_->writeEpoch;
    return true;
}

// The entity's world matrix, composing its ancestor chain on demand. Identity for a stale handle.
const Mat4& World::worldMatrix(Entity e) {
    Impl& d = *impl_;
    if (d.cleanEpoch != d.writeEpoch) {
        d.composeChain(e);
    }
#ifndef NDEBUG
    else if ((++d.verifyTick & 255u) == 0) {
        d.epochHolds("worldMatrix", e);
        d.composeChain(e);
    }
#endif
    const CWorld* w = d.wor(e);
    return w ? w->m : kIdentityMatrix;
}

// The revision of the entity's composed world matrix.
u32 World::worldRevision(Entity e) const {
    const CWorld* w = component<CWorld>(e, kComponentWorld);
    return w ? w->rev : 0;
}

// ---- frame -------------------------------------------------------------------------------------

// Retires deferred destroys, rebuilds the order if needed, then recomposes stale world matrices.
// Returns how many were recomposed.
u32 World::flush() {
    Impl& d = *impl_;
    if (d.cleanEpoch == d.writeEpoch && d.destroyQueue.empty() && !d.topoDirty) {
#ifndef NDEBUG
        if ((++d.verifyTick & 63u) != 0 || d.epochHolds("flush", kInvalidEntity)) return 0;
#else
        return 0;
#endif
    }
    if (!d.destroyQueue.empty()) {
        d.collectAndRetire();
        d.topoDirty = true;
    }
    if (d.topoDirty) d.rebuildOrder();

    u32 recomposed = 0;
    for (const Entity e : d.order) {
        if (d.composeIfStale(e)) ++recomposed;
    }
    // An order that misses a live entity (a parent link to a dead entity) leaves it to worldMatrix()'s
    // on-demand compose, so that world never takes the fast paths.
    if (d.order.size() == d.live.size()) d.cleanEpoch = d.writeEpoch;
    return recomposed;
}

} // namespace aver::scene
