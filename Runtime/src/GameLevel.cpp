#include "aver/game/GameLevel.hpp"

#include "aver/game/GameContent.hpp"
#include "aver/formats/OcStream.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"
#  include "aver/formats/OcMap.hpp"
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

namespace {

// A LEGACY .ocmap (ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM records) TRANSLATED INTO THE SHARED
// OcWorldData PLACEMENT PIPELINE -- mirrors SandboxApp::loadLegacyOcmapLevel's synth block
// (SandboxLevelLoad.cpp) minus the editor-only entityLegacyDeform_/entityLegacySurface_/
// entityLegacyMaterial_ save-round-trip maps: a shipped game never writes a level back out, so
// nothing here needs to remember which record kind or which numeric SURFACE/material name a
// placement came from, only where world::instantiate should put it.
bool loadLegacyOcmap(const std::string& path, fmt::OcMapData& m, fmt::OcWorldData& synth, std::string* err) {
    if (!fmt::loadOcmap(path, m, err)) return false;

    synth.name = m.name;
    synth.contentId = m.contentId;
    synth.build = m.build;
    synth.algo = m.algo;
    synth.hasSpawn = m.hasSpawn;
    synth.spawnX = m.spawnX; synth.spawnY = m.spawnY; synth.spawnZ = m.spawnZ; synth.spawnYaw = m.spawnYaw;
    synth.placements.reserve(m.placements.size());
    for (const fmt::OcPlacement& p : m.placements) {
        fmt::OcWorldPlacement op;
        op.asset = p.asset;
        op.objectId = p.objectId;   // already fnv1a64(asset) -- see OcMap.cpp's own PLACE/DEFORM branches
        op.x = p.x; op.y = p.y; op.z = p.z;
        op.yaw = p.yaw; op.pitch = p.pitch; op.roll = p.roll;
        // MATERIAL IS DELIBERATELY LEFT EMPTY on every synthesised placement, matching the editor: a
        // legacy PLACE names a numeric SURFACE-table index and a DEFORM a soft-body material like
        // "rubber", neither an .ocmat name -- feeding either to bindMaterial would resolve a
        // material that doesn't exist rather than leaving the mesh's own cooked material in place.
        //
        // p.scale is already forced to 1.0 for a DEFORM record (OcMap.cpp hardcodes it there; legacy
        // DEFORM has no scale concept of its own), so this one line is correct for both placement
        // kinds without a branch, same as the editor's own version of this loop.
        op.sx = op.sy = op.sz = (p.scale == 0.0 ? 1.0 : p.scale);
        // EVERY legacy placement is a collision source in this pipeline's hands, matching the
        // editor: a DEFORM's real behaviour is a server-simulated soft body, not this static box,
        // but nothing on this path spawns a deformable cage, so a static box is the honest stand-in.
        op.collide = true;
        synth.placements.push_back(std::move(op));
    }
    return true;
}

#if AVER_MODULE_PHYSICS
// <level>.oclanes: the lane graph the level's cars follow, beside the level file and under its stem. False
// for no file (the ordinary case for a level with no traffic) and for one that does not parse, which is
// told apart because the second is somebody's mistake and the first is not.
bool loadLaneSidecar(const std::string& levelPath, fmt::OcLanesData& out) {
    const std::filesystem::path p = GameLevel::laneSidecarPath(levelPath);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return false;
    std::string why;
    if (!fmt::loadOcLanes(p.string(), out, &why)) {
        AVER_WARN("[Level] {} did not load ({}) -- the level's vehicles park", p.string(), why);
        out = fmt::OcLanesData{};
        return false;
    }
    return true;
}
#endif

} // namespace

