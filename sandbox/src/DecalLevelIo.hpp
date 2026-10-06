#pragma once
// .ocworld DECAL record <-> scene::CDecal + transform, for the editor's level load and save.
// Header-only so a test can round-trip it without the editor. docs/rendering/DECALS.md.
#include "DecalDetails.hpp"
#include "EditorEuler.hpp"
#include "LightLevelIo.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelDecals.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/DecalGather.hpp"
#include "aver/scene/World.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// The record -> component half is shared with the packaged game (aver/game/LevelDecals.hpp), so the two
// hosts cannot disagree about what a DECAL record means.
inline scene::CDecal decalFromRecord(const fmt::OcDecal& r) { return game::decalFromRecord(r); }

// The record's pose as a local transform (yaw/pitch/roll degrees, the placement convention).
inline Transform transformFromDecalRecord(const fmt::OcDecal& r) { return game::transformFromDecalRecord(r); }

// One entity's decal as a record. `world` is its WORLD transform (records carry no parent).
inline fmt::OcDecal recordFromDecal(const scene::CDecal& c, const Transform& world, const std::string& name,
                                    const LightAssetPaths& assets) {
    fmt::OcDecal r;
    r.name = name;
    r.x = world.position.x; r.y = world.position.y; r.z = world.position.z;
    const Vec3 e = eulerDegFromQuat(world.rotation);
    r.roll = e.x; r.pitch = e.y; r.yaw = e.z;
    r.sx = world.scale.x; r.sy = world.scale.y; r.sz = world.scale.z;
    for (int i = 0; i < 3; ++i) r.sizeCm[i] = scene::decalSizeCm(c, i);
    f32 tint[3];
    scene::decalTint(c, tint);
    for (int i = 0; i < 3; ++i) r.tint[i] = tint[i];
    r.opacity = 1.0 - std::min(std::max<f64>(c.transparency, 0.0), 1.0);
    r.normalStrength = scene::decalNormalStrength(c);
    r.roughness = c.roughness;
    r.metallic = c.metallic;
    r.edgeFade = scene::decalEdgeFade(c);
    r.angleStartDeg = scene::decalAngleStartDeg(c);
    r.angleEndDeg = scene::decalAngleEndDeg(c);
    r.fadeDistanceCm = c.fadeDistanceCm;
    r.order = c.sortOrder;
    r.base = assets.find(c.baseTexture);
    r.normal = assets.find(c.normalTexture);
    r.orm = assets.find(c.ormTexture);
    for (int i = 0; i < 2; ++i) { r.uvScale[i] = scene::decalUvScale(c, i); r.uvOffset[i] = c.uvOffset[i]; }
    r.noColour = (c.flags & scene::kDecalNoColour) != 0;
    r.noNormal = (c.flags & scene::kDecalNoNormal) != 0;
    r.noRoughness = (c.flags & scene::kDecalNoRoughness) != 0;
    r.disabled = (c.flags & scene::kDecalDisabled) != 0;
    return r;
}

// "Add > Decal": a bare decal entity at `at` facing `yawDeg`, with sensible new-decal values. The
// caller labels it, selects it and pushes the undo entry (spawnLightAtCamera is the pattern).
inline scene::Entity createEditorDecal(scene::World& world, const Vec3& at, f32 yawDeg) {
    Transform xf;
    xf.position = at;
    xf.rotation = quatFromEulerDeg(Vec3{0.0f, 0.0f, yawDeg});
    const scene::Entity e = world.create("Decal", scene::kInvalidEntity, xf);
    if (e == scene::kInvalidEntity) return e;
    if (auto* c = static_cast<scene::CDecal*>(world.addComponent(e, scene::kComponentDecal))) *c = makeNewDecal();
    return e;
}

// Level load: one bare CDecal entity per record (the instantiator does not know them). Returns the
// entities, so the caller can label them.
inline std::vector<scene::Entity> spawnDecalsFromRecords(scene::World& world, const std::vector<fmt::OcDecal>& records) {
    return game::spawnLevelDecals(world, records);
}

// Level save: every authored CDecal entity, as records. Pooled (gameplay) decals are skipped.
inline std::vector<fmt::OcDecal> recordsFromDecalEntities(scene::World& world, const LightAssetPaths& assets) {
    std::vector<fmt::OcDecal> out;
    scene::ComponentPool* pool = world.pool(scene::kComponentDecal);
    if (!pool) return out;
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        if (!world.valid(e) || world.destroyPending(e)) continue;
        const auto* c = static_cast<const scene::CDecal*>(pool->dataAt(i));
        if (c->flags & scene::kDecalPooled) continue;
        out.push_back(recordFromDecal(*c, transformFromMatrix(world.worldMatrix(e)), world.name(e), assets));
    }
    return out;
}

} // namespace aver::editor
