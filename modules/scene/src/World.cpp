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

// A revision never legally reads 0 once an entity exists, because 0 is the "this entity has no
// parent" value CWorld::composedParentRev carries for a root. Wrapping past it would make a root
// look freshly composed forever.
inline void bumpRevision(u32& rev) {
    ++rev;
    if (rev == 0) rev = 1;
}

// Row-vector convention: the rotation rows are what Mat4::fromQuat writes, so the differences below
// are 4w*axis rather than the column-vector form's negatives. Getting this backwards produces a
// conjugated rotation that looks plausible until something is asymmetric.
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

// Only used by the keepWorld path of setParent. A mirrored parent comes back with positive scales
// and an absorbed flip, because row lengths cannot carry a sign — the alternative is a determinant
// test that guesses WHICH axis was mirrored, and a guess is worse than a documented limit.
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

struct World::Impl {
    struct ComponentType {
        std::string      name;
        usize            stride = 0;
        usize            align  = 0;
        std::vector<u32> fields;          // dense field ids, in declaration order
        bool             verified = false;
        std::string      verifyError;
    };

    // Index 0 of every registry array is the reserved invalid slot, so a 0 id can never be a valid
    // subscript by accident — the same rule the handles are under, one level down.
    std::vector<ComponentType>                  types{ComponentType{}};
    std::vector<std::unique_ptr<ComponentPool>> pools;
    std::vector<FieldDesc>                      fields{FieldDesc{}};
    // A deque, because a FieldDesc hands out a const char* and a vector's strings move when it grows.
    std::deque<std::string>                     nameStore;
    std::unordered_map<std::string, u32>        typeByName;
    std::unordered_map<std::string, u32>        fieldByName;

    // --- identity. Index 0 exists so the arrays are indexable by entity index, and is never handed
    // out, which is what makes a live handle non-zero.
    std::vector<u8>  generation{0};
    std::vector<u32> liveSlot{0};   // entity index -> position in `live` + 1
    // 0 none, 1 queued by destroy(), 2 collected by the current flush. The third state exists
    // because a queued entity can also be reached as the descendant of another queued entity.
    std::vector<u8>  pending{0};
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
    bool                topoDirty = false;

    // ---- component shortcuts. The built-ins are the only types this file knows the shape of.
    CHierarchy* hier(Entity e) {
        return static_cast<CHierarchy*>(pools[kComponentHierarchy]->get(e));
    }
    CLocal* loc(Entity e) { return static_cast<CLocal*>(pools[kComponentLocal]->get(e)); }
    CWorld* wor(Entity e) { return static_cast<CWorld*>(pools[kComponentWorld]->get(e)); }

    Entity parentOf(Entity e) {
        const CHierarchy* h = hier(e);
        return h ? h->parent : kInvalidEntity;
    }

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

    // The one place world matrices are written. Dirt is a revision compare rather than a bitset walk,
    // so a child's test reads a parent revision the same pass has already updated and dirt
    // propagates downward for free.
    bool composeIfStale(Entity e) {
        CWorld* w = wor(e);
        CLocal* l = loc(e);
        if (!w || !l) return false;

        const Entity  p         = parentOf(e);
        const CWorld* pw        = (p != kInvalidEntity) ? wor(p) : nullptr;
        const u32     parentRev = pw ? pw->rev : 0;
        if (w->composedLocalRev == l->rev && w->composedParentRev == parentRev) return false;

        // v * (L * P): a child reaches world space through its own local matrix FIRST. There is no
        // other correct order under the row-vector contract.
        w->m                 = pw ? (l->xf.toMatrix() * pw->m) : l->xf.toMatrix();
        w->composedLocalRev  = l->rev;
        w->composedParentRev = parentRev;
        bumpRevision(w->rev);
        return true;
    }

    // Composing every ancestor from the root down reaches the same fixed point as stopping at the
    // nearest fresh one, because composeIfStale is a no-op on a fresh entity. Bounded by depth
    // either way, and this form needs no second staleness predicate to keep in agreement with the
    // first.
    void composeChain(Entity e) {
        chain.clear();
        for (Entity a = e; a != kInvalidEntity; a = parentOf(a)) chain.push_back(a);
        for (usize i = chain.size(); i-- > 0;) composeIfStale(chain[i]);
    }

