#include "aver/game/GameLevel.hpp"

#include "aver/game/GameContent.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <system_error>

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/world/LevelInstance.hpp"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"
#endif

namespace aver::game {

#if AVER_MODULE_SCENE

// quatFromEulerDeg was here, as a file-static copied out of SandboxApp.cpp. It is now
// aver::world::quatFromEulerDeg in modules/world/include/aver/world/LevelTransform.hpp, reached
// through LevelInstance.hpp, and the editor re-exports the same definition -- so the rotation
// contract the comment here used to insist on is now enforced by there being one of it.

void GameLevel::load(const std::string& path, GameContent& content) {
    unload();

    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }

    // The placement loop is aver::world::instantiate now, shared with the editor. What is left here
    // is the part that is genuinely the GAME's: which material cache to bind into, and what to keep.
    world::InstantiateOptions opt;
#if AVER_MODULE_PBR
    // Bind the authored material, if the project has one for this surface token. Done at load rather
    // than per draw because materialForSurface stats up to three paths on a miss and caches the
    // negative -- per frame that would be a filesystem hit per entity.
    opt.bindMaterial = [&content](i32 token, const std::string& surface) {
        const pbr::MaterialHandle h = content.materialForSurface(surface);
        if (h) content.bindSurfaceMaterial(token, h);
    };
#else
    (void)content;
#endif

    const world::LevelInstance inst = world::instantiate(w, opt);
    levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif

#if AVER_MODULE_FRAMEWORK
    // COLLECTED HERE, SPAWNED LATER -- see classPlacements_'s own comment (GameLevel.hpp) for exactly
    // why load() itself must not call aver_fw_spawn: scripting (and with it, any graph-declared
    // class) is not ready yet at this point in EITHER composition root's boot sequence.
    classPlacements_.clear();
    for (const fmt::OcWorldPlacement& p : w.placements)
        if (!p.className.empty()) classPlacements_.push_back(p);
#endif

    if (w.hasFog) {
        levelFog_    = static_cast<f32>(w.fogDensity);
        fogColor_[0] = static_cast<f32>(w.fogColor[0]);
        fogColor_[1] = static_cast<f32>(w.fogColor[1]);
        fogColor_[2] = static_cast<f32>(w.fogColor[2]);
        hasLevelFog_ = true;
    }

    // Load sun settings from the level
    if (w.hasSun) {
        hasSun_ = true;
        sunDir_[0] = w.sunDir[0];
        sunDir_[1] = w.sunDir[1];
        sunDir_[2] = w.sunDir[2];
        sunColor_[0] = w.sunColor[0];
        sunColor_[1] = w.sunColor[1];
        sunColor_[2] = w.sunColor[2];
        sunLux_ = w.sunLux;
    }

    // Load sky settings from the level
    if (w.hasSky) {
        hasSky_ = true;
        skyPhysical_ = w.skyPhysical;
        skyMieScatter_ = w.skyMieScatter;
        skyMultiScatter_ = w.skyMultiScatter;
        skyViewSteps_ = w.skyViewSteps;
        skyAerialSteps_ = w.skyAerialSteps;
    }