void GameLevel::load(const std::string& path, GameContent& content) {
    unload();

    fmt::OcWorldData w;
    std::string why;
    // DISPATCH ON WHAT THE FILE ACTUALLY USES, NOT ITS EXTENSION OR HEADER LINE -- mirrors
    // SandboxApp::loadLevel (SandboxLevelLoad.cpp): ElectricDreams' and FirstPerson's own .ocmap
    // files are pure OCWORLD content that happens to keep the older extension, while
    // OpenConstructor's demoworld.ocmap genuinely needs the legacy grammar. Without this, a
    // genuinely legacy file went through fmt::loadOcworld, which "succeeds" on the same `OCMAP 1`
    // header line while silently dropping every ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM record --
    // the level loaded with no placements and no warning at all.
    const bool legacy = fmt::levelFileIsLegacyOcmap(path);
    fmt::OcMapData legacyMap;   // the legacy file's own records, handed to afterInstantiate
    if (legacy) {
        if (!loadLegacyOcmap(path, legacyMap, w, &why)) { AVER_WARN("[Level] {}", why); return; }
    } else if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }
    // STAGE 1 OF 3 (see LoadHooks::progress): the file is parsed.
    if (hooks_.progress) hooks_.progress("Parsing level", 0.05f);

    // TERRAIN (AND WATER) FIRST, THEN THE THINGS THAT STAND ON IT, the editor's loadLevel order: a
    // `snap` placement asks the ground how high it is. See LoadHooks.
    if (!legacy && hooks_.beforePlacements) hooks_.beforePlacements(path, w);
    // STAGE 2 OF 3: environment and terrain, whatever beforePlacements just did (a legacy .ocmap has
    // none of this -- see LoadHooks::beforePlacements' own comment -- so the fraction still advances,
    // just with nothing having actually run).
    if (hooks_.progress) hooks_.progress("Loading environment", 0.15f);

    // The placement loop is aver::world::instantiate now, shared with the editor. What is left here
    // is the part that is genuinely the GAME's: which material cache to bind into, and what to keep.
    // LEVEL STREAMING (docs/LEVEL_STREAMING.md): bounds come from the generated .ocstream, by id.
    const bool streamed = !legacy && w.stream.enabled && device_;
    streamFoliage_.clear();
    streamDataStale_ = false;
    if (streamed) {
        fmt::OcStreamData sd;
        std::string sdWhy;
        const std::string projectDir = content.project().dir;
        if (w.stream.dataPath.empty() ||
            !fmt::loadOcStream((std::filesystem::path(projectDir) / w.stream.dataPath).string(), sd, &sdWhy)) {
            streamDataStale_ = true;
            AVER_WARN("[Level] streamed level has no streaming data ({}); every object stays loaded until it is "
                      "regenerated (World Settings > Regenerate Streaming Data)",
                      w.stream.dataPath.empty() ? std::string("no data= on its STREAM record") : sdWhy);
        } else {
            const usize matched = fmt::applyOcStreamBounds(w, sd);
            streamFoliage_ = sd.foliage;
            streamDataStale_ = sd.sourceHash != fmt::hashPlacements(w) || matched < w.placements.size();
            AVER_INFO("[Level] streaming data {}: bounds for {} of {} placement(s){}", w.stream.dataPath, matched,
                      w.placements.size(), streamDataStale_ ? " -- out of date, regenerate it" : "");
        }
    } else if (device_) {
        // A whole level holds its own meshes, so one in another level's on-demand folder is uploaded too.
        for (const fmt::OcWorldPlacement& p : w.placements) {
            if (p.className.empty() && std::find(heldMeshes_.begin(), heldMeshes_.end(), p.objectId) == heldMeshes_.end() &&
                content.acquireMesh(*device_, p.objectId))
                heldMeshes_.push_back(p.objectId);
        }
    }

    world::InstantiateOptions opt;
    if (!legacy) opt.groundHeightAt = hooks_.groundHeightAt;
    // A mesh's local bounds, by the objectId CMeshRenderer::mesh carries -- see LevelInstance.hpp's
    // own comment on why this is a host callback rather than something Aver.World looks up itself.
    // GameContent already keys this exact table by this exact id (the level-bounds walk below reads
    // it the same way), so this is a lookup, not new bookkeeping. Set unconditionally: boundsFor only
    // needs AVER_MODULE_SCENE, which this whole function already requires, not AVER_MODULE_PBR.
    opt.localBoundsFor = [&content](u64 meshId, Vec3& outLocalMin, Vec3& outLocalMax) {
        const std::pair<Vec3, Vec3>* b = content.boundsFor(meshId);
        if (!b) return false;
        outLocalMin = b->first;
        outLocalMax = b->second;
        return true;
    };
    // TRIANGLES, TRIED FIRST -- see InstantiateOptions::localTrianglesFor's own comment. GameContent
    // caches the collision mesh it builds (collisionMeshFor), and that cache is not cleared during
    // instantiate(), so the pointers handed back here stay valid for exactly as long as instantiate()
    // needs them.
    opt.localTrianglesFor = [&content](u64 meshId, const f32*& outPositions, u32& outVertexCount,
                                       const u32*& outIndices, u32& outIndexCount) {
        const GameContent::CollisionMesh* cm = content.collisionMeshFor(meshId);
        if (!cm || cm->indices.size() < 3) return false;
        outPositions = cm->positions.data();
        outVertexCount = static_cast<u32>(cm->positions.size() / 3);
        outIndices = cm->indices.data();
        outIndexCount = static_cast<u32>(cm->indices.size());
        return true;
    };