    void rebuildOrder() {
        order.clear();
        order.reserve(live.size());
        for (const Entity root : live) {
            const CHierarchy* rh = hier(root);
            if (rh && rh->parent != kInvalidEntity) continue;
            if (CHierarchy* mutableRoot = hier(root)) mutableRoot->depth = 0;

            // Pre-order depth first: a parent is always emitted before its children, which is the
            // whole requirement on this array. A Kahn sweep would produce the same property at the
            // cost of an in-degree pass over links that are already a forest.
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
            // RETIRED rather than aliased. Handing this index back at generation 1 would make a
            // handle from the slot's first life validate against its 128th occupant, and a stale
            // handle that silently addresses the wrong entity is the failure this whole packing
            // exists to prevent.
            retired.push_back(idx);
        } else {
            generation[idx] = static_cast<u8>(gen + 1);
            freeIndices.push_back(idx);
        }
    }

    void collectAndRetire() {
        doomed.clear();
        // Expand every queued entity to its whole subtree first: retiring a parent and leaving its
        // children behind would leave a link pointing into a slot that is about to be reused.
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

World::World() : impl_(new Impl()) {
    impl_->pools.push_back(nullptr);   // parallel to types[0], the reserved invalid slot
    detail::registerBuiltinComponents(*this);
}

World::~World() { delete impl_; }

World& World::instance() {
    static World world;
    return world;
}

// ---- entity lifetime ---------------------------------------------------------------------------

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

    // addComponent zero-fills, which is the ABI's contract for a script-declared type but is wrong
    // for these four: a revision of 0 is the root sentinel and a scale of 0 is a collapsed object.
    *static_cast<CName*>(addComponent(e, kComponentName))           = CName{};
    *static_cast<CLocal*>(addComponent(e, kComponentLocal))         = CLocal{};
    *static_cast<CWorld*>(addComponent(e, kComponentWorld))         = CWorld{};
    *static_cast<CHierarchy*>(addComponent(e, kComponentHierarchy)) = CHierarchy{};

    setName(e, nm);
    // A new entity is always a root, and appending a root to the end of a topological order keeps it
    // topological — which is why creation does not dirty the sort.
    d.order.push_back(e);
    return e;
}

Entity World::create(std::string_view nm, Entity parentEntity, const Transform& local) {
    const Entity e = create(nm);
    if (e == kInvalidEntity) return e;
    setLocalTransform(e, local);
    if (parentEntity != kInvalidEntity) setParent(e, parentEntity);
    return e;
}

bool World::destroy(Entity e) {
    if (!valid(e)) return false;
    Impl&     d   = *impl_;
    const u32 idx = entityIndex(e);
    if (d.pending[idx] != 0) return true;
    d.pending[idx] = 1;
    d.destroyQueue.push_back(e);
    return true;
}

bool World::destroyPending(Entity e) const {
    const u32 idx = entityIndex(e);
    return idx < impl_->pending.size() && impl_->pending[idx] != 0;
}

bool World::valid(Entity e) const {
    const Impl& d = *impl_;
    if (e == kInvalidEntity) return false;
    const u32 idx = entityIndex(e);
    if (idx == 0 || idx >= d.generation.size()) return false;
    // The generation compare alone would accept a handle whose slot is free but not yet reissued,
    // because a freed slot already carries the generation its next occupant will get.
    if (d.generation[idx] != entityGen(e)) return false;
    return d.liveSlot[idx] != 0;
}

u32    World::count() const                { return static_cast<u32>(impl_->live.size()); }
Entity World::at(u32 denseIndex) const     { return denseIndex < impl_->live.size() ? impl_->live[denseIndex] : kInvalidEntity; }
u32    World::retiredSlotCount() const     { return static_cast<u32>(impl_->retired.size()); }
u32    World::freeSlotCount() const        { return static_cast<u32>(impl_->freeIndices.size()); }

// ---- identity ----------------------------------------------------------------------------------

const char* World::name(Entity e) const {
    const CName* n = component<CName>(e, kComponentName);
    return n ? impl_->nameBlob.c_str() + n->offset : "";
}

bool World::setName(Entity e, std::string_view nm) {
    CName* n = component<CName>(e, kComponentName);
    if (!n) return false;
    Impl& d = *impl_;
    // The blob only grows. Compaction would have to rewrite every CName offset, and a rename is a
    // rare editor gesture — the memory this wastes is bounded by how often a human types.
    n->offset = static_cast<u32>(d.nameBlob.size());
    n->len    = static_cast<u32>(nm.size());
    d.nameBlob.append(nm);
    d.nameBlob.push_back('\0');   // so name() can hand out a C string without a copy
    n->objectId = fnv1a64(nm);
    return true;
}

u64 World::objectId(Entity e) const {
    const CName* n = component<CName>(e, kComponentName);
    return n ? n->objectId : 0;
}

bool World::setObjectId(Entity e, u64 id) {
    CName* n = component<CName>(e, kComponentName);
    if (!n) return false;
    n->objectId = id;
    return true;
}

Entity World::find(std::string_view nm) const {
    const ComponentPool* names = impl_->pools[kComponentName].get();
    const u64            want  = fnv1a64(nm);
    for (usize i = 0; i < names->size(); ++i) {
        const CName* n = static_cast<const CName*>(names->dataAt(i));
        // The hash narrows it; the compare settles it, because a hash collision must not rename an
        // entity the caller never asked for.
        if (n->objectId != want || n->len != nm.size()) continue;
        if (nm.compare(0, nm.size(), impl_->nameBlob.c_str() + n->offset, n->len) == 0)
            return names->entityAt(i);
    }
    return kInvalidEntity;
}

// ---- component registry ------------------------------------------------------------------------

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
        // Idempotent by name: the redeclaration rebuilds the table but keeps the type id and the
        // pool, so a hot reload does not strand the entities that already carry this component.
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

u32 World::componentId(std::string_view nm) const {
    const auto it = impl_->typeByName.find(std::string(nm));
    return it == impl_->typeByName.end() ? 0 : it->second;
}

u32 World::componentCount() const { return static_cast<u32>(impl_->types.size()) - 1; }

u32 World::componentAt(u32 index) const {
    return index < componentCount() ? index + 1 : 0;   // dense ids start at 1
}

const char* World::componentName(u32 type) const {
    return (type > 0 && type < impl_->types.size()) ? impl_->types[type].name.c_str() : "";
}

usize World::componentSize(u32 type) const {
    return (type > 0 && type < impl_->types.size()) ? impl_->types[type].stride : 0;
}

bool World::componentVerified(u32 type) const {
    return type > 0 && type < impl_->types.size() && impl_->types[type].verified;
}

const char* World::componentVerifyError(u32 type) const {
    return (type > 0 && type < impl_->types.size()) ? impl_->types[type].verifyError.c_str() : "";
}

u32 World::addField(u32 type, const char* fieldName, FieldKind kind, u16 offset, u8 arity) {
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
        // A redeclaration keeps the dense id, so a class that cached this field id across a reload
        // still addresses the same field.
        FieldDesc& f = d.fields[it->second];
        f.component  = type;
        f.kind       = kind;
        f.offset     = offset;
        f.arity      = canonical;
        t.fields.push_back(it->second);
        return it->second;
    }

