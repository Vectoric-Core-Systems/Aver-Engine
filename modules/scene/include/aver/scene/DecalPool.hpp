// The runtime spawn API for gameplay decals (impacts, footprints, scorch marks): a fixed-size pool of
// CDecal entities that are recycled, oldest first, instead of being created and destroyed. Header-only
// over World; aver_decal_* in decal_abi.h is the C ABI for scripts. docs/rendering/DECALS.md.
#pragma once
#include "aver/scene/DecalGather.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver::scene {

// A projector pose for a surface hit: the decal looks INTO the surface (its +X is -normal), with its
// up (+Z) as close to `up` as the normal allows, then rolled about the projection axis.
inline Quat decalRotationForSurface(const Vec3& surfaceNormal, f32 rollRad = 0.0f, const Vec3& up = Vec3{0, 0, 1}) {
    const Vec3 x = (surfaceNormal * -1.0f).getSafeNormal();
    Vec3 z = up - x * dot(up, x);
    if (z.sizeSquared() < 1e-6f) {   // looking straight along up: any perpendicular will do
        const Vec3 alt = std::fabs(x.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        z = alt - x * dot(alt, x);
    }
    z = z.getSafeNormal();
    const Vec3 y = cross(z, x).getSafeNormal();   // +Y right: cross(Z, X) with this engine's handedness
    z = cross(x, y);
    Mat4 m;
    m.m[0][0] = x.x; m.m[0][1] = x.y; m.m[0][2] = x.z;
    m.m[1][0] = y.x; m.m[1][1] = y.y; m.m[1][2] = y.z;
    m.m[2][0] = z.x; m.m[2][1] = z.y; m.m[2][2] = z.z;
    Quat q = transformFromMatrix(m).rotation;
    if (std::fabs(rollRad) > 1e-6f) q = q * Quat::fromAxisAngle(Vec3{1, 0, 0}, rollRad);   // about local X, applied first
    return q.normalized();
}

// What a gameplay decal looks like. Zero fields mean "default", as on CDecal.
struct DecalSpawn {
    Vec3 position{0, 0, 0};
    Quat rotation = Quat::identity();   // projects along local +X; see decalRotationForSurface
    Vec3 scale{1, 1, 1};
    CDecal params;                      // everything else; flags/age/serial are overwritten
};

// A pool of up to `capacity` pooled decals. Entities are created on first need and then only ever
// disabled and re-armed, so spawning in a firefight allocates nothing once the pool has warmed up.
class DecalPool {
public:
    explicit DecalPool(u32 capacity = 256) : capacity_(capacity) {}

    u32 capacity() const { return capacity_; }
    u32 activeCount() const {
        u32 n = 0;
        for (const Slot& s : slots_) n += s.active ? 1u : 0u;
        return n;
    }
    // Slots the pool has actually created (<= capacity).
    u32 createdCount() const { return static_cast<u32>(slots_.size()); }
    // How many spawns took the place of a still-visible decal because the pool was full.
    u32 recycledCount() const { return recycled_; }

    bool owns(Entity e) const { return find(e) != nullptr; }
    bool isActive(Entity e) const { const Slot* s = find(e); return s && s->active; }

    // Spawns a decal. kInvalidEntity when the pool has capacity 0 or the world refused an entity.
    // When every slot is in use the OLDEST visible decal is recycled.
    Entity spawn(World& world, const DecalSpawn& sp) {
        if (capacity_ == 0) return kInvalidEntity;
        Slot* slot = acquire(world);
        if (!slot) return kInvalidEntity;

        world.setLocalTransform(slot->entity, Transform{sp.position, sp.rotation.normalized(), sp.scale});
        CDecal* c = static_cast<CDecal*>(world.addComponent(slot->entity, kComponentDecal));
        if (!c) return kInvalidEntity;
        *c = sp.params;
        c->flags = (sp.params.flags & ~kDecalDisabled) | kDecalPooled;
        c->age = 0.0f;
        c->serial = ++serial_;
        slot->active = true;
        slot->serial = c->serial;
        return slot->entity;
    }

    // Gives a decal back to the pool now. False when `e` is not one of this pool's active decals.
    bool release(World& world, Entity e) {
        Slot* s = find(e);
        if (!s || !s->active) return false;
        disable(world, *s);
        return true;
    }

    // Releases every decal (a level unload, a "clear impacts" command).
    void clear(World& world) {
        for (Slot& s : slots_) if (s.active) disable(world, s);
    }

    // Destroys the pool's entities and forgets them. Call before the world is torn down or reloaded.
    void destroyAll(World& world) {
        for (Slot& s : slots_) if (world.valid(s.entity)) world.destroy(s.entity);
        slots_.clear();
    }

    // Advances every lifetime by dt and takes expired decals back. Call once per game frame.
    void tick(World& world, f32 dt) {
        expired_.clear();
        tickDecalLifetimes(world, dt, &expired_);
        for (const Entity e : expired_)
            if (Slot* s = find(e)) s->active = false;
    }

private:
    struct Slot {
        Entity entity = kInvalidEntity;
        bool active = false;
        u32 serial = 0;
    };

    const Slot* find(Entity e) const {
        for (const Slot& s : slots_) if (s.entity == e) return &s;
        return nullptr;
    }
    Slot* find(Entity e) {
        for (Slot& s : slots_) if (s.entity == e) return &s;
        return nullptr;
    }

    static void disable(World& world, Slot& s) {
        if (CDecal* c = world.component<CDecal>(s.entity, kComponentDecal)) c->flags |= kDecalDisabled;
        s.active = false;
    }

    Slot* acquire(World& world) {
        // Entities the world retired under us (a level unload) are forgotten, not reused.
        slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                    [&](const Slot& s) { return !world.valid(s.entity) || world.destroyPending(s.entity); }),
                     slots_.end());
        for (Slot& s : slots_) if (!s.active) return &s;
        if (slots_.size() < capacity_) {
            const Entity e = world.create("Decal");
            if (e == kInvalidEntity) return nullptr;
            if (!world.addComponent(e, kComponentDecal)) return nullptr;
            slots_.push_back(Slot{e, false, 0});
            return &slots_.back();
        }
        Slot* oldest = &slots_.front();
        for (Slot& s : slots_) if (s.serial < oldest->serial) oldest = &s;
        ++recycled_;
        return oldest;
    }

    u32 capacity_ = 0;
    u32 serial_ = 0;
    u32 recycled_ = 0;
    std::vector<Slot> slots_;
    std::vector<Entity> expired_;
};

} // namespace aver::scene
