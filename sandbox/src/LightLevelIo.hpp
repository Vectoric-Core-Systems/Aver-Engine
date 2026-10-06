#pragma once
// .ocworld LIGHT record <-> scene::CLight + transform, for the editor's level load and save.
// Header-only so a test can round-trip it without the editor. docs/rendering/LIGHTS.md.
#include "EditorEuler.hpp"
#include "LightDetails.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"

#include <cmath>
#include <string>
#include <unordered_map>

namespace aver::editor {

// id -> content-relative path for every .ies / cookie image under a project, for turning a CLight's
// stored ObjectIds back into the paths the file keeps.
struct LightAssetPaths {
    std::unordered_map<u64, std::string> byId;

    static LightAssetPaths scan(const std::string& contentDir) {
        LightAssetPaths p;
        for (const LightAssetChoice& c : listLightAssets(contentDir, true)) p.byId.emplace(c.id, c.label);
        for (const LightAssetChoice& c : listLightAssets(contentDir, false)) p.byId.emplace(c.id, c.label);
        return p;
    }
    // The path for `id`, or empty when the id is 0 or names nothing in this project.
    std::string find(i64 id) const {
        if (id == 0) return {};
        const auto it = byId.find(static_cast<u64>(id));
        return it == byId.end() ? std::string() : it->second;
    }
};

inline scene::CLight lightFromRecord(const fmt::OcLight& r) {
    scene::CLight c{};
    c.kind = r.kind == fmt::OcLightKind::Spot ? scene::kLightSpot
           : r.kind == fmt::OcLightKind::Rect ? scene::kLightRect : scene::kLightPoint;
    for (int i = 0; i < 3; ++i) c.colour[i] = static_cast<f32>(r.colour[i]);
    c.intensityLux = static_cast<f32>(r.intensityCd);
    c.rangeCm = static_cast<f32>(r.rangeCm);
    c.innerCos = lightConeCos(static_cast<f32>(r.innerDeg));
    c.outerCos = lightConeCos(static_cast<f32>(r.outerDeg));
    c.widthCm = static_cast<f32>(r.widthCm);
    c.heightCm = static_cast<f32>(r.heightCm);
    c.sourceRadiusCm = static_cast<f32>(r.radiusCm);
    c.iesProfile = r.ies.empty() ? 0 : static_cast<i64>(lightAssetId(r.ies));
    c.cookie = r.cookie.empty() ? 0 : static_cast<i64>(lightAssetId(r.cookie));
    c.flags = (r.castShadows ? 0 : scene::kLightNoShadows) | (r.iesPeak ? scene::kLightIesPeak : 0);
    return c;
}

// The record's pose as a local transform (yaw/pitch/roll degrees, the placement convention).
inline Transform transformFromRecord(const fmt::OcLight& r) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.z)};
    xf.rotation = quatFromEulerDeg(Vec3{static_cast<f32>(r.roll), static_cast<f32>(r.pitch), static_cast<f32>(r.yaw)});
    return xf;
}

// One entity's light as a record. `world` is its WORLD transform (records carry no parent).
inline fmt::OcLight recordFromLight(const scene::CLight& c, const Transform& world, const std::string& name,
                                    const LightAssetPaths& assets) {
    fmt::OcLight r;
    r.name = name;
    r.kind = c.kind == scene::kLightSpot ? fmt::OcLightKind::Spot
           : c.kind == scene::kLightRect ? fmt::OcLightKind::Rect : fmt::OcLightKind::Point;
    r.x = world.position.x; r.y = world.position.y; r.z = world.position.z;
    const Vec3 e = eulerDegFromQuat(world.rotation);
    r.roll = e.x; r.pitch = e.y; r.yaw = e.z;
    for (int i = 0; i < 3; ++i) r.colour[i] = c.colour[i];
    r.intensityCd = c.intensityLux;
    r.rangeCm = c.rangeCm;
    r.innerDeg = lightConeDegrees(c.innerCos);
    r.outerDeg = lightConeDegrees(c.outerCos);
    r.widthCm = c.widthCm > 0.0f ? c.widthCm : 100.0;
    r.heightCm = c.heightCm > 0.0f ? c.heightCm : 100.0;
    r.radiusCm = c.sourceRadiusCm > 0.0f ? c.sourceRadiusCm : 1.0;
    r.ies = assets.find(c.iesProfile);
    r.cookie = assets.find(c.cookie);
    r.castShadows = (c.flags & scene::kLightNoShadows) == 0;
    r.iesPeak = (c.flags & scene::kLightIesPeak) != 0;
    return r;
}

} // namespace aver::editor
