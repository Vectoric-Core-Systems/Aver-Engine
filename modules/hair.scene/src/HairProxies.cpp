// HairProxies.cpp -- the kinematic convex-hull stand-ins for a character's head, neck and shoulders. See HairProxies.hpp.
#include "HairProxies.hpp"

#include "aver/core/Log.hpp"
#include "aver/hair/HairProxy.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"

#include <cctype>
#include <cmath>
#include <cstddef>

namespace aver::hair::detail {

namespace {

bool equalNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// The bone of that name: exact first, then without regard to case. -1 when the rig has none.
i32 findBone(const fmt::OcSkeleton& skel, const std::string& name) {
    if (name.empty()) return -1;
    for (usize i = 0; i < skel.bones.size(); ++i)
        if (skel.bones[i].name == name) return static_cast<i32>(i);
    for (usize i = 0; i < skel.bones.size(); ++i)
        if (equalNoCase(skel.bones[i].name, name)) return static_cast<i32>(i);
    return -1;
}

} // namespace

void setupHairLayers(i32 hairLayer, i32 proxyLayer, i32 freeLayer) {
    for (i32 x = 0; x < AVER_PHYS_LAYER_COUNT; ++x) {
        aver_phys_set_layer_collision(proxyLayer, x, 0);
        aver_phys_set_layer_collision(freeLayer, x, 0);
    }
    // Set last: the symmetric (hair, proxy) pair is the one thing a proxy touches.
    aver_phys_set_layer_collision(proxyLayer, hairLayer, 1);
}

u32 createProxies(const fmt::OcHairData& groom, const fmt::OcSkeleton& skel, f32 uniformScale, i32 proxyLayer, std::vector<ProxyBody>& out,
                  std::vector<std::string>* missing) {
    std::vector<fmt::OcHairProxy> list = groom.proxies;
    if (list.empty()) {
        std::vector<std::string> names;
        names.reserve(skel.bones.size());
        for (const fmt::OcBone& b : skel.bones) names.push_back(b.name);
        list = defaultHumanoidProxies(names, missing);
    }
    u32 made = 0;
    for (const fmt::OcHairProxy& p : list)
        if (createProxy(p, skel, uniformScale, proxyLayer, out)) ++made;
    return made;
}

bool createProxy(const fmt::OcHairProxy& p, const fmt::OcSkeleton& skel, f32 uniformScale, i32 proxyLayer, std::vector<ProxyBody>& out) {
    const f32 s = uniformScale > 1.0e-4f ? uniformScale : 1.0f;
    const i32 bone = findBone(skel, p.bone);
    if (bone < 0) {
        AVER_WARN("[Hair] proxy '{}' names bone '{}', which the skeleton does not have; it is skipped", p.name, p.bone);
        return false;
    }
    const f32 size[3] = {p.sizeCm[0] * s, p.sizeCm[1] * s, p.sizeCm[2] * s};
    const std::vector<f32> points = proxyHullPoints(p.shape, size);
    if (points.empty()) return false;
    // Created dynamic (so Jolt gives it motion properties without a rebuild), then made kinematic in the same breath, before any step runs.
    const i32 body = aver_phys_add_convex_hull(points.data(), static_cast<i32>(points.size() / 3), 0.0f, 0.0f, 0.0f, 1, 1.0f);
    if (body == 0) {
        AVER_WARN("[Hair] the physics layer would not make the '{}' proxy hull", p.name);
        return false;
    }
    aver_phys_body_set_motion_type(body, AVER_PHYS_MOTION_KINEMATIC);
    aver_phys_body_set_layer(body, proxyLayer);
    ProxyBody b;
    b.body = body;
    b.bone = static_cast<u32>(bone);
    b.offsetCm = Vec3(p.offsetCm[0] * s, p.offsetCm[1] * s, p.offsetCm[2] * s);
    b.shape = p.shape;
    for (int i = 0; i < 3; ++i) b.sizeCm[i] = size[i];
    out.push_back(b);
    return true;
}

void destroyProxiesFrom(std::vector<ProxyBody>& bodies, usize first) {
    for (usize i = first; i < bodies.size(); ++i)
        if (bodies[i].body != 0) aver_phys_remove_body(bodies[i].body);
    if (first < bodies.size()) bodies.erase(bodies.begin() + static_cast<std::ptrdiff_t>(first), bodies.end());
}

void moveProxies(std::vector<ProxyBody>& bodies, const std::vector<Mat4>& model, const Mat4& world, f32 dt) {
    for (ProxyBody& b : bodies) {
        if (b.body == 0 || b.bone >= model.size()) continue;
        // Row-vector convention: a point in the bone's frame goes through the bone's model matrix, then the owner's world.
        const Mat4 m = Mat4::translation(b.offsetCm) * model[b.bone] * world;
        const Transform t = transformFromMatrix(m);
        if (!std::isfinite(t.position.x) || !std::isfinite(t.position.y) || !std::isfinite(t.position.z)) continue;
        if (!b.placed || !(dt > 0.0f)) {
            aver_phys_body_set_position(b.body, t.position.x, t.position.y, t.position.z);
            aver_phys_body_set_rotation(b.body, t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w);
            b.placed = true;
        } else {
            aver_phys_body_move_kinematic(b.body, t.position.x, t.position.y, t.position.z, t.rotation.x, t.rotation.y, t.rotation.z,
                                          t.rotation.w, dt);
        }
    }
}

void destroyProxies(std::vector<ProxyBody>& bodies) {
    for (ProxyBody& b : bodies)
        if (b.body != 0) aver_phys_remove_body(b.body);
    bodies.clear();
}

} // namespace aver::hair::detail