    // ---- the level's declared density fields ----
    //
    // Resolved here, once, rather than every time something samples one: turning an authored record
    // into a spec is cheap but it is not free, and a raymarch asking per pixel would pay for it a
    // million times.
    pcgFields_.clear();
    for (const fmt::OcPcgVolume& v : w.pcgVolumes) {
        PcgField f;
        f.name = v.name;
        f.infinite = v.infinite;

        // fBm layers derived from the octave count, matching Aver.Pcg's fbmLayers so a field
        // authored in a level and one built in F# from the same numbers agree.
        const u32 layers = static_cast<u32>(v.octaves) < pcg::kMaxLayers
                         ? static_cast<u32>(v.octaves) : pcg::kMaxLayers;
        pcg::NoiseLayer ls[pcg::kMaxLayers]{};
        for (u32 i = 0; i < layers; ++i) {
            ls[i].frequency  = std::pow(2.0f, static_cast<f32>(i));
            ls[i].amplitude  = 1.0f / std::pow(2.0f, static_cast<f32>(i));
            ls[i].octaves    = 4;
            ls[i].lacunarity = 2.0f;
            ls[i].gain       = 0.5f;
            ls[i].seedOffset = static_cast<i32>(i + 1) * 7919;
        }

        if (v.infinite) {
            f.infiniteSpec.seed = v.seed;
            f.infiniteSpec.layerCount = layers;
            for (u32 i = 0; i < layers; ++i) f.infiniteSpec.layers[i] = ls[i];
            f.infiniteSpec.cellSizeCm    = static_cast<f32>(v.cellSizeCm);
            f.infiniteSpec.coverageFloor = static_cast<f32>(v.coverageFloor);
            f.infiniteSpec.coverageBias  = static_cast<f32>(v.coverageBias);
        } else {
            f.boundsMin = Vec3{static_cast<f32>(v.boundsMin[0]), static_cast<f32>(v.boundsMin[1]),
                               static_cast<f32>(v.boundsMin[2])};
            f.boundsMax = Vec3{static_cast<f32>(v.boundsMax[0]), static_cast<f32>(v.boundsMax[1]),
                               static_cast<f32>(v.boundsMax[2])};
            // Resolution from the bounds and the cell size, so `cell` means the same thing in both
            // cases: one lattice cell spans that much world. Clamped to something a GPU will
            // actually allocate -- a 100 km volume at 16 m cells is 6250^3 voxels.
            const f32 c = v.cellSizeCm > 0.0 ? static_cast<f32>(v.cellSizeCm) : 1600.0f;
            auto res = [c](f32 lo, f32 hi) -> u32 {
                const f32 n = (hi - lo) / c;
                const i32 i = static_cast<i32>(n < 1.0f ? 1.0f : n);
                return static_cast<u32>(i > 256 ? 256 : i);
            };
            f.boundedSpec.resX = res(f.boundsMin.x, f.boundsMax.x);
            f.boundedSpec.resY = res(f.boundsMin.y, f.boundsMax.y);
            f.boundedSpec.resZ = res(f.boundsMin.z, f.boundsMax.z);
            f.boundedSpec.seed = v.seed;
            f.boundedSpec.layerCount = layers;
            for (u32 i = 0; i < layers; ++i) f.boundedSpec.layers[i] = ls[i];
            f.boundedSpec.coverageFloor = static_cast<f32>(v.coverageFloor);
            f.boundedSpec.coverageBias  = static_cast<f32>(v.coverageBias);
        }
        pcgFields_.push_back(std::move(f));
    }
    for (const PcgField& f : pcgFields_) {
        if (f.infinite)
            AVER_INFO("[PCG] level field '{}': INFINITE, seed {}, {} cm cells, {} layer(s)",
                      f.name, f.infiniteSpec.seed, f.infiniteSpec.cellSizeCm, f.infiniteSpec.layerCount);
        else
            AVER_INFO("[PCG] level field '{}': bounded {}x{}x{}, seed {}, {} layer(s)",
                      f.name, f.boundedSpec.resX, f.boundedSpec.resY, f.boundedSpec.resZ,
                      f.boundedSpec.seed, f.boundedSpec.layerCount);
    }