#if AVER_MODULE_PBR
    // Bind the authored material, if the project has one for this surface token. Done at load rather
    // than per draw because materialForSurface stats up to three paths on a miss and caches the
    // negative -- per frame that would be a filesystem hit per entity.
    opt.bindMaterial = [&content](i32 token, const std::string& surface) {
        const pbr::MaterialHandle h = content.materialForSurface(surface);
        if (h) content.bindSurfaceMaterial(token, h);
    };

    // A MESH'S OWN SLOT MATERIALS, BOUND HERE TOO -- not only a placement's OVERRIDE (opt.bindMaterial
    // just above, called only for a placement that actually names one). Without this, an entity whose
    // CMeshRenderer.material ends up 0 draws under content.meshDefaultMaterial(mesh)'s token (or, for a
    // multi-part mesh, one of content.partsFor(mesh)'s), and nothing had ever called
    // materialForSurface()/bindSurfaceMaterial() for THAT token unless some unrelated placement,
    // foliage instance or landscape happened to name the identical surface first -- GameRender's
    // resolveDrawLook then found content.authoredFor(token) empty and silently fell through to the
    // flat gray fallback for a mesh whose own .ocmesh names a real, authored material. A LATENT defect
    // in both hosts (the editor's own copy used to paper over it by loading every project material up
    // front, which is exactly the eager load level-scoped residency replaces), so it is fixed here
    // rather than only for the editor.
    //
    // Once per UNIQUE mesh this level places, not per placement: loadProjectMeshes() already built
    // meshSlot0Material_/meshParts_ for every project mesh before any level loads, so each iteration
    // here is a lookup plus a cache-checked materialForSurface() call, not a second parse of anything.
    // All LODs of a mesh share one materialSlots table (Trifactor's ladder never renames a slot), so
    // binding the base mesh id's slots covers every LOD that stands in for it too.
    std::unordered_set<u64> meshMaterialsBound;
    // A streamed level binds a mesh's slot materials when the mesh streams in (LevelStreaming).
    for (const fmt::OcWorldPlacement& p : streamed ? std::vector<fmt::OcWorldPlacement>{} : w.placements) {
        const u64 meshId = fnv1a64(std::string_view(p.asset));
        if (!meshMaterialsBound.insert(meshId).second) continue;   // an earlier placement already did this mesh
        const i32 slot0Token = content.meshDefaultMaterial(meshId);
        if (slot0Token) {
            const std::string& name = content.meshSlot0Name(meshId);
            if (!name.empty()) {
                const pbr::MaterialHandle h = content.materialForSurface(name);
                if (h) content.bindSurfaceMaterial(slot0Token, h);
            }
        }
        if (const std::vector<GameContent::MeshPart>* parts = content.partsFor(meshId)) {
            for (const GameContent::MeshPart& part : *parts) {
                if (!part.material) continue;
                const char* partName = aver_scene_material_name(part.material);
                if (!partName || !*partName) continue;
                const pbr::MaterialHandle h = content.materialForSurface(partName);
                if (h) content.bindSurfaceMaterial(part.material, h);
            }
        }
    }
