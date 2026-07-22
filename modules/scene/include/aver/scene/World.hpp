#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/ComponentPool.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/Entity.hpp"
#include "aver/scene/Fields.hpp"

#include <string_view>

namespace aver::scene {

// The entity/component world. ONE per process, for the same reason scripting_abi.h has no module
// name to P/Invoke into: inventing a handle for a thing that can only have one instance costs every
// call site a parameter and buys nothing until multi-world exists, at which point it is an additive
// entry point rather than a break.
//
// Not thread-safe, matching the rest of the module tier: every entry point is called on the frame
// thread between flush points, and a lock here would be taken millions of times a second to protect
// against a caller that does not exist.
//
// Pimpl for the reason MaterialLibrary is: a dllexported class with standard-library members exports
// their layout too (MSVC C4251), and only this DLL may allocate or free them.
class AVER_SCENE_API World {
public:
    static World& instance();

    // ---- entity lifetime ----
    // Every entity gets CName, CLocal, CWorld and CHierarchy at birth. An entity without a transform
    // is not a thing this module has a use for, and making it optional would mean every transform
    // accessor on the ABI could fail on a freshly created entity for a reason the caller cannot see.
    Entity create(std::string_view name);
    Entity create(std::string_view name, Entity parent, const Transform& local);

    // Deferred to the next flush() and takes the whole subtree with it: an entity destroyed inside a
    // tick must not invalidate the array that tick is walking, and a consumer that sweeps its own
    // instance list with valid() must see the entity for the rest of the frame it died in.
    bool destroy(Entity e);
    bool destroyPending(Entity e) const;

    bool   valid(Entity e) const;
    u32    count() const;
    Entity at(u32 denseIndex) const;    // dense over LIVE entities; indices SHIFT on flush

    // Surfaced because a world that churns entities hard enough to retire slots in bulk has a bug
    // worth seeing, and the status bar is where it becomes visible.
    u32 retiredSlotCount() const;
    u32 freeSlotCount() const;

    // ---- identity ----
    const char* name(Entity e) const;   // "" for a stale handle
    bool        setName(Entity e, std::string_view name);
    u64         objectId(Entity e) const;
    bool        setObjectId(Entity e, u64 id);
    Entity      find(std::string_view name) const;

    // ---- component registry ----
    // Idempotent by name: re-registering a component clears its field table and returns a builder on
    // the SAME type id, so a hot reload that redeclares a component does not strand the entities that
    // already carry it. A qualified field name that already has a dense id keeps it.
    ComponentBuilder registerComponent(std::string_view name, usize stride, usize align);
    template <class T>
    ComponentBuilder registerComponent(std::string_view name) {
        return registerComponent(name, sizeof(T), alignof(T));
    }

    u32         componentId(std::string_view name) const;   // 0 when unregistered
    u32         componentCount() const;
    u32         componentAt(u32 index) const;
    const char* componentName(u32 type) const;
    usize       componentSize(u32 type) const;
    bool        componentVerified(u32 type) const;
    const char* componentVerifyError(u32 type) const;       // "" when verified

    // The two calls ComponentBuilder is sugar over; the framework's script-declared components come
    // through here too. addField returns the dense field id, 0 on rejection.
    u32  addField(u32 type, const char* name, FieldKind kind, u16 offset, u8 arity = 0, bool readOnly = false);
    bool verifyComponent(u32 type, usize structBytes);

    // ---- field lookup ----
    u32              fieldId(std::string_view qualifiedName) const;   // 0 when unknown
    const FieldDesc* field(u32 fieldId) const;                        // nullptr when unknown
    u32              fieldCount(u32 type) const;
    u32              fieldAt(u32 type, u32 index) const;

    // ---- component storage ----
    // void* never crosses the C ABI; it crosses a header, in process, between two modules that were
    // compiled together — which is a different thing.
    void*          addComponent(Entity e, u32 type);
    void*          getComponent(Entity e, u32 type);
    const void*    getComponent(Entity e, u32 type) const;
    bool           hasComponent(Entity e, u32 type) const;
    bool           removeComponent(Entity e, u32 type);
    ComponentPool* pool(u32 type);

    template <class T>
    T* component(Entity e, u32 type) { return static_cast<T*>(getComponent(e, type)); }
    template <class T>
    const T* component(Entity e, u32 type) const { return static_cast<const T*>(getComponent(e, type)); }

    // ---- hierarchy ----
    // Refuses when `parent` is `e` or a descendant of it: a cycle would hang the topological sort, so
    // it is rejected here rather than detected later. keepWorld recomputes the child's local
    // transform so it does not visibly move.
    bool   setParent(Entity e, Entity parent, bool keepWorld = false);
    Entity parent(Entity e) const;
    Entity firstChild(Entity e) const;
    Entity nextSibling(Entity e) const;
    u32    childCount(Entity e) const;
    u32    depth(Entity e) const;

    // Parents always precede their children. Rebuilt only when a parent link moves, never per frame.
    u32    topologicalCount() const;
    Entity topologicalAt(u32 index) const;

    // ---- transforms ----
    const Transform& localTransform(Entity e) const;    // identity for a stale handle
    bool             setLocalTransform(Entity e, const Transform& xf);
    bool             setLocalPosition(Entity e, const Vec3& p);
    bool             setLocalRotation(Entity e, const Quat& q);
    bool             setLocalScale(Entity e, const Vec3& s);
    // Bumps CLocal::rev after a caller has written the transform through the pool directly. Without
    // it such a write is invisible to the propagation pass, which is the one way to get a stale
    // world matrix out of this design.
    bool             touchLocal(Entity e);

    // Composes on demand when this entity is stale, walking up to the nearest ancestor whose
    // revisions agree and composing down. Bounded by hierarchy depth, and it is the same arithmetic
    // the batched pass runs, so a caller reading mid-frame gets exactly what the next flush() would
    // have written. Identity for a stale handle.
    const Mat4& worldMatrix(Entity e);
    u32         worldRevision(Entity e) const;

    // ---- frame ----
    // Retires deferred destroys, rebuilds the topological order if a parent link moved, then
    // propagates world matrices in one linear pass. Returns the number of entities recomposed.
    u32 flush();

private:
    World();
    ~World();
    World(const World&)            = delete;
    World& operator=(const World&) = delete;

    struct Impl;
    Impl* impl_;
};

} // namespace aver::scene