    d.nameStore.push_back(std::string(fieldName));
    const char* namePtr = d.nameStore.back().c_str();
    d.nameStore.push_back(qualified);
    const char* qualPtr = d.nameStore.back().c_str();

    const u32 id = static_cast<u32>(d.fields.size());
    d.fields.push_back(FieldDesc{namePtr, qualPtr, type, kind, offset, canonical});
    d.fieldByName.emplace(qualified, id);
    t.fields.push_back(id);
    return id;
}

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

    // Only trailing padding smaller than the struct's own alignment may be left unexplained. A gap
    // of a whole alignment unit or more is a member the table never mentioned, which is exactly the
    // drift this terminator exists to catch.
    if (structBytes - cursor >= t.align) {
        return fail("the table covers " + std::to_string(cursor) + " of " +
                    std::to_string(structBytes) + " bytes - a member is missing from the table");
    }

    t.verified = true;
    return true;
}

// ---- field lookup ------------------------------------------------------------------------------

u32 World::fieldId(std::string_view qualifiedName) const {
    const auto it = impl_->fieldByName.find(std::string(qualifiedName));
    return it == impl_->fieldByName.end() ? 0 : it->second;
}

const FieldDesc* World::field(u32 id) const {
    return (id > 0 && id < impl_->fields.size()) ? &impl_->fields[id] : nullptr;
}

u32 World::fieldCount(u32 type) const {
    return (type > 0 && type < impl_->types.size())
               ? static_cast<u32>(impl_->types[type].fields.size())
               : 0;
}

u32 World::fieldAt(u32 type, u32 index) const {
    if (type == 0 || type >= impl_->types.size()) return 0;
    const auto& f = impl_->types[type].fields;
    return index < f.size() ? f[index] : 0;
}

// ---- component storage -------------------------------------------------------------------------

ComponentPool* World::pool(u32 type) {
    return (type > 0 && type < impl_->pools.size()) ? impl_->pools[type].get() : nullptr;
}

void* World::addComponent(Entity e, u32 type) {
    ComponentPool* p = pool(type);
    return (p && valid(e)) ? p->add(e) : nullptr;
}

void* World::getComponent(Entity e, u32 type) {
    ComponentPool* p = pool(type);
    return p ? p->get(e) : nullptr;
}