#endif
    // STAGE 3 OF 3: placements, 15%..75% of load()'s own span -- world::InstantiateOptions::progress
    // is called at least every 256 placements and once at the end (its own contract), so a level of
    // any size still moves the bar smoothly rather than jumping straight from 15% to 75%.
    if (hooks_.progress) {
        opt.progress = [this](usize done, usize total) {
            const f32 frac = total > 0 ? static_cast<f32>(done) / static_cast<f32>(total) : 1.0f;
            hooks_.progress("Placing objects", 0.15f + frac * 0.60f);
        };
    }

#if AVER_MODULE_PHYSICS
    const u32 collisionCacheHitsBefore = content.collisionCacheHits();   // the counter is per process
#endif
    content_ = &content;
    world::LevelInstance inst;
    if (streamed) streaming_.begin(w, content, *device_, opt, streamHooks_);
    else inst = world::instantiate(w, opt);
    // load()'s OWN span ends here regardless of what opt.progress reported -- a level with zero
    // placements never calls it at all, and this is what still lands the fraction on 75% for one.
    if (hooks_.progress) hooks_.progress("Placing objects", 0.75f);
    levelEntities_ = inst.entities;
    animatedBodies_ = inst.animatedBodies;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif
    // THE CARS, RECORDED NOT BUILT: they become physics vehicles when play starts (beginVehicles), after
    // the editor's transform snapshot, and whether one CAN be built (bounds, scale) is decided then, from
    // the entity as it is by that time. The record is every placement that carries the token, so a save
    // writes it back whatever play makes of it -- with or without a physics module to drive it.
    vehiclePlacements_.clear();
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const std::string& preset = w.placements[inst.placementIndex[k]].vehiclePreset;
        if (!preset.empty()) vehiclePlacements_.push_back(VehiclePlacement{inst.entities[k], preset});
    }
#if AVER_MODULE_PHYSICS
    // The lane file is only read when there is a car to use it, so a level without traffic never looks
    // for one.
    hasLanes_ = false;
    lanes_ = fmt::OcLanesData{};
    if (!vehiclePlacements_.empty()) {
        hasLanes_ = loadLaneSidecar(path, lanes_);
        if (hasLanes_)
            AVER_INFO("[Level] {} vehicle placement(s), {} lane(s) from the level's .oclanes",
                      vehiclePlacements_.size(), lanes_.lanes.size());
        else
            AVER_INFO("[Level] {} vehicle placement(s) and no .oclanes beside the level: they will park",
                      vehiclePlacements_.size());
    }
#endif

#if AVER_MODULE_FRAMEWORK
    // COLLECTED HERE, SPAWNED LATER -- see classPlacements_'s own comment (GameLevel.hpp) for exactly
    // why load() itself must not call aver_fw_spawn: scripting (and with it, any graph-declared
    // class) is not ready yet at this point in EITHER composition root's boot sequence.
    classPlacements_.clear();
    for (const fmt::OcWorldPlacement& p : w.placements)
        if (!p.className.empty()) classPlacements_.push_back(p);