    // MESH EXTENT TIMES SCALE, not scale on its own. This loop first shipped copying the editor's
    // frameCameraOn, which uses fabs(p.sx/sy/sz) DIRECTLY as a half-extent in centimetres -- and
    // that is wrong for both record kinds, because sx/sy/sz is a dimensionless SCALE MULTIPLIER in
    // both. LevelInstance.cpp assigns it straight to Transform::scale; nothing anywhere converts it
    // to a size. An ordinary PLACE leaves it at 1.0, so every placement contributed a 1cm cube and
    // the "level bounds" were really just the point cloud of placement POSITIONS. A level holding
    // one large mesh at the origin got a 1cm GI volume.
    //
    // It read as plausible because of one coincidence: PLACEG is conventionally used with a unit-
    // cube mesh (see any level's ground slab, `PLACEG Meshes/cube.ocmesh ... 3000 3000 10`), and for
    // a mesh that spans +/-1 a scale and a half-extent are numerically the same thing. That is a
    // property of that one mesh, not of the format.
    //
    // THE MESH BOX IS SCALED, ROTATED AND RE-BOUNDED, corner by corner, rather than approximated by
    // a rotation-proof sphere. The sphere was the first version here and it is worse on exactly the
    // geometry every level has: a ground slab is enormous in X and Y and almost flat in Z, and
    // giving it one radius inflates its 20cm thickness to its 60m width. On this project's own
    // level that alone made the fitted volume 5196cm where the real content is 4243cm -- a 22%
    // over-estimate of the thing that decides voxel density, bought for nothing, since the eight
    // corners cost eight rotations per placement, once, at load.
    const Vec3 kCorner[8] = {{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},
                             {-1,-1, 1},{1,-1, 1},{-1,1, 1},{1,1, 1}};
    for (const fmt::OcWorldPlacement& p : w.placements) {
        const Vec3 c{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
        // An asset this content set never loaded (a typo, or a mesh the cook skipped) has no bounds
        // to ask for. Contributing just its position is right: it occupies no space we can prove,
        // and inventing a size for it would let one bad line inflate the whole volume.
        const std::pair<Vec3, Vec3>* mb = content.boundsFor(fnv1a64(std::string_view(p.asset)));
        const Vec3 mlo = mb ? mb->first  : Vec3{0, 0, 0};
        const Vec3 mhi = mb ? mb->second : Vec3{0, 0, 0};
        // The mesh box's own centre and half-size. NOT assumed to straddle the origin: a mesh
        // authored with its feet at z=0 -- which every character in every pack is -- has a box
        // whose centre sits half its height up, and folding that offset in is what puts the volume
        // around the model instead of around the point it was dropped at.
        const Vec3 mc{(mlo.x + mhi.x) * 0.5f, (mlo.y + mhi.y) * 0.5f, (mlo.z + mhi.z) * 0.5f};
        const Vec3 mh{(mhi.x - mlo.x) * 0.5f, (mhi.y - mlo.y) * 0.5f, (mhi.z - mlo.z) * 0.5f};
        const Vec3 sc{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
        const Quat rot = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                      static_cast<f32>(p.pitch),
                                                      static_cast<f32>(p.yaw)});
        for (const Vec3& k : kCorner) {
            // Scale then rotate then translate -- the same order LevelInstance builds the real
            // transform in, so this box bounds where the instance actually ends up.
            const Vec3 local{(mc.x + k.x * mh.x) * sc.x,
                             (mc.y + k.y * mh.y) * sc.y,
                             (mc.z + k.z * mh.z) * sc.z};
            const Vec3 wpt = c + rot.rotate(local);
            if (!hasBounds_) { boundsLo_ = boundsHi_ = wpt; hasBounds_ = true; continue; }
            boundsLo_.x = std::fmin(boundsLo_.x, wpt.x); boundsHi_.x = std::fmax(boundsHi_.x, wpt.x);
            boundsLo_.y = std::fmin(boundsLo_.y, wpt.y); boundsHi_.y = std::fmax(boundsHi_.y, wpt.y);
            boundsLo_.z = std::fmin(boundsLo_.z, wpt.z); boundsHi_.z = std::fmax(boundsHi_.z, wpt.z);
        }
    }

    levelPath_ = path;
    levelName_ = w.name;
    AVER_INFO("[Level] '{}' loaded from {} ({} placement(s))", w.name, path, w.placements.size());
#if AVER_MODULE_PHYSICS
    // One static body per COLLIDING placement, so this is checkable against the map file itself:
    // it must equal the count of PLACE lines without `nocollide`. Reported even when zero, because
    // zero bodies with colliding placements means physics was not ready at load time -- the exact
    // ordering bug initPhysics-before-openProject exists to prevent.
    AVER_INFO("[Level] {} static physics body(ies) from {} placement(s)",
              levelBodies_.size(), w.placements.size());
#endif
}

void GameLevel::loadStartMap(const fmt::ProjectDesc& project, GameContent& content) {
    if (!project.valid() || project.startMap.empty()) return;
    const std::string path = project.contentDir() + "\\" + project.startMap;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        AVER_INFO("[Level] start map '{}' does not exist yet - the world starts empty", project.startMap);
        levelName_ = std::filesystem::path(project.startMap).stem().string();
        levelPath_ = path;
        return;
    }
    load(path, content);
}

