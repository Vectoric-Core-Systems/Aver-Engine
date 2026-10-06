// LevelDecals: the .ocworld DECAL record -> a live CDecal entity, for BOTH hosts (the editor's load
// path and the packaged game's). The record -> component direction only; writing records back is the
// editor's (sandbox/src/DecalLevelIo.hpp, which delegates here for the part they share so the two
// cannot disagree about what a record means). docs/rendering/DECALS.md.
#pragma once
#include "aver/core/Hash.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace aver::game {

// A content-relative image path as the id CDecal stores: fnv1a64 of the forward-slash path, the same
// id space every other project asset uses.
inline u64 decalImageId(std::string_view contentRelativePath) {
    std::string p(contentRelativePath);
    std::replace(p.begin(), p.end(), '\\', '/');
    return fnv1a64(p);
}

inline scene::CDecal decalFromRecord(const fmt::OcDecal& r) {
    scene::CDecal c{};
    for (int i = 0; i < 3; ++i) { c.sizeCm[i] = static_cast<f32>(r.sizeCm[i]); c.tint[i] = static_cast<f32>(r.tint[i]); }
    c.transparency = 1.0f - static_cast<f32>(std::min(std::max(r.opacity, 0.0), 1.0));
    c.normalStrength = static_cast<f32>(r.normalStrength);
    c.roughness = static_cast<f32>(r.roughness);
    c.metallic = static_cast<f32>(r.metallic);
    c.edgeFade = static_cast<f32>(r.edgeFade);
    c.angleFadeStartDeg = static_cast<f32>(r.angleStartDeg);
    c.angleFadeEndDeg = static_cast<f32>(r.angleEndDeg);
    c.fadeDistanceCm = static_cast<f32>(r.fadeDistanceCm);
    c.sortOrder = r.order;
    c.baseTexture = r.base.empty() ? 0 : static_cast<i64>(decalImageId(r.base));
    c.normalTexture = r.normal.empty() ? 0 : static_cast<i64>(decalImageId(r.normal));
    c.ormTexture = r.orm.empty() ? 0 : static_cast<i64>(decalImageId(r.orm));
    for (int i = 0; i < 2; ++i) { c.uvScale[i] = static_cast<f32>(r.uvScale[i]); c.uvOffset[i] = static_cast<f32>(r.uvOffset[i]); }
    c.flags = (r.noColour ? scene::kDecalNoColour : 0u) | (r.noNormal ? scene::kDecalNoNormal : 0u) |
              (r.noRoughness ? scene::kDecalNoRoughness : 0u) | (r.disabled ? scene::kDecalDisabled : 0u);
    return c;
}

// The record's pose as a local transform (yaw/pitch/roll degrees, the placement convention).
inline Transform transformFromDecalRecord(const fmt::OcDecal& r) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.z)};
    xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(r.roll), static_cast<f32>(r.pitch), static_cast<f32>(r.yaw)});
    xf.scale = Vec3{static_cast<f32>(r.sx), static_cast<f32>(r.sy), static_cast<f32>(r.sz)};
    return xf;
}

// One bare CDecal entity per record (the instantiator does not know them). Returns the entities in
// record order, skipping any the world refused.
inline std::vector<scene::Entity> spawnLevelDecals(scene::World& world, const std::vector<fmt::OcDecal>& records) {
    std::vector<scene::Entity> out;
    for (const fmt::OcDecal& r : records) {
        const scene::Entity e = world.create(r.name.empty() ? std::string("Decal") : r.name, scene::kInvalidEntity,
                                             transformFromDecalRecord(r));
        if (e == scene::kInvalidEntity) continue;
        if (auto* c = static_cast<scene::CDecal*>(world.addComponent(e, scene::kComponentDecal)))
            *c = decalFromRecord(r);
        out.push_back(e);
    }
    return out;
}

} // namespace aver::game