#endif

    // THE WHOLE ENVIRONMENT IN ONE LINE. This was twenty assignments across three `if`s, and the
    // hasSun/hasSky/hasFog flags rode INSIDE those conditions -- so the record's own flag and the
    // decision to copy it were the same statement, which is the arrangement that made a newly added
    // field easy to miss. Slicing the base copies every field including the flags, and a field
    // added to the format reaches the runtime with nothing else edited. See GameLevel::env().
    env_ = w;

    // THE LEVEL'S PLAYER START / SPAWN RECORD -- hasSpawn/spawnX/Y/Z/spawnYaw live on OcWorldData
    // itself, not on the OcWorldEnv base env_ just sliced off above, so they need their own capture.
    // Mirrors SandboxViewport.cpp's playerStartTransform()'s second branch (the raw SPAWN record,
    // read when no PlayerStart marker entity exists) -- a shipped game never has a marker to prefer,
    // so this alone is what GameApp::placePawnAtSpawn() needs. See SpawnPoint's own comment.
    spawn_.valid = w.hasSpawn;
    spawn_.position = Vec3{static_cast<f32>(w.spawnX), static_cast<f32>(w.spawnY), static_cast<f32>(w.spawnZ)};
    spawn_.yawDeg = static_cast<f32>(w.spawnYaw);

    // ---- the level's declared density fields ----
    //
    // Resolved here, once, rather than every time something samples one: turning an authored record
    // into a spec is cheap but it is not free, and a raymarch asking per pixel would pay for it a
    // million times.
    pcgVolumes_ = w.pcgVolumes;
    scatterSpecies_ = w.scatterSpecies;
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
        if (p.hasBounds) {   // baked world bounds (streamed levels: the mesh is not loaded yet)
            const Vec3 lo{p.boundsMin[0], p.boundsMin[1], p.boundsMin[2]}, hi{p.boundsMax[0], p.boundsMax[1], p.boundsMax[2]};
            if (!hasBounds_) { boundsLo_ = lo; boundsHi_ = hi; hasBounds_ = true; continue; }
            boundsLo_.x = std::fmin(boundsLo_.x, lo.x); boundsHi_.x = std::fmax(boundsHi_.x, hi.x);
            boundsLo_.y = std::fmin(boundsLo_.y, lo.y); boundsHi_.y = std::fmax(boundsHi_.y, hi.y);
            boundsLo_.z = std::fmin(boundsLo_.z, lo.z); boundsHi_.z = std::fmax(boundsHi_.z, hi.z);
            continue;
        }
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
    // The editor's two load functions log these same two lines, so the hosts' logs diff cleanly.
    if (legacy)
        AVER_INFO("[Level] '{}' loaded from {} ({} placement(s), legacy .ocmap)", w.name, path,
                  w.placements.size());
    else
        AVER_INFO("[Level] '{}' loaded from {} ({} placement(s))", w.name, path, w.placements.size());
#if AVER_MODULE_PHYSICS
    // One static body per COLLIDING placement, so this is checkable against the map file itself:
    // it must equal the count of PLACE lines without `nocollide` (and without `vehicle`: a car's body
    // is built by play, not by the load). Reported even when zero, because
    // zero bodies with colliding placements means physics was not ready at load time -- the exact
    // ordering bug initPhysics-before-openProject exists to prevent.
    //
    // TRIANGLE MESH VS BOX, AND HOW LONG THEY TOOK: the breakdown that actually tells an owner
    // whether NewSponza's courtyard is a solid box or its walls -- inst.meshBodyCount/boxBodyCount/
    // meshTriangleCount/bodyCreationSeconds all come straight off world::instantiate's own loop (see
    // LevelInstance.hpp's own comment on why they are collected there and not re-derived here).
    // SHARED SHAPES AND THE DISK CACHE: a mesh body is a scaled reference to one shape per unique mesh
    // (inst.uniqueMeshShapeCount of them), whose triangles came from the .occol cache when
    // collisionCacheHits counts them -- the two numbers that say whether a slow load was building BVHs.
    AVER_INFO("[Level] {} static physics body(ies) from {} placement(s): {} triangle mesh(es) "
              "({} triangles total, {} shared shape(s), {} from the collision cache), {} box(es), "
              "{:.2f} ms to create",
              levelBodies_.size(), w.placements.size(), inst.meshBodyCount, inst.meshTriangleCount,
              inst.uniqueMeshShapeCount, content.collisionCacheHits() - collisionCacheHitsBefore,
              inst.boxBodyCount,
              inst.bodyCreationSeconds * 1000.0);
    if (!animatedBodies_.empty())
        AVER_INFO("[Level] {} of those bodies are kinematic, following their placement's animation",
                  animatedBodies_.size());
#endif
    if (hooks_.afterInstantiate)
        hooks_.afterInstantiate(LoadedLevel{path, w, inst, legacy ? &legacyMap : nullptr});
}

