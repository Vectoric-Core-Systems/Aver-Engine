#pragma once
// BodyPoseSync: a simulated (dynamic) rigid body moves its entity. After every fixed step each dynamic CRigidBody's
// pose is written to its entity, so OnPhysicsTick sees where the bodies are; with physics interpolation on
// (PHYSICS.INTERPOLATION, Godot's physics/common/physics_interpolation) the entity is drawn between the last two
// steps by the step alpha, and put back on the real pose before the next frame's gameplay reads it.
// Header-only: GameTick drives it for both hosts.
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <unordered_map>

namespace aver::game {

class BodyPoseSync {
public:
    bool interpolate = false;

    // After a step: each dynamic body's pose becomes its entity's, and the one before it is kept for blending.
    void capture(scene::World& w) {
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity e = w.at(i);
            const auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
            if (!rb || rb->body == 0 || rb->motionType != AVER_PHYS_MOTION_DYNAMIC || w.destroyPending(e)) continue;
            f32 p[3], q[4];
            if (!aver_phys_body_origin(rb->body, p) || !aver_phys_body_rotation(rb->body, q)) { poses_.erase(rb->body); continue; }
            Track& t = poses_[rb->body];
            const Pose now{Vec3{p[0], p[1], p[2]}, Quat{q[0], q[1], q[2], q[3]}};
            t.prev = t.seen ? t.cur : now;
            t.cur = now;
            t.seen = true;
            t.entity = e;
            place(w, e, now);
        }
    }

    // Before gameplay reads the world: interpolated entities go back to their real (last step's) pose.
    void restore(scene::World& w) {
        if (!interpolate) return;
        for (auto& [body, t] : poses_)
            if (w.valid(t.entity) && !w.destroyPending(t.entity)) place(w, t.entity, t.cur);
    }

    // For drawing: each entity between its last two steps, `alpha` (0..1) of the way from the earlier.
    void present(scene::World& w, f32 alpha) {
        if (!interpolate) return;
        for (auto it = poses_.begin(); it != poses_.end();) {
            Track& t = it->second;
            if (!w.valid(t.entity) || w.destroyPending(t.entity)) { it = poses_.erase(it); continue; }
            const Vec3 p = t.prev.p + (t.cur.p - t.prev.p) * alpha;
            place(w, t.entity, Pose{p, Quat::slerp(t.prev.q, t.cur.q, alpha)});
            ++it;
        }
    }

    void clear() { poses_.clear(); }

private:
    struct Pose { Vec3 p; Quat q; };
    struct Track { Pose prev{}, cur{}; scene::Entity entity = scene::kInvalidEntity; bool seen = false; };
    std::unordered_map<i32, Track> poses_;

    // Sets the entity's world pose (its scale kept): directly for a root, through the parent's world matrix otherwise.
    static void place(scene::World& w, scene::Entity e, const Pose& pose) {
        Transform local = w.localTransform(e);
        const scene::Entity parent = w.parent(e);
        if (parent == scene::kInvalidEntity) {
            local.position = pose.p;
            local.rotation = pose.q;
        } else {
            Transform world = local;
            world.position = pose.p;
            world.rotation = pose.q;
            const Transform rel = transformFromMatrix(world.toMatrix() * w.worldMatrix(parent).inverse());
            local.position = rel.position;
            local.rotation = rel.rotation;
        }
        w.setLocalTransform(e, local);
    }
};

// The one both hosts' frame loop uses.
inline BodyPoseSync& bodyPoseSync() { static BodyPoseSync s; return s; }

}  // namespace aver::game
