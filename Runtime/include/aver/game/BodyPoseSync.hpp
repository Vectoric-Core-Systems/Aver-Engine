#pragma once
// BodyPoseSync: a simulated (dynamic) rigid body moves its entity. After every fixed step each dynamic CRigidBody's
// pose is written to its entity, so OnPhysicsTick sees where the bodies are; with physics interpolation on
// (PHYSICS.INTERPOLATION, or aver_fw_interp_set_enabled) the entity is drawn between the last two
// steps by the step alpha, and put back on the real pose before the next frame's gameplay reads it.
// ANY entity can be put on the same footing (aver_fw_interp_set_mode, framework_interp_abi.h): its world pose is read around each fixed step,
// so whatever moved it in the physics tick (a character, a platform, an animation) is blended the same way. A mode of OFF keeps a rigid body out.
// Header-only: GameTick drives it for both hosts.
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_interp_abi.h"
#endif

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace aver::game {

class BodyPoseSync {
public:
    bool interpolate = false;   // the project's switch (PHYSICS.INTERPOLATION); a script can override it

    // Whether entities that inherit are blended: the script's switch when it set one, else the project's.
    bool enabled() const {
#if AVER_MODULE_FRAMEWORK
        const i32 o = aver_fw_interp_enabled();
        if (o >= 0) return o != 0;
#endif
        return interpolate;
    }

    // After a step: each dynamic body's pose becomes its entity's, and the one before it is kept for blending. Entities a script put on
    // (mode ON) have their world pose read as it stands: whatever moved them in the physics tick is the new pose.
    void capture(scene::World& w) {
        ++epoch_;
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity e = w.at(i);
            const auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
            if (!rb || rb->body == 0 || rb->motionType != AVER_PHYS_MOTION_DYNAMIC || w.destroyPending(e)) continue;
            f32 p[3], q[4];
            if (!aver_phys_body_origin(rb->body, p) || !aver_phys_body_rotation(rb->body, q)) continue;
            const Pose now{Vec3{p[0], p[1], p[2]}, Quat{q[0], q[1], q[2], q[3]}};
            record(e, now, true);
            place(w, e, now);
        }
#if AVER_MODULE_FRAMEWORK
        const i32 count = aver_fw_interp_entries(nullptr, nullptr, nullptr, 0);
        if (count > 0) {
            ent_.resize(static_cast<size_t>(count));
            mode_.resize(static_cast<size_t>(count));
            serial_.resize(static_cast<size_t>(count));
            const i32 got = aver_fw_interp_entries(ent_.data(), mode_.data(), serial_.data(), count);
            for (i32 k = 0; k < got && k < count; ++k) {
                if (mode_[static_cast<size_t>(k)] != AVER_FW_INTERP_ON) continue;
                const scene::Entity e = static_cast<scene::Entity>(ent_[static_cast<size_t>(k)]);
                if (!w.valid(e) || w.destroyPending(e)) continue;
                if (const auto done = poses_.find(e); done != poses_.end() && done->second.epoch == epoch_) continue;   // a body, already read
                record(e, worldPose(w, e), false);
            }
        }
#endif
        // A track whose body is gone, or that a script took off, is not blended any more.
        for (auto it = poses_.begin(); it != poses_.end();) {
            if (it->second.epoch != epoch_) {
                if (it->second.published) forget(it->first);
                it = poses_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Before gameplay reads the world: interpolated entities go back to their real (last step's) pose.
    void restore(scene::World& w) {
        if (!drawnBlended_) return;
        drawnBlended_ = false;
        for (auto& [entity, t] : poses_) {
            if (!t.shown) continue;
            t.shown = false;
            if (w.valid(entity) && !w.destroyPending(entity)) place(w, entity, t.cur);
        }
    }

    // For drawing: each entity between its last two steps, `alpha` (0..1) of the way from the earlier.
    void present(scene::World& w, f32 alpha) {
        const bool on = enabled();
        for (auto it = poses_.begin(); it != poses_.end();) {
            const scene::Entity entity = it->first;
            Track& t = it->second;
            if (!w.valid(entity) || w.destroyPending(entity)) {
                forget(entity);
                it = poses_.erase(it);
                continue;
            }
            if (blends(entity, t, on)) {
#if AVER_MODULE_FRAMEWORK
                // A teleport since the last capture: the entity is where the script put it, and that is what is drawn.
                const i32 serial = aver_fw_interp_reset_serial(static_cast<i32>(entity));
                if (serial != t.serial) {
                    t.serial = serial;
                    t.prev = t.cur = worldPose(w, entity);
                }
#endif
                const Vec3 p = t.prev.p + (t.cur.p - t.prev.p) * alpha;
                const Pose shown{p, Quat::slerp(t.prev.q, t.cur.q, alpha)};
                place(w, entity, shown);
                publish(entity, shown);
                t.shown = true;
                t.published = true;
                drawnBlended_ = true;
            } else if (t.published) {
                t.published = false;
                forget(entity);
            }
            ++it;
        }
    }

    // The pose drawn for an entity, or false when none is (it is not blended, or has not been drawn yet).
    static bool shownPose(scene::Entity entity, Vec3& position, Quat& rotation) {
#if AVER_MODULE_FRAMEWORK
        f32 p[3], q[4];
        if (!aver_fw_interp_pose(static_cast<i32>(entity), p, q)) return false;
        position = Vec3{p[0], p[1], p[2]};
        rotation = Quat{q[0], q[1], q[2], q[3]};
        return true;
#else
        (void)entity; (void)position; (void)rotation;
        return false;
#endif
    }

    void clear() { poses_.clear(); drawnBlended_ = false; }

private:
    struct Pose { Vec3 p; Quat q; };
    struct Track {
        Pose prev{}, cur{};
        bool generic = false;     // a script put it on (not a dynamic body)
        bool seen = false;
        bool shown = false;       // present() drew it blended, so restore() owes it its real pose
        bool published = false;   // the framework holds the pose present() drew
        i32 serial = 0;           // the teleport serial seen at the last capture
        u32 epoch = 0;
    };
    std::unordered_map<scene::Entity, Track> poses_;
    std::vector<i32> ent_, mode_, serial_;
    u32 epoch_ = 0;
    bool drawnBlended_ = false;   // some entity is on its blended pose, so restore() has work

    // Whether this track is drawn blended now: ON always, OFF never, otherwise a body follows the switch and a script's entity is ON already.
    static bool blends(scene::Entity entity, const Track& t, bool on) {
        (void)t;
#if AVER_MODULE_FRAMEWORK
        const i32 mode = aver_fw_interp_mode(static_cast<i32>(entity));
        if (mode == AVER_FW_INTERP_ON) return true;
        if (mode == AVER_FW_INTERP_OFF) return false;
#else
        (void)entity;
#endif
        return on && !t.generic;
    }

    void record(scene::Entity e, const Pose& now, bool body) {
        Track& t = poses_[e];
        const i32 serial =
#if AVER_MODULE_FRAMEWORK
            aver_fw_interp_reset_serial(static_cast<i32>(e));
#else
            0;
#endif
        if (!t.seen || serial != t.serial) t.prev = now;   // first sight, or a teleport: nothing to blend from
        else t.prev = t.cur;
        t.cur = now;
        t.seen = true;
        t.serial = serial;
        t.generic = !body;
        t.epoch = epoch_;
    }

    static void publish(scene::Entity e, const Pose& shown) {
#if AVER_MODULE_FRAMEWORK
        const f32 p[3] = {shown.p.x, shown.p.y, shown.p.z};
        const f32 q[4] = {shown.q.x, shown.q.y, shown.q.z, shown.q.w};
        aver_fw_interp_publish(static_cast<i32>(e), p, q);
#else
        (void)e; (void)shown;
#endif
    }

    static void forget(scene::Entity e) {
#if AVER_MODULE_FRAMEWORK
        aver_fw_interp_publish(static_cast<i32>(e), nullptr, nullptr);
#else
        (void)e;
#endif
    }

    static Pose worldPose(scene::World& w, scene::Entity e) {
        const Transform world = transformFromMatrix(w.worldMatrix(e));
        return Pose{world.position, world.rotation};
    }

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
