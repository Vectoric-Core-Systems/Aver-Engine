// LevelLights: the .ocworld LIGHT record -> a live CLight entity, for BOTH hosts (the editor's load path and the
// packaged game's). The record -> component direction plus the pure helpers it needs (asset ids, cone
// conversions, asset listing); writing records back stays the editor's (sandbox/src/LightLevelIo.hpp, which
// re-exports these names so no editor call site changes). docs/rendering/LIGHTS.md, docs/LEVEL_INSTANCES.md 2.9.
//
// Before this header the packaged game spawned no LIGHT record at all: only the editor did.
#pragma once
#if AVER_MODULE_SCENE

#include "aver/core/Hash.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aver::game {

// One pickable file: content-relative path (forward slashes) and the ObjectId a CLight stores for it.
struct LightAssetChoice {
    std::string label;
    u64 id = 0;
};

// The id a CLight stores for a content-relative path: fnv1a64 of the forward-slash path, the same id
// space CMeshRenderer::mesh and CParticleEmitter::effect use.
inline u64 lightAssetId(std::string_view contentRelativePath) {
    std::string p(contentRelativePath);
    std::replace(p.begin(), p.end(), '\\', '/');
    return fnv1a64(p);
}

inline bool lightHasExt(const std::filesystem::path& p, bool ies) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ies) return e == ".ies";
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".webp" || e == ".svg" || e == ".exr";
}

// Every .ies (or cookie-capable image) under `contentDir`, sorted by path. Capped so a huge texture
// library cannot stall the editor; `truncated` says when the cap bit.
inline std::vector<LightAssetChoice> listLightAssets(const std::string& contentDir, bool ies,
                                                     bool* truncated = nullptr) {
    constexpr usize kCap = 4000;
    std::vector<LightAssetChoice> out;
    if (truncated) *truncated = false;
    std::error_code ec;
    if (contentDir.empty() || !std::filesystem::is_directory(contentDir, ec)) return out;
    for (std::filesystem::recursive_directory_iterator it(contentDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !lightHasExt(it->path(), ies)) continue;
        if (out.size() >= kCap) { if (truncated) *truncated = true; break; }
        std::string rel = std::filesystem::relative(it->path(), contentDir, ec).generic_string();
        if (ec || rel.empty()) continue;
        out.push_back({rel, lightAssetId(rel)});
    }
    std::sort(out.begin(), out.end(), [](const LightAssetChoice& a, const LightAssetChoice& b) { return a.label < b.label; });
    return out;
}

// Cone half-angles are stored as cosines; the panel edits degrees.
inline f32 lightConeDegrees(f32 cosine) {
    return std::acos(std::min(1.0f, std::max(-1.0f, cosine))) * kRadToDeg;
}
inline f32 lightConeCos(f32 degrees) {
    return std::cos(std::min(89.9f, std::max(0.0f, degrees)) * kDegToRad);
}

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
inline Transform transformFromLightRecord(const fmt::OcLight& r) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.z)};
    xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(r.roll), static_cast<f32>(r.pitch), static_cast<f32>(r.yaw)});
    return xf;
}

// One bare CLight entity per record (the instantiator does not know them). Returns the entities in
// record order, skipping any the world refused.
inline std::vector<scene::Entity> spawnLevelLights(scene::World& world, const std::vector<fmt::OcLight>& records) {
    std::vector<scene::Entity> out;
    for (const fmt::OcLight& r : records) {
        const scene::Entity e = world.create(r.name.empty() ? std::string("Light") : r.name, scene::kInvalidEntity,
                                             transformFromLightRecord(r));
        if (e == scene::kInvalidEntity) continue;
        if (auto* c = static_cast<scene::CLight*>(world.addComponent(e, scene::kComponentLight)))
            *c = lightFromRecord(r);
        out.push_back(e);
    }
    return out;
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