const void* World::getComponent(Entity e, u32 type) const {
    const ComponentPool* p = (type > 0 && type < impl_->pools.size()) ? impl_->pools[type].get() : nullptr;
    return p ? p->get(e) : nullptr;
}

bool World::hasComponent(Entity e, u32 type) const {
    const ComponentPool* p = (type > 0 && type < impl_->pools.size()) ? impl_->pools[type].get() : nullptr;
    return p && p->has(e);
}

bool World::removeComponent(Entity e, u32 type) {
    ComponentPool* p = pool(type);
    return p && p->remove(e);
}

// ---- hierarchy ---------------------------------------------------------------------------------

bool World::setParent(Entity e, Entity newParent, bool keepWorld) {
    Impl& d = *impl_;
    if (!valid(e)) return false;
    if (newParent != kInvalidEntity && !valid(newParent)) return false;
    if (newParent == e) return false;

    // A cycle would make the topological sort skip a whole subtree forever, so it is refused here
    // rather than detected later where the symptom is a matrix that never updates.
    for (Entity a = newParent; a != kInvalidEntity; a = d.parentOf(a)) {
        if (a == e) return false;
    }

    Mat4 keep;
    if (keepWorld) keep = worldMatrix(e);

    d.unlinkFromParent(e);
    if (newParent != kInvalidEntity) {
        d.linkToParent(e, newParent);
    } else if (CHierarchy* h = d.hier(e)) {
        h->depth = 0;
    }

    // Zeroing the composed local revision is what forces the recompose. Relying on the parent
    // revision alone would miss a move between two parents whose revisions happen to be equal.
    if (CWorld* w = d.wor(e)) w->composedLocalRev = 0;
    d.topoDirty = true;

    if (keepWorld) {
        const Mat4 parentWorld =
            (newParent != kInvalidEntity) ? worldMatrix(newParent) : Mat4::identity();
        // L * P = keep, so L = keep * P^-1 — the inverse goes on the RIGHT under v * M.
        setLocalTransform(e, decomposeRowMajor(keep * parentWorld.inverse()));
    }
    return true;
}

Entity World::parent(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->parent : kInvalidEntity;
}

Entity World::firstChild(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->firstChild : kInvalidEntity;
}

Entity World::nextSibling(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->nextSibling : kInvalidEntity;
}

u32 World::childCount(Entity e) const {
    u32 n = 0;
    for (Entity c = firstChild(e); c != kInvalidEntity; c = nextSibling(c)) ++n;
    return n;
}

u32 World::depth(Entity e) const {
    const CHierarchy* h = component<CHierarchy>(e, kComponentHierarchy);
    return h ? h->depth : 0;
}

u32    World::topologicalCount() const        { return static_cast<u32>(impl_->order.size()); }
Entity World::topologicalAt(u32 index) const  { return index < impl_->order.size() ? impl_->order[index] : kInvalidEntity; }

// ---- transforms --------------------------------------------------------------------------------

const Transform& World::localTransform(Entity e) const {
    const CLocal* l = component<CLocal>(e, kComponentLocal);
    return l ? l->xf : kIdentityTransform;
}

bool World::setLocalTransform(Entity e, const Transform& xf) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf = xf;
    bumpRevision(l->rev);
    return true;
}

bool World::setLocalPosition(Entity e, const Vec3& p) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf.position = p;
    bumpRevision(l->rev);
    return true;
}

bool World::setLocalRotation(Entity e, const Quat& q) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf.rotation = q;
    bumpRevision(l->rev);
    return true;
}

bool World::setLocalScale(Entity e, const Vec3& s) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    l->xf.scale = s;
    bumpRevision(l->rev);
    return true;
}

bool World::touchLocal(Entity e) {
    CLocal* l = component<CLocal>(e, kComponentLocal);
    if (!l) return false;
    bumpRevision(l->rev);
    return true;
}

const Mat4& World::worldMatrix(Entity e) {
    Impl& d = *impl_;
    d.composeChain(e);
    const CWorld* w = d.wor(e);
    return w ? w->m : kIdentityMatrix;
}

u32 World::worldRevision(Entity e) const {
    const CWorld* w = component<CWorld>(e, kComponentWorld);
    return w ? w->rev : 0;
}

// ---- frame -------------------------------------------------------------------------------------

u32 World::flush() {
    Impl& d = *impl_;
    if (!d.destroyQueue.empty()) {
        d.collectAndRetire();
        d.topoDirty = true;
    }
    if (d.topoDirty) d.rebuildOrder();

    u32 recomposed = 0;
    for (const Entity e : d.order) {
        if (d.composeIfStale(e)) ++recomposed;
    }
    return recomposed;
}

} // namespace aver::scene