void GameLevel::loadStartMap(const fmt::ProjectDesc& project, GameContent& content,
                              const std::string& overridePath) {
    // A LEVEL NAMED ON THE COMMAND LINE OUTRANKS THE PROJECT'S START MAP, as SandboxApp::loadStartMap's
    // openMapPath_ does: checked before project.valid(), so a level outside any project still opens,
    // and used as given rather than joined against the content dir.
    if (!overridePath.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(overridePath, ec)) { load(overridePath, content); return; }
        AVER_ERROR("[Level] '{}' does not exist", overridePath);
        // Fall back to the start map rather than nothing, exactly like the editor's own fallback.
    }
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
    // A `snap` class placement asks the same ground query load() hands world::instantiate, as the
    // editor's spawnClassPlacements does.
    for (const fmt::OcWorldPlacement& p : classPlacements_) {
        const int32_t c = aver_fw_class_find(p.className.c_str());
        if (c == 0) {
            AVER_WARN("[Level] placement names class '{}', which is not declared -- skipped", p.className);
            continue;
        }

        f64 pz = p.z;
        f64 gz = 0.0;
        if (p.snapToGround && hooks_.groundHeightAt && hooks_.groundHeightAt(p.x, p.y, gz)) pz = gz + p.z;
        const f32 pos3[3]  = {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(pz)};
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

const std::string* GameLevel::vehiclePresetOf(scene::Entity e) const {
    for (const VehiclePlacement& vp : vehiclePlacements_)
        if (vp.entity == e) return &vp.preset;
    // A streamed placement's token lives on its record (streamed cars park; they are not driven).
    if (const fmt::OcWorldPlacement* r = streaming_.recordOf(e); r && !r->vehiclePreset.empty()) return &r->vehiclePreset;
    return nullptr;
}

void GameLevel::setVehiclePreset(scene::Entity e, const std::string& preset) {
    const auto it = std::find_if(vehiclePlacements_.begin(), vehiclePlacements_.end(),
                                 [e](const VehiclePlacement& vp) { return vp.entity == e; });
    if (preset.empty()) {
        if (it != vehiclePlacements_.end()) vehiclePlacements_.erase(it);
    } else if (it != vehiclePlacements_.end()) {
        it->preset = preset;
    } else {
#if AVER_MODULE_PHYSICS
        // THE FIRST CAR A LEVEL GETS AFTER ITS LOAD (one placed over MCP into a level that had none) is the
        // first reason to look for its lane file, which load() reads only for a level that already had cars.
        if (vehiclePlacements_.empty() && !hasLanes_ && !levelPath_.empty())
            hasLanes_ = loadLaneSidecar(levelPath_, lanes_);
#endif
        vehiclePlacements_.push_back(VehiclePlacement{e, preset});
    }
}

#if AVER_MODULE_PHYSICS
std::string GameLevel::laneSidecarPath(const std::string& levelPath) {
    std::filesystem::path p(levelPath);
    // A RECOVERED AUTOSAVE is `<level>.autosave`, and the editor opens it by that path: replacing its
    // extension alone would look for `<level>.ocworld.oclanes` and find no lanes.
    if (p.extension() == ".autosave") p.replace_extension();
    p.replace_extension(".oclanes");
    return p.string();
}

usize GameLevel::beginVehicles(world::VehicleSystem& vehicles, const GameContent& content) const {
    // A system left over from a play session that never reached its end must not keep its cars beside
    // the new ones; end() on a system that was never begun does nothing.
    vehicles.end();
    if (vehiclePlacements_.empty() || !aver_phys_ready()) return 0;
    scene::World& w = scene::World::instance();

    // WHETHER A PLACEMENT CAN BE A CAR IS DECIDED HERE, from the entity as it is now, not from what the
    // file said at load: the editor can delete one, swap its mesh or scale it between the two. Each
    // reason it cannot is counted into ONE line, because a generator writes hundreds of these and a line
    // apiece would bury the one that matters.
    std::vector<world::VehicleSpawn> live;
    live.reserve(vehiclePlacements_.size());
    u32 noBounds = 0, scaled = 0, animated = 0;
    std::string firstNoBounds;
    for (const VehiclePlacement& vp : vehiclePlacements_) {
        if (!w.valid(vp.entity)) continue;   // deleted since the load
        // THE MESH THE ENTITY HAS NOW, which is also the table the placement's own collision box is
        // fitted from. A mesh the content set never loaded has no bounds to size a chassis from.
        const auto* mr = w.component<scene::CMeshRenderer>(vp.entity, scene::kComponentMeshRenderer);
        const std::pair<Vec3, Vec3>* b = mr ? content.boundsFor(mr->mesh) : nullptr;
        if (!b) {
            if (!noBounds) firstNoBounds = w.name(vp.entity);
            ++noBounds;
            continue;
        }
        // A car is built from its bounds at scale 1: the preset's wheel and suspension sizes are absolute,
        // so a scaled mesh would drive on wheels out of proportion with its body.
        const Vec3 scale = transformFromMatrix(w.worldMatrix(vp.entity)).scale;
        if (std::fabs(scale.x - 1.0f) > 1e-3f || std::fabs(scale.y - 1.0f) > 1e-3f ||
            std::fabs(scale.z - 1.0f) > 1e-3f) {
            ++scaled;
            continue;
        }
        // AN OBJECT CLIP AND A PHYSICS CAR ON ONE ENTITY FIGHT OVER ITS TRANSFORM (the clip's write lands
        // after the car's every frame), so the car would drive invisibly beside the animated mesh.
        if (w.hasComponent(vp.entity, scene::kComponentAnimator)) {
            ++animated;
            continue;
        }
        live.push_back(world::VehicleSpawn{vp.entity, vp.preset, b->first, b->second});
    }
    // None of these gets a collider either: a vehicle placement is never given a static one at load.
    if (noBounds)
        AVER_WARN("[Level] {} vehicle placement(s) name a mesh with no known bounds (first: '{}') -- they "
                  "stay where they are placed, WITHOUT collision", noBounds, firstNoBounds);
    if (scaled)
        AVER_WARN("[Level] {} vehicle placement(s) are not at scale 1 -- a physics vehicle is not scaled, so "
                  "they stay where they are placed, WITHOUT collision", scaled);
    if (animated)
        AVER_WARN("[Level] {} vehicle placement(s) also carry an object animation, which would overwrite the "
                  "physics pose every frame -- they stay animated and are not driven", animated);
    if (live.empty()) return 0;
    // FROM THE LEVEL'S NAME, so a level drives the same way every run (its cars choose the same turns)
    // and two levels do not drive in step.
    const u64 h = fnv1a64(std::string_view(levelName_));
    vehicles.begin(w, live, lanes(), static_cast<u32>(h ^ (h >> 32)));
    AVER_INFO("[Level] {} physics vehicle(s) built from {} placement(s), on {} lane(s)",
              vehicles.count(), live.size(), hasLanes_ ? lanes_.lanes.size() : usize{0});
    return vehicles.count();
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

std::vector<std::string> GameLevel::foliageTablePaths(const std::vector<std::string>& foliageFiles) const {
    std::vector<std::string> out(foliageFiles.size());
    if (!content_) return out;
    for (usize i = 0; i < foliageFiles.size(); ++i)
        for (const fmt::OcStreamFoliage& f : streamFoliage_)
            if (f.source == foliageFiles[i])
                out[i] = (std::filesystem::path(content_->project().dir) / f.cells).string();
    return out;
}

void GameLevel::tickStreaming(const std::vector<Vec3>& viewers) {
    if (streaming_.active()) streaming_.tick(scene::World::instance(), viewers);
    if (content_ && device_) content_->flushMeshReleases(*device_, ++streamFrame_);
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
    streaming_.end(world);
    if (content_ && device_) for (const u64 id : heldMeshes_) content_->releaseMesh(*device_, id);
    heldMeshes_.clear();
    streamFoliage_.clear();
    animatedBodies_.clear();
    for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
    levelEntities_.clear();
#if AVER_MODULE_PHYSICS
    for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
    levelBodies_.clear();
    lanes_ = fmt::OcLanesData{};
    hasLanes_ = false;
#endif
    vehiclePlacements_.clear();
    env_ = fmt::OcWorldEnv{};
    hasBounds_ = false;
    spawn_ = SpawnPoint{};
    pcgFields_.clear();
    pcgVolumes_.clear();
    scatterSpecies_.clear();
    levelPath_.clear();
    levelName_.clear();
    if (hooks_.afterUnload) hooks_.afterUnload();
}
#endif // AVER_MODULE_SCENE

} // namespace aver::game
