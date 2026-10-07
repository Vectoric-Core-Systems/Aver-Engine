// The entity/component world: entity lifetime, the component and field registry, hierarchy,
// transforms, and the per-frame flush.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/ComponentPool.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/Entity.hpp"
#include "aver/scene/Fields.hpp"

#include <string_view>

namespace aver::scene {

// The entity/component world. One per process, and not thread-safe.
class AVER_SCENE_API World {
public:
    // The process-global world, created on first use.
    static World& instance();

    // ---- entity lifetime ----
    // Creates an entity carrying CName, CLocal, CWorld and CHierarchy. kInvalidEntity on failure.
    Entity create(std::string_view name);
    // Creates an entity, gives it `local`, and parents it to `parent`.
    Entity create(std::string_view name, Entity parent, const Transform& local);

    // Queues `e` and its whole subtree for destruction at the next flush().
    bool destroy(Entity e);
    // True once destroy() has queued `e`, until the flush that retires it.
    bool destroyPending(Entity e) const;

    // True when the handle addresses a live entity.
    bool   valid(Entity e) const;
    // Number of live entities.
    u32    count() const;
    // The live entity at a dense index.
    Entity at(u32 denseIndex) const;    // dense over LIVE entities; indices SHIFT on flush

    // Slots whose generation ran out and can never be reused.
    u32 retiredSlotCount() const;
    // Slots waiting on the free list to be reissued.
    u32 freeSlotCount() const;

    // ---- identity ----
    // The entity's name.
    const char* name(Entity e) const;   // "" for a stale handle
    // Renames the entity and rehashes its object id.
    bool        setName(Entity e, std::string_view name);
    // The entity's persisted identity.
    u64         objectId(Entity e) const;
    // Overrides the persisted identity, for a load that carries its own.
    bool        setObjectId(Entity e, u64 id);
    // The first live entity with this name, or kInvalidEntity. A linear scan.
    Entity      find(std::string_view name) const;

    // ---- component registry ----
    // Registers a component type by name, idempotently, and returns a builder for its field table.
    ComponentBuilder registerComponent(std::string_view name, usize stride, usize align);
    // Registers a component type using T's own size and alignment.
    template <class T>
    ComponentBuilder registerComponent(std::string_view name) {
        return registerComponent(name, sizeof(T), alignof(T));
    }

    // The dense id of a registered component type.
    u32         componentId(std::string_view name) const;   // 0 when unregistered
    // Number of registered component types.
    u32         componentCount() const;
    // The component type id at an enumeration index.
    u32         componentAt(u32 index) const;
    // The registered name of a component type.
    const char* componentName(u32 type) const;
    // The byte stride of a component type.
    usize       componentSize(u32 type) const;
    // True when the type's field table passed verification.
    bool        componentVerified(u32 type) const;
    // Why verification failed.
    const char* componentVerifyError(u32 type) const;       // "" when verified

    // Adds one field to a component's table. Returns the dense field id, 0 on rejection.
    u32  addField(u32 type, const char* name, FieldKind kind, u16 offset, u8 arity = 0, bool readOnly = false);
    // Checks a component's field table covers `structBytes` with no gap or overlap.
    bool verifyComponent(u32 type, usize structBytes);

    // ---- field lookup ----
    // Resolves "Component.field" to a dense field id.
    u32              fieldId(std::string_view qualifiedName) const;   // 0 when unknown
    // The descriptor for a field id.
    const FieldDesc* field(u32 fieldId) const;                        // nullptr when unknown
    // Number of fields declared on a component type.
    u32              fieldCount(u32 type) const;
    // The field id at an index within a component's table.
    u32              fieldAt(u32 type, u32 index) const;

    // ---- component storage ----
    // Attaches zero-filled storage for `type` to `e`, or returns what it already has.
    void*          addComponent(Entity e, u32 type);
    // The component bytes, or nullptr when absent or the handle is stale.
    void*          getComponent(Entity e, u32 type);
    const void*    getComponent(Entity e, u32 type) const;
    // True when `e` carries this component.
    bool           hasComponent(Entity e, u32 type) const;
    // Drops the component from `e`.
    bool           removeComponent(Entity e, u32 type);
    // The pool storing a component type.
    ComponentPool* pool(u32 type);

    // The component bytes as T.
    template <class T>
    T* component(Entity e, u32 type) { return static_cast<T*>(getComponent(e, type)); }
    template <class T>
    const T* component(Entity e, u32 type) const { return static_cast<const T*>(getComponent(e, type)); }

    // ---- hierarchy ----
    // Reparents `e`. Refuses a cycle, a self-parent, or a parent already queued for destruction.
    // keepWorld recomputes the local transform so `e` does not visibly move.
    bool   setParent(Entity e, Entity parent, bool keepWorld = false);
    // The immediate parent, or kInvalidEntity for a root.
    Entity parent(Entity e) const;
    // The first child, or kInvalidEntity.
    Entity firstChild(Entity e) const;
    // The next sibling under the same parent, or kInvalidEntity.
    Entity nextSibling(Entity e) const;
    // Number of immediate children.
    u32    childCount(Entity e) const;
    // Distance from the root; 0 for a root.
    u32    depth(Entity e) const;

    // Length of the topological order. Parents always precede their children.
    u32    topologicalCount() const;
    // The entity at an index in the topological order.
    Entity topologicalAt(u32 index) const;

    // ---- transforms ----
    // The entity's local transform.
    const Transform& localTransform(Entity e) const;    // identity for a stale handle
    // Replaces the local transform and bumps its revision.
    bool             setLocalTransform(Entity e, const Transform& xf);
    // Sets local position and bumps the revision.
    bool             setLocalPosition(Entity e, const Vec3& p);
    // Sets local rotation and bumps the revision.
    bool             setLocalRotation(Entity e, const Quat& q);
    // Sets local scale and bumps the revision.
    bool             setLocalScale(Entity e, const Vec3& s);
    // Bumps CLocal::rev after a caller wrote the transform through the pool directly. Required, not
    // optional: flush() and worldMatrix() skip all work while no World writer has run, so a raw
    // write or a raw rev bump is never seen.
    bool             touchLocal(Entity e);

    // The entity's world matrix, composing its ancestor chain on demand if it is stale.
    const Mat4& worldMatrix(Entity e);
    // The revision of the entity's composed world matrix.
    u32         worldRevision(Entity e) const;

    // ---- frame ----
    // Retires deferred destroys, rebuilds the topological order if needed, then propagates world
    // matrices in one pass. Returns the number of entities recomposed.
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
