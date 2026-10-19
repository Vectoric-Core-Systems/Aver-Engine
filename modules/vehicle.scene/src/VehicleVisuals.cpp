// Wheel visuals. A wheel is drawn by a CHILD ENTITY the spec names (WHEEL ... node X), whose local transform the system
// writes every frame from aver_phys_vehicle_wheel_pose (steer then spin, vehicle-origin relative: the wheel mesh must have
// its axle on engine Y, forward on X and its origin at the hub), or by a SKELETON BONE (WHEEL ... bone B) driven through
// named control-rig inputs the .ocrig's SetBone ops read. Suspension travel needs no code: it is the hub moving in the pose.
#include "aver/vehicle/VehiclePhysicsSystem.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/scene/Components.hpp"

#include <string>
#include <vector>

namespace aver::vehicle {

namespace {

scene::Entity findDescendant(scene::World& w, scene::Entity root, const std::string& name) {
    if (name.empty()) return scene::kInvalidEntity;
    std::vector<scene::Entity> stack{root};
    while (!stack.empty()) {
        const scene::Entity cur = stack.back();
        stack.pop_back();
        for (scene::Entity c = w.firstChild(cur); c != scene::kInvalidEntity; c = w.nextSibling(c)) {
            if (name == w.name(c)) return c;
            stack.push_back(c);
        }
    }
    return scene::kInvalidEntity;
}

bool same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const Quat& a, const Quat& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

}  // namespace

void VehiclePhysicsSystem::Impl::resolveWheelNodes(scene::World& w, detail::Entry& e) {
    if (!e.spec) return;
    for (u32 i = 0; i < detail::kMaxWheelSlots; ++i) e.wheelNode[i] = scene::kInvalidEntity;
    for (u32 i = 0; i < e.wheelCount && i < e.spec->wheels.size(); ++i) {
        const std::string& node = e.spec->wheels[i].node;
        if (node.empty()) continue;
        e.wheelNode[i] = findDescendant(w, e.entity, node);
        // A missing child is one warning per wheel, never an error: the vehicle still drives, its wheel is simply not drawn.
        if (e.wheelNode[i] == scene::kInvalidEntity && !e.warnedNode[i]) {
            AVER_WARN("[Vehicle] entity {}: no child entity named '{}' for wheel {}", e.entity, node, i);
            e.warnedNode[i] = true;
        }
    }
}

void VehiclePhysicsSystem::Impl::writeWheelVisuals(scene::World& w, detail::Entry& e, CVehicle&) {
    if (!e.spec) return;
    const std::vector<WheelDef>& defs = e.spec->wheels;
    bool anyBone = false;
    for (u32 i = 0; i < e.wheelCount && i < defs.size(); ++i) {
        f32 p[3], q[4];
        if (!aver_phys_vehicle_wheel_pose(e.vehicle, static_cast<i32>(i), p, q)) continue;
        const Vec3 pos{p[0], p[1], p[2]};
        const Quat rot = Quat{q[0], q[1], q[2], q[3]}.normalized();

        scene::Entity node = e.wheelNode[i];
        // A node that did not exist at build (a child added afterwards) is looked for again until it is found.
        if (node == scene::kInvalidEntity && !defs[i].node.empty() && !e.warnedNode[i]) node = e.wheelNode[i] = findDescendant(w, e.entity, defs[i].node);
        if (node != scene::kInvalidEntity && w.valid(node)) {
            if (!e.wheelWritten[i] || !same(pos, e.wheelWrittenPos[i]) || !same(rot, e.wheelWrittenRot[i])) {
                Transform xf = w.localTransform(node);
                xf.position = pos;
                xf.rotation = rot;
                w.setLocalTransform(node, xf);
                e.wheelWrittenPos[i] = pos;
                e.wheelWrittenRot[i] = rot;
                e.wheelWritten[i] = true;
            }
        }

        if (rigFn && !defs[i].bone.empty()) {
            anyBone = true;
            const f32 quat[4] = {rot.x, rot.y, rot.z, rot.w};
            const f32 vec[3] = {pos.x, pos.y, pos.z};
            rigFn(e.entity, fnv1a64(std::string_view("wheel_" + defs[i].bone + "_rot")), quat, 4, rigUser);
            rigFn(e.entity, fnv1a64(std::string_view("wheel_" + defs[i].bone + "_pos")), vec, 3, rigUser);
        }
    }
    // A tracked vehicle's belts: the two track speeds, for a scrolling material or a rig.
    if (rigFn && e.spec->kind == Kind::Tracked && anyBone) {
        const f32 l = e.tel.trackLeftCmS, r = e.tel.trackRightCmS;
        rigFn(e.entity, fnv1a64(std::string_view("track_left")), &l, 1, rigUser);
        rigFn(e.entity, fnv1a64(std::string_view("track_right")), &r, 1, rigUser);
    }
}

}  // namespace aver::vehicle