#if AVER_MODULE_FRAMEWORK
void GameLevel::spawnClassPlacements() {
    // SPAWNED FOR REAL (aver_fw_spawn, not aver_fw_spawn_preview): a shipped game has no separate
    // "loaded but not yet playing" state for its own level content (see
    // GameApp::beginPlayIfGameModeDeclared's own comment -- "boot the game" and "begin playing" are
    // the same moment for it), so there is no later moment to promote a preview into. BeginPlay/
    // OnStart fire immediately, at this call.
    //
    // NO GROUND SNAP HERE, UNLIKE THE EDITOR'S OWN LEVEL LOAD: `snap` support needs
    // InstantiateOptions::groundHeightAt, a landscape query this file's own load() never wires up for
    // the game runtime today (grep this file -- opt.groundHeightAt is never assigned here, only in
    // SandboxApp.cpp) -- a PRE-EXISTING gap for every ordinary mesh placement too, not something this
    // slice introduces or narrows.
    for (const fmt::OcWorldPlacement& p : classPlacements_) {
        const int32_t c = aver_fw_class_find(p.className.c_str());
        if (c == 0) {
            AVER_WARN("[Level] placement names class '{}', which is not declared -- skipped", p.className);
            continue;
        }

        const f32 pos3[3]  = {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
        const Quat rot     = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                           static_cast<f32>(p.pitch),
                                                           static_cast<f32>(p.yaw)});
        const f32 quat4[4]  = {rot.x, rot.y, rot.z, rot.w};
        const f32 scale3[3] = {static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};

        const int32_t e = aver_fw_spawn(c, p.className.c_str(), pos3, quat4, scale3);
        if (e == 0) {
            AVER_WARN("[Level] class '{}' failed to spawn at ({:.0f}, {:.0f}, {:.0f})",
                      p.className, pos3[0], pos3[1], pos3[2]);
            continue;
        }
        levelClassInstances_.push_back(e);
    }
    if (!levelClassInstances_.empty())
        AVER_INFO("[Level] {} class instance(s) placed -- an entity exists for each; whether its graph "
                  "COMPILED is reported per instance above, because aver_fw_spawn returns a live entity "
                  "even when the managed bind behind it failed, so this count is placement, not success",
                  levelClassInstances_.size());
}
#endif

const GameLevel::PcgField* GameLevel::pcgField(const std::string& name) const {
    for (const PcgField& f : pcgFields_) if (f.name == name) return &f;
    return nullptr;
}

bool GameLevel::placementBounds(Vec3& lo, Vec3& hi, f32& radius) const {
    if (!hasBounds_) return false;
    lo = boundsLo_;
    hi = boundsHi_;
    // Half the diagonal, floored at 1cm. The editor's own floor, and it matters: a level of one
    // zero-scaled placement would otherwise ask Voxi for a volume with no extent at all.
    const f32 dx = boundsHi_.x - boundsLo_.x, dy = boundsHi_.y - boundsLo_.y, dz = boundsHi_.z - boundsLo_.z;
    radius = std::fmax(1.0f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    return true;
}

void GameLevel::unload() {
#if AVER_MODULE_FRAMEWORK
    // BEFORE the raw-entity loop below, and through aver_fw_destroy rather than world.destroy(): a
    // class instance needs its managed-dispatch unbind hook to actually fire (HostBridge.cs's
    // DispUnbind), which is what releases its GraphHost/VAR storage and drops it out of
    // GraphTickBoundInstances' walk -- world.destroy() alone would leak both.
    for (const int32_t e : levelClassInstances_) aver_fw_destroy(e);
    levelClassInstances_.clear();
    classPlacements_.clear();   // in case unload() runs before spawnClassPlacements() ever did
#endif
    scene::World& world = scene::World::instance();
    for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
    levelEntities_.clear();
#if AVER_MODULE_PHYSICS
    for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
    levelBodies_.clear();
#endif
    hasLevelFog_ = false;
    hasSun_ = false;
    hasSky_ = false;
    hasBounds_ = false;
    pcgFields_.clear();
    levelPath_.clear();
    levelName_.clear();
}
#endif // AVER_MODULE_SCENE

} // namespace aver::game
